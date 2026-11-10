/* SPDX-License-Identifier: MPL-2.0 */
#include "native_runtime.h"

#include "kirton/eval.h"
#include "kirton/native.h"
#include "native_stdlib_whitelist.h"
#include "kirton/ast.h"
#include "eval_internal.h"
#include "kirton/common.h"
#include "kirton/diag.h"
#include "kirton/utf8.h"

#include <stdio.h>
#include <limits.h>
#include <string.h>

struct KirtonNativeValue {
    KirPinnedValue* pin;
    struct KirtonNativeValue* next;
    /* A root-frame pin created for a global store, not an expression temporary. */
    int global_anchor;
};

struct KirtonNativeFrame {
    struct KirtonNativeRuntime* runtime;
    struct KirtonNativeFrame* parent;
    KirtonNativeValue* values;
};

typedef struct {
    AstFunc ast_identity;
    KirtonNativeCompiledFn compiled;
    size_t argc;
} KirtonNativeCompiledDescriptor;

struct KirtonNativeRuntime {
    VM* vm;
    KirtonNativeFrame* frame;
    KirtonNativeFrame* root;
    int status;
    AstUserType** types;
    size_t type_count;
    KirtonNativeCompiledDescriptor** compiled_functions;
    size_t compiled_count;
};

static void runtime_fail(KirtonNativeRuntime* runtime){
    if (runtime && !runtime->status) runtime->status=70;
}
static void native_user_error(KirtonNativeRuntime* runtime,const char* message);

static KirtonNativeValue* value_in_frame(KirtonNativeRuntime* runtime,
                                         KirtonNativeFrame* frame,Value value){
    if (!runtime || !frame || frame->runtime!=runtime || !runtime->vm){
        runtime_fail(runtime);
        return NULL;
    }
    KirPinnedValue* pin=vm_value_pin(runtime->vm,value);
    if (!pin){ runtime_fail(runtime); return NULL; }
    KirtonNativeValue* handle=(KirtonNativeValue*)kir_malloc(sizeof(*handle));
    handle->pin=pin;
    handle->global_anchor=0;
    handle->next=frame->values;
    frame->values=handle;
    return handle;
}

static Value native_value(KirtonNativeRuntime* runtime,
                          const KirtonNativeValue* value){
    if (!runtime || !value || !value->pin){
        runtime_fail(runtime);
        return v_nil();
    }
    return vm_value_pinned(value->pin);
}

/* These modules contain pure Value-based operations and use the same
   KirValueHeap as the compiled code. Registration is process-wide; repeated
   calls update static entries and do not change the VM's ownership. */
void str_init(void);
void list_init(void);
void dict_init(void);
void math_init(void);
void memory_init(void);

KirtonNativeRuntime* kirton_native_runtime_new(void){
    KirtonNativeRuntime* runtime=(KirtonNativeRuntime*)kir_malloc(sizeof(*runtime));
    runtime->vm=vm_new();
    runtime->frame=NULL;
    runtime->root=NULL;
    runtime->status=0;
    runtime->types=NULL;
    runtime->type_count=0;
    runtime->compiled_functions=NULL;
    runtime->compiled_count=0;
    if (!runtime->vm){ kir_free(runtime); return NULL; }
    runtime->frame=kirton_native_frame_enter(runtime);
    if (!runtime->frame){
        vm_free(runtime->vm);
        kir_free(runtime);
        return NULL;
    }
    runtime->root=runtime->frame;
    str_init();
    list_init();
    dict_init();
    math_init();
    memory_init();
    return runtime;
}

KirtonNativeValue* kirton_native_memory_call(KirtonNativeRuntime* runtime,
    const char* method,size_t method_length,
    const KirtonNativeValue* const* args,size_t argc){
    if(!runtime || runtime->status || !runtime->vm || !runtime->frame ||
       !method || method_length>200 || argc>INT_MAX || (argc && !args)){
        runtime_fail(runtime);return NULL;
    }
    char fullname[256];
    const char* prefix="память.";
    size_t prefix_length=strlen(prefix);
    if(method_length>sizeof(fullname)-prefix_length-1){runtime_fail(runtime);return NULL;}
    memcpy(fullname,prefix,prefix_length);
    memcpy(fullname+prefix_length,method,method_length);
    fullname[prefix_length+method_length]='\0';
    Value* values=argc?(Value*)kir_malloc(argc*sizeof(Value)):NULL;
    for(size_t i=0;i<argc;++i)values[i]=native_value(runtime,args[i]);
    if(runtime->status){kir_free(values);return NULL;}
    Value result=v_nil();
    unsigned errors_before=kir_diag_error_count();
    int found=vm_value_native_call(runtime->vm,fullname,prefix_length+method_length,
                                   (int)argc,values,&result);
    kir_free(values);
    if(!found){
        native_user_error(runtime,"Неизвестная операция памяти");return NULL;
    }
    if(kir_diag_error_count()!=errors_before){
        if(!runtime->status)runtime->status=1;
        return NULL;
    }
    return value_in_frame(runtime,runtime->frame,result);
}

void kirton_native_ffi_enable(KirtonNativeRuntime* runtime){
    if(!runtime || !runtime->vm || !vm_ffi_set_enabled(runtime->vm,1))
        runtime_fail(runtime);
}

/* The generated program passes rooted Value handles, never raw host pointers.
   VM ownership, ABI validation and resource lifetimes remain in ffi.c. */
KirtonNativeValue* kirton_native_ffi_invoke(KirtonNativeRuntime* runtime,
    int method,const KirtonNativeValue* const* args,size_t argc){
    if(!runtime || !runtime->vm || runtime->status || !args ||
       argc>INT_MAX || (method==1 ? argc!=1 : (method!=2 || argc<2))){
        runtime_fail(runtime);return NULL;
    }
    Value* values=(Value*)kir_malloc(argc*sizeof(Value));
    for(size_t i=0;i<argc;++i)values[i]=native_value(runtime,args[i]);
    if(runtime->status){kir_free(values);return NULL;}
    size_t names_count=method==1?1:2;
    char* names[2]={NULL,NULL};
    for(size_t i=0;i<names_count;++i){
        Value v=values[i];
        if(v.kind!=VAL_STR || !v.as.str.s ||
           memchr(v.as.str.s,'\0',v.as.str.n) || v.as.str.n==SIZE_MAX){
            native_user_error(runtime,"Неверные аргументы внешнего вызова: ожидались имена-строки без NUL");
            goto failed;
        }
        names[i]=(char*)kir_malloc(v.as.str.n+1);
        memcpy(names[i],v.as.str.s,v.as.str.n);
        names[i][v.as.str.n]='\0';
    }
    Value result=v_nil();
    int ok=method==1 ? vm_ffi_load(runtime->vm,names[0]) :
         vm_ffi_call(runtime->vm,names[0],names[1],(int)(argc-2),values+2,&result);
    if(!ok){
        native_user_error(runtime,vm_ffi_last_error(runtime->vm));
        goto failed;
    }
    result=method==1?v_bool(true):result;
    KirtonNativeValue* handle=value_in_frame(runtime,runtime->frame,result);
    for(size_t i=0;i<names_count;++i)kir_free(names[i]);
    kir_free(values);
    return handle;
failed:
    for(size_t i=0;i<names_count;++i)kir_free(names[i]);
    kir_free(values);
    return NULL;
}

