/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "kirton/number.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Базовый тип значения Kirton:
   - VAL_NIL   : отсутствие значения
   - VAL_NUM   : число (double)
   - VAL_BOOL  : булево
   - VAL_STR   : строка (UTF-8, длина в байтах)
   - VAL_LIST  : список Value
   - VAL_DICT  : словарь строковых ключей -> Value
*/
typedef enum {
    VAL_NIL,
    VAL_NUM,
    VAL_BOOL,
    VAL_STR,
    VAL_LIST,
    VAL_DICT,
    VAL_ARENA,    /* opaque, GC-owned fixed-capacity memory arena */
    VAL_BUFFER,   /* checked view into an arena (not a raw pointer) */
    VAL_TYPED_ARRAY, /* contiguous, checked, arena-backed numeric array */
    VAL_RECORD_LAYOUT, /* immutable, GC-owned description of native-layout scalar fields */
    VAL_RECORD, /* checked arena-backed record referring to its layout */
    VAL_NATIVE_RESOURCE, /* opaque VM-owned native handle; never a raw Kirton pointer */
    VAL_FUNCTION, /* GC-managed function/closure value */
    VAL_USER_STRUCT,
    VAL_ENUM_VALUE
} ValKind;

typedef struct Value Value;
typedef struct KirValueHeap KirValueHeap;
typedef struct KirArena KirArena;
typedef struct KirBuffer KirBuffer;
typedef struct KirTypedArray KirTypedArray;
typedef struct KirRecordLayout KirRecordLayout;
typedef struct KirRecord KirRecord;
typedef struct KirNativeResource KirNativeResource;
typedef struct KirFunction KirFunction;
typedef struct AstFunc AstFunc;
typedef struct AstUserType AstUserType;
typedef struct KirUserStruct KirUserStruct;
typedef struct KirEnumValue KirEnumValue;
/* Both callbacks are synchronous, single-threaded. The destructor MUST NOT
   call any Kirton VM API or throw/longjmp. The visit callback may invoke GC,
   but cannot close the visited resource or destroy its owning VM. */
typedef void (*KirNativeResourceDestroy)(void* native_handle, void* user_data);
typedef int (*KirNativeResourceVisit)(void* native_handle, void* user_data);

/* Element representations use host-native byte order, not a wire format. */
typedef enum {
    KIR_ELEM_U8 = 1,
    KIR_ELEM_I32,
    KIR_ELEM_I64,
    KIR_ELEM_F64
} KirElementType;

typedef struct {
    const char* name;
    size_t name_length;
    KirElementType type;
} KirRecordFieldDef;

/* Динамический список */
typedef struct {
    Value*  data;
    size_t  len;
    size_t  cap;
} VList;

/* Простейший словарь "строка -> Value".
   Ключи — heap-строки, копируются внутрь словаря. */
typedef struct {
    char**  keys;
    size_t* kn;    /* длина key[i] */
    Value*  vals;
    size_t  len;
    size_t  cap;
} VDict;

/* Универсальное значение VM. */
struct Value {
    ValKind kind;
    union {
        double num;
        bool   boolean;
        struct { char* s; size_t n; } str; /* UTF-8, n байт */
        VList* list;
        VDict* dict;
        KirArena* arena;
        KirBuffer* buffer;
        KirTypedArray* typed_array;
        KirRecordLayout* record_layout;
        KirRecord* record;
        KirNativeResource* native_resource;
        KirFunction* function;
        KirUserStruct* user_struct;
        KirEnumValue* enum_value;
    } as;
    /* VAL_NUM can be an exact integer or an IEEE-754 double. as.num stores
       the double view used by existing native APIs; integer fields preserve
       exactness in the language core. Big integers are immutable. */
    bool       is_integer;
    int64_t    integer_small;
    KirBigInt* integer_big;
};

/* Конструкторы примитивов */

static inline Value v_nil(void){
    Value v;
    v.kind = VAL_NIL;
    v.is_integer = false;
    v.integer_small = 0;
    v.integer_big = NULL;
    return v;
}

Value v_num(double x);
/* Opt-in heap: external C Values must be marked as roots before each sweep.
   Destroy invalidates all strings and collections owned by the heap. */
KirValueHeap* v_value_heap_new(void);
void v_value_heap_destroy(KirValueHeap* heap);
KirValueHeap* v_value_heap_set_current(KirValueHeap* heap);
void v_value_heap_mark(KirValueHeap* heap, KirBigIntHeap* bigints, Value value);
Value v_function_new(AstFunc* function, void* closure,
                     void (*trace_closure)(void*,void*),
                     void (*release_closure)(void*,void*),void* trace_context);
