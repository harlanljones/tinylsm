// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// Page bootstrap: loads the WebAssembly engine, then drives the REPL, the
// visualizer, and the benchmark charts.
import { Terminal, HELP } from './terminal.js';
import { Visualizer } from './visualizer.js';
import { Charts } from './charts.js';

const createTinylsm = globalThis.createTinylsm;
if (typeof createTinylsm !== 'function') {
  throw new Error('tinylsm_wasm.js did not load: expected the global createTinylsm factory');
}

const DB_PATH = '/data/tinylsm';
const MEMTABLE_BYTES = 64 * 1024; // small on purpose: visitors see flushes
const BLOCK_BYTES = 4 * 1024;
const $ = (id) => document.getElementById(id);
// Yield to the page without waiting for a frame: keeps the load generator
// responsive while making progress independent of animation timing.
const yieldToPage = () => new Promise((resolve) => setTimeout(resolve, 0));
const wasm = await createTinylsm({ print: () => {}, printErr: (text) => console.warn(text) });
const openDb = (fn) => wasm.ccall(fn, 'number', ['string', 'number', 'number', 'number'],
  [DB_PATH, MEMTABLE_BYTES, BLOCK_BYTES, 0]);
const command = (line) => wasm.ccall('tinylsm_command', 'string', ['string'], [line]);
const lastError = () => wasm.ccall('tinylsm_last_error', 'string', [], []);
let stats = null;
let written = 0;

const terminal = new Terminal({
  screen: $('screen'),
  form: $('entry'),
  input: $('command-line'),
  bar: $('toolbar'),
  controls: [
    { label: 'FILL 500', hint: 'Write 500 keys and watch the memtable flush', action: 'fill', value: 500 },
    { label: 'FILL 5000', hint: 'Write 5000 keys and trigger cascading compactions', action: 'fill', value: 5000 },
    { label: 'FLUSH', hint: 'Rotate the memtable into a Level 0 SST', action: 'command', value: 'FLUSH' },
    { label: 'COMPACT', hint: 'Merge levels now', action: 'command', value: 'COMPACT' },
    { label: 'SCAN', hint: 'Iterate every live key', action: 'command', value: 'SCAN' },
    { label: 'STATS', hint: 'Print engine counters', action: 'command', value: 'STATS' },
    { label: 'REOPEN', hint: 'Close and reopen from MEMFS (WAL replay)', action: 'reopen' },
    { label: 'RESET', hint: 'Discard the database', action: 'reset' },
    { label: 'HELP', hint: 'List commands', action: 'help' },
  ],
  onSubmit: async (line) => runCommand(line),
  onControl: async (action, value) => {
    if (action === 'command') return runCommand(value);
    if (action === 'fill') return fill(Number(value));
    if (action === 'help') return terminal.print(HELP, 'meta');
    if (action === 'reopen') {
      if (openDb('tinylsm_recover') !== 0) return terminal.print(lastError(), 'err');
      terminal.print('closed and reopened from MEMFS — WAL and manifest replayed', 'meta');
      await refresh(lastError().replace(/^STATS\s*/, ''));
      return undefined;
    }
    if (action === 'reset') {
      if (openDb('tinylsm_reset') !== 0) return terminal.print(lastError(), 'err');
      written = 0;
      terminal.print('database reset: fresh WAL, empty memtable', 'meta');
      await refresh();
      return undefined;
    }
    return undefined;
  },
});

async function runCommand(line) {
  terminal.echo(line);
  const verb = line.trim().toUpperCase();
  if (verb === 'HELP') {
    terminal.print(HELP, 'meta');
    return;
  }
  const reply = command(line);
  if (verb.startsWith('STATS')) {
    const pretty = JSON.stringify(parseStats(reply) ?? reply, null, 1);
    terminal.print(pretty, 'out');
  }
  await refresh(reply);
}

function parseStats(reply) {
  const text = String(reply).trim();
  if (!text.startsWith('{')) return null;
  try {
    return JSON.parse(text);
  } catch {
    return null;
  }
}

