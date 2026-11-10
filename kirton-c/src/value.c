/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/value.c
#include "kirton/value.h"
#include "kirton/common.h"   /* kir_malloc, kir_xreallocn и др. */
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>  // HANDLE, DWORD, CP_UTF8, WriteConsoleW, ...
  #include <wchar.h>    // WCHAR
#endif

/* ========================= Strings helpers ========================= */

static char* str_dup_n(const char* s, size_t n){
    /* Используем kir_malloc: OOM => аккуратный exit(1) */
    char* out = (char*)kir_malloc(n + 1);
    if (n) memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* ========================= Constructors ========================= */

#ifdef _MSC_VER
static __declspec(thread) KirBigIntHeap* current_bigint_heap;
static __declspec(thread) KirValueHeap* current_value_heap;
#else
static _Thread_local KirBigIntHeap* current_bigint_heap;
static _Thread_local KirValueHeap* current_value_heap;
#endif

KirBigIntHeap* v_bigint_heap_set_current(KirBigIntHeap* heap){
    KirBigIntHeap* previous = current_bigint_heap;
    current_bigint_heap = heap;
    return previous;
}

/* The work list avoids recursion for deep containers; identities are stable
   heap objects, so cyclic and shared collections are visited only once. */
static size_t mark_pointer_hash(const void* pointer){
    uintptr_t value=(uintptr_t)pointer;
    value ^= value >> 4;
    value *= (uintptr_t)0x9e3779b1u;
    value ^= value >> 16;
    return (size_t)value;
}

typedef struct ValueHeapEntry {
    void* object;
    ValKind kind;
    bool marked;
    struct ValueHeapEntry* next;
    struct ValueHeapEntry* bucket_next;
} ValueHeapEntry;

struct KirValueHeap {
    ValueHeapEntry* objects;
    ValueHeapEntry** buckets;
    size_t bucket_count;
    size_t count;
    size_t arena_bytes;
    size_t arena_limit;
    bool finalizing; /* block VM reentry while native destructors run */
    struct KirValueHeap* all_next;
};

static KirValueHeap* all_value_heaps;

/* Non-moving arena storage. A buffer is a GC-owned checked view, not an
   owning pointer. Generation invalidation prevents use-after-reset/close. */
struct KirArena {
    KirValueHeap* owner;
    unsigned char* data;
    void* allocation; /* original pointer returned by allocator */
    size_t base_alignment;
    size_t capacity, used;
    uint64_t generation;
    bool closed;
    size_t borrow_readers;
    bool borrow_writer;
};

struct KirBuffer {
    KirArena* arena;
    size_t offset, length;
    uint64_t generation;
};

struct KirTypedArray {
    KirArena* arena;
    size_t offset, count;
    uint64_t generation;
    KirElementType type;
};

typedef struct {
    char* name;
    size_t name_length;
    size_t offset;
    KirElementType type;
} KirRecordField;

struct KirRecordLayout {
    KirValueHeap* owner;
    KirRecordField* fields;
    size_t count;
    size_t size;
    size_t alignment;
};

struct KirRecord {
    KirArena* arena;
    KirRecordLayout* layout;
    size_t offset;
    uint64_t generation;
};

/* Opaque native handles are tracked like other GC objects. No pointers to
   VM-managed Values are stored here: native clients must use vm_value_pin(). */
struct KirNativeResource {
    KirValueHeap* owner;
    void* handle;
    char* type_tag;
    KirNativeResourceDestroy destructor;
    void* user_data;
    size_t active_visits;
    bool closed;
    int64_t identity; /* prevent stale aliases after allocator reuses address */
};

struct KirFunction {
    KirValueHeap* owner;
    AstFunc* ast;
    void* closure;
    void (*trace_closure)(void* context, void* closure);
    void (*release_closure)(void* context, void* closure);
    void* trace_context;
    int64_t identity;
    bool tracing;
};

struct KirUserStruct {
    KirValueHeap* owner;
    AstUserType* type;
    Value fields;
    int64_t identity;
};

struct KirEnumValue {
    KirValueHeap* owner;
    AstUserType* type;
    char* variant;
    size_t variant_len;
    int64_t identity;
};

/* Process-wide unique identities. Resource APIs are single-threaded. */
static int64_t next_native_resource_identity;
static int64_t next_function_identity;
static int64_t next_user_type_value_identity;



KirValueHeap* v_value_heap_new(void){
    KirValueHeap* heap=(KirValueHeap*)kir_malloc(sizeof(*heap));
    memset(heap,0,sizeof(*heap));
    heap->arena_limit=SIZE_MAX;
    heap->all_next=all_value_heaps;
    all_value_heaps=heap;
    return heap;
}

KirValueHeap* v_value_heap_set_current(KirValueHeap* heap){
    KirValueHeap* previous=current_value_heap;
    current_value_heap=heap;
    return previous;
}

static void value_heap_rehash(KirValueHeap* heap, size_t count){
    ValueHeapEntry** buckets=(ValueHeapEntry**)kir_xmallocn(count,sizeof(*buckets));
    memset(buckets,0,count*sizeof(*buckets));
    for (ValueHeapEntry* e=heap->objects;e;e=e->next){
        size_t at=mark_pointer_hash(e->object)&(count-1);
        e->bucket_next=buckets[at];
        buckets[at]=e;
    }
    kir_free(heap->buckets);
    heap->buckets=buckets;
    heap->bucket_count=count;
}

static void value_heap_track(KirValueHeap* heap, void* object, ValKind kind){
    if (!heap || !object) return;
    if (!heap->bucket_count || heap->count>=heap->bucket_count/2)
        value_heap_rehash(heap,heap->bucket_count ? heap->bucket_count*2 : 64);
    ValueHeapEntry* entry=(ValueHeapEntry*)kir_malloc(sizeof(*entry));
    entry->object=object;
    entry->kind=kind;
    entry->marked=false;
    entry->next=heap->objects;
    heap->objects=entry;
    size_t at=mark_pointer_hash(object)&(heap->bucket_count-1);
    entry->bucket_next=heap->buckets[at];
    heap->buckets[at]=entry;
    ++heap->count;
}

static int value_heap_mark_object(KirValueHeap* heap, const void* object, ValKind kind){
    if (!heap || !object || !heap->bucket_count) return 0;
    size_t at=mark_pointer_hash(object)&(heap->bucket_count-1);
    for (ValueHeapEntry* e=heap->buckets[at];e;e=e->bucket_next){
        if (e->object==object && e->kind==kind){
            if (e->marked) return 0;
            e->marked=true;
            return 1;
        }
    }
    return 0;
}

/* Move the handle out before invoking the destructor. This prevents a second
   callback if an alias is explicitly closed before the next collection. */
static void native_resource_finalize(KirNativeResource* resource){
    if (!resource || resource->closed) return;
    resource->closed=true;
    void* handle=resource->handle;
    resource->handle=NULL;
    KirValueHeap* heap=resource->owner;
    bool was_finalizing=heap ? heap->finalizing : false;
    if (heap) heap->finalizing=true;
    if (resource->destructor) resource->destructor(handle,resource->user_data);
    if (heap) heap->finalizing=was_finalizing;
}

static void value_heap_free_object(ValueHeapEntry* entry){
    if (entry->kind==VAL_LIST){
        VList* list=(VList*)entry->object;
        kir_free(list->data);
        kir_free(list);
    } else if (entry->kind==VAL_DICT){
        VDict* dict=(VDict*)entry->object;
        for (size_t i=0;i<dict->len;++i) kir_free(dict->keys[i]);
        kir_free(dict->keys);
        kir_free(dict->kn);
        kir_free(dict->vals);
        kir_free(dict);
    } else if (entry->kind==VAL_STR){
        kir_free(entry->object);
    } else if (entry->kind==VAL_ARENA){
        KirArena* arena=(KirArena*)entry->object;
        if (arena->data && arena->owner) arena->owner->arena_bytes-=arena->capacity;
        kir_free(arena->allocation);
        kir_free(arena);
    } else if (entry->kind==VAL_NATIVE_RESOURCE){
        KirNativeResource* resource=(KirNativeResource*)entry->object;
        native_resource_finalize(resource);
        kir_free(resource->type_tag);
        kir_free(resource);
    } else if (entry->kind==VAL_FUNCTION){
        KirFunction* function=(KirFunction*)entry->object;
        if (function->release_closure)
            function->release_closure(function->trace_context,function->closure);
        kir_free(function);
    } else if (entry->kind==VAL_USER_STRUCT){
        kir_free(entry->object);
    } else if (entry->kind==VAL_ENUM_VALUE){
        KirEnumValue* value=(KirEnumValue*)entry->object;
        kir_free(value->variant);
        kir_free(value);
    } else if (entry->kind==VAL_RECORD_LAYOUT){
        KirRecordLayout* layout=(KirRecordLayout*)entry->object;
        for (size_t i=0;i<layout->count;++i) kir_free(layout->fields[i].name);
        kir_free(layout->fields);
        kir_free(layout);
    } else if (entry->kind==VAL_BUFFER || entry->kind==VAL_TYPED_ARRAY ||
               entry->kind==VAL_RECORD){
        kir_free(entry->object);
    }
}

size_t v_value_heap_sweep(KirValueHeap* heap){
    if (!heap || heap->finalizing) return 0;
    size_t freed=0;
    ValueHeapEntry** link=&heap->objects;
    while (*link){
        ValueHeapEntry* entry=*link;
        if (entry->marked){ entry->marked=false; link=&entry->next; continue; }
        *link=entry->next;
        size_t at=mark_pointer_hash(entry->object)&(heap->bucket_count-1);
        ValueHeapEntry** bucket=&heap->buckets[at];
        while (*bucket && *bucket!=entry) bucket=&(*bucket)->bucket_next;
        if (*bucket) *bucket=entry->bucket_next;
        value_heap_free_object(entry);
        kir_free(entry);
        --heap->count;
        ++freed;
    }
    return freed;
}

size_t v_value_heap_count(const KirValueHeap* heap){ return heap ? heap->count : 0; }
size_t v_value_heap_arena_bytes(const KirValueHeap* heap){ return heap ? heap->arena_bytes : 0; }
int v_value_heap_set_arena_limit(KirValueHeap* heap, size_t maximum_bytes){
    if (!heap || maximum_bytes<heap->arena_bytes) return 0;
    heap->arena_limit=maximum_bytes;
    return 1;
}

void v_value_heap_release_all(KirValueHeap* heap){
    if (!heap) return;
    ValueHeapEntry* entry=heap->objects;
    while (entry){
        ValueHeapEntry* next=entry->next;
        /* A relinquished raw Value must not leave an adopted native handle
           orphaned without its destructor. Other values retain their old
           release-all semantics and become unmanaged. */
        if (entry->kind==VAL_NATIVE_RESOURCE || entry->kind==VAL_FUNCTION ||
            entry->kind==VAL_USER_STRUCT || entry->kind==VAL_ENUM_VALUE)
            value_heap_free_object(entry);
        kir_free(entry);
        entry=next;
    }
    heap->objects=NULL;
    heap->count=0;
    heap->arena_bytes=0;
    if (heap->buckets) memset(heap->buckets,0,heap->bucket_count*sizeof(*heap->buckets));
}

void v_value_heap_destroy(KirValueHeap* heap){
    if (!heap || heap->finalizing) return;
    if (current_value_heap==heap) current_value_heap=NULL;
    heap->finalizing=true;
    ValueHeapEntry* entry=heap->objects;
    while (entry){
        ValueHeapEntry* next=entry->next;
        value_heap_free_object(entry);
        kir_free(entry);
        entry=next;
    }
    kir_free(heap->buckets);
    KirValueHeap** link=&all_value_heaps;
    while (*link && *link!=heap) link=&(*link)->all_next;
    if (*link) *link=heap->all_next;
    kir_free(heap);
}

static void mark_seen_grow(const void*** slots, size_t* capacity){
    size_t old_capacity=*capacity;
    size_t new_capacity=old_capacity ? old_capacity*2 : 64;
    const void** next=(const void**)kir_xmallocn(new_capacity,sizeof(*next));
    memset(next,0,new_capacity*sizeof(*next));
    for (size_t i=0;i<old_capacity;++i){
        const void* item=(*slots)[i];
        if (!item) continue;
        size_t at=mark_pointer_hash(item)&(new_capacity-1);
        while (next[at]) at=(at+1)&(new_capacity-1);
        next[at]=item;
    }
    kir_free((void*)*slots);
    *slots=next;
    *capacity=new_capacity;
}

static int mark_seen_insert(const void* object, const void*** slots,
                            size_t* capacity, size_t* count){
    if (!*capacity || *count >= *capacity/2) mark_seen_grow(slots,capacity);
    size_t at=mark_pointer_hash(object)&(*capacity-1);
    while ((*slots)[at]){
        if ((*slots)[at]==object) return 0;
        at=(at+1)&(*capacity-1);
    }
    (*slots)[at]=object;
    ++*count;
    return 1;
}

static void mark_work_push(Value** values, size_t* length, size_t* capacity, Value value){
    if (*length==*capacity){
        size_t next=*capacity ? *capacity*2 : 64;
        *values=(Value*)kir_xreallocn(*values,next,sizeof(**values));
        *capacity=next;
    }
    (*values)[(*length)++]=value;
}

static int value_heap_owns(const KirValueHeap* heap, const void* object, ValKind kind);

void v_value_heap_mark(KirValueHeap* heap, KirBigIntHeap* bigints, Value value){
    if (!heap && !bigints) return;
    Value* work=NULL;
    size_t length=0, work_capacity=0;
    const void** seen=NULL;
    size_t seen_count=0, seen_capacity=0;
    mark_work_push(&work,&length,&work_capacity,value);
    while (length){
        Value current=work[--length];
        if (current.kind==VAL_NUM && current.is_integer && current.integer_big){
            if (bigints) (void)kir_bigint_heap_mark(bigints,current.integer_big);
        } else if (current.kind==VAL_STR && current.as.str.s){
            value_heap_mark_object(heap,current.as.str.s,VAL_STR);
        } else if (current.kind==VAL_LIST && current.as.list){
            VList* list=current.as.list;
            value_heap_mark_object(heap,list,VAL_LIST);
            if (!mark_seen_insert(list,&seen,&seen_capacity,&seen_count)) continue;
            for (size_t i=0;i<list->len;++i)
                mark_work_push(&work,&length,&work_capacity,list->data[i]);
        } else if (current.kind==VAL_ARENA && current.as.arena){
            value_heap_mark_object(heap,current.as.arena,VAL_ARENA);
        } else if (current.kind==VAL_BUFFER && current.as.buffer){
            /* Never inspect an unowned (possibly stale) foreign pointer. */
            if (value_heap_mark_object(heap,current.as.buffer,VAL_BUFFER))
                value_heap_mark_object(heap,current.as.buffer->arena,VAL_ARENA);
        } else if (current.kind==VAL_TYPED_ARRAY && current.as.typed_array){
            if (value_heap_mark_object(heap,current.as.typed_array,VAL_TYPED_ARRAY))
                value_heap_mark_object(heap,current.as.typed_array->arena,VAL_ARENA);
        } else if (current.kind==VAL_NATIVE_RESOURCE && current.as.native_resource){
            if (value_heap_owns(heap,current.as.native_resource,VAL_NATIVE_RESOURCE) &&
                current.as.native_resource->identity==current.integer_small)
                value_heap_mark_object(heap,current.as.native_resource,VAL_NATIVE_RESOURCE);
        } else if (current.kind==VAL_FUNCTION && current.as.function){
            KirFunction* function=value_heap_owns(heap,current.as.function,VAL_FUNCTION)
                ? current.as.function : NULL;
            if (function && function->identity==current.integer_small &&
                value_heap_mark_object(heap,function,VAL_FUNCTION) &&
                mark_seen_insert(function,&seen,&seen_capacity,&seen_count) &&
                function->trace_closure && !function->tracing){
                    function->tracing=true;
                    function->trace_closure(function->trace_context,function->closure);
                    function->tracing=false;
            }
        } else if (current.kind==VAL_USER_STRUCT && current.as.user_struct){
            KirUserStruct* object=value_heap_owns(heap,current.as.user_struct,VAL_USER_STRUCT)
                ? current.as.user_struct : NULL;
            if (object && object->identity==current.integer_small &&
                value_heap_mark_object(heap,object,VAL_USER_STRUCT) &&
                mark_seen_insert(object,&seen,&seen_capacity,&seen_count))
                mark_work_push(&work,&length,&work_capacity,object->fields);
        } else if (current.kind==VAL_ENUM_VALUE && current.as.enum_value){
            KirEnumValue* object=value_heap_owns(heap,current.as.enum_value,VAL_ENUM_VALUE)
                ? current.as.enum_value : NULL;
            if (object && object->identity==current.integer_small)
                value_heap_mark_object(heap,object,VAL_ENUM_VALUE);
        } else if (current.kind==VAL_RECORD_LAYOUT && current.as.record_layout){
            value_heap_mark_object(heap,current.as.record_layout,VAL_RECORD_LAYOUT);
        } else if (current.kind==VAL_RECORD && current.as.record){
            /* First validate ownership, then dereference descriptor children. */
            if (value_heap_mark_object(heap,current.as.record,VAL_RECORD)){
                value_heap_mark_object(heap,current.as.record->arena,VAL_ARENA);
                value_heap_mark_object(heap,current.as.record->layout,VAL_RECORD_LAYOUT);
            }
        } else if (current.kind==VAL_DICT && current.as.dict){
            VDict* dict=current.as.dict;
            value_heap_mark_object(heap,dict,VAL_DICT);
            if (!mark_seen_insert(dict,&seen,&seen_capacity,&seen_count)) continue;
            for (size_t i=0;i<dict->len;++i)
                mark_work_push(&work,&length,&work_capacity,dict->vals[i]);
        }
    }
    kir_free(work);
    kir_free((void*)seen);
}

void v_mark_bigints(KirBigIntHeap* heap, Value value){
    v_value_heap_mark(NULL,heap,value);
}

Value v_num(double x){
    Value v;
    v.kind   = VAL_NUM;
    v.as.num = x;
    v.is_integer = false;
    v.integer_small = 0;
    v.integer_big = NULL;
    return v;
}

Value v_int(int64_t x){
    Value v = v_num((double)x);
    v.is_integer = true;
    v.integer_small = x;
    return v;
}

Value v_int_from_size(size_t x){
    if (x <= (size_t)INT64_MAX) return v_int((int64_t)x);
    char text[3*sizeof(size_t) + 1];
    int len = snprintf(text, sizeof(text), "%ju", (uintmax_t)x);
    if (len <= 0 || (size_t)len >= sizeof(text)) return v_int(0);
    return v_int_parse(text, (size_t)len, 10);
}

Value v_int_from_double(double x){
    if (!isfinite(x) || trunc(x) != x) return v_num(x);
    if (x >= -9223372036854775808.0 && x < 9223372036854775808.0)
        return v_int((int64_t)x);
    char text[400];
    int len = snprintf(text, sizeof(text), "%.0f", x);
    if (len <= 0 || (size_t)len >= sizeof(text)) return v_num(x);
    return v_int_parse(text, (size_t)len, 10);
}

Value v_int_parse(const char* text, size_t len, unsigned int base){
    KirBigInt* integer = kir_bigint_parse(text, len, base);
    if (!integer) return v_num(0.0);
    int64_t small;
    if (kir_bigint_fits_i64(integer, &small)){
        kir_bigint_free(integer);
        return v_int(small);
    }
    Value v = v_num(kir_bigint_to_double(integer));
    v.is_integer = true;
    v.integer_big = integer;
    if (current_bigint_heap) (void)kir_bigint_heap_manage(current_bigint_heap, integer);
    return v;
}

bool v_num_is_integer(Value v){
    return v.kind == VAL_NUM && v.is_integer;
}

double v_num_to_double(Value v){
    if (v.kind != VAL_NUM) return 0.0;
    if (!v.is_integer) return v.as.num;
    if (v.integer_big) return kir_bigint_to_double(v.integer_big);
    return (double)v.integer_small;
}

int v_num_fits_i64(Value v, int64_t* out){
    if (v.kind != VAL_NUM) return 0;
    if (v.is_integer){
        if (v.integer_big) return kir_bigint_fits_i64(v.integer_big, out);
        if (out) *out = v.integer_small;
        return 1;
    }
    if (!isfinite(v.as.num) || trunc(v.as.num) != v.as.num ||
        v.as.num < -9223372036854775808.0 || v.as.num >= 9223372036854775808.0) return 0;
    if (out) *out = (int64_t)v.as.num;
    return 1;
}

int v_num_to_int_trunc(Value v, int* out){
    if (v.kind != VAL_NUM || !out) return 0;
    if (v.is_integer){
        int64_t exact;
        if (!v_num_fits_i64(v, &exact) || exact < INT_MIN || exact > INT_MAX) return 0;
        *out = (int)exact;
        return 1;
    }
    if (!isfinite(v.as.num)) return 0;
    double truncated = trunc(v.as.num);
    if (truncated < (double)INT_MIN || truncated > (double)INT_MAX) return 0;
    *out = (int)truncated;
    return 1;
}

static KirBigInt* value_as_bigint(Value v, int* owned){
    if (v.integer_big){
        if (owned) *owned = 0;
        return v.integer_big;
    }
    if (owned) *owned = 1;
    return kir_bigint_from_i64(v.integer_small);
}

int v_num_compare(Value a, Value b, int* out_compare){
    if (a.kind != VAL_NUM || b.kind != VAL_NUM) return 0;
    if (a.is_integer && b.is_integer){
        if (!a.integer_big && !b.integer_big){
            if (out_compare) *out_compare = a.integer_small < b.integer_small ? -1 :
                                            a.integer_small > b.integer_small ? 1 : 0;
            return 1;
        }
        int own_a=0, own_b=0;
        KirBigInt* big_a=value_as_bigint(a,&own_a);
        KirBigInt* big_b=value_as_bigint(b,&own_b);
        int cmp=kir_bigint_compare(big_a,big_b);
        if (own_a) kir_bigint_free(big_a);
        if (own_b) kir_bigint_free(big_b);
        if (out_compare) *out_compare = cmp;
        return 1;
    }
    if (a.is_integer){
        int owned=0;
        KirBigInt* big=value_as_bigint(a,&owned);
        int ok=kir_bigint_compare_double(big,b.as.num,out_compare);
        if (owned) kir_bigint_free(big);
        return ok;
    }
    if (b.is_integer){
        int cmp;
        int owned=0;
        KirBigInt* big=value_as_bigint(b,&owned);
        int ok=kir_bigint_compare_double(big,a.as.num,&cmp);
        if (owned) kir_bigint_free(big);
        if (!ok) return 0;
        if (out_compare) *out_compare = -cmp;
        return 1;
    }
    if (isnan(a.as.num) || isnan(b.as.num)) return 0;
    if (out_compare) *out_compare = a.as.num < b.as.num ? -1 : a.as.num > b.as.num ? 1 : 0;
    return 1;
}

bool v_num_equal(Value a, Value b){
    int cmp = 0;
    return v_num_compare(a, b, &cmp) && cmp == 0;
}

static int add_overflows_i64(int64_t a, int64_t b){
    return (b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b);
}

static int sub_overflows_i64(int64_t a, int64_t b){
    return (b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b);
}

static int mul_overflows_i64(int64_t a, int64_t b){
    if (a == 0 || b == 0) return 0;
    if ((a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN)) return 1;
    if (a > 0){
        if (b > 0) return a > INT64_MAX / b;
        return b < INT64_MIN / a;
    }
    if (b > 0) return a < INT64_MIN / b;
    return a < INT64_MAX / b;
}

static Value value_from_bigint(KirBigInt* integer){
    int64_t small;
    if (kir_bigint_fits_i64(integer, &small)){
        kir_bigint_free(integer);
        return v_int(small);
    }
    Value v = v_num(kir_bigint_to_double(integer));
    v.is_integer = true;
    v.integer_big = integer;
    if (current_bigint_heap) (void)kir_bigint_heap_manage(current_bigint_heap, integer);
    return v;
}

Value v_num_add(Value a, Value b){
    if (a.is_integer && b.is_integer){
        if (!a.integer_big && !b.integer_big && !add_overflows_i64(a.integer_small,b.integer_small))
            return v_int(a.integer_small+b.integer_small);
        int own_a=0, own_b=0;
        KirBigInt* big_a=value_as_bigint(a,&own_a);
        KirBigInt* big_b=value_as_bigint(b,&own_b);
        KirBigInt* result=kir_bigint_add(big_a,big_b);
        if (own_a) kir_bigint_free(big_a);
        if (own_b) kir_bigint_free(big_b);
        return value_from_bigint(result);
    }
    return v_num(v_num_to_double(a)+v_num_to_double(b));
}

Value v_num_sub(Value a, Value b){
    if (a.is_integer && b.is_integer){
        if (!a.integer_big && !b.integer_big && !sub_overflows_i64(a.integer_small,b.integer_small))
            return v_int(a.integer_small-b.integer_small);
        int own_a=0, own_b=0;
        KirBigInt* big_a=value_as_bigint(a,&own_a);
        KirBigInt* big_b=value_as_bigint(b,&own_b);
        KirBigInt* result=kir_bigint_sub(big_a,big_b);
        if (own_a) kir_bigint_free(big_a);
        if (own_b) kir_bigint_free(big_b);
        return value_from_bigint(result);
    }
    return v_num(v_num_to_double(a)-v_num_to_double(b));
}

Value v_num_mul(Value a, Value b){
    if (a.is_integer && b.is_integer){
        if (!a.integer_big && !b.integer_big && !mul_overflows_i64(a.integer_small,b.integer_small))
            return v_int(a.integer_small*b.integer_small);
        int own_a=0, own_b=0;
        KirBigInt* big_a=value_as_bigint(a,&own_a);
        KirBigInt* big_b=value_as_bigint(b,&own_b);
        KirBigInt* result=kir_bigint_mul(big_a,big_b);
        if (own_a) kir_bigint_free(big_a);
        if (own_b) kir_bigint_free(big_b);
        return value_from_bigint(result);
    }
    return v_num(v_num_to_double(a)*v_num_to_double(b));
}

Value v_num_neg(Value v){
    if (v.is_integer){
        if (!v.integer_big && v.integer_small != INT64_MIN) return v_int(-v.integer_small);
        int owned=0;
        KirBigInt* big=value_as_bigint(v,&owned);
        KirBigInt* zero=kir_bigint_from_i64(0);
        KirBigInt* result=kir_bigint_sub(zero,big);
        if (owned) kir_bigint_free(big);
        kir_bigint_free(zero);
        return value_from_bigint(result);
    }
    return v_num(-v.as.num);
}

Value v_num_floor_div(Value a, Value b){
    if (a.kind!=VAL_NUM || b.kind!=VAL_NUM) return v_nil();
    int compare=1;
    if (v_num_compare(b,v_int(0),&compare) && compare==0) return v_nil();
    if (a.is_integer && b.is_integer){
        if (!a.integer_big && !b.integer_big &&
            !(a.integer_small==INT64_MIN && b.integer_small==-1)){
            int64_t quotient=a.integer_small/b.integer_small;
            int64_t remainder=a.integer_small%b.integer_small;
            if (remainder && ((remainder<0)!=(b.integer_small<0))) --quotient;
            return v_int(quotient);
        }
        int own_a=0, own_b=0;
        KirBigInt* big_a=value_as_bigint(a,&own_a);
        KirBigInt* big_b=value_as_bigint(b,&own_b);
        KirBigInt *quotient=NULL, *remainder=NULL;
        int ok=kir_bigint_divmod_floor(big_a,big_b,&quotient,&remainder);
        if (own_a) kir_bigint_free(big_a);
        if (own_b) kir_bigint_free(big_b);
        if (!ok) return v_nil();
        kir_bigint_free(remainder);
        return value_from_bigint(quotient);
    }
    return v_num(floor(v_num_to_double(a)/v_num_to_double(b)));
}

Value v_num_mod(Value a, Value b){
    if (a.kind!=VAL_NUM || b.kind!=VAL_NUM) return v_nil();
    int compare=1;
    if (v_num_compare(b,v_int(0),&compare) && compare==0) return v_nil();
    if (a.is_integer && b.is_integer){
        if (!a.integer_big && !b.integer_big){
            if (a.integer_small==INT64_MIN && b.integer_small==-1) return v_int(0);
            int64_t remainder=a.integer_small%b.integer_small;
            if (remainder && ((remainder<0)!=(b.integer_small<0))) remainder+=b.integer_small;
            return v_int(remainder);
        }
        int own_a=0, own_b=0;
        KirBigInt* big_a=value_as_bigint(a,&own_a);
        KirBigInt* big_b=value_as_bigint(b,&own_b);
        KirBigInt *quotient=NULL, *remainder=NULL;
        int ok=kir_bigint_divmod_floor(big_a,big_b,&quotient,&remainder);
        if (own_a) kir_bigint_free(big_a);
        if (own_b) kir_bigint_free(big_b);
        if (!ok) return v_nil();
        kir_bigint_free(quotient);
        return value_from_bigint(remainder);
    }
    double divisor=v_num_to_double(b);
    double remainder=fmod(v_num_to_double(a),divisor);
    if (remainder == 0.0) remainder=copysign(0.0,divisor);
    else if (signbit(remainder) != signbit(divisor)) remainder += divisor;
    return v_num(remainder);
}

Value v_num_bitwise(Value a, Value b, char op){
    if (!a.is_integer || !b.is_integer) return v_nil();
    if (!a.integer_big && !b.integer_big){
        uint64_t left=(uint64_t)a.integer_small, right=(uint64_t)b.integer_small, result;
        if (op=='&') result=left&right;
        else if (op=='|') result=left|right;
        else if (op=='^') result=left^right;
        else return v_nil();
        if (result<=(uint64_t)INT64_MAX) return v_int((int64_t)result);
        uint64_t magnitude=(~result)+1;
        if (magnitude==(UINT64_C(1)<<63)) return v_int(INT64_MIN);
        return v_int(-(int64_t)magnitude);
    }
    int own_a=0, own_b=0;
    KirBigInt* big_a=value_as_bigint(a,&own_a);
    KirBigInt* big_b=value_as_bigint(b,&own_b);
    KirBigInt* result=kir_bigint_bitwise(big_a,big_b,op);
    if (own_a) kir_bigint_free(big_a);
    if (own_b) kir_bigint_free(big_b);
    return result ? value_from_bigint(result) : v_nil();
}

Value v_num_bitnot(Value value){
    if (!value.is_integer) return v_nil();
    if (!value.integer_big){
        uint64_t result=~(uint64_t)value.integer_small;
        if (result<=(uint64_t)INT64_MAX) return v_int((int64_t)result);
        uint64_t magnitude=(~result)+1;
        if (magnitude==(UINT64_C(1)<<63)) return v_int(INT64_MIN);
        return v_int(-(int64_t)magnitude);
    }
    int owned=0;
    KirBigInt* big=value_as_bigint(value,&owned);
    KirBigInt* result=kir_bigint_bitnot(big);
    if (owned) kir_bigint_free(big);
    return result ? value_from_bigint(result) : v_nil();
}

Value v_num_shift(Value value, size_t bits, bool left){
    if (!value.is_integer) return v_nil();
    if (!value.integer_big){
        int64_t number=value.integer_small;
        if (!bits) return value;
        if (!left){
            if (bits>=63) return v_int(number<0 ? -1 : 0);
            int64_t divisor=(int64_t)(UINT64_C(1)<<bits);
            int64_t quotient=number/divisor;
            if (number%divisor<0) --quotient;
            return v_int(quotient);
        }
        if (bits<64){
            uint64_t magnitude=number<0 ? (uint64_t)(-(number+1))+1 : (uint64_t)number;
            uint64_t limit=number<0 ? (UINT64_C(1)<<63) : (uint64_t)INT64_MAX;
            if (magnitude<=(limit>>bits)){
                uint64_t shifted=magnitude<<bits;
                if (number>=0) return v_int((int64_t)shifted);
                if (shifted==(UINT64_C(1)<<63)) return v_int(INT64_MIN);
                return v_int(-(int64_t)shifted);
            }
        }
    }
    int owned=0;
    KirBigInt* big=value_as_bigint(value,&owned);
    KirBigInt* result=left ? kir_bigint_shift_left(big,bits)
                           : kir_bigint_shift_right(big,bits);
    if (owned) kir_bigint_free(big);
    return result ? value_from_bigint(result) : v_nil();
}

char* v_num_to_string(Value v, size_t* out_len){
    if (v.is_integer){
        if (v.integer_big) return kir_bigint_to_string(v.integer_big, out_len);
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%lld", (long long)v.integer_small);
        size_t n = len > 0 ? (size_t)len : 1;
        char* out = (char*)kir_malloc(n + 1);
        memcpy(out, buf, n + 1);
        if (out_len) *out_len = n;
        return out;
    }
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%.17g", v.as.num);
    size_t n = len > 0 ? (size_t)len : 1;
    if (isfinite(v.as.num) && !strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')){
        buf[n++] = '.';
        buf[n++] = '0';
        buf[n] = '\0';
    }
    char* out = (char*)kir_malloc(n + 1);
    memcpy(out, buf, n + 1);
    if (out_len) *out_len = n;
    return out;
}

Value v_bool(bool b){
    Value v;
    v.kind       = VAL_BOOL;
    v.as.boolean = b;
    v.is_integer = false;
    v.integer_small = 0;
    v.integer_big = NULL;
    return v;
}

Value v_str_copy(const char* s, int n){
    Value v;
    v.kind = VAL_STR;
    v.is_integer = false;
    v.integer_small = 0;
    v.integer_big = NULL;

    if (!s){
        v.as.str.s = NULL;
        v.as.str.n = 0;
        return v;
    }

    size_t len = (n < 0) ? strlen(s) : (size_t)n;
    v.as.str.s = str_dup_n(s, len);
    value_heap_track(current_value_heap,v.as.str.s,VAL_STR);
    v.as.str.n = (int)len;
    return v;
}

Value v_list_new(void){
    Value v;
    v.kind           = VAL_LIST;
    v.is_integer = false;
    v.integer_small = 0;
    v.integer_big = NULL;
    v.as.list = (VList*)kir_malloc(sizeof(VList));
    value_heap_track(current_value_heap,v.as.list,VAL_LIST);
    v.as.list->data   = NULL;
    v.as.list->len    = 0;
    v.as.list->cap    = 0;
    return v;
}

Value v_dict_new(void){
    Value v;
    v.kind          = VAL_DICT;
    v.is_integer = false;
    v.integer_small = 0;
    v.integer_big = NULL;
    v.as.dict = (VDict*)kir_malloc(sizeof(VDict));
    value_heap_track(current_value_heap,v.as.dict,VAL_DICT);
    v.as.dict->keys  = NULL;
    v.as.dict->kn    = NULL;
    v.as.dict->vals  = NULL;
    v.as.dict->len   = 0;
    v.as.dict->cap   = 0;
    return v;
}


/* ========================= Native resources (V2.5) ========================= */

int v_value_heap_is_finalizing(const KirValueHeap* heap){
    return heap && heap->finalizing;
}

Value v_function_new(AstFunc* ast, void* closure,
                     void (*trace_closure)(void*,void*),
                     void (*release_closure)(void*,void*),void* trace_context){
    if (!current_value_heap || !ast || next_function_identity==INT64_MAX) return v_nil();
    KirFunction* function=(KirFunction*)kir_malloc(sizeof(*function));
    *function=(KirFunction){current_value_heap,ast,closure,trace_closure,
                            release_closure,trace_context,
                            ++next_function_identity,false};
    value_heap_track(current_value_heap,function,VAL_FUNCTION);
    Value value=v_nil();
    value.kind=VAL_FUNCTION;
    value.as.function=function;
    value.integer_small=function->identity;
    return value;
}

int v_function_is_owned(const KirValueHeap* heap,Value value){
    return heap && value.kind==VAL_FUNCTION &&
           value_heap_owns(heap,value.as.function,VAL_FUNCTION) &&
           value.as.function->owner==heap &&
           value.as.function->identity==value.integer_small;
}

AstFunc* v_function_ast(Value value){
    return value.kind==VAL_FUNCTION && value.as.function &&
           value.as.function->identity==value.integer_small ?
           value.as.function->ast : NULL;
}

void* v_function_closure(Value value){
    return value.kind==VAL_FUNCTION && value.as.function &&
           value.as.function->identity==value.integer_small ?
           value.as.function->closure : NULL;
}

Value v_user_struct_new(AstUserType* type,Value fields){
    if (!current_value_heap || !type || fields.kind!=VAL_DICT ||
        next_user_type_value_identity==INT64_MAX) return v_nil();
    KirUserStruct* object=(KirUserStruct*)kir_malloc(sizeof(*object));
    *object=(KirUserStruct){current_value_heap,type,fields,
                            ++next_user_type_value_identity};
    value_heap_track(current_value_heap,object,VAL_USER_STRUCT);
    Value value=v_nil();
    value.kind=VAL_USER_STRUCT;
    value.as.user_struct=object;
    value.integer_small=object->identity;
    return value;
}

AstUserType* v_user_struct_type(Value value){
    return value.kind==VAL_USER_STRUCT && value.as.user_struct &&
           value.as.user_struct->identity==value.integer_small ?
           value.as.user_struct->type : NULL;
}
Value v_user_struct_fields(Value value){
    return value.kind==VAL_USER_STRUCT && value.as.user_struct &&
           value.as.user_struct->identity==value.integer_small ?
           value.as.user_struct->fields : v_nil();
}

Value v_enum_value_new(AstUserType* type,const char* variant,size_t len){
    if (!current_value_heap || !type || (!variant && len) ||
        next_user_type_value_identity==INT64_MAX) return v_nil();
    KirEnumValue* object=(KirEnumValue*)kir_malloc(sizeof(*object));
    object->owner=current_value_heap;
    object->type=type;
    object->variant=str_dup_n(variant?variant:"",len);
    object->variant_len=len;
    object->identity=++next_user_type_value_identity;
    value_heap_track(current_value_heap,object,VAL_ENUM_VALUE);
    Value value=v_nil();
    value.kind=VAL_ENUM_VALUE;
    value.as.enum_value=object;
    value.integer_small=object->identity;
    return value;
}
AstUserType* v_enum_value_type(Value value){
    return value.kind==VAL_ENUM_VALUE && value.as.enum_value &&
           value.as.enum_value->identity==value.integer_small ?
           value.as.enum_value->type : NULL;
}
int v_enum_value_variant(Value value,const char** name,size_t* len){
    if (value.kind!=VAL_ENUM_VALUE || !value.as.enum_value ||
        value.as.enum_value->identity!=value.integer_small) return 0;
    if (name) *name=value.as.enum_value->variant;
    if (len) *len=value.as.enum_value->variant_len;
    return 1;
}

Value v_native_resource_adopt(KirValueHeap* heap, void* handle,
                              const char* type_tag,
                              KirNativeResourceDestroy destroy, void* user_data){
    if (!heap || heap->finalizing || !handle || !type_tag || !*type_tag ||
        !destroy || next_native_resource_identity==INT64_MAX) return v_nil();
    KirNativeResource* resource=(KirNativeResource*)kir_malloc(sizeof(*resource));
    memset(resource,0,sizeof(*resource));
    resource->owner=heap;
    resource->handle=handle;
    resource->identity=++next_native_resource_identity;
    resource->type_tag=str_dup_n(type_tag,strlen(type_tag));
    resource->destructor=destroy;
    resource->user_data=user_data;
    value_heap_track(heap,resource,VAL_NATIVE_RESOURCE);
    Value value=v_nil();
    value.kind=VAL_NATIVE_RESOURCE;
    value.as.native_resource=resource;
    /* Numeric payload is unused for nonnumeric Values. */
    value.integer_small=resource->identity;
    return value;
}

Value v_native_resource_adopt_current(void* handle,const char* type_tag,
                                      KirNativeResourceDestroy destroy,
                                      void* user_data){
    return v_native_resource_adopt(current_value_heap,handle,type_tag,destroy,user_data);
}

/* Do not dereference a Value until the heap's tracking table has confirmed
   that the descriptor belongs to the expected VM. */
static KirNativeResource* checked_native_resource(KirValueHeap* heap, Value value){
    if (!heap || heap->finalizing || value.kind!=VAL_NATIVE_RESOURCE ||
        !value_heap_owns(heap,value.as.native_resource,VAL_NATIVE_RESOURCE))
        return NULL;
    if (value.as.native_resource->identity!=value.integer_small) return NULL;
    return value.as.native_resource;
}

int v_native_resource_is_open(KirValueHeap* heap, Value value, const char* type_tag){
    KirNativeResource* resource=checked_native_resource(heap,value);
    return resource && !resource->closed &&
           (!type_tag || strcmp(resource->type_tag,type_tag)==0);
}

int v_native_resource_close(KirValueHeap* heap, Value value){
    KirNativeResource* resource=checked_native_resource(heap,value);
    if (!resource || resource->closed || resource->active_visits) return 0;
    native_resource_finalize(resource);
    return 1;
}

int v_native_resource_visit(KirValueHeap* heap, Value value,
                            const char* type_tag,
                            KirNativeResourceVisit callback, void* user_data){
    KirNativeResource* resource=checked_native_resource(heap,value);
    if (!resource || resource->closed || !callback || !type_tag ||
        strcmp(resource->type_tag,type_tag)!=0) return 0;
    ++resource->active_visits;
    int result=callback(resource->handle,user_data);
    --resource->active_visits;
    return result;
}

int v_native_resource_lease_start(KirValueHeap* heap, Value value,
                                  const char* type_tag){
    KirNativeResource* resource=checked_native_resource(heap,value);
    if (!resource || resource->closed || !type_tag ||
        strcmp(resource->type_tag,type_tag)!=0 ||
        resource->active_visits==SIZE_MAX) return 0;
    ++resource->active_visits;
    return 1;
}
int v_native_resource_lease_end(KirValueHeap* heap, Value value){
    KirNativeResource* resource=checked_native_resource(heap,value);
    if (!resource || !resource->active_visits) return 0;
    --resource->active_visits;
    return 1;
}

int v_native_resource_close_current(Value value){
    return v_native_resource_close(current_value_heap,value);
}

int v_native_resource_is_open_current(Value value){
    return v_native_resource_is_open(current_value_heap,value,NULL);
}

/* ========================= Memory arenas (V2 preview) ========================= */

/* When executing a VM's native function, reject handles from another VM
   before dereferencing them. When no VM heap is active, the C caller must
   obey the pin/VM lifetime contract described in eval.h. */
static int value_heap_owns(const KirValueHeap* heap, const void* object, ValKind kind){
    if (!heap || !object || !heap->bucket_count) return 0;
    size_t at=mark_pointer_hash(object)&(heap->bucket_count-1);
    for (ValueHeapEntry* e=heap->buckets[at];e;e=e->bucket_next)
        if (e->object==object && e->kind==kind) return 1;
    return 0;
}

/* Walk nested collections when crossing a VM boundary. Accepting a list
   whose child is a foreign buffer/typed array would otherwise bypass direct
   handle validation. The visited set supports cyclic lists/dictionaries.
   Ordinary C-created lists still require caller-managed lifetime/pinning. */
int v_value_heap_accepts_memory_handle(const KirValueHeap* heap, Value value){
    if (!heap) return 0;
    Value* work=NULL;
    size_t length=0, capacity=0;
    const void** seen=NULL;
    size_t seen_count=0, seen_capacity=0;
    int accepted=1;
    mark_work_push(&work,&length,&capacity,value);
    while (length && accepted){
        Value current=work[--length];
        switch (current.kind){
            case VAL_ARENA:
                accepted=value_heap_owns(heap,current.as.arena,VAL_ARENA);
                break;
            case VAL_BUFFER:
                if (!value_heap_owns(heap,current.as.buffer,VAL_BUFFER)){
                    accepted=0;
                } else {
                    accepted=value_heap_owns(heap,current.as.buffer->arena,VAL_ARENA);
                }
                break;
            case VAL_TYPED_ARRAY:
                if (!value_heap_owns(heap,current.as.typed_array,VAL_TYPED_ARRAY)){
                    accepted=0;
                } else {
                    accepted=value_heap_owns(heap,current.as.typed_array->arena,VAL_ARENA);
                }
                break;
            case VAL_NATIVE_RESOURCE:
                accepted=value_heap_owns(heap,current.as.native_resource,VAL_NATIVE_RESOURCE) &&
                         current.as.native_resource->identity==current.integer_small;
                break;
            case VAL_FUNCTION:
                accepted=value_heap_owns(heap,current.as.function,VAL_FUNCTION) &&
                         current.as.function->owner==heap &&
                         current.as.function->identity==current.integer_small;
                break;
            case VAL_ENUM_VALUE:
                accepted=value_heap_owns(heap,current.as.enum_value,VAL_ENUM_VALUE) &&
                         current.as.enum_value->owner==heap &&
                         current.as.enum_value->identity==current.integer_small;
                break;
            case VAL_USER_STRUCT:
                if (!value_heap_owns(heap,current.as.user_struct,VAL_USER_STRUCT)){
                    accepted=0;
                } else if (mark_seen_insert(current.as.user_struct,&seen,
                                            &seen_capacity,&seen_count)){
                    KirUserStruct* object=current.as.user_struct;
                    accepted=object->owner==heap &&
                             object->identity==current.integer_small;
                    if (accepted) mark_work_push(&work,&length,&capacity,object->fields);
                }
                break;
            case VAL_RECORD_LAYOUT:
                accepted=value_heap_owns(heap,current.as.record_layout,VAL_RECORD_LAYOUT);
                break;
            case VAL_RECORD:
                if (!value_heap_owns(heap,current.as.record,VAL_RECORD)){
                    accepted=0;
                } else {
                    accepted=value_heap_owns(heap,current.as.record->arena,VAL_ARENA) &&
                             value_heap_owns(heap,current.as.record->layout,VAL_RECORD_LAYOUT);
                }
                break;
            case VAL_LIST:
                if (!current.as.list){ accepted=0; break; }
                if (!mark_seen_insert(current.as.list,&seen,&seen_capacity,&seen_count)) break;
                for (size_t i=0;i<current.as.list->len;++i)
                    mark_work_push(&work,&length,&capacity,current.as.list->data[i]);
                break;
            case VAL_DICT:
                if (!current.as.dict){ accepted=0; break; }
                if (!mark_seen_insert(current.as.dict,&seen,&seen_capacity,&seen_count)) break;
                for (size_t i=0;i<current.as.dict->len;++i)
                    mark_work_push(&work,&length,&capacity,current.as.dict->vals[i]);
                break;
            default: break;
        }
    }
    kir_free(work);
    kir_free((void*)seen);
    return accepted;
}

static const KirValueHeap* value_heap_owner(const void* object,ValKind kind){
    for (const KirValueHeap* candidate=all_value_heaps;candidate;
         candidate=candidate->all_next){
        if (value_heap_owns(candidate,object,kind)) return candidate;
    }
    return NULL;
}

int v_value_heap_accepts_value(const KirValueHeap* heap,
                               const KirBigIntHeap* bigints,Value value){
    if (!heap || !v_value_heap_accepts_memory_handle(heap,value)) return 0;

    Value* work=NULL;
    size_t length=0,capacity=0;
    const void** seen=NULL;
    size_t seen_count=0,seen_capacity=0;
    int accepted=1;
    mark_work_push(&work,&length,&capacity,value);
    while (length && accepted){
        Value current=work[--length];
        switch (current.kind){
            case VAL_NUM:
                if (current.is_integer && current.integer_big)
                    accepted=kir_bigint_heap_accepts_value(bigints,current.integer_big);
                break;
            case VAL_STR: {
                const KirValueHeap* owner=value_heap_owner(current.as.str.s,VAL_STR);
                accepted=!owner || owner==heap;
                break;
            }
            case VAL_LIST:
                if (!current.as.list){ accepted=0; break; }
                {
                    const KirValueHeap* owner=value_heap_owner(current.as.list,VAL_LIST);
                    if (owner && owner!=heap){ accepted=0; break; }
                }
                if (!accepted) break;
                if (!mark_seen_insert(current.as.list,&seen,&seen_capacity,&seen_count)) break;
                for (size_t i=0;i<current.as.list->len;++i)
                    mark_work_push(&work,&length,&capacity,current.as.list->data[i]);
                break;
            case VAL_DICT:
                if (!current.as.dict){ accepted=0; break; }
                {
                    const KirValueHeap* owner=value_heap_owner(current.as.dict,VAL_DICT);
                    if (owner && owner!=heap){ accepted=0; break; }
                }
                if (!accepted) break;
                if (!mark_seen_insert(current.as.dict,&seen,&seen_capacity,&seen_count)) break;
                for (size_t i=0;i<current.as.dict->len;++i)
                    mark_work_push(&work,&length,&capacity,current.as.dict->vals[i]);
                break;
            case VAL_USER_STRUCT:
                if (!mark_seen_insert(current.as.user_struct,&seen,
                                      &seen_capacity,&seen_count)) break;
                mark_work_push(&work,&length,&capacity,
                               current.as.user_struct->fields);
                break;
            default:
                break;
        }
    }
    kir_free(work);
    kir_free((void*)seen);
    return accepted;
}

static KirArena* checked_arena(Value arena){
    if (arena.kind!=VAL_ARENA || !arena.as.arena) return NULL;
    if (current_value_heap &&
        !value_heap_owns(current_value_heap,arena.as.arena,VAL_ARENA)) return NULL;
    return arena.as.arena;
}

static KirBuffer* checked_buffer(Value buffer){
    if (buffer.kind!=VAL_BUFFER || !buffer.as.buffer) return NULL;
    if (current_value_heap &&
        !value_heap_owns(current_value_heap,buffer.as.buffer,VAL_BUFFER)) return NULL;
    return buffer.as.buffer;
}

Value v_arena_new(size_t capacity){
    return v_arena_new_aligned(capacity,64);
}

Value v_arena_new_aligned(size_t capacity, size_t base_alignment){
    /* Reject impossible alignment and capacity arithmetic before allocating. */
    if (!current_value_heap || !base_alignment ||
        (base_alignment&(base_alignment-1))!=0 ||
        base_alignment>1024u*1024u ||
        capacity>SIZE_MAX-(base_alignment-1) ||
        capacity>current_value_heap->arena_limit-current_value_heap->arena_bytes)
        return v_nil();
    KirArena* arena=(KirArena*)kir_malloc(sizeof(*arena));
    /* One extra byte for an empty arena keeps a stable, non-null base. */
    size_t allocation_size=capacity ? capacity+base_alignment-1 : base_alignment;
    void* allocation=kir_oom_calloc(allocation_size,1);
    if (!allocation){ kir_free(arena); return v_nil(); }
    uintptr_t start=(uintptr_t)allocation;
    uintptr_t mask=(uintptr_t)(base_alignment-1);
    uintptr_t padding=((uintptr_t)base_alignment-(start&mask))&mask;
    uintptr_t aligned=start+padding;
    arena->allocation=allocation;
    arena->data=(unsigned char*)aligned;
    arena->base_alignment=base_alignment;
    arena->owner=current_value_heap;
    arena->capacity=capacity;
    arena->used=0;
    arena->generation=1;
    arena->closed=false;
    arena->borrow_readers=0;
    arena->borrow_writer=false;
    current_value_heap->arena_bytes+=capacity;
    value_heap_track(current_value_heap,arena,VAL_ARENA);
    Value result=v_nil();
    result.kind=VAL_ARENA;
    result.as.arena=arena;
    return result;
}

Value v_arena_allocate(Value value, size_t length, size_t alignment){
    KirArena* arena=checked_arena(value);
    if (!arena || arena->closed || arena->borrow_readers || arena->borrow_writer ||
        !current_value_heap || arena->owner!=current_value_heap ||
        alignment>arena->base_alignment ||
        !alignment || (alignment&(alignment-1))!=0) return v_nil();
    if (arena->used > SIZE_MAX-(alignment-1)) return v_nil();
    size_t start=(arena->used+alignment-1)&~(alignment-1);
    if (start>arena->capacity || length>arena->capacity-start) return v_nil();
    /* Safe default: reused bytes are zero-filled, not previous allocations.
       The cost is proportional to the requested block, not entire arena. */
    if (length) memset(arena->data+start,0,length);
    KirBuffer* buffer=(KirBuffer*)kir_malloc(sizeof(*buffer));
    buffer->arena=arena;
    buffer->offset=start;
    buffer->length=length;
    buffer->generation=arena->generation;
    arena->used=start+length;
    value_heap_track(current_value_heap,buffer,VAL_BUFFER);
    Value result=v_nil();
    result.kind=VAL_BUFFER;
    result.as.buffer=buffer;
    return result;
}

int v_arena_reset(Value value){
    KirArena* arena=checked_arena(value);
    if (!arena || arena->closed || arena->generation==UINT64_MAX ||
        arena->borrow_readers || arena->borrow_writer) return 0;
    /* Old buffer views remain allocated (GC-managed) but become invalid. */
    ++arena->generation;
    arena->used=0;
    return 1;
}

int v_arena_close(Value value){
    KirArena* arena=checked_arena(value);
    if (!arena || arena->closed || arena->borrow_readers || arena->borrow_writer) return 0;
    arena->closed=true;
    if (arena->data) arena->owner->arena_bytes-=arena->capacity;
    kir_free(arena->allocation);
    arena->allocation=NULL;
    arena->data=NULL;
    arena->used=0;
    return 1;
}

int v_arena_stats(Value value, size_t* capacity, size_t* used, bool* closed){
    KirArena* arena=checked_arena(value);
    if (!arena) return 0;
    if (capacity) *capacity=arena->capacity;
    if (used) *used=arena->used;
    if (closed) *closed=arena->closed;
    return 1;
}

int v_buffer_valid(Value value){
    KirBuffer* buffer=checked_buffer(value);
    if (!buffer) return 0;
    KirArena* arena=buffer->arena;
    if (!arena || (current_value_heap &&
        !value_heap_owns(current_value_heap,arena,VAL_ARENA))) return 0;
    return !arena->closed && buffer->generation==arena->generation &&
           buffer->offset<=arena->capacity &&
           buffer->length<=arena->capacity-buffer->offset;
}

int v_buffer_is_aligned(Value value, size_t alignment){
    if (!alignment || (alignment&(alignment-1))!=0 || !v_buffer_valid(value)) return 0;
    KirBuffer* buffer=value.as.buffer;
    return ((uintptr_t)(buffer->arena->data+buffer->offset) & (alignment-1))==0;
}

int v_buffer_length(Value value, size_t* length){
    KirBuffer* buffer=checked_buffer(value);
    if (!buffer || !v_buffer_valid(value)) return 0;
    if (length) *length=buffer->length;
    return 1;
}

int v_buffer_set_u8(Value value, size_t index, uint8_t byte){
    if (!v_buffer_valid(value)) return 0;
    KirBuffer* buffer=value.as.buffer;
    if (index>=buffer->length || buffer->arena->borrow_readers || buffer->arena->borrow_writer) return 0;
    buffer->arena->data[buffer->offset+index]=byte;
    return 1;
}

int v_buffer_get_u8(Value value, size_t index, uint8_t* byte){
    if (!byte || !v_buffer_valid(value)) return 0;
    KirBuffer* buffer=value.as.buffer;
    if (index>=buffer->length || buffer->arena->borrow_writer) return 0;
    *byte=buffer->arena->data[buffer->offset+index];
    return 1;
}

/* ========================= Typed arrays (V2.1) ========================= */

typedef struct {
    size_t size;
    size_t alignment;
} KirTypedElementProperties;

/* KIR_ELEM_* values are contiguous from 1. Keep size and alignment in one
   table: GCC may merge identical switch tables, which makes ASan's ODR
   tracker report the merged compiler-generated globals as duplicates. */
static const KirTypedElementProperties g_typed_element_properties[] = {
    {sizeof(uint8_t), _Alignof(uint8_t)},
    {sizeof(int32_t), _Alignof(int32_t)},
    {sizeof(int64_t), _Alignof(int64_t)},
    {sizeof(double), _Alignof(double)}
};

static const KirTypedElementProperties* typed_element_properties(KirElementType type){
    if (type<KIR_ELEM_U8 || type>KIR_ELEM_F64) return NULL;
    return &g_typed_element_properties[(size_t)(type-KIR_ELEM_U8)];
}

static size_t typed_element_size(KirElementType type){
    const KirTypedElementProperties* properties=typed_element_properties(type);
    return properties ? properties->size : 0;
}

static size_t typed_element_alignment(KirElementType type){
    const KirTypedElementProperties* properties=typed_element_properties(type);
    return properties ? properties->alignment : 0;
}

static KirTypedArray* checked_typed_array(Value value){
    if (value.kind!=VAL_TYPED_ARRAY || !value.as.typed_array) return NULL;
    if (current_value_heap &&
        !value_heap_owns(current_value_heap,value.as.typed_array,VAL_TYPED_ARRAY)) return NULL;
    return value.as.typed_array;
}

Value v_typed_array_new(Value value, size_t count, KirElementType type){
    KirArena* arena=checked_arena(value);
    size_t width=typed_element_size(type);
    if (!arena || arena->closed || arena->borrow_readers || arena->borrow_writer ||
        !current_value_heap || arena->owner!=current_value_heap || !width || count>SIZE_MAX/width ||
        width>arena->base_alignment) return v_nil();
    size_t bytes=count*width;
    if (arena->used>SIZE_MAX-(width-1)) return v_nil();
    size_t start=(arena->used+width-1)&~(width-1);
    if (start>arena->capacity || bytes>arena->capacity-start) return v_nil();
    if (bytes) memset(arena->data+start,0,bytes);
    KirTypedArray* array=(KirTypedArray*)kir_malloc(sizeof(*array));
    array->arena=arena;
    array->offset=start;
    array->count=count;
    array->type=type;
    array->generation=arena->generation;
    arena->used=start+bytes;
    value_heap_track(current_value_heap,array,VAL_TYPED_ARRAY);
    Value result=v_nil();
    result.kind=VAL_TYPED_ARRAY;
    result.as.typed_array=array;
    return result;
}

int v_typed_array_valid(Value value){
    KirTypedArray* array=checked_typed_array(value);
    if (!array) return 0;
    KirArena* arena=array->arena;
    if (!arena || (current_value_heap &&
        !value_heap_owns(current_value_heap,arena,VAL_ARENA))) return 0;
    size_t width=typed_element_size(array->type);
    return width && !arena->closed && arena->data &&
           array->generation==arena->generation &&
           array->count<=SIZE_MAX/width &&
           array->offset<=arena->capacity &&
           array->count*width<=arena->capacity-array->offset &&
           ((uintptr_t)(arena->data+array->offset)&(width-1))==0;
}

int v_typed_array_length(Value value, size_t* count){
    if (!v_typed_array_valid(value)) return 0;
    if (count) *count=value.as.typed_array->count;
    return 1;
}

int v_typed_array_type(Value value, KirElementType* type){
    if (!v_typed_array_valid(value)) return 0;
    if (type) *type=value.as.typed_array->type;
    return 1;
}

int v_typed_array_is_aligned(Value value){ return v_typed_array_valid(value); }

int v_typed_array_get(Value value, size_t index, Value* out){
    if (!out || !v_typed_array_valid(value)) return 0;
    KirTypedArray* array=value.as.typed_array;
    if (index>=array->count || array->arena->borrow_writer) return 0;
    size_t width=typed_element_size(array->type);
    const unsigned char* at=array->arena->data+array->offset+index*width;
    switch (array->type){
        case KIR_ELEM_U8: { uint8_t n; memcpy(&n,at,sizeof(n)); *out=v_int(n); return 1; }
        case KIR_ELEM_I32: { int32_t n; memcpy(&n,at,sizeof(n)); *out=v_int(n); return 1; }
        case KIR_ELEM_I64: { int64_t n; memcpy(&n,at,sizeof(n)); *out=v_int(n); return 1; }
        case KIR_ELEM_F64: { double n; memcpy(&n,at,sizeof(n)); *out=v_num(n); return 1; }
        default: return 0;
    }
}

int v_typed_array_set(Value value, size_t index, Value number){
    if (!v_typed_array_valid(value) || number.kind!=VAL_NUM) return 0;
    KirTypedArray* array=value.as.typed_array;
    if (index>=array->count || array->arena->borrow_readers || array->arena->borrow_writer) return 0;
    size_t width=typed_element_size(array->type);
    unsigned char* at=array->arena->data+array->offset+index*width;
    int64_t integer;
    switch (array->type){
        case KIR_ELEM_U8: {
            if (!v_num_fits_i64(number,&integer) || integer<0 || integer>UINT8_MAX) return 0;
            uint8_t n=(uint8_t)integer; memcpy(at,&n,sizeof(n)); return 1;
        }
        case KIR_ELEM_I32: {
            if (!v_num_fits_i64(number,&integer) || integer<INT32_MIN || integer>INT32_MAX) return 0;
            int32_t n=(int32_t)integer; memcpy(at,&n,sizeof(n)); return 1;
        }
        case KIR_ELEM_I64: {
            if (!v_num_fits_i64(number,&integer)) return 0;
            memcpy(at,&integer,sizeof(integer)); return 1;
        }
        case KIR_ELEM_F64: {
            double n=v_num_to_double(number);
            if (!isfinite(n)) return 0;
            memcpy(at,&n,sizeof(n)); return 1;
        }
        default: return 0;
    }
}

/* ========================= Fixed-layout records (V2.2) ========================= */

static KirRecordLayout* checked_layout(Value value){
    if (value.kind!=VAL_RECORD_LAYOUT || !value.as.record_layout) return NULL;
    /* C callers outside a running VM have the same explicit pin/lifetime
       obligation as v_typed_array_get. VM execution validates ownership. */
    if (current_value_heap &&
        !value_heap_owns(current_value_heap,value.as.record_layout,VAL_RECORD_LAYOUT))
        return NULL;
    return value.as.record_layout;
}

static int size_align_up(size_t value, size_t alignment, size_t* result){
    if (!alignment || (alignment&(alignment-1))!=0 || value>SIZE_MAX-(alignment-1))
        return 0;
    *result=(value+alignment-1)&~(alignment-1);
    return 1;
}

Value v_record_layout_new(const KirRecordFieldDef* fields, size_t count){
    if (!current_value_heap || !fields || count==0 || count>256) return v_nil();
    size_t offset=0, max_alignment=1;
    /* Validate the entire schema before allocating tracked objects. */
    for (size_t i=0;i<count;++i){
        size_t width=typed_element_size(fields[i].type);
        size_t field_alignment=typed_element_alignment(fields[i].type), start=0;
        if (!fields[i].name || !fields[i].name_length ||
            fields[i].name_length>255 || !width || !field_alignment ||
            memchr(fields[i].name,'\0',fields[i].name_length) ||
            !size_align_up(offset,field_alignment,&start) || start>SIZE_MAX-width)
            return v_nil();
        for (size_t j=0;j<i;++j){
            if (fields[i].name_length==fields[j].name_length &&
                memcmp(fields[i].name,fields[j].name,fields[i].name_length)==0)
                return v_nil();
        }
        offset=start+width;
        if (field_alignment>max_alignment) max_alignment=field_alignment;
    }
    size_t size=0;
    if (!size_align_up(offset,max_alignment,&size)) return v_nil();
    KirRecordLayout* layout=(KirRecordLayout*)kir_malloc(sizeof(*layout));
    layout->fields=(KirRecordField*)kir_xmallocn(count,sizeof(*layout->fields));
    layout->count=count;
    layout->owner=current_value_heap;
    layout->size=size;
    layout->alignment=max_alignment;
    offset=0;
    for (size_t i=0;i<count;++i){
        size_t width=typed_element_size(fields[i].type);
        size_align_up(offset,typed_element_alignment(fields[i].type),&offset); /* checked in preflight */
        layout->fields[i]=(KirRecordField){
            str_dup_n(fields[i].name,fields[i].name_length),
            fields[i].name_length,offset,fields[i].type};
        offset+=width;
    }
    value_heap_track(current_value_heap,layout,VAL_RECORD_LAYOUT);
    Value result=v_nil();
    result.kind=VAL_RECORD_LAYOUT;
    result.as.record_layout=layout;
    return result;
}

int v_record_layout_info(Value value, size_t* size, size_t* alignment, size_t* fields){
    KirRecordLayout* layout=checked_layout(value);
    if (!layout) return 0;
    if (size) *size=layout->size;
    if (alignment) *alignment=layout->alignment;
    if (fields) *fields=layout->count;
    return 1;
}

static KirRecordField* record_field(KirRecordLayout* layout,
                                     const char* name, size_t length){
    if (!layout || !name) return NULL;
    for (size_t i=0;i<layout->count;++i)
        if (layout->fields[i].name_length==length &&
            memcmp(layout->fields[i].name,name,length)==0)
            return &layout->fields[i];
    return NULL;
}

int v_record_field_info(Value value, const char* name, size_t length,
                        size_t* offset, KirElementType* type){
    KirRecordLayout* layout=checked_layout(value);
    KirRecordField* field=record_field(layout,name,length);
    if (!field) return 0;
    if (offset) *offset=field->offset;
    if (type) *type=field->type;
    return 1;
}

Value v_record_new(Value value, Value schema){
    KirArena* arena=checked_arena(value);
    KirRecordLayout* layout=checked_layout(schema);
    if (!arena || !layout || arena->closed || arena->borrow_readers ||
        arena->borrow_writer || !current_value_heap ||
        arena->owner!=current_value_heap || layout->owner!=current_value_heap ||
        layout->alignment>arena->base_alignment) return v_nil();
    size_t start=0;
    if (!size_align_up(arena->used,layout->alignment,&start) ||
        start>arena->capacity || layout->size>arena->capacity-start) return v_nil();
    memset(arena->data+start,0,layout->size);
    KirRecord* record=(KirRecord*)kir_malloc(sizeof(*record));
    *record=(KirRecord){arena,layout,start,arena->generation};
    arena->used=start+layout->size;
    value_heap_track(current_value_heap,record,VAL_RECORD);
    Value result=v_nil();
    result.kind=VAL_RECORD;
    result.as.record=record;
    return result;
}

int v_record_valid(Value value){
    if (value.kind!=VAL_RECORD || !value.as.record ||
        (current_value_heap &&
         !value_heap_owns(current_value_heap,value.as.record,VAL_RECORD))) return 0;
    KirRecord* record=value.as.record;
    KirArena* arena=record->arena;
    KirRecordLayout* layout=record->layout;
    if (current_value_heap &&
        (!value_heap_owns(current_value_heap,arena,VAL_ARENA) ||
         !value_heap_owns(current_value_heap,layout,VAL_RECORD_LAYOUT))) return 0;
    return !arena->closed && arena->data && record->generation==arena->generation &&
           layout->alignment<=arena->base_alignment &&
           record->offset<=arena->capacity &&
           layout->size<=arena->capacity-record->offset &&
           ((uintptr_t)(arena->data+record->offset)&(layout->alignment-1))==0;
}

int v_record_layout_details(Value record, size_t* bytes, size_t* alignment,
                            size_t* field_count){
    if (!v_record_valid(record)) return 0;
    KirRecordLayout* layout=record.as.record->layout;
    if (bytes) *bytes=layout->size;
    if (alignment) *alignment=layout->alignment;
    if (field_count) *field_count=layout->count;
    return 1;
}
int v_record_layout_field_at(Value record, size_t index, const char** name,
                             size_t* name_length, size_t* offset, KirElementType* type){
    if (!v_record_valid(record) || index>=record.as.record->layout->count) return 0;
    const KirRecordField* field=&record.as.record->layout->fields[index];
    if (name) *name=field->name;
    if (name_length) *name_length=field->name_length;
    if (offset) *offset=field->offset;
    if (type) *type=field->type;
    return 1;
}

int v_record_get(Value value, const char* name, size_t length, Value* out){
    if (!out || !v_record_valid(value)) return 0;
    KirRecord* record=value.as.record;
    if (record->arena->borrow_writer) return 0;
    KirRecordField* field=record_field(record->layout,name,length);
    if (!field) return 0;
    const unsigned char* at=record->arena->data+record->offset+field->offset;
    switch (field->type){
        case KIR_ELEM_U8: { uint8_t n; memcpy(&n,at,sizeof(n)); *out=v_int(n); return 1; }
        case KIR_ELEM_I32: { int32_t n; memcpy(&n,at,sizeof(n)); *out=v_int(n); return 1; }
        case KIR_ELEM_I64: { int64_t n; memcpy(&n,at,sizeof(n)); *out=v_int(n); return 1; }
        case KIR_ELEM_F64: { double n; memcpy(&n,at,sizeof(n)); *out=v_num(n); return 1; }
        default: return 0;
    }
}

int v_record_set(Value value, const char* name, size_t length, Value number){
    if (!v_record_valid(value) || number.kind!=VAL_NUM) return 0;
    KirRecord* record=value.as.record;
    if (record->arena->borrow_readers || record->arena->borrow_writer) return 0;
    KirRecordField* field=record_field(record->layout,name,length);
    if (!field) return 0;
    unsigned char* at=record->arena->data+record->offset+field->offset;
    int64_t integer=0;
    switch (field->type){
        case KIR_ELEM_U8: {
            if (!v_num_fits_i64(number,&integer) || integer<0 || integer>UINT8_MAX) return 0;
            uint8_t n=(uint8_t)integer; memcpy(at,&n,sizeof(n)); return 1;
        }
        case KIR_ELEM_I32: {
            if (!v_num_fits_i64(number,&integer) || integer<INT32_MIN || integer>INT32_MAX) return 0;
            int32_t n=(int32_t)integer; memcpy(at,&n,sizeof(n)); return 1;
        }
        case KIR_ELEM_I64: {
            if (!v_num_fits_i64(number,&integer)) return 0;
            memcpy(at,&integer,sizeof(integer)); return 1;
        }
        case KIR_ELEM_F64: {
            double n=v_num_to_double(number);
            if (!isfinite(n)) return 0;
            memcpy(at,&n,sizeof(n)); return 1;
        }
        default: return 0;
    }
}

/* ========================= V2.4 checked zero-copy borrowing ========================= */

/* All checks precede dereferencing the handle. A successful native borrow
   guarantees a stable address; the VM pin keeps the arena and its descriptor
   GC-live, while reader/writer counters prevent reset, close, and mutations. */
int v_memory_borrow_begin(KirValueHeap* heap, Value target, bool writable,
                          const void** data, size_t* bytes){
    if (!heap || !data || !bytes) return 0;
    KirArena* arena=NULL;
    size_t offset=0, size=0;
    switch (target.kind){
        case VAL_BUFFER: {
            if (!value_heap_owns(heap,target.as.buffer,VAL_BUFFER)) return 0;
            KirBuffer* buffer=target.as.buffer;
            arena=buffer->arena;
            if (!value_heap_owns(heap,arena,VAL_ARENA) ||
                !v_buffer_valid(target)) return 0;
            offset=buffer->offset;
            size=buffer->length;
            break;
        }
        case VAL_TYPED_ARRAY: {
            if (!value_heap_owns(heap,target.as.typed_array,VAL_TYPED_ARRAY)) return 0;
            KirTypedArray* array=target.as.typed_array;
            arena=array->arena;
            if (!value_heap_owns(heap,arena,VAL_ARENA) ||
                !v_typed_array_valid(target)) return 0;
            size_t width=typed_element_size(array->type);
            if (!width || array->count>SIZE_MAX/width) return 0;
            offset=array->offset;
            size=array->count*width;
            break;
        }
        case VAL_RECORD: {
            if (!value_heap_owns(heap,target.as.record,VAL_RECORD)) return 0;
            KirRecord* record=target.as.record;
            arena=record->arena;
            if (!value_heap_owns(heap,arena,VAL_ARENA) ||
                !value_heap_owns(heap,record->layout,VAL_RECORD_LAYOUT) ||
                !v_record_valid(target)) return 0;
            offset=record->offset;
            size=record->layout->size;
            break;
        }
        default: return 0;
    }
    if (arena->owner!=heap || arena->closed || !arena->data ||
        offset>arena->capacity || size>arena->capacity-offset) return 0;
    if (writable){
        if (arena->borrow_writer || arena->borrow_readers) return 0;
        arena->borrow_writer=true;
    } else {
        if (arena->borrow_writer || arena->borrow_readers==SIZE_MAX) return 0;
        ++arena->borrow_readers;
    }
    *data=arena->data+offset;
    *bytes=size;
    return 1;
}

int v_memory_borrow_end(KirValueHeap* heap, Value target, bool writable){
    if (!heap) return 0;
    KirArena* arena=NULL;
    switch (target.kind){
        case VAL_BUFFER:
            if (!value_heap_owns(heap,target.as.buffer,VAL_BUFFER)) return 0;
            arena=target.as.buffer->arena;
            break;
        case VAL_TYPED_ARRAY:
            if (!value_heap_owns(heap,target.as.typed_array,VAL_TYPED_ARRAY)) return 0;
            arena=target.as.typed_array->arena;
            break;
        case VAL_RECORD:
            if (!value_heap_owns(heap,target.as.record,VAL_RECORD)) return 0;
            arena=target.as.record->arena;
            break;
        default: return 0;
    }
    if (!value_heap_owns(heap,arena,VAL_ARENA)) return 0;
    if (writable){
        if (!arena->borrow_writer) return 0;
        arena->borrow_writer=false;
    } else {
        if (!arena->borrow_readers) return 0;
        --arena->borrow_readers;
    }
    return 1;
}

/* ========================= List ops ========================= */

static void list_grow(VList* L, size_t need){
    if (L->len + need <= L->cap) return;

    size_t nc = L->cap ? L->cap * 2 : 8;
    while (nc < L->len + need) nc *= 2;

    /* kir_xreallocn: защищённый realloc с проверкой переполнения */
    L->data = (Value*)kir_xreallocn(L->data, nc, sizeof(Value));
    L->cap  = nc;
}

void v_list_push(Value* vl, Value v){
    if (!vl || vl->kind != VAL_LIST) return;
    list_grow(vl->as.list, 1);
    vl->as.list->data[vl->as.list->len++] = v;
}

/* ========================= Dict ops ========================= */

int v_dict_find(Value* vd, const char* k, size_t kn){
    if (!vd || vd->kind != VAL_DICT) return -1;
    for (size_t i = 0; i < vd->as.dict->len; ++i){
        if (vd->as.dict->kn[i] == kn &&
            memcmp(vd->as.dict->keys[i], k, kn) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

static void dict_grow(VDict* D, size_t need){
    if (D->len + need <= D->cap) return;

    size_t nc = D->cap ? D->cap * 2 : 8;
    while (nc < D->len + need) nc *= 2;

    /* Все три массива растут согласованно, OOM -> аварийный выход. */
    D->keys = (char**)  kir_xreallocn(D->keys, nc, sizeof(char*));
    D->kn   = (size_t*) kir_xreallocn(D->kn,   nc, sizeof(size_t));
    D->vals = (Value*)  kir_xreallocn(D->vals, nc, sizeof(Value));
    D->cap  = nc;
}

void v_dict_set(Value* vd, const char* k, size_t kn, Value v){
    if (!vd || vd->kind != VAL_DICT) return;

    int at = v_dict_find(vd, k, kn);
    if (at >= 0){
        vd->as.dict->vals[at] = v;
        return;
    }

    VDict* D = vd->as.dict;
    dict_grow(D, 1);

    size_t idx   = D->len++;
    D->keys[idx] = str_dup_n(k, kn);
    D->kn[idx]   = kn;
    D->vals[idx] = v;
}

int v_dict_get(Value* vd, const char* k, size_t kn, Value* out){
    int at = v_dict_find(vd, k, kn);
    if (at < 0) return 0;
    if (out) *out = vd->as.dict->vals[at];
    return 1;
}

/* ========================= Printing ========================= */

typedef struct {
    const void** objects;
    size_t len;
    size_t cap;
} PrintPath;

static bool print_path_enter(PrintPath* path, const void* object){
    for (size_t i=0;i<path->len;++i) if (path->objects[i]==object) return false;
    if (path->len==path->cap){
        size_t next=path->cap ? path->cap*2 : 16;
        path->objects=(const void**)kir_xreallocn(path->objects,next,sizeof(*path->objects));
        path->cap=next;
    }
    path->objects[path->len++]=object;
    return true;
}

static void print_value(Value v, PrintPath* path);

static void print_string(const char* s, size_t n){
#ifdef _WIN32
    /* Windows Console: WriteConsoleW, чтобы кириллица не ломалась,
       когда консоль в CP866/1251 и т.п. */
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  mode = 0;
    if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)){
        int wlen = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, NULL, 0);
        if (wlen > 0){
            WCHAR* wbuf = (WCHAR*)kir_malloc(((size_t)wlen + 1) * sizeof(WCHAR));
            MultiByteToWideChar(CP_UTF8, 0, s, (int)n, wbuf, wlen);
            wbuf[wlen] = 0;
            DWORD written = 0;
            WriteConsoleW(h, wbuf, (DWORD)wlen, &written, NULL);
            kir_free(wbuf);
            return;
        }
    }
#endif
    fwrite(s, 1, n, stdout);
}

static void print_value(Value v, PrintPath* path){
    switch (v.kind){
        case VAL_NIL:
            fputs("пусто", stdout);
            break;

        case VAL_NUM: {
            size_t n = 0;
            char* text = v_num_to_string(v, &n);
            print_string(text, n);
            kir_free(text);
            break;
        }

        case VAL_BOOL:
            fputs(v.as.boolean ? "истина" : "ложь", stdout);
            break;

        case VAL_STR:
            if (v.as.str.s)
                print_string(v.as.str.s, (size_t)v.as.str.n);
            break;

        case VAL_LIST: {
            if (!print_path_enter(path,v.as.list)){ fputs("[...]",stdout); break; }
            fputc('[', stdout);
            for (size_t i = 0; i < v.as.list->len; ++i){
                if (i) fputs(", ", stdout);
                print_value(v.as.list->data[i],path);
            }
            fputc(']', stdout);
            --path->len;
            break;
        }

        case VAL_ARENA: {
            size_t cap=0, used=0; bool closed=false;
            (void)v_arena_stats(v,&cap,&used,&closed);
            printf("<область памяти%s: %zu/%zu>",closed ? " закрыта" : "",used,cap);
            break;
        }
        case VAL_BUFFER: {
            size_t size=0;
            if (v_buffer_length(v,&size)) printf("<буфер: %zu байт>",size);
            else fputs("<недействительный буфер>",stdout);
            break;
        }
        case VAL_TYPED_ARRAY: {
            size_t count=0;
            if (v_typed_array_length(v,&count))
                printf("<типизированный массив: %zu элементов>",count);
            else fputs("<недействительный типизированный массив>",stdout);
            break;
        }
        case VAL_RECORD_LAYOUT: {
            size_t size=0, fields=0;
            if (v_record_layout_info(v,&size,NULL,&fields))
                printf("<структура: %zu полей, %zu байт>",fields,size);
            else fputs("<недействительная структура>",stdout);
            break;
        }
        case VAL_RECORD:
            fputs(v_record_valid(v) ? "<запись>" : "<недействительная запись>",stdout);
            break;
        case VAL_NATIVE_RESOURCE:
            fputs("<внешний ресурс>",stdout);
            break;
        case VAL_FUNCTION:
            fputs("<функция>",stdout);
            break;
        case VAL_USER_STRUCT:
            fputs("<структура>",stdout);
            break;
        case VAL_ENUM_VALUE: {
            const char* variant=NULL;
            size_t variant_len=0;
            if (v_enum_value_variant(v,&variant,&variant_len))
                print_string(variant,variant_len);
            else fputs("<перечисление>",stdout);
        } break;
        case VAL_DICT: {
            if (!print_path_enter(path,v.as.dict)){ fputs("{...}",stdout); break; }
            fputc('{', stdout);
            for (size_t i = 0; i < v.as.dict->len; ++i){
                if (i) fputs(", ", stdout);
                print_string(v.as.dict->keys[i], v.as.dict->kn[i]);
                fputs(": ", stdout);
                print_value(v.as.dict->vals[i],path);
            }
            fputc('}', stdout);
            --path->len;
            break;
        }
    }
}

void v_print(Value v){
    PrintPath path={0};
    print_value(v,&path);
    kir_free((void*)path.objects);
}

bool v_truthy(Value v){
    switch (v.kind){
        case VAL_BOOL: return v.as.boolean;
        case VAL_NUM:  return v.is_integer ? (v.integer_big ? v_num_to_double(v) != 0.0 : v.integer_small != 0)
                                           : v.as.num != 0.0;
        case VAL_STR:  return v.as.str.n > 0;
        case VAL_LIST: return v.as.list->len > 0;
        case VAL_DICT: return v.as.dict->len > 0;
        case VAL_ARENA: return true;
        case VAL_BUFFER: return true;
        case VAL_TYPED_ARRAY: return true;
        case VAL_RECORD_LAYOUT: return true;
        case VAL_RECORD: return true;
        case VAL_NATIVE_RESOURCE: return v.as.native_resource != NULL;
        case VAL_FUNCTION: return v.as.function != NULL;
        case VAL_USER_STRUCT: return v.as.user_struct != NULL;
        case VAL_ENUM_VALUE: return v.as.enum_value != NULL;
        default:       return false;
    }
}
