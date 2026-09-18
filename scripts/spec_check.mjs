#!/usr/bin/env node
// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// Checks a benchmark.jsonl run against the performance targets in design
// section 5 and prints a verdict table. Exits non-zero if any measurable target
// is missed. Usage: node scripts/spec_check.mjs [reports/benchmark.jsonl]
import { readFileSync } from 'node:fs';

const TARGETS = [
  { metric: 'buffered_put', field: 'p99', limit: 5, unit: 'us', direction: 'below',
    label: 'sequential write latency (p99, buffered WAL)' },
  { metric: 'cached_get', field: 'p99', limit: 2, unit: 'us', direction: 'below',
    label: 'point read latency (p99, cached)' },
  { metric: 'cold_get', field: 'p99', limit: 150, unit: 'us', direction: 'below',
    label: 'point read latency (p99, cold page cache)' },
  { metric: 'concurrent_write', field: 'ops_per_second', limit: 150000, unit: 'ops/s', direction: 'above',
    label: 'write throughput (4 client threads)' },
];

const file = process.argv[2] ?? 'reports/benchmark.jsonl';
let records;
try {
  records = readFileSync(file, 'utf8').split('\n').filter((line) => line.trim()).map((line) => JSON.parse(line));
} catch (error) {
  console.error(`cannot read ${file}: ${error.message}`);
  process.exit(2);
}

const width = Math.max(...TARGETS.map((t) => t.label.length));
let failed = 0;
let skipped = 0;
for (const target of TARGETS) {
  const record = records.find((row) => row.metric === target.metric);
  const label = target.label.padEnd(width);
  if (!record) {
    skipped += 1;
    console.log(`${label}  NOT MEASURED (${target.metric})`);
    continue;
  }
  const measured = record[target.field];
  const pass = target.direction === 'below' ? measured <= target.limit : measured >= target.limit;
  if (!pass) failed += 1;
  const shown = target.unit === 'us' ? measured.toFixed(3) : Math.round(measured).toLocaleString();
  console.log(`${label}  ${pass ? 'pass' : 'MISS'}  ` +
    `target ${target.direction === 'below' ? '<' : '>'} ${target.limit} ${target.unit}, measured ${shown} ${target.unit}`);
}
console.log(`\n${records.length} metrics from ${file}: ${TARGETS.length - failed - skipped} pass, ${failed} miss, ${skipped} not measured`);
process.exit(failed === 0 ? 0 : 1);