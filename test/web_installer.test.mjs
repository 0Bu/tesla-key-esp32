import assert from "node:assert/strict";
import { createHash, webcrypto } from "node:crypto";
import fs from "node:fs";
import test from "node:test";
import { bootstrapInstaller } from "../docs/installer-bootstrap.mjs";

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

test("published builds have no serial-type-specific branch", () => {
  const invalid = structuredClone(manifest);
  invalid.builds[1].serialType = "cdc";
  assert.throws(() => validateManifest(invalid, "https://example.test/manifest.json"), /build fields must be exactly/);
});

test("installer labels root, development and PR channels before device setup", () => {
  for (const [pathname, expected] of [["/", "Release"], ["/project/", "Release"],
                                    ["/project/dev/", "Development"], ["/project/PR/404/", "Preview build · PR #404"]]) {
    const elements = new Map();
    for (const id of ["build-channel", "preview-note", "preview-note-text"]) {
      elements.set(id, { textContent: "", dataset: {}, hidden: true, append() {} });
    }
    const documentImpl = { getElementById(id) { return elements.get(id) || null; }, createElement() { return {}; } };
    assert.equal(bootstrapInstaller({ documentImpl, locationImpl: { pathname }, navigatorImpl: {} }), null);
    assert.equal(elements.get("build-channel").textContent, expected);
    assert.equal(elements.get("preview-note").hidden, !pathname.includes("/PR/"));
  }
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
  const resetSignals = [];
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
    async main() { calls.push("main"); this.IS_STUB = true; }
    async flashId() { calls.push("flashId"); }
    async eraseFlash() { calls.push("erase"); }
    async writeFlash(options) {
      calls.push(["write", options.fileArray.map((file) => file.address)]);
      if (writeError) throw writeError;
      options.reportProgress(0, options.fileArray[0].data.length, options.fileArray[0].data.length);
    }
    async after(mode) {
      calls.push(`reset:${mode}`);
      if (mode === "soft_reset" && this.IS_STUB && this.chip.CHIP_NAME !== "ESP8266") {
        throw new Error("Soft resetting is currently only supported on ESP8266");
      }
    }
  }
  const port = { getInfo() { return info || { usbVendorId: 0x303a, usbProductId: 0x1001 }; } };
  if (resetLines) port.setSignals = async (signals) => { resetSignals.push(signals); };
  return { calls, resetSignals, port, Transport, Loader };
}

test("the installer fake refuses a soft reset once the flasher stub runs", async () => {
  const fake = fakeEsptool();
  const loader = new fake.Loader({});
  await loader.main();
  await assert.rejects(loader.after("soft_reset"), /only supported on ESP8266/);
});

test("probe retains serial ownership until a delayed EN reset settles", { timeout: 5000 }, async () => {
  const fake = fakeEsptool();
  const signal = deferred();
  const signalStarted = deferred();
  const setSignals = fake.port.setSignals;
  let first = true;
  fake.port.setSignals = async (value) => {
    if (first) {
      first = false;
      signalStarted.resolve();
      await signal.promise;
    }
    await setSignals(value);
  };

  let finished = false;
  const pending = probeDevice({
    port: fake.port,
    manifest,
    TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader,
    cleanupTimeoutMs: 10,
    delay: async () => {}
  }).then((result) => { finished = true; return result; });
  await signalStarted.promise;
  await new Promise((resolve) => setTimeout(resolve, 25));
  assert.equal(finished, false);
  assert.equal(fake.calls.includes("disconnect"), false, "disconnect must not overtake EN assertion");

  signal.resolve();
  const result = await pending;
  assert.equal(result.chipFamily, "ESP32-S3");
  assert.equal(finished, true);
  assert.equal(fake.resetSignals.length, 2);
  assert.equal(fake.calls.at(-1), "disconnect");
});

test("completed writes remain successful while a delayed EN restart keeps the port reserved", { timeout: 5000 }, async () => {
  const fake = fakeEsptool();
  const signal = deferred();
  const signalStarted = deferred();
  const setSignals = fake.port.setSignals;
  let first = true;
  fake.port.setSignals = async (value) => {
    if (first) {
      first = false;
      signalStarted.resolve();
      await signal.promise;
    }
    await setSignals(value);
  };

  const logs = [];
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
    timeoutMs: 1000,
    cleanupTimeoutMs: 10,
    delay: async () => {},
    onLog(text) { logs.push(text); }
  }).then((result) => { finished = true; return result; });

  await signalStarted.promise;
  await new Promise((resolve) => setTimeout(resolve, 25));
  assert.equal(finished, false);
  assert.match(logs.join(""), /Automatic restart unavailable: Device reset timed out\./);
  assert.equal(fake.calls.includes("disconnect"), false, "disconnect cannot overtake the pending reset");
  assert.equal(fake.calls.filter((call) => Array.isArray(call) && call[0] === "write").length, 4);

  signal.resolve();
  const result = await pending;
  assert.equal(result.restarted, false);
  assert.equal(finished, true);
  assert.equal(fake.resetSignals.length, 2);
  assert.equal(fake.calls.at(-1), "disconnect");
});

