/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#pragma once
#include "kirton/ast.h"
#include "kirton/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------
   Исполнение Kirton: VM + инициализация и вызовы
   По умолчанию выполняется только top-level init.
   Конкретную функцию можно запустить через eval_set_entry()/--entry.
   -------------------------------------------------------------------- */

/* Сохранён для совместимости: выполняет init и регистрирует функции. */
int  eval_program(AstProgram* p);

/* Явная установка точки входа (NULL или "" — не вызывать ничего). */
void eval_set_entry(const char* name_or_null);

/* Opaque-типы VM и окружения. */
typedef struct VM VM;
typedef struct KirEnv KirEnv;
typedef struct KirPinnedValue KirPinnedValue;
typedef struct KirMemoryBorrow KirMemoryBorrow;
typedef struct KirNativeResourceLease KirNativeResourceLease;

typedef enum {
    KIR_NUM_ADD=1, KIR_NUM_SUB, KIR_NUM_MUL, KIR_NUM_DIV,
    KIR_NUM_FLOOR_DIV, KIR_NUM_MOD, KIR_NUM_EQ, KIR_NUM_NE,
    KIR_NUM_LT, KIR_NUM_LE, KIR_NUM_GT, KIR_NUM_GE,
    KIR_NUM_BITAND, KIR_NUM_BITOR, KIR_NUM_BITXOR,
    KIR_NUM_SHL, KIR_NUM_SHR
} KirNumericOperation;
typedef enum {
    KIR_NUMERIC_DIVIDE_BY_ZERO=-1,
    KIR_NUMERIC_INVALID=0,
    KIR_NUMERIC_OK=1
} KirNumericStatus;

/* Создать VM. Возвращает NULL, если отдельное FFI состояние не удалось
   создать. Общие kir_* OOM пути могут завершить процесс; см. OOM contract. */
VM*  vm_new(void);
/* Закрывает VM; значения, выделенные ею, и её окружения становятся невалидны.
   Исходные AstProgram* принадлежат вызывающей стороне и освобождаются отдельно. */
void vm_free(VM* vm);
/* Returns 0 without destroying the VM while any zero-copy borrow is active.
   vm_free() also refuses destruction in that case (diagnostic to stderr). */
int vm_try_free(VM* vm);

/* Автоматический GC работает на безопасных границах операторов.
   Значение, удерживаемое C-кодом между вызовами, нужно закрепить.
   Незакреплённые Value из vm_call_by_name не гарантируют время жизни
   после следующей сборки. Управляемые значения можно закреплять/передавать
   только в их VM; неуправляемые значения (в том числе C-owned и literal из AST)
   остаются ответственностью вызывающего и должны жить дольше их использования.
   pin действителен до unpin или vm_free и не передаёт владение. */
KirPinnedValue* vm_value_pin(VM* vm, Value value);
Value vm_value_pinned(const KirPinnedValue* pin);
void vm_value_unpin(VM* vm, KirPinnedValue* pin);
/* Creates a VM-owned UTF-8 string suitable for pinning with vm_value_pin(). */
/* VM-owned collection constructors: allocate under this VM's Value heap.
   Values returned by these functions must be rooted or pinned by the caller. */
Value vm_value_list_new(VM* vm);
Value vm_value_dict_new(VM* vm);
/* Native compilers use the same VM-owned heap for user types. */
Value vm_value_user_struct_new(VM* vm, AstUserType* type, Value fields);
Value vm_value_enum_new(VM* vm, AstUserType* type, const char* variant, size_t len);
/* Native-generated callback descriptors contain an AST header only as an
   identity token. Their bodies are always executed by compiled machine code. */
Value vm_value_compiled_function_new(VM* vm, AstFunc* identity, void* descriptor,
                                     Value captures);
/* Retrieve the registered callback token and the GC-traced capture list. */
void* vm_value_compiled_descriptor(Value function);
Value vm_value_compiled_captures(Value function);
int vm_value_is_compiled_function(VM* vm, Value function);
Value vm_value_string_new(VM* vm, const char* utf8, size_t length);
/* Invoke an already-registered Value-ABI native function with this VM's
   heaps active. All arguments must be pinned by the caller. Returns 0 for
   an absent name or invalid invocation (not a Kirton boolean result). */
int vm_value_native_call(VM* vm,const char* name,size_t name_length,
                         int argc,const Value* argv,Value* out);
