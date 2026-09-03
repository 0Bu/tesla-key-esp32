const ESPRESSIF_USB_VENDOR_ID = 0x303a;
const CDC_PRODUCT_IDS = new Set([0x0002, 0x0003, 0x1001, 0x1002, 0x1003]);
const MAX_MONITOR_CHARS = 100000;
const DEVICE_PROBE_TIMEOUT_MS = 10000;
const TRANSPORT_CLEANUP_TIMEOUT_MS = 2000;
const FLASH_OPERATION_TIMEOUT_MS = 180000;
const BOOT_VERIFICATION_TIMEOUT_MS = 20000;
const MANIFEST_LAYOUT_VERSION = 2;
const OTADATA_OFFSET = 0xf000;
const OTADATA_SIZE = 0x2000;
const PARTITION_TABLE_OFFSET = 0x8000;
const APP_OFFSET = 0x20000;
const APP_SLOT_SIZE = 0x1f0000;
const SUPPORTED_CHIPS = ["ESP32", "ESP32-S3", "ESP32-C3", "ESP32-C6"];
// Classic ESP32 firmware is built with CONFIG_ESP32_REV_MIN_3 (see sdkconfig.defaults.esp32) so the
// signed image boots only on chip revision v3.0 (ECO3) or newer. esptool reports the classic ESP32
// wafer revision as a bare major integer, so ECO3 is revision >= 3. A pre-ECO3 chip still flashes to
// 100% and only then has its image rejected by the 2nd-stage bootloader ("chip revision check
// failed. Required >= v3.0"), which looks like a brick. The other targets meet their signing
// scheme's floor at their default revision, so only ESP32 carries a minimum here.
const MIN_CHIP_REVISION_MAJOR = { ESP32: 3 };
const ANSI_CONTROL_SEQUENCE = /\x1B\[[0-?]*[ -/]*[@-~]/g;
const sleep = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

function errorWithName(name, message) {
  const error = new Error(message);
  error.name = name;
  return error;
}

function errorMessage(error) {
  return error && typeof error.message === "string" ? error.message : String(error);
}

export function stripSerialAnsi(text) {
  return String(text || "").replace(ANSI_CONTROL_SEQUENCE, "");
}

export function serialLogLevel(line) {
  const plain = stripSerialAnsi(line).trimStart();
  if (/^E\s+\(/.test(plain)) return "error";
  if (/^W\s+\(/.test(plain)) return "warning";
  return "info";
}

export function splitSerialChunk(pending, text, flush = false) {
  const combined = `${pending || ""}${text || ""}`;
  // A CR/LF pair may be split across two Web Serial reads. Hold a trailing CR until the next chunk
  // so it remains one line ending instead of becoming an empty line followed by LF.
  const holdTrailingCr = !flush && combined.endsWith("\r");
  const source = holdTrailingCr ? combined.slice(0, -1) : combined;
  const parts = source.replace(/\r\n?/g, "\n").split("\n");
  let nextPending = parts.pop() || "";
  if (holdTrailingCr) nextPending += "\r";

  const lines = parts.map((line) => ({ text: line, terminated: true }));
  if (flush && nextPending) {
    lines.push({ text: nextPending, terminated: false });
    nextPending = "";
  }
  return { lines, pending: nextPending };
}

async function withTimeout(operation, milliseconds, timeoutError) {
  let timeoutId;
  const timeout = new Promise((_, reject) => {
    timeoutId = setTimeout(() => reject(timeoutError), milliseconds);
  });
  try {
    return await Promise.race([Promise.resolve(operation), timeout]);
  } finally {
    clearTimeout(timeoutId);
  }
}

export async function stopSerialReader(reader, loop, timeoutMs = TRANSPORT_CLEANUP_TIMEOUT_MS) {
  if (reader && typeof reader.cancel === "function") {
    try {
      await withTimeout(reader.cancel(), timeoutMs, new Error("Stopping the serial reader timed out."));
    } catch (_error) {}
  }
  if (loop) {
    try {
      await withTimeout(loop, timeoutMs, new Error("Stopping the serial read loop timed out."));
    } catch (_error) {}
  }
}

function terminalAdapter(onLog) {
  return {
    clean() {},
    write(data) { onLog(String(data)); },
    writeLine(data) { onLog(`${String(data)}\n`); }
  };
}

async function settleTransport(transport, loader, resetMode, timeoutMs = TRANSPORT_CLEANUP_TIMEOUT_MS) {
  if (resetMode && loader && loader.chip && typeof loader.after === "function") {
    try {
      await withTimeout(loader.after(resetMode), timeoutMs, new Error("Device reset timed out."));
    } catch (_error) {
      // Cleanup still has to close the port if a reset signal is not supported by the adapter.
    }
  }
  if (transport && typeof transport.disconnect === "function") {
    try {
      await withTimeout(transport.disconnect(), timeoutMs, new Error("Serial cleanup timed out."));
    } catch (_error) {
      // The device can disappear during its reset. In that case the browser already closed it.
    }
  }
}

export async function resetToUserFirmware(port, delay = sleep) {
  if (!port || typeof port.setSignals !== "function") {
    throw new Error("This serial adapter cannot reset the ESP automatically.");
  }

  // Keep IO0 high while EN is pulsed low, then release EN. This is the same firmware-mode reset
  // used by ESPConnect and avoids leaving the chip in the flasher stub after probing it.
  await port.setSignals({ dataTerminalReady: false, requestToSend: true });
  await delay(100);
  await port.setSignals({ dataTerminalReady: false, requestToSend: false });
  await delay(250);
}

export async function resetConnectedDevice(port, { keepOpen = false, delay = sleep } = {}) {
  if (!port) throw new Error("No serial device is connected.");
  const wasOpen = Boolean(port.readable || port.writable);

  if (!wasOpen) await port.open({ baudRate: 115200, bufferSize: 8192 });
  try {
    await resetToUserFirmware(port, delay);
  } finally {
    if (!wasOpen && !keepOpen && (port.readable || port.writable)) await port.close();
  }
}

export function detectedSerialType(info = {}) {
  return info.usbVendorId === ESPRESSIF_USB_VENDOR_ID &&
    CDC_PRODUCT_IDS.has(info.usbProductId) ? "cdc" : "uart";
}

export function describeSerialConnection(info = {}) {
  if (detectedSerialType(info) === "cdc") return "USB Serial/JTAG";
  if (info.usbVendorId !== undefined) return "USB UART";
  return "Web Serial";
}

export function selectManifestBuild(manifest, chipFamily, info = {}) {
  if (!manifest || !Array.isArray(manifest.builds)) return undefined;
  const serialType = detectedSerialType(info);
  return manifest.builds.find((build) =>
    build.chipFamily === chipFamily && build.serialType === serialType
  ) || manifest.builds.find((build) =>
    build.chipFamily === chipFamily && build.serialType === undefined
  );
}

export function combinedProgress(fileArray, fileIndex, written, total) {
  const totalBytes = fileArray.reduce((sum, file) => sum + file.data.length, 0);
  if (!totalBytes || !fileArray[fileIndex]) return 0;
  const completedBytes = fileArray
    .slice(0, fileIndex)
    .reduce((sum, file) => sum + file.data.length, 0);
  const currentBytes = total > 0
    ? Math.min(1, Math.max(0, written / total)) * fileArray[fileIndex].data.length
    : 0;
  return Math.min(100, Math.max(0, Math.floor(((completedBytes + currentBytes) / totalBytes) * 100)));
}

function expectedPartLayout(chipFamily) {
  return [
    { role: "bootloader", offset: chipFamily === "ESP32" ? 0x1000 : 0, maxSize: chipFamily === "ESP32" ? 0x7000 : 0x8000 },
    { role: "partition table", offset: PARTITION_TABLE_OFFSET, maxSize: 0x1000 },
    { role: "application", offset: APP_OFFSET, maxSize: APP_SLOT_SIZE },
    { role: "OTA selector", offset: OTADATA_OFFSET, exactSize: OTADATA_SIZE }
  ];
}

function manifestError(message) {
  return errorWithName("InvalidManifestError", message);
}

function validateBuild(build, manifestUrl) {
  if (!build || !SUPPORTED_CHIPS.includes(build.chipFamily)) {
    throw manifestError("The firmware manifest contains an unsupported chip family.");
  }
  if (!Array.isArray(build.parts) || build.parts.length !== 4) {
    throw manifestError(`${build.chipFamily} must contain exactly four flash parts.`);
  }

  const base = new URL(manifestUrl);
  const baseDirectory = new URL("./", base);
  const layout = expectedPartLayout(build.chipFamily);
  build.parts.forEach((part, index) => {
    const expected = layout[index];
    if (!part || !Number.isSafeInteger(part.offset) || part.offset !== expected.offset) {
      throw manifestError(`${build.chipFamily} has an invalid ${expected.role} offset.`);
    }
    if (!Number.isSafeInteger(part.size) || part.size <= 0 ||
        (expected.exactSize !== undefined ? part.size !== expected.exactSize : part.size > expected.maxSize)) {
      throw manifestError(`${build.chipFamily} has an invalid ${expected.role} size.`);
    }
    if (typeof part.sha256 !== "string" || !/^[0-9a-f]{64}$/.test(part.sha256)) {
      throw manifestError(`${build.chipFamily} has an invalid ${expected.role} SHA-256.`);
    }
    if (typeof part.path !== "string" || !/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(part.path)) {
      throw manifestError(`${build.chipFamily} has an unsafe ${expected.role} path.`);
    }
    const partUrl = new URL(part.path, baseDirectory);
    if (partUrl.origin !== base.origin || !partUrl.href.startsWith(baseDirectory.href) || partUrl.search || partUrl.hash) {
      throw manifestError(`${build.chipFamily} ${expected.role} must be a same-directory URL.`);
    }
  });
  return build;
}

export function validateManifest(manifest, manifestUrl) {
  if (!manifest || manifest.layoutVersion !== MANIFEST_LAYOUT_VERSION) {
    throw manifestError(`Firmware manifest layoutVersion must be ${MANIFEST_LAYOUT_VERSION}.`);
  }
  if (typeof manifest.version !== "string" || manifest.version.length > 31 ||
      !/^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z.-]+)?$/.test(manifest.version)) {
    throw manifestError("The firmware manifest contains an invalid version.");
  }
  if (typeof manifest.sourceSha !== "string" || !/^[0-9a-f]{40}$/.test(manifest.sourceSha)) {
    throw manifestError("The firmware manifest contains an invalid source commit.");
  }
  if (typeof manifest.new_install_prompt_erase !== "boolean") {
    throw manifestError("The firmware manifest must declare its first-install erase policy.");
  }
  if (!Array.isArray(manifest.builds) || manifest.builds.length !== SUPPORTED_CHIPS.length) {
    throw manifestError("The firmware manifest must contain exactly four target builds.");
  }
  const families = manifest.builds.map((build) => build && build.chipFamily);
  if (SUPPORTED_CHIPS.some((family) => families.filter((item) => item === family).length !== 1)) {
    throw manifestError("The firmware manifest target set is incomplete or duplicated.");
  }
  manifest.builds.forEach((build) => validateBuild(build, manifestUrl));
  return manifest;
}

export async function sha256Hex(data, cryptoImpl = globalThis.crypto) {
  if (!cryptoImpl || !cryptoImpl.subtle || typeof cryptoImpl.subtle.digest !== "function") {
    throw errorWithName("IntegrityCheckError", "This browser cannot verify firmware SHA-256 hashes.");
  }
  const digest = new Uint8Array(await cryptoImpl.subtle.digest("SHA-256", data));
  return Array.from(digest, (byte) => byte.toString(16).padStart(2, "0")).join("");
}

export async function fetchFirmwareParts(build, manifestUrl, fetchImpl = fetch, cryptoImpl = globalThis.crypto) {
  validateBuild(build, manifestUrl);

  const baseDirectory = new URL("./", manifestUrl);
  return Promise.all(build.parts.map(async (part, index) => {
    const url = new URL(part.path, baseDirectory).toString();
    const response = await fetchImpl(url, { cache: "no-store" });
    if (!response.ok) {
      throw errorWithName(
        "FirmwareDownloadError",
        `Downloading ${part.path} failed with HTTP ${response.status}.`
      );
    }
    const data = new Uint8Array(await response.arrayBuffer());
    if (data.byteLength !== part.size) {
      throw errorWithName(
        "FirmwareIntegrityError",
        `${part.path} has ${data.byteLength} bytes; the signed manifest requires ${part.size}.`
      );
    }
    const actualHash = await sha256Hex(data, cryptoImpl);
    if (actualHash !== part.sha256) {
      throw errorWithName("FirmwareIntegrityError", `${part.path} failed its SHA-256 integrity check.`);
    }
    return {
      address: part.offset,
      data,
      role: expectedPartLayout(build.chipFamily)[index].role
    };
  }));
}

export function bootLogHasVersion(text, expectedVersion) {
  if (typeof expectedVersion !== "string" || !expectedVersion) return false;
  const escaped = expectedVersion.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  return new RegExp(`(?:App version:\\s*|BOOT version=)${escaped}(?:\\s|$)`).test(stripSerialAnsi(text));
}

export async function verifyBootVersion({
  port,
  expectedVersion,
  onLog = () => {},
  timeoutMs = BOOT_VERIFICATION_TIMEOUT_MS,
  cleanupTimeoutMs = TRANSPORT_CLEANUP_TIMEOUT_MS,
  delay = sleep
}) {
  if (!port) throw errorWithName("BootVerificationError", "The serial port disappeared before boot verification.");
  if (typeof expectedVersion !== "string" || !expectedVersion) {
    throw errorWithName("BootVerificationError", "The expected firmware version is missing.");
  }

  const deadline = Date.now() + timeoutMs;
  let lastOpenError = null;
  while (!port.readable && Date.now() < deadline) {
    try {
      await withTimeout(
        port.open({ baudRate: 115200, bufferSize: 8192 }),
        Math.min(2000, Math.max(1, deadline - Date.now())),
        new Error("Opening the serial port timed out.")
      );
    } catch (error) {
      lastOpenError = error;
      await delay(Math.min(250, Math.max(0, deadline - Date.now())));
    }
  }
  if (!port.readable) {
    throw errorWithName(
      "BootVerificationError",
      `The flashed device did not reconnect for version verification${lastOpenError ? `: ${errorMessage(lastOpenError)}` : "."}`
    );
  }

  const reader = port.readable.getReader();
  const decoder = new TextDecoder();
  let captured = "";
  let readLoop;
  try {
    // Generate a fresh, observable boot after the monitor has acquired the stream. Some native USB
    // targets re-enumerate here; a read error is still a failed verification, never silent success.
    try { await resetToUserFirmware(port, delay); } catch (error) { onLog(`Automatic verification reset unavailable: ${errorMessage(error)}\n`); }
    readLoop = (async () => {
      while (true) {
        const { value, done } = await reader.read();
        if (done) return false;
        const text = decoder.decode(value, { stream: true });
        onLog(text);
        captured = `${captured}${text}`.slice(-32768);
        if (bootLogHasVersion(captured, expectedVersion)) return true;
      }
    })();
    const verified = await withTimeout(
      readLoop,
      Math.max(1, deadline - Date.now()),
      errorWithName("BootVerificationError", `Timed out waiting for firmware ${expectedVersion} to boot.`)
    );
    if (!verified) {
      throw errorWithName("BootVerificationError", `The serial stream ended before firmware ${expectedVersion} was observed.`);
    }
    return { version: expectedVersion, log: captured };
  } finally {
    await stopSerialReader(reader, readLoop, cleanupTimeoutMs);
    try { reader.releaseLock(); } catch (_error) {}
    if (port.readable || port.writable) {
      try {
        await withTimeout(port.close(), cleanupTimeoutMs, new Error("Closing boot verification timed out."));
      } catch (_error) {}
    }
  }
}

// Refuse a chip whose revision is below its firmware's minimum before any flash write, turning the
// otherwise cryptic post-flash boot loop into an actionable message. Reads that cannot resolve a
// numeric revision fail open — the normal flash path and its on-device bootloader still apply.
async function assertChipRevisionSupported(loader, chipFamily) {
  const required = MIN_CHIP_REVISION_MAJOR[chipFamily];
  const chip = loader && loader.chip;
  if (required === undefined || !chip || typeof chip.getChipRevision !== "function") return;
  let revision;
  try {
    revision = await chip.getChipRevision(loader);
  } catch (_error) {
    return;
  }
  if (typeof revision !== "number" || revision >= required) return;
  let description;
  try {
    if (typeof chip.getChipDescription === "function") description = await chip.getChipDescription(loader);
  } catch (_error) {}
  const label = description || `${chipFamily} (revision ${revision})`;
  throw errorWithName(
    "UnsupportedChipRevisionError",
    `${label} is pre-ECO3. The ${chipFamily} firmware requires chip revision v${required}.0 (ECO3) or ` +
    `newer, so this board would flash but then fail to boot ("chip revision check failed"). Use an ECO3 ` +
    `ESP32 (standard since ~2020) or an esp32s3/c3/c6 board — see Requirements in the README.`
  );
}

export async function probeDevice({
  port,
  manifest,
  TransportCtor,
  ESPLoaderCtor,
  onLog = () => {},
  timeoutMs = DEVICE_PROBE_TIMEOUT_MS,
  cleanupTimeoutMs = TRANSPORT_CLEANUP_TIMEOUT_MS
}) {
  const transport = new TransportCtor(port);
  const loader = new ESPLoaderCtor({
    transport,
    baudrate: 115200,
    terminal: terminalAdapter(onLog),
    debugLogging: false,
    enableTracing: false
  });
  let loaderReady = false;

  try {
    return await withTimeout((async () => {
      await loader.main();
      loaderReady = true;
      if (typeof loader.flashId === "function") await loader.flashId();
      const chipFamily = loader.chip && loader.chip.CHIP_NAME;
      if (!chipFamily) throw errorWithName("ChipDetectionError", "The connected ESP chip could not be identified.");
      const build = selectManifestBuild(manifest, chipFamily, port.getInfo());
      if (!build) {
        throw errorWithName("UnsupportedChipError", `${chipFamily} is not supported by this firmware.`);
      }
      await assertChipRevisionSupported(loader, chipFamily);
      return { chipFamily, build };
    })(), timeoutMs, errorWithName(
      "DeviceProbeTimeoutError",
      "The serial device did not answer in flashing mode. Select the ESP chip's native USB port when available; otherwise hold BOOT, tap RESET, then try again."
    ));
  } finally {
    // The compatibility probe runs the flasher stub. Explicitly hand control back to the installed
    // application before releasing the serial port.
    await settleTransport(transport, loader, loaderReady ? "soft_reset" : undefined, cleanupTimeoutMs);
  }
}

export async function flashDevice({
  port,
  manifest,
  manifestUrl,
  eraseFirst,
  TransportCtor,
  ESPLoaderCtor,
  fetchImpl = fetch,
  cryptoImpl = globalThis.crypto,
  onState = () => {},
  onLog = () => {},
  timeoutMs = FLASH_OPERATION_TIMEOUT_MS,
  cleanupTimeoutMs = TRANSPORT_CLEANUP_TIMEOUT_MS
}) {
  const transport = new TransportCtor(port);
  const loader = new ESPLoaderCtor({
    transport,
    baudrate: 115200,
    terminal: terminalAdapter(onLog),
    debugLogging: false,
    enableTracing: false
  });
  let completed = false;

  try {
    return await withTimeout((async () => {
    validateManifest(manifest, manifestUrl);
    onState({ stage: "connecting", percentage: 0, message: "Checking device" });
    await loader.main();
    if (typeof loader.flashId === "function") await loader.flashId();

    const chipFamily = loader.chip && loader.chip.CHIP_NAME;
    const build = selectManifestBuild(manifest, chipFamily, port.getInfo());
    if (!build) {
      throw errorWithName(
        "UnsupportedChipError",
        chipFamily ? `${chipFamily} is not supported by this firmware.` : "The ESP chip could not be identified."
      );
    }
    await assertChipRevisionSupported(loader, chipFamily);

    onState({ stage: "preparing", percentage: 0, message: "Loading firmware" });
    const fileArray = await fetchFirmwareParts(build, manifestUrl, fetchImpl, cryptoImpl);

    if (eraseFirst) {
      onState({ stage: "erasing", percentage: 0, message: "Erasing flash" });
      await loader.eraseFlash();
    }

    const writePhase = async (phaseFiles, firstGlobalIndex, stage, message) => {
      onState({
        stage,
        percentage: combinedProgress(fileArray, firstGlobalIndex, 0, phaseFiles[0].data.length),
        message
      });
      await loader.writeFlash({
        fileArray: phaseFiles,
        flashSize: "keep",
        flashMode: "keep",
        flashFreq: "keep",
        eraseAll: false,
        compress: true,
        reportProgress(fileIndex, written, total) {
          onState({
            stage,
            percentage: combinedProgress(fileArray, firstGlobalIndex + fileIndex, written, total),
            message
          });
        }
      });
    };

    // Keep-mode may start on ota_1. Write bootloader/table/new ota_0 first; only after all of those
    // succeeded write ota_data_initial in its own final phase, atomically selecting the new ota_0.
    // A failure before this phase leaves the previously active image selected and bootable.
    await writePhase(fileArray.slice(0, 3), 0, "writing", "Writing firmware");
    await writePhase(fileArray.slice(3), 3, "activating", "Activating new firmware");

    onState({ stage: "restarting", percentage: 100, message: "Starting firmware" });
    await loader.after("soft_reset");
    completed = true;
    return { chipFamily, build };
    })(), timeoutMs, errorWithName("FlashTimeoutError", "Firmware installation timed out before it completed."));
  } finally {
    await settleTransport(transport, loader, completed ? undefined : "soft_reset", cleanupTimeoutMs);
  }
}

export function attachWebInstaller({
  root,
  serial,
  TransportCtor,
  ESPLoaderCtor,
  fetchImpl = fetch,
  manifestPath = "manifest.json",
  verifyBootImpl = verifyBootVersion
}) {
  if (!root) throw new Error("The installer root is missing.");

  const element = (id) => root.querySelector(`#${id}`);
  const connectButton = element("connect-button");
  const disconnectButton = element("disconnect-button");
  const resetButton = element("reset-button");
  const installButton = element("install-button");
  const monitorButton = element("serial-monitor-button");
  const connectionLabel = element("connection-label");
  const deviceValue = element("device-value");
  const connectionValue = element("connection-value");
  const compatibilityValue = element("compatibility-value");
  const monitor = element("serial-monitor");
  const monitorOutput = element("serial-monitor-output");
  const monitorLive = element("serial-monitor-live");
  const progressStage = element("progress-stage");
  const progressPercent = element("progress-percent");
  const progressTrack = element("progress-track");
  const progressFill = element("progress-fill");
  const pageStatus = element("page-status");
  const unsupported = element("serial-unsupported");
  const versionLine = element("firmware-version");
  const versionValue = element("firmware-version-value");
  const sourceLink = element("firmware-source-link");
  const sourceValue = element("firmware-source-value");
  const previewLink = element("preview-source-link");
  const steps = Array.from(root.querySelectorAll(".installer-step"));

  let selectedPort = null;
  let manifest = null;
  let monitorReader = null;
  let monitorLoop = null;
  let monitorPendingLine = "";
  let busy = false;
  const serialSupported = Boolean(
    serial && typeof serial.requestPort === "function" && globalThis.isSecureContext
  );

  const setPageStatus = (message, kind = "info") => {
    pageStatus.hidden = !message;
    pageStatus.dataset.kind = kind;
    pageStatus.textContent = message || "";
  };

  const appendRenderedMonitorLine = ({ text, terminated }) => {
    const line = monitorOutput.ownerDocument.createElement("span");
    const level = serialLogLevel(text);
    line.className = `installer-monitor-line installer-monitor-line-${level}`;
    line.textContent = `${stripSerialAnsi(text)}${terminated ? "\n" : ""}`;
    monitorOutput.append(line);
  };

  const appendMonitor = (text, { flush = false } = {}) => {
    if (!text && !flush) return;
    const parsed = splitSerialChunk(monitorPendingLine, text, flush);
    monitorPendingLine = parsed.pending;
    parsed.lines.forEach(appendRenderedMonitorLine);

    if (monitorOutput.textContent.length + monitorPendingLine.length > MAX_MONITOR_CHARS) {
      // Trim in batches so a busy serial stream does not force a full DOM rebuild on every chunk.
      const keep = Math.floor(MAX_MONITOR_CHARS * 0.8);
      const retained = `${monitorOutput.textContent}${stripSerialAnsi(monitorPendingLine)}`.slice(-keep);
      monitorOutput.replaceChildren();
      const reparsed = splitSerialChunk("", retained);
      monitorPendingLine = reparsed.pending;
      reparsed.lines.forEach(appendRenderedMonitorLine);
    }
    monitorOutput.scrollTop = monitorOutput.scrollHeight;
  };

  const appendStatusLine = (message) => {
    const time = new Date().toLocaleTimeString([], { hour12: false });
    appendMonitor(`[${time}] ${message}\n`);
  };

  const markSteps = (activeStep) => {
    steps.forEach((step, index) => {
      const number = index + 1;
      step.dataset.state = number < activeStep ? "done" : number === activeStep ? "active" : "";
    });
  };

  const setProgress = (message, percentage) => {
    const value = Math.min(100, Math.max(0, Math.round(percentage || 0)));
    progressStage.textContent = message;
    progressPercent.textContent = `${value}%`;
    progressTrack.setAttribute("aria-valuenow", String(value));
    progressFill.style.width = `${value}%`;
  };

  const closePort = async () => {
    if (!selectedPort || (!selectedPort.readable && !selectedPort.writable)) return;
    try {
      await withTimeout(
        selectedPort.close(),
        TRANSPORT_CLEANUP_TIMEOUT_MS,
        new Error("Closing the serial port timed out.")
      );
    } catch (_error) {
      // A USB reset or unplug can close the port before the page reaches cleanup.
    }
  };

  const stopMonitor = async ({ collapse = true } = {}) => {
    if (collapse) {
      root.dataset.monitor = "closed";
      monitorButton.setAttribute("aria-expanded", "false");
      monitor.setAttribute("aria-hidden", "true");
    }
    monitorLive.textContent = "Stopped";
    monitorLive.dataset.state = "stopped";

    const reader = monitorReader;
    const loop = monitorLoop;
    monitorReader = null;
    monitorLoop = null;
    await stopSerialReader(reader, loop);
    await closePort();
  };

  const startMonitor = async () => {
    if (!selectedPort || busy) return;
    root.dataset.monitor = "open";
    monitorButton.setAttribute("aria-expanded", "true");
    monitor.setAttribute("aria-hidden", "false");
    monitorLive.textContent = "Connecting";
    monitorLive.dataset.state = "connecting";

    try {
      await selectedPort.open({ baudRate: 115200, bufferSize: 8192 });
      if (!selectedPort.readable) throw new Error("The serial input stream is unavailable.");
      const reader = selectedPort.readable.getReader();
      const decoder = new TextDecoder();
      monitorReader = reader;

      monitorLoop = (async () => {
        try {
          while (monitorReader === reader) {
            const { value, done } = await reader.read();
            if (done) break;
            appendMonitor(decoder.decode(value, { stream: true }));
          }
          appendMonitor(decoder.decode(), { flush: true });
        } catch (error) {
          if (monitorReader === reader) appendStatusLine(`Serial monitor stopped: ${errorMessage(error)}`);
        } finally {
          try { reader.releaseLock(); } catch (_error) {}
          if (monitorReader === reader) {
            monitorReader = null;
            monitorLoop = null;
            monitorLive.textContent = "Stopped";
            monitorLive.dataset.state = "stopped";
          }
        }
      })();

      appendStatusLine("Resetting the ESP into normal firmware mode");
      try {
        await resetToUserFirmware(selectedPort);
      } catch (error) {
        appendStatusLine(`Automatic reset unavailable: ${errorMessage(error)} Press RESET once to see boot output.`);
      }

      monitorLive.textContent = "Live";
      monitorLive.dataset.state = "live";
      appendStatusLine("Serial monitor started at 115200 baud");
    } catch (error) {
      monitorLive.textContent = "Error";
      monitorLive.dataset.state = "error";
      appendStatusLine(`Could not open serial monitor: ${errorMessage(error)}`);
      await closePort();
    }
  };

  const setDisconnected = () => {
    root.dataset.connected = "false";
    root.dataset.flashing = "false";
    root.dataset.finished = "false";
    connectionLabel.textContent = "Not connected";
    connectButton.disabled = !manifest || !serialSupported;
    disconnectButton.disabled = true;
    resetButton.disabled = true;
    installButton.disabled = true;
    monitorButton.disabled = true;
    deviceValue.textContent = "—";
    connectionValue.textContent = "Serial monitor";
    compatibilityValue.textContent = "—";
    setProgress("Preparing", 0);
    markSteps(1);
  };

  const disconnect = async () => {
    busy = false;
    await stopMonitor();
    await closePort();
    selectedPort = null;
    setDisconnected();
    setPageStatus("", "info");
  };

  // Permission removal identifies its target before asking the controller to disconnect. Close
  // only the installer-owned instance of that exact port and never interrupt an operation that
  // has deliberately disabled the regular Disconnect button.
  const disconnectPortForRelease = async (port) => {
    if (!selectedPort || port !== selectedPort) return false;
    if (busy || root.dataset.flashing === "true") {
      const error = new Error("The serial port is busy.");
      error.name = "InvalidStateError";
      throw error;
    }
    await disconnect();
    return true;
  };

  const connect = async () => {
    if (!serial || typeof serial.requestPort !== "function" || !manifest || busy) return;
    busy = true;
    connectButton.disabled = true;
    setPageStatus("Select your ESP board in the browser dialog.");

    try {
      const port = await serial.requestPort();
      selectedPort = port;
      connectionLabel.textContent = "Checking device…";
      setPageStatus("Checking chip and firmware compatibility…");
      appendStatusLine("USB port selected; checking device");
      const result = await probeDevice({
        port,
        manifest,
        TransportCtor,
        ESPLoaderCtor,
        onLog: appendMonitor
      });
      const info = port.getInfo ? port.getInfo() : {};
      deviceValue.textContent = result.chipFamily;
      connectionValue.textContent = describeSerialConnection(info);
      compatibilityValue.textContent = "Suitable";
      root.dataset.connected = "true";
      connectionLabel.textContent = `${result.chipFamily} connected`;
      disconnectButton.disabled = false;
      resetButton.disabled = false;
      installButton.disabled = !manifest;
      monitorButton.disabled = false;
      markSteps(2);
      setPageStatus(
        manifest.new_install_prompt_erase
          ? "Device ready. Erase is selected for a first install; choose Keep only for an existing dual-OTA installation."
          : "Device ready. Choose how the firmware should be installed.",
        "success"
      );
      appendStatusLine(`${result.chipFamily} detected and compatible`);
    } catch (error) {
      await closePort();
      selectedPort = null;
      setDisconnected();
      if (error && error.name === "NotFoundError") {
        setPageStatus("No USB device was selected.");
      } else {
        setPageStatus(`Connection failed: ${errorMessage(error)}`, "error");
        appendStatusLine(`Connection failed: ${errorMessage(error)}`);
      }
    } finally {
      busy = false;
      if (selectedPort) connectButton.disabled = true;
    }
  };

  const install = async () => {
    if (!selectedPort || !manifest || busy) return;
    busy = true;
    await stopMonitor();
    root.dataset.flashing = "true";
    root.dataset.finished = "false";
    installButton.disabled = true;
    disconnectButton.disabled = true;
    resetButton.disabled = true;
    monitorButton.disabled = true;
    markSteps(3);
    setProgress("Checking device", 0);
    setPageStatus("Keep this page open and leave the USB cable connected.");
    appendStatusLine(`Firmware installation ${manifest.version || ""} started`);

    try {
      const eraseFirst = Boolean(root.querySelector('input[name="install-mode"]:checked')?.value === "erase");
      await flashDevice({
        port: selectedPort,
        manifest,
        manifestUrl: new URL(manifestPath, location.href).toString(),
        eraseFirst,
        TransportCtor,
        ESPLoaderCtor,
        fetchImpl,
        onLog: appendMonitor,
        onState(state) {
          setProgress(state.message, state.percentage);
        }
      });
      setProgress("Verifying firmware boot", 100);
      setPageStatus(`Firmware written. Waiting for version ${manifest.version} to boot…`);
      await verifyBootImpl({
        port: selectedPort,
        expectedVersion: manifest.version,
        onLog: appendMonitor
      });
      root.dataset.flashing = "false";
      root.dataset.finished = "true";
      connectionLabel.textContent = "Restart complete";
      setProgress("Installation complete", 100);
      setPageStatus(`Firmware ${manifest.version} booted and was verified successfully.`, "success");
      appendStatusLine(`Firmware ${manifest.version} booted and passed version verification`);
      markSteps(4);
    } catch (error) {
      root.dataset.flashing = "false";
      root.dataset.finished = "false";
      setPageStatus(`Installation failed: ${errorMessage(error)}`, "error");
      appendStatusLine(`Installation failed: ${errorMessage(error)}`);
      markSteps(2);
    } finally {
      busy = false;
      const connected = Boolean(selectedPort && root.dataset.connected === "true");
      installButton.disabled = !connected;
      disconnectButton.disabled = !connected;
      resetButton.disabled = !connected;
      monitorButton.disabled = !connected;
    }
  };

  const resetDevice = async () => {
    if (!selectedPort || busy) return;
    busy = true;
    installButton.disabled = true;
    disconnectButton.disabled = true;
    resetButton.disabled = true;
    monitorButton.disabled = true;
    appendStatusLine("Manual device reset requested");
    setPageStatus("Resetting the ESP…");

    try {
      await resetConnectedDevice(selectedPort, { keepOpen: Boolean(monitorReader) });
      setPageStatus("ESP reset. The firmware is starting now.", "success");
      appendStatusLine("ESP reset into normal firmware mode");
    } catch (error) {
      setPageStatus(`Reset failed: ${errorMessage(error)}`, "error");
      appendStatusLine(`Reset failed: ${errorMessage(error)}`);
    } finally {
      busy = false;
      const connected = Boolean(selectedPort && root.dataset.connected === "true");
      installButton.disabled = !connected;
      disconnectButton.disabled = !connected;
      resetButton.disabled = !connected;
      monitorButton.disabled = !connected;
    }
  };

  connectButton.addEventListener("click", connect);
  disconnectButton.addEventListener("click", disconnect);
  resetButton.addEventListener("click", resetDevice);
  installButton.addEventListener("click", install);
  monitorButton.addEventListener("click", async () => {
    if (root.dataset.monitor === "open") await stopMonitor();
    else await startMonitor();
  });

  if (serial && typeof serial.addEventListener === "function") {
    serial.addEventListener("disconnect", async (event) => {
      if (selectedPort && (!event.target || event.target === selectedPort)) {
        appendStatusLine("USB device disconnected");
        await disconnect();
      }
    });
  }

  root.dataset.monitor = "closed";
  setDisconnected();

  if (!serialSupported) {
    connectButton.disabled = true;
    unsupported.hidden = false;
    setPageStatus("Web Serial needs Chrome or Edge on a secure desktop page.", "error");
  }

  const manifestUrl = new URL(manifestPath, location.href).toString();
  fetchImpl(manifestUrl, { cache: "no-store" })
    .then((response) => {
      if (!response.ok) throw new Error(`Firmware manifest returned HTTP ${response.status}.`);
      return response.json();
    })
    .then((loadedManifest) => {
      manifest = validateManifest(loadedManifest, manifestUrl);
      if (typeof manifest.version === "string" && manifest.version) {
        versionValue.textContent = manifest.version;
        versionLine.hidden = false;
      }
      const shortSha = manifest.sourceSha.slice(0, 12);
      if (sourceLink && sourceValue) {
        sourceValue.textContent = shortSha;
        sourceLink.href = `https://github.com/0Bu/tesla-key-esp32/commit/${manifest.sourceSha}`;
        sourceLink.title = `Source commit ${manifest.sourceSha}`;
        sourceLink.hidden = false;
      }
      if (previewLink && location.pathname.match(/\/PR\/\d+\//)) {
        previewLink.href = `https://github.com/0Bu/tesla-key-esp32/commit/${manifest.sourceSha}`;
        previewLink.textContent = `View source commit ${shortSha}`;
      }
      if (manifest.new_install_prompt_erase) {
        const eraseOption = root.querySelector('input[name="install-mode"][value="erase"]');
        const keepBadge = element("keep-recommended");
        const eraseBadge = element("erase-recommended");
        if (eraseOption) eraseOption.checked = true;
        if (keepBadge) keepBadge.hidden = true;
        if (eraseBadge) eraseBadge.hidden = false;
      }
      if (selectedPort && !busy) installButton.disabled = false;
      if (!selectedPort && !busy && serialSupported) connectButton.disabled = false;
    })
    .catch((error) => {
      setPageStatus(`Firmware metadata could not be loaded: ${errorMessage(error)}`, "error");
    });

  return {
    connect,
    disconnect,
    disconnectPortForRelease,
    install,
    resetDevice,
    startMonitor,
    stopMonitor
  };
}