test("cancellation racing the completed otadata write reports the installed image", async () => {
  const fake = fakeEsptool();
  const controller = new AbortController();
  const finalWriteStarted = deferred();
  const finishFinalWrite = deferred();
  class FinalWriteLoader extends fake.Loader {
    writeFlash(options) {
      if (options.fileArray[0].address === 0xf000) {
        fake.calls.push(["write", options.fileArray.map((file) => file.address)]);
        options.reportProgress(0, 0, options.fileArray[0].data.length);
        finalWriteStarted.resolve();
        return finishFinalWrite.promise;
      }
      return super.writeFlash(options);
    }
  }

  const pending = flashDevice({
    port: fake.port,
    manifest,
    manifestUrl: "https://example.test/manifest.json",
    eraseFirst: false,
    TransportCtor: fake.Transport,
    ESPLoaderCtor: FinalWriteLoader,
    fetchImpl: async (url) => firmwareResponse(url),
    cryptoImpl: webcrypto,
    signal: controller.signal,
    timeoutMs: 1000,
    delay: async () => {}
  });

  await finalWriteStarted.promise;
  // Resolve the physical write and abort in the same task. The abort races the loader promise's
  // continuation, exactly where ota_data_initial may already have selected the new app.
  finishFinalWrite.resolve();
  controller.abort(Object.assign(new Error("USB disconnect raced the final write"), { name: "AbortError" }));

  const result = await pending;
  assert.equal(result.chipFamily, "ESP32-S3");
  assert.equal(result.restarted, true);
  assert.equal(fake.calls.filter((call) => Array.isArray(call) && call[0] === "write").length, 4);
  assert.deepEqual(fake.calls.slice(-2), [["write", [0xf000]], "disconnect"]);
  assert.equal(fake.resetSignals.length, 2);
});

test("a failed write pulses EN before releasing the flasher transport", async () => {
  const fake = fakeEsptool("ESP32-S3", { writeError: new Error("write failed") });
  await assert.rejects(flashDevice({
    port: fake.port,
    manifest,
    manifestUrl: "https://example.test/manifest.json",
    eraseFirst: false,
    TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader,
    fetchImpl: async (url) => firmwareResponse(url),
    cryptoImpl: webcrypto,
    delay: async () => {}
  }), /write failed/);
  assert.equal(fake.resetSignals.length, 2);
  assert.equal(fake.calls.at(-1), "disconnect");
});

test("boot failure after both write phases is reported as installed but unverified", () => {
  const status = installFailureStatus(new Error("boot log ended before the expected version"), {
    written: true,
    version: "1.2.3"
  });
  assert.equal(status.kind, "warning");
  assert.equal(status.step, 4);
  assert.match(status.message, /^Firmware 1\.2\.3 installed, but its start was not verified/);
  assert.match(status.statusLine, /boot verification failed/);
});

test("device probing validates the manifest and always resets and closes the port", async () => {
  const fake = fakeEsptool();
  const port = {
    getInfo() { return { usbVendorId: 0x303a, usbProductId: 0x1001 }; },
    setSignals: fake.port.setSignals
  };
  const result = await probeDevice({
    port,
    manifest,
    TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader
  });

  assert.equal(result.chipFamily, "ESP32-S3");
  assert.deepEqual(fake.calls, ["transport", "loader", "main", "flashId", "disconnect"]);
  assert.equal(fake.resetSignals.length, 2, "probe pulses EN while IO0 remains high");
});

