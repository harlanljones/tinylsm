// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// Browser acceptance test for the portfolio demo (design section 7): serves
// wasm/site, drives it in headless Chrome through a deep link, and asserts that
// the real engine answered commands, the visualizer painted frames, the
// benchmark charts loaded, and no page errors occurred.
// Usage: node tests/site_smoke.mjs
import { execFileSync, spawn } from 'node:child_process';
import { setTimeout as delay } from 'node:timers/promises';

const PORT = Number(process.env.PORT ?? 8137);
const BASE = `http://localhost:${PORT}`;
const CHROME = process.env.CHROME ?? 'google-chrome-stable';
const failures = [];
const check = (ok, what) => {
  if (!ok) failures.push(what);
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`);
};

const server = spawn(process.execPath, ['wasm/site/serve.mjs'], {
  env: { ...process.env, PORT: String(PORT) },
  stdio: ['ignore', 'ignore', 'inherit'],
});
async function ready() {
  for (let attempt = 0; attempt < 60; ++attempt) {
    try {
      const response = await fetch(`${BASE}/index.html`);
      if (response.ok) return;
    } catch { /* not listening yet */ }
    await delay(150);
  }
  throw new Error('static server never became ready');
}

let dom = '';
try {
  await ready();
  const scenario = [
    'PUT "browser" "verified"',
    'FLUSH',
    'GET "browser"',
    'STATS',
  ].map(encodeURIComponent).join(';');
  const url = `${BASE}/?run=${scenario}&fill=600`;
  dom = execFileSync(CHROME, [
    '--headless=new', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage',
    '--user-data-dir=/tmp/tinylsm-chrome', '--virtual-time-budget=25000',
    '--dump-dom', url,
  ], { encoding: 'utf8', maxBuffer: 96 * 1024 * 1024, timeout: 120000, stdio: ['ignore', 'pipe', 'ignore'] });
} catch (error) {
  check(false, `headless Chrome run: ${error.message.split('\n')[0]}`);
} finally {
  server.kill();
}

check(dom.length > 0, 'headless Chrome returned the rendered DOM');
check(dom.includes('tinylsm 0.1.0'), 'page booted the WebAssembly engine and printed its banner');
check(dom.includes('/data/tinylsm'), 'engine opened the MEMFS database path');
check(dom.includes('verified'), 'deep-linked GET returned the value written by the deep-linked PUT');
check(dom.includes('&gt; FLUSH') || dom.includes('> FLUSH'), 'REPL echoed the FLUSH command');
check(dom.includes('active_bytes'), 'STATS printed engine counters as JSON');

const errors = /id="page-errors"[^>]*>([\s\S]*?)<\/div>/.exec(dom)?.[1]?.trim() ?? 'MISSING';
check(errors === '', `no page errors or rejected promises (saw: ${JSON.stringify(errors)})`);

const writes = Number(/<dt>writes<\/dt><dd>([\d,]+)<\/dd>/.exec(dom)?.[1]?.replace(/,/g, '') ?? -1);
check(dom.includes('records in'), 'load generator ran to completion');
// 600 keys x (one key record + one hot-range record) + a delete every 25 keys.
check(writes >= 1200, `load generator issued ${writes} engine writes through the page`);
check(/<dt>flushes<\/dt><dd>[1-9]/.test(dom), 'a memtable flush was recorded by the visualizer counters');
check(/<dt>cache hits<\/dt><dd>/.test(dom), 'cache statistics rendered');
const frames = Number(/data-frames="(\d+)"/.exec(dom)?.[1] ?? 0);
const canvasWidth = Number(/<canvas id="visualizer"[^>]*width="(\d+)"/.exec(dom)?.[1] ?? 0);
check(frames >= 1 && canvasWidth > 0,
  `visualizer painted ${frames} frame(s) into a ${canvasWidth}px-wide canvas`);
check(Number(/data-levels="(\d+)"/.exec(dom)?.[1] ?? 0) >= 1, 'visualizer rendered at least one SST level');
check(dom.includes('Latency distribution'), 'benchmark chart section rendered');
const chartCanvasWidth = Number(/<canvas id="distribution"[^>]*width="(\d+)"/.exec(dom)?.[1] ?? 0);
check(chartCanvasWidth > 0, `latency-distribution canvas initialised at ${chartCanvasWidth}px`);
check(/<span class="swatch">/.test(dom) && dom.includes('tinylsm'),
  'chart legends rendered from the parsed benchmark.jsonl engine list');
check(dom.includes('p99 write latency'), 'benchmark target table rendered from benchmark.jsonl');
check((dom.match(/class="pass"/g) ?? []).length >= 3, 'measured latencies met the design targets on this machine');
check((dom.match(/<tr>/g) ?? []).length >= 5, 'every design target row is present in the table');

console.log(failures.length === 0
  ? '\nPASS: portfolio page verified in headless Chrome (engine, REPL, visualizer, charts, no page errors)'
  : `\nFAIL: ${failures.length} check(s): ${failures.join('; ')}`);
process.exit(failures.length === 0 ? 0 : 1);