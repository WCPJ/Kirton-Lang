# Kirton Compiler 1.2.0 Stable

Release date: 2026-11-06<br>
Publisher: WorldCraft Studio<br>
License: MPL-2.0

Kirton 1.2.0 packages the stabilized interpreter, Bytecode compiler and VM,
standard runtime modules, C headers/library, examples, and documentation.

## Verified configurations

- Windows x64/MSVC: Release, Debug, and AddressSanitizer; 217/217 CTest tests
  per configuration. Stage 4's 85-module smoke checks also passed.
- Linux x86_64 / Ubuntu 26.04.1: GCC 15.2 and Clang 21.1.8 Release, plus
  ASan+UBSan+LSan for both compilers; 204/204 CTest tests in each run.
  This is a verified build host, not a blanket Linux distro compatibility
  claim. Linux package smoke results and checksums are recorded in the release
  manifest.

## Known limits

- AST is the reference mode. Bytecode has extensive differential coverage but
  is not claimed to be fully equivalent for every native module/resource path.
- The public C API is experimental and has no ABI stability promise. Runtime
  OOM paths are not universally recoverable; an embedded caller must treat
  allocation failure as potentially fatal and must not assume the Runtime can
  continue.
- Windows graphics/device behavior is not covered by headless tests. Linux
  does not build the Windows window, clipboard, OpenGL, or Direct3D modules;
  POSIX HTTP and some process options remain unsupported.
- Windows setup and SDK artifacts are unsigned. Windows binaries require the
  Microsoft Visual C++ runtime; the setup UI uses .NET Framework 4.x.

See `KIRTON_1_2_RELEASE_READINESS.md` for the full validation evidence and
`KIRTON_OOM_CONTRACT.md` for OOM behavior.
