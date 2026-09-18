// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// Headless acceptance test for the WebAssembly build: drives the exact C ABI
// the portfolio site calls, inside Node's MEMFS, and asserts real engine
// behaviour (WAL durability, flush, compaction, recovery, scans).
// Usage: node tests/wasm_smoke.mjs [path-to-tinylsm_wasm.js]
import createTinylsm from '../wasm/site/tinylsm_wasm.js';

const failures = [];
function check(ok, what) {
  if (!ok) failures.push(what);
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`);
}

const module = await createTinylsm({ print: () => {}, printErr: () => {} });
const open = (path, memtable, block, sync) =>
  module.ccall('tinylsm_open', 'number', ['string', 'number', 'number', 'number'], [path, memtable, block, sync]);
const recover = (path, memtable, block, sync) =>
  module.ccall('tinylsm_recover', 'number', ['string', 'number', 'number', 'number'], [path, memtable, block, sync]);
const command = (line) => module.ccall('tinylsm_command', 'string', ['string'], [line]);
const stats = () => JSON.parse(command('STATS'));

const path = '/data/smoke';
check(open(path, 65536, 4096, 0) === 0, 'Open the engine on MEMFS /data/smoke');

for (let i = 0; i < 200; ++i) {
  const reply = command(`PUT "key/${i}" "value-${i}-${'x'.repeat(i % 40)}"`);
  if (reply !== 'Ok') { check(false, `PUT key/${i} -> ${reply}`); break; }
}
check(command('GET "key/137"') === `value-137-${'x'.repeat(137 % 40)}`, 'GET returns the written value');
check(command('GET "missing"') === 'NotFound', 'GET on a missing key reports NotFound');

let snapshot = stats();
check(snapshot.writes === 200, `STATS counts 200 writes (saw ${snapshot.writes})`);
check(snapshot.active_bytes > 0, `STATS reports a non-empty active MemTable (${snapshot.active_bytes} bytes)`);

check(command('FLUSH') === 'Ok', 'FLUSH rotates the MemTable');
snapshot = stats();
check(snapshot.active_bytes === 0, 'active MemTable is empty after FLUSH');
check(snapshot.levels[0] >= 1, `a Level 0 SST exists after FLUSH (${JSON.stringify(snapshot.levels)})`);

check(command('DEL "key/137"') === 'Ok', 'DEL writes a tombstone');
check(command('GET "key/137"') === 'NotFound', 'deleted key disappears before compaction');
check(command('COMPACT') === 'Ok', 'COMPACT merges L0 into L1');
snapshot = stats();
check(snapshot.compactions >= 1, `STATS records compaction (${snapshot.compactions})`);
check(snapshot.levels[0] === 0 || snapshot.levels.length >= 2,
  `compaction moved data out of L0 (${JSON.stringify(snapshot.levels)})`);
check(command('GET "key/137"') === 'NotFound', 'tombstone still hides the key after compaction');
check(command('GET "key/42"').startsWith('value-42'), 'unrelated key survives compaction');

const scan = command('SCAN').trim().split('\n').filter(Boolean);
check(scan.length === 199, `SCAN returns 199 live keys (saw ${scan.length})`);
// Keys sort lexicographically, so derive the expected range count from the same order.
const live = [];
for (let i = 0; i < 200; ++i) if (i !== 137) live.push(`key/${i}`);
live.sort();
const expectedRanged = live.filter((k) => k >= 'key/100').length;
const ranged = command('SCAN "key/100"').trim().split('\n').filter(Boolean);
check(expectedRanged > 0 && ranged.length === expectedRanged,
  `SCAN from "key/100" returns ${expectedRanged} rows (saw ${ranged.length})`);

// Close and reopen from MEMFS: the browser analogue of a process restart.
check(recover(path, 65536, 4096, 0) === 0, 'Reopen the engine from MEMFS (WAL + manifest replay)');
check(command('GET "key/42"').startsWith('value-42'), 'key survives reopen');
check(command('GET "key/137"') === 'NotFound', 'deletion survives reopen');

const wal = command('PUT "after-recovery" "durable"');
check(wal === 'Ok', 'engine remains writable after recovery');
check(command('SYNC') === 'Ok', 'SYNC succeeds');
check(module.ccall('tinylsm_close', 'number', [], []) === 0, 'Close the engine');

console.log(failures.length === 0
  ? '\nPASS: WebAssembly engine verified end to end in Node (MEMFS, WAL, flush, compaction, recovery, scans)'
  : `\nFAIL: ${failures.length} check(s): ${failures.join('; ')}`);
process.exit(failures.length === 0 ? 0 : 1);