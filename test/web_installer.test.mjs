// Installer module tests: manifest and download integrity, serial-port ownership, probe/flash/monitor
// lifecycles, the installed-firmware check and the published page.
import assert from "node:assert/strict";
import { createHash, webcrypto } from "node:crypto";
import fs from "node:fs";
import test from "node:test";
import { bootstrapInstaller } from "../docs/installer-bootstrap.mjs";

import { acquireSerialPortLease, releaseSerialPortLease, serialPortLease } from "../docs/serial-port-release.mjs";
import {
  FLASH_LAYOUT,
  attachWebInstaller,
  bootVersion,
  channelLabel,
  combinedProgress,
  configurationKeepable,
  describeSerialConnection,
  detectedSerialType,
  fetchFirmwareParts,
  fetchPartitionTable,
  flashDevice,
  hasCompatibleNvs,
  inspectInstalledFirmware,
  loadFirmwareOffer,
  otaSequenceCrc,
  parseAppDescription,
  parsePartitionTable,
  partitionTableLength,
  probeDevice,
  resetToUserFirmware,
  selectBootPartition,
  selectManifestBuild,
  serialLogLevel,
  sharedPortTransport,
  splitSerialChunk,
  stripSerialAnsi,
  validateManifest
} from "../docs/web-installer.mjs";

const digest = (data) => createHash("sha256").update(data).digest("hex");
const CHIPS = ["ESP32", "ESP32-S3", "ESP32-C3", "ESP32-C6"];
const SUFFIX = { ESP32: "esp32", "ESP32-S3": "esp32s3", "ESP32-C3": "esp32c3", "ESP32-C6": "esp32c6" };
const SOURCE_SHA = "a".repeat(40);
const MANIFEST_URL = "https://example.test/tesla-key-esp32/manifest.json";
const noDelay = async () => {};

// partitions.csv in the ESP-IDF entry format, padded like gen_esp32part.py output (without its MD5 entry).
const PARTITION_TYPES = { app: 0, data: 1 };
const PARTITION_SUBTYPES = { nvs: 0x02, ota: 0x00, phy: 0x01, coredump: 0x03, ota_0: 0x10, ota_1: 0x11 };
function partitionRows() {
  return fs.readFileSync(new URL("../partitions.csv", import.meta.url), "utf8").split("\n")
    .map((raw) => raw.trim()).filter((line) => line && !line.startsWith("#"))
    .map((line) => line.split(",").map((field) => field.trim()));
}
function partitionTableBytes(rows = partitionRows()) {
  const table = Buffer.alloc(0xc00, 0xff);
  rows.forEach(([name, type, subtype, offset, size], index) => {
    const at = index * 32;
    table.writeUInt16LE(0x50aa, at);
    table[at + 2] = PARTITION_TYPES[type];
    table[at + 3] = PARTITION_SUBTYPES[subtype];
    table.writeUInt32LE(Number(offset), at + 4);
    table.writeUInt32LE(Number(size), at + 8);
    table.fill(0, at + 12, at + 32);
    table.write(name, at + 12, 16, "latin1");
  });
  return table;
}
const PARTITION_TABLE = partitionTableBytes();
// A table from before the coredump partition: the same nvs, so the configuration can be kept.
const OLDER_TABLE = partitionTableBytes(partitionRows().filter(([name]) => name !== "coredump"));

// esp_image_header_t and the first segment header, then esp_app_desc_t (magic, version, project_name).
function appHead(project = "tesla-key-esp32", version = "1.6.2") {
  const head = Buffer.alloc(0x100, 0);
  head[0] = 0xe9;
  head.writeUInt32LE(0xabcd5432, 0x20);
  head.write(version, 0x30, 32, "latin1");
  head.write(project, 0x50, 32, "latin1");
  return head;
}

// esp_ota_select_entry_t; state 2 is ESP_OTA_IMG_VALID.
function otadataEntry(sequence, state = 2, crc = otaSequenceCrc(sequence)) {
  const entry = Buffer.alloc(32, 0xff);
  entry.writeUInt32LE(sequence, 0);
  entry.writeUInt32LE(state, 24);
  entry.writeUInt32LE(crc, 28);
  return entry;
}

// Sparse flash contents for readFlash; anything not listed reads as erased.
function flashImage(regions = { 0x8000: PARTITION_TABLE, 0x20000: appHead() }) {
  return (offset, length) => {
    const bytes = new Uint8Array(length).fill(0xff);
    for (const [start, data] of Object.entries(regions)) {
      for (let index = 0; index < data.length; index++) {
        const at = Number(start) + index - offset;
        if (at >= 0 && at < length) bytes[at] = data[index];
      }
    }
    return bytes;
  };
}
const INSTALLED_FLASH = flashImage();

// A schema-v2 Pages feed for all four targets, served as streamed responses.
function firmwareFixture(version = "1.6.3") {
  const payloads = new Map();
  const builds = CHIPS.map((chipFamily, index) => {
    const suffix = SUFFIX[chipFamily];
    const parts = [
      [`bootloader-${suffix}.bin`, chipFamily === "ESP32" ? 0x1000 : 0, Buffer.alloc(0x40, 0x10 + index)],
      [`partition-table-${suffix}.bin`, 0x8000, Buffer.from(PARTITION_TABLE)],
      [`tesla-key-esp32-${suffix}.bin`, 0x20000, Buffer.concat([appHead("tesla-key-esp32", version), Buffer.alloc(0x40, 0x20 + index)])],
      [`ota_data_initial-${suffix}.bin`, 0xf000, Buffer.alloc(0x2000, 0xff)]
    ];
    for (const [path, , data] of parts) payloads.set(path, data);
    return { chipFamily, parts: parts.map(([path, offset, data]) => ({ path, offset, size: data.length, sha256: digest(data) })) };
  });
  const document = { name: "tesla-key-esp32", layoutVersion: 2, sourceSha: SOURCE_SHA, version, new_install_prompt_erase: true, builds };
  const fixture = {
    document, payloads, seen: [],
    refresh() {
      this.rawManifest = Buffer.from(JSON.stringify(document));
      return this;
    },
    async fetch(url, options) {
      fixture.seen.push({ url, options });
      const name = new URL(url).pathname.split("/").at(-1);
      const data = name === "manifest.json" ? fixture.rawManifest : payloads.get(name);
      return {
        ok: Boolean(data), status: data ? 200 : 404, url, redirected: false,
        body: new ReadableStream({
          start(controller) {
            if (data) controller.enqueue(Uint8Array.from(data));
            controller.close();
          }
        }),
        async arrayBuffer() { throw new Error("bulk body consumption must never be used"); }
      };
    },
    load(manifestUrl = MANIFEST_URL) {
      return loadFirmwareOffer({ manifestUrl, fetchImpl: fixture.fetch, cryptoImpl: webcrypto });
    }
  };
  return fixture.refresh();
}

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((accept, fail) => { resolve = accept; reject = fail; });
  promise.catch(() => {});
  return { promise, resolve, reject };
}

async function drainNativeTasks() {
  for (let turn = 0; turn < 60; turn++) await Promise.resolve();
}

async function until(predicate) {
  for (let turn = 0; turn < 500 && !predicate(); turn++) await new Promise((resolve) => setImmediate(resolve));
  assert.ok(predicate(), "the awaited native step was not reached");
}

function controlledBody({ chunks = [], hang = false, cancelHangs = false } = {}) {
  let started;
  const record = { cancellations: [], pulls: 0, started: new Promise((resolve) => { started = resolve; }) };
  let chunkIndex = 0;
  record.body = new ReadableStream({
    pull(controller) {
      record.pulls++;
      started();
      if (chunkIndex < chunks.length) controller.enqueue(Uint8Array.from(chunks[chunkIndex++]));
      else if (hang) return new Promise(() => {});
      else controller.close();
    },
    cancel(reason) {
      record.cancellations.push(reason);
      if (cancelHangs) return new Promise(() => {});
    }
  }, { highWaterMark: 0 });
  return record;
}

function streamedResponse(url, record) {
  return {
    ok: true, status: 200, url, redirected: false, body: record.body,
    async arrayBuffer() { throw new Error("bulk body consumption must never be used"); }
  };
}

// A port that esptool-js has opened; closing it ends both streams.
function openPort(info = { usbVendorId: 0x303a, usbProductId: 0x1001 }) {
  return {
    readable: {}, writable: {}, signals: [], closes: 0,
    getInfo() { return info; },
    async setSignals(value) { this.signals.push(value); },
    async close() { this.closes++; this.readable = null; this.writable = null; }
  };
}

function fakeEsptool(chipFamily = "ESP32-S3", options = {}) {
  const { chipRevision, flash = INSTALLED_FLASH } = options;
  // An explicit undefined stands for a flash ID esptool-js cannot map to a size.
  const flashSize = Object.hasOwn(options, "flashSize") ? options.flashSize : "4MB";
  const calls = [];
  const writes = [];
  const loaderOptions = [];
  class Transport {
    constructor(port) { this.port = port; this.device = port; calls.push("transport"); }
    async disconnect() { calls.push("disconnect"); await this.port.close?.(); }
  }
  class Loader {
    constructor(options) {
      this.options = options;
      this.chip = { CHIP_NAME: chipFamily };
      if (chipRevision !== undefined) {
        this.chip.getChipRevision = async () => chipRevision;
        this.chip.getChipDescription = async () => `${chipFamily} (revision v${chipRevision}.0)`;
      }
      calls.push("loader");
      loaderOptions.push(options);
    }
    // Like esptool-js: main() uploads and starts the flasher stub.
    async main() { calls.push("main"); this.IS_STUB = true; }
    async flashId() { calls.push("flashId"); }
    async detectFlashSize() { calls.push("detectFlashSize"); return flashSize; }
    async eraseFlash() { calls.push("erase"); }
    async readFlash(offset, length) { return flash(offset, length); }
    async writeFlash(options) {
      calls.push(["write", options.fileArray.map((file) => file.address)]);
      writes.push(options);
      options.reportProgress(0, options.fileArray[0].data.length, options.fileArray[0].data.length);
    }
  }
  return { calls, writes, loaderOptions, Transport, Loader };
}

const writesOf = (calls) => calls.filter((call) => Array.isArray(call) && call[0] === "write").map((call) => call[1]);
const EN_PULSE = [{ dataTerminalReady: false, requestToSend: true }, { dataTerminalReady: false, requestToSend: false }];

test("native Espressif USB ports and UART bridges are described without restricting multi-target builds", () => {
  const { document } = firmwareFixture();
  const cdcInfo = { usbVendorId: 0x303a, usbProductId: 0x1001 };
  const uartInfo = { usbVendorId: 0x1a86, usbProductId: 0x55d3 };
  assert.equal(detectedSerialType(cdcInfo), "cdc");
  assert.equal(detectedSerialType(uartInfo), "uart");
  assert.equal(describeSerialConnection(cdcInfo), "USB Serial/JTAG");
  assert.equal(describeSerialConnection(uartInfo), "USB UART");
  assert.equal(describeSerialConnection({}), "Web Serial");
  assert.equal(selectManifestBuild(document, "ESP32-S3").parts[0].path, "bootloader-esp32s3.bin");
  assert.equal(selectManifestBuild(document, "ESP32-C6").parts[0].path, "bootloader-esp32c6.bin");
  assert.equal(selectManifestBuild(document, "ESP32-C5"), undefined);
});

test("multi-part progress is weighted by each uncompressed image size", () => {
  const files = [{ data: new Uint8Array(4), address: 0 }, { data: new Uint8Array(6), address: 4 }];
  assert.equal(combinedProgress(files, 0, 2, 4), 20);
  assert.equal(combinedProgress(files, 1, 0, 6), 40);
  assert.equal(combinedProgress(files, 1, 3, 6), 70);
  assert.equal(combinedProgress(files, 1, 6, 6), 100);
  assert.equal(combinedProgress(files, 2, 0, 0), 0);
});

test("serial log parsing preserves line endings across chunks and classifies IDF levels", () => {
  const first = splitSerialChunk("", "\x1b[0;33mW (7700) uart: pin busy\r");
  assert.deepEqual(first.lines, []);
  assert.equal(first.pending, "\x1b[0;33mW (7700) uart: pin busy\r");
  const second = splitSerialChunk(first.pending, "\nE (8340) uart: failed\nI (9000) diag: continuing");
  assert.deepEqual(second.lines, [
    { text: "\x1b[0;33mW (7700) uart: pin busy", terminated: true },
    { text: "E (8340) uart: failed", terminated: true }
  ]);
  assert.equal(second.pending, "I (9000) diag: continuing");
  assert.equal(serialLogLevel(second.lines[0].text), "warning");
  assert.equal(serialLogLevel(second.lines[1].text), "error");
  assert.equal(serialLogLevel(second.pending), "info");
  assert.equal(stripSerialAnsi(second.lines[0].text), "W (7700) uart: pin busy");
  assert.deepEqual(splitSerialChunk(second.pending, "", true), {
    lines: [{ text: "I (9000) diag: continuing", terminated: false }],
    pending: ""
  });
});

test("the boot version comes only from the fixed IDF and BOOT line shapes", () => {
  assert.equal(bootVersion("I (105) app_init: App version:      1.6.3"), "1.6.3");
  assert.equal(bootVersion("\x1b[0;32mI (498) main: BOOT version=1.6.3-PR-412 elf=3f2a reset=usb safe_mode=no\x1b[0m"), "1.6.3-PR-412");
  assert.equal(bootVersion("BOOT version=1.6.4-dev.7"), "1.6.4-dev.7");
  assert.equal(bootVersion("I (99) main: rebooting into version 1.6.3"), "");
  assert.equal(bootVersion("BOOT version=<img src=x>"), "");
  assert.equal(bootVersion("XBOOT version=1.0.0"), "");
});

test("the subtitle channel follows the Pages path for previews and the version otherwise", () => {
  assert.equal(channelLabel("1.6.3", "/tesla-key-esp32/"), "Release");
  assert.equal(channelLabel("1.6.4-dev.7", "/tesla-key-esp32/"), "Dev");
  assert.equal(channelLabel("1.6.4", "/tesla-key-esp32/dev/"), "Dev", "the dev feed by its path");
  assert.equal(channelLabel("1.6.3-PR-412", "/tesla-key-esp32/PR/412/"), "PR #412 preview");
  assert.equal(channelLabel("1.6.3-device", "/"), "Release");
});

