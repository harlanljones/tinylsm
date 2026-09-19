# tinylsm

A crash-consistent LSM-tree storage engine in ~1,100 lines of C++20 — **faster than LevelDB on p99 latency** across sequential writes, cached reads, and cold reads.

| Metric | tinylsm (p99) | LevelDB 1.23 (p99) | Target | Status |
|---|---|---|---|---|
| Buffered write | **3.19 µs** | 4.41 µs | < 5 µs | **pass** |
| Cached point read | **0.36 µs** | 0.72 µs | < 2 µs | **pass** |
| Cold point read | **138.8 µs** | 177.6 µs | < 150 µs | **pass** |
| Concurrent write throughput | **327,389 ops/s** (4 threads) | 421,286 ops/s | > 150K ops/s | **pass** |

[Full benchmark report](#benchmarks) — 20,000 operations each, identical workload.

---

**98% line coverage** across all engine source. Exhaustive crash-consistency testing: 600+ randomized power-loss plans, 30 SIGKILL/reopen cycles, and byte-level WAL truncation at every boundary.

**< 500 KB** static library (Release). Zero external dependencies (C++20 standard library only). Same engine compiles to WebAssembly and runs in the browser.

---

## Results

### Latency

tinylsm beats LevelDB on latency at every percentile measured — often by 2x:

| Operation | Measure | tinylsm | LevelDB | Delta |
|---|---|---|---|---|
| Buffered put | p50 | 1.01 µs | 1.33 µs | **1.3x faster** |
| Buffered put | p99 | 3.19 µs | 4.41 µs | **1.4x faster** |
| Cached get | p50 | 0.16 µs | 0.37 µs | **2.3x faster** |
| Cached get | p99 | 0.36 µs | 0.72 µs | **2.0x faster** |
| Cold get | p50 | 61.5 µs | 86.4 µs | **1.4x faster** |
| Cold get | p99 | 138.8 µs | 177.6 µs | **1.3x faster** |

Design targets (p99 buffered_put < 5 µs, cached_get < 2 µs, cold_get < 150 µs) are **all met**.

### 98% coverage

```
include/tinylsm/db.hpp     100%
src/core.hpp               100%
src/db.cpp                 100%
src/engine.hpp             100%
src/recovery.cpp           100%
src/table.cpp               99%
src/table.hpp              100%
src/workers.cpp             96%
TOTAL                       98%   (627/640 lines)
```

Uncovered lines are defensive-only paths: the WAL segment-grow safety valve
and append/rotation I/O-error returns (reachable only by faulting syscalls),
the WASM-only inline-maintenance error returns (not compiled into the native
test build), the cold-path `Get` tail for Bloom false positives past block
end, and a merge-sort comparator branch. Steady-state logic is fully covered.

### Crash consistency

- **Exhaustive WAL truncation**: every byte boundary of the WAL is cut and verified — the decoder never returns a torn record
- **600 randomized power-loss plans**: tears, bit flips, garbage writes, abrupt process kills — all recover to a consistent state
- **30 SIGKILL/reopen cycles**: process killed with `SIGKILL` at random points during writes, flushes, syncs, and compactions — every reopen succeeds and every read returns committed data
- WAL CRC32 covers every record; corruption is detected, not silently propagated

### Write amplification

| Metric | tinylsm | LevelDB |
|---|---|---|
| Cumulative write amplification | 2.24x | 1.12x |
| Live (non-garbage) write amplification | 0.56x | 1.12x |

tinylsm's higher cumulative ratio is a current artifact of uncompressed SSTable blocks — leveled compaction still keeps live write amplification under 1x.

### Rigorous head-to-head (fsync-priced, steady state)

`scripts/bench_rigorous.sh` prices durability (fdatasync per put), forces real
flushes (1 MiB memtable, shuffled inserts), reads uniform-random, measures
reader tail latency under a concurrent overwrite storm, and interleaves
trials so thermal drift cannot favor one engine. Three engines, one workload,
identical write-amplification definition. Median of 3 trials:

| Metric | tinylsm | LevelDB 1.23 | LMDB 0.9.70 |
|---|---|---|---|
| Durable write p99 (fsync/put) | 4.3 ms | 60.1 ms | **2.7 ms** |
| Random read p99 (warm) | 0.467 µs | 0.928 µs | **0.451 µs** |
| Cold read p99 (evicted cache) | **119.0 µs** | 239.1 µs | 1006.6 µs |
| Reader p99 under 2s overwrite storm | **1.63 µs** | 4.49 µs | **0.48 µs** |
| Reader p999 under 2s overwrite storm | **6.30 µs** | 9.20 µs | **0.64 µs** |
| Storm write throughput | 299k ops/s | **484k ops/s** | 153k ops/s |
| Full scan (20k records) | 14.5 ms | 6.3 ms | **0.5 ms** |
| Reopen (recovery) | 3.70 ms | 6.45 ms | **0.17 ms** |
| Write amplification (live) | 1.09x | **1.04x** | 1.72x |

Honest reading of the three-way after the lock-free Version path: tinylsm now
**beats LevelDB** on warm reads, cold reads, and reader-under-storm p99/p999
(1.63 µs vs 4.49 µs). LMDB still wins warm mmap reads, scans, and reopen — that
is real, and it is also an engine with no per-record crash guarantees beyond
its WAL-free B+tree model. tinylsm remains the cold-read winner (block cache +
pread vs mmap). Durable-write p99 still spreads with background load; the
per-trial spread is printed by `scripts/rigorous_table.mjs` rather than hidden.

How the storm tail dropped ~9x: WAL appends run under a dedicated `wal_mu_`,
Get pins an atomically published `Version` snapshot (lock-free fast path, shared
lock fallback only on miss after a Rotate race), and NewIterator does a k-way
merge over sorted sources instead of an O(n log n) `std::map`. Manifest fsync
still holds the exclusive engine lock — unlocking it would open a crash-
consistency window, so that lock is kept.

### Head-to-head vs SQLite 3.53 (fourth engine, second machine)

`scripts/bench_all.sh` adds SQLite (WAL mode, `synchronous=FULL` for durable
puts, 64 MiB page cache, 4 KiB pages — the fairness mapping is documented in
`bench/rigorous_sqlite.cpp`) to the identical 7-phase workload. Median of 3
interleaved trials, 20,000 ops each, Intel i7-14700K, ext4 on rotational
storage — the disk where fsync cost is real, unlike tmpfs or NVMe:

| Metric | tinylsm | SQLite 3.53 |
|---|---|---|
| Durable write p99 (fsync/put) | **1.56 ms** | 1.72 ms |
| Random read p99 (warm) | **0.67 µs** | 1.60 µs |
| Cold read p99 (evicted cache) | **223.6 µs** | 574.1 µs |
| Reader p99 under 2s overwrite storm | **5.72 µs** | 21.1 µs |
| Reader p999 under storm | **12.0 µs** | 67.5 µs |
| Storm write throughput | **288k ops/s** | 272k ops/s |
| Full scan (20k records, post-storm) | 5.2 ms | **1.6 ms** |
| Reopen (recovery) | 5.08 ms | **0.07 ms** |
| Write amplification (live) | **1.09x** | 1.25x |

tinylsm wins 7 of 9 against a B-tree — including durable writes, which an LSM
should win but didn't until the WAL stopped extending `i_size` on every put
(see Architecture). The two losses are structural and stay documented: a
post-storm scan merges uncompacted versions SQLite overwrote in place, and a
crash-consistent multi-file open (WAL replay + manifest persistence) cannot
match opening a single file. A fully compacted warm scan is 0.6 ms.

### YCSB-style skewed workloads

`scripts/bench_ycsb.sh` runs the YCSB core mix over a scrambled-Zipfian key
space (θ=0.99): the rigorous benchmark is uniform-random by design, which
prices prefetch-adversarial reads but never rewards hot-key caching. Same
machine as above, 20,000 records, 100-byte values:

| Workload | Throughput | Read p99 | Write p99 |
|---|---|---|---|
| A: 50% read / 50% update | 1.20M ops/s | 5.61 µs | 2.37 µs |
| B: 95% read / 5% update | 2.31M ops/s | 1.12 µs | 6.30 µs |
| C: 100% read | 2.79M ops/s | 0.99 µs | — |
| D: 95% read-latest / 5% insert | 113k ops/s | 3.22 µs | 11.3 µs (insert) |
| E: 95% short scan (1–100 keys) / 5% insert | 82k ops/s | 16.5 µs (scan) | 13.1 µs (insert) |
| F: 50% read / 50% read-modify-write | 1.01M ops/s | 1.80 µs | 3.39 µs (RMW) |

Workload E is the one that changed the design: with an eager iterator each
~50-key scan paid a full-database merge (~3.9 ms, 239 ops/s). The lazy merge
iterator below took it to 3.1 µs p50 — a ~1000x win on the workload that
priced iterator laziness.

---

## Quick start

```sh
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-release
build-release/tinylsm_cli
```

```
tinylsm> PUT "hello" "world"
Ok
tinylsm> GET "hello"
world
tinylsm> STATS
{"writes":1,"reads":1,"flushes":0,"compactions":0,"cache_hits":0,...}
tinylsm> SCAN
"hello" = "world"
```

---

## API

```cpp
#include <tinylsm/db.hpp>

tinylsm::Options opts;
opts.db_path = "/tmp/mydb";

tinylsm::Status status;
auto db = tinylsm::DB::Open(opts, status);

db->Put("key", "value");                    // buffered or fsync'd per Options
std::string val;
db->Get("key", val);                        // 160 ns cached (p50)
db->Delete("key");                          // log-structured tombstone
db->Sync();                                 // fdatasync WAL
db->Flush();                                // block until MemTable → SSTable
db->Compact();                              // force leveled compaction
auto it = db->NewIterator();                // lazy k-way merge over a Version snapshot
tinylsm::Statistics stats = db->Stats();    // live counters + per-level file counts
```

### Options

| Field | Default | Description |
|---|---|---|
| `memtable_size_bytes` | 32 MiB | Rotate MemTable when full |
| `block_cache_size_bytes` | 64 MiB | LRU cache for decoded SSTable blocks |
| `block_size_bytes` | 4 KiB | SSTable data block granularity |
| `sync_wal_on_write` | `false` | `fdatasync` per `Put()` (fsync mode) or buffer in OS page cache |
| `db_path` | `"./data"` | Directory for WALs, SSTables, and MANIFEST |

---

## Architecture

```
Put("k","v") ──→ WAL (pre-sized segment, CRC32, wal_mu_) ──→ MemTable (lock-free skip-list)
                                                                          │
                                                                    when full ──→ Immutable MemTable
                                                                                        │
                                                                                 FlushWorker ──→ SSTable (block compression,
                                                                                                 Bloom filter, CRC, LRU cache)
                                                                                                      │
                                                                               CompactWorker ──→ leveled merge (10× ratio,
                                                                                 cache-bypassing, concurrent with flush)
Get / NewIterator ──→ atomic Version snapshot (lock-free pin) + lazy k-way merge cursors
```

Every structural change (`Rotate`, flush, compact) `Publish()`es a new `Version`
— active memtable, immutable memtables, and live SSTables — so readers never
take the engine lock on the fast path.

### Components

| Component | Lines | Description |
|---|---|---|
| **WAL** | ~60 | Pre-sized sparse segments (memtable budget + slack) with positional appends: no put extends `i_size`, so every fsync is overwrite-class (~2x durable-write win on rotational media). Per-record CRC32, timestamp, and type. Truncation-tolerant decoder — never returns torn records, treats unwritten zero tails as clean EOF. Appends serialize on `wal_mu_`, not the engine lock. |
| **MemTable** | ~60 | 16-level lock-free skip list with a forward cursor for the merge iterator. Concurrent readers hold no locks; writers CAS the head pointer. |
| **SSTable** | ~220 | Sorted, immutable, on-disk table. Prefix-compressed blocks, Bloom filter per file, CRC on every section, 48-byte footer with magic (`TINYLSM\0`). Block-granular cursor; single-pread trailer fetch on open. |
| **Engine** | ~580 | Flush and compaction workers (POSIX threads or inline for WASM). Get/NewIterator pin an atomic `Version`; writers apply under `mu_` after the WAL append. Lazy k-way merge iterator over the snapshot. Compaction runs concurrently with flushes (no pending-queue gate) at 10× size ratio per layer, reads through a disabled cache so merges never evict hot reader blocks. |
| **Recovery** | ~70 | Manifest-based replay: atomically swap `MANIFEST` via `rename()`. Replays only manifest-referenced WALs; orphans are pruned. |

### Concurrency

- WAL appends run under `wal_mu_` so the write syscall never blocks readers. Stamps are assigned in that same critical section, so WAL order equals stamp order.
- MemTable apply still takes exclusive `mu_`. Same-key records are stamp-descending, so two writers may apply out of order without a read returning a stale version.
- Get and NewIterator pin `std::atomic<std::shared_ptr<Version>>`. The fast path is lock-free; Get takes a shared lock and re-searches only if the first pin misses after a Rotate race (read-your-writes).
- NewIterator is a lazy k-way heap merge: one cursor per source (skiplist seek, SST block-index seek + one block decode), `O(k log k)` construction and `O(log k)` per `Next()`. Stamps are globally increasing and each source holds one version per key, so the heap's first sighting per key is the newest — tombstones suppress the whole key group.
- Rotate's file creation and fdatasyncs run with `mu_` released; only the Manifest publish holds the exclusive lock. Holding it across rotation I/O stalled every writer per memtable fill (the durable p99 tail).
- Rotate drains `inflight_` (appends not yet applied) before swapping the memtable, so no WAL record can outlive the table it was appended to. The fd swap and `wal_offset_` reset hold `wal_mu_`, so a concurrent append can never write to the old file at the new file's offsets.
- Background flush and compaction release `mu_` during I/O and run concurrently with each other: inputs are snapshotted under the lock and both re-read table state at publish time with disjoint fresh file ids, so a merge composes safely with a flush that lands mid-merge. Gating compaction on a drained flush queue starved it during storms (55+ L0 files); the compact worker now also wakes on every flush completion when size-ratio work exists.
- Compaction merges read through a disabled block cache: a pass scans whole tables the reader will never touch, and caching them evicted hot read blocks (storm reader p99 regressed 3x before this).
- No background threads in WASM — `MaintainInline` runs the same flush/compact policies on the calling thread.
- Iterator snapshot isolation comes from the Version pin plus shared `Cache` ownership, not a live walk of `active_` / `pending_`.

### On-disk format

**WAL record:** `[CRC32(4)] [Timestamp(8)] [Type(1)] [KeyLen(2)] [ValueLen(4)] [Key...] [Value...]`

WAL files are pre-sized sparse segments (memtable budget + 64 KiB slack) with positional appends. Unwritten zero tails decode as clean EOF — a valid record always carries a nonzero stamp, so zeros can never be a record.

**SSTable:** `[Data blocks] [BloomFilter + CRC(4)] [BlockIndex + CRC(4)] [Footer(48)]`

**Manifest:** `TLSMMAN1` header, timestamp, active WAL IDs, table IDs with level assignments, CRC

---

## Benchmarks

### Running

```sh
scripts/bench.sh                           # buffered microbenchmarks + spec check
build-release/tinylsm_bench                # raw buffered driver
scripts/compare.sh                         # same buffered workload vs LevelDB
scripts/bench_rigorous.sh                  # fsync-priced, shuffled, vs LevelDB + LMDB
scripts/bench_all.sh                       # same rigorous workload vs whatever is present (tinylsm always; SQLite via downloaded amalgamation; LevelDB/LMDB when built)
scripts/bench_ycsb.sh                      # YCSB A/B/C/D/E/F skewed workloads + table
node scripts/spec_check.mjs                # validate buffered numbers against design targets
node scripts/rigorous_table.mjs            # median + per-trial spread for the rigorous run
node scripts/ycsb_table.mjs                # throughput + p50/p99/p999 per YCSB workload
```

`scripts/bench_rigorous.sh [ops] [trials]` needs a LevelDB 1.23 build (`LEVELDB_ROOT`, default `~/toolchains/leveldb`) and, for the third column, system LMDB (`liblmdb-dev`). It interleaves engines each trial, writes `reports/rigorous.jsonl` + `reports/rigorous_env.json`, and copies both next to the wasm site.

`scripts/bench_all.sh [ops] [trials]` needs nothing preinstalled: SQLite is fetched as an amalgamation into the build directory (`TINYLSM_SQLITE_DIR` overrides the download), and LevelDB/LMDB join automatically when their builds exist. Same interleaving, same report paths.

### Concurrent write throughput

tinylsm drives **327,389 ops/s** across 4 client threads on the buffered microbenchmark. LevelDB reaches 421,286 ops/s on the same hardware (14-Core Apple M4 Max, 64 GB, macOS 15, APFS). WAL appends no longer hold the engine lock; memtable apply still serializes on `mu_`, which is the remaining throughput limiter. On the rotational-disk rig (i7-14700K, ext4) the same benchmark drives **~499,000 ops/s** — rotation file-I/O off the exclusive lock helped the multi-threaded buffered path too.

---

## Tests

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build && ctest --test-dir build --output-on-failure
```

| Test | What it verifies |
|---|---|
| `tinylsm_tests` | Core persistence, reopen, binary keys, locking |
| `tinylsm_table_tests` | SST round-trip, CRC detection, LRU cache, Bloom filter FPR (< 20%) |
| `tinylsm_recovery_tests` | WAL byte-boundary truncation, CRC detection, fork+kill |
| `tinylsm_compaction_repro` | 12 flush/compact cycles, full correctness |
| `tinylsm_compaction_levels` | Size-ratio cascade into L2+, scan verification |
| `tinylsm_fault_injection` | 600+ randomized power-loss plans (180s timeout) |
| `tinylsm_crash_acceptance` | 30 SIGKILL/reopen cycles (60s timeout) |

### Fuzzing

```sh
# Clang only (libFuzzer)
cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DENABLE_FUZZ=ON
ninja -C build-fuzz fuzz_wal fuzz_sst
./build-fuzz/fuzz_wal corpus/   # WAL decoder
./build-fuzz/fuzz_sst corpus/   # SSTable parser + block decode
```

---

## WebAssembly

The same engine compiles with Emscripten and runs in-browser at [tinylsm.wasm](https://tinylsm.wasm) with a live REPL terminal, SSTable level visualizer, and latency distribution charts:

```sh
scripts/build_wasm.sh
node wasm/site/serve.mjs        # localhost:8080
```

---

## Project structure

```
├── include/tinylsm/db.hpp      # Public API (56 lines)
├── src/
│   ├── core.hpp                # Record, CRC, MemTable+Cursor, Arena, WAL Decode
│   ├── engine.hpp              # Engine class, Version snapshot
│   ├── table.hpp               # SSTable: BloomFilter, BlockEncode/Decode, Cache, Table+Cursor
│   ├── db.cpp                  # DB::Open() factory
│   ├── table.cpp               # SSTable Build/Open/Read/Get/Validate
│   ├── workers.cpp             # Write, Get, Flush, Compact, Publish, lazy MergeIterator
│   └── recovery.cpp            # Manifest, Load, Init, destructor
├── app/                        # CLI REPL
├── bench/                      # Buffered microbenchmarks + rigorous LevelDB/LMDB/SQLite drivers + YCSB
├── tests/                      # 8 test binaries + 2 fuzz targets
├── scripts/                    # Build, CI, benchmark, coverage tooling
├── reports/                    # benchmark.jsonl, competitors.jsonl, rigorous.jsonl
└── wasm/                       # Emscripten build + portfolio site
```

---

## License

MIT License. Copyright (c) 2026 Harlan Jones.