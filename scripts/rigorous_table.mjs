#!/usr/bin/env node
// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// Aggregates reports/rigorous.jsonl across interleaved trials and prints an
// honest comparison table. Deliberately NOT a pass/fail spec check: fixed
// targets the author also sets are circular evidence. This prints
// median-across-trials with the full per-trial spread, so variance is visible
// instead of hidden behind a single lucky run.
//
// Extra legitimacy checks:
//   - coldness: cold p50 must be clearly above warm p50, else page-cache
//     eviction failed and the "cold" column is a lie.
//   - timer floor: sub-microsecond percentiles within ~10x of the measured
//     clock overhead are flagged as below the trust threshold.
// Usage: node scripts/rigorous_table.mjs [rigorous.jsonl] [rigorous_env.json]
import { readFileSync } from 'node:fs';

const load = (file) => {
  try {
    return readFileSync(file, 'utf8').split('\n').filter((l) => l.trim()).map((l) => JSON.parse(l));
  } catch { return []; }
};

const records = load(process.argv[2] ?? 'reports/rigorous.jsonl');
let env = {};
try { env = JSON.parse(readFileSync(process.argv[3] ?? 'reports/rigorous_env.json', 'utf8')); } catch { /* optional */ }

const byEngineMetric = new Map(); // engine -> metric -> [records]
for (const r of records) {
  if (!byEngineMetric.has(r.engine)) byEngineMetric.set(r.engine, new Map());
  const m = byEngineMetric.get(r.engine);
  if (!m.has(r.metric)) m.set(r.metric, []);
  m.get(r.metric).push(r);
}
const engines = [...byEngineMetric.keys()];
if (engines.length === 0) { console.log('no rigorous records found'); process.exit(0); }

const median = (xs) => {
  const s = [...xs].sort((a, b) => a - b);
  const mid = Math.floor(s.length / 2);
  return s.length % 2 ? s[mid] : (s[mid - 1] + s[mid]) / 2;
};
const med = (engine, metric, field) => {
  const rows = byEngineMetric.get(engine)?.get(metric) ?? [];
  const vals = rows.map((r) => r[field]).filter((v) => typeof v === 'number');
  return vals.length ? median(vals) : undefined;
};
const spread = (engine, metric, field) => {
  const rows = byEngineMetric.get(engine)?.get(metric) ?? [];
  const vals = rows.map((r) => r[field]).filter((v) => typeof v === 'number').sort((a, b) => a - b);
  return vals.length ? [vals[0], vals[vals.length - 1]] : undefined;
};

const fmtUs = (v) => v === undefined ? '—' : `${v.toFixed(v < 10 ? 3 : 1)} us`;
const fmtOps = (v) => v === undefined ? '—' : `${Math.round(v).toLocaleString()} ops/s`;

const latencyMetrics = [
  ['durable_put', 'durable write (fsync/put, shuffled)'],
  ['random_get', 'random read (uniform, warm)'],
  ['cold_get', 'cold read (random, evicted page cache)'],
  ['read_under_write', 'reader during 2s overwrite storm'],
];
const pcts = ['p50', 'p99', 'p999'];
const rows = [];
for (const [metric, label] of latencyMetrics) {
  for (const p of pcts) {
    const row = { metric: `${label} ${p}` };
    for (const e of engines) row[e] = fmtUs(med(e, metric, p));
    rows.push(row);
  }
}
const extra = [
  ['durable write throughput', 'durable_put', 'ops_per_second', fmtOps],
  ['storm writer throughput', 'read_under_write', 'ops_per_second', fmtOps],
  ['full scan', 'scan', 'total_ms', (v) => v === undefined ? '—' : `${v.toFixed(1)} ms`],
  ['scan per key', 'scan', 'ns_per_key', (v) => v === undefined ? '—' : `${Math.round(v).toLocaleString()} ns`],
  ['close/reopen', 'reopen', 'reopen_ms', (v) => v === undefined ? '—' : `${v.toFixed(2)} ms`],
  ['write amplification (live)', 'write_amp', 'live_ratio', (v) => v === undefined ? '—' : `${v.toFixed(2)}x`],
];
for (const [label, metric, field, fmt] of extra) {
  const row = { metric: label };
  for (const e of engines) row[e] = fmt(med(e, metric, field));
  rows.push(row);
}

