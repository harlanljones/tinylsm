#!/usr/bin/env bash
# Head-to-head run (design section 7): measures tinylsm and LevelDB with the
# identical workload in one process pair and writes the files the portfolio
# charts read (reports/benchmark.jsonl and reports/competitors.jsonl).
# Usage: scripts/compare.sh [operations]
#   LEVELDB_ROOT overrides the LevelDB checkout location.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
count="${1:-20000}"
leveldb_root="${LEVELDB_ROOT:-$HOME/toolchains/leveldb}"
build="$root/build-compare"
include="$leveldb_root/leveldb-1.23/include"
library="$leveldb_root/build/libleveldb.a"

if [ ! -f "$library" ]; then
  echo "LevelDB not built at $library" >&2
  echo "Fetch a release tarball and build it, for example:" >&2
  echo "  curl -sSL https://github.com/google/leveldb/archive/refs/tags/1.23.tar.gz | tar xz" >&2
  echo "  cmake -S leveldb-1.23 -B build -DCMAKE_BUILD_TYPE=Release -DLEVELDB_BUILD_TESTS=OFF -DLEVELDB_BUILD_BENCHMARKS=OFF" >&2
  echo "  cmake --build build -j --target leveldb" >&2
  exit 1
fi

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DTINYLSM_LEVELDB_INCLUDE_DIR="$include" -DTINYLSM_LEVELDB_LIBRARY="$library" >/dev/null
cmake --build "$build" -j "$(nproc)" --target tinylsm_bench tinylsm_bench_leveldb >/dev/null

mkdir -p "$root/reports"
echo "== tinylsm ($count operations) =="
"$build/tinylsm_bench" "$count" "$build/bench-db" | tee "$root/reports/benchmark.jsonl"
echo "== leveldb $("$build/tinylsm_bench_leveldb" --version 2>/dev/null || echo 1.23) ($count operations) =="
"$build/tinylsm_bench_leveldb" "$count" "$build/leveldb-db" | tee "$root/reports/competitors.jsonl"

cp "$root/reports/benchmark.jsonl" "$root/reports/competitors.jsonl" "$root/wasm/site/"
echo
node "$root/scripts/compare_table.mjs" "$root/reports/benchmark.jsonl" "$root/reports/competitors.jsonl"
echo
node "$root/scripts/spec_check.mjs" "$root/reports/benchmark.jsonl"