# Kirton Compiler 1.3.0 Stable

**Channel:** Stable<br>
**Date:** 2026-11-10<br>
**License:** MPL-2.0

Kirton 1.3.0 integrates the LLVM Native Compiler with the established AST
Interpreter and Bytecode VM. The Native Compiler emits LLVM IR and uses
Clang/LLVM to create machine-code executables. The generated program links the
shared Kirton C Runtime; it does not invoke the CLI or interpret embedded
source at runtime.

## What changed

- Added Native code generation for dynamic values, BigInt, strings, collections,
  structures/enums, functions, closures, modules, exceptions/finally, and
  supported standard-library operations.
- Integrated VM-scoped Native FFI V2/V3 and memory operations. FFI remains
  disabled unless the caller passes `--allow-ffi`.
- Fixed GC rooting through `finally`, typed-array and FFI metadata table
  merging under GCC ASan, Windows sound-ID underflow, installed Native
  Runtime/Clang lookup, and Unicode Windows `KIRTON_PATH` resolution.
- Added Windows and Ubuntu validation matrices, Native/FFI integration tests,
  deterministic property probes, and a reproducible benchmark harness.

## Toolchain requirements

- Windows x64: MSVC, Windows SDK, and external Clang/LLVM 23.1.5 targeting
  `x86_64-pc-windows-msvc`.
- Linux x86-64: external Clang for Native compilation. Validation used Ubuntu
  26.04.1 with GCC 15.2 and Clang 21.1.8.

LLVM/Clang is not bundled in either SDK. FFI plugins are trusted native code
and run in-process. The C Runtime API remains experimental and is not
ABI-stable. Some module-object/alias-rebinding constructs are conservatively
rejected, and exact diagnostic parity is not complete; see
`docs/KNOWN_LIMITATIONS.md`.

## Validation

- Windows x64 clean-source Release, Debug, and MSVC AddressSanitizer CTest
  passed 533/533 in each configuration. The suite includes Native EXE
  differential cases and real FFI DLL V2/V3 calls.
- The installed Windows SDK ZIP passed Native/AST/Bytecode parity, Unicode
  `KIRTON_PATH`, FFI V2/V3, C embedding, and setup EXE install/uninstall smoke.
- Ubuntu 26.04.1 x86-64 GCC Release/Debug, Clang Release, GCC ASan/UBSan/LSan,
  and Clang ASan/UBSan/LSan each passed 520/520 CTest cases. Native `.so` FFI
  V2/V3, installed SDK Native module parity, and eight deterministic 100-case
  property suites passed.
- Independent clean-source builds passed on both platforms. The multi-module
  integration output hash matched across AST, Bytecode, and Native.
- Windows performance was measured on 13 workloads with five runs per mode;
  results are host-specific and make no universal speedup claim.

See `release/1.3.0/COMPATIBILITY_MATRIX.md` for feature-level coverage.