int vm_value_number_parse(VM* vm, const char* text, size_t length,
                          int is_integer, Value* out);
int vm_value_numeric_binary(VM* vm, KirNumericOperation operation,
                            Value left, Value right, Value* out);
int vm_value_numeric_neg(VM* vm, Value input, Value* out);
int vm_value_numeric_bitnot(VM* vm, Value input, Value* out);
size_t vm_gc_collect(VM* vm);
size_t vm_gc_tracked(const VM* vm);
size_t vm_gc_collections(const VM* vm);
/* Tracks only live arena reservations (NOT total memory consumed by a VM). */
size_t vm_memory_arena_bytes(const VM* vm);
/* Rejects limits smaller than the currently reserved arena storage. */
int vm_memory_set_arena_limit(VM* vm, size_t maximum_bytes);

/* V2.3: VM-scoped, synchronous, checked native access. These routines never
   expose borrowed arena pointers. Every handle is checked against this VM,
   and stale handles are rejected. C must retain values across GC via pin.
   Passing a dangling C Value after collection remains undefined behavior. */
int vm_memory_buffer_read(VM* vm, Value buffer, size_t offset, void* destination, size_t bytes);
int vm_memory_buffer_write(VM* vm, Value buffer, size_t offset, const void* source, size_t bytes);
int vm_memory_array_get(VM* vm, Value array, size_t index, Value* out);
int vm_memory_array_set(VM* vm, Value array, size_t index, Value number);
int vm_memory_record_get(VM* vm, Value record, const char* field, size_t field_length, Value* out);
int vm_memory_record_set(VM* vm, Value record, const char* field, size_t field_length, Value number);

/* V2.4: scoped, zero-copy native borrowing (single-threaded).
   The handle is an opaque token, valid until vm_memory_borrow_release().
   Target may be a buffer, typed numeric array, or fixed-layout record.
   READ allows other readers of the same arena, WRITE is exclusive per arena.
   While borrowed, arena reset/close/new allocations are rejected; VM destruction
   is rejected; VM writes are rejected, and VM reads are rejected during WRITE.
   GC may still run: the borrow pins the target and its backing arena.
   Returned addresses are valid ONLY while the token is held. Copying the raw
   address past release is unsafe C usage and cannot be detected by Kirton.
   No concurrent threads, retained callbacks, or async use are permitted. */
typedef enum { KIR_BORROW_READ = 1, KIR_BORROW_WRITE = 2 } KirBorrowMode;
KirMemoryBorrow* vm_memory_borrow(VM* vm, Value target, KirBorrowMode mode);
const void* vm_memory_borrow_data(const KirMemoryBorrow* borrow);
void* vm_memory_borrow_mutable_data(const KirMemoryBorrow* borrow);
size_t vm_memory_borrow_size(const KirMemoryBorrow* borrow);
size_t vm_memory_borrow_count(const VM* vm);
/* Checks that the token is owned by this VM; releases precisely once. */
int vm_memory_borrow_release(VM* vm, KirMemoryBorrow* borrow);

/* V2.5: deterministic, single-threaded native resource ownership.
   Adopt transfers ownership only on success (VAL_NATIVE_RESOURCE). On
   failure, the caller keeps ownership and must destroy its native handle.
   type_tag is copied; destroy(handle, user_data) must not re-enter Kirton.
   Both handle and user_data belong to the embedding application, and must
   remain valid until exactly one destructor invocation. No VM-managed Value
   may be referenced by those pointers without an independent GC pin.
   A resource is finalized at explicit close, GC after loss of reachability,
   or VM destruction. Close fails when already closed or actively visited.
   Visit is synchronous; it pins the resource while calling the visitor.
   The visitor MUST NOT retain the supplied pointer for asynchronous use. */
Value vm_native_resource_adopt(VM* vm, void* native_handle, const char* type_tag,
                               KirNativeResourceDestroy destroy, void* user_data);
int vm_native_resource_is_open(VM* vm, Value resource, const char* type_tag);
int vm_native_resource_close(VM* vm, Value resource);
int vm_native_resource_visit(VM* vm, Value resource, const char* type_tag,
                             KirNativeResourceVisit visitor, void* user_data);

