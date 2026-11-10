# Linux and Gentoo build

Linux support is experimental until a named Linux host completes the full
build and test gate. The core/CLI CMake configuration does not require a
graphics SDK and does not hard-code distribution paths. The current Linux
build does not supply X11 or Wayland backends.

## Dependencies

Required: CMake 3.15 or newer, a C11 compiler (GCC or Clang), a build tool
supported by CMake (Make or Ninja), and the system C runtime/development
headers. Linux linking uses `libm` and the platform dynamic-loader library when
provided by CMake. No package-manager commands are required by the build.

On Gentoo, install those tools through the local Portage configuration as
appropriate for the host. Custom `CFLAGS`, `LDFLAGS`, compiler wrappers, and
multilib settings are passed through CMake's normal toolchain/environment
mechanisms. glibc and musl have not both been verified; do not infer musl
support from a successful glibc build.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/install"
```

For GCC and Clang coverage, configure separate build directories with
`CC=gcc` and `CC=clang`. Use `scripts/test-gentoo.sh` to capture host details,
build/test results, and an optional ASan/UBSan run in a diagnostic report.
Review the full output; a failed or skipped compiler lane is not a pass.

The test suite is currently broader on Windows than on POSIX. In particular,
the POSIX HTTP module is not implemented and may cause Linux tests to fail. Such
results must be reported, not suppressed, before Linux stability is claimed.

## Source archive and Linux SDK

Run `scripts/build-linux.sh` on the target Linux system. It performs a Release
build and CTest, installs into a clean staging directory, and produces a
platform-specific `.tar.gz` plus SHA-256. It also creates a clean source
archive. The binary SDK states the detected libc and build host; it is not a
universal Linux binary. The archive is not considered release-cleared until
the project license is selected.

## Gentoo diagnostic submission

Run:

```sh
bash scripts/test-gentoo.sh
```

The script writes `kirton-gentoo-report.txt` in the project directory. Send
that report together with the exact source revision and any failed test output
to the Kirton maintainer. It records the kernel, CPU architecture, compiler,
libc, CMake, build, CTest, and sanitizer results. No Gentoo build is claimed
until this report is produced on an actual Gentoo host.
