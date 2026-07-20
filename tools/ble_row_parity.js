#!/usr/bin/env node
// Parity check: does the BLE-row decision that actually runs in the browser still match the
// host-tested C++ presenter (tk::ble::decide in main/logic/ble_row.hpp)?
//
// Reads the golden TSV emitted by test/ble_row_golden_dump.cpp, extracts the BLE_ROW region
// from main/www/app.js — the real shipped source, not a copy — evaluates it in this process,
// re-decides every input vector, and diffs. Any disagreement is a hard failure with the
// offending row printed, because a silent divergence here is precisely the bug class this
// pair of files exists to make impossible.
//
// Usage: node tools/ble_row_parity.js <golden.tsv>   (run by scripts/check-ble-row-parity.sh)

'use strict';
const fs = require('fs');
const path = require('path');

const goldenPath = process.argv[2];
if (!goldenPath) {
  console.error('usage: ble_row_parity.js <golden.tsv>');
  process.exit(2);
}

const appPath = path.join(__dirname, '..', 'main', 'www', 'app.js');
const app = fs.readFileSync(appPath, 'utf8');

// Extract the marked region. Failing loudly here matters: if the markers are ever renamed or
// dropped, this must NOT silently pass by testing nothing.
const begin = app.indexOf('/* BLE_ROW_BEGIN */');
const end = app.indexOf('/* BLE_ROW_END */');
if (begin < 0 || end < 0 || end <= begin) {
  console.error('ble-row parity: BLE_ROW_BEGIN/END markers not found in main/www/app.js — the '
              + 'decision region must stay marked so this harness can extract it.');
  process.exit(1);
}
const region = app.slice(begin, end);
if (!/function\s+bleRowFromStatus\s*\(/.test(region)) {
  console.error('ble-row parity: the BLE_ROW region does not define bleRowFromStatus().');
  process.exit(1);
}

let bleRowFromStatus;
try {
  // eslint-disable-next-line no-eval
  bleRowFromStatus = eval(region + '\n; bleRowFromStatus');
} catch (e) {
  console.error('ble-row parity: could not evaluate the BLE_ROW region:', e.message);
  process.exit(1);
}

const lines = fs.readFileSync(goldenPath, 'utf8').trim().split('\n');
const header = lines.shift().split('\t');
const col = Object.fromEntries(header.map((h, i) => [h, i]));

let checked = 0;
const mismatches = [];
for (const line of lines) {
  if (!line.trim()) continue;
  const f = line.split('\t');
  // Build the /status object the browser would actually receive — the point is to exercise the
  // adapter too, so nothing here may pre-derive what bleRowFromStatus is supposed to derive.
  const ble = {
    connected: f[col.connected] === '1',
    devices: Array.from({ length: parseInt(f[col.devices], 10) }, (_, i) => ({ addr: `de:ad:be:ef:00:0${i}` })),
  };
  if (f[col.connect_fail] !== '0') ble.connect_fail = parseInt(f[col.connect_fail], 10);
  if (f[col.phase] !== 'none') ble.phase = f[col.phase];
  const status = {
    vin: f[col.vin],
    paired: f[col.paired] === '1',
    link: f[col.link],
    ble,
  };
  const got = bleRowFromStatus(status);
  const want = {
    row: f[col.row],
    cd: f[col.cd],
    stateless: f[col.stateless] === '1',
  };
  checked++;
  if (got.row !== want.row || got.cd !== want.cd || Boolean(got.stateless) !== want.stateless) {
    mismatches.push({ input: line, want, got });
  }
}

if (!checked) {
  console.error('ble-row parity: golden file had no data rows');
  process.exit(1);
}

if (mismatches.length) {
  console.error(`FAILED  app.js disagrees with logic/ble_row.hpp on ${mismatches.length}/${checked} vectors:`);
  for (const m of mismatches.slice(0, 10)) {
    console.error('  in  ', m.input);
    console.error('  want', JSON.stringify(m.want));
    console.error('  got ', JSON.stringify(m.got));
  }
  if (mismatches.length > 10) console.error(`  … and ${mismatches.length - 10} more`);
  process.exit(1);
}

console.log(`OK  app.js bleRowFromStatus() matches the C++ presenter on ${checked} golden vectors`);