void kirton_native_function_register(KirtonNativeRuntime* runtime,size_t id,
    size_t argc,KirtonNativeCompiledFn compiled){
    if (!runtime || runtime->status || !compiled || id>SIZE_MAX/sizeof(void*)-1){
        runtime_fail(runtime); return;
    }
    if (id>=runtime->compiled_count){
        size_t count=id+1;
        KirtonNativeCompiledDescriptor** resized=(KirtonNativeCompiledDescriptor**)
            kir_realloc(runtime->compiled_functions,count*sizeof(*resized));
        if (!resized){runtime_fail(runtime);return;}
        for (size_t i=runtime->compiled_count;i<count;++i) resized[i]=NULL;
        runtime->compiled_functions=resized;
        runtime->compiled_count=count;
    }
    if (runtime->compiled_functions[id]){runtime_fail(runtime);return;}
    KirtonNativeCompiledDescriptor* descriptor=(KirtonNativeCompiledDescriptor*)
        kir_malloc(sizeof(*descriptor));
    memset(descriptor,0,sizeof(*descriptor));
    descriptor->compiled=compiled;
    descriptor->argc=argc;
    runtime->compiled_functions[id]=descriptor;
}

KirtonNativeValue* kirton_native_cell_new(KirtonNativeRuntime* runtime){
    if(!runtime || runtime->status || !runtime->frame){
        runtime_fail(runtime);return NULL;
    }
    Value cell=vm_value_list_new(runtime->vm);
    if(cell.kind!=VAL_LIST){runtime_fail(runtime);return NULL;}
    KirtonNativeValue* pinned=value_in_frame(runtime,runtime->frame,cell);
    if(!pinned)return NULL;
    v_list_push(&cell,v_nil());
    v_list_push(&cell,v_bool(false));
    return pinned;
}

static int native_cell_valid(Value cell){
    return cell.kind==VAL_LIST && cell.as.list && cell.as.list->len==2 &&
           cell.as.list->data[1].kind==VAL_BOOL;
}

KirtonNativeValue* kirton_native_cell_get(KirtonNativeRuntime* runtime,
    const KirtonNativeValue* handle){
    Value cell=native_value(runtime,handle);
    if(!native_cell_valid(cell) || !cell.as.list->data[1].as.boolean){
        fprintf(stderr,"Kirton Native: неинициализированная захваченная переменная\n");
        runtime_fail(runtime);return NULL;
    }
    return value_in_frame(runtime,runtime->frame,cell.as.list->data[0]);
}

void kirton_native_cell_set(KirtonNativeRuntime* runtime,
    const KirtonNativeValue* handle,const KirtonNativeValue* value){
    Value cell=native_value(runtime,handle);
    if(!native_cell_valid(cell)){runtime_fail(runtime);return;}
    cell.as.list->data[0]=native_value(runtime,value);
    cell.as.list->data[1]=v_bool(true);
}

KirtonNativeValue* kirton_native_function_new_captured(KirtonNativeRuntime* runtime,
    size_t id,const KirtonNativeValue* const* cells,size_t count){
    if(!runtime || runtime->status || !runtime->frame ||
        id>=runtime->compiled_count || !runtime->compiled_functions[id] ||
        (count && !cells)){
        runtime_fail(runtime);return NULL;
    }
    Value captures=vm_value_list_new(runtime->vm);
    if(captures.kind!=VAL_LIST){runtime_fail(runtime);return NULL;}
    /* Pin the temporary list before evaluating any other Value operations. */
    if(!value_in_frame(runtime,runtime->frame,captures))return NULL;
    for(size_t i=0;i<count;++i){
        Value cell=native_value(runtime,cells[i]);
        if(!native_cell_valid(cell)){runtime_fail(runtime);return NULL;}
        v_list_push(&captures,cell);
    }
    KirtonNativeCompiledDescriptor* desc=runtime->compiled_functions[id];
    Value function=vm_value_compiled_function_new(runtime->vm,
        &desc->ast_identity,desc,captures);
    if(function.kind!=VAL_FUNCTION){runtime_fail(runtime);return NULL;}
    return value_in_frame(runtime,runtime->frame,function);
}

KirtonNativeValue* kirton_native_function_new(KirtonNativeRuntime* runtime,size_t id){
    return kirton_native_function_new_captured(runtime,id,NULL,0);
}

KirtonNativeValue* kirton_native_capture_cell(KirtonNativeRuntime* runtime,
    const KirtonNativeValue* function,size_t index){
    Value fn=native_value(runtime,function);
    if(!vm_value_is_compiled_function(runtime->vm,fn)){
        runtime_fail(runtime);return NULL;
    }
    Value captures=vm_value_compiled_captures(fn);
    if(captures.kind!=VAL_LIST || index>=captures.as.list->len){
        runtime_fail(runtime);return NULL;
    }
    Value cell=captures.as.list->data[index];
    if(!native_cell_valid(cell)){runtime_fail(runtime);return NULL;}
    return value_in_frame(runtime,runtime->frame,cell);
}

KirtonNativeValue* kirton_native_function_call(KirtonNativeRuntime* runtime,
    const KirtonNativeValue* function,
    const KirtonNativeValue* const* arguments,size_t argc){
    if(!runtime || runtime->status || !runtime->frame || (argc && !arguments)){
        runtime_fail(runtime);return NULL;
    }
    Value value=native_value(runtime,function);
    if(value.kind!=VAL_FUNCTION || !vm_value_is_compiled_function(runtime->vm,value)){
        fprintf(stderr,"Kirton Native: значение не является функцией\n");
        runtime_fail(runtime);return NULL;
    }
    KirtonNativeCompiledDescriptor* desc=(KirtonNativeCompiledDescriptor*)vm_value_compiled_descriptor(value);
    int found=0;
    for(size_t i=0;i<runtime->compiled_count;++i)
        if (runtime->compiled_functions[i]==desc){found=1;break;}
    if(!found || !desc || desc->argc!=argc || !desc->compiled){
        fprintf(stderr,"Kirton Native: некорректный вызов функции\n");
        runtime_fail(runtime);return NULL;
    }
    /* The caller's frame retains all argument pins while the callee runs. */
    KirtonNativeValue* result=desc->compiled(runtime,function,arguments);
    if (!result) runtime_fail(runtime);
    return result;
}

