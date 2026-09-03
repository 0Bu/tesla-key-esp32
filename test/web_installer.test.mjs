import assert from "node:assert/strict";
import { createHash, webcrypto } from "node:crypto";
import fs from "node:fs";
import test from "node:test";

import {
  bootLogHasVersion,
  combinedProgress,
  describeSerialConnection,
  detectedSerialType,
  fetchFirmwareParts,
  flashDevice,
  probeDevice,
  resetConnectedDevice,
  resetToUserFirmware,
  serialLogLevel,
  selectManifestBuild,
  stopSerialReader,
  splitSerialChunk,
  stripSerialAnsi,
  validateManifest,
  verifyBootVersion
} from "../docs/web-installer.mjs";

const bytesByRole = {
  bootloader: Uint8Array.from([1, 2, 3, 4]),
  partitions: Uint8Array.from([5, 6, 7, 8]),
  app: Uint8Array.from([9, 10, 11, 12]),
  otadata: new Uint8Array(0x2000).fill(0xff)
};
const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");
const suffixFor = (chipFamily) => ({
  ESP32: "esp32",
  "ESP32-S3": "esp32s3",
  "ESP32-C3": "esp32c3",
  "ESP32-C6": "esp32c6"
})[chipFamily];
const buildFor = (chipFamily) => {
  const suffix = suffixFor(chipFamily);
  return {
    chipFamily,
    parts: [
      { path: `bootloader-${suffix}.bin`, offset: chipFamily === "ESP32" ? 0x1000 : 0, size: bytesByRole.bootloader.length, sha256: sha256(bytesByRole.bootloader) },
      { path: `partition-table-${suffix}.bin`, offset: 0x8000, size: bytesByRole.partitions.length, sha256: sha256(bytesByRole.partitions) },
      { path: `tesla-key-${suffix}.bin`, offset: 0x20000, size: bytesByRole.app.length, sha256: sha256(bytesByRole.app) },
      { path: `ota-data-${suffix}.bin`, offset: 0xf000, size: bytesByRole.otadata.length, sha256: sha256(bytesByRole.otadata) }
    ]
  };
};

const manifest = {
  layoutVersion: 2,
  version: "1.4.75",
  sourceSha: "a".repeat(40),
  new_install_prompt_erase: true,
  builds: ["ESP32", "ESP32-S3", "ESP32-C3", "ESP32-C6"].map(buildFor)
};

function bytesForPath(path) {
  if (path.startsWith("bootloader-")) return bytesByRole.bootloader;
  if (path.startsWith("partition-table-")) return bytesByRole.partitions;
  if (path.startsWith("tesla-key-")) return bytesByRole.app;
  if (path.startsWith("ota-data-")) return bytesByRole.otadata;
  throw new Error(`unexpected firmware path ${path}`);
}

function firmwareResponse(url, override = null) {
  const path = new URL(url).pathname.split("/").pop();
  const bytes = override || bytesForPath(path);
  return {
    ok: true,
    status: 200,
    async arrayBuffer() { return bytes.slice().buffer; }
  };
}

test("native Espressif USB ports and UART bridges are described without restricting multi-target builds", () => {
  const cdcInfo = { usbVendorId: 0x303a, usbProductId: 0x1001 };
  const uartInfo = { usbVendorId: 0x1a86, usbProductId: 0x55d3 };

  assert.equal(detectedSerialType(cdcInfo), "cdc");
  assert.equal(detectedSerialType(uartInfo), "uart");
  assert.equal(describeSerialConnection(cdcInfo), "USB Serial/JTAG");
  assert.equal(describeSerialConnection(uartInfo), "USB UART");
  assert.equal(selectManifestBuild(manifest, "ESP32-S3", cdcInfo).parts[0].path, "bootloader-esp32s3.bin");
  assert.equal(selectManifestBuild(manifest, "ESP32-S3", uartInfo).parts[0].path, "bootloader-esp32s3.bin");
  assert.equal(selectManifestBuild(manifest, "ESP32-C6", cdcInfo).parts[0].path, "bootloader-esp32c6.bin");
  assert.equal(selectManifestBuild(manifest, "ESP32-C5", cdcInfo), undefined);
});

