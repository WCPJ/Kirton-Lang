# Known limitations

- The release build is verified on Windows x64/MSVC and on Ubuntu 26.04.1
  x86_64 with GCC 15.2 and Clang 21.1.8. This does not qualify every Linux
  distribution, libc, or toolchain version. Gentoo, musl, and minimum glibc
  compatibility have not been tested.
- The POSIX HTTP native reports unsupported; POSIX process working-directory
  and timeout options are not implemented.
- The C API is experimental, source-level and not ABI-stable. Public headers
  include internal structures. One VM is single-threaded; native resource
  borrows and leases have strict lifetimes.
- Loading FFI libraries executes trusted native code in-process and can crash
  or compromise the host. FFI is opt-in.
- Windows window, OpenGL, Direct3D, audio, and real GPU/device behavior are not
  validated by the headless conformance suite. Linux has no X11/Wayland
  graphics backend in the current build.
- The Windows binary relies on the Microsoft Visual C++ runtime. The SDK does
  not redistribute the runtime; install the official Microsoft Visual C++
  v14 Redistributable if the target machine lacks it. See Microsoft's
  [supported downloads](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
- The graphical Setup EXE uses the .NET Framework 4.x runtime. Supported
  Windows 10/11 releases include a compatible .NET Framework version; older or
  customized Windows images may require the official runtime separately. See
  Microsoft's [.NET Framework installation guide](https://learn.microsoft.com/dotnet/framework/install/).
- The public C API remains experimental and is not ABI stable. OOM is not
  universally recoverable, and embedded applications must not assume a Runtime
  can safely continue after arbitrary allocation failure. See the OOM contract.
- AST is the reference execution mode. Bytecode has broad regression and
  differential coverage, but complete equivalence across all native modules,
  FFI, and native resource lifetimes is not promised.
- Native compilation is validated for the cases in the 1.3.0 compatibility
  matrix. Some module-object and alias-rebinding patterns are conservatively
  rejected; diagnostic formatting and source locations do not fully match
  AST/Bytecode. Unsupported Native constructs fail compilation rather than
  falling back.
- Native compilation requires external Clang/LLVM. Windows x64 was validated
  with LLVM/Clang 23.1.5 targeting MSVC; Ubuntu 26.04 x86-64 was validated with
  Clang 21.1.8. The SDK does not redistribute LLVM/Clang.
