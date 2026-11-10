/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/ffi_internal.h"
#include "kirton/common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdarg.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
   typedef HMODULE KirFfiLibrary;
#else
#  include <dlfcn.h>
   typedef void* KirFfiLibrary;
#endif

typedef struct {
    KirFfiLibrary library;
    const KirFfiModuleV1* descriptor; /* V1 or NULL */
    const KirFfiModuleV2* descriptor_v2; /* V2 or NULL */
} LoadedModule;

struct KirFfiState {
    LoadedModule* loaded;
    size_t count, capacity;
    char error[256];
};

static void set_error(KirFfiState* s, const char* fmt, ...){
    va_list args;
    va_start(args,fmt);
    vsnprintf(s->error,sizeof(s->error),fmt,args);
    va_end(args);
}

static KirFfiLibrary open_library(const char* path){
#ifdef _WIN32
    if (!path || !*path) return NULL;
    int chars=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,NULL,0);
    if (chars<=0) return NULL;
    wchar_t* wide=(wchar_t*)kir_oom_malloc((size_t)chars*sizeof(wchar_t));
    if (!wide) return NULL;
    if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,chars)){
        free(wide); return NULL;
    }
    HMODULE lib=LoadLibraryW(wide);
    free(wide);
    return lib;
#else
    return dlopen(path,RTLD_NOW | RTLD_LOCAL);
#endif
}

static void close_library(KirFfiLibrary lib){
#ifdef _WIN32
    if (lib) FreeLibrary(lib);
#else
    if (lib) dlclose(lib);
#endif
}

static KirFfiModuleEntryV1 module_entry(KirFfiLibrary lib){
#ifdef _WIN32
    return (KirFfiModuleEntryV1)(void*)GetProcAddress(lib,"kirton_ffi_module_v1");
#else
    return (KirFfiModuleEntryV1)dlsym(lib,"kirton_ffi_module_v1");
#endif
}

static KirFfiModuleEntryV2 module_entry_v2(KirFfiLibrary lib){
#ifdef _WIN32
    return (KirFfiModuleEntryV2)(void*)GetProcAddress(lib,"kirton_ffi_module_v2");
#else
    return (KirFfiModuleEntryV2)dlsym(lib,"kirton_ffi_module_v2");
#endif
}
static const char* loaded_name(const LoadedModule* m){
    return m->descriptor_v2 ? m->descriptor_v2->module_name : m->descriptor->module_name;
}
KirFfiState* kir_ffi_state_create(void){
    return (KirFfiState*)kir_oom_calloc(1,sizeof(KirFfiState));
}
void kir_ffi_state_destroy(KirFfiState* s){
    if (!s) return;
    /* All callbacks must have returned before this call. */
    for (size_t i=s->count;i>0;--i) close_library(s->loaded[i-1].library);
    free(s->loaded);
    free(s);
}
const char* kir_ffi_state_error(const KirFfiState* s){
    return s ? s->error : "FFI не инициализирован";
}

static int valid_type(uint32_t tag, int allow_void){
    return (allow_void && tag==KIR_FFI_VOID) ||
           tag==KIR_FFI_I64 || tag==KIR_FFI_F64 ||
           tag==KIR_FFI_BOOL || tag==KIR_FFI_TEXT;
}
static size_t bounded_len(const char* text, size_t limit){
    size_t n=0;
    while (n<limit && text[n]) ++n;
    return n;
}
static int valid_name(const char* name){
    if (!name || !*name) return 0;
    size_t len=bounded_len(name,257);
    if (len>256) return 0;
    for (size_t i=0;i<len;++i) if (name[i]=='.') return 0;
    return 1;
}
static int validate_module(KirFfiState* s,const KirFfiModuleV1* m){
    if (!m || m->abi_version!=KIR_FFI_ABI_V1 ||
        m->struct_size!=sizeof(*m)){
        set_error(s,"Несовместимая версия или размер FFI ABI"); return 0;
    }
    if (!valid_name(m->module_name) || m->function_count==0 ||
        m->function_count>1024 || !m->functions){
        set_error(s,"Некорректное имя или список функций FFI"); return 0;
    }
    for (size_t i=0;i<s->count;++i){
        if (!strcmp(loaded_name(&s->loaded[i]),m->module_name)){
            set_error(s,"Модуль FFI '%s' уже загружен",m->module_name); return 0;
        }
    }
    for (uint32_t i=0;i<m->function_count;++i){
        const KirFfiFunctionV1* f=&m->functions[i];
        if (!valid_name(f->name) || !f->invoke || f->argc>KIR_FFI_MAX_ARGS ||
            (f->argc && !f->args) || !valid_type(f->returns,1)){
            set_error(s,"Некорректное описание функции FFI"); return 0;
        }
        for (uint32_t j=0;j<f->argc;++j){
            if (!valid_type(f->args[j],0)){
                set_error(s,"Недопустимый тип аргумента FFI"); return 0;
            }
        }
        for (uint32_t j=0;j<i;++j) if (!strcmp(m->functions[j].name,f->name)){
            set_error(s,"Повторяющееся имя функции FFI '%s'",f->name); return 0;
        }
    }
    return 1;
}

