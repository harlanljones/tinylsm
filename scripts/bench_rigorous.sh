#!/usr/bin/env bash
# Rigorous head-to-head benchmark.
#
# Why this exists: bench/main.cpp measures buffered (unsynced) sequential puts
# into a 32 MiB memtable holding a ~2.3 MiB dataset — pure in-memory skiplist
# speed with zero flushes — followed by quiescent sequential reads. That is a
# best-case microbenchmark, not evidence. This harness instead:
#   - prices durability (fdatasync per Put on both engines),
#   - forces steady state (1 MiB memtable, shuffled inserts, real flushes),
#   - reads uniform-random (no prefetch/sequential subsidy),
#   - measures reader tail latency UNDER concurrent write pressure,
#   - repeats interleaved trials (default 3) to expose run-to-run variance,
#   - computes write amplification with one identical definition,
#   - records the machine environment for reproducibility.
#
# Usage: scripts/bench_rigorous.sh [operations] [trials]
#   LEVELDB_ROOT overrides the LevelDB checkout location.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
count="${1:-20000}"
trials="${2:-3}"
leveldb_root="${LEVELDB_ROOT:-$HOME/toolchains/leveldb}"
build="$root/build-compare"
include="$leveldb_root/leveldb-1.23/include"
library="$leveldb_root/build/libleveldb.a"

if [ ! -f "$library" ]; then
  echo "LevelDB not built at $library" >&2
  exit 1
fi

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DTINYLSM_LEVELDB_INCLUDE_DIR="$include" -DTINYLSM_LEVELDB_LIBRARY="$library" >/dev/null
cmake --build "$build" -j "$(nproc)" --target tinylsm_rigorous tinylsm_rigorous_leveldb tinylsm_rigorous_lmdb >/dev/null

mkdir -p "$root/reports"
out="$root/reports/rigorous.jsonl"
env_out="$root/reports/rigorous_env.json"
: > "$out"

{
  echo "{"
  echo "  \"date_utc\": \"$(date -u +%FT%TZ)\","
  echo "  \"git_sha\": \"$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)\","
  echo "  \"git_dirty\": \"$(git -C "$root" status --porcelain 2>/dev/null | head -5 | tr '\n' ';')\","
  echo "  \"uname\": \"$(uname -a)\","
  echo "  \"cpu\": \"$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs)\","
  echo "  \"fstype\": \"$(df -T "$build" | awk 'NR==2{print $2}')\","
  echo "  \"mount_opts\": \"$(findmnt -no OPTIONS -T "$build" 2>/dev/null || echo unknown)\","
  echo "  \"build_type\": \"Release\","
  echo "  \"ops\": $count,"
  echo "  \"trials\": $trials,"
  echo "  \"workload\": \"durable_put(shuffled) random_get(uniform) cold_get(random,evicted) read_under_write(2s) scan reopen write_amp(iterator-scanned)\""
  echo "}"
} > "$env_out"

# Interleaved trials: T, L, LMDB each round — never all of one engine
# then all of another, so a thermal or page-cache drift cannot
# systematically favor one engine.
for ((t = 0; t < trials; t++)); do
  echo "== trial $((t + 1))/$trials: tinylsm =="
  "$build/tinylsm_rigorous" "$count" "$build/rig-db-t" "$t" | tee -a "$out"
  echo "== trial $((t + 1))/$trials: leveldb =="
  "$build/tinylsm_rigorous_leveldb" "$count" "$build/rig-leveldb-db" "$t" | tee -a "$out"
  echo "== trial $((t + 1))/$trials: lmdb =="
  "$build/tinylsm_rigorous_lmdb" "$count" "$build/rig-lmdb-db" "$t" | tee -a "$out"
done

cp "$out" "$env_out" "$root/wasm/site/" 2>/dev/null || true
echo
node "$root/scripts/rigorous_table.mjs" "$out" "$env_out"
