# Kirton Memory System V2.3 — VM-scoped native memory access

The V2.3 increment adds VM-bound **C ABI entry points** in `include/kirton/eval.h` and `src/eval.c`:

- `vm_memory_buffer_read` / `vm_memory_buffer_write`: copy byte spans without exporting raw arena addresses; validate VM ownership, generation, `offset`, length and null pointers before copying.
- `vm_memory_array_get` / `vm_memory_array_set`: checked numeric element access.
- `vm_memory_record_get` / `vm_memory_record_set`: checked field access for existing native-layout scalar records.

All return `1` on success, `0` on failure; they do not throw exceptions. Zero-length copies accept null data pointers if the offset is within the live buffer. The buffer-copy API is synchronous, byte-by-byte, and currently O(n); it is intended as the **safe foundation**, not a high-throughput FFI interface. No raw pointers are returned.

## Lifetime and ownership contract

1. A VM must be alive for every `vm_memory_*` operation.
2. Every arena-backed handle must belong to the VM passed to the operation. A handle from another VM is rejected.
3. A handle becomes invalid after closing/resetting its arena. It must never be used again, including through the C API.
4. C code **must pin** any returned `Value` that needs to survive future Kirton execution / GC with `vm_value_pin(vm, value)`; access via `vm_value_pinned(pin)` and release with `vm_value_unpin(vm, pin)`. `vm_free` invalidates all pinned handles.
5. **This API cannot make stale raw C `Value` or `KirPinnedValue*` safe** after GC/unpin/free. Never dereference or reuse a stale pointer; invalid raw pointers remain undefined behavior in C. No cross-thread access or concurrent VM mutation is supported.
6. The ABI depends on the program being compiled against the same Kirton headers and binary; this is a C-level integration layer, **not** dynamic library loading / a stabilized FFI or a promise of stable binary ABI.

## Example

```c
KirPinnedValue *pin = vm_value_pin(vm, returned_buffer);
if (pin) {
    unsigned char bytes[4] = {1, 2, 3, 4};
    int ok = vm_memory_buffer_write(vm, vm_value_pinned(pin), 0, bytes, sizeof bytes);
    vm_value_unpin(vm, pin);
}
```

## Validation

`tests/memory_v2_native_api.c` exercises VM isolation, 64-bit integers, out-of-bounds and overflow-safe copy checks, null pointers, pinning across GC, and post-unpin collection. All 55 CTest tests passed in Release and under ASan/UBSan/LeakSanitizer on the tested Linux environment.

## Still missing

- Native callback registration with explicit per-VM context and cancellation/error propagation.
- Borrowed zero-copy native access (requires explicit borrow lifetime rules and no GC/arena-reset during borrow).
- Cross-platform ABI validation, Windows/MSVC, C++/C foreign-library integration, pointer types, FFI library loading, thread safety.
- Error objects/exceptions at the Kirton language level and a formal type/lifetime specification.