KirtonNativeFrame* kirton_native_frame_enter(KirtonNativeRuntime* runtime){
    if (!runtime || !runtime->vm || runtime->status) return NULL;
    KirtonNativeFrame* frame=(KirtonNativeFrame*)kir_malloc(sizeof(*frame));
    frame->runtime=runtime;
    frame->parent=runtime->frame;
    frame->values=NULL;
    runtime->frame=frame;
    return frame;
}

void kirton_native_frame_leave(KirtonNativeRuntime* runtime,
                               KirtonNativeFrame* frame){
    if (!runtime || !frame || runtime->frame!=frame || frame->runtime!=runtime){
        runtime_fail(runtime);
        return;
    }
    runtime->frame=frame->parent;
    KirtonNativeValue* value=frame->values;
    while (value){
        KirtonNativeValue* next=value->next;
        vm_value_unpin(runtime->vm,value->pin);
        kir_free(value);
        value=next;
    }
    kir_free(frame);
}

KirtonNativeValue* kirton_native_frame_return(KirtonNativeRuntime* runtime,
                                              KirtonNativeFrame* frame,
                                              const KirtonNativeValue* value){
    if (!runtime || !frame || runtime->frame!=frame || !frame->parent){
        runtime_fail(runtime);
        return NULL;
    }
    Value returned=native_value(runtime,value);
    KirtonNativeValue* transferred=value_in_frame(runtime,frame->parent,returned);
    kirton_native_frame_leave(runtime,frame);
    return transferred;
}

KirtonNativeValue* kirton_native_root_value(KirtonNativeRuntime* runtime,
                                            const KirtonNativeValue* value){
    if (!runtime || !runtime->root){ runtime_fail(runtime); return NULL; }
    KirtonNativeValue* global=value_in_frame(runtime,runtime->root,
                                              native_value(runtime,value));
    if (global) global->global_anchor=1;
    return global;
}

KirtonNativeValue* kirton_native_string(KirtonNativeRuntime* runtime,
                                        const char* utf8,size_t length){
    if (!runtime || runtime->status || !runtime->frame){
        runtime_fail(runtime);
        return NULL;
    }
    Value value=vm_value_string_new(runtime->vm,utf8,length);
    if (value.kind!=VAL_STR){ runtime_fail(runtime); return NULL; }
    return value_in_frame(runtime,runtime->frame,value);
}

KirtonNativeValue* kirton_native_number(KirtonNativeRuntime* runtime,
                                        const char* text,size_t length,
                                        int is_integer){
    if (!runtime || runtime->status || !runtime->frame){
        runtime_fail(runtime);
        return NULL;
    }
    Value value=v_nil();
    if (!vm_value_number_parse(runtime->vm,text,length,is_integer,&value)){
        runtime_fail(runtime);
        return value_in_frame(runtime,runtime->frame,v_nil());
    }
    return value_in_frame(runtime,runtime->frame,value);
}

KirtonNativeValue* kirton_native_nil(KirtonNativeRuntime* runtime){
    if (!runtime || runtime->status || !runtime->frame){
        runtime_fail(runtime);
        return NULL;
    }
    return value_in_frame(runtime,runtime->frame,v_nil());
}

KirtonNativeValue* kirton_native_i64(KirtonNativeRuntime* runtime,int64_t value){
    if (!runtime || !runtime->frame){ runtime_fail(runtime); return NULL; }
    return value_in_frame(runtime,runtime->frame,v_int(value));
}

KirtonNativeValue* kirton_native_bool(KirtonNativeRuntime* runtime,int64_t value){
    if (!runtime || !runtime->frame || runtime->status){
        runtime_fail(runtime);
        return NULL;
    }
    return value_in_frame(runtime,runtime->frame,v_bool(value!=0));
}

/* Dynamic values are the existing VM Value type, never a second tagged object.
   This path is entered only when static inference has promoted an expression
   to NATIVE_ANY. Preserve the language's numeric/string addition dispatch. */
/* Native collections use the same managed Value heap and GC as the VM.
   Builders are pinned immediately, before any child expression is evaluated. */
KirtonNativeValue* kirton_native_list_new(KirtonNativeRuntime* runtime){
    if (!runtime || !runtime->frame || runtime->status) return NULL;
    return value_in_frame(runtime,runtime->frame,vm_value_list_new(runtime->vm));
}

KirtonNativeValue* kirton_native_dict_new(KirtonNativeRuntime* runtime){
    if (!runtime || !runtime->frame || runtime->status) return NULL;
    return value_in_frame(runtime,runtime->frame,vm_value_dict_new(runtime->vm));
}

void kirton_native_list_append(KirtonNativeRuntime* runtime,
                               const KirtonNativeValue* collection,
                               const KirtonNativeValue* item){
    if (!runtime || runtime->status) return;
    Value list=native_value(runtime,collection);
    if (list.kind!=VAL_LIST){runtime_fail(runtime);return;}
    v_list_push(&list,native_value(runtime,item));
}

void kirton_native_dict_put_literal(KirtonNativeRuntime* runtime,
                                    const KirtonNativeValue* collection,
                                    const char* key,size_t length,
                                    const KirtonNativeValue* item){
    if (!runtime || runtime->status) return;
    Value dict=native_value(runtime,collection);
    if (dict.kind!=VAL_DICT){runtime_fail(runtime);return;}
    v_dict_set(&dict,key,length,native_value(runtime,item));
}

static void native_index_error(KirtonNativeRuntime* runtime,const char* message){
    if (runtime && !runtime->status){
        kir_diag_error_simple("<native>",message);
        /* Runtime evaluation errors (unlike compiler/ABI errors) exit 1. */
        runtime->status=1;
    }
}

/* IDs match native_builtin_id() in the compiler. Keep the subset
   conservative: these operations share the existing VM heap and Value ABI. */