int v_function_is_owned(const KirValueHeap* heap, Value value);
AstFunc* v_function_ast(Value value);
void* v_function_closure(Value value);
Value v_user_struct_new(AstUserType* type,Value fields);
AstUserType* v_user_struct_type(Value value);
Value v_user_struct_fields(Value value);
Value v_enum_value_new(AstUserType* type,const char* variant,size_t variant_len);
AstUserType* v_enum_value_type(Value value);
int v_enum_value_variant(Value value,const char** name,size_t* len);
size_t v_value_heap_sweep(KirValueHeap* heap);
size_t v_value_heap_count(const KirValueHeap* heap);
/* Only arena reservations, not total bytes used by strings/lists/BigInt. */
size_t v_value_heap_arena_bytes(const KirValueHeap* heap);
int v_value_heap_set_arena_limit(KirValueHeap* heap, size_t maximum_bytes);
/* Reject foreign arena/buffer/typed-array/record/layout handles at a VM boundary,
   including inside cyclic or nested lists/dicts from C. */
int v_value_heap_accepts_memory_handle(const KirValueHeap* heap, Value value);
/* Reject managed strings, lists, dictionaries and BigInts owned by other VMs.
   Unmanaged values remain caller-managed and must outlive every use. */
int v_value_heap_accepts_value(const KirValueHeap* heap,
                               const KirBigIntHeap* bigints, Value value);
/* Relinquish ordinary Value ownership without freeing them; adopted native
   resources are finalized (to avoid losing their only cleanup callback). */
void v_value_heap_release_all(KirValueHeap* heap);
/* Set the current thread's heap for subsequently created large integer Values. */
KirBigIntHeap* v_bigint_heap_set_current(KirBigIntHeap* heap);
/* Mark managed big integers reachable through lists and dictionaries. */
void v_mark_bigints(KirBigIntHeap* heap, Value value);
Value v_int(int64_t x);
Value v_int_from_size(size_t x);
Value v_int_from_double(double x);
Value v_int_parse(const char* text, size_t len, unsigned int base);
Value v_bool(bool b);
bool  v_num_is_integer(Value v);
int   v_num_fits_i64(Value v, int64_t* out);
int   v_num_to_int_trunc(Value v, int* out);
double v_num_to_double(Value v);
int   v_num_compare(Value a, Value b, int* out_compare);
bool  v_num_equal(Value a, Value b);
Value v_num_add(Value a, Value b);
Value v_num_sub(Value a, Value b);
Value v_num_mul(Value a, Value b);
Value v_num_neg(Value v);
Value v_num_floor_div(Value a, Value b);
Value v_num_mod(Value a, Value b);
Value v_num_bitwise(Value a, Value b, char op);
Value v_num_bitnot(Value value);
Value v_num_shift(Value value, size_t bits, bool left);
char* v_num_to_string(Value v, size_t* out_len);

/* Копирует строку s:
   - если n >= 0 — берётся ровно n байт;
   - если n < 0  — до '\0'. */
Value v_str_copy(const char* s, int n);

/* Пустые коллекции */
Value v_list_new(void);
Value v_dict_new(void);

/* Операции со списком */
void  v_list_push(Value* vl, Value v);

/* Операции со словарём:
   - find: возвращает индекс или -1
   - set : вставка/перезапись (копирует ключ)
   - get : 1 при успехе, 0 если нет ключа
*/
int   v_dict_find(Value* vd, const char* k, size_t kn);
void  v_dict_set (Value* vd, const char* k, size_t kn, Value v);
int   v_dict_get (Value* vd, const char* k, size_t kn, Value* out);

/* Memory System V2 (preview): safe fixed-capacity arenas and byte buffers.
   An arena and every buffer view are GC-managed VM values. An arena stays
   reachable as long as at least one of its buffer views is reachable.
   Reset/close revoke ALL older buffer views (generation checked on access).
   Neither raw pointers nor arbitrary physical addresses are exposed to Kirton.
   The API is single-threaded; do not mix objects from different VMs.
   Arena operations fail on invalid arguments/closed arenas/out-of-space.
   No object can be created without an active KirValueHeap (VM context).
*/
/* Default base alignment is 64 bytes. The requested allocation alignment
   must be <= arena base alignment; this guarantees actual pointer alignment. */
