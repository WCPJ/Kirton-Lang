# Kirton Compiler 1.3.0 Stable

Kirton is a UTF-8 scripting language with an AST Interpreter, Bytecode VM, and
LLVM Native Compiler. Programs use `.kir` files. Version 1.3.0 is a Stable
release for the tested Windows x64 and Ubuntu 26.04 x86-64 configurations.

## Install

Download the Windows x64 SDK archive `Kirton-Compiler-1.3.0-windows-x64.zip` or
the Linux x86-64 archive `Kirton-SDK-1.3.0-linux-x86_64.tar.gz` from the GitHub
release. Verify the archive with the `SHA256SUMS` file attached to that release.
The Windows setup executable is an optional convenience installer; the SDK ZIP
can be used without installing software or changing system settings.

Native code generation requires LLVM/Clang in addition to the C Runtime archive.
The Windows build was validated with LLVM/Clang 23.1.5 for
`x86_64-pc-windows-msvc`; the CLI searches
`%LOCALAPPDATA%\Kirton\LLVM-23.1.5\bin\clang.exe` and then `PATH`. Linux was
validated with Ubuntu 26.04 x86-64, GCC 15.2, and Clang 21.1.8. The SDK does
not bundle LLVM/Clang; install a compatible Clang toolchain to build Native
applications.

## Run a program

```kirton
печатать("Привет, Kirton!")
```

Save as `hello.kir`, then run `kirton hello.kir`. See
[Getting Started](docs/GETTING_STARTED.md) and the
[Kirton 1.3 guide](docs/KIRTON_1_3_GUIDE.md).

## Execution modes

```sh
kirton program.kir
kirton --bytecode program.kir
kirton --emit-llvm program.kir -o program.ll
kirton --native program.kir -o program
./program
```

Native compilation emits machine code and links the shared Kirton C Runtime;
the resulting program does not require `kirton.exe` or the `kirton` CLI at
runtime. The Native compiler rejects unsupported constructs instead of falling
back to AST or Bytecode. FFI is disabled by default; compile trusted FFI code
with `--native --allow-ffi`.

AST is the reference implementation. Bytecode and Native have broad
differential coverage, but the release does not claim complete observational
equivalence for every standard library, diagnostic, or native-resource path.
The C Runtime API remains experimental and is not ABI stable. OOM can terminate
a Runtime operation; embedded hosts must not assume recovery or process
isolation. See [known limitations](docs/KNOWN_LIMITATIONS.md),
[the OOM contract](docs/KIRTON_OOM_CONTRACT.md), and
`RELEASE_NOTES_1.3.0.md`.

## Build from source

Requirements: CMake 3.15+, a C11 compiler, and Make or Ninja. Native executable
tests additionally require Clang.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix ./install
```

## License

Kirton is distributed under the Mozilla Public License 2.0. See `LICENSE`,
`LICENSE_STATUS.md`, and `THIRD_PARTY_NOTICES.md`.