KirtonNativeValue* kirton_native_collection_builtin(KirtonNativeRuntime* runtime,
                                                   int builtin_id,
                                                   const KirtonNativeValue* const* args,
                                                   size_t argc){
    if (!runtime || runtime->status || !runtime->frame || (argc && !args))
        return NULL;
    Value a=argc>0 ? native_value(runtime,args[0]) : v_nil();
    Value b=argc>1 ? native_value(runtime,args[1]) : v_nil();
    Value c=argc>2 ? native_value(runtime,args[2]) : v_nil();
    Value out=v_nil();
    if (builtin_id>=KIRTON_NATIVE_STDLIB_FIRST_ID &&
        (size_t)(builtin_id-KIRTON_NATIVE_STDLIB_FIRST_ID)<KIRTON_NATIVE_STDLIB_COUNT){
        /* The arguments are all pinned before a call reaches this point.
           Enter the VM's memory domain for any strings/lists/BigInts created
           by the existing stdlib implementation; restore TLS afterward. */
        if (argc>(size_t)INT_MAX){ runtime_fail(runtime); return NULL; }
        Value* argv=argc ? (Value*)kir_malloc(argc*sizeof(Value)) : NULL;
        if (argc && !argv){ runtime_fail(runtime); return NULL; }
        for (size_t i=0;i<argc;++i) argv[i]=native_value(runtime,args[i]);
        size_t id=(size_t)(builtin_id-KIRTON_NATIVE_STDLIB_FIRST_ID);
        const char* name=kirton_native_stdlib_names[id];
        Value result=v_nil();
        int found=vm_value_native_call(runtime->vm,name,strlen(name),(int)argc,argv,&result);
        kir_free(argv);
        if (!found){ runtime_fail(runtime); return NULL; }
        return value_in_frame(runtime,runtime->frame,result);
    }
    switch(builtin_id){
    case 1: out=vm_value_list_new(runtime->vm);break;
    case 2: out=v_int_from_size(a.kind==VAL_LIST?a.as.list->len:0);break;
    case 3:
        if (argc>=2 && a.kind==VAL_LIST){
            v_list_push(&a,b);
            out=v_bool(true);
        } else out=v_bool(false);
        break;
    case 4: out=vm_value_dict_new(runtime->vm);break;
    case 5: {
        size_t alive=0;
        if (a.kind==VAL_DICT)
            for (size_t i=0;i<a.as.dict->len;++i)
                if (a.as.dict->vals[i].kind!=VAL_NIL) ++alive;
        out=v_int_from_size(alive);
        break;
    }
    case 6: {
        Value found=v_nil();
        int ok=argc>=2 && a.kind==VAL_DICT && b.kind==VAL_STR &&
            v_dict_get(&a,b.as.str.s,b.as.str.n,&found);
        out=v_bool(ok && found.kind!=VAL_NIL);
        break;
    }
    case 7:
        if (argc>=3 && a.kind==VAL_DICT && b.kind==VAL_STR){
            v_dict_set(&a,b.as.str.s,b.as.str.n,c);
            out=v_bool(true);
        } else out=v_bool(false);
        break;
    case 8: /* список_получить */
        if(argc>=2 && a.kind==VAL_LIST && b.kind==VAL_NUM){
            int64_t i=0;
            if(v_num_fits_i64(b,&i) && i>=0 && (uint64_t)i<a.as.list->len)
                out=a.as.list->data[(size_t)i];
            else if(argc>=3) out=c;
        }
        break;
    case 9: /* список_удалить */
        if(argc>=2 && a.kind==VAL_LIST && b.kind==VAL_NUM){
            int64_t i=0;
            if(v_num_fits_i64(b,&i) && i>=0 && (uint64_t)i<a.as.list->len){
                VList* list=a.as.list;
                out=list->data[(size_t)i];
                if((size_t)i+1<list->len) memmove(list->data+i,list->data+i+1,
                    (list->len-(size_t)i-1)*sizeof(Value));
                --list->len;
            }
        }
        break;
    case 10: /* список_вставить */
        out=v_bool(false);
        if(argc>=3 && a.kind==VAL_LIST && b.kind==VAL_NUM){
            int64_t i=0;
            if(v_num_fits_i64(b,&i) && a.as.list->len<SIZE_MAX){
                VList* list=a.as.list;
                size_t index=i<0?0:(uint64_t)i>list->len?list->len:(size_t)i;
                /* Allocate through Value's own public API; it owns the heap. */
                v_list_push(&a,v_nil());
                if(list->len>index+1) memmove(list->data+index+1,
                    list->data+index,(list->len-index-1)*sizeof(Value));
                list->data[index]=c;
                out=v_bool(true);
            }
        }
        break;
    case 11: /* словарь_получить */
        if(argc>=2 && a.kind==VAL_DICT && b.kind==VAL_STR){
            Value found=v_nil();
            if(v_dict_get(&a,b.as.str.s,b.as.str.n,&found) && found.kind!=VAL_NIL)
                out=found;
            else if(argc>=3) out=c;
        }
        break;
    case 12: /* словарь_удалить */
        if(argc>=2 && a.kind==VAL_DICT && b.kind==VAL_STR){
            Value found=v_nil();
            if(v_dict_get(&a,b.as.str.s,b.as.str.n,&found) && found.kind!=VAL_NIL){
                out=found;
                v_dict_set(&a,b.as.str.s,b.as.str.n,v_nil());
            }
        }
        break;
    case 13: /* сумма */
    case 14: { /* произведение */
        out=v_int(builtin_id==13?0:1);
        if(a.kind==VAL_LIST) for(size_t i=0;i<a.as.list->len;++i){
            Value element=a.as.list->data[i];
            if(element.kind!=VAL_NUM) continue;
            Value next=v_nil();
            int status=vm_value_numeric_binary(runtime->vm,
                builtin_id==13?KIR_NUM_ADD:KIR_NUM_MUL,out,element,&next);
            if(status!=KIR_NUMERIC_OK){runtime_fail(runtime);break;}
            out=next;
        }
        break;
    }
    default:runtime_fail(runtime);return NULL;
    }
    return value_in_frame(runtime,runtime->frame,out);
}

KirtonNativeValue* kirton_native_iter_next(KirtonNativeRuntime* runtime,
                                           const KirtonNativeValue* sequence,
                                           size_t* cursor){
    if (!runtime || !runtime->frame || runtime->status || !cursor) return NULL;
    Value seq=native_value(runtime,sequence);
    if (runtime->status) return NULL;
    if (seq.kind==VAL_LIST){
        if (*cursor>=seq.as.list->len) return NULL;
        Value item=seq.as.list->data[(*cursor)++];
        return value_in_frame(runtime,runtime->frame,item);
    }
    if (seq.kind==VAL_STR){
        if (*cursor>=seq.as.str.n) return NULL;
        const char* p=seq.as.str.s+*cursor;
        const char* end=seq.as.str.s+seq.as.str.n;
        uint32_t codepoint=0;
        int width=kir_utf8_decode(p,end,&codepoint);
        if (width<=0) width=1; /* AST treats invalid bytes as one byte. */
        *cursor+=(size_t)width;
        Value character=vm_value_string_new(runtime->vm,p,(size_t)width);
        return value_in_frame(runtime,runtime->frame,character);
    }
    return NULL; /* AST silently skips non-iterable types. */
}

