#!/usr/bin/env bash
set -euo pipefail

source_dir="$GITHUB_WORKSPACE/source"
expected_commit=e191d6b7a816be5f49624c80a63baa3c188db730
actual_commit="$(git -C "$source_dir" rev-parse HEAD)"
[[ "$actual_commit" == "$expected_commit" ]]
[[ "$(uname -m)" == x86_64 ]]
export LC_ALL=C.UTF-8

root="$RUNNER_TEMP/kirton-release"
export KIRTON_BUILD_DIR="$root/build"
export KIRTON_STAGE_DIR="$root/stage"
export KIRTON_DIST_DIR="$root/dist"
export KIRTON_RELEASE_CHANNEL=Stable
mkdir -p "$root"

# This script builds and runs the complete registered CTest suite.
bash "$source_dir/kirton-c/scripts/build-linux.sh"

sdk_name=Kirton-SDK-1.3.0-linux-x86_64
archive="$KIRTON_DIST_DIR/$sdk_name.tar.gz"
mkdir -p "$root/smoke"
tar -xzf "$archive" -C "$root/smoke"
sdk="$root/smoke/$sdk_name"
cli="$sdk/bin/kirton"
test -x "$cli"
test -f "$sdk/lib/libkirton.a"
test -f "$sdk/include/kirton/eval.h"
test -f "$sdk/examples/hello.kir"
test -f "$sdk/docs/GETTING_STARTED.md"
test -f "$sdk/LICENSE"
test -f "$sdk/THIRD_PARTY_NOTICES.md"
"$cli" --version | grep -F 'Kirton 1.3.0'
ldd "$cli" | tee "$root/ldd.txt"
if grep -Fq 'not found' "$root/ldd.txt"; then exit 1; fi

program="$source_dir/kirton-c/tests/native_compiler_foundation.kir"
ast="$("$cli" "$program")"
bytecode="$("$cli" --bytecode "$program")"
"$cli" --native "$program" -o "$root/smoke/native-foundation"
file -b "$root/smoke/native-foundation" | grep -E 'ELF 64-bit.*x86-64'
native="$("$root/smoke/native-foundation")"
[[ "$ast" == 42 && "$ast" == "$bytecode" && "$ast" == "$native" ]]

for version in v2 v3; do
  ffi_source="$KIRTON_BUILD_DIR/ffi_${version}_cli_Release.kir"
  test -f "$ffi_source"
  ast="$("$cli" --allow-ffi "$ffi_source")"
  bytecode="$("$cli" --allow-ffi --bytecode "$ffi_source")"
  "$cli" --allow-ffi --native "$ffi_source" -o "$root/smoke/ffi-$version"
  file -b "$root/smoke/ffi-$version" | grep -E 'ELF 64-bit.*x86-64'
  native="$("$root/smoke/ffi-$version")"
  [[ "$ast" == "$bytecode" && "$ast" == "$native" ]]
  if [[ "$version" == v2 ]]; then [[ "$ast" == FFI_CLI_OK ]]; else [[ "$ast" == FFI_V3_CLI_OK ]]; fi
done

cd "$KIRTON_DIST_DIR"
sha256sum -c "$sdk_name.tar.gz.sha256"
printf '%s  %s\n' 'bd76302d4f00b6fc4bc65ca40b569cdaa5bee96e8a79d1016229c8366f8aa0b2' 'Kirton-Compiler-1.3.0-windows-x64.zip' > SHA256SUMS
sha256sum "$sdk_name.tar.gz" >> SHA256SUMS
printf '%s\n' "$actual_commit" > SOURCE_COMMIT
echo 'Linux SDK extraction, AST, Bytecode, Native ELF and FFI V2/V3: PASS'
