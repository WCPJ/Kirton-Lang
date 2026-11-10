#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
jobs="${KIRTON_STAGE4_JOBS:-2}"
if [[ -n "${KIRTON_STAGE4_BUILD_ROOT:-}" ]]; then
  build_root="$KIRTON_STAGE4_BUILD_ROOT"
  mkdir -p "$build_root"
else
  build_root="$(mktemp -d "${TMPDIR:-/tmp}/kirton-stage4-linux.XXXXXX")"
fi

for compiler in gcc clang; do
  if ! command -v "$compiler" >/dev/null 2>&1; then
    printf 'Required compiler not found: %s\n' "$compiler" >&2
    exit 2
  fi
done
if ! command -v ninja >/dev/null 2>&1; then
  printf 'Ninja is required for clean reproducible builds.\n' >&2
  exit 2
fi

printf 'Repository: %s\nBuild/results root: %s\n' "$repo_root" "$build_root"
{
  gcc --version | sed -n '1p'
  clang --version | sed -n '1p'
  cmake --version | sed -n '1p'
  ninja --version
} | tee "$build_root/tool-versions.txt"

run_configuration(){
  local name="$1"
  local compiler="$2"
  local sanitizer="$3"
  local build_dir="$build_root/$name"

  if [[ -e "$build_dir" ]]; then
    printf 'Refusing to reuse existing build directory: %s\n' "$build_dir" >&2
    exit 2
  fi
  mkdir -p "$build_dir"

  if [[ "$sanitizer" == "on" ]]; then
    cmake -S "$repo_root" -B "$build_dir" -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_COMPILER="$compiler" \
      -DBUILD_TESTING=ON \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
      '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined' \
      '-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined' \
      2>&1 | tee "$build_dir/configure.log"
  else
    cmake -S "$repo_root" -B "$build_dir" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER="$compiler" \
      -DBUILD_TESTING=ON \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      2>&1 | tee "$build_dir/configure.log"
  fi

  cmake --build "$build_dir" --parallel "$jobs" \
    2>&1 | tee "$build_dir/build.log"

  if [[ "$sanitizer" == "on" ]]; then
    # Leak detection is retained for normal tests, including repeated VM teardown.
    # Intentional fail-fast OOM child processes do not unwind by contract, so only
    # those children disable LSan; AddressSanitizer/UBSan remain enabled there.
    KIRTON_OOM_DISABLE_LSAN=1 \
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
    LSAN_OPTIONS="exitcode=23:report_objects=1" \
    UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
      ctest --test-dir "$build_dir" --output-on-failure --parallel "$jobs" \
      2>&1 | tee "$build_dir/ctest.log"
  else
    ctest --test-dir "$build_dir" --output-on-failure --parallel "$jobs" \
      2>&1 | tee "$build_dir/ctest.log"
  fi
}

run_configuration gcc-release gcc off
run_configuration gcc-asan-ubsan gcc on
run_configuration clang-release clang off
run_configuration clang-asan-ubsan clang on

printf '\nAll GCC/Clang Release and ASan+UBSan+LSan configurations passed.\nResults: %s\n' "$build_root"