test("manifest v2 rejects malicious offsets, sizes, paths, products and incomplete provenance", () => {
  const { document } = firmwareFixture();
  const invalid = (mutate, pattern) => {
    const manifest = structuredClone(document);
    mutate(manifest);
    assert.throws(() => validateManifest(manifest, MANIFEST_URL),
      (error) => error.name === "InvalidManifestError" && pattern.test(error.message), String(pattern));
  };
  assert.equal(validateManifest(structuredClone(document), MANIFEST_URL).version, "1.6.3");
  invalid((m) => { m.builds[1].parts[2].offset = 0x9000; }, /application offset/);
  invalid((m) => { m.builds[1].parts[0].offset = false; }, /bootloader offset/);
  invalid((m) => { m.builds[1].parts[2].path = "https://evil.example/app.bin"; }, /unsafe application path/);
  invalid((m) => { m.builds[1].parts[1].size = 0x1001; }, /partition table size/);
  invalid((m) => { m.builds[1].parts[3].size = 0x1000; }, /OTA selector size/);
  invalid((m) => { m.builds[1].parts[2].sha256 = "A".repeat(64); }, /application SHA-256/);
  invalid((m) => { m.sourceSha = "main"; }, /source commit/);
  invalid((m) => { m.name = "supramatic-hcp-esp32"; }, /must name tesla-key-esp32/);
  invalid((m) => { m.layoutVersion = 1; }, /layoutVersion must be 2/);
  invalid((m) => { m.version = "01.6.3"; }, /version/);
  invalid((m) => { m.version = `1.6.3-${"x".repeat(26)}`; }, /version/);
  invalid((m) => { delete m.new_install_prompt_erase; }, /erase policy/);
  invalid((m) => { m.builds.pop(); }, /exactly four target builds/);
  invalid((m) => { m.builds[3].chipFamily = "ESP32"; }, /incomplete or duplicated/);
  invalid((m) => { m.builds[1].serialType = "cdc"; }, /build fields must be exactly chipFamily and parts/);
  // The partition table may fill its sector up to nvs, never into it.
  const boundary = structuredClone(document);
  boundary.builds[1].parts[1].size = 0x1000;
  assert.doesNotThrow(() => validateManifest(boundary, MANIFEST_URL));
});

test("an offer is loaded only from its HTTPS directory, as strict JSON, in place and within its byte limit", async () => {
  const fixture = firmwareFixture();
  const offer = await fixture.load();
  assert.equal(offer.manifest.version, "1.6.3");
  assert.ok(Object.isFrozen(offer.manifest.builds[0].parts[0]), "the displayed offer cannot change afterwards");
  assert.deepEqual(fixture.seen.map(({ url, options }) => [url, options.cache, options.redirect]),
    [[MANIFEST_URL, "no-store", "error"]]);
  assert.ok((await loadFirmwareOffer({
    manifestUrl: "http://127.0.0.1:8766/manifest.json", fetchImpl: fixture.fetch, cryptoImpl: webcrypto
  })).manifest, "a loopback page serves local browser tests");
  for (const manifestUrl of ["http://example.test/manifest.json", `${MANIFEST_URL}?x=1`, "https://example.test/other.json",
    "https://user:secret@example.test/manifest.json"]) {
    await assert.rejects(loadFirmwareOffer({ manifestUrl, fetchImpl: fixture.fetch, cryptoImpl: webcrypto }),
      { name: "InvalidManifestError", message: /HTTPS installer directory/ }, manifestUrl);
  }
  const duplicate = firmwareFixture();
  duplicate.rawManifest = Buffer.from(duplicate.rawManifest.toString().replace('"layoutVersion":2', '"layoutVersion":2,"layoutVersion":2'));
  await assert.rejects(duplicate.load(), { name: "InvalidManifestError", message: /duplicate key layoutVersion/ });
  const redirected = firmwareFixture();
  const redirect = async (url, options) => ({ ...(await redirected.fetch(url, options)), url: "https://evil.example/manifest.json" });
  await assert.rejects(loadFirmwareOffer({ manifestUrl: MANIFEST_URL, fetchImpl: redirect, cryptoImpl: webcrypto }),
    { name: "FirmwareIntegrityError", message: /changed location/ });
  await assert.rejects(loadFirmwareOffer({ manifestUrl: MANIFEST_URL, fetchImpl: fixture.fetch, cryptoImpl: {} }),
    { name: "FirmwareVerificationUnavailableError" });
  const source = controlledBody({ chunks: [Buffer.alloc(65536, 32), Buffer.from(" ")], cancelHangs: true });
  let signal;
  await assert.rejects(loadFirmwareOffer({
    manifestUrl: MANIFEST_URL, cryptoImpl: webcrypto,
    fetchImpl: async (url, options) => { signal = options.signal; return streamedResponse(url, source); }
  }), /exceeded its byte limit/);
  assert.equal(signal.aborted, true);
  assert.equal(source.cancellations.length, 1);
  assert.equal(source.pulls, 2, "no chunk beyond the first excess byte may be requested");
});

test("firmware parts stay in the feed directory, keep the manifest order and are SHA-256 verified", async () => {
  const fixture = firmwareFixture();
  const offer = await fixture.load();
  const build = offer.manifest.builds.find((item) => item.chipFamily === "ESP32-S3");
  const parts = await fetchFirmwareParts(build, offer, fixture.fetch);
  assert.deepEqual(fixture.seen.slice(1).map(({ url }) => url), build.parts.map((part) =>
    `https://example.test/tesla-key-esp32/${part.path}`));
  assert.deepEqual(parts.map((part) => [part.address, part.role]), [
    [0, "bootloader"], [0x8000, "partition table"], [0x20000, "application"], [0xf000, "OTA selector"]
  ]);
  assert.deepEqual(parts.map((part) => digest(part.data)), build.parts.map((part) => part.sha256));
  assert.ok(Buffer.from(await fetchPartitionTable(offer, build, fixture.fetch)).equals(PARTITION_TABLE));
  await assert.rejects(fetchFirmwareParts(structuredClone(build), offer, fixture.fetch), /outside the verified firmware offer/);
  await assert.rejects(fetchFirmwareParts(build, structuredClone(offer), fixture.fetch), /verified firmware offer is required/);
  fixture.payloads.get(build.parts[2].path)[0x80] ^= 1;
  await assert.rejects(fetchFirmwareParts(build, offer, fixture.fetch),
    { name: "FirmwareIntegrityError", message: /tesla-key-esp32-esp32s3\.bin failed its SHA-256 integrity check/ });
});

test("the first corrupt part aborts and cancels every pending sibling download", async () => {
  const fixture = firmwareFixture();
  const offer = await fixture.load();
  const build = offer.manifest.builds[1];
  const corrupt = Uint8Array.from(fixture.payloads.get(build.parts[0].path));
  corrupt[0] ^= 1;
  const sources = build.parts.map((_part, index) => index === 0 ?
    controlledBody({ chunks: [corrupt] }) : controlledBody({ hang: true, cancelHangs: true }));
  const signals = [];
  await assert.rejects(fetchFirmwareParts(build, offer, async (url, options) => {
    const index = build.parts.findIndex((part) => url.endsWith(part.path));
    signals[index] = options.signal;
    return streamedResponse(url, sources[index]);
  }), /bootloader-esp32s3\.bin failed its SHA-256/);
  for (let index = 1; index < sources.length; index++) {
    assert.equal(signals[index].aborted, true);
    assert.equal(sources[index].cancellations.length, 1);
    assert.equal(sources[index].body.locked, false);
  }
});

test("a part longer than its manifest size by one byte fails before the next read", async () => {
  const fixture = firmwareFixture();
  const offer = await fixture.load();
  const build = offer.manifest.builds[1];
  const source = controlledBody({ chunks: [fixture.payloads.get(build.parts[0].path), Uint8Array.of(1)] });
  await assert.rejects(fetchFirmwareParts(build, offer, async (url, options) =>
    url.endsWith(build.parts[0].path) ? streamedResponse(url, source) : fixture.fetch(url, options)), /exceeded its byte limit/);
  assert.equal(source.cancellations.length, 1);
});

test("one absolute deadline covers fetch and a hung body", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const source = controlledBody({ hang: true, cancelHangs: true });
  let signal;
  const loading = loadFirmwareOffer({
    manifestUrl: MANIFEST_URL, cryptoImpl: webcrypto,
    fetchImpl: async (url, options) => { signal = options.signal; return streamedResponse(url, source); }
  });
  const rejection = assert.rejects(loading, /60-second time limit/);
  await source.started;
  t.mock.timers.tick(59999);
  assert.equal(signal.aborted, false);
  t.mock.timers.tick(1);
  await rejection;
  assert.equal(signal.aborted, true);
  assert.equal(source.cancellations.length, 1);
});

test("installed firmware inspection follows the ESP-IDF partition, otadata and app formats", async () => {
  // Vectors from binascii.crc32(struct.pack("<I", seq), 0xFFFFFFFF), the formula of ESP-IDF otatool.py.
  assert.equal(otaSequenceCrc(1), 0x4743989a);
  assert.equal(otaSequenceCrc(2), 0x55f63774);
  assert.equal(otaSequenceCrc(0x12345678), 0x71d6a731);
  const partitions = parsePartitionTable(PARTITION_TABLE);
  assert.deepEqual(partitions.map(({ label, offset }) => [label, offset]),
    partitionRows().map(([name, , , offset]) => [name, Number(offset)]));
  const boot = (entries) => selectBootPartition(partitions, entries).label;
  assert.equal(boot([]), "ota_0", "no valid otadata boots ota_0");
  assert.equal(boot([otadataEntry(1), otadataEntry(2)]), "ota_1");
  assert.equal(boot([otadataEntry(3), otadataEntry(2)]), "ota_0");
  assert.equal(boot([otadataEntry(1), otadataEntry(2, 2, 0)]), "ota_0", "wrong CRC");
  assert.equal(boot([otadataEntry(1), otadataEntry(2, 3)]), "ota_0", "ESP_OTA_IMG_INVALID");
  assert.equal(boot([otadataEntry(1), otadataEntry(2, 4)]), "ota_0", "ESP_OTA_IMG_ABORTED");
  assert.deepEqual(parseAppDescription(appHead("tesla-key-esp32", "1.6.2")), { version: "1.6.2", project: "tesla-key-esp32" });
  const withoutMagic = appHead();
  withoutMagic.writeUInt32LE(0, 0x20);
  assert.equal(parseAppDescription(withoutMagic), null);
  assert.equal(parseAppDescription(appHead("ÿbad")).project, "", "non-printable names are dropped");

  const reads = [];
  const flash = flashImage({ 0x8000: PARTITION_TABLE, 0xf000: otadataEntry(1), 0x10000: otadataEntry(2),
    0x210000: appHead("tesla-key-esp32", "1.6.2") });
  const loader = { async readFlash(offset, length) { reads.push([offset, length]); return flash(offset, length); } };
  assert.deepEqual(await inspectInstalledFirmware(loader, PARTITION_TABLE, "4MB"), {
    layoutMatches: true, nvsMatches: true, firmware: { version: "1.6.2", project: "tesla-key-esp32" }, slot: "ota_1"
  });
  assert.deepEqual(reads, [[0x8000, 0x100], [0xf000, 32], [0x10000, 32], [0x210000, 0x70]],
    "the table is read in small steps up to its end, then otadata and the app head");
  // gen_esp32part.py ends the table with an MD5 entry; nothing after it is compared or read.
  const rowsBytes = partitionRows().length * 32;
  const withMd5 = Buffer.from(PARTITION_TABLE);
  withMd5.writeUInt16LE(0xebeb, rowsBytes);
  assert.equal(partitionTableLength(withMd5), rowsBytes + 32);
  assert.equal(partitionTableLength(PARTITION_TABLE), rowsBytes);
  assert.equal(partitionTableLength(PARTITION_TABLE.subarray(0, rowsBytes)), undefined, "the end is not reached yet");
  assert.equal(partitionTableLength(new Uint8Array(32).fill(0xff)), 0);
  const md5Loader = { async readFlash(offset, length) { return flashImage({ 0x8000: withMd5, 0x20000: appHead() })(offset, length); } };
  assert.equal((await inspectInstalledFirmware(md5Loader, withMd5, "4MB")).layoutMatches, true);
  assert.equal((await inspectInstalledFirmware(md5Loader, PARTITION_TABLE, "4MB")).layoutMatches, false, "a different end differs");
  const erasedReads = [];
  const erased = { async readFlash(offset, length) { erasedReads.push(offset); return new Uint8Array(length).fill(0xff); } };
  assert.deepEqual(await inspectInstalledFirmware(erased, PARTITION_TABLE, "4MB"),
    { layoutMatches: false, nvsMatches: false, firmware: null, slot: "" });
  assert.deepEqual(erasedReads, [0x8000], "an erased table ends in its first step");
  const short = { async readFlash() { return new Uint8Array(4); } };
  await assert.rejects(inspectInstalledFirmware(short, PARTITION_TABLE, "4MB"), { name: "FlashReadError" });
  // An older 8 MB layout booting from a slot above 4 MB on a board whose flash ID is not recognised.
  const highSlot = partitionTableBytes(partitionRows().map((row) => row[0] === "ota_1" ? [...row.slice(0, 3), "0x410000", row[4]] : row));
  const highLoader = { async readFlash(offset, length) {
    return flashImage({ 0x8000: highSlot, 0xf000: otadataEntry(2), 0x410000: appHead() })(offset, length);
  } };
  await assert.rejects(inspectInstalledFirmware(highLoader, PARTITION_TABLE, ""),
    { name: "FlashReadError", message: /0x410000 lies beyond the assumed 4MB flash/ });
  assert.equal(configurationKeepable({ error: "0x410000 lies beyond the assumed 4MB flash." }), true,
    "unread is unknown, not foreign");
  assert.equal((await inspectInstalledFirmware(highLoader, PARTITION_TABLE, "8MB")).firmware.project, "tesla-key-esp32");
  assert.equal((await inspectInstalledFirmware(loader, PARTITION_TABLE, "")).slot, "ota_1", "an undetected size reads within 4 MB");
});