typedef struct {
    uint32_t type;
    size_t width;
    size_t alignment;
} KirFfiFieldProperties;

/* Keep width/alignment together. Separate identical switch tables can be
   merged by GCC and appear as duplicate globals to Clang's ASan ODR tracker. */
static const KirFfiFieldProperties g_ffi_field_properties[] = {
    {KIR_FFI_FIELD_U8, sizeof(uint8_t), _Alignof(uint8_t)},
    {KIR_FFI_FIELD_I32, sizeof(int32_t), _Alignof(int32_t)},
    {KIR_FFI_FIELD_I64, sizeof(int64_t), _Alignof(int64_t)},
    {KIR_FFI_FIELD_F64, sizeof(double), _Alignof(double)}
};

static const KirFfiFieldProperties* field_properties(uint32_t type){
    for (size_t i=0;i<sizeof(g_ffi_field_properties)/sizeof(g_ffi_field_properties[0]);++i)
        if (g_ffi_field_properties[i].type==type) return &g_ffi_field_properties[i];
    return NULL;
}

static size_t field_width(uint32_t type){
    const KirFfiFieldProperties* properties=field_properties(type);
    return properties ? properties->width : 0;
}

static size_t field_align(uint32_t type){
    const KirFfiFieldProperties* properties=field_properties(type);
    return properties ? properties->alignment : 0;
}
static int valid_schema(const KirFfiRecordSchemaV2* r){
    if (!r || !r->fields || !r->field_count || r->field_count>256 ||
        !r->alignment || r->alignment>4096 ||
        (r->alignment&(r->alignment-1)) || !r->size ||
        (r->size&(r->alignment-1))) return 0;
    size_t at=0, max_align=1;
    for (uint32_t i=0;i<r->field_count;++i){
        const KirFfiFieldV2* f=&r->fields[i];
        size_t width=field_width(f->type), align=field_align(f->type);
        if (!valid_name(f->name) || !width || !align ||
            f->offset%align || f->offset<at || f->offset>r->size ||
            width>r->size-f->offset) return 0;
        for (uint32_t j=0;j<i;++j)
            if (!strcmp(f->name,r->fields[j].name)) return 0;
        if (align>max_align) max_align=align;
        at=f->offset+width;
    }
    /* Standard C-like scalar packing: reject noncanonical padding and
       unrelated layouts that merely fit in a sufficiently large buffer. */
    at=0;
    for (uint32_t i=0;i<r->field_count;++i){
        const KirFfiFieldV2* f=&r->fields[i];
        const size_t a=field_align(f->type);
        if (at>SIZE_MAX-(a-1)) return 0;
        at=(at+a-1)&~(a-1);
        if (at!=f->offset) return 0;
        at+=field_width(f->type);
    }
    if (r->alignment!=max_align || at>SIZE_MAX-(max_align-1)) return 0;
    return r->size==((at+max_align-1)&~(max_align-1));
}
static int valid_param_v2(const KirFfiParamV2* p, int returns){
    if (!p) return 0;
    switch (p->type){
        case KIR_FFI_V2_VOID: return returns && !p->record && !p->resource_type;
        case KIR_FFI_V2_I64: case KIR_FFI_V2_F64:
        case KIR_FFI_V2_BOOL: case KIR_FFI_V2_TEXT:
            return !p->record && !p->resource_type;
        case KIR_FFI_V2_RECORD:
            return !p->resource_type && valid_schema(p->record);
        case KIR_FFI_V2_RESOURCE:
            return !p->record && valid_name(p->resource_type);
        default: return 0;
    }
}
static int validate_module_v2(KirFfiState* s,const KirFfiModuleV2* m){
    if (!m || m->abi_version!=KIR_FFI_ABI_V2 ||
        m->struct_size!=sizeof(*m)){
        set_error(s,"Несовместимая версия или размер FFI ABI V2");return 0;
    }
    if (!valid_name(m->module_name) || m->function_count==0 ||
        m->function_count>1024 || !m->functions){
        set_error(s,"Некорректный модуль FFI V2");return 0;
    }
    for (size_t i=0;i<s->count;++i){
        if (!strcmp(loaded_name(&s->loaded[i]),m->module_name)){
            set_error(s,"Модуль FFI '%s' уже загружен",m->module_name);return 0;
        }
    }
    for (uint32_t i=0;i<m->function_count;++i){
        const KirFfiFunctionV2* f=&m->functions[i];
        if (!valid_name(f->name) || !f->invoke ||
            f->argc>KIR_FFI_MAX_ARGS || (f->argc && !f->args) ||
            !valid_param_v2(&f->returns,1)){
            set_error(s,"Некорректная сигнатура FFI V2");return 0;
        }
        for (uint32_t j=0;j<f->argc;++j){
            if (!valid_param_v2(&f->args[j],0)){
                set_error(s,"Некорректный аргумент FFI V2");return 0;
            }
        }
        for (uint32_t j=0;j<i;++j)
            if (!strcmp(f->name,m->functions[j].name)){
                set_error(s,"Повтор имени функции FFI V2");return 0;
            }
    }
    return 1;
}

