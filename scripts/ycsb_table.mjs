#!/usr/bin/env node
// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// Renders bench/ycsb.cpp JSONL output as a workload table: throughput plus
// per-op p50/p99/p999. Single-engine today; the schema carries an engine
// field so future competitors join without changing this script.
// Usage: node scripts/ycsb_table.mjs [ycsb.jsonl]
import { readFileSync } from 'node:fs';

const load = (file) => {
  try {
    return readFileSync(file, 'utf8').split('\n').filter((l) => l.trim()).map((l) => JSON.parse(l));
  } catch { return []; }
};

const records = load(process.argv[2] ?? 'reports/ycsb.jsonl');
if (records.length === 0) { console.log('no ycsb records found'); process.exit(0); }

const byMetric = new Map(records.map((r) => [`${r.engine}:${r.metric}`, r]));
const env = byMetric.get('tinylsm:ycsb_env') ?? {};
const get = (m) => byMetric.get(`tinylsm:${m}`);

const fmtUs = (v) => v === undefined ? '—' : `${v.toFixed(v < 10 ? 3 : 1)} us`;
const fmtOps = (v) => v === undefined ? '—' : `${Math.round(v).toLocaleString()} ops/s`;

const workloads = [
  ['a', 'A: 50% read / 50% update', [['read', 'read'], ['update', 'update']]],
  ['b', 'B: 95% read / 5% update', [['read', 'read'], ['update', 'update']]],
  ['c', 'C: 100% read', [['read', 'read']]],
  ['d', 'D: 95% read-latest / 5% insert', [['readlatest', 'read-latest'], ['insert', 'insert']]],
  ['e', 'E: 95% short scan / 5% insert', [['scan', 'scan (len 1..100)'], ['insert', 'insert']]],
  ['f', 'F: 50% read / 50% RMW', [['read', 'read'], ['readmodifywrite', 'read-modify-write']]],
];

console.log(`workload (scrambled zipfian θ=0.99, 100B values)${env.records ? ` — ${env.records} records` : ''}`);
console.log('workload'.padEnd(34) + 'throughput'.padEnd(20) + 'op'.padEnd(22) + 'p50'.padEnd(12) + 'p99'.padEnd(12) + 'p999');
console.log('-'.repeat(112));
for (const [w, label, ops] of workloads) {
  const wrec = get(`ycsb_${w}`);
  const tput = fmtOps(wrec?.ops_per_second);
  let first = true;
  for (const [suffix, oplabel] of ops) {
    const r = get(`ycsb_${w}_${suffix}`);
    console.log(
      (first ? label : '').padEnd(34) +
      (first ? tput : '').padEnd(20) +
      oplabel.padEnd(22) +
      fmtUs(r?.p50).padEnd(12) +
      fmtUs(r?.p99).padEnd(12) +
      fmtUs(r?.p999),
    );
    first = false;
  }
}
const loadRec = get('ycsb_load');
if (loadRec) console.log(`\nload: ${fmtOps(loadRec.ops_per_second)} (${loadRec.ops} sequential inserts + compact)`);