test("device probing cancels its outstanding read before releasing the serial device", async () => {
  const calls = [];
  let finishMain;
  class Transport {
    constructor() {
      calls.push("transport");
      this.reader = { async cancel() { calls.push("cancel-reader"); finishMain(); } };
    }
    async disconnect() {
      calls.push("disconnect");
      await this.reader.cancel();
    }
  }
  class Loader {
    constructor() { calls.push("loader"); }
    async main() {
      calls.push("main");
      return new Promise((resolve) => { finishMain = resolve; });
    }
    async flashId() { calls.push("flashId"); }
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

  assert.deepEqual(calls, [
    "transport", "loader", "main", "cancel-reader",
    "disconnect", "cancel-reader", "disconnect", "cancel-reader"
  ]);
});

test("device probe retains the port through a late open and disconnects again afterwards", async () => {
  const calls = [];
  const opening = deferred();
  const enteredMain = deferred();
  const port = {
    opening: true,
    open: true,
    getInfo() { return { usbVendorId: 0x303a, usbProductId: 0x1001 }; },
    async close() {
      calls.push("close");
      if (this.opening) throw Object.assign(new Error("open pending"), { name: "InvalidStateError" });
      this.open = false;
    }
  };
  class Transport {
    constructor() { calls.push("transport"); }
    async disconnect() {
      calls.push("disconnect");
      await port.close();
    }
  }
  class Loader {
    constructor() { this.chip = { CHIP_NAME: "ESP32-S3" }; calls.push("loader"); }
    async main() {
      calls.push("main-open-pending");
      enteredMain.resolve();
      await opening.promise;
      port.opening = false;
      calls.push("main-open-complete");
    }
  }

  let settled = false;
  const probing = probeDevice({
    port,
    manifest,
    TransportCtor: Transport,
    ESPLoaderCtor: Loader,
    timeoutMs: 20
  });
  probing.finally(() => { settled = true; }).catch(() => {});
  await enteredMain.promise;
  await new Promise((resolve) => setTimeout(resolve, 30));
  assert.equal(calls.includes("close"), true, "timeout attempts to close the port");
  assert.equal(settled, false, "the pending open still owns the port");
  opening.resolve();
  await assert.rejects(probing, { name: "DeviceProbeTimeoutError" });
  assert.equal(port.open, false, "a second disconnect closes a port opened after the first attempt");
  assert.equal(calls.filter((call) => call === "disconnect").length, 2);
});

async function runFlash(eraseFirst) {
  const fake = fakeEsptool();
  const states = [];
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
    delay: async () => {}
  });
  return { fake, states, result };
}

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((accept, decline) => {
    resolve = accept;
    reject = decline;
  });
  return { promise, resolve, reject };
}