KirtonNativeValue* kirton_native_collection_index(KirtonNativeRuntime* runtime,
                                                  const KirtonNativeValue* collection,
                                                  const KirtonNativeValue* index){
    if (!runtime || !runtime->frame || runtime->status) return NULL;
    Value base=native_value(runtime,collection),idx=native_value(runtime,index);
    Value result=v_nil();
    if (base.kind==VAL_LIST && idx.kind==VAL_NUM){
        int64_t i=0;
        if (!v_num_fits_i64(idx,&i))
            native_index_error(runtime,"Индекс списка должен быть целым в диапазоне int64");
        else if (i<0 || (uint64_t)i>=(uint64_t)base.as.list->len)
            native_index_error(runtime,"Индекс списка вне диапазона");
        else result=base.as.list->data[(size_t)i];
    } else if (base.kind==VAL_DICT && idx.kind==VAL_STR){
        (void)v_dict_get(&base,idx.as.str.s,idx.as.str.n,&result);
    } else if (base.kind==VAL_STR && idx.kind==VAL_NUM){
        int64_t i=0;
        if (!v_num_fits_i64(idx,&i) || i<0){
            native_index_error(runtime,"Индекс строки должен быть неотрицательным целым");
        } else {
            const char* p=base.as.str.s;
            const char* end=p+base.as.str.n;
            int64_t n=0;
            while (p<end){
                uint32_t cp=0;
                int width=kir_utf8_decode(p,end,&cp);
                if (width<=0){
                    native_index_error(runtime,"Строка содержит недопустимый UTF-8");
                    break;
                }
                if (n==i){
                    result=vm_value_string_new(runtime->vm,p,(size_t)width);
                    break;
                }
                p+=width;
                ++n;
            }
            if (p>=end) native_index_error(runtime,"Индекс строки вне диапазона");
        }
    }
    return value_in_frame(runtime,runtime->frame,result);
}

void kirton_native_collection_set(KirtonNativeRuntime* runtime,
                                  const KirtonNativeValue* collection,
                                  const KirtonNativeValue* index,
                                  const KirtonNativeValue* value){
    if (!runtime || runtime->status) return;
    Value base=native_value(runtime,collection),idx=native_value(runtime,index);
    Value replacement=native_value(runtime,value);
    if (base.kind==VAL_LIST && idx.kind==VAL_NUM){
        int64_t i=0;
        if (!v_num_fits_i64(idx,&i))
            native_index_error(runtime,"Индекс списка должен быть целым в диапазоне int64");
        else if (i<0 || (uint64_t)i>=(uint64_t)base.as.list->len)
            native_index_error(runtime,"Индекс списка вне диапазона");
        else base.as.list->data[(size_t)i]=replacement;
    } else if (base.kind==VAL_DICT && idx.kind==VAL_STR){
        v_dict_set(&base,idx.as.str.s,idx.as.str.n,replacement);
    } else native_index_error(runtime,"Неверные типы для присваивания по индексу");
}


static char* native_copy_name(const char* name,size_t len){
    char* result=(char*)kir_malloc(len+1);
    if (len) memcpy(result,name,len);
    result[len]=0;
    return result;
}

void kirton_native_type_define(KirtonNativeRuntime* runtime,size_t id,int kind,
                               const char* name,size_t name_len,size_t fields){
    if (!runtime || runtime->status || (kind!=0 && kind!=1) ||
        id!=runtime->type_count || (!name && name_len) ||
        fields>SIZE_MAX/sizeof(AstTypeField)){runtime_fail(runtime);return;}
    AstUserType* type=(AstUserType*)kir_malloc(sizeof(*type));
    memset(type,0,sizeof(*type));
    type->kind=kind==0 ? AST_TYPE_STRUCT : AST_TYPE_ENUM;
    type->name=native_copy_name(name,name_len);
    type->name_len=name_len;
    if (kind==0){
        type->field_count=fields;
        type->fields=(AstTypeField*)kir_malloc(sizeof(AstTypeField)*(fields?fields:1));
        memset(type->fields,0,sizeof(AstTypeField)*(fields?fields:1));
    }
    AstUserType** next=(AstUserType**)kir_malloc((id+1)*sizeof(*next));
    if (id) memcpy(next,runtime->types,id*sizeof(*next));
    next[id]=type;
    kir_free(runtime->types);
    runtime->types=next;
    runtime->type_count=id+1;
}

void kirton_native_type_field(KirtonNativeRuntime* runtime,size_t id,size_t index,
                              const char* name,size_t name_len,
                              const char* type_name,size_t type_len,
                              size_t type_id_plus_one){
    if (!runtime || runtime->status || id>=runtime->type_count ||
        !runtime->types[id] || runtime->types[id]->kind!=AST_TYPE_STRUCT ||
        index>=runtime->types[id]->field_count || !name || !type_name){
        runtime_fail(runtime);return;
    }
    AstTypeField* field=&runtime->types[id]->fields[index];
    if (field->name || field->type_name){runtime_fail(runtime);return;}
    field->name=native_copy_name(name,name_len);
    field->name_len=name_len;
    field->type_name=native_copy_name(type_name,type_len);
    field->type_name_len=type_len;
    /* References may point to types registered later during initialization;
       the target ID is validated before inspecting the value. */
    field->native_type_id_plus_one=type_id_plus_one;
}

static int native_type_eq(const char* name,size_t length,const char* reference){
    size_t n=strlen(reference);
    return n==length && memcmp(name,reference,n)==0;
}

static int native_type_accepts(KirtonNativeRuntime* runtime,
                                const AstTypeField* field,Value value){
    const char* name=field->type_name;
    size_t length=field->type_name_len;
    if (native_type_eq(name,length,"пусто")) return value.kind==VAL_NIL;
    if (native_type_eq(name,length,"булево")) return value.kind==VAL_BOOL;
    if (native_type_eq(name,length,"целое"))
        return value.kind==VAL_NUM && v_num_is_integer(value);
    if (native_type_eq(name,length,"число"))
        return value.kind==VAL_NUM && !v_num_is_integer(value);
    if (native_type_eq(name,length,"строка")) return value.kind==VAL_STR;
    if (native_type_eq(name,length,"список")) return value.kind==VAL_LIST;
    if (native_type_eq(name,length,"словарь")) return value.kind==VAL_DICT;
    if(field->native_type_id_plus_one){
        size_t index=field->native_type_id_plus_one-1;
        if(index>=runtime->type_count || !runtime->types[index]) return 0;
        AstUserType* type=runtime->types[index];
        return type->kind==AST_TYPE_STRUCT ?
            v_user_struct_type(value)==type : v_enum_value_type(value)==type;
    }
    return 0;
}

static void native_user_error(KirtonNativeRuntime* runtime,const char* message){
    if (runtime && !runtime->status){
        kir_diag_error_simple("<native>",message);
        runtime->status=1;
    }
}

