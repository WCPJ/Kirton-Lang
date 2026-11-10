#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public License v. 2.0. See https://mozilla.org/MPL/2.0.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPORT="${KIRTON_REPORT:-$ROOT/kirton-gentoo-report.txt}"
exec > >(tee "$REPORT") 2>&1

echo "Kirton Gentoo/Linux qualification report"
date -u '+UTC: %Y-%m-%d %H:%M:%S'
echo "Kernel: $(uname -sr)"
echo "Architecture: $(uname -m)"
echo "CPU: $(awk -F: '/model name/ {gsub(/^ /, "", $2); print $2; exit}' /proc/cpuinfo 2>/dev/null || true)"
echo "OS release:"
cat /etc/os-release 2>/dev/null || true
OS_ID="$(. /etc/os-release 2>/dev/null; echo "${ID:-unknown}")"
if [[ "$OS_ID" == "gentoo" ]]; then
  echo "Gentoo host check: PASS (ID=gentoo)"
else
  echo "Gentoo host check: BLOCKED (this report is from ID=$OS_ID, not Gentoo)"
fi
echo "CMake: $(cmake --version 2>&1 | head -n 1)"
echo "libc:"
if command -v ldd >/dev/null; then ldd --version 2>&1 | head -n 2; else echo "ldd unavailable"; fi

run_lane() {
  local compiler="$1" name="$2" dir="$ROOT/build-gentoo-$2"
  if ! command -v "$compiler" >/dev/null 2>&1; then
    echo "LANE $name: BLOCKED (compiler $compiler unavailable)"
    return
  fi
  echo "LANE $name compiler: $($compiler --version | head -n 1)"
  if CC="$compiler" cmake -S "$ROOT" -B "$dir" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON &&
     cmake --build "$dir" --parallel; then
    if ctest --test-dir "$dir" --output-on-failure; then
      echo "LANE $name build+CTest: PASS"
    else
      echo "LANE $name build: PASS; CTest: FAIL"
    fi
  else
    echo "LANE $name build: FAIL"
  fi
}

run_lane gcc gcc
run_lane clang clang

if command -v clang >/dev/null 2>&1; then
  SAN="$ROOT/build-gentoo-sanitize"
  if CC=clang cmake -S "$ROOT" -B "$SAN" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' &&
     cmake --build "$SAN" --parallel && ctest --test-dir "$SAN" --output-on-failure; then
    echo "ASan/UBSan: PASS"
  else
    echo "ASan/UBSan: FAIL"
  fi
else
  echo "ASan/UBSan: BLOCKED (Clang unavailable)"
fi

echo "Report written to: $REPORT"
