# Changelog

Notable user-facing changes for Kirton. Older implementation-stage notes are intentionally not part of this release changelog.

## 1.3.0 — 2026-11-10

- Integrated the LLVM Native compiler with the AST interpreter and Bytecode VM.
- Added Native code generation for BigInt, dynamic values, collections, structures and enums, functions and closures, modules, exceptions, and supported standard-library operations.
- Added VM-scoped Native FFI V2/V3 and memory operations. FFI remains opt-in with `--allow-ffi`.
- Fixed GC roots across `finally`, GCC AddressSanitizer metadata-table merging, Windows sound-ID underflow, and installed Native runtime/toolchain lookup.
- Expanded Native differential, FFI, C embedding, Unicode-path, and release-package coverage.

The AST interpreter is the reference execution mode. The Native compiler and C Runtime API remain subject to the limitations documented in [`kirton-c/docs/KNOWN_LIMITATIONS.md`](kirton-c/docs/KNOWN_LIMITATIONS.md).

## 1.2.0 — 2026-11-06

- Stabilized the interpreter, Bytecode VM, and SDK packages for Windows x64 and qualified Ubuntu x86-64 builds.
- Fixed Bytecode VM teardown ownership and aligned supported callable expressions with AST behavior.
- Added OOM fault-injection coverage and documented runtime limits.
- Added portable POSIX tests, release metadata, and package validation.