async function refresh(reply) {
  const next = reply === undefined ? stats : parseStats(reply);
  if (reply !== undefined && next === null) {
    const text = String(reply).trim();
    terminal.print(text === '' ? '(no output)' : text, text.includes('Usage') ? 'err' : 'out');
  } else if (next !== null) {
    stats = next;
    // Push state immediately so the DOM reflects the newest counters even
    // between animation frames.
    if (visualizer) visualizer.update(next);
  }
  terminal.print(`writes ${stats?.writes ?? 0} · flushes ${stats?.flushes ?? 0} · compactions ${stats?.compactions ?? 0} · SSTs/level ${JSON.stringify(stats?.levels ?? [])}`, 'meta');
}

// A page-friendly load generator: chunks keep the animation smooth while the
// engine performs real WAL appends, flushes, and compactions.
async function fill(count) {
  const chunk = 250;
  const started = performance.now();
  terminal.print(`writing ${count} keys (memtable ${(MEMTABLE_BYTES / 1024).toFixed(0)} KiB, writes are page-driven)...`, 'meta');
  for (let offset = 0; offset < count; offset += chunk) {
    const consumed = Math.min(chunk, count - offset);
    for (let index = 0; index < consumed; ++index) {
      const key = `key/${written + index}`;
      command(`PUT "${key}" "${'v'.repeat(24 + (index % 48))}"`);
      // Rewrite a hot range so compaction always has superseded versions to prune.
      command(`PUT "hot/${index % 40}" "${'w'.repeat(32)}"`);
      if (index % 25 === 24) command(`DEL "key/${written + index - 12}"`);
    }
    written += consumed;
    const next = parseStats(command('STATS'));
    if (next) {
      stats = next;
      if (visualizer) visualizer.update(next);
    }
    if (offset % (chunk * 4) === 0) terminal.print(`  ${Math.min(count, offset + chunk)} / ${count} keys`, 'meta');
    await yieldToPage();
  }
  const seconds = (performance.now() - started) / 1000;
  terminal.print(`wrote ${count * 2} records in ${seconds.toFixed(2)}s (${Math.round((count * 2) / seconds).toLocaleString()} ops/s from the page)`, 'out');
  await refresh();
}

let visualizer = null;
visualizer = new Visualizer($('visualizer'), $('counters'), MEMTABLE_BYTES);
(function frame() {
  if (visualizer) visualizer.render();
  requestAnimationFrame(frame);
})();

const charts = new Charts({ target: $('chart-status'), legends: $('chart-legends'), summary: $('spec-targets') });
await charts.load();

if (openDb('tinylsm_open') !== 0) {
  terminal.print(`failed to open ${DB_PATH}: ${lastError()}`, 'err');
} else {
  terminal.print([
    'tinylsm 0.1.0 — embedded LSM-tree key-value store compiled to WebAssembly',
    `database   ${DB_PATH} (MEMFS, in-memory)`,
    `memtable   ${(MEMTABLE_BYTES / 1024).toFixed(0)} KiB · block ${BLOCK_BYTES} B · WAL sync: buffered`,
    '',
    'The real engine runs in this tab: append-only WAL, concurrent skip-list',
    'memtable, prefix-compressed SSTables with Bloom filters, and leveled',
    'compaction (10x per level, L0 trigger at four files).',
    '',
    HELP,
    '',
    'Try: FILL 5000, then COMPACT, then REOPEN.',
  ].join('\n'), 'meta');
  await refresh(command('STATS'));
}

// Deep links make scenarios reproducible and browser-testable:
//   ?run=PUT%20%22a%22%20%22b%22;FLUSH;SCAN&fill=2000
const params = new URLSearchParams(location.search);
for (const line of (params.get('run') ?? '').split(';').map((part) => part.trim()).filter(Boolean)) {
  await runCommand(line);
}
const fillCount = Number(params.get('fill') ?? 0);
if (Number.isFinite(fillCount) && fillCount > 0) await fill(fillCount);