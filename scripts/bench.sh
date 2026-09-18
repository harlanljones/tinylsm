#!/usr/bin/env bash
# Microbenchmarks (design section 5): builds the release binary, runs the
# workload against a disk-backed database (so cold-read numbers are real),
# records reports/benchmark.jsonl, and checks the design targets.
# Usage: scripts/bench.sh [operations]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
count="${1:-20000}"
build="$root/build-release"
db_dir="${TINYLSM_BENCH_DB:-$build/bench-db}"

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build" -j "$(nproc)" --target tinylsm_bench >/dev/null
mkdir -p "$root/reports"
"$build/tinylsm_bench" "$count" "$db_dir" | tee "$root/reports/benchmark.jsonl"
cp "$root/reports/benchmark.jsonl" "$root/wasm/site/benchmark.jsonl"
echo
node "$root/scripts/spec_check.mjs" "$root/reports/benchmark.jsonl"