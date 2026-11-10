# Embedding Kirton in C

## Status

`kirton_core` is currently a static implementation library, not a versioned
stable ABI. The installed `include/kirton/` headers expose implementation-level
AST and `Value` representations as well as VM APIs. Treat this as an
experimental source-level C API; rebuild the host when updating Kirton.

## Build and link

Build the project with CMake and link `lib/kirton.lib` (MSVC) or
`lib/libkirton.a` (Linux), using the headers under `include/`. Consumers must
also link the platform libraries shown by the project CMake configuration:
Windows uses WinHTTP, User32, GDI, WinMM, OpenGL, D3D11, DXGI, D3DCompiler,
Windows Imaging Component, OLE, and DXGUID. Linux links `m` and `dl` where
available. The compiler and C runtime must be compatible with the library build.

## VM lifecycle

`vm_new()` creates a VM; `vm_free()` destroys it. Prefer `vm_try_free()` and
check its result when using zero-copy memory borrows or native-resource leases;
failed destruction leaves the VM alive. For file-based execution,
`vm_run_root()` executes the file and returns its root environment. Lower-level
parse/evaluation functions are exposed too, but require the caller to provide
an environment; no public environment constructor exists yet.

Values owned by a VM become invalid when that VM is destroyed. Values retained
across collection must be pinned with `vm_value_pin()` and released with
`vm_value_unpin()`. Borrowed pointers are valid only for their borrow token.
Native resource handles transfer ownership only when adoption succeeds; see
the detailed contracts in `include/kirton/eval.h`.

VM calls and native resource APIs are single-threaded. Do not call one VM
concurrently. FFI loads trusted in-process code and may crash the host; it is
disabled by default. The evaluator API does not currently promise stable ABI,
symbol versioning, or binary compatibility.

## Minimal host pattern

```c
#include <stdio.h>
#include "kirton/eval.h"

int main(int argc, char **argv) {
    if (argc != 2) return 64;
    VM *vm = vm_new();
    if (!vm) return 70;
    KirEnv *root = vm_run_root(vm, argv[1]);
    int freed = vm_try_free(vm);
    return root && freed ? 0 : 1;
}
```

Compile against the exact SDK headers and static library, then pass a `.kir`
file path to the host executable. The header API is broad and evolving; verify
an embedding against each SDK build before shipping.