test("Flash keeps the configuration only for this firmware on an nvs-compatible layout", () => {
  const rows = partitionRows();
  const moved = partitionTableBytes(rows.map((row) => row[0] === "nvs" ? [...row.slice(0, 3), "0x9000", "0x5000"] : row));
  assert.equal(hasCompatibleNvs(parsePartitionTable(PARTITION_TABLE)), true);
  assert.equal(hasCompatibleNvs(parsePartitionTable(OLDER_TABLE)), true, "an older layout without coredump keeps nvs");
  assert.equal(hasCompatibleNvs(parsePartitionTable(moved)), false);
  const ours = { project: "tesla-key-esp32", version: "1.6.2" };
  assert.equal(configurationKeepable({ nvsMatches: true, firmware: ours }), true);
  assert.equal(configurationKeepable({ nvsMatches: true, firmware: { project: "micropython", version: "1.24.1" } }), false);
  assert.equal(configurationKeepable({ nvsMatches: false, firmware: ours }), false);
  assert.equal(configurationKeepable({ nvsMatches: false, firmware: null }), false, "an erased chip has nothing to keep");
  assert.equal(configurationKeepable({ error: "Reading flash at 0x8000 timed out." }), true, "unknown is not foreign");
  assert.equal(configurationKeepable(null), true, "a device that was not inspected keeps both choices");
});

test("device probing checks chip and installed firmware, then resets into it and closes the port", async () => {
  const fake = fakeEsptool();
  const port = openPort();
  const steps = [];
  const result = await probeDevice({
    port, manifest: firmwareFixture().document, expectedPartitionTables: new Map([["ESP32-S3", PARTITION_TABLE]]),
    TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader, delay: noDelay, onStep: (step) => steps.push(step)
  });
  assert.equal(result.chipFamily, "ESP32-S3");
  assert.equal(result.flashSize, "4MB");
  assert.equal(result.build.parts[0].path, "bootloader-esp32s3.bin");
  assert.deepEqual(result.installed, {
    layoutMatches: true, nvsMatches: true, firmware: { version: "1.6.2", project: "tesla-key-esp32" }, slot: "ota_0"
  });
  assert.deepEqual(steps, ["Reading partition table and app header"]);
  assert.deepEqual(fake.calls, ["transport", "loader", "main", "flashId", "detectFlashSize", "disconnect"]);
  assert.deepEqual(port.signals, EN_PULSE, "the chip leaves the flasher stub with IO0 high");
  assert.equal(port.readable, null);
  assert.equal(serialPortLease(port), undefined);
  assert.equal(fake.loaderOptions[0].serialOptions.bufferSize, 0x10000, "far above a 4-KiB flash read block");
});

test("probing refuses a pre-ECO3 classic ESP32 and still releases the port", async () => {
  const fake = fakeEsptool("ESP32", { chipRevision: 1 });
  const port = openPort({ usbVendorId: 0x10c4, usbProductId: 0xea60 });
  await assert.rejects(probeDevice({
    port, manifest: firmwareFixture().document, TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader, delay: noDelay
  }), (error) => error.name === "UnsupportedChipRevisionError" && /ECO3/.test(error.message) && /v3\.0/.test(error.message));
  assert.deepEqual(fake.calls, ["transport", "loader", "main", "flashId", "disconnect"]);
  assert.deepEqual(port.signals, EN_PULSE);
  const accepted = fakeEsptool("ESP32", { chipRevision: 3 });
  const result = await probeDevice({
    port: openPort({ usbVendorId: 0x10c4, usbProductId: 0xea60 }), manifest: firmwareFixture().document,
    TransportCtor: accepted.Transport, ESPLoaderCtor: accepted.Loader, delay: noDelay
  });
  assert.equal(result.build.parts[0].path, "bootloader-esp32.bin");
});

test("probe and install refuse a detected flash below 4 MB before downloading or writing", async (t) => {
  for (const [capacity, accepted, reported] of [["2MB", false], ["4MB", true, "4MB"], ["16MB", true, "16MB"],
    [undefined, true, ""], ["512KB", false]]) {
    await t.test(String(capacity), async () => {
      const fixture = firmwareFixture();
      const offer = await fixture.load();
      const fake = fakeEsptool("ESP32-S3", { flashSize: capacity });
      const probing = probeDevice({ port: openPort(), manifest: offer.manifest, TransportCtor: fake.Transport,
        ESPLoaderCtor: fake.Loader, delay: noDelay });
      if (accepted) assert.equal((await probing).flashSize, reported);
      else await assert.rejects(probing, { name: "UnsupportedFlashSizeError", message: /at least 4 MB/ });
      const flashing = flashDevice({ port: openPort(), offer, eraseFirst: true, TransportCtor: fake.Transport,
        ESPLoaderCtor: fake.Loader, fetchImpl: fixture.fetch, delay: noDelay });
      if (accepted) await flashing;
      else await assert.rejects(flashing, { name: "UnsupportedFlashSizeError" });
      assert.equal(fake.calls.includes("erase"), accepted);
      assert.equal(writesOf(fake.calls).length, accepted ? 4 : 0);
      assert.equal(fixture.seen.length, accepted ? 5 : 1);
    });
  }
});

test("device probing resets when loader startup fails after the flasher stub starts", async () => {
  const startupError = new Error("Flash ID validation failed after stub startup");
  const fake = fakeEsptool();
  fake.Loader.prototype.main = async function main() { fake.calls.push("main:stub-running"); throw startupError; };
  const port = openPort();
  await assert.rejects(probeDevice({
    port, manifest: firmwareFixture().document, TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader, delay: noDelay
  }), (error) => error === startupError);
  assert.deepEqual(fake.calls, ["transport", "loader", "main:stub-running", "disconnect"]);
  assert.deepEqual(port.signals, EN_PULSE);
});

test("device probing times out without closing or releasing a still-running native startup", async () => {
  const fake = fakeEsptool();
  fake.Loader.prototype.main = async function main() { fake.calls.push("main"); return new Promise(() => {}); };
  const port = openPort();
  await assert.rejects(probeDevice({
    port, manifest: firmwareFixture().document, TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader,
    timeoutMs: 10, cleanupTimeoutMs: 10, delay: noDelay
  }), (error) => error.name === "DeviceProbeTimeoutError" && /native USB port/.test(error.message) &&
    /stays reserved/.test(error.message));
  assert.deepEqual(fake.calls, ["transport", "loader", "main"]);
  assert.equal(serialPortLease(port).phase, "probe", "the pending startup keeps the port reserved");
});

// esptool-js 0.7.0's connect loop: up to seven attempts, each pulsing the reset lines through the
// transport (IO0 low: download mode) and then waiting for a sync answer whose failure it swallows.
function connectLoopEsptool() {
  const calls = [];
  const started = deferred();
  class Transport {
    constructor(port) { this.port = port; this.device = port; }
    async setDTR(value) { await this.port.setSignals({ dataTerminalReady: value }); }
    async setRTS(value) { await this.port.setSignals({ requestToSend: value }); }
    async write() { calls.push("sync-write"); }
    read() { return new Promise(() => {}); }
    async disconnect() { calls.push("disconnect"); await this.port.close(); }
  }
  class Loader {
    constructor(options) { this.options = options; }
    async main() {
      const transport = this.options.transport;
      for (let attempt = 1; attempt <= 7; attempt++) {
        await transport.setDTR(false);
        calls.push(`attempt ${attempt}`);
        await transport.setRTS(true);
        await transport.setDTR(true);
        await transport.setRTS(false);
        try {
          await transport.write(Uint8Array.of(0xc0));
          started.resolve();
          await transport.read(100);
        } catch (_error) { /* esptool-js retries a failed sync */ }
      }
      throw new Error("Failed to connect with the device");
    }
  }
  return { calls, started, Transport, Loader };
}

test("a cancelled check stops esptool's reset pulses and leaves the chip in its firmware", async () => {
  const fake = connectLoopEsptool();
  const port = openPort();
  let operation;
  const probing = probeDevice({
    port, manifest: firmwareFixture().document, TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader,
    delay: noDelay, onOwnership: (state) => { operation = state; }
  });
  await fake.started.promise;
  const signalsBeforeCancel = port.signals.length;
  operation.cancel(Object.assign(new Error("USB disconnected"), { name: "DeviceProbeAbortedError" }));
  await assert.rejects(probing, { name: "DeviceProbeAbortedError" });
  await operation.settled;
  assert.deepEqual(fake.calls.filter((call) => call.startsWith("attempt")), ["attempt 1"],
    "no further connect attempt after the cancellation");
  assert.deepEqual(port.signals.slice(signalsBeforeCancel), EN_PULSE,
    "the only line change afterwards is the firmware-mode reset");
  assert.equal(port.readable, null);
  assert.equal(serialPortLease(port), undefined);
});

test("a check that never synchronises still resets the chip out of download mode", async () => {
  const fake = connectLoopEsptool();
  fake.Transport.prototype.read = async () => { throw new Error("no sync answer"); };
  const port = openPort();
  await assert.rejects(probeDevice({
    port, manifest: firmwareFixture().document, TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader, delay: noDelay
  }), /Failed to connect with the device/);
  assert.equal(fake.calls.filter((call) => call.startsWith("attempt")).length, 7);
  assert.deepEqual(port.signals.slice(-2), EN_PULSE, "the wrong port of a dual-port board is not left halted");
});

test("an install without the running flasher stub writes nothing", async () => {
  const fixture = firmwareFixture();
  const offer = await fixture.load();
  const fake = fakeEsptool();
  fake.Loader.prototype.main = async function main() { fake.calls.push("main"); };
  await assert.rejects(flashDevice({ port: openPort(), offer, eraseFirst: true, TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader, fetchImpl: fixture.fetch, delay: noDelay }),
  { name: "FlasherStubError", message: /ROM loader could erase NVS/ });
  assert.equal(fixture.seen.length, 1, "nothing downloaded");
  assert.equal(fake.calls.includes("erase"), false);
  assert.deepEqual(writesOf(fake.calls), []);
});

test("a cancellation ends esptool's long buffered read at once and releases the port", async () => {
  const fixture = firmwareFixture();
  const offer = await fixture.load();
  const fake = fakeEsptool();
  const eraseStarted = deferred();
  // A chip erase waits up to 120 s for its answer in esptool-js's read loop.
  fake.Transport.prototype.read = () => new Promise(() => {});
  fake.Loader.prototype.eraseFlash = async function eraseFlash() {
    fake.calls.push("erase");
    eraseStarted.resolve();
    await this.options.transport.read(120000);
  };
  const port = openPort();
  let operation;
  const flashing = flashDevice({ port, offer, eraseFirst: true, TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader,
    fetchImpl: fixture.fetch, delay: noDelay, onOwnership: (state) => { operation = state; } });
  await eraseStarted.promise;
  operation.cancel(Object.assign(new Error("USB disconnected"), { name: "FlashAbortedError" }));
  await assert.rejects(flashing, { name: "FlashAbortedError" });
  const startedAt = Date.now();
  await operation.settled;
  assert.ok(Date.now() - startedAt < 250, "the port is not held for the command timeout");
  assert.equal(serialPortLease(port), undefined);
  assert.deepEqual(writesOf(fake.calls), []);
});

test("a cancelled reset pulse still releases EN", async () => {
  const signals = [];
  let cancelled = false;
  const port = { async setSignals(value) { signals.push(value); cancelled = true; } };
  await assert.rejects(resetToUserFirmware(port, noDelay, () => { if (cancelled) throw new Error("cancelled"); }), /cancelled/);
  assert.deepEqual(signals, EN_PULSE, "the chip is never left held in reset");
});

test("a stuck transport cleanup cannot leave the page busy forever", async () => {
  const fake = fakeEsptool();
  fake.Transport.prototype.disconnect = async () => new Promise(() => {});
  const startedAt = Date.now();
  await assert.rejects(probeDevice({
    port: openPort(), manifest: firmwareFixture().document, TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader,
    timeoutMs: 10, cleanupTimeoutMs: 10, delay: noDelay
  }), { name: "SerialCleanupTimeoutError" });
  assert.ok(Date.now() - startedAt < 250, "probe and cleanup deadlines release the UI promptly");
});

test("flash writes ota_0 first, activates it last, erases only when selected and restarts via EN", async () => {
  for (const eraseFirst of [false, true]) {
    const fixture = firmwareFixture();
    const offer = await fixture.load();
    const fake = fakeEsptool();
    const port = openPort();
    const states = [];
    const result = await flashDevice({
      port, offer, eraseFirst, TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader, fetchImpl: fixture.fetch,
      delay: noDelay,
      onState(state) {
        states.push(state);
        if (["erasing", "writing"].includes(state.stage)) assert.equal(fixture.seen.length, 5, "all parts verified first");
      }
    });
    assert.equal(result.chipFamily, "ESP32-S3");
    assert.equal(fake.calls.includes("erase"), eraseFirst);
    if (eraseFirst) assert.ok(fake.calls.indexOf("erase") < fake.calls.findIndex(Array.isArray));
    assert.deepEqual(writesOf(fake.calls), [[0], [0x8000], [0x20000], [0xf000]], "one range per call, OTA selector last");
    assert.ok(fake.writes.every((write) => write.eraseAll === false && write.flashSize === "keep"),
      "signed image headers stay unchanged");
    assert.equal(fake.calls.at(-1), "disconnect");
    assert.deepEqual(port.signals, EN_PULSE);
    assert.ok(states.some((state) => state.stage === "activating"));
    assert.deepEqual(states.at(-1), { stage: "restarting", percentage: 100, message: "Starting firmware" });
  }
});

test("a cancellation during a write never starts the next range or activates the new app", async () => {
  const fixture = firmwareFixture();
  const offer = await fixture.load();
  const fake = fakeEsptool();
  const finishWrite = deferred();
  const writeStarted = deferred();
  fake.Loader.prototype.writeFlash = async function writeFlash(options) {
    fake.calls.push(["write", options.fileArray.map((file) => file.address)]);
    writeStarted.resolve();
    await finishWrite.promise;
    options.reportProgress(0, 1, 1);
  };
  let operation;
  const port = openPort();
  const flashing = flashDevice({
    port, offer, eraseFirst: false, TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader,
    fetchImpl: fixture.fetch, delay: noDelay, onOwnership: (state) => { operation = state; }
  });
  await writeStarted.promise;
  operation.cancel(Object.assign(new Error("USB disconnected"), { name: "FlashAbortedError" }));
  await assert.rejects(flashing, (error) => error.name === "FlashAbortedError" && /stays reserved/.test(error.message));
  assert.equal(serialPortLease(port).phase, "flash", "the in-flight write keeps the port reserved");
  finishWrite.resolve();
  const outcome = await operation.settled;
  assert.equal(outcome.error.name, "FlashAbortedError");
  assert.deepEqual(writesOf(fake.calls), [[0]]);
  assert.equal(fake.calls.at(-1), "disconnect");
  assert.equal(serialPortLease(port), undefined);
});

