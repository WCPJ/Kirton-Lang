# Build Kirton from source

The CMake project is in `kirton-c/`. Kirton is written in C11.

## Requirements

- CMake 3.15 or newer.
- A C11 compiler and the platform's native SDK/toolchain.
- Clang/LLVM to enable the Native compiler executable tests. Without Clang, CMake still builds the interpreter and Bytecode VM, but Native execution tests are not registered.

## Windows x64

Install Visual Studio 2022 (or its Build Tools) with the Desktop development with C++ workload and a Windows SDK. Install a compatible LLVM/Clang separately for Native compiler tests.

From PowerShell at the repository root:

```powershell
cmake -S kirton-c -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure --parallel 2
cmake --install build --config Release --prefix install
```

The executable is `build\Release\kirton.exe`. Check it with:

```powershell
.\build\Release\kirton.exe --version
```

## Linux x86-64

On Debian or Ubuntu, install the compiler and build tools:

```sh
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build clang
```

Configure, build, test, and install:

```sh
cmake -S kirton-c -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --parallel 2
cmake --install build --prefix "$PWD/install"
```

The executable is `build/kirton`. Run `./build/kirton --version` to check it.

## Optional Native compilation

The Native compiler needs an external Clang/LLVM installation compatible with the target platform. Kirton does not bundle LLVM. CMake discovers Clang from `PATH`; on Windows it also checks `%LOCALAPPDATA%\Kirton\LLVM-23.1.5\bin`.

```sh
kirton --emit-llvm hello.kir -o hello.ll
kirton --native hello.kir -o hello
```

For the complete command-line behavior and feature limitations, see [`kirton-c/docs/CLI_REFERENCE.md`](kirton-c/docs/CLI_REFERENCE.md) and [`kirton-c/docs/KNOWN_LIMITATIONS.md`](kirton-c/docs/KNOWN_LIMITATIONS.md).
