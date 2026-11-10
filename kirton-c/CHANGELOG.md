# Changelog

## 1.3.0 Stable — 2026-11-10

- Integrated the LLVM Native Compiler with the AST Interpreter and Bytecode VM.
- Added Native BigInt, dynamic values, collections, structures/enums, closures,
  module linking, exceptions/finally, and opt-in FFI V2/V3.
- Fixed GC roots through `finally`, GCC-ASan typed-array/FFI metadata table
  merging, sound-ID signed underflow, and Native SDK Runtime lookup.
- Added Windows and Ubuntu validation matrices, Native/FFI integration tests,
  deterministic property probes, and a reproducible benchmark harness.

## 1.2.0 Stable — 2026-11-06

- Stabilization across Windows and a qualified Ubuntu 26.04 x86_64 Linux host.
- Fixed Bytecode VM teardown ownership and aligned AST/Bytecode calls for
  supported callable expressions.
- Added local OOM fault-injection handling and documented fail-fast limits.
- Added portable POSIX test setup and corrected a test-only leak found by LSan.
- Added Kirton 1.2 user guide, release metadata, and reproducible package checks.

## 1.2.0 Preview

See `docs/KIRTON_COMPILER_1_2_PREVIEW.md` for historical preview notes.
