# tinylsm — portfolio demo (design document section 7)

`tinylsm` is compiled to WebAssembly with Emscripten and mounted on an
in-memory filesystem (`MEMFS`), so the real engine — WAL, skip-list MemTable,
SSTable blocks, Bloom filters, leveled compaction — runs inside the page with
no server behind it.

## Build

```bash
bash scripts/build_wasm.sh          # stages wasm/site/tinylsm_wasm.{js,wasm}
node tests/wasm_smoke.mjs           # headless acceptance test of the same C ABI
```

`build_wasm.sh` finds Emscripten through `$EMSDK`, `~/toolchains/emsdk`, or
`/opt/emsdk`, then configures `wasm/` with `emcmake` and copies the artifacts
next to the site.

## Serve

```bash
node wasm/site/serve.mjs            # http://localhost:8080
```

Any static host works: the browser build does **not** need threads, so no
`COOP`/`COEP` headers and no `SharedArrayBuffer` are required.

## Browser build notes

The POSIX build runs flush and compaction on background worker threads. In
WebAssembly those workers deadlock when the page's main thread blocks, because
Emscripten proxies part of a worker thread's syscalls back to the main thread:
`tinylsm_command("FLUSH")` then waits out its full 30 s budget and returns
`IOError` while the immutable MemTable stays unflushed.

The browser build therefore compiles `TINYLSM_INLINE_MAINTENANCE`
(`src/engine.hpp`) and runs *the same* `FlushFront`/`CompactPass` policies on
the calling thread, with no worker threads at all. Level sizing (10x per level),
the L0 trigger at four files, tombstone retention, and the 4 KB block layout are
unchanged. Pass `-DTINYLSM_BACKGROUND_THREADS=1` if you want the threaded
scheduler in a wasm build anyway.

MEMFS has no persistent backing store, so a page reload starts empty; the
`REOPEN` control closes and reopens the engine inside the page, which replays
the WAL and the `MANIFEST` exactly as a process restart does on disk.

## What the page shows

* **Terminal** — the same command parser as the native CLI
  (`PUT`, `GET`, `DEL`, `SCAN [key]`, `SYNC`, `FLUSH`, `COMPACT`, `STATS`),
  plus load generation and reset/reopen controls.
* **Visualizer** — live MemTable fill, immutable MemTables, the Level 0..N
  stacks, cache hit ratio, and flush/compaction counters, drawn from `STATS`.
* **Charts** — latency distributions (log-bucketed histograms), throughput, and
  write amplification, read from `benchmark.jsonl`. Add `competitors.jsonl`
  with the same record shape plus an `"engine"` field to overlay other engines.