Value v_arena_new(size_t capacity);
Value v_arena_new_aligned(size_t capacity, size_t base_alignment);
Value v_arena_allocate(Value arena, size_t length, size_t alignment);
int v_arena_reset(Value arena);
int v_arena_close(Value arena);
int v_arena_stats(Value arena, size_t* capacity, size_t* used, bool* closed);
int v_buffer_valid(Value buffer);
/* Checks the backing address's alignment without exposing a raw pointer. */
int v_buffer_is_aligned(Value buffer, size_t alignment);
int v_buffer_length(Value buffer, size_t* length);
int v_buffer_set_u8(Value buffer, size_t index, uint8_t byte);
int v_buffer_get_u8(Value buffer, size_t index, uint8_t* byte);

/* V2.1: tightly packed, zero-initialized, numeric arrays in an arena.
   The array is GC-tracked, holds its arena live, and is invalidated on
   reset/close. Values are copied via memcpy, without pointer exposure.
   Each operation is range-checked and rejects a stale or foreign handle.
   A failed get/set never accesses the underlying storage. */
Value v_typed_array_new(Value arena, size_t count, KirElementType type);
int v_typed_array_valid(Value array);
int v_typed_array_length(Value array, size_t* count);
int v_typed_array_type(Value array, KirElementType* type);
int v_typed_array_is_aligned(Value array);
int v_typed_array_get(Value array, size_t index, Value* out);
int v_typed_array_set(Value array, size_t index, Value number);

/* V2.2: immutable scalar-only layouts and checked records. Layout uses
   host C-compatible scalar alignment/padding, *not* a portable wire format.
   Field names/types are copied into the GC-owned layout. No VM pointers or
   GC-managed values may be stored in arena memory. */
Value v_record_layout_new(const KirRecordFieldDef* fields, size_t count);
int v_record_layout_info(Value layout, size_t* size, size_t* alignment, size_t* fields);
int v_record_field_info(Value layout, const char* name, size_t length,
                        size_t* offset, KirElementType* type);
Value v_record_new(Value arena, Value layout);
int v_record_valid(Value record);
/* Read-only description of a checked record's layout for plugin ABI checking.
   The returned field name is borrowed from its GC-owned layout. */
int v_record_layout_details(Value record, size_t* bytes, size_t* alignment,
                            size_t* field_count);
int v_record_layout_field_at(Value record, size_t index, const char** name,
                             size_t* name_length, size_t* offset, KirElementType* type);
int v_record_get(Value record, const char* name, size_t length, Value* out);
int v_record_set(Value record, const char* name, size_t length, Value number);

/* Internal checked, VM-owned zero-copy spans (V2.4).
   Callers MUST keep the owning VM alive and pair begin/end exactly once.
   Multiple readers OR one exclusive writer per entire arena; no threads.
   The native VM facade in eval.h owns the lifetime token and GC pin. */
int v_memory_borrow_begin(KirValueHeap* heap, Value target, bool writable,
                          const void** data, size_t* bytes);
int v_memory_borrow_end(KirValueHeap* heap, Value target, bool writable);

/* V2.5 internal primitives. Embedding applications should prefer the
   VM-scoped facade in eval.h; native handles can never be serialized or
   passed to a different VM. The GC calls destructors exactly once when an
   unclosed resource becomes unreachable or on VM teardown. */
Value v_native_resource_adopt(KirValueHeap* owner, void* handle, const char* type_tag,
                              KirNativeResourceDestroy destroy, void* user_data);
int v_native_resource_is_open(KirValueHeap* owner, Value value, const char* type_tag);
int v_native_resource_close(KirValueHeap* owner, Value value);
int v_native_resource_visit(KirValueHeap* owner, Value value, const char* type_tag,
                            KirNativeResourceVisit callback, void* user_data);
/* V2.8: a long-running native resource lease increments the same protection
   count as visits, but keeps it across multiple synchronous host calls.
   Lease owner must explicitly end it after external IO/GPU completion. */
int v_native_resource_lease_start(KirValueHeap* owner, Value resource,
                                  const char* type_tag);
int v_native_resource_lease_end(KirValueHeap* owner, Value resource);
int v_value_heap_is_finalizing(const KirValueHeap* owner);
/* For built-in native modules while a VM is executing (TLS-bound heap). */
int v_native_resource_close_current(Value value);
int v_native_resource_is_open_current(Value value);

/* Печать и приведение к "истинности" по правилам языка */
void  v_print(Value v);
bool  v_truthy(Value v);

#ifdef __cplusplus
}
#endif
