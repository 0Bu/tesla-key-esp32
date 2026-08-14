import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";

import {
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
  splitSerialChunk,
  stripSerialAnsi
} from "../docs/web-installer.mjs";

const manifest = {
  version: "1.4.75",
  builds: [
    { chipFamily: "ESP32", parts: [{ path: "esp32.bin", offset: 0x1000 }] },
    { chipFamily: "ESP32-S3", parts: [{ path: "s3.bin", offset: 0 }] },
    { chipFamily: "ESP32-C3", parts: [{ path: "c3.bin", offset: 0 }] },
    { chipFamily: "ESP32-C6", parts: [{ path: "c6.bin", offset: 0 }] }
  ]
};

test("native Espressif USB ports and UART bridges are described without restricting multi-target builds", () => {
  const cdcInfo = { usbVendorId: 0x303a, usbProductId: 0x1001 };
  const uartInfo = { usbVendorId: 0x1a86, usbProductId: 0x55d3 };

  assert.equal(detectedSerialType(cdcInfo), "cdc");
  assert.equal(detectedSerialType(uartInfo), "uart");
  assert.equal(describeSerialConnection(cdcInfo), "USB Serial/JTAG");
  assert.equal(describeSerialConnection(uartInfo), "USB UART");
  assert.equal(selectManifestBuild(manifest, "ESP32-S3", cdcInfo).parts[0].path, "s3.bin");
  assert.equal(selectManifestBuild(manifest, "ESP32-S3", uartInfo).parts[0].path, "s3.bin");
  assert.equal(selectManifestBuild(manifest, "ESP32-C6", cdcInfo).parts[0].path, "c6.bin");
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

test("firmware parts resolve relative to the manifest and preserve offsets", async () => {
  const seen = [];
  const parts = await fetchFirmwareParts(
    { parts: [{ path: "firmware/app.bin", offset: 0x20000 }] },
    "https://example.test/PR/234/manifest.json",
    async (url, options) => {
      seen.push({ url, options });
      return {
        ok: true,
        status: 200,
        async arrayBuffer() { return Uint8Array.from([1, 2, 3]).buffer; }
      };
    }
  );

  assert.equal(seen[0].url, "https://example.test/PR/234/firmware/app.bin");
  assert.equal(seen[0].options.cache, "no-store");
  assert.equal(parts[0].address, 0x20000);
  assert.deepEqual(Array.from(parts[0].data), [1, 2, 3]);
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

function fakeEsptool(chipFamily = "ESP32-S3") {
  const calls = [];
  class Transport {
    constructor(port) { this.port = port; calls.push("transport"); }
    async disconnect() { calls.push("disconnect"); }
  }
  class Loader {
    constructor(options) {
      this.options = options;
      this.chip = { CHIP_NAME: chipFamily };
      calls.push("loader");
    }
    async main() { calls.push("main"); }
    async flashId() { calls.push("flashId"); }
    async eraseFlash() { calls.push("erase"); }
    async writeFlash(options) {
      calls.push("write");
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
    fetchImpl: async () => ({
      ok: true,
      status: 200,
      async arrayBuffer() { return Uint8Array.from([1, 2, 3, 4]).buffer; }
    }),
    onState(state) { states.push(state); }
  });
  return { fake, states, result };
}

test("keep-configuration mode writes sparse parts without erasing", async () => {
  const { fake, states, result } = await runFlash(false);
  assert.equal(result.chipFamily, "ESP32-S3");
  assert.equal(fake.calls.includes("erase"), false);
  assert.deepEqual(fake.calls.slice(-3), ["write", "reset:soft_reset", "disconnect"]);
  assert.equal(states.at(-1).percentage, 100);
});

test("factory-reset mode erases before writing the same manifest parts", async () => {
  const { fake } = await runFlash(true);
  assert.ok(fake.calls.indexOf("erase") > fake.calls.indexOf("flashId"));
  assert.ok(fake.calls.indexOf("erase") < fake.calls.indexOf("write"));
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

test("published installer keeps Tesla styling and inline Web Serial controls", () => {
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
  assert.match(html, /class="installer-action-row installer-device-actions"[\s\S]*id="install-button"[\s\S]*id="reset-button"[\s\S]*id="disconnect-button"[\s\S]*id="release-serial-port"/);
  assert.match(html, /onBeforeRelease:\s*controller\.disconnectPortForRelease/);
  assert.match(html, /esptool-js@0\.6\.1\/\+esm/);
  assert.match(html, /location\.pathname\.match\(\/\\\/PR\\\/\(\\d\+\)\\\//);
  assert.doesNotMatch(html, /esp-web-install-button/);
  assert.doesNotMatch(html, /#0097e0|#0079bd|#33abe8|#36b8ed|#63bde7/i);
});