KirtonNativeValue* kirton_native_struct_construct(KirtonNativeRuntime* runtime,
    size_t id,const char* const* field_names,const size_t* field_lens,
    const KirtonNativeValue* const* values,size_t count){
    if (!runtime || runtime->status || !runtime->frame ||
        id>=runtime->type_count) return NULL;
    AstUserType* type=runtime->types[id];
    if (!type || type->kind!=AST_TYPE_STRUCT || count!=type->field_count){
        native_user_error(runtime,"Неверное число аргументов структуры");
        return NULL;
    }
    Value fields=vm_value_dict_new(runtime->vm);
    KirtonNativeValue* protected_fields=value_in_frame(runtime,runtime->frame,fields);
    if (!protected_fields) return NULL;
    for (size_t i=0;i<count;++i){
        const AstTypeField* declared=NULL;
        if (!field_names || !field_lens || !values || !values[i]){
            native_user_error(runtime,"Неизвестное поле структуры");return NULL;
        }
        for (size_t f=0;f<type->field_count;++f){
            if (type->fields[f].name_len==field_lens[i] &&
                !memcmp(type->fields[f].name,field_names[i],field_lens[i])){
                declared=&type->fields[f];break;
            }
        }
        if (!declared || v_dict_find(&fields,field_names[i],field_lens[i])>=0){
            native_user_error(runtime,"Неизвестное или повторное поле структуры");
            return NULL;
        }
        Value value=native_value(runtime,values[i]);
        if (!native_type_accepts(runtime,declared,value)){
            native_user_error(runtime,"Неверный тип поля структуры");return NULL;
        }
        v_dict_set(&fields,field_names[i],field_lens[i],value);
    }
    Value instance=vm_value_user_struct_new(runtime->vm,type,fields);
    if (instance.kind!=VAL_USER_STRUCT){runtime_fail(runtime);return NULL;}
    return value_in_frame(runtime,runtime->frame,instance);
}

KirtonNativeValue* kirton_native_enum_constant(KirtonNativeRuntime* runtime,
    size_t id,const char* variant,size_t length){
    if (!runtime || runtime->status || !runtime->frame ||
        id>=runtime->type_count || !runtime->types[id] ||
        runtime->types[id]->kind!=AST_TYPE_ENUM){runtime_fail(runtime);return NULL;}
    Value value=vm_value_enum_new(runtime->vm,runtime->types[id],variant,length);
    if (value.kind!=VAL_ENUM_VALUE){runtime_fail(runtime);return NULL;}
    return value_in_frame(runtime,runtime->frame,value);
}

KirtonNativeValue* kirton_native_attr_get(KirtonNativeRuntime* runtime,
    const KirtonNativeValue* object,const char* name,size_t length){
    if (!runtime || runtime->status || !runtime->frame) return NULL;
    Value base=native_value(runtime,object),result=v_nil();
    if (base.kind==VAL_USER_STRUCT){
        Value fields=v_user_struct_fields(base);
        if (!v_dict_get(&fields,name,length,&result))
            native_user_error(runtime,"Неизвестное поле структуры");
    } else if (base.kind==VAL_DICT){
        (void)v_dict_get(&base,name,length,&result);
    } else if (base.kind==VAL_RECORD){
        if(!v_record_get(base,name,length,&result))
            native_user_error(runtime,"Недопустимое поле записи памяти");
    }
    return value_in_frame(runtime,runtime->frame,result);
}

void kirton_native_attr_set(KirtonNativeRuntime* runtime,
    const KirtonNativeValue* object,const char* name,size_t length,
    const KirtonNativeValue* replacement){
    if (!runtime || runtime->status) return;
    Value base=native_value(runtime,object),value=native_value(runtime,replacement);
    if (base.kind==VAL_USER_STRUCT){
        AstUserType* type=v_user_struct_type(base);
        const AstTypeField* field=NULL;
        if (type) for(size_t i=0;i<type->field_count;++i){
            if (type->fields[i].name_len==length &&
                !memcmp(type->fields[i].name,name,length)){
                field=&type->fields[i];break;
            }
        }
        if (!field || !native_type_accepts(runtime,field,value)){
            native_user_error(runtime,"Неверное поле или тип структуры");return;
        }
        Value fields=v_user_struct_fields(base);
        v_dict_set(&fields,name,length,value);
    } else if (base.kind==VAL_DICT){
        v_dict_set(&base,name,length,value);
    } else if(base.kind==VAL_RECORD){
        if(!v_record_set(base,name,length,value))
            native_user_error(runtime,"Неверное поле, тип или срок жизни записи памяти");
    } else native_user_error(runtime,"Присваивание полю требует структуру, словарь или запись");
}

KirtonNativeValue* kirton_native_dynamic_add(KirtonNativeRuntime* runtime,
                                             const KirtonNativeValue* left,
                                             const KirtonNativeValue* right){
    if (!runtime || runtime->status || !runtime->frame) return NULL;
    Value a=native_value(runtime,left),b=native_value(runtime,right);
    if (a.kind==VAL_STR && b.kind==VAL_STR)
        return kirton_native_string_concat(runtime,left,right);
    if (a.kind==VAL_NUM && b.kind==VAL_NUM)
        return kirton_native_numeric_binary(runtime,KIR_NUM_ADD,left,right);
    /* eval_binop returns nil (without a diagnostic) for non-matching '+' kinds. */
    return value_in_frame(runtime,runtime->frame,v_nil());
}

int kirton_native_equal(KirtonNativeRuntime* runtime,
                        const KirtonNativeValue* left,
                        const KirtonNativeValue* right){
    if (!runtime || runtime->status) return 0;
    return eval_values_equal(native_value(runtime,left),native_value(runtime,right));
}

KirtonNativeValue* kirton_native_hold_value(KirtonNativeRuntime* runtime,
                                            const KirtonNativeValue* value){
    if (!runtime || runtime->status || !runtime->frame){
        runtime_fail(runtime);
        return NULL;
    }
    return value_in_frame(runtime,runtime->frame,native_value(runtime,value));
}

KirtonNativeValue* kirton_native_string_concat(KirtonNativeRuntime* runtime,
                                               const KirtonNativeValue* left,
                                               const KirtonNativeValue* right){
    if (!runtime || runtime->status || !runtime->frame) return NULL;
    Value a=native_value(runtime,left),b=native_value(runtime,right);
    if (a.kind!=VAL_STR || b.kind!=VAL_STR ||
        a.as.str.n>SIZE_MAX-b.as.str.n-1){
        runtime_fail(runtime);
        return NULL;
    }
    size_t total=a.as.str.n+b.as.str.n;
    char* utf8=(char*)kir_malloc(total+1);
    if (a.as.str.n) memcpy(utf8,a.as.str.s,a.as.str.n);
    if (b.as.str.n) memcpy(utf8+a.as.str.n,b.as.str.s,b.as.str.n);
    utf8[total]='\0';
    Value result=vm_value_string_new(runtime->vm,utf8,total);
    kir_free(utf8);
    if (result.kind!=VAL_STR){runtime_fail(runtime); return NULL;}
    return value_in_frame(runtime,runtime->frame,result);
}