int kir_ffi_state_load(KirFfiState* s, const char* path){
    if (!s || !path || !*path) return 0;
    s->error[0]=0;
    KirFfiLibrary lib=open_library(path);
    if (!lib){
        set_error(s,"Не удалось загрузить библиотеку '%s'",path); return 0;
    }
    /* Prefer V2 when both are exported; an invalid V2 must not silently
       downgrade to V1. Existing V1-only plugins retain their original ABI. */
    KirFfiModuleEntryV2 entry2=module_entry_v2(lib);
    const KirFfiModuleV2* m2=NULL;
    const KirFfiModuleV1* m=NULL;
    if (entry2){
        m2=entry2();
        if (!validate_module_v2(s,m2)){
            close_library(lib);return 0;
        }
    } else {
        KirFfiModuleEntryV1 entry=module_entry(lib);
        if (!entry){
            set_error(s,"Библиотека не экспортирует kirton_ffi_module_v1/v2");
            close_library(lib);return 0;
        }
        m=entry();
        if (!validate_module(s,m)){
            close_library(lib);return 0;
        }
    }
    if (s->count==s->capacity){
        size_t n=s->capacity ? s->capacity*2 : 4;
        if(n>SIZE_MAX/sizeof(*s->loaded)){
            set_error(s,"Недостаточно памяти для FFI");close_library(lib);return 0;
        }
        LoadedModule* grown=(LoadedModule*)kir_oom_realloc(s->loaded,n*sizeof(*grown));
        if (!grown){set_error(s,"Недостаточно памяти для FFI"); close_library(lib); return 0;}
        s->loaded=grown; s->capacity=n;
    }
    s->loaded[s->count++]=(LoadedModule){lib,m,m2};
    return 1;
}

static const KirFfiFunctionV1* find_function(const KirFfiState* s,
                                              const char* module,const char* name){
    for (size_t i=0;i<s->count;++i){
        const KirFfiModuleV1* m=s->loaded[i].descriptor;
        if (!m || strcmp(m->module_name,module)) continue;
        for (uint32_t j=0;j<m->function_count;++j)
            if (!strcmp(m->functions[j].name,name)) return &m->functions[j];
    }
    return NULL;
}

static int kir_ffi_state_call_v2(KirFfiState* s, KirValueHeap* heap,
                const KirFfiFunctionV2* f, const char* module,
                int argc, const Value* args, Value* out);