test("a serial-type-specific build wins over the generic fallback", () => {
  const builds = {
    builds: [
      { chipFamily: "ESP32-S3", parts: [{ path: "generic.bin", offset: 0 }] },
      { chipFamily: "ESP32-S3", serialType: "cdc", parts: [{ path: "native.bin", offset: 0 }] }
    ]
  };
  assert.equal(selectManifestBuild(
    builds,
    "ESP32-S3",
    { usbVendorId: 0x303a, usbProductId: 0x1001 }
  ).parts[0].path, "native.bin");
});

test("multi-part progress is weighted by each uncompressed image size", () => {
  const files = [
    { data: new Uint8Array(4), address: 0 },
    { data: new Uint8Array(6), address: 4 }
  ];
  assert.equal(combinedProgress(files, 0, 2, 4), 20);
  assert.equal(combinedProgress(files, 1, 0, 6), 40);
  assert.equal(combinedProgress(files, 1, 3, 6), 70);
  assert.equal(combinedProgress(files, 1, 6, 6), 100);
});

test("firmware parts are same-directory, size checked and SHA-256 verified", async () => {
  const seen = [];
  const build = manifest.builds.find((item) => item.chipFamily === "ESP32-S3");
  const parts = await fetchFirmwareParts(
    build,
    "https://example.test/PR/234/manifest.json",
    async (url, options) => {
      seen.push({ url, options });
      return firmwareResponse(url);
    },
    webcrypto
  );

  assert.equal(seen[0].url, "https://example.test/PR/234/bootloader-esp32s3.bin");
  assert.equal(seen[0].options.cache, "no-store");
  assert.deepEqual(parts.map((part) => part.address), [0, 0x8000, 0x20000, 0xf000]);
  assert.deepEqual(Array.from(parts[0].data), Array.from(bytesByRole.bootloader));
});

test("manifest v2 rejects malicious offsets, paths and incomplete provenance", () => {
  const badOffset = structuredClone(manifest);
  badOffset.builds[1].parts[2].offset = 0x9000;
  assert.throws(
    () => validateManifest(badOffset, "https://example.test/manifest.json"),
    (error) => error.name === "InvalidManifestError" && /application offset/.test(error.message)
  );

  const badPath = structuredClone(manifest);
  badPath.builds[1].parts[2].path = "https://evil.example/app.bin";
  assert.throws(
    () => validateManifest(badPath, "https://example.test/manifest.json"),
    (error) => error.name === "InvalidManifestError" && /unsafe application path/.test(error.message)
  );

  const badSha = structuredClone(manifest);
  badSha.sourceSha = "main";
  assert.throws(
    () => validateManifest(badSha, "https://example.test/manifest.json"),
    (error) => error.name === "InvalidManifestError" && /source commit/.test(error.message)
  );

  for (const version of ["01.4.75", `1.4.75-${"x".repeat(25)}`]) {
    const badVersion = structuredClone(manifest);
    badVersion.version = version;
    assert.throws(
      () => validateManifest(badVersion, "https://example.test/manifest.json"),
      (error) => error.name === "InvalidManifestError" && /version/.test(error.message)
    );
  }
});

test("firmware integrity failure is detected before flashing", async () => {
  const build = manifest.builds.find((item) => item.chipFamily === "ESP32-S3");
  await assert.rejects(
    fetchFirmwareParts(
      build,
      "https://example.test/manifest.json",
      async (url) => {
        const path = new URL(url).pathname.split("/").pop();
        const expected = bytesForPath(path);
        const corrupt = expected.slice();
        corrupt[0] ^= 0xff;
        return firmwareResponse(url, corrupt);
      },
      webcrypto
    ),
    (error) => error.name === "FirmwareIntegrityError" && /SHA-256/.test(error.message)
  );
});

test("serial log parsing preserves split line endings and classifies IDF levels", () => {
  const first = splitSerialChunk("", "\x1b[0;33mW (7700) uart: pin busy\r");
  assert.deepEqual(first.lines, []);
  assert.equal(first.pending, "\x1b[0;33mW (7700) uart: pin busy\r");

  const second = splitSerialChunk(
    first.pending,
    "\nE (8340) uart: failed\nI (9000) diag: continuing"
  );
  assert.deepEqual(second.lines, [
    { text: "\x1b[0;33mW (7700) uart: pin busy", terminated: true },
    { text: "E (8340) uart: failed", terminated: true }
  ]);
  assert.equal(second.pending, "I (9000) diag: continuing");
  assert.equal(serialLogLevel(second.lines[0].text), "warning");
  assert.equal(serialLogLevel(second.lines[1].text), "error");
  assert.equal(serialLogLevel(second.pending), "info");
  assert.equal(stripSerialAnsi(second.lines[0].text), "W (7700) uart: pin busy");
});

