#!/usr/bin/env bash
# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

check_dir=$(mktemp -d)
trap 'rm -rf "$check_dir"' EXIT
source_dir=$(cd "$(dirname "$0")" && pwd)
flags=(-std=c++26 -g -O1 -march=x86-64-v2 -mtune=generic
       -fopenmp -static-libstdc++ -static-libgcc -Wl,--as-needed)
g++ "${flags[@]}" "$source_dir/check-toolchain.cpp" \
  -Wl,-Bstatic -lgomp -Wl,-Bdynamic -o "$check_dir/normal"
g++ "${flags[@]}" -fsanitize=address -static-libasan -fno-omit-frame-pointer \
  "$source_dir/check-toolchain.cpp" \
  -Wl,-Bstatic -lgomp -Wl,-Bdynamic -o "$check_dir/asan"

for executable in "$check_dir/normal" "$check_dir/asan"; do
  OMP_NUM_THREADS=2 "$executable"
  readelf --version-info "$executable" > "$check_dir/versions"
  readelf --dynamic "$executable" > "$check_dir/dynamic"
  python3 - "$check_dir/versions" "$check_dir/dynamic" <<'PY'
import pathlib, re, sys
versions = pathlib.Path(sys.argv[1]).read_text()
required = {tuple(map(int, v.split('.')))
            for v in re.findall(r'GLIBC_([0-9.]+)', versions)}
assert max(required) <= (2, 35), sorted(required)
dynamic = pathlib.Path(sys.argv[2]).read_text()
needed = re.findall(r'\(NEEDED\).*\[(.*?)\]', dynamic)
assert set(needed) <= {'libc.so.6', 'libm.so.6', 'ld-linux-x86-64.so.2'}, needed
print('Runtime check:', needed, 'maximum GLIBC', max(required))
PY
done

if ASAN_OPTIONS=allow_addr2line=1 "$check_dir/asan" 4 > "$check_dir/report" 2>&1; then
  echo 'ASan failed to detect the deliberate buffer overflow' >&2
  exit 1
fi
grep -q 'AddressSanitizer: heap-buffer-overflow' "$check_dir/report"
grep -q 'check-toolchain.cpp:' "$check_dir/report"
echo 'C++26, chrono/tzdb, exceptions, static OpenMP and static ASan checks passed.'