test("a cancellation after a range's last progress report still stops before the next range", async () => {
  const fixture = firmwareFixture();
  const offer = await fixture.load();
  const fake = fakeEsptool();
  const finish = deferred();
  const reported = deferred();
  // Like esptool-js: the last block is reported before flashDeflFinish and the MD5 check return.
  fake.Loader.prototype.writeFlash = async function writeFlash(options) {
    fake.calls.push(["write", options.fileArray.map((file) => file.address)]);
    options.reportProgress(0, 1, 1);
    reported.resolve();
    await finish.promise;
  };
  let operation;
  const flashing = flashDevice({
    port: openPort(), offer, eraseFirst: false, TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader,
    fetchImpl: fixture.fetch, delay: noDelay, onOwnership: (state) => { operation = state; }
  });
  await reported.promise;
  operation.cancel(Object.assign(new Error("USB disconnected"), { name: "FlashAbortedError" }));
  finish.resolve();
  await assert.rejects(flashing, { name: "FlashAbortedError" });
  await operation.settled;
  assert.deepEqual(writesOf(fake.calls), [[0]], "the next range never starts");
});

test("a cancellation racing the completed OTA-selector write still settles as installed", async () => {
  const fixture = firmwareFixture();
  const offer = await fixture.load();
  const fake = fakeEsptool();
  const finalWrite = deferred();
  const finalStarted = deferred();
  const write = fake.Loader.prototype.writeFlash;
  fake.Loader.prototype.writeFlash = async function writeFlash(options) {
    if (options.fileArray[0].address !== 0xf000) return write.call(this, options);
    fake.calls.push(["write", [0xf000]]);
    finalStarted.resolve();
    await finalWrite.promise;
  };
  let operation;
  const flashing = flashDevice({
    port: openPort(), offer, eraseFirst: false, TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader,
    fetchImpl: fixture.fetch, delay: noDelay, onOwnership: (state) => { operation = state; }
  });
  await finalStarted.promise;
  finalWrite.resolve();
  operation.cancel(Object.assign(new Error("USB disconnect raced the final write"), { name: "AbortError" }));
  await assert.rejects(flashing, { name: "AbortError" });
  const outcome = await operation.settled;
  assert.equal(outcome.value.chipFamily, "ESP32-S3", "the selected app is reported as written");
  assert.deepEqual(writesOf(fake.calls), [[0], [0x8000], [0x20000], [0xf000]]);
});

test("a flash timeout aborts the active Web Serial writer and rejects later writes", async () => {
  const fixture = firmwareFixture();
  const offer = await fixture.load();
  const fake = fakeEsptool();
  const pendingWrite = deferred();
  let writerCount = 0;
  const writer = {
    write(bytes) { fake.calls.push(["wire-write", Array.from(bytes)]); return pendingWrite.promise; },
    abort(reason) { fake.calls.push("writer-abort"); pendingWrite.reject(reason); return Promise.resolve(); },
    releaseLock() { fake.calls.push("release-lock"); }
  };
  const port = openPort();
  port.writable = { getWriter() { writerCount += 1; return writer; } };
  fake.Transport.prototype.slipWriter = (bytes) => bytes;
  fake.Loader.prototype.writeFlash = async function writeFlash(options) {
    fake.calls.push(["write", options.fileArray.map((file) => file.address)]);
    const transport = this.options.transport;
    try {
      await transport.write(Uint8Array.from([1]));
    } catch (error) {
      await assert.rejects(transport.write(Uint8Array.from([2])), { name: "FlashTimeoutError" });
      fake.calls.push("late-write-blocked");
      throw error;
    }
  };
  let operation;
  await assert.rejects(flashDevice({
    port, offer, eraseFirst: false, TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader, fetchImpl: fixture.fetch,
    timeoutMs: 20, cleanupTimeoutMs: 100, delay: noDelay, onOwnership: (state) => { operation = state; }
  }), { name: "FlashTimeoutError" });
  await operation.settled;
  assert.equal(writerCount, 1);
  assert.equal(fake.calls.filter((call) => Array.isArray(call) && call[0] === "wire-write").length, 1);
  assert.ok(fake.calls.indexOf("writer-abort") < fake.calls.indexOf("release-lock"));
  assert.ok(fake.calls.indexOf("late-write-blocked") < fake.calls.indexOf("disconnect"));
  assert.deepEqual(writesOf(fake.calls), [[0]], "nothing after the first range");
  assert.equal(fake.calls.at(-1), "disconnect");
});

test("a flash timeout aborts pending downloads before anything is erased or written", async () => {
  const fixture = firmwareFixture();
  const offer = await fixture.load();
  const fake = fakeEsptool();
  const requests = [];
  await assert.rejects(flashDevice({
    port: openPort(), offer, eraseFirst: true, TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader,
    fetchImpl(url, options) {
      const pending = deferred();
      requests.push({ url, signal: options.signal, ...pending });
      return pending.promise;
    },
    timeoutMs: 20, cleanupTimeoutMs: 100, delay: noDelay
  }), { name: "FlashTimeoutError" });
  await until(() => requests.every((request) => request.signal.aborted));
  assert.equal(requests.length, 4);
  assert.equal(fake.calls.includes("erase"), false);
  assert.deepEqual(writesOf(fake.calls), []);
});

test("integrity, revision and offer failures stop before erase or write", async (t) => {
  await t.test("corrupt part", async () => {
    const fixture = firmwareFixture();
    const offer = await fixture.load();
    fixture.payloads.get("tesla-key-esp32-esp32s3.bin")[0x80] ^= 1;
    const fake = fakeEsptool();
    await assert.rejects(flashDevice({ port: openPort(), offer, eraseFirst: true, TransportCtor: fake.Transport,
      ESPLoaderCtor: fake.Loader, fetchImpl: fixture.fetch, delay: noDelay }), { name: "FirmwareIntegrityError", message: /Reload this page/ });
    assert.equal(fake.calls.includes("erase"), false);
    assert.deepEqual(writesOf(fake.calls), []);
  });
  await t.test("pre-ECO3", async () => {
    const fixture = firmwareFixture();
    const offer = await fixture.load();
    const fake = fakeEsptool("ESP32", { chipRevision: 1 });
    await assert.rejects(flashDevice({ port: openPort({ usbVendorId: 0x10c4, usbProductId: 0xea60 }), offer, eraseFirst: true,
      TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader, fetchImpl: fixture.fetch, delay: noDelay }),
    { name: "UnsupportedChipRevisionError" });
    assert.equal(fake.calls.includes("erase"), false);
    assert.equal(fixture.seen.length, 1, "nothing downloaded");
  });
  await t.test("raw manifest", async () => {
    const fixture = firmwareFixture();
    const fake = fakeEsptool();
    await assert.rejects(flashDevice({ port: openPort(), offer: { manifestUrl: MANIFEST_URL, manifest: fixture.document },
      eraseFirst: true, TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader, fetchImpl: fixture.fetch }),
    /verified firmware offer is required/);
    assert.deepEqual(fake.calls, []);
  });
});

test("a flash write failure is preserved after the reset and close", async () => {
  const fixture = firmwareFixture();
  const offer = await fixture.load();
  const fake = fakeEsptool();
  const writeError = new Error("write failed");
  fake.Loader.prototype.writeFlash = async () => { throw writeError; };
  const port = openPort();
  await assert.rejects(flashDevice({ port, offer, eraseFirst: false, TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader, fetchImpl: fixture.fetch, delay: noDelay }), (error) => error === writeError);
  assert.deepEqual(port.signals, EN_PULSE);
  assert.equal(fake.calls.at(-1), "disconnect");
});

test("a failed restart after a complete write is logged, not reported as a failed install", async () => {
  const fixture = firmwareFixture();
  const offer = await fixture.load();
  const fake = fakeEsptool();
  const port = openPort();
  port.setSignals = async () => { throw new Error("no reset lines"); };
  const logs = [];
  const result = await flashDevice({ port, offer, eraseFirst: false, TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader, fetchImpl: fixture.fetch, delay: noDelay, onLog: (text) => logs.push(text) });
  assert.equal(result.chipFamily, "ESP32-S3");
  assert.match(logs.join(""), /Automatic restart unavailable: no reset lines/);
});

test("cleanup composition preserves DOMException and frozen primary errors without masking them", async (t) => {
  const underlying = new Error("original lower-level cause");
  for (const primary of [
    new DOMException("native write denied", "OperationError"),
    Object.freeze(new Error("frozen native write failed", { cause: underlying }))
  ]) {
    await t.test(primary.name, async () => {
      const fixture = firmwareFixture();
      const offer = await fixture.load();
      const fake = fakeEsptool();
      const originalMessage = primary.message;
      const closing = new DOMException("native close denied", "NetworkError");
      fake.Loader.prototype.writeFlash = async () => { throw primary; };
      fake.Transport.prototype.disconnect = async () => { throw closing; };
      // The write failed on an open port, so the failed close leaves it open and reserved.
      const port = { readable: {}, writable: {}, getInfo() { return {}; }, async setSignals() {} };
      await assert.rejects(flashDevice({ port, offer, eraseFirst: false, TransportCtor: fake.Transport,
        ESPLoaderCtor: fake.Loader, fetchImpl: fixture.fetch, delay: noDelay }), (error) => {
        assert.notEqual(error, primary);
        assert.equal(error.name, primary.name);
        assert.equal(error.cause, primary);
        assert.equal(error.cleanupError, closing);
        assert.ok(error.message.includes(originalMessage));
        assert.match(error.message, /native close denied.*remains reserved/);
        return true;
      });
      assert.equal(primary.message, originalMessage);
      assert.equal(primary.cleanupError, undefined);
      assert.equal(serialPortLease(port).phase, "quarantined");
    });
  }
});

test("serial monitor reset keeps IO0 high while EN is pulsed", async () => {
  const calls = [];
  await resetToUserFirmware({ async setSignals(signals) { calls.push(signals); } },
    async (milliseconds) => { calls.push(`wait:${milliseconds}`); });
  assert.deepEqual(calls, [EN_PULSE[0], "wait:100", EN_PULSE[1], "wait:250"]);
  await assert.rejects(resetToUserFirmware({}), /cannot reset the ESP automatically/);
});

// A Web Serial port whose esptool-js transport opens it in connect() and closes it in disconnect().
function lifecyclePort(name) {
  const port = {
    name, writable: null, opens: 0, closes: 0, signals: [], forgets: 0, written: [],
    flashIds: 0, writes: 0, isOpen: false, stream: null,
    getInfo() { return {}; },
    // Like Web Serial: while the port is open, a cancelled input stream is replaced by a new one.
    get readable() {
      if (!this.isOpen) return null;
      this.stream ||= this.readerSource || new ReadableStream({
        start: (controller) => { this.readController = controller; },
        cancel: () => { this.stream = null; }
      });
      return this.stream;
    },
    async open(options) {
      this.opens++;
      this.openOptions = options;
      if (this.isOpen) throw new DOMException("The port is already open.", "InvalidStateError");
      if (this.openFailures > 0) {
        this.openFailures--;
        throw new DOMException("Failed to open serial port.", "NetworkError");
      }
      if (this.openWait) await this.openWait.promise;
      this.isOpen = true;
      this.writable = this.heldWriter || {};
    },
    async close() {
      this.closes++;
      if (this.closeWait) await this.closeWait.promise;
      if (this.closeError) throw this.closeError;
      if (this.readable?.locked) throw new Error("native readable stream is still locked");
      this.isOpen = false;
      this.stream = null;
      this.writable = null;
    },
    async setSignals(value) {
      this.signals.push(value);
      this.signalEntered?.resolve();
      if (this.signalWait) await this.signalWait.promise;
    },
    async forget() { this.forgets++; this.onForget?.(); }
  };
  return port;
}

function lifecycleEsptool() {
  class Transport {
    constructor(port) { this.port = port; this.device = port; }
    async connect(baudRate, options) { await this.device.open({ baudRate, ...options }); }
    async disconnect() { await this.port.close(); }
  }
  class Loader {
    constructor(options) { this.options = options; this.port = options.transport.port; }
    async main() {
      await this.options.transport.connect(115200, this.options.serialOptions);
      if (this.port.mainError) throw this.port.mainError;
      if (this.options.enableTracing) {
        this.options.terminal.write(`native ${this.port.name}`);
        this.options.transport.trace(`main packet ${this.port.name}`);
      }
      this.port.mainEntered?.resolve();
      if (this.port.mainWait) await this.port.mainWait.promise;
      this.chip = { CHIP_NAME: "ESP32-S3" };
      this.IS_STUB = true;
      this.options.terminal.writeLine(`native ${this.port.name}`);
    }
    async flashId() { this.port.flashIds++; }
    async detectFlashSize() { return "4MB"; }
    async readFlash(offset, length) { return (this.port.flash || INSTALLED_FLASH)(offset, length); }
    async eraseFlash() { this.port.erases = (this.port.erases || 0) + 1; }
    async writeFlash(options) {
      this.port.writes++;
      this.port.written.push(options.fileArray.map((file) => file.address));
      this.port.writeEntered?.resolve();
      if (this.port.writeWait) await this.port.writeWait.promise;
      options.reportProgress(0, 1, 1);
      this.options.terminal.writeLine(`late write ${this.port.name}`);
      if (this.port.writeError) throw this.port.writeError;
      if (this.port.writeErrorAt === options.fileArray[0].address) throw new Error(`write at 0x${this.port.writeErrorAt.toString(16)} failed`);
    }
  }
  return { Transport, Loader };
}

test("a check or install with keepOpen hands the open port on without a reset or a close", async () => {
  const fixture = firmwareFixture();
  const offer = await fixture.load();
  const fake = lifecycleEsptool();
  const port = lifecyclePort("A");
  const tables = new Map([["ESP32-S3", PARTITION_TABLE]]);
  const use = () => [port.opens, port.closes, port.signals.length, port.isOpen, Boolean(serialPortLease(port))];
  await probeDevice({ port, manifest: offer.manifest, expectedPartitionTables: tables, TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader, keepOpen: true, delay: noDelay });
  assert.deepEqual(use(), [1, 0, 0, true, false], "the check leaves the port open and unowned");
  assert.equal(port.openOptions.bufferSize, 0x10000);
  await flashDevice({ port, offer, eraseFirst: false, TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader,
    fetchImpl: fixture.fetch, keepOpen: true, delay: noDelay });
  assert.deepEqual(use(), [1, 0, 0, true, false], "the install reuses it and leaves it open");
  assert.deepEqual(port.written, [[0], [0x8000], [0x20000], [0xf000]]);
  await probeDevice({ port, manifest: offer.manifest, expectedPartitionTables: tables, TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader, delay: noDelay });
  assert.deepEqual(use(), [1, 1, 2, false, false], "without keepOpen the check resets the chip and closes");
  port.mainError = new Error("no answer");
  await assert.rejects(probeDevice({ port, manifest: offer.manifest, TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader, keepOpen: true, delay: noDelay }), /no answer/);
  assert.deepEqual(use(), [2, 2, 4, false, false],
    "a failed check never hands a port on, and still resets the chip out of download mode");
  assert.deepEqual(port.signals.slice(-2), EN_PULSE);
});