const width = Math.max(...rows.map((r) => r.metric.length), 12);
const colw = Object.fromEntries(engines.map((e) => [e, 22]));
const line = (cells) => cells.map((c, i) => String(c).padEnd(i === 0 ? width + 2 : colw[engines[i - 1]])).join('');
console.log(line(['metric (median of trials)', ...engines]));
console.log(line([''.padEnd(width + 2, '-'), ...engines.map(() => ''.padEnd(22, '-'))]));
for (const r of rows) console.log(line([r.metric, ...engines.map((e) => r[e])]));

// Per-trial p99 spread: the variance the old single-run bench hid.
console.log('\nper-trial p99 spread (min..max across trials):');
for (const [metric, label] of latencyMetrics) {
  const cells = engines.map((e) => {
    const s = spread(e, metric, 'p99');
    return s ? `${s[0].toFixed(2)}..${s[1].toFixed(2)} us` : '—';
  });
  console.log(line([label, ...cells]));
}

// Coldness check.
console.log('\nlegitimacy checks:');
let ok = true;
for (const e of engines) {
  const warm = med(e, 'random_get', 'p50');
  const cold = med(e, 'cold_get', 'p50');
  if (warm === undefined || cold === undefined) { console.log(`  ${e}: cold/warm not measurable — inconclusive`); ok = false; continue; }
  const ratio = cold / warm;
  const verdict = ratio > 5 ? 'COLD_OK' : 'EVICTION_SUSPECT';
  if (ratio <= 5) ok = false;
  console.log(`  ${e}: cold p50 / warm p50 = ${ratio.toFixed(1)}x  [${verdict}]`);
}
// Timer-floor check.
for (const e of engines) {
  const envRows = (byEngineMetric.get(e).get('env') ?? []);
  const overhead = envRows.length ? median(envRows.map((r) => r.timer_overhead_ns)) : NaN;
  const fastest = med(e, 'random_get', 'p50');
  if (!Number.isFinite(overhead) || fastest === undefined) continue;
  const ratio = (fastest * 1000) / overhead;
  const verdict = ratio > 10 ? 'TIMER_OK' : 'BELOW_TRUST_FLOOR';
  if (ratio <= 10) ok = false;
  console.log(`  ${e}: fastest p50 / clock overhead = ${ratio.toFixed(1)}x (overhead ${overhead.toFixed(0)} ns)  [${verdict}]`);
}
// Winner summary with ratios across every engine present (informative, not a verdict).
console.log('\nhead-to-head (median of trials):');
const head = (label, metric, field, lowerBetter, fmt) => {
  const vals = engines.map((e) => [e, med(e, metric, field)]);
  if (vals.some(([, v]) => v === undefined)) return;
  const best = vals.reduce((a, b) => (lowerBetter ? (b[1] < a[1] ? b : a) : (b[1] > a[1] ? b : a)));
  const cells = vals.map(([e, v]) => {
    const ratio = v === 0 ? 0 : (lowerBetter ? best[1] / v : v / best[1]);
    const tag = e === best[0] ? 'best' : `${(1 / ratio).toFixed(2)}x slower`;
    return `${fmt(v)} (${tag})`;
  });
  console.log(`  ${label}: best=${best[0]} | ${vals.map(([e], i) => `${e}: ${cells[i]}`).join(' | ')}`);
};
head('durable write p99', 'durable_put', 'p99', true, fmtUs);
head('random read p99', 'random_get', 'p99', true, fmtUs);
head('reader-under-storm p99', 'read_under_write', 'p99', true, fmtUs);
head('reader-under-storm p999', 'read_under_write', 'p999', true, fmtUs);
head('cold read p99', 'cold_get', 'p99', true, fmtUs);
head('storm writer tput', 'read_under_write', 'ops_per_second', false, fmtOps);
head('scan', 'scan', 'total_ms', true, (v) => `${v.toFixed(1)} ms`);
head('reopen', 'reopen', 'reopen_ms', true, (v) => `${v.toFixed(2)} ms`);
head('write amp', 'write_amp', 'live_ratio', true, (v) => `${v.toFixed(2)}x`);

if (env.git_sha) console.log(`\nenv: ${env.cpu ?? '?'} | ${env.fstype ?? '?'} | git ${String(env.git_sha).slice(0, 12)} | ${env.ops ?? '?'} ops x ${env.trials ?? '?'} trials interleaved`);
process.exit(ok ? 0 : 1);