function fakeEsptool(chipFamily = "ESP32-S3", { chipRevision } = {}) {
  const calls = [];
  class Transport {
    constructor(port) { this.port = port; calls.push("transport"); }
    async disconnect() { calls.push("disconnect"); }
  }
  class Loader {
    constructor(options) {
      this.options = options;
      this.chip = { CHIP_NAME: chipFamily };
      if (chipRevision !== undefined) {
        this.chip.getChipRevision = async () => chipRevision;
        this.chip.getChipDescription = async () => `${chipFamily} (revision ${chipRevision})`;
      }
      calls.push("loader");
    }
    async main() { calls.push("main"); }
    async flashId() { calls.push("flashId"); }
    async eraseFlash() { calls.push("erase"); }
    async writeFlash(options) {
      calls.push(["write", options.fileArray.map((file) => file.address)]);
      options.reportProgress(0, options.fileArray[0].data.length, options.fileArray[0].data.length);
    }
    async after(mode) { calls.push(`reset:${mode}`); }
  }
  return { calls, Transport, Loader };
}

test("device probing validates the manifest and always resets and closes the port", async () => {
  const fake = fakeEsptool();
  const port = { getInfo() { return { usbVendorId: 0x303a, usbProductId: 0x1001 }; } };
  const result = await probeDevice({
    port,
    manifest,
    TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader
  });

  assert.equal(result.chipFamily, "ESP32-S3");
  assert.deepEqual(fake.calls, ["transport", "loader", "main", "flashId", "reset:soft_reset", "disconnect"]);
});

test("device probing times out and releases an unresponsive serial device", async () => {
  const calls = [];
  class Transport {
    constructor() { calls.push("transport"); }
    async disconnect() { calls.push("disconnect"); }
  }
  class Loader {
    constructor() { calls.push("loader"); }
    async main() {
      calls.push("main");
      return new Promise(() => {});
    }
  }

  await assert.rejects(
    probeDevice({
      port: { getInfo() { return { usbVendorId: 0x303a, usbProductId: 0x1001 }; } },
      manifest,
      TransportCtor: Transport,
      ESPLoaderCtor: Loader,
      timeoutMs: 10,
      cleanupTimeoutMs: 10
    }),
    (error) => error.name === "DeviceProbeTimeoutError" && /native USB port/.test(error.message)
  );

  assert.deepEqual(calls, ["transport", "loader", "main", "disconnect"]);
});

async function runFlash(eraseFirst) {
  const fake = fakeEsptool();
  const states = [];
  const result = await flashDevice({
    port: { getInfo() { return { usbVendorId: 0x303a, usbProductId: 0x1001 }; } },
    manifest,
    manifestUrl: "https://example.test/manifest.json",
    eraseFirst,
    TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader,
    fetchImpl: async (url) => firmwareResponse(url),
    cryptoImpl: webcrypto,
    onState(state) { states.push(state); }
  });
  return { fake, states, result };
}

test("keep-configuration writes ota_0 first and activates it with otadata last", async () => {
  const { fake, states, result } = await runFlash(false);
  assert.equal(result.chipFamily, "ESP32-S3");
  assert.equal(fake.calls.includes("erase"), false);
  assert.deepEqual(fake.calls.slice(-4), [
    ["write", [0, 0x8000, 0x20000]],
    ["write", [0xf000]],
    "reset:soft_reset",
    "disconnect"
  ]);
  assert.equal(states.some((item) => item.stage === "activating"), true);
  assert.equal(states.at(-1).percentage, 100);
});

test("factory-reset mode erases before writing the same manifest parts", async () => {
  const { fake } = await runFlash(true);
  assert.ok(fake.calls.indexOf("erase") > fake.calls.indexOf("flashId"));
  assert.ok(fake.calls.indexOf("erase") < fake.calls.findIndex((call) => Array.isArray(call) && call[0] === "write"));
});

