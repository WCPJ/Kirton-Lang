# Kirton 1.3.0 Native Compiler Guide (Release Candidate)

This guide describes the 1.3.0 Release Candidate. The release gate remains open
until both SDK packages and their installed applications pass validation.

## Execution modes

- **AST Interpreter:** reference execution mode.
- **Bytecode VM:** select with `--bytecode`.
- **Native Compiler:** lowers the parsed program to LLVM IR, then asks Clang to
  emit and link a real platform executable. It does not embed `.kir` source or
  fall back to AST/Bytecode when it encounters an unsupported construct.

```sh
kirton program.kir
kirton --bytecode program.kir
kirton --emit-llvm program.kir -o program.ll
kirton --native program.kir -o program
./program
```

On Windows, use `-o program.exe` and run `program.exe`. Native executables link
the Kirton C Runtime into the generated program and do not require
`kirton.exe` at runtime. The runtime archive is located next to the installed
SDK; the build-tree path is only a development fallback.

## Toolchain requirements

Building the Kirton CLI requires CMake 3.15+, a C11 compiler, and Make or Ninja.
Native code generation additionally requires Clang/LLVM and a platform linker.

- Windows x64: MSVC, Windows SDK, and Clang targeting
  `x86_64-pc-windows-msvc`. The validated compiler is LLVM/Clang 23.1.5. CMake
  searches `%LOCALAPPDATA%\Kirton\LLVM-23.1.5\bin`; at runtime the CLI
  searches that per-user location and PATH.
- Linux x86-64: GCC or Clang to build Kirton, plus Clang on PATH for Native
  applications. Validation used Ubuntu 26.04.1, GCC 15.2, and Clang 21.1.8.

The SDK does not redistribute LLVM/Clang. Install a compatible toolchain before
using `--native`. CMake reports when Clang is not found; the CLI then rejects
Native compilation and produces no executable.

## Multi-file programs

Keep user modules alongside the importing source or in a module search
directory. Use UTF-8 source and explicit aliases:

```kirton
импорт "lib/math" как math
печатать(math.сложить(20, 22))
```

```sh
kirton main.kir
kirton --bytecode main.kir
kirton --native main.kir -o main
```

Native module linking expands supported user modules into the generated
executable and preserves module caching and shared state. Some module-object
and alias-rebinding patterns are conservatively rejected; see
[KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md) and the
[1.3.0 compatibility matrix](../../release/1.3.0-rc/COMPATIBILITY_MATRIX.md).

## Collections, closures, structures, and exceptions

The RC includes differential coverage for BigInt, strings/Unicode, lists,
dictionaries, structures, enums, higher-order functions, mutable closures,
modules, and `try`/`catch`/`finally`. The compatibility matrix documents which
cases were executed on each backend. Passing those cases does not claim
complete observational equivalence for every program or standard-library
operation.

## FFI

FFI loads trusted native libraries into the current process. It is disabled by
default. Enable it explicitly when running or compiling code:

```sh
kirton --allow-ffi app.kir
kirton --native --allow-ffi app.kir -o app
```

Kirton supports the declared FFI V2 and V3 plugin ABIs. Do not treat an
arbitrary C library as compatible without matching its calling convention,
argument types, ownership rules, and ABI descriptor. FFI code runs with the
host process's permissions and can corrupt or terminate that process.

## Remaining limits

The C API remains experimental and is not ABI stable. OOM is not universally
recoverable. Native diagnostics do not yet match AST/Bytecode formatting and
source locations in every case. Windows graphics/device behavior is not
covered by the headless suite; some POSIX modules remain unsupported. The
current release gate and platform evidence are in
[`release/1.3.0-rc/STATUS_REPORT.md`](../../release/1.3.0-rc/STATUS_REPORT.md).