int kirton_native_string_equal(KirtonNativeRuntime* runtime,
                               const KirtonNativeValue* left,
                               const KirtonNativeValue* right){
    if (!runtime || runtime->status) return 0;
    Value a=native_value(runtime,left),b=native_value(runtime,right);
    if (a.kind!=VAL_STR || b.kind!=VAL_STR){runtime_fail(runtime);return 0;}
    return a.as.str.n==b.as.str.n &&
        (!a.as.str.n || memcmp(a.as.str.s,b.as.str.s,a.as.str.n)==0);
}

static int numeric_error(KirtonNativeRuntime* runtime,int status,int operation){
    if (!runtime) return 0;
    if (status==KIR_NUMERIC_DIVIDE_BY_ZERO){
        if (!runtime->status){
            if (operation==KIR_NUM_DIV)
                kir_diag_error_simple("<native>","Деление на ноль");
            else
                kir_diag_error_simple("<native>","Деление на ноль в операции деления/остатка");
            runtime->status=70;
        }
        return 1;
    }
    if (status==KIR_NUMERIC_INVALID && !runtime->status){
        if (operation==KIR_NUM_SHL || operation==KIR_NUM_SHR)
            kir_diag_error_simple("<native>",
                "Сдвиг требует два целых числа и неотрицательную величину сдвига в диапазоне size_t");
        else if (operation==KIR_NUM_BITAND || operation==KIR_NUM_BITOR ||
                 operation==KIR_NUM_BITXOR)
            kir_diag_error_simple("<native>","Побитовая операция требует два целых числа");
    }
    if (status!=KIR_NUMERIC_OK) runtime_fail(runtime);
    return status!=KIR_NUMERIC_OK;
}

/* Polymorphic runtime operations keep the AST/Bytecode contract: invalid
   kinds for + - * / evaluate to nil, while bitwise / floor-divide / modulo
   report a runtime error. No unsafe casts or implicit unboxing. */
KirtonNativeValue* kirton_native_dynamic_numeric(KirtonNativeRuntime* runtime,
                                                 int operation,
                                                 const KirtonNativeValue* left,
                                                 const KirtonNativeValue* right){
    if (!runtime || runtime->status || !runtime->frame) return NULL;
    Value a=native_value(runtime,left),b=native_value(runtime,right);
    if (a.kind==VAL_NUM && b.kind==VAL_NUM)
        return kirton_native_numeric_binary(runtime,operation,left,right);
    if (operation==KIR_NUM_FLOOR_DIV || operation==KIR_NUM_MOD ||
        operation==KIR_NUM_BITAND || operation==KIR_NUM_BITOR ||
        operation==KIR_NUM_BITXOR || operation==KIR_NUM_SHL ||
        operation==KIR_NUM_SHR){
        kir_diag_error_simple("<native>","Операция требует два целых/числовых значения");
        runtime_fail(runtime);
    }
    return value_in_frame(runtime,runtime->frame,v_nil());
}

KirtonNativeValue* kirton_native_dynamic_compare(KirtonNativeRuntime* runtime,
                                                 int operation,
                                                 const KirtonNativeValue* left,
                                                 const KirtonNativeValue* right){
    if (!runtime || runtime->status || !runtime->frame) return NULL;
    Value a=native_value(runtime,left),b=native_value(runtime,right);
    if (a.kind!=VAL_NUM || b.kind!=VAL_NUM)
        return value_in_frame(runtime,runtime->frame,v_nil());
    Value result=v_nil();
    int status=vm_value_numeric_binary(runtime->vm,(KirNumericOperation)operation,a,b,&result);
    if (numeric_error(runtime,status,operation)) return value_in_frame(runtime,runtime->frame,v_nil());
    return value_in_frame(runtime,runtime->frame,result);
}

KirtonNativeValue* kirton_native_dynamic_neg(KirtonNativeRuntime* runtime,
                                             const KirtonNativeValue* value){
    if (!runtime || runtime->status || !runtime->frame) return NULL;
    Value a=native_value(runtime,value);
    if (a.kind!=VAL_NUM) return value_in_frame(runtime,runtime->frame,v_nil());
    return kirton_native_numeric_neg(runtime,value);
}

KirtonNativeValue* kirton_native_numeric_binary(KirtonNativeRuntime* runtime,
                                                int operation,
                                                const KirtonNativeValue* left,
                                                const KirtonNativeValue* right){
    if (!runtime || !runtime->frame){ runtime_fail(runtime); return NULL; }
    Value result=v_nil();
    int status=vm_value_numeric_binary(runtime->vm,(KirNumericOperation)operation,
        native_value(runtime,left),native_value(runtime,right),&result);
    if (numeric_error(runtime,status,operation)) result=v_nil();
    return value_in_frame(runtime,runtime->frame,result);
}

KirtonNativeValue* kirton_native_numeric_neg(KirtonNativeRuntime* runtime,
                                             const KirtonNativeValue* value){
    if (!runtime || !runtime->frame){ runtime_fail(runtime); return NULL; }
    Value result=v_nil();
    if (!vm_value_numeric_neg(runtime->vm,native_value(runtime,value),&result)){
        runtime_fail(runtime);
        result=v_nil();
    }
    return value_in_frame(runtime,runtime->frame,result);
}

KirtonNativeValue* kirton_native_numeric_bitnot(KirtonNativeRuntime* runtime,
                                                const KirtonNativeValue* value){
    if (!runtime || !runtime->frame || runtime->status) return NULL;
    Value result=v_nil();
    if (!vm_value_numeric_bitnot(runtime->vm,native_value(runtime,value),&result)){
        kir_diag_error_simple("<native>","Побитовое НЕ требует целое число");
        runtime_fail(runtime);
    }
    return value_in_frame(runtime,runtime->frame,result);
}

int kirton_native_numeric_compare(KirtonNativeRuntime* runtime,int operation,
                                  const KirtonNativeValue* left,
                                  const KirtonNativeValue* right){
    if (!runtime || runtime->status) return 0;
    Value result=v_nil();
    int status=vm_value_numeric_binary(runtime->vm,(KirNumericOperation)operation,
        native_value(runtime,left),native_value(runtime,right),&result);
    if (numeric_error(runtime,status,operation)) return 0;
    return result.kind==VAL_BOOL && result.as.boolean;
}

void kirton_native_print_value(KirtonNativeRuntime* runtime,
                               const KirtonNativeValue* value){
    if (!runtime || runtime->status) return;
    v_print(native_value(runtime,value));
}