test("an integrity error occurs before erase or either write phase", async () => {
  const fake = fakeEsptool();
  await assert.rejects(
    flashDevice({
      port: { getInfo() { return { usbVendorId: 0x303a, usbProductId: 0x1001 }; } },
      manifest,
      manifestUrl: "https://example.test/manifest.json",
      eraseFirst: true,
      TransportCtor: fake.Transport,
      ESPLoaderCtor: fake.Loader,
      fetchImpl: async (url) => {
        const path = new URL(url).pathname.split("/").pop();
        const expected = bytesForPath(path);
        const corrupt = expected.slice();
        corrupt[0] ^= 1;
        return firmwareResponse(url, corrupt);
      },
      cryptoImpl: webcrypto
    }),
    { name: "FirmwareIntegrityError" }
  );
  assert.equal(fake.calls.includes("erase"), false);
  assert.equal(fake.calls.some((call) => Array.isArray(call) && call[0] === "write"), false);
});

test("probing refuses a pre-ECO3 classic ESP32 and still releases the port", async () => {
  const fake = fakeEsptool("ESP32", { chipRevision: 1 });
  await assert.rejects(
    probeDevice({
      port: { getInfo() { return { usbVendorId: 0x10c4, usbProductId: 0xea60 }; } },
      manifest,
      TransportCtor: fake.Transport,
      ESPLoaderCtor: fake.Loader
    }),
    (error) => error.name === "UnsupportedChipRevisionError" &&
      /ECO3/.test(error.message) && /v3\.0/.test(error.message)
  );
  // The rejected probe still hands control back to the app and closes the port.
  assert.deepEqual(fake.calls, ["transport", "loader", "main", "flashId", "reset:soft_reset", "disconnect"]);
});

test("probing accepts an ECO3 (revision v3) classic ESP32", async () => {
  const fake = fakeEsptool("ESP32", { chipRevision: 3 });
  const result = await probeDevice({
    port: { getInfo() { return { usbVendorId: 0x10c4, usbProductId: 0xea60 }; } },
    manifest,
    TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader
  });
  assert.equal(result.chipFamily, "ESP32");
  assert.equal(result.build.parts[0].path, "bootloader-esp32.bin");
});

test("installing refuses a pre-ECO3 classic ESP32 before erasing or writing flash", async () => {
  const fake = fakeEsptool("ESP32", { chipRevision: 1 });
  await assert.rejects(
    flashDevice({
      port: { getInfo() { return { usbVendorId: 0x10c4, usbProductId: 0xea60 }; } },
      manifest,
      manifestUrl: "https://example.test/manifest.json",
      eraseFirst: true,
      TransportCtor: fake.Transport,
      ESPLoaderCtor: fake.Loader,
      fetchImpl: async (url) => firmwareResponse(url),
      cryptoImpl: webcrypto,
      onState() {}
    }),
    { name: "UnsupportedChipRevisionError" }
  );
  assert.equal(fake.calls.includes("erase"), false);
  assert.equal(fake.calls.some((call) => Array.isArray(call) && call[0] === "write"), false);
});

test("serial monitor reset keeps IO0 high while EN is pulsed", async () => {
  const calls = [];
  const port = {
    async setSignals(signals) { calls.push(signals); }
  };

  await resetToUserFirmware(port, async (milliseconds) => {
    calls.push(`wait:${milliseconds}`);
  });

  assert.deepEqual(calls, [
    { dataTerminalReady: false, requestToSend: true },
    "wait:100",
    { dataTerminalReady: false, requestToSend: false },
    "wait:250"
  ]);
});

test("standalone reset opens a closed port and releases it afterwards", async () => {
  const calls = [];
  const port = {
    readable: null,
    writable: null,
    async open(options) {
      calls.push(["open", options]);
      this.readable = {};
      this.writable = {};
    },
    async setSignals(signals) { calls.push(["signals", signals]); },
    async close() {
      calls.push(["close"]);
      this.readable = null;
      this.writable = null;
    }
  };

  await resetConnectedDevice(port, {
    delay: async (milliseconds) => calls.push(["wait", milliseconds])
  });

  assert.deepEqual(calls, [
    ["open", { baudRate: 115200, bufferSize: 8192 }],
    ["signals", { dataTerminalReady: false, requestToSend: true }],
    ["wait", 100],
    ["signals", { dataTerminalReady: false, requestToSend: false }],
    ["wait", 250],
    ["close"]
  ]);
});

