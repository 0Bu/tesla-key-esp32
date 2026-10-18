import assert from "node:assert/strict";
import { createHash, webcrypto } from "node:crypto";
import fs from "node:fs";
import test from "node:test";

import {
  attachWebInstaller,
  bootLogHasVersion,
  combinedProgress,
  describeSerialConnection,
  detectedSerialType,
  fetchFirmwareParts,
  flashDevice,
  installFailureStatus,
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

function fakeEsptool(chipFamily = "ESP32-S3", { chipRevision, info, resetLines = true, writeError } = {}) {
  const calls = [];
  class Transport {
    constructor(port) { this.port = port; calls.push("transport"); }
    async disconnect() { calls.push("disconnect"); }
  }
  class Loader {
    constructor(options) {
      this.options = options;
      this.IS_STUB = false;
      this.chip = { CHIP_NAME: chipFamily };
      if (chipRevision !== undefined) {
        this.chip.getChipRevision = async () => chipRevision;
        this.chip.getChipDescription = async () => `${chipFamily} (revision ${chipRevision})`;
      }
      calls.push("loader");
    }
    // Like esptool-js, main() leaves the flasher stub running.
    async main() { calls.push("main"); this.IS_STUB = true; }
    async flashId() { calls.push("flashId"); }
    async eraseFlash() { calls.push("erase"); }
    async writeFlash(options) {
      calls.push(["write", options.fileArray.map((file) => file.address)]);
      if (writeError) throw writeError;
      options.reportProgress(0, options.fileArray[0].data.length, options.fileArray[0].data.length);
    }
    // Models the vendored esptool-js 0.6.1: with the stub running, a soft reset throws on every
    // chip except the ESP8266.
    async after(mode) {
      calls.push(`reset:${mode}`);
      if (mode === "soft_reset" && this.IS_STUB && this.chip.CHIP_NAME !== "ESP8266") {
        throw new Error("Soft resetting is currently only supported on ESP8266");
      }
    }
  }
  const port = { getInfo() { return info || { usbVendorId: 0x303a, usbProductId: 0x1001 }; } };
  if (resetLines) port.setSignals = async (signals) => { calls.push(["signals", signals]); };
  return { calls, Transport, Loader, port };
}

// The firmware-mode reset: EN pulsed low while IO0 stays high.
const EN_PULSE = [
  ["signals", { dataTerminalReady: false, requestToSend: true }],
  ["signals", { dataTerminalReady: false, requestToSend: false }]
];
const noDelay = async () => {};

test("the installer fake refuses a soft reset once the flasher stub runs", async () => {
  const fake = fakeEsptool();
  const loader = new fake.Loader({});
  await loader.main();
  await assert.rejects(loader.after("soft_reset"), /only supported on ESP8266/);
});

test("device probing validates the manifest, pulses EN back to the app and closes the port", async () => {
  const fake = fakeEsptool();
  const result = await probeDevice({
    port: fake.port,
    manifest,
    TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader,
    delay: noDelay
  });

  assert.equal(result.chipFamily, "ESP32-S3");
  assert.deepEqual(fake.calls, ["transport", "loader", "main", "flashId", ...EN_PULSE, "disconnect"]);
});

test("probe cleanup keeps the serial port owned until a delayed reset settles", { timeout: 5000 }, async () => {
  const fake = fakeEsptool();
  const setSignals = fake.port.setSignals.bind(fake.port);
  let releaseFirstSignal;
  let firstSignal = true;
  fake.port.setSignals = async (signals) => {
    if (firstSignal) {
      firstSignal = false;
      await new Promise((resolve) => { releaseFirstSignal = resolve; });
    }
    await setSignals(signals);
  };
  let finished = false;
  const pending = probeDevice({
    port: fake.port,
    manifest,
    TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader,
    timeoutMs: 1000,
    cleanupTimeoutMs: 50,
    delay: noDelay
  }).then((result) => { finished = true; return result; });

  for (let tries = 0; tries < 100 && !releaseFirstSignal; tries++) {
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  assert.equal(typeof releaseFirstSignal, "function", "probe cleanup started its reset sequence");
  await new Promise((resolve) => setTimeout(resolve, 100));
  assert.equal(finished, false, "probe cleanup still owns the pending reset");
  assert.equal(fake.calls.includes("disconnect"), false, "disconnect cannot overtake setSignals");

  releaseFirstSignal();
  await pending;
  assert.equal(finished, true);
  assert.deepEqual(fake.calls.slice(-3), [...EN_PULSE, "disconnect"]);
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

async function runFlash(eraseFirst, fakeOptions = {}) {
  const fake = fakeEsptool("ESP32-S3", fakeOptions);
  const states = [];
  const logs = [];
  const result = await flashDevice({
    port: fake.port,
    manifest,
    manifestUrl: "https://example.test/manifest.json",
    eraseFirst,
    TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader,
    fetchImpl: async (url) => firmwareResponse(url),
    cryptoImpl: webcrypto,
    onState(state) { states.push(state); },
    onLog(text) { logs.push(text); },
    delay: noDelay
  });
  return { fake, states, logs, result };
}

test("keep-configuration writes ota_0 first, activates it with otadata last and restarts via EN", async () => {
  const { fake, states, result } = await runFlash(false);
  assert.equal(result.chipFamily, "ESP32-S3");
  assert.equal(result.restarted, true);
  assert.equal(fake.calls.includes("erase"), false);
  assert.deepEqual(fake.calls.slice(-5), [
    ["write", [0, 0x8000, 0x20000]],
    ["write", [0xf000]],
    ...EN_PULSE,
    "disconnect"
  ]);
  assert.equal(fake.calls.some((call) => typeof call === "string" && call.startsWith("reset:")), false);
  assert.equal(states.some((item) => item.stage === "activating"), true);
  assert.equal(states.at(-1).stage, "restarting");
  assert.equal(states.at(-1).percentage, 100);
});

test("a completed write stays a successful install when the adapter cannot drive the reset lines", async () => {
  const { fake, logs, result } = await runFlash(false, { resetLines: false });
  assert.equal(result.chipFamily, "ESP32-S3");
  assert.equal(result.restarted, false);
  assert.match(logs.join(""), /Automatic restart unavailable/);
  assert.deepEqual(fake.calls.slice(-3), [["write", [0, 0x8000, 0x20000]], ["write", [0xf000]], "disconnect"]);
});

test("a failed write hands control back to the selected app and closes the port", async () => {
  const fake = fakeEsptool("ESP32-S3", { writeError: new Error("write failed") });
  await assert.rejects(
    flashDevice({
      port: fake.port,
      manifest,
      manifestUrl: "https://example.test/manifest.json",
      eraseFirst: false,
      TransportCtor: fake.Transport,
      ESPLoaderCtor: fake.Loader,
      fetchImpl: async (url) => firmwareResponse(url),
      cryptoImpl: webcrypto,
      delay: noDelay
    }),
    /write failed/
  );
  assert.deepEqual(fake.calls.slice(-4), [["write", [0, 0x8000, 0x20000]], ...EN_PULSE, "disconnect"]);
});

test("after a completed write a failed restart or boot check is reported as installed, not failed", () => {
  const unverified = installFailureStatus(new Error("Timed out waiting for firmware 1.2.3 to boot."), {
    written: true,
    version: "1.2.3"
  });
  assert.equal(unverified.kind, "warning");
  assert.equal(unverified.step, 4);
  assert.match(unverified.message, /^Firmware 1\.2\.3 installed, but its start was not verified: Timed out/);
  assert.match(unverified.message, /RESET button or by reconnecting/);
  assert.doesNotMatch(`${unverified.message} ${unverified.statusLine}`, /Installation failed/);

  const failed = installFailureStatus(new Error("write failed"), { written: false, version: "1.2.3" });
  assert.deepEqual(failed, {
    kind: "error",
    step: 2,
    message: "Installation failed: write failed",
    statusLine: "Installation failed: write failed"
  });
});

test("the install handler marks firmware written before waiting for reset and verifying boot", () => {
  // The page handler is DOM-bound; pin its wiring to the tested status decision.
  const source = fs.readFileSync(new URL("../docs/web-installer.mjs", import.meta.url), "utf8");
  const handler = source.match(/const install = async \(\) => \{([\s\S]*?)\n  \};/)?.[1];
  assert.ok(handler, "install handler found");
  assert.match(handler, /let written = false;[\s\S]*await flashDevice\([\s\S]*onState\(state\) \{[\s\S]*state\.stage === "restarting"[\s\S]*written = true;[\s\S]*await verifyBootImpl\(/);
  assert.match(handler, /catch \(error\) \{[\s\S]*installFailureStatus\(error, \{ written, version: manifest\.version \}\)/);
  const html = fs.readFileSync(new URL("../docs/index.html", import.meta.url), "utf8");
  assert.match(html, /\.installer-page-status\[data-kind="warning"\]\{color:var\(--warn-text\)\}/);
});

test("a delayed restart keeps the serial port owned until reset and cleanup settle", { timeout: 5000 }, async () => {
  const fake = fakeEsptool();
  const setSignals = fake.port.setSignals.bind(fake.port);
  let releaseFirstSignal;
  let firstSignal = true;
  fake.port.setSignals = async (signals) => {
    if (firstSignal) {
      firstSignal = false;
      await new Promise((resolve) => { releaseFirstSignal = resolve; });
    }
    await setSignals(signals);
  };
  const logs = [];
  let written = false;
  let finished = false;
  const pending = flashDevice({
    port: fake.port,
    manifest,
    manifestUrl: "https://example.test/manifest.json",
    eraseFirst: false,
    TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader,
    fetchImpl: async (url) => firmwareResponse(url),
    cryptoImpl: webcrypto,
    onLog(text) { logs.push(text); },
    onState(state) { if (state.stage === "restarting") written = true; },
    timeoutMs: 200,
    cleanupTimeoutMs: 50,
    delay: noDelay
  }).then((result) => { finished = true; return result; });

  for (let tries = 0; tries < 100 && !logs.join("").includes("Device reset timed out."); tries++) {
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  assert.match(logs.join(""), /Automatic restart unavailable: Device reset timed out\./);
  assert.equal(written, true, "all flash writes completed before reset began");
  assert.equal(finished, false, "the pending signal still owns the serial operation");
  assert.equal(fake.calls.includes("disconnect"), false, "disconnect waits for the reset sequence");

  releaseFirstSignal();
  const result = await pending;
  assert.equal(result.restarted, false);
  assert.equal(finished, true);
  assert.deepEqual(fake.calls.slice(-3), [...EN_PULSE, "disconnect"]);
});

// Minimal stand-in for the installer page: elements by id, the four steps and the mode radios.
function fakeInstallerPage() {
  const elements = new Map();
  const element = (id) => {
    if (!elements.has(id)) {
      elements.set(id, {
        id,
        textContent: "",
        hidden: false,
        disabled: false,
        dataset: {},
        style: {},
        attributes: {},
        scrollTop: 0,
        scrollHeight: 0,
        addEventListener() {},
        setAttribute(name, value) { this.attributes[name] = value; },
        append(child) { this.textContent += child.textContent; },
        replaceChildren() { this.textContent = ""; },
        ownerDocument: { createElement: () => ({ className: "", textContent: "" }) }
      });
    }
    return elements.get(id);
  };
  const steps = [1, 2, 3, 4].map(() => ({ dataset: {} }));
  const modes = [{ value: "keep", checked: true }, { value: "erase", checked: false }];
  const root = {
    dataset: {},
    querySelector(selector) {
      if (selector.startsWith("#")) return element(selector.slice(1));
      if (selector === 'input[name="install-mode"]:checked') return modes.find((mode) => mode.checked) || null;
      if (selector === 'input[name="install-mode"][value="erase"]') return modes[1];
      return null;
    },
    querySelectorAll(selector) { return selector === ".installer-step" ? steps : []; }
  };
  return { root, element, steps };
}

async function runPageInstall({ fakeOptions = {}, verifyBootImpl }) {
  const saved = { location: globalThis.location, isSecureContext: globalThis.isSecureContext };
  globalThis.location = { href: "https://example.test/", pathname: "/" };
  globalThis.isSecureContext = true;
  try {
    const fake = fakeEsptool("ESP32-S3", fakeOptions);
    const page = fakeInstallerPage();
    const installer = attachWebInstaller({
      root: page.root,
      serial: { requestPort: async () => fake.port, addEventListener() {} },
      TransportCtor: fake.Transport,
      ESPLoaderCtor: fake.Loader,
      fetchImpl: async (url) => url.endsWith("manifest.json")
        ? { ok: true, status: 200, json: async () => manifest }
        : firmwareResponse(url),
      verifyBootImpl: async (options) => {
        fake.calls.push("verify");
        return verifyBootImpl(options);
      }
    });
    for (let tries = 0; page.element("firmware-version-value").textContent === "" && tries < 50; tries++) {
      await new Promise((resolve) => setTimeout(resolve, 0));
    }
    await installer.connect();
    await installer.install();
    const status = page.element("page-status");
    return {
      fake,
      kind: status.dataset.kind,
      message: status.textContent,
      finished: page.root.dataset.finished,
      activeStep: page.steps.findIndex((step) => step.dataset.state === "active") + 1,
      monitor: page.element("serial-monitor-output").textContent
    };
  } finally {
    globalThis.location = saved.location;
    globalThis.isSecureContext = saved.isSecureContext;
  }
}

test("the page reports a verified install after the write and the boot check", async () => {
  const page = await runPageInstall({ verifyBootImpl: async () => ({ version: manifest.version }) });
  assert.equal(page.kind, "success");
  assert.equal(page.message, `Firmware ${manifest.version} booted and was verified successfully.`);
  assert.equal(page.finished, "true");
  assert.equal(page.activeStep, 4);
  const lastWrite = page.fake.calls.findLastIndex((call) => Array.isArray(call) && call[0] === "write");
  assert.ok(lastWrite >= 0 && page.fake.calls.indexOf("verify") > lastWrite, "boot check runs after the write");
});

test("the page reports an installed but unverified firmware when the restart and boot check fail", async () => {
  const page = await runPageInstall({
    fakeOptions: { resetLines: false },
    verifyBootImpl: async () => { throw new Error("Timed out waiting for firmware 1.4.75 to boot."); }
  });
  assert.equal(page.kind, "warning");
  assert.match(page.message, /^Firmware 1\.4\.75 installed, but its start was not verified: Timed out/);
  assert.equal(page.finished, "false");
  assert.equal(page.activeStep, 4);
  assert.match(page.monitor, /Automatic restart unavailable/);
  assert.doesNotMatch(`${page.message}\n${page.monitor}`, /Installation failed/);
});

test("the page reports a failed install when the write fails, without a boot check", async () => {
  const page = await runPageInstall({
    fakeOptions: { writeError: new Error("write failed") },
    verifyBootImpl: async () => ({ version: manifest.version })
  });
  assert.equal(page.kind, "error");
  assert.equal(page.message, "Installation failed: write failed");
  assert.equal(page.activeStep, 2);
  assert.equal(page.fake.calls.includes("verify"), false);
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
  const fake = fakeEsptool("ESP32", { chipRevision: 1, info: { usbVendorId: 0x10c4, usbProductId: 0xea60 } });
  await assert.rejects(
    probeDevice({
      port: fake.port,
      manifest,
      TransportCtor: fake.Transport,
      ESPLoaderCtor: fake.Loader,
      delay: noDelay
    }),
    (error) => error.name === "UnsupportedChipRevisionError" &&
      /ECO3/.test(error.message) && /v3\.0/.test(error.message)
  );
  // The rejected probe still hands control back to the app and closes the port.
  assert.deepEqual(fake.calls, ["transport", "loader", "main", "flashId", ...EN_PULSE, "disconnect"]);
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
