#!/usr/bin/env bash
# Rigorous head-to-head across every engine available on this machine.
#
# bench_rigorous.sh needs a LevelDB checkout and system LMDB; this script runs
# whatever the CMake configure finds: tinylsm always, plus SQLite (downloaded
# amalgamation, no system install needed), LevelDB, and LMDB when present.
# Trials interleave engines so thermal or page-cache drift cannot favor one.
#
# Usage: scripts/bench_all.sh [operations] [trials]
#   TINYLSM_SQLITE_DIR skips the amalgamation download (dir with sqlite3.c).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
count="${1:-20000}"
trials="${2:-3}"
leveldb_root="${LEVELDB_ROOT:-$HOME/toolchains/leveldb}"
build="$root/build-compare"
sqlite_version="3530400"
sqlite_dir="${TINYLSM_SQLITE_DIR:-$build/sqlite-amalgamation}"

if [ ! -f "$sqlite_dir/sqlite3.c" ]; then
  echo "-- fetching SQLite amalgamation $sqlite_version"
  mkdir -p "$build/sqlite-download"
  curl -s --max-time 120 -o "$build/sqlite-download/amalg.zip" \
    "https://www.sqlite.org/2026/sqlite-amalgamation-$sqlite_version.zip"
  unzip -o -q "$build/sqlite-download/amalg.zip" -d "$build/sqlite-download"
  sqlite_dir="$build/sqlite-download/sqlite-amalgamation-$sqlite_version"
fi

leveldb_flags=()
if [ -f "$leveldb_root/build/libleveldb.a" ]; then
  leveldb_flags+=("-DTINYLSM_LEVELDB_INCLUDE_DIR=$leveldb_root/leveldb-1.23/include"
                  "-DTINYLSM_LEVELDB_LIBRARY=$leveldb_root/build/libleveldb.a")
else
  echo "-- no LevelDB at $leveldb_root/build/libleveldb.a (skipping leveldb)"
fi

export PATH="/home/harlan/.local/share/mise/installs/python/3.12.13/bin:$PATH"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DTINYLSM_SQLITE_DIR="$sqlite_dir" "${leveldb_flags[@]}" >/dev/null
targets="tinylsm_rigorous tinylsm_rigorous_sqlite"
[ -f "$build/build.ninja" ] && targets="$targets $(ninja -C "$build" -t targets all 2>/dev/null | awk -F: '/^tinylsm_rigorous[_a-z]*: CXX_EXECUTABLE/ {print $1}' | grep -v -x -e tinylsm_rigorous -e tinylsm_rigorous_sqlite | tr '\n' ' ' || true)"
cmake --build "$build" -j "$(nproc)" --target $targets >/dev/null

mapfile -t drivers < <(ls "$build"/tinylsm_rigorous* 2>/dev/null)
if [ "${#drivers[@]}" -eq 0 ]; then echo "no benchmark drivers built" >&2; exit 1; fi
echo "-- engines: $(basename -a "${drivers[@]}" | tr '\n' ' ')"

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

t=0
for ((trial = 0; trial < trials; trial++)); do
  for driver in "${drivers[@]}"; do
    name="$(basename "$driver")"
    echo "== trial $((trial + 1))/$trials: $name =="
    "$driver" "$count" "$build/rig-$(echo "$name" | tr '_' '-')" "$t" | tee -a "$out"
  done
  t=$((t + 1))
done

cp "$out" "$env_out" "$root/wasm/site/" 2>/dev/null || true
echo
node "$root/scripts/rigorous_table.mjs" "$out" "$env_out"
