# Kirton Compiler 1.2 Preview 1

**Channel:** Preview, Windows x64, MSVC. This is not a Stable release. The
AST interpreter remains the default execution mode. Bytecode support is
experimental and limited to the forms listed below.

## What Bytecode does

Bytecode compiles Kirton source into an internal instruction representation and
executes those instructions on Kirton's virtual machine. This is not native
compilation to machine code. The package includes both the AST interpreter and
the Bytecode compiler/VM; use `--bytecode` to select the latter.

## Supported and checked

The current Preview checks cover Kirton 1.1 literals and values, the listed
arithmetic/logical/bitwise operators, variables and assignment, lists and
dictionaries, indexing and mutation, loops, direct named calls, closures,
structures/enums, user modules/import/include, tested exception/finally paths,
GC, and selected Runtime modules. Native callbacks are enabled through an
explicit allowlist.

On Windows x64 the checked Runtime surface includes the listed filesystem CRUD
operations, environment set/readback, monotonic time, timer operations,
process echo/exit behavior, system result shape, local HTTP GET/POST/response
and error handling, generic resource lifecycle, and FFI V2/V3 fixtures. FFI
requires explicit `--allow-ffi` and loads trusted native code into the host
process.

## Unsupported and not checked

- Computed-callee calls such as `factory()(...)` and `table[key](...)` are not
  specified by the normative Kirton 1.1 call rules. The parser can construct
  these AST shapes, but both AST and Bytecode execution do not support them.
  Preview does not add this language behavior.
- The overall HTTP timeout argument does not enforce an end-to-end deadline.
  GET/POST and listed response/error cases are checked; timeout is not a
  readiness guarantee.
- Recoverable parser/AST allocation failure is unsupported. Parser OOM currently
  exits through a fail-fast diagnostic. Do not treat execution of untrusted
  programs inside a long-lived host process as safe.
- TCP/UDP APIs and persistent `.kbc` bytecode files are not part of this
  language/package surface.
- Linux package-manager behavior, broad third-party FFI compatibility,
  cross-platform paths, audio device output, clipboard writes, interactive
  input, broad image corpora, and visual rendering beyond the named OpenGL
  lifecycle checks are not verified.
- The C API is experimental, source-level, and not ABI-stable. VM instances are
  single-threaded and values cannot outlive their owning VM.

See `docs/COMPILER_COMPATIBILITY_1_2.md` for the capability-by-capability
status and `docs/KNOWN_LIMITATIONS.md` for platform and host limitations.

## Install

Run `Kirton-Compiler-1.2.0-preview.1-windows-x64-setup-ru-en.exe`, review and accept
the included MPL-2.0 license, choose an install directory, and choose whether
to add this Preview's `bin` directory to the current user's PATH. The default
directory and uninstall registration are Preview-specific; stable SDK 1.0/1.1
files are kept separate. Use the included uninstall shortcut or run
`Uninstall-Kirton.ps1` from the install directory. The uninstaller removes only
files recorded by its install manifest and removes only the PATH entry added
by this Preview.

Alternatively, extract
`Kirton-Compiler-1.2.0-preview.1-windows-x64.zip` to a directory and invoke
`bin\kirton.exe` directly.

The source archive can be rebuilt with CMake using
`cmake -S . -B build -DBUILD_TESTING=OFF` followed by
`cmake --build build --config Release`. The packaged `VERSION` file supplies
the Preview label for a rebuilt CLI. C sources, tests, CMake files, and the
installer's C#/PowerShell sources are included in the archive.

## Run programs

```powershell
kirton --version
kirton path\to\program.kir
kirton --bytecode path\to\program.kir
kirton --allow-ffi path\to\trusted_ffi_program.kir
```

AST mode is the default. Bytecode mode is explicitly opt-in per invocation.
`--allow-ffi` is also explicit and does not sandbox native libraries.

## Use the C API

Include headers from `include\kirton`, link `lib\kirton.lib`, and link the
Windows libraries documented by the SDK CMake target. The package includes
`examples\embedding\host.c` and its Kirton script. Build the example with the
included CMake project or use it as a starting point; check every VM creation,
parse/run, and cleanup result. See `docs\EMBEDDING.md` for ownership, pinning,
borrow, and lease rules. The static library exposes the AST embedding API; the
CLI exposes Bytecode through `--bytecode`.

## License and Preview status

Kirton source and headers identify MPL-2.0; the complete `LICENSE`,
`LICENSE_STATUS.md`, and `THIRD_PARTY_NOTICES.md` are included. `stb_image.h`
retains its existing public-domain notice. This Preview is a validation
artifact, not a Stable SDK 1.2 release and not a promise of a stable C ABI.