test("the shared transport uses an open port and ends esptool-js's read loop without closing", async () => {
  const port = lifecyclePort("A");
  const Shared = sharedPortTransport(lifecycleEsptool().Transport);
  assert.equal(sharedPortTransport(lifecycleEsptool().Transport) === Shared, false, "one subclass per base class");
  const transport = new Shared(port);
  let opened = 0;
  transport.onOpened = () => { opened++; };
  await transport.connect(115200, { bufferSize: 0x10000 });
  await transport.connect(115200, { bufferSize: 0x10000 });
  assert.deepEqual([port.opens, opened, transport.baudrate], [1, 2, 115200], "an open port is not opened again");
  // esptool-js's readLoop holds a reader and releases it once its read ends.
  transport.reader = port.readable.getReader();
  const loop = transport.reader.read().finally(() => transport.reader.releaseLock());
  await transport.release();
  assert.deepEqual(await loop, { value: undefined, done: true });
  assert.deepEqual([port.isOpen, port.closes, port.readable.locked, transport.reader], [true, 0, false, undefined]);
});

function installerDom() {
  const elements = new Map();
  const makeElement = () => ({
    dataset: {}, textContent: "", className: "", hidden: true, disabled: false, href: "", title: "",
    // Scrolling stands in for layout: one pixel per character of text.
    scrollTop: 0, clientHeight: 0, get scrollHeight() { return this.textContent.length; },
    style: { setProperty(name, value) { this[name] = value; }, removeProperty(name) { delete this[name]; } },
    attributes: new Map(), listeners: new Map(), children: [],
    addEventListener(name, callback) { this.listeners.set(name, callback); },
    setAttribute(name, value) { this.attributes.set(name, value); },
    append(child) { this.children.push(child); this.textContent += child.textContent; },
    replaceChildren() { this.children = []; this.textContent = ""; },
    ownerDocument: { createElement: () => makeElement() }
  });
  const root = {
    dataset: {}, attributes: new Map(),
    setAttribute(name, value) { this.attributes.set(name, value); },
    querySelector(selector) {
      if (!elements.has(selector)) elements.set(selector, makeElement());
      return elements.get(selector);
    }
  };
  return { root, element: (id) => root.querySelector(`#${id}`) };
}

// The log's rendered lines with the level from their class (status, ok, info, warning, error).
function logLines(dom, level) {
  return dom.element("serial-monitor-output").children
    .map((line) => ({ text: line.textContent.trimEnd(), level: /installer-monitor-line-(\w+)/.exec(line.className)[1] }))
    .filter((line) => !level || line.level === level)
    .map((line) => line.text);
}
const ownLines = (dom) => dom.element("serial-monitor-output").children
  .filter((line) => line.className.includes("installer-monitor-line-own")).map((line) => line.textContent.trimEnd());

// The ownership tests drive the live log themselves; followLog is exercised separately. `forget`
// stands in for a browser whose SerialPort can forget its permission.
async function attachedLifecycle(t, ports, options = {}) {
  const {
    followLog = false, forget = false, trace = false, pathname = "/tesla-key-esp32/", fixture = firmwareFixture(),
    fetchImpl = null, cryptoImpl = webcrypto
  } = options;
  // Clean flash is confirmed unless a test supplies its own answer, or none for the browser default.
  const confirmEraseImpl = Object.hasOwn(options, "confirmEraseImpl") ? options.confirmEraseImpl : () => true;
  const priorSecureContext = globalThis.isSecureContext;
  globalThis.isSecureContext = true;
  t.after(() => { globalThis.isSecureContext = priorSecureContext; });
  const dom = installerDom();
  const fake = lifecycleEsptool();
  const serial = {
    choice: ports[0], grants: [...ports], chooserCalls: 0, listeners: new Map(),
    async requestPort() { this.chooserCalls++; return this.choice; },
    async getPorts() { return [...this.grants]; },
    addEventListener(name, callback) {
      if (!this.listeners.has(name)) this.listeners.set(name, []);
      this.listeners.get(name).push(callback);
    }
  };
  for (const port of ports) port.onForget = () => { serial.grants = serial.grants.filter((granted) => granted !== port); };
  const controller = attachWebInstaller({
    root: dom.root, serial, TransportCtor: fake.Transport, ESPLoaderCtor: fake.Loader,
    fetchImpl: fetchImpl || fixture.fetch, cryptoImpl, followLog, trace, delay: noDelay, confirmEraseImpl,
    locationImpl: { href: `https://example.test${pathname}`, pathname },
    SerialPortCtor: forget ? { prototype: { forget() {} } } : undefined
  });
  await controller.ready;
  return { controller, serial, dom, fixture, fake };
}

function lifecycleUi(dom) {
  return JSON.stringify({
    dataset: dom.root.dataset,
    monitor: dom.element("serial-monitor-live").textContent,
    output: dom.element("serial-monitor-output").textContent
  });
}

const encode = (text) => new TextEncoder().encode(text);

test("connect follows the firmware log, reset restarts it and an install hands it back to the log", async (t) => {
  const a = lifecyclePort("A");
  const { controller, dom } = await attachedLifecycle(t, [a], { followLog: true });
  await controller.connect();
  assert.equal(dom.element("serial-monitor-live").textContent, "Live");
  const portUse = () => ({ opens: a.opens, closes: a.closes });
  assert.deepEqual(portUse(), { opens: 1, closes: 0 }, "the check hands its open port to the log");
  assert.deepEqual(a.signals, EN_PULSE, "only the log resets the chip, once");
  assert.equal(a.openOptions.bufferSize, 0x10000, "the one open uses esptool-js's large buffer");
  assert.deepEqual(logLines(dom, "warning"), []);
  assert.deepEqual([dom.element("connect-button").hidden, dom.element("disconnect-button").hidden], [true, false]);
  assert.deepEqual(["reset-button", "flash-button", "clean-flash-button"].map((id) => dom.element(id).disabled),
    [false, false, false], "the live log yields the port to every action");
  const status = logLines(dom, "status").join("\n");
  assert.match(status, /\] ESP32-S3 · 4MB · Web Serial$/m);
  assert.match(status, /\] Installed: tesla-key-esp32 1\.6\.2 \(ota_0\); configuration can be kept$/m);
  assert.ok(ownLines(dom).every((line) => /^\[[^\]]+\] /.test(line)), "only the installer's own lines are marked");
  assert.ok(ownLines(dom).length > 3);

  await controller.reset();
  assert.equal(a.signals.length, 4, "reset pulses EN with IO0 high again");
  assert.deepEqual(portUse(), { opens: 1, closes: 0 }, "Reset keeps the log's port");

  await dom.element("flash-button").listeners.get("click")();
  assert.equal(dom.root.dataset.finished, "true");
  assert.deepEqual(a.written, [[0], [0x8000], [0x20000], [0xf000]]);
  assert.equal(a.erases, undefined, "Flash keeps the configuration");
  assert.equal(dom.element("serial-monitor-live").textContent, "Live", "the log follows the new firmware");
  assert.ok(serialPortLease(a), "the live log owns the port again");
  assert.deepEqual(portUse(), { opens: 1, closes: 0 }, "the install borrows the open port");
  assert.match(logLines(dom, "status").join("\n"), /\] Flash 1\.6\.3: configuration kept$/m);
  assert.match(logLines(dom, "status").join("\n"), /\] Installed 1\.6\.3$/m);
  a.readController.enqueue(encode("I (498) main: BOOT version=1.6.3 elf=3f2a reset=usb safe_mode=no\n"));
  await drainNativeTasks();
  assert.match(logLines(dom, "ok").at(-1), /\] Booted 1\.6\.3$/);
  a.readController.enqueue(encode("I (512) main: BOOT version=1.6.3 elf=3f2a reset=usb safe_mode=no\n"));
  await drainNativeTasks();
  assert.equal(logLines(dom, "ok").length, 1, "the check reports once per install");

  await dom.element("clean-flash-button").listeners.get("click")();
  assert.deepEqual([a.erases, a.writes], [1, 8], "Clean flash erases before writing");
  assert.match(logLines(dom, "status").join("\n"), /\] Clean flash 1\.6\.3: erase all$/m);
  assert.equal(dom.element("serial-monitor-live").textContent, "Live");
  await controller.disconnect();
  assert.equal(serialPortLease(a), undefined);
  assert.equal(a.readable, null);
  assert.deepEqual(portUse(), { opens: 1, closes: 1 }, "Disconnect is the only close");
  assert.equal(dom.element("connect-button").hidden, false);
});

test("the boot check warns about another version or none within 20 seconds", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const a = lifecyclePort("A");
  const { controller, dom } = await attachedLifecycle(t, [a], { followLog: true });
  await controller.connect();
  await controller.install();
  a.readController.enqueue(encode("I (105) app_init: App version:      1.6.2\n"));
  await drainNativeTasks();
  assert.match(logLines(dom, "warning").at(-1), /\] Booted 1\.6\.2, expected 1\.6\.3$/);
  await controller.install();
  t.mock.timers.tick(19999);
  await drainNativeTasks();
  assert.equal(logLines(dom, "warning").length, 1);
  t.mock.timers.tick(1);
  assert.match(logLines(dom, "warning").at(-1), /\] 1\.6\.3 not seen within 20 s; press Reset or check the board$/);
  await controller.install();
  await controller.disconnect();
  t.mock.timers.tick(20000);
  assert.equal(logLines(dom, "warning").length, 2, "a disconnect ends the pending check silently");
});

test("the boot check reads the device's own lines, even when an installer line splits the display", async (t) => {
  const a = lifecyclePort("A");
  const { controller, dom } = await attachedLifecycle(t, [a], { followLog: true });
  await controller.connect();
  await controller.install();
  a.readController.enqueue(encode("I (105) app_init: App vers"));
  await drainNativeTasks();
  await controller.reset();
  a.readController.enqueue(encode("ion:      1.6.3\n"));
  await drainNativeTasks();
  assert.match(logLines(dom, "ok").at(-1), /\] Booted 1\.6\.3$/);
  assert.deepEqual(logLines(dom, "warning"), []);
});

test("a failure while the OTA selector is written says the new firmware may already be selected", async (t) => {
  const a = lifecyclePort("A");
  a.writeErrorAt = 0xf000;
  const { controller, dom } = await attachedLifecycle(t, [a], { followLog: true });
  await controller.connect();
  await controller.install();
  assert.match(logLines(dom, "error").at(-1), /\] Flash failed: write at 0xf000 failed$/);
  assert.match(logLines(dom, "warning").at(-1), /\] The OTA selector was being written; 1\.6\.3 may already be selected\. Press Reset and read the version in the log$/);
  a.writeErrorAt = 0x20000;
  await controller.install();
  assert.equal(logLines(dom, "warning").filter((line) => /OTA selector/.test(line)).length, 1,
    "a failure before the selector write makes no such claim");
});

test("Disconnect waits for a reset pulse on the live log", async (t) => {
  const a = lifecyclePort("A");
  const { controller, dom } = await attachedLifecycle(t, [a], { followLog: true });
  await controller.connect();
  a.signalWait = deferred();
  a.signalEntered = deferred();
  const resetting = controller.reset();
  await a.signalEntered.promise;
  assert.equal(dom.element("disconnect-button").disabled, true);
  a.signalWait.resolve();
  a.signalWait = null;
  await resetting;
  assert.equal(dom.element("disconnect-button").disabled, false);
  assert.deepEqual(a.signals.slice(-2), EN_PULSE);
});

test("the subtitle shows version, channel and source commit once the offer is verified", async (t) => {
  const release = await attachedLifecycle(t, [lifecyclePort("A")]);
  assert.equal(release.dom.element("firmware-version-value").textContent, "1.6.3");
  assert.equal(release.dom.element("firmware-channel-value").textContent, "Release");
  assert.equal(release.dom.element("firmware-source-link").textContent, SOURCE_SHA.slice(0, 12));
  assert.equal(release.dom.element("firmware-source-link").href, `https://github.com/0Bu/tesla-key-esp32/commit/${SOURCE_SHA}`);
  assert.equal(release.dom.element("firmware-meta").hidden, false);
  const preview = await attachedLifecycle(t, [lifecyclePort("B")], { pathname: "/tesla-key-esp32/PR/412/",
    fixture: firmwareFixture("1.6.3-PR-412") });
  assert.equal(preview.dom.element("firmware-version-value").textContent, "1.6.3-PR-412");
  assert.equal(preview.dom.element("firmware-channel-value").textContent, "PR #412 preview");
  const dev = await attachedLifecycle(t, [lifecyclePort("C")], { fixture: firmwareFixture("1.6.4-dev.7") });
  assert.equal(dev.dom.element("firmware-channel-value").textContent, "Dev");
  assert.equal(release.fixture.seen.length, 5, "the manifest and the four offered partition tables");
});

test("other firmware, a moved nvs or an erased chip offer only Clean flash", async (t) => {
  const moved = partitionTableBytes(partitionRows().map((row) => row[0] === "nvs" ? [...row.slice(0, 3), "0x9000", "0x5000"] : row));
  for (const [name, flash, installed] of [
    ["other firmware", flashImage({ 0x8000: PARTITION_TABLE, 0x20000: appHead("micropython", "v1.24.1") }),
      "micropython v1.24.1 \\(ota_0\\); other firmware"],
    ["moved nvs", flashImage({ 0x8000: moved, 0x20000: appHead() }), "tesla-key-esp32 1\\.6\\.2 \\(ota_0\\); NVS layout differs"],
    ["erased chip", flashImage({}), "no app; NVS layout differs"]
  ]) {
    await t.test(name, async (st) => {
      const a = lifecyclePort("A");
      a.flash = flash;
      const { controller, dom } = await attachedLifecycle(st, [a]);
      await controller.connect();
      assert.match(logLines(dom, "status").join("\n"), new RegExp(`\\] Installed: ${installed}$`, "m"));
      assert.match(logLines(dom, "warning").at(-1), /\] Clean flash required$/);
      assert.deepEqual([dom.element("flash-button").hidden, dom.element("flash-button").disabled], [true, true],
        "only Clean flash is offered");
      assert.deepEqual([dom.element("clean-flash-button").hidden, dom.element("clean-flash-button").disabled], [false, false]);
      await controller.install();
      assert.match(logLines(dom, "error").at(-1), /\] Flash refused: Clean flash required$/);
      assert.equal(a.writes, 0, "the API refuses a keep-configuration install as well");
      await dom.element("clean-flash-button").listeners.get("click")();
      assert.deepEqual([a.erases, a.writes], [1, 4]);
      assert.deepEqual([dom.element("flash-button").hidden, dom.element("flash-button").disabled], [false, false],
        "after installing, updates keep the configuration again");
    });
  }
});

