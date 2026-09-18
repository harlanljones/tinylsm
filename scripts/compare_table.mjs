#!/usr/bin/env node
// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// Prints the head-to-head table for two benchmark.jsonl files.
// Usage: node scripts/compare_table.mjs <primary.jsonl> <other.jsonl>
import { readFileSync } from 'node:fs';

const load = (file) => {
  try {
    return readFileSync(file, 'utf8').split('\n').filter((line) => line.trim()).map((line) => JSON.parse(line));
  } catch {
    return [];
  }
};
const groups = new Map();
for (const file of process.argv.slice(2)) {
  for (const record of load(file)) {
    const engine = record.engine ?? 'tinylsm';
    if (!groups.has(engine)) groups.set(engine, new Map());
    groups.get(engine).set(record.metric, record);
  }
}
if (groups.size === 0) {
  console.log('no benchmark records found');
  process.exit(0);
}

const metricLabel = {
  buffered_put: ['write p50', 'write p90', 'write p99'],
  cached_get: ['cached read p50', 'cached read p90', 'cached read p99'],
  cold_get: ['cold read p50', 'cold read p90', 'cold read p99'],
};
const PERCENTILES = ['p50', 'p90', 'p99'];
const rows = [];
for (const [metric, labels] of Object.entries(metricLabel)) {
  labels.forEach((label, index) => {
    const row = { metric: label };
    for (const [engine, records] of groups) {
      const record = records.get(metric);
      row[engine] = record ? `${Number(record[PERCENTILES[index]]).toFixed(3)} us` : '—';
    }
    rows.push(row);
  });
}
const engines = [...groups.keys()];
const counters = [
  ['write throughput', 'concurrent_write', 'ops_per_second', (v) => `${Math.round(v).toLocaleString()} ops/s`],
  ['write amplification', 'sst_write_amplification', 'live_ratio', (v) => `${Number(v).toFixed(2)}x`],
];
for (const [label, metric, field, format] of counters) {
  const row = { metric: label };
  for (const engine of engines) {
    const value = groups.get(engine).get(metric)?.[field];
    row[engine] = value === undefined ? '—' : format(value);
  }
  rows.push(row);
}

const width = Math.max(...rows.map((row) => row.metric.length), 12);
const columns = Object.fromEntries(engines.map((engine) => [engine, 16]));
const line = (cells) => cells.map((cell, index) => String(cell).padEnd(index === 0 ? width + 2 : columns[engines[index - 1]])).join('');
console.log(line(['metric', ...engines]));
console.log(line([''.padEnd(width + 2, '-'), ...engines.map(() => ''.padEnd(16, '-'))]));
for (const row of rows) console.log(line([row.metric, ...engines.map((engine) => row[engine])]));
console.log('\nlatency in microseconds; lower is better for latency and amplification, higher is better for throughput');