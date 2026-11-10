# Kirton Memory System V2.4 — checked zero-copy native borrowing

This increment extends the **V2.3 VM-scoped C API**. It is a non-moving,
**single-threaded** borrow mechanism for contiguous arena-backed objects, not
a full FFI or a Rust-like static borrow checker. The Kirton scripting syntax
is unchanged.

## Public API

Declared in `include/kirton/eval.h`:

```c
KirMemoryBorrow* vm_memory_borrow(VM* vm, Value target, KirBorrowMode mode);
const void* vm_memory_borrow_data(const KirMemoryBorrow* borrow);
void* vm_memory_borrow_mutable_data(const KirMemoryBorrow* borrow);
size_t vm_memory_borrow_size(const KirMemoryBorrow* borrow);
size_t vm_memory_borrow_count(const VM* vm);
int vm_memory_borrow_release(VM* vm, KirMemoryBorrow* borrow);
int vm_try_free(VM* vm);
```

`target` may be `VAL_BUFFER`, `VAL_TYPED_ARRAY`, or `VAL_RECORD`, **owned by
`vm`**. `mode` is `KIR_BORROW_READ` or `KIR_BORROW_WRITE`. On failure, borrow
returns `NULL`; other failure-returning functions return `0`.

A borrow pins its target with the existing VM GC and adds a temporary borrow
counter to its arena. The exported address is direct arena storage, not a copy.
Only **one writer or multiple readers** may borrow an arena at once. Locking
is deliberately at *arena granularity*, including non-overlapping buffers.

### Example: native typed-array processing

```c
KirPinnedValue *pin = vm_value_pin(vm, returned_array);
if (!pin) { /* report error */ }
else {
    KirMemoryBorrow *loan = vm_memory_borrow(
        vm, vm_value_pinned(pin), KIR_BORROW_WRITE);
    if (loan) {
        /* Use memcpy for portable host-native numeric loads/stores. */
        if (vm_memory_borrow_size(loan) >= sizeof(int64_t)) {
            int64_t n = 42;
            memcpy(vm_memory_borrow_mutable_data(loan), &n, sizeof n);
        }
        vm_memory_borrow_release(vm, loan); /* mandatory */
    }
    vm_value_unpin(vm, pin);
}
```

The explicit `pin` is optional *while the borrow itself exists*, but needed if
C will use the `Value` across future GC after releasing the borrow.

## Guaranteed checks during an active borrow

- VM ownership and target type are verified **before dereferencing** an arena
  descriptor; stale generation, closed arena, or out-of-bounds views are rejected.
- During a READ borrow, other READ borrows are permitted; all VM writes,
  new allocations in that arena, reset and close are rejected.
- During a WRITE borrow, other READ/WRITE borrows, VM reads and writes, new
  allocations, reset and close are rejected.
- `vm_gc_collect()` is permitted: the token's GC pin retains the target and
  underlying arena. The address remains stable throughout the borrow.
- `vm_try_free()` returns `0` without destroying a VM that still has borrows.
  The pre-existing `vm_free()` likewise refuses to destroy such a VM and
  writes a diagnostic to `stderr`; **always release borrows before shutdown**.
- Passing a borrow token to `vm_memory_borrow_release()` with a *different VM*
  is rejected without releasing it.
- The token must be released exactly once with the original VM.

## Important limitations

1. **No concurrent threads.** The counters are not atomic. Only synchronous,
   single-threaded C calls are supported; no async GPU/DMA access or callbacks
   retaining addresses beyond the borrow. Copy or synchronize externally.
2. **Unsafe raw C remains unsafe.** After release, the pointer and token must
   never be used. Kirton cannot prevent C code from retaining a pointer or
   casting away `const`. A `Value` held across GC must be pinned.
3. **No field-level or slice-level locks.** Conflict detection is per arena,
   even for disjoint allocations; lock-free concurrent reads/writes and
   fine-grained borrowing are not supported.
4. **Writable zero-copy bypasses numeric validation.** Native writes can
   introduce non-finite `число64` values or arbitrary bytes. The caller must
   respect alignment, host byte order, representation, record offsets and
   type invariants. For checked numeric writes, use the V2.3 copy API.
5. **Not a stable C ABI/FFI.** No dynamic loading, pointer lifetime inference,
   native resource ownership, compiler-enforced lifetimes, or MSVC validation
   is provided yet.
6. The low-level `v_memory_borrow_begin/end` functions in `value.h` are
   internal building blocks. External integrations should use the `vm_*` API.

## Validation

`tests/memory_v2_borrow_api.c` and `tests/memory_v2_borrow_vm.kir` cover:
invalid mode/kind/foreign VM, nested readers, exclusive writers, direct native
writes to all kinds of memory-backed objects, pinning across GC, rejected
arena reset/close, rejected VM destruction, stale generations, empty buffers,
wrong-VM release, and 25,000 repeated borrows with periodic collections.

Linux Release and ASan/UBSan/LeakSanitizer run **56/56** CTest cases, including
this new case. Windows/MSVC and multithreading have **not** been validated.

## Next steps

- Audit V2.4 under Windows/MSVC (alignment, C ABI, compiler warnings).
- Define native resources and callback lifetime/ownership contracts before a
  full FFI; do not pass arena pointers to asynchronous APIs with longer life.
- Add deterministic teardown of user-owned resources, host integration tests,
  and performance measurements versus the V2.3 copy API.
