#!/usr/bin/env bash
# YCSB-style skewed workloads (bench/ycsb.cpp): builds the driver, runs the
# A/B/C/D/E/F mix over a scrambled-Zipfian key space, saves JSONL, and prints
# the workload table.
#
# Usage: scripts/bench_ycsb.sh [records] [ops_per_workload]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
records="${1:-20000}"
ops="${2:-20000}"
build="$root/build-compare-ycsb"

export PATH="/home/harlan/.local/share/mise/installs/python/3.12.13/bin:$PATH"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build" -j "$(nproc)" --target tinylsm_ycsb >/dev/null

mkdir -p "$root/reports"
out="$root/reports/ycsb.jsonl"
"$build/tinylsm_ycsb" "$records" "$ops" "$build/ycsb-db" | tee "$out"
cp "$out" "$root/wasm/site/" 2>/dev/null || true
echo
node "$root/scripts/ycsb_table.mjs" "$out"