test("Clean flash asks every time, and a declined confirmation leaves the board and its log untouched", async (t) => {
  const a = lifecyclePort("A");
  const messages = [];
  const { controller, dom, fixture } = await attachedLifecycle(t, [a], {
    followLog: true, confirmEraseImpl: (message) => { messages.push(message); return false; }
  });
  await controller.connect();
  const downloads = fixture.seen.length;
  const before = { opens: a.opens, closes: a.closes, signals: a.signals.length };
  await dom.element("clean-flash-button").listeners.get("click")();
  await dom.element("clean-flash-button").listeners.get("click")();
  assert.equal(messages.length, 2, "each attempt asks again");
  assert.match(messages[0], /WiFi, VIN, MQTT settings, the private pairing key and BLE sessions/);
  assert.match(messages[0], /Back up secret NVS/);
  assert.match(logLines(dom, "status").at(-1), /\] Clean flash cancelled; nothing was written$/);
  assert.deepEqual([a.erases, a.writes, fixture.seen.length], [undefined, 0, downloads], "no download, erase or write");
  assert.deepEqual({ opens: a.opens, closes: a.closes, signals: a.signals.length }, before, "the log keeps the port");
  assert.equal(dom.element("serial-monitor-live").textContent, "Live");
  assert.equal(dom.element("clean-flash-button").disabled, false);
  await dom.element("flash-button").listeners.get("click")();
  assert.equal(messages.length, 2, "Flash keeps the configuration and never asks");
  assert.equal(a.writes, 4);
});

test("Clean flash fails closed when the browser cannot ask for confirmation", async (t) => {
  const previous = globalThis.confirm;
  globalThis.confirm = undefined;
  t.after(() => { globalThis.confirm = previous; });
  const a = lifecyclePort("A");
  const { controller, dom } = await attachedLifecycle(t, [a], { confirmEraseImpl: undefined });
  await controller.connect();
  await controller.install({ eraseFirst: true });
  assert.deepEqual([a.erases, a.writes], [undefined, 0]);
  assert.match(logLines(dom, "status").at(-1), /Clean flash cancelled/);
});

test("an older layout with the same nvs keeps Flash and says the table will be rewritten", async (t) => {
  const a = lifecyclePort("A");
  a.flash = flashImage({ 0x8000: OLDER_TABLE, 0x20000: appHead("tesla-key-esp32", "1.4.9") });
  const { controller, dom } = await attachedLifecycle(t, [a]);
  await controller.connect();
  assert.match(logLines(dom, "status").join("\n"), /\] Installed: tesla-key-esp32 1\.4\.9 \(ota_0\); configuration can be kept$/m);
  assert.match(logLines(dom, "warning").at(-1), /\] Partition table differs; Flash also writes the current table$/);
  assert.deepEqual(["flash-button", "clean-flash-button"].map((id) => [dom.element(id).hidden, dom.element(id).disabled]),
    [[false, false], [false, false]]);
  await controller.install();
  assert.deepEqual([a.erases, a.written.at(-3)], [undefined, [0x8000]], "the new table is written, nvs is not erased");
});

test("a stuck flash read leaves the firmware unknown, keeps both actions and releases the port", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const a = lifecyclePort("A");
  const reads = [];
  a.flash = (offset) => { reads.push(offset); return new Promise(() => {}); };
  const { controller, dom } = await attachedLifecycle(t, [a]);
  const connecting = controller.connect();
  await until(() => reads.length === 1);
  t.mock.timers.tick(2499);
  await drainNativeTasks();
  assert.equal(dom.root.dataset.connected, "false", "each read gets 2.5 s");
  t.mock.timers.tick(1);
  await connecting;
  assert.deepEqual(reads, [0x8000], "no further reads after a timeout");
  assert.equal(dom.root.dataset.connected, "true");
  assert.equal(serialPortLease(a), undefined, "the probe closed the port normally");
  assert.match(logLines(dom, "warning").join("\n"), /\] Installed firmware unknown: Reading flash at 0x8000 timed out\.$/);
  assert.deepEqual(["flash-button", "clean-flash-button"].map((id) => [dom.element(id).hidden, dom.element(id).disabled]),
    [[false, false], [false, false]], "unknown is not foreign: Flash and Clean flash stay available");
  a.flash = null;
  await controller.install();
  assert.deepEqual([a.erases, a.writes], [undefined, 4], "a keep-configuration update is allowed");
});

test("the clicked flash button shows the progress as fill and label, cleared afterwards", async (t) => {
  const a = lifecyclePort("A");
  const { controller, dom } = await attachedLifecycle(t, [a], { followLog: true });
  await controller.connect();
  const [flash, clean] = ["flash-button", "clean-flash-button"].map((id) => dom.element(id));
  for (const [button, other, label] of [[clean, flash, "Clean flash"], [flash, clean, "Flash"]]) {
    a.writeWait = deferred();
    a.writeEntered = deferred();
    const installing = button.listeners.get("click")();
    await a.writeEntered.promise;
    assert.deepEqual([button.dataset.progress, button.style["--progress"], button.textContent], ["0", "0%", `${label} 0%`]);
    assert.equal(other.dataset.progress, undefined, "only the clicked button fills");
    assert.deepEqual([dom.element("serial-monitor-live").textContent, dom.element("serial-monitor-live").dataset.state],
      ["Flashing", "busy"]);
    assert.equal(dom.element("disconnect-button").disabled, true, "Disconnect waits for the flash");
    a.writeWait.resolve();
    await installing;
    assert.deepEqual([button.dataset.progress, button.style["--progress"], button.textContent], [undefined, undefined, label]);
  }
  a.writeError = new Error("write failed");
  a.writeWait = null;
  await flash.listeners.get("click")();
  assert.equal(flash.dataset.progress, undefined, "cleared after a failure too");
  assert.match(logLines(dom, "error").at(-1), /\] Flash failed: write failed$/);
  assert.equal(dom.element("serial-monitor-live").textContent, "Live", "a failed install is followed on the same port");
  assert.deepEqual([a.opens, a.closes], [1, 0]);
});

test("the log follows new lines only while it is scrolled to its end", async (t) => {
  const a = lifecyclePort("A");
  const { controller, dom } = await attachedLifecycle(t, [a], { followLog: true });
  await controller.connect();
  const log = dom.element("serial-monitor-output");
  log.clientHeight = 50;
  const scrollTo = (top) => { log.scrollTop = top; log.listeners.get("scroll")(); };
  const write = async (text) => { a.readController.enqueue(encode(text)); await drainNativeTasks(); };
  await write("first line\n");
  assert.equal(log.scrollTop, log.scrollHeight, "at the end the log follows");
  scrollTo(10);
  await write("second line\n");
  assert.equal(log.scrollTop, 10, "scrolled up, the view holds");
  scrollTo(log.scrollHeight - log.clientHeight - 8);
  await write("third line\n");
  assert.equal(log.scrollTop, log.scrollHeight, "back at the end it follows again");
  // Older lines are dropped above a held view; it keeps showing the same lines.
  await write(`${"x".repeat(59)}\n`.repeat(800));
  scrollTo(45000);
  const total = log.textContent.length + 60 * 1100;
  await write(`${"y".repeat(59)}\n`.repeat(1100));
  assert.ok(log.textContent.length < total, "the log was trimmed");
  assert.equal(log.scrollTop, 45000 - (total - log.textContent.length));
  // An incomplete device line stays apart from the installer's own time-stamped lines.
  await write("partial device text");
  await controller.reset();
  assert.ok(!logLines(dom).some((line) => /partial device text.*Reset to firmware/.test(line)));
  assert.ok(logLines(dom).includes("partial device text"), "the unfinished line is shown on its own first");
});

test("attached monitor cancellation captures A's delayed open and cannot close or reset connected B", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const a = lifecyclePort("A");
  const b = lifecyclePort("B");
  const { controller, serial, dom } = await attachedLifecycle(t, [a, b]);
  await controller.connect();
  const probeSignals = a.signals.length;
  a.openWait = deferred();
  const starting = controller.startMonitor();
  await drainNativeTasks();
  assert.equal(dom.root.attributes.get("aria-busy"), "true");
  assert.equal(dom.element("reset-button").attributes.get("aria-busy"), "true");
  const disconnecting = controller.disconnect();
  await drainNativeTasks();
  t.mock.timers.tick(2000);
  await disconnecting;
  await starting;
  assert.ok(serialPortLease(a), "the delayed native open must retain A's lease");
  serial.choice = b;
  await controller.connect();
  const before = lifecycleUi(dom);
  a.openWait.resolve();
  await drainNativeTasks();
  assert.equal(a.signals.length, probeSignals, "cancelled monitor opening must never reset A");
  assert.equal(a.readable, null);
  assert.equal(serialPortLease(a), undefined);
  assert.equal(controller.getSelectedPort(), b);
  assert.equal(b.closes, 1, "only B's own compatibility probe may close B");
  assert.equal(lifecycleUi(dom), before);
});

test("attached timed-out probe retains its lease until late native open and cleanup actually settle", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const a = lifecyclePort("A");
  const b = lifecyclePort("B");
  a.openWait = deferred();
  const { controller, serial, dom } = await attachedLifecycle(t, [a, b]);
  const probing = controller.connect();
  await drainNativeTasks();
  t.mock.timers.tick(19999);
  await drainNativeTasks();
  assert.equal(dom.root.dataset.connected, "false");
  assert.doesNotMatch(dom.element("serial-monitor-output").textContent, /still pending/, "the chip probe and the firmware check share 20 s");
  t.mock.timers.tick(1);
  await probing;
  assert.match(logLines(dom, "error").join("\n"), /native serial operation is still pending/);
  assert.equal(a.closes, 0, "cleanup must not race an unresolved native startup");
  assert.ok(serialPortLease(a));
  assert.equal(dom.root.dataset.busy, "true", "native cleanup stays visibly busy after the foreground deadline");
  assert.equal(dom.element("connect-button").attributes.get("aria-busy"), "true");
  serial.choice = b;
  await controller.connect();
  const before = lifecycleUi(dom);
  a.openWait.resolve();
  await drainNativeTasks();
  assert.equal(a.readable, null);
  assert.equal(a.flashIds, 0, "no later probe stages may run after timeout");
  assert.equal(serialPortLease(a), undefined);
  assert.equal(controller.getSelectedPort(), b);
  assert.equal(lifecycleUi(dom), before);
});

test("attached pending probe cleanup reports failure and prevents reuse until actual closure", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const a = lifecyclePort("A");
  a.closeWait = deferred();
  const { controller, dom } = await attachedLifecycle(t, [a]);
  const probing = controller.connect();
  await drainNativeTasks();
  t.mock.timers.tick(2000);
  await probing;
  assert.match(logLines(dom, "error").join("\n"), /cleanup timed out/);
  assert.equal(dom.root.dataset.connected, "false");
  assert.equal(dom.element("flash-button").disabled, true);
  assert.ok(serialPortLease(a));
  await controller.connect();
  assert.match(logLines(dom, "error").at(-1), /\] Probe failed: Port busy: a previous operation is still closing\.$/);
  a.closeWait.resolve();
  await drainNativeTasks();
  assert.equal(serialPortLease(a), undefined);
  assert.equal(dom.root.dataset.connected, "false", "late cleanup must not retroactively certify compatibility");
  a.closeWait = null;
  await controller.connect();
  assert.equal(dom.root.dataset.connected, "true");
});

test("attached rejected flash cleanup retains the original failure and never claims installation success", async (t) => {
  const a = lifecyclePort("A");
  const { controller, dom } = await attachedLifecycle(t, [a]);
  await controller.connect();
  a.writeError = new Error("original native write failure");
  a.closeError = new Error("native close rejected");
  await controller.install();
  assert.match(logLines(dom, "error").join("\n"), /original native write failure.*native close rejected/);
  assert.equal(dom.root.dataset.finished, "false");
  assert.equal(serialPortLease(a).phase, "quarantined");
  assert.equal(dom.element("flash-button").disabled, true);
  assert.equal(dom.element("clean-flash-button").disabled, true);
  assert.equal(a.forgets, 0);
});

test("attached disconnect cannot report a clean stop when native close resolves without closing the port", async (t) => {
  const a = lifecyclePort("A");
  const { controller, dom } = await attachedLifecycle(t, [a]);
  await controller.connect();
  await controller.startMonitor();
  a.close = async () => { a.closes++; };
  await controller.disconnect();
  assert.match(logLines(dom, "error").at(-1), /Disconnect:.*closure could not be confirmed/);
  assert.equal(serialPortLease(a).phase, "quarantined");
});

test("Reset on a live log only pulses the reset line: no close, no reopen", async (t) => {
  const a = lifecyclePort("A");
  const { controller, dom } = await attachedLifecycle(t, [a]);
  await controller.connect();
  await controller.startMonitor();
  const [opens, closes, signals] = [a.opens, a.closes, a.signals.length];
  await controller.reset();
  assert.deepEqual([a.opens - opens, a.closes - closes, a.signals.length - signals], [0, 0, 2]);
  assert.deepEqual(a.signals.slice(-2), EN_PULSE, "EN pulsed with IO0 high");
  assert.equal(dom.element("serial-monitor-live").textContent, "Live");
  assert.ok(logLines(dom, "status").some((line) => /\] Reset to firmware$/.test(line)));
  assert.deepEqual(logLines(dom, "error"), []);
});