test("boot verification accepts only the expected application version and closes the port", async () => {
  const calls = [];
  const chunks = [
    new TextEncoder().encode("I (101) cpu_start: Project name: tesla-key-esp32\n"),
    new TextEncoder().encode("I (105) cpu_start: App version: 1.4.75\n")
  ];
  const reader = {
    async read() { return chunks.length ? { value: chunks.shift(), done: false } : { done: true }; },
    async cancel() { calls.push("cancel"); },
    releaseLock() { calls.push("release"); }
  };
  const port = {
    readable: { getReader() { return reader; } },
    writable: {},
    async setSignals(signals) { calls.push(["signals", signals]); },
    async close() { calls.push("close"); this.readable = null; this.writable = null; }
  };

  const result = await verifyBootVersion({
    port,
    expectedVersion: "1.4.75",
    delay: async () => {},
    timeoutMs: 100,
    cleanupTimeoutMs: 20
  });

  assert.equal(result.version, "1.4.75");
  assert.equal(bootLogHasVersion(result.log, "1.4.75"), true);
  assert.equal(bootLogHasVersion(result.log, "1.4.74"), false);
  assert.deepEqual(calls.slice(-3), ["cancel", "release", "close"]);
});

test("serial reader cleanup is bounded even when cancel and read never settle", async () => {
  const started = Date.now();
  await stopSerialReader(
    { cancel() { return new Promise(() => {}); } },
    new Promise(() => {}),
    10
  );
  assert.ok(Date.now() - started < 200, "reader cleanup must not hang the installer");
});

test("published installer keeps Tesla styling and local Web Serial controls", () => {
  const html = fs.readFileSync(new URL("../docs/index.html", import.meta.url), "utf8");
  assert.equal((html.match(/id="serial-monitor-button"/g) || []).length, 1);
  assert.equal((html.match(/id="reset-button"/g) || []).length, 1);
  assert.match(html, /--brand:#e82127/);
  assert.match(html, /--installer-rail-width:214px/);
  assert.match(html, /\.installer-layout\{[^}]*grid-template-columns:var\(--installer-rail-width\)/);
  assert.match(html, /\.installer-footer\{[\s\S]*background:linear-gradient\(to right,[\s\S]*var\(--brand-tint\)[\s\S]*var\(--installer-rail-width\)/);
  assert.match(html, /@media \(max-width:780px\)[\s\S]*\.installer-footer\{background:transparent\}/);
  assert.match(html, /<span class="installer-logo"[^>]*><svg[\s\S]*M13 2 4 14h6/);
  assert.match(html, /Keep configuration[\s\S]*WiFi, VIN, MQTT, pairing key and BLE sessions stay intact/);
  assert.match(html, /id="serial-release"[\s\S]*id="release-serial-port"[\s\S]*class="installer-device"/);
  assert.doesNotMatch(html, /class="installer-card installer-options"[\s\S]*id="release-serial-port"/);
  assert.match(html, /id="firmware-source-link"[\s\S]*id="firmware-source-value"/);
  assert.match(html, /<script type="module" src="\.\/installer-bootstrap\.mjs"><\/script>/);
  assert.match(html, /Content-Security-Policy[^>]*script-src 'self'/);
  const style = html.match(/<style>([\s\S]*?)<\/style>/)?.[1];
  assert.ok(style, "installer stylesheet is embedded and covered by its CSP hash");
  const styleHash = createHash("sha256").update(style).digest("base64");
  assert.match(html, new RegExp(`style-src 'sha256-${styleHash.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}'`));
  assert.doesNotMatch(html, /unsafe-inline|unsafe-eval/);
  assert.doesNotMatch(html, /<script(?:\s|>)(?![^>]*\bsrc=)/);
  assert.doesNotMatch(html, /cdn\.jsdelivr\.net|unpkg\.com/);

  const bootstrap = fs.readFileSync(new URL("../docs/installer-bootstrap.mjs", import.meta.url), "utf8");
  assert.match(bootstrap, /from "\.\/vendor\/esptool-js-0\.6\.1\.bundle\.js"/);
  assert.match(bootstrap, /link\.id\s*=\s*"preview-source-link"/);
  assert.match(bootstrap, /onBeforeRelease:\s*controller\.disconnectPortForRelease/);
  assert.match(bootstrap, /locationImpl\.pathname\.match\(\/\\\/PR\\\/\(\\d\+\)\\\//);
  assert.doesNotMatch(html, /esp-web-install-button/);
  assert.doesNotMatch(html, /#0097e0|#0079bd|#33abe8|#36b8ed|#63bde7/i);
});