function fakeInstallerPage() {
  const elements = new Map();
  const element = (id) => {
    if (!elements.has(id)) {
      elements.set(id, {
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
  return { root, element, steps, modes };
}

async function withInstallerPage(confirmEraseImpl, exercise, beforeConnect = () => {}) {
  const previous = { location: globalThis.location, isSecureContext: globalThis.isSecureContext };
  globalThis.location = { href: "https://example.test/", pathname: "/" };
  globalThis.isSecureContext = true;
  try {
    const fake = fakeEsptool();
    const page = fakeInstallerPage();
    const downloads = [];
    const installer = attachWebInstaller({
      root: page.root,
      serial: { async requestPort() { return fake.port; }, addEventListener() {} },
      TransportCtor: fake.Transport,
      ESPLoaderCtor: fake.Loader,
      confirmEraseImpl,
      verifyBootImpl: async ({ expectedVersion }) => ({ version: expectedVersion, log: "" }),
      fetchImpl: async (url) => {
        if (url.endsWith("manifest.json")) {
          return { ok: true, status: 200, json: async () => structuredClone(manifest) };
        }
        downloads.push(url);
        return firmwareResponse(url);
      }
    });
    for (let attempt = 0; page.element("firmware-version-value").textContent === "" && attempt < 50; attempt++) {
      await new Promise((resolve) => setTimeout(resolve, 0));
    }
    assert.equal(page.element("firmware-version-value").textContent, manifest.version);
    beforeConnect(page);
    await installer.connect();
    await exercise({ fake, page, downloads, installer });
  } finally {
    globalThis.location = previous.location;
    globalThis.isSecureContext = previous.isSecureContext;
  }
}

test("first-install guidance never replaces the Keep default or prompts during an update", async () => {
  await withInstallerPage(() => { assert.fail("Keep must not prompt for erase"); }, async ({ fake, page, installer }) => {
    assert.deepEqual(page.modes.map((mode) => mode.checked), [true, false]);
    assert.equal(page.element("keep-recommended").hidden, false);
    assert.equal(page.element("erase-recommended").hidden, false);
    assert.match(page.element("page-status").textContent, /Keep configuration preserves/);
    await installer.install();
    assert.equal(fake.calls.includes("erase"), false);
    assert.equal(page.root.dataset.finished, "true");
  });
});

test("connecting preserves a deliberate Erase choice without claiming Keep is selected", async () => {
  await withInstallerPage(() => false, async ({ page }) => {
    assert.deepEqual(page.modes.map((mode) => mode.checked), [false, true]);
    assert.match(page.element("page-status").textContent, /For a first install, choose Erase/);
    assert.doesNotMatch(page.element("page-status").textContent, /Keep.*selected/);
  }, (page) => {
    page.modes[0].checked = false;
    page.modes[1].checked = true;
  });
});

test("cancelling Erase requires a fresh confirmation and performs no flash work", async () => {
  const messages = [];
  await withInstallerPage((message) => { messages.push(message); return false; }, async ({ fake, page, downloads, installer }) => {
    page.modes[0].checked = false;
    page.modes[1].checked = true;
    const priorCalls = structuredClone(fake.calls);
    await installer.install();
    await installer.install();
    assert.equal(messages.length, 2);
    assert.match(messages[0], /WiFi, VIN, MQTT settings, the private pairing key and BLE sessions/);
    assert.match(messages[0], /Back up secret NVS/);
    assert.deepEqual(fake.calls, priorCalls);
    assert.deepEqual(downloads, []);
    assert.equal(page.element("install-button").disabled, false);
    assert.match(page.element("page-status").textContent, /cancelled.*No flash changes/);
  });
});

test("an explicit Erase confirmation allows the factory reset and verified install", async () => {
  let confirmations = 0;
  await withInstallerPage(() => { confirmations += 1; return true; }, async ({ fake, page, installer }) => {
    page.modes[0].checked = false;
    page.modes[1].checked = true;
    await installer.install();
    assert.equal(confirmations, 1);
    assert.equal(fake.calls.filter((call) => call === "erase").length, 1);
    assert.ok(fake.calls.indexOf("erase") < fake.calls.findIndex((call) => Array.isArray(call) && call[0] === "write"));
    assert.equal(page.root.dataset.finished, "true");
  });
});

test("Erase fails closed when browser confirmation is unavailable", async () => {
  const previous = globalThis.confirm;
  globalThis.confirm = undefined;
  try {
    await withInstallerPage(undefined, async ({ fake, page, downloads, installer }) => {
      page.modes[0].checked = false;
      page.modes[1].checked = true;
      const priorCalls = structuredClone(fake.calls);
      await installer.install();
      assert.deepEqual(fake.calls, priorCalls);
      assert.deepEqual(downloads, []);
      assert.match(page.element("page-status").textContent, /cancelled/);
    });
  } finally {
    globalThis.confirm = previous;
  }
});

test("keep-configuration writes ota_0 first, activates it last, and restarts via EN", async () => {
  const { fake, states, result } = await runFlash(false);
  assert.equal(result.chipFamily, "ESP32-S3");
  assert.equal(result.restarted, true);
  assert.equal(fake.calls.includes("erase"), false);
  assert.deepEqual(fake.calls.slice(-5), [
    ["write", [0]],
    ["write", [0x8000]],
    ["write", [0x20000]],
    ["write", [0xf000]],
    "disconnect"
  ]);
  assert.deepEqual(fake.resetSignals, [
    { dataTerminalReady: false, requestToSend: true },
    { dataTerminalReady: false, requestToSend: false }
  ]);
  assert.equal(states.some((item) => item.stage === "activating"), true);
  assert.equal(states.at(-1).stage, "restarting");
  assert.equal(states.at(-1).percentage, 100);
});

test("a flash timeout aborts delayed downloads before any flash phase can start", async () => {
  const fake = fakeEsptool();
  const requests = [];
  const flash = flashDevice({
    port: fake.port,
    manifest,
    manifestUrl: "https://example.test/manifest.json",
    eraseFirst: false,
    TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader,
    fetchImpl(url, options) {
      const pending = deferred();
      requests.push({ url, signal: options.signal, ...pending });
      return pending.promise;
    },
    cryptoImpl: webcrypto,
    timeoutMs: 20,
    cleanupTimeoutMs: 10,
    delay: async () => {}
  });

  await assert.rejects(flash, { name: "FlashTimeoutError" });
  assert.equal(requests.length, 4);
  assert.ok(requests.every((request) => request.signal instanceof AbortSignal && request.signal.aborted));
  requests.forEach(({ url, resolve }) => resolve(firmwareResponse(url)));
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(fake.calls.some((call) => Array.isArray(call) && call[0] === "write"), false);
  assert.equal(fake.calls.at(-1), "disconnect");
  assert.equal(fake.resetSignals.length, 2, "failed flash cleanup returns the chip to user firmware");
});

test("a flash timeout stops a delayed write before the OTA activation phase", async () => {
  const fake = fakeEsptool();
  let finishWrite;
  let delayedWrite;
  class DelayedLoader extends fake.Loader {
    writeFlash(options) {
      fake.calls.push(["write", options.fileArray.map((part) => part.address)]);
      const finish = deferred();
      finishWrite = finish.resolve;
      delayedWrite = (async () => {
        // Mirror esptool-js 0.6.1: each file starts with flashDeflBegin and progress callback;
        // flashDeflFinish is awaited before the internal loop starts the next file.
        for (let index = 0; index < options.fileArray.length; index += 1) {
          const part = options.fileArray[index];
          fake.calls.push(["flashDeflBegin", part.address]);
          options.reportProgress(index, 0, part.data.length);
          if (index === 0) await finish.promise;
          fake.calls.push(["flashDeflFinish", part.address]);
        }
      })();
      return delayedWrite;
    }
  }

  const flash = flashDevice({
    port: fake.port,
    manifest,
    manifestUrl: "https://example.test/manifest.json",
    eraseFirst: false,
    TransportCtor: fake.Transport,
    ESPLoaderCtor: DelayedLoader,
    fetchImpl: async (url) => firmwareResponse(url),
    cryptoImpl: webcrypto,
    timeoutMs: 20,
    cleanupTimeoutMs: 10
  });

  await new Promise((resolve) => setTimeout(resolve, 40));
  assert.equal(fake.resetSignals.length, 0);
  assert.equal(fake.calls.includes("disconnect"), false);
  assert.deepEqual(fake.calls.filter((call) => Array.isArray(call) && call[0] === "write"), [
    ["write", [0]]
  ]);

  finishWrite();
  await assert.rejects(flash, { name: "FlashTimeoutError" });
  assert.deepEqual(fake.calls.filter((call) => Array.isArray(call) && call[0] === "write"), [
    ["write", [0]]
  ]);
  assert.equal(fake.calls.at(-1), "disconnect");
  assert.equal(fake.resetSignals.length, 2);
  await delayedWrite;
  assert.deepEqual(fake.calls.filter((call) => Array.isArray(call) && call[0] === "write"), [
    ["write", [0]]
  ]);
  assert.deepEqual(fake.calls.filter((call) => Array.isArray(call) && call[0] === "flashDeflBegin"), [
    ["flashDeflBegin", 0]
  ]);
});

test("a flash timeout waits for a pending loader command before transport cleanup", async () => {
  const fake = fakeEsptool();
  let finishMain;
  class ReaderAwareTransport {
    constructor() {
      this.reader = {
        async cancel() {
          fake.calls.push("cancel-reader");
          finishMain();
        }
      };
      fake.calls.push("transport");
    }
    async disconnect() { fake.calls.push("disconnect"); }
  }
  class DelayedLoader extends fake.Loader {
    main() {
      fake.calls.push("main-pending");
      return new Promise((resolve) => { finishMain = resolve; });
    }
  }

  const flash = flashDevice({
    port: fake.port,
    manifest,
    manifestUrl: "https://example.test/manifest.json",
    eraseFirst: false,
    TransportCtor: ReaderAwareTransport,
    ESPLoaderCtor: DelayedLoader,
    fetchImpl: async (url) => firmwareResponse(url),
    cryptoImpl: webcrypto,
    timeoutMs: 20,
    cleanupTimeoutMs: 10
  });

  await assert.rejects(flash, { name: "FlashTimeoutError" });
  assert.equal(fake.calls.includes("cancel-reader"), true);
  assert.equal(fake.calls.includes("flashId"), false);
  assert.equal(fake.calls.some((call) => Array.isArray(call) && call[0] === "write"), false);
  assert.equal(fake.calls.at(-1), "disconnect");
  assert.equal(fake.resetSignals.length, 2);
});

test("a flash timeout retains port ownership until disconnect actually settles", async () => {
  const fake = fakeEsptool();
  const disconnect = deferred();
  const write = deferred();
  let settled = false;
  class DelayedDisconnectTransport extends fake.Transport {
    async disconnect() {
      fake.calls.push("disconnect-start");
      await disconnect.promise;
      fake.calls.push("disconnect-end");
    }
  }
  class DelayedWriteLoader extends fake.Loader {
    async writeFlash() {
      fake.calls.push("write-start");
      await write.promise;
      fake.calls.push("write-end");
    }
  }

  const flash = flashDevice({
    port: fake.port,
    manifest,
    manifestUrl: "https://example.test/manifest.json",
    eraseFirst: false,
    TransportCtor: DelayedDisconnectTransport,
    ESPLoaderCtor: DelayedWriteLoader,
    fetchImpl: async (url) => firmwareResponse(url),
    cryptoImpl: webcrypto,
    timeoutMs: 20,
    cleanupTimeoutMs: 10,
    delay: async () => {}
  });
  flash.finally(() => { settled = true; }).catch(() => {});

  await new Promise((resolve) => setTimeout(resolve, 40));
  assert.equal(settled, false, "the in-flight command remains reserved after timeout");
  write.resolve();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(fake.calls.includes("disconnect-start"), true);
  assert.equal(settled, false, "the installer must retain ownership while disconnect is pending");
  disconnect.resolve();
  await assert.rejects(flash, { name: "FlashTimeoutError" });
  assert.equal(fake.calls.includes("disconnect-end"), true);
});

test("a flash timeout aborts the active Web Serial writer and rejects later writes", async () => {
  const fake = fakeEsptool();
  const pendingWrite = deferred();
  let writerCount = 0;
  const writer = {
    write(bytes) {
      fake.calls.push(["wire-write", Array.from(bytes)]);
      return pendingWrite.promise;
    },
    abort(reason) {
      fake.calls.push("writer-abort");
      pendingWrite.reject(reason);
      return Promise.resolve();
    },
    releaseLock() { fake.calls.push("release-lock"); }
  };
  class AbortableTransport {
    constructor() {
      this.device = { writable: { getWriter() { writerCount += 1; return writer; } } };
      fake.calls.push("transport");
    }
    slipWriter(bytes) { return bytes; }
    async disconnect() { fake.calls.push("disconnect"); }
  }
  class PendingWriteLoader extends fake.Loader {
    async writeFlash(options) {
      fake.calls.push(["write", options.fileArray.map((part) => part.address)]);
      const transport = this.options.transport;
      try {
        await transport.write(Uint8Array.from([1]));
      } catch (error) {
        await assert.rejects(transport.write(Uint8Array.from([2])), { name: "FlashTimeoutError" });
        fake.calls.push("late-write-blocked");
        throw error;
      }
    }
  }

  const states = [];
  await assert.rejects(flashDevice({
    port: fake.port,
    manifest,
    manifestUrl: "https://example.test/manifest.json",
    eraseFirst: false,
    TransportCtor: AbortableTransport,
    ESPLoaderCtor: PendingWriteLoader,
    fetchImpl: async (url) => firmwareResponse(url),
    cryptoImpl: webcrypto,
    onState(state) { states.push(state); },
    timeoutMs: 20,
    cleanupTimeoutMs: 10
  }), { name: "FlashTimeoutError" });

  assert.equal(writerCount, 1);
  assert.equal(fake.calls.filter((call) => Array.isArray(call) && call[0] === "wire-write").length, 1);
  assert.ok(fake.calls.indexOf("writer-abort") < fake.calls.indexOf("release-lock"));
  assert.ok(fake.calls.indexOf("late-write-blocked") < fake.calls.indexOf("disconnect"));
  assert.equal(fake.resetSignals.length, 2);
  assert.equal(states.some((state) => state.stage === "stopping"), true);
  assert.equal(fake.calls.at(-1), "disconnect");
});

test("a flash timeout waits for a pending erase and never starts a write phase", async () => {
  const fake = fakeEsptool();
  let finishErase;
  class DelayedEraseLoader extends fake.Loader {
    eraseFlash() {
      fake.calls.push("erase-pending");
      return new Promise((resolve) => { finishErase = resolve; });
    }
  }

  const flash = flashDevice({
    port: fake.port,
    manifest,
    manifestUrl: "https://example.test/manifest.json",
    eraseFirst: true,
    TransportCtor: fake.Transport,
    ESPLoaderCtor: DelayedEraseLoader,
    fetchImpl: async (url) => firmwareResponse(url),
    cryptoImpl: webcrypto,
    timeoutMs: 20,
    cleanupTimeoutMs: 10
  });

  await new Promise((resolve) => setTimeout(resolve, 40));
  assert.equal(fake.resetSignals.length, 0);
  assert.equal(fake.calls.includes("disconnect"), false);
  assert.equal(fake.calls.some((call) => Array.isArray(call) && call[0] === "write"), false);
  finishErase();
  await assert.rejects(flash, { name: "FlashTimeoutError" });
  assert.equal(fake.calls.some((call) => Array.isArray(call) && call[0] === "write"), false);
  assert.equal(fake.calls.at(-1), "disconnect");
  assert.equal(fake.resetSignals.length, 2);
});

test("the installer disconnect event aborts a pending flash and keeps its late fetch inert", async () => {
  const previous = { location: globalThis.location, isSecureContext: globalThis.isSecureContext };
  globalThis.location = { href: "https://example.test/", pathname: "/" };
  globalThis.isSecureContext = true;
  try {
    const pageManifest = structuredClone(manifest);
    pageManifest.new_install_prompt_erase = false;
    const fake = fakeEsptool();
    const page = fakeInstallerPage();
    const requests = [];
    let disconnectHandler;
    const serial = {
      async requestPort() { return fake.port; },
      addEventListener(name, handler) { if (name === "disconnect") disconnectHandler = handler; }
    };
    const installer = attachWebInstaller({
      root: page.root,
      serial,
      TransportCtor: fake.Transport,
      ESPLoaderCtor: fake.Loader,
      fetchImpl(url, options) {
        if (url.endsWith("manifest.json")) {
          return Promise.resolve({ ok: true, status: 200, json: async () => pageManifest });
        }
        const pending = deferred();
        requests.push({ url, signal: options.signal, ...pending });
        return pending.promise;
      }
    });
    for (let attempt = 0; page.element("firmware-version-value").textContent === "" && attempt < 50; attempt++) {
      await new Promise((resolve) => setTimeout(resolve, 0));
    }

    await installer.connect();
    assert.equal(page.root.dataset.connected, "true");
    const install = installer.install();
    for (let attempt = 0; requests.length < 4 && attempt < 50; attempt++) {
      await new Promise((resolve) => setTimeout(resolve, 0));
    }
    assert.equal(requests.length, 4);
    await disconnectHandler({ target: fake.port });
    await install;
    requests.forEach(({ url, resolve }) => resolve(firmwareResponse(url)));
    await new Promise((resolve) => setImmediate(resolve));

    assert.ok(requests.every((request) => request.signal instanceof AbortSignal && request.signal.aborted));
    assert.equal(page.root.dataset.connected, "false");
    assert.match(page.element("page-status").textContent, /USB disconnected during installation/);
    assert.equal(page.element("install-button").disabled, true);
    assert.equal(page.element("connect-button").disabled, false);
    assert.equal(fake.calls.some((call) => Array.isArray(call) && call[0] === "write"), false);
  } finally {
    globalThis.location = previous.location;
    globalThis.isSecureContext = previous.isSecureContext;
  }
});

test("disconnect during device probing keeps connect and install locked through cleanup", async () => {
  const previous = { location: globalThis.location, isSecureContext: globalThis.isSecureContext };
  globalThis.location = { href: "https://example.test/", pathname: "/" };
  globalThis.isSecureContext = true;
  try {
    const pageManifest = structuredClone(manifest);
    pageManifest.new_install_prompt_erase = false;
    const fake = fakeEsptool();
    const page = fakeInstallerPage();
    const firstMain = deferred();
    const firstMainStarted = deferred();
    let loaderCount = 0;
    let requestCount = 0;
    let disconnectHandler;
    class ProbeTransport extends fake.Transport {
      constructor(port) {
        super(port);
        this.reader = { async cancel() { firstMain.resolve(); } };
      }
    }
    class ProbeLoader extends fake.Loader {
      async main() {
        loaderCount += 1;
        if (loaderCount === 1) {
          fake.calls.push("first-main-pending");
          firstMainStarted.resolve();
          await firstMain.promise;
          return;
        }
        return super.main();
      }
    }
    const serial = {
      async requestPort() { requestCount += 1; return fake.port; },
      addEventListener(name, handler) { if (name === "disconnect") disconnectHandler = handler; }
    };
    const installer = attachWebInstaller({
      root: page.root,
      serial,
      TransportCtor: ProbeTransport,
      ESPLoaderCtor: ProbeLoader,
      verifyBootImpl: async ({ expectedVersion }) => ({ version: expectedVersion, log: "" }),
      fetchImpl: async (url) => url.endsWith("manifest.json")
        ? { ok: true, status: 200, json: async () => pageManifest }
        : firmwareResponse(url)
    });
    for (let attempt = 0; page.element("firmware-version-value").textContent === "" && attempt < 50; attempt++) {
      await new Promise((resolve) => setTimeout(resolve, 0));
    }

    const firstConnect = installer.connect();
    await firstMainStarted.promise;
    await disconnectHandler({ target: fake.port });
    await installer.connect();
    await installer.install();
    assert.equal(requestCount, 1, "a second connection cannot start while the first probe owns the port");
    assert.equal(fake.calls.some((call) => Array.isArray(call) && call[0] === "write"), false);

    await firstConnect;
    assert.equal(page.root.dataset.connected, "false");
    await installer.connect();
    assert.equal(page.root.dataset.connected, "true");
    await installer.install();
    assert.equal(requestCount, 2);
    assert.equal(fake.calls.filter((call) => Array.isArray(call) && call[0] === "write").length, 4);
    assert.equal(loaderCount, 3, "one cancelled probe, one accepted probe and one flash loader ran");
  } finally {
    globalThis.location = previous.location;
    globalThis.isSecureContext = previous.isSecureContext;
  }
});

test("disconnect waits for a pending serial monitor open before closing or releasing the port", async () => {
  const previous = { location: globalThis.location, isSecureContext: globalThis.isSecureContext };
  globalThis.location = { href: "https://example.test/", pathname: "/" };
  globalThis.isSecureContext = true;
  try {
    const fake = fakeEsptool();
    const page = fakeInstallerPage();
    const opening = deferred();
    const openStarted = deferred();
    const readPending = deferred();
    const calls = [];
    const reader = {
      async read() { return readPending.promise; },
      async cancel() { calls.push("cancel"); readPending.resolve({ done: true }); },
      releaseLock() { calls.push("release"); }
    };
    const port = {
      readable: null,
      writable: null,
      getInfo() { return { usbVendorId: 0x303a, usbProductId: 0x1001 }; },
      async open() {
        calls.push("open-start");
        openStarted.resolve();
        await opening.promise;
        this.readable = { getReader() { return reader; } };
        this.writable = {};
        calls.push("open-end");
      },
      async setSignals() {},
      async close() {
        calls.push("close");
        this.readable = null;
        this.writable = null;
      }
    };
    const installer = attachWebInstaller({
      root: page.root,
      serial: { requestPort: async () => port, addEventListener() {} },
      TransportCtor: fake.Transport,
      ESPLoaderCtor: fake.Loader,
      fetchImpl: async (url) => url.endsWith("manifest.json")
        ? { ok: true, status: 200, json: async () => manifest }
        : firmwareResponse(url)
    });
    for (let attempt = 0; page.element("firmware-version-value").textContent === "" && attempt < 50; attempt++) {
      await new Promise((resolve) => setTimeout(resolve, 0));
    }
    await installer.connect();
    const starting = installer.startMonitor();
    await openStarted.promise;
    const disconnecting = installer.disconnect();
    await installer.install();
    await installer.connect();
    assert.equal(calls.includes("close"), false, "the in-flight open still owns the port");
    assert.equal(page.root.dataset.connected, "true");
    opening.resolve();
    await Promise.all([starting, disconnecting]);

    assert.ok(calls.indexOf("open-end") < calls.indexOf("cancel"));
    assert.ok(calls.indexOf("cancel") < calls.indexOf("release"));
    assert.ok(calls.indexOf("release") < calls.indexOf("close"));
    assert.equal(page.root.dataset.connected, "false");
    assert.equal(fake.calls.some((call) => Array.isArray(call) && call[0] === "write"), false);
  } finally {
    globalThis.location = previous.location;
    globalThis.isSecureContext = previous.isSecureContext;
  }
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
  const fake = fakeEsptool("ESP32", {
    chipRevision: 1,
    info: { usbVendorId: 0x10c4, usbProductId: 0xea60 }
  });
  await assert.rejects(
    probeDevice({
      port: fake.port,
      manifest,
      TransportCtor: fake.Transport,
      ESPLoaderCtor: fake.Loader
    }),
    (error) => error.name === "UnsupportedChipRevisionError" &&
      /ECO3/.test(error.message) && /v3\.0/.test(error.message)
  );
  // The rejected probe still hands control back to the app and closes the port.
  assert.deepEqual(fake.calls, ["transport", "loader", "main", "flashId", "disconnect"]);
  assert.equal(fake.resetSignals.length, 2);
});

test("probing accepts an ECO3 (revision v3) classic ESP32", async () => {
  const fake = fakeEsptool("ESP32", {
    chipRevision: 3,
    info: { usbVendorId: 0x10c4, usbProductId: 0xea60 }
  });
  const result = await probeDevice({
    port: fake.port,
    manifest,
    TransportCtor: fake.Transport,
    ESPLoaderCtor: fake.Loader
  });
  assert.equal(result.chipFamily, "ESP32");
  assert.equal(result.build.parts[0].path, "bootloader-esp32.bin");
});

test("installing refuses a pre-ECO3 classic ESP32 before erasing or writing flash", async () => {
  const fake = fakeEsptool("ESP32", {
    chipRevision: 1,
    info: { usbVendorId: 0x10c4, usbProductId: 0xea60 }
  });
  await assert.rejects(
    flashDevice({
      port: fake.port,
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

test("boot verification keeps the port reserved until close actually settles", async () => {
  const calls = [];
  const closing = deferred();
  const reader = {
    async read() { return { value: new TextEncoder().encode("App version: 1.4.75\n"), done: false }; },
    async cancel() { calls.push("cancel"); },
    releaseLock() { calls.push("release"); }
  };
  const port = {
    readable: { getReader() { return reader; } },
    writable: {},
    async setSignals() {},
    async close() {
      calls.push("close-start");
      await closing.promise;
      this.readable = null;
      this.writable = null;
      calls.push("close-end");
    }
  };

  let settled = false;
  const verification = verifyBootVersion({
    port,
    expectedVersion: "1.4.75",
    delay: async () => {},
    timeoutMs: 1000,
    cleanupTimeoutMs: 10
  });
  verification.then(() => { settled = true; });
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.deepEqual(calls.slice(-2), ["release", "close-start"]);
  assert.equal(settled, false, "the port remains reserved until its close operation completes");
  closing.resolve();
  const result = await verification;
  assert.equal(result.version, "1.4.75");
  assert.deepEqual(calls.slice(-3), ["release", "close-start", "close-end"]);
});

test("boot verification closes a port whose open completes after cancellation", async () => {
  const calls = [];
  const opening = deferred();
  const controller = new AbortController();
  const port = {
    readable: null,
    writable: null,
    async open() {
      calls.push("open");
      await opening.promise;
      this.readable = {};
      this.writable = {};
    },
    async close() {
      calls.push("close");
      this.readable = null;
      this.writable = null;
    }
  };

  const verification = verifyBootVersion({
    port,
    expectedVersion: "1.4.75",
    signal: controller.signal,
    delay: async () => {},
    timeoutMs: 1000,
    cleanupTimeoutMs: 20
  });
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(calls, ["open"]);
  controller.abort(Object.assign(new Error("verification cancelled"), { name: "AbortError" }));
  let settled = false;
  verification.finally(() => { settled = true; }).catch(() => {});
  await new Promise((resolve) => setTimeout(resolve, 10));
  assert.equal(settled, false, "the port remains reserved while open is unresolved");
  assert.equal(calls.includes("close"), false);

  opening.resolve();
  await assert.rejects(verification, { name: "AbortError" });
  assert.deepEqual(calls, ["open", "close"]);
  assert.equal(port.readable, null);
});

test("serial reader cleanup retains ownership until cancel and read settle", async () => {
  const cancel = deferred();
  const read = deferred();
  let settled = false;
  const stopping = stopSerialReader({ cancel() { return cancel.promise; } }, read.promise, 10);
  stopping.then(() => { settled = true; });
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(settled, false, "a cleanup deadline must not release a still-locked reader");
  cancel.resolve();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(settled, false, "a pending read loop still owns its reader");
  read.resolve();
  await stopping;
  assert.equal(settled, true);
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