test("attached natural monitor EOF releases its real reader and closes without a spurious cancellation error", async (t) => {
  const a = lifecyclePort("A");
  const { controller, dom } = await attachedLifecycle(t, [a]);
  await controller.connect();
  await controller.startMonitor();
  const nativeStream = a.readable;
  a.readController.enqueue(encode("normal completed log"));
  a.readController.close();
  await drainNativeTasks();
  assert.equal(nativeStream.locked, false);
  assert.equal(a.readable, null);
  assert.equal(serialPortLease(a), undefined);
  assert.equal(a.closes, 2);
  assert.equal(dom.element("serial-monitor-live").textContent, "Stopped");
  assert.deepEqual(logLines(dom, "error"), []);
  assert.match(dom.element("serial-monitor-output").textContent, /normal completed log/);
});

test("attached natural monitor read error keeps the native error and still confirms closure", async (t) => {
  const a = lifecyclePort("A");
  const { controller, dom } = await attachedLifecycle(t, [a]);
  await controller.connect();
  await controller.startMonitor();
  const nativeStream = a.readable;
  const primary = new DOMException("native serial read lost", "NetworkError");
  a.readController.error(primary);
  await drainNativeTasks();
  assert.equal(nativeStream.locked, false);
  assert.equal(a.readable, null);
  assert.equal(serialPortLease(a), undefined);
  assert.equal(dom.element("serial-monitor-live").textContent, "Error");
  const [stopped] = logLines(dom, "error");
  assert.match(stopped, /Monitor stopped: native serial read lost/);
  assert.doesNotMatch(stopped, /TypeError|stays reserved|remains reserved/);
});

test("attached repeated monitor stop is bounded while native reader cancellation remains owned", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const a = lifecyclePort("A");
  const { controller, dom } = await attachedLifecycle(t, [a]);
  await controller.connect();
  const reading = deferred();
  const cancelling = deferred();
  let cancels = 0;
  a.readerSource = {
    locked: false,
    getReader() {
      this.locked = true;
      return {
        read: () => reading.promise,
        cancel() {
          cancels++;
          reading.resolve({ done: true });
          return cancelling.promise;
        },
        releaseLock: () => { a.readerSource.locked = false; }
      };
    }
  };
  await controller.startMonitor();
  const first = assert.rejects(controller.stopMonitor(), { name: "SerialCleanupTimeoutError" });
  const second = assert.rejects(controller.stopMonitor(), { name: "SerialCleanupTimeoutError" });
  await drainNativeTasks();
  t.mock.timers.tick(2000);
  await first;
  await second;
  assert.equal(cancels, 1, "repeated stop must reuse the captured cancellation promise");
  assert.equal(serialPortLease(a).phase, "cleanup");
  assert.equal(dom.root.attributes.get("aria-busy"), "true");
  assert.equal(dom.element("reset-button").disabled, true);
  assert.equal(a.closes, 1, "only the earlier successful probe has closed A yet");
  cancelling.resolve();
  await drainNativeTasks();
  assert.equal(a.readable, null);
  assert.equal(serialPortLease(a), undefined);
  assert.equal(dom.element("serial-monitor-live").textContent, "Stopped");
});

test("attached late probe error and cleanup for A cannot change B's completed connection", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const a = lifecyclePort("A");
  const b = lifecyclePort("B");
  a.mainWait = deferred();
  a.mainEntered = deferred();
  const { controller, serial, dom } = await attachedLifecycle(t, [a, b]);
  const probing = controller.connect();
  await a.mainEntered.promise;
  const disconnecting = controller.disconnect();
  await drainNativeTasks();
  t.mock.timers.tick(2000);
  await disconnecting;
  await probing;
  serial.choice = b;
  await controller.connect();
  const before = lifecycleUi(dom);
  a.mainWait.reject(new Error("obsolete A probe failure"));
  await drainNativeTasks();
  assert.equal(a.readable, null);
  assert.equal(controller.getSelectedPort(), b);
  assert.equal(lifecycleUi(dom), before);
});

test("attached late flash progress, terminal text and success for A cannot alter B or release B mid-flash", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const a = lifecyclePort("A");
  const b = lifecyclePort("B");
  const { controller, serial, dom } = await attachedLifecycle(t, [a, b]);
  await controller.connect();
  a.writeWait = deferred();
  a.writeEntered = deferred();
  const flashing = controller.install();
  await a.writeEntered.promise;
  serial.choice = b;
  const disconnecting = controller.disconnect();
  await drainNativeTasks();
  t.mock.timers.tick(2000);
  await disconnecting;
  await flashing;
  await controller.connect();
  const before = lifecycleUi(dom);
  a.writeWait.resolve();
  await drainNativeTasks();
  assert.equal(a.readable, null);
  assert.deepEqual(a.written, [[0]], "the cancelled install never starts another range");
  assert.equal(controller.getSelectedPort(), b);
  assert.equal(lifecycleUi(dom), before);
  assert.equal(dom.root.dataset.finished, "false");
});

test("attached delayed reset of A cannot signal B or publish a stale success", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const a = lifecyclePort("A");
  const b = lifecyclePort("B");
  const { controller, serial, dom } = await attachedLifecycle(t, [a, b]);
  await controller.connect();
  const probeSignals = a.signals.length;
  a.signalWait = deferred();
  a.signalEntered = deferred();
  const resetting = controller.reset();
  await a.signalEntered.promise;
  const disconnecting = controller.disconnect();
  await drainNativeTasks();
  t.mock.timers.tick(2000);
  await disconnecting;
  await resetting;
  serial.choice = b;
  a.signalWait = null;
  await controller.connect();
  const before = lifecycleUi(dom);
  const bSignals = b.signals.length;
  b.signalWait = null;
  a.signalEntered = null;
  await drainNativeTasks();
  assert.equal(a.signals.length, probeSignals + 1, "cancellation must prevent the next reset signal");
  assert.equal(b.signals.length, bSignals, "B only saw its own probe's reset");
  assert.equal(lifecycleUi(dom), before);
});

test("attached stale monitor reader A cannot append into B's monitor output", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const a = lifecyclePort("A");
  const b = lifecyclePort("B");
  const { controller, serial, dom } = await attachedLifecycle(t, [a, b]);
  await controller.connect();
  const reading = deferred();
  a.readerSource = {
    locked: false,
    getReader() {
      this.locked = true;
      return {
        read: () => reading.promise,
        async cancel() {},
        releaseLock: () => { a.readerSource.locked = false; }
      };
    }
  };
  await controller.startMonitor();
  const disconnecting = controller.disconnect();
  await drainNativeTasks();
  t.mock.timers.tick(2000);
  await disconnecting;
  assert.ok(serialPortLease(a));
  serial.choice = b;
  await controller.connect();
  const before = lifecycleUi(dom);
  reading.resolve({ value: encode("obsolete A output\n"), done: false });
  await drainNativeTasks();
  assert.equal(a.readable, null);
  assert.equal(lifecycleUi(dom), before);
});

test("unplugging during an install aborts its downloads and leaves the late responses inert", async (t) => {
  const fixture = firmwareFixture();
  const requests = [];
  let holdParts = false;
  const a = lifecyclePort("A");
  const { controller, serial, dom } = await attachedLifecycle(t, [a], {
    fixture,
    fetchImpl(url, options) {
      if (!holdParts) return fixture.fetch(url, options);
      const pending = deferred();
      requests.push({ url, signal: options.signal, ...pending });
      return pending.promise;
    }
  });
  await controller.connect();
  holdParts = true;
  const installing = controller.install();
  await until(() => requests.length === 4);
  await serial.listeners.get("disconnect")[0]({ port: a });
  await installing;
  requests.forEach(({ url, resolve }) => resolve(fixture.fetch(url, {})));
  await drainNativeTasks();
  assert.ok(requests.every((request) => request.signal.aborted));
  assert.equal(dom.root.dataset.connected, "false");
  assert.match(logLines(dom, "status").join("\n"), /\] USB disconnected$/m);
  assert.equal(a.writes, 0);
  assert.equal(a.erases, undefined);
  await until(() => !serialPortLease(a));
  assert.equal(a.readable, null);
});

test("the attached installer freezes the displayed offer and reports drift without changing the version", async (t) => {
  const a = lifecyclePort("A");
  const { controller, dom, fixture } = await attachedLifecycle(t, [a]);
  assert.equal(dom.element("firmware-version-value").textContent, "1.6.3");
  await controller.connect();
  assert.equal(dom.element("flash-button").disabled, false);
  fixture.payloads.get("tesla-key-esp32-esp32s3.bin")[0x80] ^= 1;
  await controller.install();
  assert.match(logLines(dom, "error").join("\n"), /Flash failed:.*Reload this page/);
  assert.equal(dom.element("firmware-version-value").textContent, "1.6.3");
  assert.equal(dom.root.dataset.finished, "false");
  assert.equal(a.erases, undefined);
  assert.equal(a.writes, 0);
});

test("the attached installer stays disabled without WebCrypto verification or Web Serial", async (t) => {
  const fixture = firmwareFixture();
  const { dom } = await attachedLifecycle(t, [lifecyclePort("A")], { fixture, cryptoImpl: {} });
  assert.match(logLines(dom, "error").join("\n"), /Manifest error: Firmware verification is unavailable/);
  assert.equal(fixture.seen.length, 0);
  assert.deepEqual(["connect-button", "reset-button", "flash-button", "clean-flash-button"].map((id) => dom.element(id).disabled),
    [true, true, true, true]);
  assert.deepEqual(["connect-button", "reset-button", "flash-button", "clean-flash-button"].map((id) => dom.element(id).hidden),
    [false, false, false, false], "unavailable controls are greyed out, not hidden");
  assert.equal(dom.element("disconnect-button").hidden, true, "Disconnect shares Connect's place");

  const prior = globalThis.isSecureContext;
  globalThis.isSecureContext = true;
  t.after(() => { globalThis.isSecureContext = prior; });
  const plain = installerDom();
  const controller = attachWebInstaller({
    root: plain.root, serial: undefined, TransportCtor: class {}, ESPLoaderCtor: class {},
    fetchImpl: firmwareFixture().fetch, cryptoImpl: webcrypto, delay: noDelay,
    locationImpl: { href: "https://example.test/tesla-key-esp32/", pathname: "/tesla-key-esp32/" }
  });
  await controller.ready;
  assert.match(logLines(plain, "error")[0], /\] Web Serial unavailable \(Chrome or Edge on a desktop required\)$/);
  assert.equal(plain.element("connect-button").disabled, true);
  await controller.reconnectPaired();
});

test("the Disconnect button also unpairs exactly the connected port", async (t) => {
  const [a, b] = ["A", "B"].map((name) => lifecyclePort(name));
  const { controller, serial, dom } = await attachedLifecycle(t, [a, b], { forget: true });
  serial.choice = b;
  await controller.connect();
  const chooserCalls = serial.chooserCalls;
  await dom.element("disconnect-button").listeners.get("click")();
  assert.deepEqual([a.forgets, b.forgets], [0, 1], "only the connected port is released");
  assert.equal(serial.chooserCalls, chooserCalls, "without asking which port");
  assert.deepEqual(serial.grants, [a]);
  assert.equal(b.readable, null);
  assert.equal(controller.getSelectedPort(), null);
  assert.equal(dom.element("connect-button").hidden, false);
  assert.match(dom.element("serial-monitor-output").textContent, /\] Port unpaired$/m);
  assert.deepEqual(logLines(dom, "error"), []);
  await dom.element("disconnect-button").listeners.get("click")();
  assert.deepEqual([a.forgets, b.forgets], [0, 1], "nothing connected: nothing to unpair");
});

test("Disconnect during a check waits for the port before unpairing it", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  const a = lifecyclePort("A");
  a.mainWait = deferred();
  a.mainEntered = deferred();
  const { controller, dom } = await attachedLifecycle(t, [a], { forget: true });
  const probing = controller.connect();
  await a.mainEntered.promise;
  const disconnecting = dom.element("disconnect-button").listeners.get("click")();
  await drainNativeTasks();
  t.mock.timers.tick(2000);
  await drainNativeTasks();
  assert.equal(controller.getSelectedPort(), null);
  assert.ok(serialPortLease(a), "esptool-js is still trying to connect");
  assert.equal(a.forgets, 0, "not while the cancelled probe owns the port");
  a.mainWait.resolve();
  await probing;
  await disconnecting;
  assert.equal(serialPortLease(a), undefined);
  assert.equal(a.forgets, 1, "unpaired once the port is free");
  assert.match(logLines(dom, "status").at(-1), /\] Port unpaired$/);
});

test("Disconnect stops a live log before unpairing; unplugging and older browsers only disconnect", async (t) => {
  const a = lifecyclePort("A");
  const { controller, serial, dom } = await attachedLifecycle(t, [a], { forget: true });
  await controller.connect();
  await controller.startMonitor();
  assert.equal(dom.element("serial-monitor-live").textContent, "Live");
  await dom.element("disconnect-button").listeners.get("click")();
  assert.equal(a.forgets, 1);
  assert.equal(a.readable, null, "the log was stopped and the port closed before forgetting");
  assert.equal(serialPortLease(a), undefined);
  assert.equal(dom.element("serial-monitor-live").textContent, "Stopped");

  serial.grants = [a];
  await controller.connect();
  await serial.listeners.get("disconnect")[0]({ port: a });
  assert.equal(controller.getSelectedPort(), null, "unplugging disconnects");
  assert.equal(a.forgets, 1, "but keeps the pairing");

  const b = lifecyclePort("B");
  const plain = await attachedLifecycle(t, [b]);
  await plain.controller.connect();
  await plain.dom.element("disconnect-button").listeners.get("click")();
  assert.equal(b.forgets, 0, "without SerialPort.forget nothing is unpaired");
  assert.equal(plain.controller.getSelectedPort(), null);
  assert.deepEqual(logLines(plain.dom, "error"), []);
});

test("Disconnect during the check's hand-over still resets the chip and closes the port it handed on", async (t) => {
  const a = lifecyclePort("A");
  // esptool-js still holds a writer, so the hand-over waits for it.
  a.heldWriter = { locked: true };
  const { controller, dom } = await attachedLifecycle(t, [a], { followLog: true });
  const connecting = controller.connect();
  await until(() => a.flashIds === 1 && serialPortLease(a)?.phase === "cleanup");
  const disconnecting = controller.disconnect();
  a.heldWriter.locked = false;
  await connecting;
  await disconnecting;
  assert.deepEqual([a.isOpen, a.opens, a.closes], [false, 1, 1], "handed on during the Disconnect, then closed by it");
  assert.deepEqual(a.signals, EN_PULSE, "the chip left in the flasher is reset into the firmware before the close");
  assert.equal(serialPortLease(a), undefined);
  assert.deepEqual(logLines(dom, "error"), []);
});