int kir_ffi_state_call(KirFfiState* s, KirValueHeap* heap,
                       const char* module, const char* name,
                       int argc, const Value* args, Value* out){
    if (!s || !module || !name || !out || argc<0 ||
        (argc && !args)) return 0;
    *out=v_nil();
    s->error[0]=0;
    /* V2 dispatch is checked first; V1 remains binary/source compatible. */
    for (size_t i=0;i<s->count;++i){
        const KirFfiModuleV2* m=s->loaded[i].descriptor_v2;
        if (!m || strcmp(m->module_name,module)) continue;
        for (uint32_t j=0;j<m->function_count;++j){
            if (!strcmp(m->functions[j].name,name))
                return kir_ffi_state_call_v2(s,heap,&m->functions[j],module,
                                              argc,args,out);
        }
    }
    const KirFfiFunctionV1* f=find_function(s,module,name);
    if (!f){set_error(s,"Неизвестная функция FFI '%s.%s'",module,name); return 0;}
    if ((uint32_t)argc!=f->argc){
        set_error(s,"'%s.%s' ожидает %u аргументов, получено %d",
                  module,name,f->argc,argc); return 0;
    }
    KirFfiValue packed[KIR_FFI_MAX_ARGS];
    memset(packed,0,sizeof(packed));
    for (int i=0;i<argc;++i){
        uint32_t type=f->args[i];
        packed[i].type=type;
        if (type==KIR_FFI_I64){
            if (!v_num_fits_i64(args[i],&packed[i].data.i64)){
                set_error(s,"Аргумент %d: ожидается точное целое64",i+1); return 0;
            }
        } else if (type==KIR_FFI_F64){
            if (args[i].kind!=VAL_NUM){
                set_error(s,"Аргумент %d: ожидается число64",i+1); return 0;
            }
            packed[i].data.f64=v_num_to_double(args[i]);
        } else if (type==KIR_FFI_BOOL){
            if (args[i].kind!=VAL_BOOL){
                set_error(s,"Аргумент %d: ожидается логическое значение",i+1); return 0;
            }
            packed[i].data.boolean=args[i].as.boolean?1:0;
        } else if (type==KIR_FFI_TEXT){
            if (args[i].kind!=VAL_STR){
                set_error(s,"Аргумент %d: ожидается строка",i+1); return 0;
            }
            packed[i].data.text=(KirFfiText){args[i].as.str.s,args[i].as.str.n};
        }
    }
    KirFfiValue result;
    memset(&result,0,sizeof(result));
    result.type=f->returns;
    int success=f->invoke(f->argc,packed,&result);
    if (!success){set_error(s,"Нативная функция '%s.%s' сообщила об ошибке",module,name); return 0;}
    if (result.type!=f->returns){
        set_error(s,"Неверный тип результата FFI '%s.%s'",module,name); return 0;
    }
    switch (f->returns){
        case KIR_FFI_VOID: *out=v_nil(); return 1;
        case KIR_FFI_I64: *out=v_int(result.data.i64); return 1;
        case KIR_FFI_F64: *out=v_num(result.data.f64); return 1;
        case KIR_FFI_BOOL:
            if (result.data.boolean!=0 && result.data.boolean!=1){
                set_error(s,"Результат FFI: логическое значение должно быть 0 или 1"); return 0;
            }
            *out=v_bool(result.data.boolean!=0); return 1;
        case KIR_FFI_TEXT:
            if ((!result.data.text.bytes && result.data.text.length) ||
                result.data.text.length>INT_MAX){
                set_error(s,"Результат FFI: некорректная строка"); return 0;
            }
            *out=v_str_copy(result.data.text.bytes?result.data.text.bytes:"",
                            (int)result.data.text.length);
            return 1;
        default: set_error(s,"Неизвестный тип результата FFI"); return 0;
    }
}

/* ============================ ABI V2 ============================ */
typedef struct {
    KirValueHeap* heap;
    int argc;
    const Value* args;
    const KirFfiFunctionV2* function;
    int active;
} KirFfiCallScopeV2;

static int visit_resource_v2(const KirFfiHostV2* host, uint64_t token,
                             const char* type, KirFfiResourceVisitorV2 visitor,
                             void* user_data){
    if (!host || host->struct_size!=sizeof(*host) || !host->context ||
        !visitor || !type || token==0) return 0;
    KirFfiCallScopeV2* scope=(KirFfiCallScopeV2*)host->context;
    if (!scope->active || token>(uint64_t)scope->argc) return 0;
    size_t index=(size_t)(token-1);
    const KirFfiParamV2* sig=&scope->function->args[index];
    if (sig->type!=KIR_FFI_V2_RESOURCE ||
        strcmp(type,sig->resource_type)!=0) return 0;
    return v_native_resource_visit(scope->heap,scope->args[index],
                                   sig->resource_type,visitor,user_data);
}

