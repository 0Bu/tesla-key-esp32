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
// Usage: node tools/ble_row_parity.js <golden.tsv> <web-presenter.tsv>   (run by scripts/check-ble-row-parity.sh)

'use strict';
const fs = require('fs');
const path = require('path');

const goldenPath = process.argv[2], webGoldenPath = process.argv[3];
if (!goldenPath || !webGoldenPath) {
  console.error('usage: ble_row_parity.js <golden.tsv> <web-presenter.tsv>');
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

let bleRowFromStatus, bleFailWarn;
try {
  // eslint-disable-next-line no-eval
  const exported = eval(region + '\n; ({bleRowFromStatus, BLE_CONNECT_FAIL_WARN})');
  bleRowFromStatus = exported.bleRowFromStatus; bleFailWarn = exported.BLE_CONNECT_FAIL_WARN;
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

// The second golden comes from the actual native SoC/RSSI helpers, not another JS table.
const webBegin='/* WEB_PRESENTERS_BEGIN */', webEnd='/* WEB_PRESENTERS_END */';
if (app.split(webBegin).length!==2 || app.split(webEnd).length!==2 || app.indexOf(webBegin)>=app.indexOf(webEnd))
  throw new Error('web presenter parity markers missing or ambiguous');
const webRegion=app.slice(app.indexOf(webBegin),app.indexOf(webEnd));
const actualNum=app.match(/^function num\(x\)\{.*\}[^\n]*$/m);
if (!actualNum) throw new Error('actual numeric adapter shape changed');
const web=eval(actualNum[0]+'\n'+webRegion+'\n; ({socColor,signalBars,barsHTML})');
const webLines=fs.readFileSync(webGoldenPath,'utf8').trim().split('\n');
if (webLines.shift()!=='kind\tinput\tr\tg\tb') throw new Error('invalid web presenter golden header');
const counts={soc:0,rssi:0,warn:0};
for(const line of webLines){
  const fields=line.split('\t');
  if(fields.length!==5 || !fields.slice(1).every(x=>/^-?(0|[1-9][0-9]*)$/.test(x)))
    throw new Error('invalid web presenter golden row: '+line);
  const [kind,raw,...channels]=fields, input=Number(raw), expected=channels.map(Number);
  let actual;
  if(kind==='soc'){
    const rgb=web.socColor(input).match(/^rgb\(([0-9]+),([0-9]+),([0-9]+)\)$/);
    if(!rgb) throw new Error('invalid SoC color');
    actual=rgb.slice(1).map(Number);
  }else if(kind==='rssi'){
    actual=[web.signalBars(input),0,0];
    const lit=(web.barsHTML(input).match(/opacity="1"/g)||[]).length;
    if(lit!==actual[0]) throw new Error('rendered signal bars bypass presenter: '+line);
  }else if(kind==='warn') actual=[bleFailWarn,0,0];
  else throw new Error('unknown web presenter vector: '+kind);
  counts[kind]++;
  if(actual.some((value,i)=>value!==expected[i]))
    throw new Error('web presenter mismatch: '+line+' got '+actual.join(','));
}
if(counts.soc!==303 || counts.rssi!==258 || counts.warn!==1)
  throw new Error('incomplete web presenter golden: '+JSON.stringify(counts));
console.log('OK app.js SoC, RSSI glyphs and warning threshold match native helpers on 562 golden vectors');