test("Disconnect during the check with the live log resets the chip into the firmware and closes", async (t) => {
  const a = lifecyclePort("A");
  a.mainWait = deferred();
  a.mainEntered = deferred();
  const { controller } = await attachedLifecycle(t, [a], { followLog: true });
  const probing = controller.connect();
  await a.mainEntered.promise;
  const disconnecting = controller.disconnect();
  a.mainWait.resolve();
  await probing;
  await disconnecting;
  await until(() => !serialPortLease(a));
  assert.deepEqual([a.isOpen, a.closes, a.signals], [false, 1, EN_PULSE],
    "a cancelled check never hands its port on: it resets the chip and closes");
});

test("?trace adds esptool-js's packet trace to the log as whole lines, and only then", async (t) => {
  const a = lifecyclePort("A");
  const traced = await attachedLifecycle(t, [a], { trace: true });
  await traced.controller.connect();
  assert.ok(logLines(traced.dom).some((line) => /^TRACE \+\d+ ms main packet A$/.test(line)));
  assert.ok(logLines(traced.dom).some((line) => line === "native A"), "esptool's own line stays apart from the trace");
  const b = lifecyclePort("B");
  const plain = await attachedLifecycle(t, [b]);
  await plain.controller.connect();
  assert.doesNotMatch(plain.dom.element("serial-monitor-output").textContent, /TRACE/);
});

test("a single paired port reconnects without the chooser after a reload or a replug", async (t) => {
  const [a, b] = ["A", "B"].map((name) => lifecyclePort(name));
  const { controller, serial, dom } = await attachedLifecycle(t, [a, b], { forget: true });
  await controller.reconnectPaired();
  assert.deepEqual([controller.getSelectedPort(), serial.chooserCalls], [null, 0], "two paired ports are left to the chooser");
  serial.grants = [];
  await controller.reconnectPaired();
  assert.equal(controller.getSelectedPort(), null, "nothing paired: nothing to reconnect");
  serial.grants = [b];
  await controller.reconnectPaired();
  assert.deepEqual([controller.getSelectedPort(), serial.chooserCalls], [b, 0], "the paired port is used without asking");
  assert.deepEqual([dom.element("connect-button").hidden, dom.element("disconnect-button").hidden], [true, false]);
  assert.match(logLines(dom, "status").join("\n"), /\] Probe \(paired port\)$/m);
  assert.equal(b.opens, 1, "the check is the only open");
  await controller.reconnectPaired();
  assert.equal(b.opens, 1, "an existing connection is kept");

  await dom.element("disconnect-button").listeners.get("click")();
  await serial.listeners.get("connect")[0]();
  assert.equal(controller.getSelectedPort(), null, "Disconnect unpaired B, so plugging it in again does nothing");
  serial.grants = [a];
  await serial.listeners.get("connect")[0]();
  assert.equal(controller.getSelectedPort(), a, "plugging in a paired device reconnects it");

  await controller.disconnect();
  const chooserCalls = serial.chooserCalls;
  await dom.element("connect-button").listeners.get("click")();
  assert.equal(serial.chooserCalls, chooserCalls + 1, "a manual Connect asks even with one paired port");

  await controller.disconnect();
  a.mainError = new Error("no answer from the bootloader");
  await controller.reconnectPaired();
  assert.equal(controller.getSelectedPort(), a);
  assert.match(logLines(dom, "error").at(-1), /Probe failed: no answer from the bootloader/);
  a.mainError = null;
  await serial.listeners.get("connect")[0]();
  assert.equal(controller.isConnected(), true, "after a failed check, plugging the device in again retries");
});

test("a busy paired port is retried quietly by the check itself and gets one line", async (t) => {
  const a = lifecyclePort("A");
  // Like Chrome: a port that failed to open reports "already closed" on close.
  const close = a.close.bind(a);
  a.close = async () => {
    if (!a.readable) throw new DOMException("The port is already closed.", "InvalidStateError");
    return close();
  };
  const { controller, serial, dom } = await attachedLifecycle(t, [a], { forget: true });
  const probes = () => logLines(dom, "status").filter((line) => line.endsWith("Probe (paired port)")).length;

  a.openFailures = 1; // still closing after a reload
  await controller.reconnectPaired();
  assert.equal(controller.isConnected(), true);
  assert.deepEqual([a.opens, probes(), logLines(dom, "error"), logLines(dom, "warning")], [2, 1, [], []],
    "one failed open, then the check; no noise in the log");

  await controller.disconnect();
  a.opens = 0;
  a.openFailures = 3; // held by another tab
  await controller.reconnectPaired();
  assert.deepEqual([controller.isConnected(), a.opens, probes()], [false, 3, 1], "three opens at most, no further probe line");
  assert.match(logLines(dom, "warning").at(-1), /\] Paired port busy \(open in another tab or program\)$/);

  a.opens = 0;
  a.openFailures = 1;
  await dom.element("connect-button").listeners.get("click")();
  assert.match(logLines(dom, "error").at(-1), /\] Probe failed: port busy \(open in another tab or program\)$/);
  assert.equal(serialPortLease(a), undefined, "a port that never opened is not held as reserved");
  assert.equal(serial.chooserCalls, 1);

  a.mainError = new Error("no answer from the bootloader");
  a.opens = 0;
  await controller.reconnectPaired();
  assert.deepEqual([controller.isConnected(), a.opens], [false, 1], "a device that does not answer is not retried");
});

test("an operation cannot take over a port another lease still owns", async (t) => {
  const a = lifecyclePort("A");
  const { controller, dom } = await attachedLifecycle(t, [a]);
  const foreign = acquireSerialPortLease(a, "release");
  await controller.connect();
  assert.match(logLines(dom, "error").at(-1), /Probe failed: Port busy/);
  assert.equal(a.opens, 0);
  releaseSerialPortLease(foreign);
});

test("the installer's flash layout mirrors partitions.csv", () => {
  const rows = new Map(partitionRows().map(([name, , , offset, size]) => [name, { offset: Number(offset), size: Number(size) }]));
  assert.deepEqual(FLASH_LAYOUT.nvs, rows.get("nvs"));
  assert.deepEqual(FLASH_LAYOUT.otadata, rows.get("otadata"));
  assert.deepEqual(FLASH_LAYOUT.phyInit, rows.get("phy_init"));
  assert.deepEqual(FLASH_LAYOUT.coredump, rows.get("coredump"));
  assert.deepEqual(FLASH_LAYOUT.ota0, rows.get("ota_0"));
  assert.equal(FLASH_LAYOUT.partitionTable.offset + FLASH_LAYOUT.partitionTable.size, rows.get("nvs").offset);
  assert.equal(rows.get("ota_1").offset + rows.get("ota_1").size, 0x400000, "the table fills exactly 4 MB");
});

test("the published page keeps one card, one always visible log and the Tesla styling", () => {
  const html = fs.readFileSync(new URL("../docs/index.html", import.meta.url), "utf8");
  for (const id of ["web-installer", "connect-button", "disconnect-button", "reset-button", "flash-button",
    "clean-flash-button", "serial-monitor-output", "serial-monitor-live", "firmware-version-value", "firmware-meta",
    "firmware-channel-value", "firmware-source-link"]) {
    assert.equal((html.match(new RegExp(`id="${id}"`, "g")) || []).length, 1, id);
  }
  // Device controls come first; until the page has checked its offer and a device they are greyed out.
  assert.match(html, /id="connect-button"[\s\S]*id="disconnect-button"[\s\S]*id="reset-button"[\s\S]*id="flash-button"[\s\S]*id="clean-flash-button"/);
  for (const id of ["connect-button", "reset-button", "flash-button", "clean-flash-button"]) {
    const tag = new RegExp(`<button[^>]*id="${id}"[^>]*>`).exec(html)[0];
    assert.match(tag, / disabled>$/, id);
    assert.doesNotMatch(tag, /\shidden\b/, id);
  }
  assert.match(html, /<button[^>]*id="disconnect-button"[^>]*hidden disabled>/);
  assert.match(html, /<div class="installer-group" aria-label="Firmware">\s*<button[^>]*id="flash-button"[^>]*>Flash<\/button>\s*<button[^>]*id="clean-flash-button"[^>]*>Clean flash<\/button>\s*<\/div>/);
  assert.match(html, /id="flash-button"[^>]*title="Keeps WiFi, VIN, pairing key and sessions"/);
  assert.match(html, /id="clean-flash-button"[^>]*title="Erases all flash, including WiFi, VIN and the pairing key; asks for confirmation"/);
  // One card: no step rail, choice tiles, progress bar, success banner, monitor toggle or permission button.
  assert.doesNotMatch(html, /installer-rail|installer-step|install-mode|type="radio"|installer-progress|role="progressbar"|installer-success|serial-monitor-button|release-serial-port|Remove browser permission|page-status/);
  // The log has a fixed height, keeps long lines unwrapped and cannot be collapsed.
  assert.match(html, /\.installer-monitor-output \{[^}]*height:clamp\(240px, 55vh, 480px\);[^}]*overflow-x:auto;[^}]*white-space:pre;/s);
  assert.match(html, /<pre class="installer-monitor-output" id="serial-monitor-output" role="log"><\/pre>/);
  // Progress fills the clicked button; colours set background-color only, so they never wipe it.
  assert.match(html, /\.installer-button\[data-progress\] \{[^}]*opacity:1;[^}]*linear-gradient\(to right, var\(--progress-fill\) var\(--progress, 0%\)/s);
  assert.doesNotMatch(html, /(installer-button[^{]*|#clean-flash-button) \{[^}]*\bbackground:/);
  // Tesla red is reserved for the primary action and errors; installer lines are bold, not red.
  assert.match(html, /--accent:#E82127;/);
  assert.match(html, /\.installer-button-secondary \{ color:var\(--fg\); background-color:var\(--card\); border:1px solid var\(--line\); \}/);
  assert.match(html, /\.installer-monitor-line-status \{ color:var\(--fg\); \}/);
  assert.match(html, /\.installer-monitor-line-own \{ font-weight:600; \}/);
  assert.match(html, /<span class="installer-logo" aria-hidden="true"><svg viewBox="0 0 24 24"><path d="M13 2 4 14h6l-1 8 9-12h-6l1-8z"\/><\/svg><\/span>/);
  // The commit link never leaves a running install.
  assert.match(html, /<a id="firmware-source-link" target="_blank" rel="noopener noreferrer"><\/a>/);
  // Short notes below the card; WiFi and VIN are entered in the setup network, not on this page.
  assert.match(html, /<strong>Clean flash<\/strong> erases WiFi, VIN, MQTT, the pairing key and sessions after a confirmation; then join <code>tesla-key-esp32-setup<\/code> and enter WiFi and VIN\. <strong>Flash<\/strong> keeps all of it\./);
  assert.match(html, /<code>http:\/\/tesla-key-esp32\.local<\/code>\. Key setup and pairing run automatically; when pairing is needed, place a Tesla keycard on the console reader/);
  assert.doesNotMatch(html, /generate the key/i, "the key is created automatically; regenerating it destroys a pairing");
  assert.match(html, /<strong>Connecting restarts the board:<\/strong>/);
  assert.match(html, /<strong>Two USB ports\?<\/strong>/);
  assert.doesNotMatch(html, /type="password"|name="vin"/i);
  // Licence notices ship with the page; every link opens beside a running install.
  assert.match(html, /<a href="\.\/LICENSE" target="_blank" rel="noopener noreferrer">AGPL-3\.0<\/a>/);
  assert.match(html, /<a href="\.\/THIRD_PARTY\.md" target="_blank" rel="noopener noreferrer">Third-party notices<\/a>/);
  assert.doesNotMatch(html, /<a (?![^>]*target="_blank")[^>]*href=/, "no link replaces the installer tab");
  assert.doesNotMatch(html, /\bMIT\b/);
  // Content Security Policy: only same-origin scripts, the one inline stylesheet by hash, no CDN.
  assert.match(html, /<script type="module" src="\.\/installer-bootstrap\.mjs"><\/script>/);
  assert.match(html, /Content-Security-Policy[^>]*script-src 'self'/);
  const style = html.match(/<style>([\s\S]*?)<\/style>/)?.[1];
  assert.ok(style, "the stylesheet is embedded and covered by its CSP hash");
  const styleHash = createHash("sha256").update(style).digest("base64");
  assert.match(html, new RegExp(`style-src 'sha256-${styleHash.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}'`));
  assert.doesNotMatch(html, /unsafe-inline|unsafe-eval/);
  assert.doesNotMatch(html, /<script(?:\s|>)(?![^>]*\bsrc=)/);
  assert.doesNotMatch(html, /cdn\.jsdelivr\.net|unpkg\.com|https:\/\/cdn\./);
  assert.doesNotMatch(html, /esp-web-install-button/);
});

test("the bootstrap loads the vendored esptool-js, honours ?trace and reconnects a paired port", () => {
  const bootstrap = fs.readFileSync(new URL("../docs/installer-bootstrap.mjs", import.meta.url), "utf8");
  assert.match(bootstrap, /from "\.\/vendor\/esptool-js-0\.7\.0\.bundle\.js"/);
  assert.match(bootstrap, /trace: new URLSearchParams\(locationImpl\.search\)\.has\("trace"\)/);
  assert.match(bootstrap, /controller\.ready\.then\(\(\) => controller\.reconnectPaired\(\)\);/);
  for (const name of ["esptool-js-0.7.0.bundle.js", "esptool-js-0.7.0.LICENSE"]) {
    assert.ok(fs.existsSync(new URL(`../docs/vendor/${name}`, import.meta.url)), name);
  }
  assert.deepEqual(fs.readdirSync(new URL("../docs/vendor/", import.meta.url)).sort(),
    ["esptool-js-0.7.0.LICENSE", "esptool-js-0.7.0.bundle.js", "pako-2.1.0.LICENSE", "pako-2.1.0.zlib.README"],
    "only the pinned runtime and its notices are served");
  const notices = fs.readFileSync(new URL("../docs/THIRD_PARTY.md", import.meta.url), "utf8");
  assert.match(notices, /\[Apache-2\.0\]\(vendor\/esptool-js-0\.7\.0\.LICENSE\)/);
  assert.doesNotMatch(notices, /0\.6\.1/);
});

test("the bootstrap module loads the vendored bundle and stays inert without the installer root", () => {
  assert.equal(bootstrapInstaller({ documentImpl: { getElementById: () => null }, locationImpl: { pathname: "/", search: "" } }), null);
  assert.equal(bootstrapInstaller({ documentImpl: null }), null);
});