static int record_schema_matches(Value record,
                                  const KirFfiRecordSchemaV2* schema){
    size_t size=0,alignment=0,count=0;
    if (!v_record_layout_details(record,&size,&alignment,&count) ||
        size!=schema->size || alignment!=schema->alignment ||
        count!=schema->field_count) return 0;
    for (size_t i=0;i<count;++i){
        const char* name=NULL;
        size_t len=0,offset=0;
        KirElementType type=0;
        const KirFfiFieldV2* expected=&schema->fields[i];
        if (!v_record_layout_field_at(record,i,&name,&len,&offset,&type) ||
            offset!=expected->offset || (uint32_t)type!=expected->type ||
            len!=strlen(expected->name) || memcmp(name,expected->name,len)!=0)
            return 0;
    }
    return 1;
}

static int valid_record_bytes(const unsigned char* bytes,
                              const KirFfiRecordSchemaV2* schema){
    if (!bytes) return 0;
    for (uint32_t i=0;i<schema->field_count;++i){
        const KirFfiFieldV2* field=&schema->fields[i];
        if (field->type==KIR_FFI_FIELD_F64){
            double val=0;
            memcpy(&val,bytes+field->offset,sizeof(val));
            if (!isfinite(val)) return 0;
        }
    }
    return 1;
}

static int make_record_result(KirFfiState* s, KirValueHeap* heap,
                              const KirFfiRecordSchemaV2* schema,
                              KirFfiRecordV2 source, Value* out){
    if (source.length!=schema->size ||
        !valid_record_bytes(source.bytes,schema)){
        set_error(s,"FFI V2: результат записи имеет неверный размер или число");
        return 0;
    }
    /* The layout is canonical and was checked at plugin load. Native scalar
       layout is platform-dependent. GC owns layout, arena, and record. */
    KirRecordFieldDef fields[256];
    for (uint32_t i=0;i<schema->field_count;++i){
        const KirFfiFieldV2* f=&schema->fields[i];
        fields[i]=(KirRecordFieldDef){f->name,strlen(f->name),
                                      (KirElementType)f->type};
    }
    Value layout=v_record_layout_new(fields,schema->field_count);
    if (layout.kind!=VAL_RECORD_LAYOUT){
        set_error(s,"FFI V2: не удалось создать схему записи");return 0;
    }
    size_t alignment=schema->alignment>64?schema->alignment:64;
    Value arena=v_arena_new_aligned(schema->size,alignment);
    if (arena.kind!=VAL_ARENA){
        set_error(s,"FFI V2: лимит памяти арены при создании записи");return 0;
    }
    Value record=v_record_new(arena,layout);
    if (record.kind!=VAL_RECORD){
        set_error(s,"FFI V2: не удалось создать запись");return 0;
    }
    const void* destination=NULL;
    size_t destination_size=0;
    if (!v_memory_borrow_begin(heap,record,true,&destination,&destination_size) ||
        destination_size!=schema->size){
        set_error(s,"FFI V2: не удалось получить буфер записи");return 0;
    }
    /* Plugin memory is copied immediately; no pointer is kept. */
    memcpy((void*)destination,source.bytes,schema->size);
    (void)v_memory_borrow_end(heap,record,true);
    *out=record;
    return 1;
}