void kirton_native_print_i64(KirtonNativeRuntime* runtime,int64_t value){
    if (runtime && !runtime->status) v_print(v_int(value));
}

void kirton_native_print_bool(KirtonNativeRuntime* runtime,int64_t value){
    if (runtime && !runtime->status) v_print(v_bool(value!=0));
}

void kirton_native_print_space(KirtonNativeRuntime* runtime){
    if (runtime && !runtime->status) putchar(' ');
}

void kirton_native_print_end(KirtonNativeRuntime* runtime){
    if (runtime && !runtime->status) putchar('\n');
}

int kirton_native_truthy(KirtonNativeRuntime* runtime,
                         const KirtonNativeValue* value){
    if (!runtime || runtime->status) return 0;
    return v_truthy(native_value(runtime,value)) ? 1 : 0;
}

void kirton_native_safepoint(KirtonNativeRuntime* runtime,
                             const KirtonNativeValue* const* live_values,
                             size_t live_count){
    if (!runtime || runtime->status || !runtime->frame) return;
    KirtonNativeValue** link=&runtime->frame->values;
    while (*link){
        KirtonNativeValue* value=*link;
        int live=0;
        for (size_t i=0;i<live_count;++i){
            if (live_values[i]==value){ live=1; break; }
        }
        if (live){ link=&value->next; continue; }
        *link=value->next;
        vm_value_unpin(runtime->vm,value->pin);
        kir_free(value);
    }
    /* Global updates in a long-running child function used to leave every
       obsolete root-frame pin alive until program shutdown. Sweep only
       global-owned handles here: root-frame expression temporaries may still
       be in use by a suspended caller and must never be reclaimed here.
       Generated loads of global managed values acquire a frame-local pin. */
    if (runtime->frame!=runtime->root){
        KirtonNativeValue** root_link=&runtime->root->values;
        while (*root_link){
            KirtonNativeValue* value=*root_link;
            int live=!value->global_anchor;
            if (!live){
                for (size_t i=0;i<live_count;++i){
                    if (live_values[i]==value){live=1;break;}
                }
            }
            if (live){root_link=&value->next;continue;}
            *root_link=value->next;
            vm_value_unpin(runtime->vm,value->pin);
            kir_free(value);
        }
    }
    (void)vm_gc_collect(runtime->vm);
}

static void native_function_table_free(KirtonNativeRuntime* runtime){
    for (size_t i=0;i<runtime->compiled_count;++i)
        kir_free(runtime->compiled_functions[i]);
    kir_free(runtime->compiled_functions);
    runtime->compiled_functions=NULL;
    runtime->compiled_count=0;
}

static void native_type_table_free(KirtonNativeRuntime* runtime){
    for (size_t i=0;i<runtime->type_count;++i){
        AstUserType* type=runtime->types[i];
        if (!type) continue;
        for (size_t f=0;f<type->field_count;++f){
            kir_free((char*)type->fields[f].name);
            kir_free((char*)type->fields[f].type_name);
        }
        kir_free(type->fields);
        kir_free((char*)type->name);
        kir_free(type);
    }
    kir_free(runtime->types);
}

int kirton_native_runtime_destroy(KirtonNativeRuntime* runtime){
    if (!runtime) return 70;
    while (runtime->frame){
        KirtonNativeFrame* frame=runtime->frame;
        kirton_native_frame_leave(runtime,frame);
    }
    int status=runtime->status;
    int freed=vm_try_free(runtime->vm);
    if (!freed){
        runtime_fail(runtime);
        return runtime->status;
    }
    native_type_table_free(runtime);
    native_function_table_free(runtime);
    kir_free(runtime);
    return status;
}

/* A caught runtime diagnostic is transferred into the caller's root-frame
   Value heap. This is deliberately independent of AST/Bytecode evaluation. */
void kirton_native_try_enter(KirtonNativeRuntime* runtime){
    if(runtime) kir_diag_silence_push();
}

void kirton_native_try_leave(KirtonNativeRuntime* runtime){
    if(!runtime) return;
    kir_diag_silence_pop();
    if(runtime->status) kir_diag_replay_last_error();
}

int kirton_native_error_pending(KirtonNativeRuntime* runtime){
    return runtime && runtime->status != 0;
}

int kirton_native_error_suspend(KirtonNativeRuntime* runtime){
    if(!runtime) return 70;
    int result=runtime->status;
    runtime->status=0;
    return result;
}

void kirton_native_error_resume(KirtonNativeRuntime* runtime,int prior){
    if(runtime && !runtime->status && prior){
        runtime->status=prior;
        /* The try frame has already left its diagnostic silence. The
           original unhandled error must become visible unless an enclosing
           handler is still suppressing diagnostics. */
        kir_diag_replay_last_error();
    }
}

static void native_error_dict_text(KirtonNativeRuntime* runtime,
                                   Value* dict,const char* key,const char* value){
    Value v=vm_value_string_new(runtime->vm,value?value:"",value?strlen(value):0);
    v_dict_set(dict,key,strlen(key),v);
}

KirtonNativeValue* kirton_native_error_take(KirtonNativeRuntime* runtime){
    if(!runtime || !runtime->frame || !runtime->status) return NULL;
    runtime->status=0;
    KirDiagnostic diag={0};
    kir_diag_last_error(&diag);
    Value error=vm_value_dict_new(runtime->vm);
    KirtonNativeValue* pinned=value_in_frame(runtime,runtime->frame,error);
    if(!pinned) return NULL;
    native_error_dict_text(runtime,&error,"код","runtime");
    native_error_dict_text(runtime,&error,"сообщение",diag.message[0]?diag.message:"Native runtime error");
    native_error_dict_text(runtime,&error,"файл",diag.file[0]?diag.file:"<native>");
    v_dict_set(&error,"строка",strlen("строка"),diag.line>0?v_int(diag.line):v_nil());
    v_dict_set(&error,"столбец",strlen("столбец"),diag.col>0?v_int(diag.col):v_nil());
    Value stack=vm_value_list_new(runtime->vm);
    KirtonNativeValue* held_stack=value_in_frame(runtime,runtime->frame,stack);
    if(!held_stack) return NULL;
    v_dict_set(&error,"стек",strlen("стек"),stack);
    return pinned;
}

void kirton_native_runtime_abort(KirtonNativeRuntime* runtime,int status){
    if (!runtime) return;
    while (runtime->frame){
        KirtonNativeFrame* frame=runtime->frame;
        kirton_native_frame_leave(runtime,frame);
    }
    if (runtime->vm) (void)vm_try_free(runtime->vm);
    runtime->vm=NULL;
    runtime->status=status ? status : 70;
    native_type_table_free(runtime);
    native_function_table_free(runtime);
    kir_free(runtime);
}
