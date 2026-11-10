#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public License v. 2.0. See https://mozilla.org/MPL/2.0.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(cat "$ROOT/VERSION")"
RELEASE_CHANNEL="${KIRTON_RELEASE_CHANNEL:-Release Candidate}"
ARCH="$(uname -m)"
if [[ "$ARCH" != "x86_64" ]]; then
  echo "Expected x86_64; found $ARCH" >&2
  exit 2
fi
command -v cmake >/dev/null || { echo "cmake is required" >&2; exit 2; }
command -v tar >/dev/null || { echo "tar is required" >&2; exit 2; }

BUILD_DIR="${KIRTON_BUILD_DIR:-$ROOT/build-linux-release}"
STAGE="${KIRTON_STAGE_DIR:-$ROOT/dist/linux-stage}"
DIST="$ROOT/dist"
if [[ -n "${KIRTON_DIST_DIR:-}" ]]; then
  DIST="$KIRTON_DIST_DIR"
fi
SDK="Kirton-SDK-${VERSION}-linux-x86_64"
mkdir -p "$DIST"

for path in "$STAGE" "$DIST/$SDK" "$DIST/$SDK.tar.gz" "$DIST/${SDK}.tar.gz.sha256" \
            "$DIST/Kirton-${VERSION}-source.tar.gz" "$DIST/Kirton-${VERSION}-source.tar.gz.sha256"; do
  if [[ -e "$path" ]]; then
    echo "Refusing to overwrite existing SDK build/package path: $path" >&2
    exit 2
  fi
done
mkdir -p "$BUILD_DIR"
mkdir -p "$STAGE"
if [[ -n "$(find "$STAGE" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
  echo "Install staging directory must be empty: $STAGE" >&2
  exit 2
fi

cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
  -DKIRTON_RUNTIME_LIBRARY_PATH= -DKIRTON_CLANG_RUNTIME_PATH= -DBUILD_TESTING=ON
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure

cmake --install "$BUILD_DIR" --prefix "$STAGE"
mkdir -p "$DIST/$SDK/bin" "$DIST/$SDK/include" "$DIST/$SDK/lib" "$DIST/$SDK/examples" "$DIST/$SDK/docs"
cp "$STAGE/bin/kirton" "$DIST/$SDK/bin/"
cp -a "$STAGE/include/kirton" "$DIST/$SDK/include/"
cp "$STAGE/lib/"*kirton* "$DIST/$SDK/lib/"
cp -a "$STAGE/share/kirton/examples/." "$DIST/$SDK/examples/"
cp "$ROOT/examples/embedding/host.c" "$DIST/$SDK/examples/embedding/"
for doc in GETTING_STARTED.md LANGUAGE_REFERENCE.md CLI_REFERENCE.md EMBEDDING.md LICENSING.md \
           KNOWN_LIMITATIONS.md GENTOO.md SYNTAX_V1.md CONFORMANCE_V1.md \
           RELEASE_GATE_V1.md PLATFORMS_V1.md; do
  cp "$ROOT/docs/$doc" "$DIST/$SDK/docs/"
done
cp "$ROOT/docs/KIRTON_1_3_GUIDE.md" "$ROOT/docs/KIRTON_OOM_CONTRACT.md" "$DIST/$SDK/docs/"
cp "$ROOT/README.md" "$ROOT/VERSION" "$ROOT/LICENSE" "$ROOT/LICENSE_STATUS.md" \
  "$ROOT/THIRD_PARTY_NOTICES.md" "$ROOT/CHANGELOG.md" \
  "$ROOT/RELEASE_NOTES_${VERSION}.md" "$DIST/$SDK/"
printf '%s\n' 'WorldCraft Studio' > "$DIST/$SDK/PUBLISHER.txt"
printf '%s\n' "$RELEASE_CHANNEL" > "$DIST/$SDK/RELEASE_CHANNEL.txt"
{
  echo "Architecture: $ARCH"
  echo "Kernel: $(uname -sr)"
  echo "Compiler: $(${CC:-cc} --version | head -n 1)"
  echo "CMake: $(cmake --version | head -n 1)"
  if command -v ldd >/dev/null; then ldd --version 2>&1 | head -n 1; fi
  if command -v ldd >/dev/null; then ldd "$DIST/$SDK/bin/kirton" || true; fi
} > "$DIST/$SDK/COMPATIBILITY.txt"

tar -C "$DIST" -czf "$DIST/$SDK.tar.gz" "$SDK"
sha256sum "$DIST/$SDK.tar.gz" | tee "$DIST/$SDK.tar.gz.sha256"

SRC="Kirton-${VERSION}-source"
ROOT_NAME="$(basename "$ROOT")"
tar -C "$(dirname "$ROOT")" \
  --exclude="$ROOT_NAME/build" --exclude="$ROOT_NAME/build-*" \
  --exclude="$ROOT_NAME/dist" --exclude="$ROOT_NAME/.git" \
  --exclude="$ROOT_NAME/CMakeFiles" \
  --exclude='*.exe' --exclude='*.dll' --exclude='*.obj' --exclude='*.pdb' \
  --exclude='*.lib' --exclude='*.a' --exclude='*.so' --exclude='*.o' \
  -czf "$DIST/$SRC.tar.gz" "$ROOT_NAME"
sha256sum "$DIST/$SRC.tar.gz" | tee "$DIST/$SRC.tar.gz.sha256"
echo "Created $DIST/$SDK.tar.gz and $DIST/$SRC.tar.gz"