/* V2.8: explicit long-running native resource lifetime.
   Begin pins the handle and prevents close/GC finalization/VM shutdown.
   The host may submit an asynchronous operation inside lease_visit(). The
   lease MUST remain active until the external operation is confirmed complete.
   Release is explicit and must run on the VM owner thread. The host cannot
   safely release before an outstanding GPU/DMA/IO operation has completed.
   Only in-process trusted C code can acquire leases; no Kirton script syntax. */
KirNativeResourceLease* vm_native_resource_lease_begin(VM* vm, Value resource,
                                                       const char* type_tag);
int vm_native_resource_lease_visit(VM* vm, KirNativeResourceLease* lease,
                                  KirNativeResourceVisit visitor, void* user_data);
int vm_native_resource_lease_release(VM* vm, KirNativeResourceLease* lease);
size_t vm_native_resource_lease_count(const VM* vm);

/* V2.6: opt-in, single-threaded C plugin ABI (kirton/ffi.h).
   Explicitly trusted native libraries can crash the process. The API uses
   primitive values only; no VM pointers, raw arena pointers or async calls.
   The VM owns loaded libraries and unloads them on successful vm_try_free().
   FFI is disabled by default. Host must opt in before loading/calling. */
int vm_ffi_set_enabled(VM* vm, int enabled);
int vm_ffi_load(VM* vm, const char* library_path_utf8);
int vm_ffi_call(VM* vm, const char* module, const char* function,
                int argc, const Value* argv, Value* out);
const char* vm_ffi_last_error(const VM* vm);
/* V2.7+: plugin ABI V2 (kirton_ffi_module_v2) coexists with the V1 plugin
   entrypoint. Record arguments are validated copies, not arena pointers;
   resource arguments are scoped tokens, not native addresses. V2 can return
   a GC-owned record or an existing input native resource. */
/* Only for the CLI's internally created VM (eval_program). */
void eval_set_ffi_enabled(int enabled);

/* Выполнить скрипт-файл:
   - парсит файл,
   - выполняет его top-level init в новом корневом окружении,
   - регистрирует функции в VM.
   Возвращает это корневое окружение. */
KirEnv* vm_run_root(VM* vm, const char* path_utf8);

/* Выполнить top-level init и зарегистрировать функции; 0 при успехе, иначе ошибка. */
int    vm_eval_program_into(VM* vm, AstProgram* p, KirEnv* env);

/* Проверка наличия и вызов функции; 1 при успехе, 0 если вызов не выполнен. */
int    vm_has_func(VM* vm, const char* name);
int    vm_call_by_name(VM* vm, const char* name, int argc, const Value* argv, Value* out);

/* --------------------------------------------------------------------
   Импорт и «включить»
   --------------------------------------------------------------------

   vm_import — стабильная загрузка модуля с кэшем и анти-циклами.

   Имя модуля:
     - Поддерживается точечная нотация: "пакет.подпакет.модуль"
       → ищется как "пакет/подпакет/модуль.кир".
     - Можно передать уже путь (относительный/абсолютный) — суффикс .кир добавляется,
       но если файл и так существует без добавления, он будет использован как есть.

   Поиск модулей (порядок):
     1) Если имя — путь, пробуем <имя>.кир и <имя> как файл.
     2) from_dir (если задано, см. синтаксис: `импорт имя из "каталог"`).
     3) Каталоги из переменной среды KIRTON_PATH
        (разделители: ':' на POSIX, ';' на Windows).
     4) Текущая рабочая директория.

   Кэш:
     - Ключ кэша — нормализованный абсолютный путь найденного файла.
     - Повторный import отдаёт ранее инициализированный словарь модуля.

   Alias:
     - Если alias != NULL/"" — словарь модуля также кладётся в переменную alias
       в целевом окружении (параллельно «совместимому» мерджу экспортов).

   Возврат:
     - 1 при успехе, 0 при ошибке.

   Параметры:
     - name        : модуль без суффикса (или путь)
     - alias       : необязательный алиас (имя переменной-словаря)
     - from_dir    : каталог для начала поиска (может быть NULL)
     - into_env    : окружение, куда применяется мердж экспортов
     - out_module_dict : если не NULL — сюда возвращается словарь модуля
*/
int    vm_import (VM* vm, const char* name, const char* alias,
                  const char* from_dir, KirEnv* into_env, Value* out_module_dict);

/* Включить и выполнить другой файл в текущее окружение (без кэша модулей). */
int    vm_include(VM* vm, const char* path, KirEnv* into_env);

#ifdef __cplusplus
}
#endif
