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

async function withTimeout(operation, milliseconds, timeoutError, onTimeout, signal) {
  let timeoutId;
  let abortHandler;
  const pending = [Promise.resolve(operation), new Promise((_, reject) => {
    timeoutId = setTimeout(() => {
      try {
        if (typeof onTimeout === "function") onTimeout();
      } catch (_error) {
        // The deadline still rejects the operation if its cancellation hook itself fails.
      } finally {
        reject(timeoutError);
      }
    }, milliseconds);
  })];
  if (signal) {
    pending.push(new Promise((_, reject) => {
      abortHandler = () => reject(signal.reason instanceof Error
        ? signal.reason
        : errorWithName("AbortError", "The operation was aborted."));
      if (signal.aborted) abortHandler();
      else signal.addEventListener("abort", abortHandler, { once: true });
    }));
  }
  try {
    return await Promise.race(pending);
  } finally {
    clearTimeout(timeoutId);
    if (signal && abortHandler) signal.removeEventListener("abort", abortHandler);
  }
}

function throwIfAborted(signal, fallback = "The operation was aborted.") {
  if (!signal || !signal.aborted) return;
  throw signal.reason instanceof Error ? signal.reason : errorWithName("AbortError", fallback);
}

export async function stopSerialReader(reader, loop) {
  if (reader && typeof reader.cancel === "function") {
    try {
      await reader.cancel();
    } catch (_error) {}
  }
  if (loop) {
    try {
      await loop;
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

async function settleTransport(transport, restart, timeoutMs = TRANSPORT_CLEANUP_TIMEOUT_MS) {
  if (restart) {
    let restartPromise;
    try {
      restartPromise = Promise.resolve().then(restart);
      await withTimeout(restartPromise, timeoutMs, new Error("Device reset timed out."));
    } catch (_error) {
      // A timeout bounds the status report, not ownership of an in-flight serial signal. Wait for
      // it to settle before disconnecting so a late reset cannot affect a subsequent session.
      if (restartPromise) await restartPromise.catch(() => {});
    }
  }
  if (transport && typeof transport.disconnect === "function") {
    let disconnectPromise;
    try {
      disconnectPromise = Promise.resolve().then(() => transport.disconnect());
      await withTimeout(disconnectPromise, timeoutMs, new Error("Serial cleanup timed out."));
    } catch (_error) {
      // The device can disappear during its reset. In that case the browser already closed it.
      // A late disconnect still owns this serial transport; do not release it to a retry yet.
      if (disconnectPromise) await disconnectPromise.catch(() => {});
    }
  }
}

function makeAbortableTransport(transport, signal) {
  const originalWrite = typeof transport.write === "function"
    ? transport.write.bind(transport)
    : async () => { throw new Error("The serial transport cannot write bytes."); };
  const originalRead = typeof transport.read === "function" ? transport.read.bind(transport) : null;
  let stopped = false;
  let activeWriter = null;
  let activeWrite = null;
  let writerAbort = null;
  let readerCancel = null;

  const abort = () => {
    if (stopped) return;
    stopped = true;
    const reason = signal.reason instanceof Error
      ? signal.reason
      : errorWithName("AbortError", "The serial operation was aborted.");
    if (activeWriter && typeof activeWriter.abort === "function") {
      const writer = activeWriter;
      writerAbort = Promise.resolve().then(() => writer.abort(reason)).catch(() => {});
    }
    const reader = transport.reader;
    if (reader && typeof reader.cancel === "function") {
      readerCancel = Promise.resolve().then(() => reader.cancel(reason)).catch(() => {});
    }
  };

  transport.write = async (data) => {
    throwIfAborted(signal, "Firmware installation stopped.");
    if (stopped) throw errorWithName("AbortError", "Further serial writes were stopped.");

    const writable = transport.device && transport.device.writable;
    const writer = writable && typeof writable.getWriter === "function" &&
      typeof transport.slipWriter === "function"
      ? writable.getWriter()
      : null;
    if (!writer) {
      const pending = Promise.resolve().then(() => {
        throwIfAborted(signal, "Firmware installation stopped.");
        return originalWrite(data);
      });
      activeWrite = pending;
      try {
        await pending;
        throwIfAborted(signal, "Firmware installation stopped.");
      } finally {
        if (activeWrite === pending) activeWrite = null;
      }
      return;
    }

    activeWriter = writer;
    const pending = Promise.resolve().then(() => {
      throwIfAborted(signal, "Firmware installation stopped.");
      const framed = transport.slipWriter(data);
      if (transport.tracing) transport.trace(`Write ${framed.length} bytes: ${transport.hexConvert(framed)}`);
      return writer.write(framed);
    });
    activeWrite = pending;
    try {
      await pending;
      throwIfAborted(signal, "Firmware installation stopped.");
    } finally {
      if (activeWriter === writer) activeWriter = null;
      if (activeWrite === pending) activeWrite = null;
      try { writer.releaseLock(); } catch (_error) {}
    }
  };

  if (originalRead) {
    transport.read = async (...args) => {
      throwIfAborted(signal, "Firmware installation stopped.");
      if (stopped) throw errorWithName("AbortError", "Further serial reads were stopped.");
      const result = await originalRead(...args);
      throwIfAborted(signal, "Firmware installation stopped.");
      return result;
    };
  }

  signal.addEventListener("abort", abort, { once: true });
  if (signal.aborted) abort();
  return {
    async settleAbort() {
      abort();
      await Promise.allSettled([activeWrite, writerAbort, readerCancel].filter(Boolean));
    },
    dispose() { signal.removeEventListener("abort", abort); }
  };
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

export function installFailureStatus(error, { written = false, version = "" } = {}) {
  const reason = errorMessage(error);
  if (written) {
    return {
      kind: "warning",
      step: 4,
      message: `Firmware ${version} installed, but its start was not verified: ${reason} ` +
        "Restart the board with its RESET button or by reconnecting the USB cable.",
      statusLine: `Firmware ${version} installed; boot verification failed: ${reason}`
    };
  }
  return {
    kind: "error",
    step: 2,
    message: `Installation failed: ${reason}`,
    statusLine: `Installation failed: ${reason}`
  };
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

export async function fetchFirmwareParts(build, manifestUrl, fetchImpl = fetch, cryptoImpl = globalThis.crypto, signal) {
  validateBuild(build, manifestUrl);

  const baseDirectory = new URL("./", manifestUrl);
  return Promise.all(build.parts.map(async (part, index) => {
    const url = new URL(part.path, baseDirectory).toString();
    const requestOptions = { cache: "no-store" };
    if (signal) requestOptions.signal = signal;
    const response = await fetchImpl(url, requestOptions);
    throwIfAborted(signal, "Firmware download stopped.");
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
  delay = sleep,
  signal
}) {
  if (!port) throw errorWithName("BootVerificationError", "The serial port disappeared before boot verification.");
  if (typeof expectedVersion !== "string" || !expectedVersion) {
    throw errorWithName("BootVerificationError", "The expected firmware version is missing.");
  }

  const deadline = Date.now() + timeoutMs;
  let lastOpenError = null;
  let pendingOpen = null;
  let reader = null;
  let readLoop = null;
  let captured = "";
  try {
    while (!port.readable && Date.now() < deadline) {
      throwIfAborted(signal, "Boot verification stopped.");
      let openSettled = false;
      const opening = Promise.resolve().then(() => port.open({ baudRate: 115200, bufferSize: 8192 }));
      pendingOpen = opening.finally(() => { openSettled = true; });
      try {
        await withTimeout(
          pendingOpen,
          Math.min(2000, Math.max(1, deadline - Date.now())),
          new Error("Opening the serial port timed out."),
          undefined,
          signal
        );
        pendingOpen = null;
      } catch (error) {
        lastOpenError = error;
        if (signal && signal.aborted) throw error;
        if (!openSettled) {
          throw errorWithName(
            "BootVerificationError",
            `The serial port did not finish opening for version verification: ${errorMessage(error)}`
          );
        }
        await delay(Math.min(250, Math.max(0, deadline - Date.now())));
        pendingOpen = null;
      }
    }
    if (!port.readable) {
      throw errorWithName(
        "BootVerificationError",
        `The flashed device did not reconnect for version verification${lastOpenError ? `: ${errorMessage(lastOpenError)}` : "."}`
      );
    }
    throwIfAborted(signal, "Boot verification stopped.");

    reader = port.readable.getReader();
    const decoder = new TextDecoder();
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
      errorWithName("BootVerificationError", `Timed out waiting for firmware ${expectedVersion} to boot.`),
      undefined,
      signal
    );
    if (!verified) {
      throw errorWithName("BootVerificationError", `The serial stream ended before firmware ${expectedVersion} was observed.`);
    }
    return { version: expectedVersion, log: captured };
  } finally {
    // port.open() cannot be cancelled by AbortSignal. Retain ownership until its pending attempt
    // settles so a late success is closed before the installer can release or reuse this port.
    if (pendingOpen) {
      try { await pendingOpen; } catch (_error) {}
      pendingOpen = null;
    }
    if (reader) {
      await stopSerialReader(reader, readLoop);
      try { reader.releaseLock(); } catch (_error) {}
    }
    if (port.readable || port.writable) {
      try {
        await port.close();
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
  cleanupTimeoutMs = TRANSPORT_CLEANUP_TIMEOUT_MS,
  delay = sleep,
  signal: callerSignal
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
  let activeOperation = null;
  const timeoutError = errorWithName(
    "DeviceProbeTimeoutError",
    "The serial device did not answer in flashing mode. Select the ESP chip's native USB port when available; otherwise hold BOOT, tap RESET, then try again."
  );
  const controller = new AbortController();
  const abort = (reason) => {
    if (!controller.signal.aborted) controller.abort(reason);
  };
  const abortFromCaller = () => abort(callerSignal?.reason instanceof Error
    ? callerSignal.reason
    : errorWithName("DeviceProbeAbortedError", "The USB connection ended during device checking."));
  if (callerSignal) {
    if (callerSignal.aborted) abortFromCaller();
    else callerSignal.addEventListener("abort", abortFromCaller, { once: true });
  }
  const abortableTransport = makeAbortableTransport(transport, controller.signal);

  try {
    activeOperation = (async () => {
      throwIfAborted(controller.signal, "Device checking stopped.");
      await loader.main();
      throwIfAborted(controller.signal, "Device checking stopped.");
      loaderReady = true;
      if (typeof loader.flashId === "function") {
        await loader.flashId();
        throwIfAborted(controller.signal, "Device checking stopped.");
      }
      const chipFamily = loader.chip && loader.chip.CHIP_NAME;
      if (!chipFamily) throw errorWithName("ChipDetectionError", "The connected ESP chip could not be identified.");
      const build = selectManifestBuild(manifest, chipFamily, port.getInfo());
      if (!build) {
        throw errorWithName("UnsupportedChipError", `${chipFamily} is not supported by this firmware.`);
      }
      await assertChipRevisionSupported(loader, chipFamily);
      throwIfAborted(controller.signal, "Device checking stopped.");
      return { chipFamily, build };
    })();
    return await withTimeout(
      activeOperation,
      timeoutMs,
      timeoutError,
      () => abort(timeoutError),
      controller.signal
    );
  } finally {
    // The compatibility probe runs the flasher stub. Explicitly hand control back to the installed
    // application before releasing the serial port. On cancellation, abort active Web Serial IO,
    // disconnect to cancel any remaining read, wait for the loader, then disconnect again in case
    // a late port.open completed after the first close attempt.
    if (controller.signal.aborted) {
      await abortableTransport.settleAbort();
      await settleTransport(transport, undefined, cleanupTimeoutMs);
      if (activeOperation) {
        try { await activeOperation; } catch (_error) {}
      }
      await settleTransport(transport, undefined, cleanupTimeoutMs);
    } else {
      await settleTransport(
        transport,
        loaderReady ? () => resetToUserFirmware(port, delay) : undefined,
        cleanupTimeoutMs
      );
    }
    abortableTransport.dispose();
    if (callerSignal) callerSignal.removeEventListener("abort", abortFromCaller);
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
  cleanupTimeoutMs = TRANSPORT_CLEANUP_TIMEOUT_MS,
  delay = sleep,
  signal: callerSignal
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
  let activeOperation = null;
  let restartPromise = null;
  let completedResult = null;
  const timeoutError = errorWithName("FlashTimeoutError", "Firmware installation timed out before it completed.");
  const controller = new AbortController();
  const stop = (error) => {
    if (!controller.signal.aborted) controller.abort(error);
  };
  const abortFromCaller = () => {
    stop(callerSignal?.reason instanceof Error
      ? callerSignal.reason
      : errorWithName("FlashAbortedError", "The USB connection ended during firmware installation."));
  };
  if (callerSignal) {
    if (callerSignal.aborted) abortFromCaller();
    else callerSignal.addEventListener("abort", abortFromCaller, { once: true });
  }
  const flashTransport = makeAbortableTransport(transport, controller.signal);
  controller.signal.addEventListener("abort", () => {
    onState({ stage: "stopping", percentage: 0, message: "Stopping active USB operation" });
  }, { once: true });
  const runDeviceOperation = async (operation, onSuccess) => {
    throwIfAborted(controller.signal, "Firmware installation stopped.");
    const pendingOperation = Promise.resolve().then(operation);
    activeOperation = pendingOperation;
    try {
      await pendingOperation;
    } finally {
      if (activeOperation === pendingOperation) activeOperation = null;
    }
    if (onSuccess) onSuccess();
    if (!completed) throwIfAborted(controller.signal, "Firmware installation stopped.");
  };

  try {
    let written;
    try {
      written = await withTimeout((async () => {
    throwIfAborted(controller.signal, "Firmware installation stopped.");
    validateManifest(manifest, manifestUrl);
    onState({ stage: "connecting", percentage: 0, message: "Checking device" });
    await runDeviceOperation(() => loader.main());
    throwIfAborted(controller.signal, "Firmware installation stopped.");
    if (typeof loader.flashId === "function") await runDeviceOperation(() => loader.flashId());
    throwIfAborted(controller.signal, "Firmware installation stopped.");

    const chipFamily = loader.chip && loader.chip.CHIP_NAME;
    const build = selectManifestBuild(manifest, chipFamily, port.getInfo());
    if (!build) {
      throw errorWithName(
        "UnsupportedChipError",
        chipFamily ? `${chipFamily} is not supported by this firmware.` : "The ESP chip could not be identified."
      );
    }
    await runDeviceOperation(() => assertChipRevisionSupported(loader, chipFamily));
    throwIfAborted(controller.signal, "Firmware installation stopped.");

    onState({ stage: "preparing", percentage: 0, message: "Loading firmware" });
    const fileArray = await fetchFirmwareParts(build, manifestUrl, fetchImpl, cryptoImpl, controller.signal);
    throwIfAborted(controller.signal, "Firmware download stopped.");

    if (eraseFirst) {
      throwIfAborted(controller.signal, "Firmware installation stopped.");
      onState({ stage: "erasing", percentage: 0, message: "Erasing flash" });
      await runDeviceOperation(() => loader.eraseFlash());
    }

    const writePhase = async (phaseFiles, firstGlobalIndex, stage, message) => {
      for (let phaseIndex = 0; phaseIndex < phaseFiles.length; phaseIndex += 1) {
        throwIfAborted(controller.signal, "Firmware installation stopped.");
        const globalIndex = firstGlobalIndex + phaseIndex;
        const phaseFile = phaseFiles[phaseIndex];
        onState({
          stage,
          percentage: combinedProgress(fileArray, globalIndex, 0, phaseFile.data.length),
          message
        });
        // esptool-js 0.6.1 loops over every entry in one writeFlash call. A late abort can arrive
        // after flashDeflFinish but before its next internal flashDeflBegin, so pass one part at a
        // time and regain control between commands before starting another address range.
        await runDeviceOperation(() => loader.writeFlash({
          fileArray: [phaseFile],
          flashSize: "keep",
          flashMode: "keep",
          flashFreq: "keep",
          eraseAll: false,
          compress: true,
          reportProgress(fileIndex, written, total) {
            throwIfAborted(controller.signal, "Firmware write stopped.");
            onState({
              stage,
              percentage: combinedProgress(fileArray, globalIndex + fileIndex, written, total),
              message
            });
          }
        }), stage === "activating" && globalIndex === fileArray.length - 1 ? () => {
          // Once the last otadata command resolves, the new app is selected. Record that boundary
          // before checking a just-arrived abort so a timeout cannot misreport a complete write.
          completed = true;
          completedResult = { chipFamily, build };
        } : undefined);
      }
    };

    // Keep-mode may start on ota_1. Write bootloader/table/new ota_0 first; only after all of those
    // succeeded write ota_data_initial in its own final phase, atomically selecting the new ota_0.
    // A failure before this phase leaves the previously active image selected and bootable.
    await writePhase(fileArray.slice(0, 3), 0, "writing", "Writing firmware");
    throwIfAborted(controller.signal, "Firmware installation stopped.");
    await writePhase(fileArray.slice(3), 3, "activating", "Activating new firmware");

    if (!completed) {
      throwIfAborted(controller.signal, "Firmware installation stopped.");
      completed = true;
      completedResult = { chipFamily, build };
    }
    return completedResult;
      })(), timeoutMs, timeoutError, () => stop(timeoutError), controller.signal);
    } catch (error) {
      if (activeOperation) {
        try {
          // Let the in-flight loader command cross its commit boundary before deciding whether
          // cancellation happened before or after the final otadata write.
          await activeOperation;
        } catch (_operationError) {
          // The original timeout or cancellation remains the reported failure unless activation
          // completed successfully while the command was settling.
        }
      }
      if (!completed) throw error;
      // A deadline can race the final loader promise's resolution. Once otadata has committed,
      // finish the reset and report the installed image instead of a failed flash.
      written = completedResult;
    }

    // The flash phases are complete. Detach cancellation and the install deadline before the EN
    // pulse; neither may turn a successful write into a timeout or release the port mid-reset.
    if (callerSignal) callerSignal.removeEventListener("abort", abortFromCaller);
    onState({ stage: "restarting", percentage: 100, message: "Starting firmware" });
    let restarted = true;
    try {
      restartPromise = Promise.resolve().then(() => resetToUserFirmware(port, delay));
      await withTimeout(restartPromise, cleanupTimeoutMs, new Error("Device reset timed out."));
    } catch (error) {
      restarted = false;
      onLog(`Automatic restart unavailable: ${errorMessage(error)}\n`);
      // Keep the serial port owned until the reset sequence settles. Promise.race timeouts do not
      // cancel Web Serial setSignals() and must not let disconnect or a new session overtake it.
      if (restartPromise) await restartPromise.catch(() => {});
    }
    return { ...written, restarted };
  } finally {
    if (callerSignal) callerSignal.removeEventListener("abort", abortFromCaller);
    if (restartPromise) await restartPromise.catch(() => {});
    if (controller.signal.aborted && activeOperation) {
      try {
        // The transport aborts its active Web Serial writer and reader. Keep this operation and
        // port quarantined until its continuation has stopped; a later loader step must not race
        // reset, disconnect, or a retry.
        await activeOperation;
      } catch (_error) {
        // The failed command has settled; the aborted signal prevents any following phase.
      }
    }
    if (controller.signal.aborted) await flashTransport.settleAbort();
    await settleTransport(
      transport,
      !completed && loader.chip ? () => resetToUserFirmware(port, delay) : undefined,
      cleanupTimeoutMs
    );
    flashTransport.dispose();
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
  let activeInstallController = null;
  let activeConnectController = null;
  let monitorStartPromise = null;
  let monitorStopPromise = null;
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
      await selectedPort.close();
    } catch (_error) {
      // A USB reset or unplug can close the port before the page reaches cleanup.
    }
  };

  const stopMonitor = async ({ collapse = true } = {}) => {
    if (monitorStopPromise) return monitorStopPromise;
    if (collapse) {
      root.dataset.monitor = "closed";
      monitorButton.setAttribute("aria-expanded", "false");
      monitor.setAttribute("aria-hidden", "true");
    }
    monitorStopPromise = (async () => {
      // A start can still be waiting for port.open(). Let it finish before taking the reader,
      // otherwise cleanup can miss a late-opened stream and release the port while it is active.
      if (monitorStartPromise) {
        try { await monitorStartPromise; } catch (_error) {}
      }
      monitorLive.textContent = "Stopped";
      monitorLive.dataset.state = "stopped";

      const reader = monitorReader;
      const loop = monitorLoop;
      monitorReader = null;
      monitorLoop = null;
      await stopSerialReader(reader, loop);
      await closePort();
    })();
    try {
      await monitorStopPromise;
    } finally {
      monitorStopPromise = null;
    }
  };

  const startMonitor = async () => {
    if (!selectedPort || busy || monitorStartPromise || monitorStopPromise) return;
    const starting = (async () => {
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
    })();
    monitorStartPromise = starting;
    try {
      await starting;
    } finally {
      if (monitorStartPromise === starting) monitorStartPromise = null;
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
    if (busy) {
      if (activeInstallController) {
        activeInstallController.abort(errorWithName(
          "FlashAbortedError",
          "The USB connection ended during firmware installation."
        ));
      } else if (activeConnectController) {
        activeConnectController.abort(errorWithName(
          "DeviceProbeAbortedError",
          "The USB connection ended during device checking."
        ));
      }
      return;
    }
    busy = true;
    connectButton.disabled = true;
    disconnectButton.disabled = true;
    resetButton.disabled = true;
    installButton.disabled = true;
    monitorButton.disabled = true;
    try {
      await stopMonitor();
      await closePort();
      selectedPort = null;
      setPageStatus("", "info");
    } finally {
      busy = false;
      setDisconnected();
    }
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
    if (!serial || typeof serial.requestPort !== "function" || !manifest || busy || monitorStartPromise || monitorStopPromise) return;
    busy = true;
    const connectController = new AbortController();
    activeConnectController = connectController;
    connectButton.disabled = true;
    setPageStatus("Select your ESP board in the browser dialog.");

    try {
      const port = await serial.requestPort();
      throwIfAborted(connectController.signal, "Device checking stopped.");
      selectedPort = port;
      connectionLabel.textContent = "Checking device…";
      setPageStatus("Checking chip and firmware compatibility…");
      appendStatusLine("USB port selected; checking device");
      const result = await probeDevice({
        port,
        manifest,
        TransportCtor,
        ESPLoaderCtor,
        onLog: appendMonitor,
        signal: connectController.signal
      });
      throwIfAborted(connectController.signal, "Device checking stopped.");
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
      if (activeConnectController === connectController) activeConnectController = null;
      busy = false;
      if (selectedPort) connectButton.disabled = true;
    }
  };

  const install = async () => {
    if (!selectedPort || !manifest || busy || monitorStartPromise || monitorStopPromise) return;
    busy = true;
    const installController = new AbortController();
    activeInstallController = installController;
    installController.signal.addEventListener("abort", () => {
      setPageStatus("Stopping the active USB operation. Keep the board connected until it stops.", "info");
    }, { once: true });
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

    let written = false;
    try {
      const eraseFirst = Boolean(root.querySelector('input[name="install-mode"]:checked')?.value === "erase");
      const flashed = await flashDevice({
        port: selectedPort,
        manifest,
        manifestUrl: new URL(manifestPath, location.href).toString(),
        eraseFirst,
        TransportCtor,
        ESPLoaderCtor,
        fetchImpl,
        signal: installController.signal,
        onLog: appendMonitor,
        onState(state) {
          if (state.stage === "restarting") {
            written = true;
            setPageStatus("Firmware written. Waiting for the board to restart…", "warning");
          }
          setProgress(state.message, state.percentage);
          if (state.stage === "stopping") {
            setPageStatus("Stopping the active USB operation. Keep the board connected until it stops.", "info");
          }
        }
      });
      written = true;
      if (flashed.restarted === false) {
        appendStatusLine("Automatic restart unavailable. Press RESET on the board if the boot check does not finish.");
      }
      setProgress("Verifying firmware boot", 100);
      setPageStatus(`Firmware written. Waiting for version ${manifest.version} to boot…`);
      await verifyBootImpl({
        port: selectedPort,
        expectedVersion: manifest.version,
        onLog: appendMonitor,
        signal: installController.signal
      });
      throwIfAborted(installController.signal, "Boot verification stopped.");
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
      if (!written && error && (error.name === "FlashTimeoutError" || error.name === "FlashAbortedError" || error.name === "AbortError")) {
        const message = error.name === "FlashTimeoutError"
          ? "Installation timed out. Further flash phases were stopped. Reconnect and inspect the board before retrying."
          : "USB disconnected during installation. Further flash phases were stopped. Reconnect and inspect the board before retrying.";
        await closePort();
        selectedPort = null;
        setDisconnected();
        setPageStatus(message, "error");
        appendStatusLine(message);
      } else {
        const status = installFailureStatus(error, { written, version: manifest.version });
        setPageStatus(status.message, status.kind);
        appendStatusLine(status.statusLine);
        markSteps(status.step);
      }
    } finally {
      activeInstallController = null;
      busy = false;
      const connected = Boolean(selectedPort && root.dataset.connected === "true");
      installButton.disabled = !connected;
      disconnectButton.disabled = !connected;
      resetButton.disabled = !connected;
      monitorButton.disabled = !connected;
    }
  };

  const resetDevice = async () => {
    if (!selectedPort || busy || monitorStartPromise || monitorStopPromise) return;
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