static int kir_ffi_state_call_v2(KirFfiState* s, KirValueHeap* heap,
                const KirFfiFunctionV2* f, const char* module,
                int argc, const Value* args, Value* out){
    if (!heap) return 0;
    if ((uint32_t)argc!=f->argc){
        set_error(s,"FFI V2 '%s.%s': ожидается %u аргументов, получено %d",
                  module,f->name,f->argc,argc);
        return 0;
    }
    KirFfiValueV2 packed[KIR_FFI_MAX_ARGS];
    unsigned char* copies[KIR_FFI_MAX_ARGS]={0};
    memset(packed,0,sizeof(packed));
    int ok=0;
    for (int i=0;i<argc;++i){
        const KirFfiParamV2* param=&f->args[i];
        packed[i].type=param->type;
        switch (param->type){
            case KIR_FFI_V2_I64:
                if (!v_num_fits_i64(args[i],&packed[i].data.i64)) goto wrong_type;
                break;
            case KIR_FFI_V2_F64:
                if (args[i].kind!=VAL_NUM) goto wrong_type;
                packed[i].data.f64=v_num_to_double(args[i]);
                break;
            case KIR_FFI_V2_BOOL:
                if (args[i].kind!=VAL_BOOL) goto wrong_type;
                packed[i].data.boolean=args[i].as.boolean?1:0;
                break;
            case KIR_FFI_V2_TEXT:
                if (args[i].kind!=VAL_STR) goto wrong_type;
                packed[i].data.text=(KirFfiText){args[i].as.str.s,args[i].as.str.n};
                break;
            case KIR_FFI_V2_RECORD: {
                if (args[i].kind!=VAL_RECORD ||
                    !record_schema_matches(args[i],param->record)) goto wrong_type;
                const void* borrowed_data=NULL;
                size_t len=0;
                if (!v_memory_borrow_begin(heap,args[i],false,&borrowed_data,&len)) goto wrong_type;
                if (len!=param->record->size){
                    v_memory_borrow_end(heap,args[i],false);goto wrong_type;
                }
                copies[i]=(unsigned char*)kir_oom_malloc(len);
                if (!copies[i]){
                    v_memory_borrow_end(heap,args[i],false);
                    set_error(s,"FFI V2: недостаточно памяти для копии записи");
                    goto finish;
                }
                memcpy(copies[i],borrowed_data,len);
                (void)v_memory_borrow_end(heap,args[i],false);
                packed[i].data.record=(KirFfiRecordV2){copies[i],len};
                break;
            }
            case KIR_FFI_V2_RESOURCE:
                if (args[i].kind!=VAL_NATIVE_RESOURCE ||
                    !v_native_resource_is_open(heap,args[i],param->resource_type))
                    goto wrong_type;
                /* 1-based token, scoped to the current invocation. */
                packed[i].data.resource_token=(uint64_t)i+1;
                break;
            default: goto wrong_type;
        }
        continue;
wrong_type:
        set_error(s,"FFI V2 '%s.%s': неверный тип или схема аргумента %d",
                  module,f->name,i+1);
        goto finish;
    }
    KirFfiCallScopeV2 scope={heap,argc,args,f,1};
    KirFfiHostV2 host={sizeof(KirFfiHostV2),&scope,visit_resource_v2};
    KirFfiValueV2 result={0};
    result.type=f->returns.type;
    int invoked=f->invoke(&host,f->argc,packed,&result);
    scope.active=0;
    if (!invoked){
        set_error(s,"FFI V2 '%s.%s': нативная функция сообщила об ошибке",
                  module,f->name);
        goto finish;
    }
    if (result.type!=f->returns.type){
        set_error(s,"FFI V2 '%s.%s': неверный тип результата",module,f->name);
        goto finish;
    }
    switch (result.type){
        case KIR_FFI_V2_VOID: *out=v_nil(); ok=1; break;
        case KIR_FFI_V2_I64: *out=v_int(result.data.i64); ok=1; break;
        case KIR_FFI_V2_F64:
            if (!isfinite(result.data.f64)){
                set_error(s,"FFI V2: недопустимое число64");break;
            }
            *out=v_num(result.data.f64);ok=1;break;
        case KIR_FFI_V2_BOOL:
            if (result.data.boolean!=0 && result.data.boolean!=1){
                set_error(s,"FFI V2: логическое значение должно быть 0 или 1");break;
            }
            *out=v_bool(result.data.boolean!=0);ok=1;break;
        case KIR_FFI_V2_TEXT:
            if ((!result.data.text.bytes && result.data.text.length) ||
                result.data.text.length>INT_MAX){
                set_error(s,"FFI V2: неверная строка результата");break;
            }
            *out=v_str_copy(result.data.text.bytes?result.data.text.bytes:"",
                            (int)result.data.text.length);
            ok=1;break;
        case KIR_FFI_V2_RECORD:
            ok=make_record_result(s,heap,f->returns.record,result.data.record,out);
            break;
        case KIR_FFI_V2_RESOURCE: {
            uint64_t token=result.data.resource_token;
            if (!token || token>(uint64_t)argc ||
                f->args[token-1].type!=KIR_FFI_V2_RESOURCE ||
                strcmp(f->args[token-1].resource_type,
                       f->returns.resource_type)!=0 ||
                !v_native_resource_is_open(heap,args[token-1],
                                           f->returns.resource_type)){
                set_error(s,"FFI V2: нельзя возвращать чужой дескриптор");break;
            }
            *out=args[token-1];ok=1;break;
        }
        default: set_error(s,"FFI V2: неизвестный тип результата");break;
    }
finish:
    for (int i=0;i<argc;++i) free(copies[i]);
    return ok;
}
