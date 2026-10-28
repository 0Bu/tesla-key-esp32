// Web Serial installer for the signed Pages feed: a verified schema-v2 manifest, serial-port
// ownership, a read-only check of the installed partition table and app before flashing, Flash
// (keeps NVS) and Clean flash side by side with only Clean flash left when the installed layout
// cannot keep the configuration, and a log that follows the firmware after connecting and installing.
//
// One connection opens the port once and closes it once. Repeated open/close cycles have locked an
// ESP32-S3's USB-Serial/JTAG link on macOS until a power cycle while the firmware kept running, so
// the check hands its open port to the live log, Reset only pulses the reset line, and an install
// borrows the port from the log and hands it back.
import {
  acquireSerialPortLease, forgetSerialPort, releaseFeedback, releaseSerialPortLease, serialPortIsOpen,
  serialPortLease, subscribeSerialOwnership, supportsSerialForget, updateSerialPortLease
} from "./serial-port-release.mjs";

const PROJECT_NAME = "tesla-key-esp32";
const SOURCE_COMMIT_URL = "https://github.com/0Bu/tesla-key-esp32/commit/";
const ESPRESSIF_USB_VENDOR_ID = 0x303a;
const CDC_PRODUCT_IDS = new Set([0x0002, 0x0003, 0x1001, 0x1002, 0x1003]);
const MAX_MONITOR_CHARS = 100000;
// Within this many pixels of its end the log counts as scrolled to the end and follows new lines.
const LOG_END_SLACK_PX = 8;
const DEVICE_PROBE_TIMEOUT_MS = 10000;
const TRANSPORT_CLEANUP_TIMEOUT_MS = 2000;
const FLASH_OPERATION_TIMEOUT_MS = 180000;
// The new firmware prints its version early in its boot log; a missing line is a warning, not a
// failed installation, because the image is already on flash.
const BOOT_VERIFICATION_TIMEOUT_MS = 20000;
// esptool-js opens the port with Web Serial's default 255-byte buffer unless told otherwise. Chrome
// then drains USB only as fast as the page reads, and the flasher stub on USB-Serial/JTAG drops data
// it cannot send in time. A large buffer makes such losses rare; the installed-firmware check also
// reads in small steps.
const ESPTOOL_SERIAL_OPTIONS = Object.freeze({ bufferSize: 0x10000 });
// Each read of the installed-firmware check has its own bound, so a slow or stuck read can neither
// eat the chip probe's budget nor hold the port; the check stops at the first read that fails.
const FLASH_READ_STEP_TIMEOUT_MS = 2500;
const INSPECTION_TIMEOUT_MS = 4 * FLASH_READ_STEP_TIMEOUT_MS;
// A cancelled probe keeps its port until esptool-js gives up connecting; unpairing waits that long.
const PORT_RELEASE_WAIT_MS = 15000;
// A port opens once per browser: right after a reload Chrome may still hold the previous page's
// handle, and another tab may hold it for good. A failed open never reaches the device, so the
// check of a paired port is retried.
const PAIRED_OPEN_ATTEMPTS = 3;
const PAIRED_OPEN_RETRY_MS = 500;
// A board that re-enumerates on its reset pulse reports a new USB connect. Checking it again would
// reset it again; a connect this soon after the installer's own port activity waits for a click.
// The installer cannot tell that from a quick manual replug, which waits for the click too.
const RECONNECT_GRACE_MS = 5000;
const ANSI_CONTROL_SEQUENCE = /\x1B\[[0-?]*[ -/]*[@-~]/g;
const SHA256_PATTERN = /^[0-9a-f]{64}$/;
const MAX_MANIFEST_BYTES = 65536;
const FIRMWARE_DOWNLOAD_TIMEOUT_MS = 60000;
const FLASH_SECTOR_BYTES = 0x1000;
const MANIFEST_LAYOUT_VERSION = 2;
const SUPPORTED_CHIPS = ["ESP32", "ESP32-S3", "ESP32-C3", "ESP32-C6"];
// Firmware built for these targets refuses to boot on an older silicon revision. Probing the
// revision up front turns a 100 % write that then boot-loops into an actionable message.
const MIN_CHIP_REVISION_MAJOR = { ESP32: 3 };
// partitions.csv fills exactly 4 MB, the smallest flash across the supported boards.
const MIN_FLASH_BYTES = 0x400000;
// Mirrors partitions.csv; test/web_installer.test.mjs checks these values against it. Ordinary OTA
// and Flash keep nvs (WiFi, VIN, MQTT, pairing key, sessions), phy_init and a pending core dump.
export const FLASH_LAYOUT = Object.freeze({
  partitionTable: Object.freeze({ offset: 0x8000, size: 0x1000 }),
  nvs: Object.freeze({ offset: 0x9000, size: 0x6000 }),
  otadata: Object.freeze({ offset: 0xf000, size: 0x2000 }),
  phyInit: Object.freeze({ offset: 0x11000, size: 0x1000 }),
  coredump: Object.freeze({ offset: 0x12000, size: 0xc000 }),
  ota0: Object.freeze({ offset: 0x20000, size: 0x1f0000 })
});
const PRESERVED_FLASH_RANGES = Object.freeze(["nvs", "phyInit", "coredump"].map((name) => Object.freeze({
  name, start: FLASH_LAYOUT[name].offset, end: FLASH_LAYOUT[name].offset + FLASH_LAYOUT[name].size
})));
const LOOPBACK_HOSTS = new Set(["localhost", "127.0.0.1", "[::1]"]);
const verifiedOffers = new WeakMap();
const sleep = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

function errorWithName(name, message) {
  const error = new Error(message);
  error.name = name;
  return error;
}

function errorMessage(error) {
  return error && typeof error.message === "string" ? error.message : String(error);
}

const isOpenFailure = (error) => error?.name === "NetworkError" && /open/i.test(errorMessage(error));
const PORT_BUSY = Symbol("port busy");

// esptool-js opens the port in Transport.connect() and closes it in disconnect(). This transport
// uses a port that is already open and, through release(), hands it on without closing it.
const sharedTransports = new WeakMap();
export function sharedPortTransport(Base) {
  if (!sharedTransports.has(Base)) {
    sharedTransports.set(Base, class extends Base {
      constructor(port, ...rest) {
        super(port, ...rest);
        this.sharedPort = port;
      }
      async connect(...args) {
        if (serialPortIsOpen(this.sharedPort)) this.baudrate = args[0] ?? 115200;
        else await super.connect(...args);
        this.onOpened?.();
      }
      // Ends esptool-js's read loop and waits until both streams are unlocked; the port stays open.
      async release() {
        const port = this.sharedPort;
        if (port?.readable?.locked) await this.reader?.cancel();
        const deadline = Date.now() + TRANSPORT_CLEANUP_TIMEOUT_MS;
        while (port?.readable?.locked || port?.writable?.locked) {
          if (Date.now() >= deadline) throw errorWithName("SerialCleanupError", "The flasher did not release the serial port.");
          await sleep(10);
        }
        this.reader = undefined;
      }
    });
  }
  return sharedTransports.get(Base);
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

// The firmware's version as its boot log states it: the ESP-IDF app line or the firmware's BOOT
// line (main/logic/bootlog.hpp). Log text is untrusted; only these fixed shapes are interpreted.
const BOOT_VERSION_LINE = /(?:^|\s)(?:App version:\s*|BOOT version=)([0-9A-Za-z.+-]{1,31})(?:\s|$)/;
export function bootVersion(line) {
  const match = BOOT_VERSION_LINE.exec(stripSerialAnsi(line));
  return match ? match[1] : "";
}

// The subtitle's channel: a signed PR preview or the dev feed by its Pages path, else the version's.
export function channelLabel(version, pathname = "") {
  const preview = /\/PR\/(\d+)\//.exec(String(pathname));
  if (preview) return `PR #${preview[1]} preview`;
  return /\/dev(?:\/|$)/.test(String(pathname)) || /-dev(?:[.-]|$)/.test(String(version)) ? "Dev" : "Release";
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

function throwIfAborted(signal, fallback) {
  if (!signal || !signal.aborted) return;
  throw signal.reason instanceof Error ? signal.reason : errorWithName("AbortError", fallback);
}

function terminalAdapter(onLog) {
  return {
    clean() {},
    write(data) { onLog(String(data)); },
    writeLine(data) { onLog(`${String(data)}\n`); }
  };
}

// Cancellation must stop the active Web Serial write or read, not only the next loader command:
// esptool-js keeps writing a block until its writer resolves, polls its read buffer until its own
// command timeout, and its connect loop keeps pulsing the reset lines through up to seven
// attempts. After an abort the transport rejects every further write, read and line change, so a
// late loader step can neither reach the device nor leave it in download mode.
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
    throwIfAborted(signal, "The serial operation was cancelled.");
    if (stopped) throw errorWithName("AbortError", "Further serial writes were stopped.");

    const writable = transport.device && transport.device.writable;
    const writer = writable && typeof writable.getWriter === "function" &&
      typeof transport.slipWriter === "function"
      ? writable.getWriter()
      : null;
    if (!writer) {
      const pending = Promise.resolve().then(() => {
        throwIfAborted(signal, "The serial operation was cancelled.");
        return originalWrite(data);
      });
      activeWrite = pending;
      try {
        await pending;
        throwIfAborted(signal, "The serial operation was cancelled.");
      } finally {
        if (activeWrite === pending) activeWrite = null;
      }
      return;
    }

    activeWriter = writer;
    const pending = Promise.resolve().then(() => {
      throwIfAborted(signal, "The serial operation was cancelled.");
      const framed = transport.slipWriter(data);
      if (transport.tracing) transport.trace(`Write ${framed.length} bytes: ${transport.hexConvert(framed)}`);
      return writer.write(framed);
    });
    activeWrite = pending;
    try {
      await pending;
      throwIfAborted(signal, "The serial operation was cancelled.");
    } finally {
      if (activeWriter === writer) activeWriter = null;
      if (activeWrite === pending) activeWrite = null;
      try { writer.releaseLock(); } catch (_error) {}
    }
  };

  const aborted = new Promise((_, reject) => {
    const rejectWithReason = () => reject(signal.reason instanceof Error
      ? signal.reason
      : errorWithName("AbortError", "The serial operation was cancelled."));
    if (signal.aborted) rejectWithReason();
    else signal.addEventListener("abort", rejectWithReason, { once: true });
  });
  aborted.catch(() => {});

  if (originalRead) {
    transport.read = async (...args) => {
      throwIfAborted(signal, "The serial operation was cancelled.");
      if (stopped) throw errorWithName("AbortError", "Further serial reads were stopped.");
      // esptool-js's read polls its own buffer until its timeout; the abort ends the wait at once.
      const result = await Promise.race([originalRead(...args), aborted]);
      throwIfAborted(signal, "The serial operation was cancelled.");
      return result;
    };
  }

  for (const name of ["setSignals", "setRTS", "setDTR"]) {
    if (typeof transport[name] !== "function") continue;
    const original = transport[name].bind(transport);
    transport[name] = async (...args) => {
      throwIfAborted(signal, "The serial operation was cancelled.");
      if (stopped) throw errorWithName("AbortError", "Further reset-line changes were stopped.");
      return original(...args);
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

// `keepOpen` hands a port that is still open on to the next owner instead of resetting and closing
// it; when that release fails, the chip is reset into its firmware and the port closed as usual.
async function settleTransport(port, transport, { reset = null, keepOpen = false, onResetError = () => {} } = {}) {
  let error;
  if (keepOpen && serialPortIsOpen(port) && typeof transport?.release === "function") {
    try {
      await transport.release();
      return { released: true, handedOn: true };
    } catch (failure) { error = failure; }
  }
  if (reset && serialPortIsOpen(port)) {
    // A device that disappeared cannot be reset; closing still decides the outcome.
    try { await reset(); } catch (failure) { onResetError(failure); }
  }
  let closed = !transport && !serialPortIsOpen(port);
  if (transport && typeof transport.disconnect === "function") {
    try {
      await transport.disconnect();
    } catch (failure) {
      // A port that never opened, or is already closed, leaves nothing to close.
      if (serialPortIsOpen(port)) error ||= failure;
    }
    closed = !serialPortIsOpen(port);
  }
  if (!closed && serialPortIsOpen(port) && typeof port.close === "function") {
    try { await port.close(); closed = !serialPortIsOpen(port); } catch (failure) { error ||= failure; }
  }
  if (!closed) error ||= new Error("Native serial-port closure could not be confirmed.");
  return { released: closed, handedOn: false, error };
}

function cleanupFailure(error, limitation, port) {
  const primary = error || errorWithName("SerialCleanupError", "Serial cleanup failed.");
  const lease = serialPortLease(port);
  const reservation = lease
    ? lease.phase === "quarantined"
      ? "The port remains reserved because closure was not confirmed. Reconnect the device and reload this page."
      : "The port stays reserved until its native operation and cleanup settle."
    : "The serial port is closed; no reservation is held.";
  // DOMException.message is read-only, and a primary Error may be frozen. Compose without
  // changing it; the cause retains the original name, message and any earlier cause chain.
  const details = primary === limitation ? errorMessage(primary) : `${errorMessage(primary)} ${errorMessage(limitation)}`;
  const failure = new Error(`${details} ${reservation}`, { cause: primary });
  failure.name = primary.name || "SerialCleanupError";
  failure.cleanupError = limitation;
  return failure;
}

// A foreground deadline never means native open/main/changeBaudrate was cancelled. Keep the
// lease until the actual body AND captured cleanup settle, and quarantine an unconfirmed release
// (the port closed, or handed on open with unlocked streams). Cancelling also aborts `signal`,
// which stops downloads and the active serial write or read.
function ownPortOperation(port, kind, body, cleanup, onOwnership = () => {}) {
  const lease = acquireSerialPortLease(port, kind);
  const controller = new AbortController();
  let resolveSettled;
  let rejectCancellation;
  const state = {
    port, lease, kind, cancelled: false, foreground: true, bodyFinished: false, signal: controller.signal,
    settled: new Promise((resolve) => { resolveSettled = resolve; }),
    cancellation: new Promise((_, reject) => { rejectCancellation = reject; }),
    cancel(reason = errorWithName("SerialOperationCancelledError", "The serial operation was cancelled.")) {
      if (this.cancelled) return;
      this.cancelled = true;
      this.reason = reason;
      rejectCancellation(reason);
      controller.abort(reason);
      this.onCancel?.();
    },
    guard() { if (this.cancelled) throw this.reason; }
  };
  state.cancellation.catch(() => {});
  state.bodyOutcome = Promise.resolve().then(async () => {
    try { return { value: await body(state) }; }
    catch (error) { return { error }; }
    finally { state.bodyFinished = true; }
  });
  state.bodyOutcome.then(async (outcome) => {
    updateSerialPortLease(lease, "cleanup");
    let result;
    try { result = await cleanup(state, outcome); }
    catch (error) { result = { released: false, error }; }
    if (!result.released) result.error ||= new Error("Native serial-port closure could not be confirmed.");
    if (result.released) releaseSerialPortLease(lease);
    else updateSerialPortLease(lease, "quarantined");
    resolveSettled({ ...outcome, cleanupError: result.error });
  }).catch((error) => {
    updateSerialPortLease(lease, "quarantined");
    resolveSettled({ error, cleanupError: error });
  });
  onOwnership(state);
  return state;
}

async function awaitPortOperation(state, timeoutMs, timeoutError, cleanupTimeoutMs = TRANSPORT_CLEANUP_TIMEOUT_MS) {
  let outcome;
  try {
    try {
      outcome = await withTimeout(Promise.race([state.bodyOutcome, state.cancellation]), timeoutMs, timeoutError);
    } catch (error) {
      state.cancel(error);
      if (!state.bodyFinished) throw cleanupFailure(error, new Error("The native serial operation is still pending."), state.port);
      outcome = { error };
    }
    let completed;
    try {
      completed = await withTimeout(state.settled, cleanupTimeoutMs,
        errorWithName("SerialCleanupTimeoutError", "Serial cleanup timed out."));
    } catch (error) {
      state.cancel(error);
      throw cleanupFailure(outcome.error || error, new Error("Native serial cleanup is still pending."), state.port);
    }
    if (completed.cleanupError) throw cleanupFailure(outcome.error || completed.error, completed.cleanupError, state.port);
    if (outcome.error || completed.error) throw outcome.error || completed.error;
    state.guard();
    return outcome.value;
  } finally { state.foreground = false; }
}

export async function resetToUserFirmware(port, delay = sleep, guard = () => {}) {
  if (!port || typeof port.setSignals !== "function") {
    throw new Error("This serial adapter cannot reset the ESP automatically.");
  }

  // Keep IO0 high while EN is pulsed low, then release EN. This firmware-mode reset avoids
  // leaving the chip in the flasher stub after the compatibility probe.
  guard();
  await port.setSignals({ dataTerminalReady: false, requestToSend: true });
  try {
    guard();
    await delay(100);
  } finally {
    // Once asserted, EN is always released: a cancelled pulse must not hold the chip in reset.
    await port.setSignals({ dataTerminalReady: false, requestToSend: false });
  }
  guard();
  await delay(250);
  guard();
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

export function selectManifestBuild(manifest, chipFamily) {
  if (!manifest || !Array.isArray(manifest.builds)) return undefined;
  return manifest.builds.find((build) => build.chipFamily === chipFamily);
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

// The schema-v2 part order is also the write order: the OTA selector is written last, so a stopped
// update keeps the previous boot selection. A stopped bootloader or partition-table write still
// needs another USB install.
function expectedPartLayout(chipFamily) {
  return [
    { role: "bootloader", offset: chipFamily === "ESP32" ? 0x1000 : 0, maxSize: chipFamily === "ESP32" ? 0x7000 : 0x8000 },
    { role: "partition table", offset: FLASH_LAYOUT.partitionTable.offset, maxSize: FLASH_LAYOUT.partitionTable.size },
    { role: "application", offset: FLASH_LAYOUT.ota0.offset, maxSize: FLASH_LAYOUT.ota0.size },
    { role: "OTA selector", offset: FLASH_LAYOUT.otadata.offset, exactSize: FLASH_LAYOUT.otadata.size }
  ];
}

function manifestError(message) {
  return errorWithName("InvalidManifestError", message);
}

function validateBuild(build, manifestUrl) {
  if (!build || !SUPPORTED_CHIPS.includes(build.chipFamily)) {
    throw manifestError("The firmware manifest contains an unsupported chip family.");
  }
  if (Object.keys(build).sort().join(",") !== "chipFamily,parts") {
    throw manifestError("Firmware build fields must be exactly chipFamily and parts.");
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
    // Flash writes erase whole sectors, so compare the erased range, not only the payload.
    const start = Math.floor(part.offset / FLASH_SECTOR_BYTES) * FLASH_SECTOR_BYTES;
    const end = Math.ceil((part.offset + part.size) / FLASH_SECTOR_BYTES) * FLASH_SECTOR_BYTES;
    for (const preserved of PRESERVED_FLASH_RANGES) {
      if (start < preserved.end && end > preserved.start) {
        throw manifestError(`${build.chipFamily} ${expected.role} would erase preserved ${preserved.name} data.`);
      }
    }
    if (typeof part.sha256 !== "string" || !SHA256_PATTERN.test(part.sha256)) {
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
  if (manifest.name !== PROJECT_NAME) {
    throw manifestError(`The firmware manifest must name ${PROJECT_NAME}.`);
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

function integrityError(message) {
  return errorWithName("FirmwareIntegrityError", `${message} Reload this page to select the current firmware, then try again.`);
}

function parseManifestJson(bytes) {
  // JSON.parse silently accepts repeated object keys. Reject them before interpreting the manifest.
  let source;
  try {
    source = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
  } catch (_error) {
    throw manifestError("manifest.json is not valid UTF-8.");
  }
  let cursor = 0;
  const whitespace = () => { while (/\s/.test(source[cursor] || "") && cursor < source.length) cursor++; };
  const string = () => {
    const start = cursor++;
    while (cursor < source.length) {
      const character = source[cursor++];
      if (character === "\\") cursor++;
      else if (character === '"') return JSON.parse(source.slice(start, cursor));
    }
    throw new Error("unterminated string");
  };
  const value = (depth = 0) => {
    if (depth > 16) throw new Error("manifest is too deeply nested");
    whitespace();
    const opener = source[cursor];
    if (opener === "{" || opener === "[") {
      cursor++;
      whitespace();
      const closer = opener === "{" ? "}" : "]";
      const keys = new Set();
      if (source[cursor] !== closer) {
        while (true) {
          if (opener === "{") {
            if (source[cursor] !== '"') throw new Error("invalid object key");
            const key = string();
            if (keys.has(key)) throw new Error(`duplicate key ${key}`);
            keys.add(key);
            whitespace();
            if (source[cursor++] !== ":") throw new Error("missing object colon");
          }
          value(depth + 1);
          whitespace();
          if (source[cursor] !== ",") break;
          cursor++;
          whitespace();
        }
      }
      if (source[cursor++] !== closer) throw new Error("invalid container");
    } else if (opener === '"') {
      string();
    } else {
      const primitive = /^(?:true|false|null|-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)/.exec(source.slice(cursor));
      if (!primitive) throw new Error("invalid value");
      cursor += primitive[0].length;
    }
  };
  try {
    value();
    whitespace();
    if (cursor !== source.length) throw new Error("trailing data");
    return JSON.parse(source);
  } catch (error) {
    throw manifestError(`manifest.json is invalid: ${errorMessage(error)}.`);
  }
}

function freezeDeep(value) {
  if (value && typeof value === "object") {
    Object.values(value).forEach(freezeDeep);
    Object.freeze(value);
  }
  return value;
}

function requireCrypto(cryptoImpl) {
  if (!cryptoImpl || !cryptoImpl.subtle || typeof cryptoImpl.subtle.digest !== "function") {
    throw errorWithName("FirmwareVerificationUnavailableError", "Firmware verification is unavailable in this browser. Use Chrome or Edge on a secure desktop page.");
  }
}

async function sha256(bytes, cryptoImpl) {
  const digest = new Uint8Array(await cryptoImpl.subtle.digest("SHA-256", bytes));
  return Array.from(digest, (byte) => byte.toString(16).padStart(2, "0")).join("");
}

// Streams one file into a buffer of exactly `limit` bytes. One absolute deadline covers the request
// and the body, a redirect or changed URL fails, and an aborted download cancels its body without
// waiting for a stalled source.
async function downloadBytes(url, fetchImpl, limit, siblingSignal) {
  const controller = new AbortController();
  let reader;
  let timeoutId;
  const seconds = FIRMWARE_DOWNLOAD_TIMEOUT_MS / 1000;
  const timeoutError = () => integrityError(`Firmware download exceeded its ${seconds}-second time limit.`);
  const deadline = performance.now() + FIRMWARE_DOWNLOAD_TIMEOUT_MS;
  const abortFromSibling = () => controller.abort(siblingSignal.reason);
  const interruption = new Promise((_, reject) => {
    controller.signal.addEventListener("abort", () => {
      const reason = controller.signal.reason || integrityError("Firmware download was cancelled.");
      reject(reason);
      if (reader) {
        // Cancellation may itself stall in a broken source; abort and return without waiting for it.
        try { Promise.resolve(reader.cancel(reason)).catch(() => {}); } catch (_error) {}
      }
    }, { once: true });
    timeoutId = setTimeout(() => controller.abort(timeoutError()), FIRMWARE_DOWNLOAD_TIMEOUT_MS);
  });
  interruption.catch(() => {});
  if (siblingSignal) {
    siblingSignal.addEventListener("abort", abortFromSibling, { once: true });
    if (siblingSignal.aborted) abortFromSibling();
  }
  const requireTime = () => {
    if (!controller.signal.aborted && performance.now() >= deadline) controller.abort(timeoutError());
    if (controller.signal.aborted) throw controller.signal.reason;
  };
  const consume = async () => {
    requireTime();
    const response = await fetchImpl(url, { cache: "no-store", redirect: "error", signal: controller.signal });
    if (!response.body || typeof response.body.getReader !== "function") {
      throw integrityError("The browser cannot stream firmware downloads safely.");
    }
    reader = response.body.getReader();
    // A fetch implementation may finish after its abort signal. Cancel that late body too.
    if (controller.signal.aborted) {
      try { Promise.resolve(reader.cancel(controller.signal.reason)).catch(() => {}); } catch (_error) {}
      try { reader.releaseLock(); } catch (_error) {}
    }
    requireTime();
    if (!response.ok || response.redirected || response.url !== url) {
      throw integrityError(`Downloading ${new URL(url).pathname.split("/").pop()} changed location or failed with HTTP ${response.status}.`);
    }
    const declaredSize = response.headers?.get("content-length");
    if (declaredSize !== undefined && declaredSize !== null &&
        (!/^[0-9]+$/.test(declaredSize) || !Number.isSafeInteger(Number(declaredSize)) ||
         Number(declaredSize) > limit)) {
      throw integrityError("Firmware download declared an invalid size.");
    }
    const bytes = new Uint8Array(limit);
    let size = 0;
    while (true) {
      requireTime();
      const { value, done } = await reader.read();
      requireTime();
      if (done) break;
      if (!(value instanceof Uint8Array) || value.byteLength > limit - size) {
        throw integrityError("Firmware download exceeded its byte limit.");
      }
      bytes.set(value, size);
      size += value.byteLength;
    }
    if (!size) throw integrityError("Firmware download is empty.");
    return bytes.subarray(0, size);
  };
  try {
    return await Promise.race([consume(), interruption]);
  } catch (error) {
    const failure = error?.name === "FirmwareIntegrityError" || error?.name === "InvalidManifestError" ||
      siblingSignal?.aborted && error === siblingSignal.reason
      ? error
      : integrityError(`Downloading firmware failed: ${errorMessage(error)}.`);
    controller.abort(failure);
    throw failure;
  } finally {
    clearTimeout(timeoutId);
    if (siblingSignal) siblingSignal.removeEventListener("abort", abortFromSibling);
    if (reader) {
      try { reader.releaseLock(); } catch (_error) {}
    }
  }
}

// The offer is the manifest exactly as displayed: validated once, frozen and remembered, so a flash
// can only use bytes that match the version and hashes the page shows.
export async function loadFirmwareOffer({ manifestUrl, fetchImpl = fetch, cryptoImpl = globalThis.crypto }) {
  requireCrypto(cryptoImpl);
  const url = new URL(manifestUrl);
  // The HTTPS Pages feed, or a loopback page for local browser tests.
  const local = url.protocol === "http:" && LOOPBACK_HOSTS.has(url.hostname);
  if ((url.protocol !== "https:" && !local) || url.username || url.password || url.search || url.hash ||
      !url.pathname.endsWith("/manifest.json")) {
    throw manifestError("The firmware manifest must come from the HTTPS installer directory.");
  }
  const manifest = validateManifest(parseManifestJson(await downloadBytes(url.href, fetchImpl, MAX_MANIFEST_BYTES)), url.href);
  const offer = freezeDeep({ manifestUrl: url.href, manifest });
  verifiedOffers.set(offer, { cryptoImpl });
  return offer;
}

function requireOffer(offer) {
  const verification = offer && verifiedOffers.get(offer);
  if (!verification) throw integrityError("A verified firmware offer is required before installation.");
  return verification;
}

async function fetchPart(offer, part, fetchImpl, signal) {
  const { cryptoImpl } = requireOffer(offer);
  const url = new URL(part.path, offer.manifestUrl).href;
  const data = await downloadBytes(url, fetchImpl, part.size, signal);
  if (data.length !== part.size) {
    throw integrityError(`${part.path} has ${data.length} bytes; the manifest requires ${part.size}.`);
  }
  if (await sha256(data, cryptoImpl) !== part.sha256) {
    throw integrityError(`${part.path} failed its SHA-256 integrity check.`);
  }
  return data;
}

// Downloads and verifies every part before anything is erased or written. The first failure aborts
// and cancels the sibling downloads.
export async function fetchFirmwareParts(build, offer, fetchImpl = fetch, signal) {
  requireOffer(offer);
  if (!offer.manifest.builds.includes(build)) throw integrityError("The selected build is outside the verified firmware offer.");
  const layout = expectedPartLayout(build.chipFamily);
  const downloads = new AbortController();
  const abortFromCaller = () => downloads.abort(signal.reason);
  if (signal) {
    if (signal.aborted) abortFromCaller();
    else signal.addEventListener("abort", abortFromCaller, { once: true });
  }
  const pending = build.parts.map(async (part, index) => {
    try {
      const data = await fetchPart(offer, part, fetchImpl, downloads.signal);
      if (downloads.signal.aborted) throw downloads.signal.reason;
      return { address: part.offset, data, role: layout[index].role };
    } catch (error) {
      downloads.abort(error);
      throw error;
    }
  });
  try {
    return await Promise.all(pending);
  } catch (error) {
    downloads.abort(error);
    await Promise.allSettled(pending);
    throw error;
  } finally {
    if (signal) signal.removeEventListener("abort", abortFromCaller);
  }
}

// The offered partition table of one build, verified like every other part.
export async function fetchPartitionTable(offer, build, fetchImpl = fetch) {
  requireOffer(offer);
  if (!offer.manifest.builds.includes(build)) throw integrityError("The selected build is outside the verified firmware offer.");
  return fetchPart(offer, build.parts[1], fetchImpl);
}

// Read-only look at what the device would boot, taken while the flasher stub runs. Formats follow
// ESP-IDF: partition table entries, esp_ota_select_entry_t and esp_app_desc_t.
const PARTITION_ENTRY_MAGIC = 0x50aa;
const PARTITION_MD5_MAGIC = 0xebeb;
const PARTITION_TYPE_DATA = 1;
const PARTITION_SUBTYPE_NVS = 2;
const MAX_PARTITION_TABLE_BYTES = 0xc00;
// Small reads: Chrome drains USB in pieces with short pauses, and a 3-KiB burst from the stub lost
// its middle when a pause outlasted the stub's send timeout.
const FLASH_READ_CHUNK_BYTES = 0x100;
const OTA_SELECT_SECTOR_BYTES = 0x1000;
const OTA_IMG_INVALID = 3;
const OTA_IMG_ABORTED = 4;
const APP_HEAD_BYTES = 0x70; // image and first segment headers (0x20), esp_app_desc_t through project_name
const APP_DESC_MAGIC = 0xabcd5432;

function cString(bytes) {
  const end = bytes.indexOf(0);
  const text = String.fromCharCode(...bytes.subarray(0, end < 0 ? bytes.length : end));
  return /^[\x20-\x7e]*$/.test(text) ? text : "";
}

const littleEndian = (bytes) => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

export function parsePartitionTable(bytes) {
  const view = littleEndian(bytes);
  const partitions = [];
  for (let at = 0; at + 32 <= bytes.length && view.getUint16(at, true) === PARTITION_ENTRY_MAGIC; at += 32) {
    partitions.push({
      type: bytes[at + 2], subtype: bytes[at + 3],
      offset: view.getUint32(at + 4, true), size: view.getUint32(at + 8, true),
      label: cString(bytes.subarray(at + 12, at + 28))
    });
  }
  return partitions;
}

// The configuration survives a Flash only where the installed table puts nvs exactly where this
// firmware expects it; the table itself may differ (an older layout without coredump, say).
export function hasCompatibleNvs(partitions) {
  return partitions.some((partition) => partition.type === PARTITION_TYPE_DATA &&
    partition.subtype === PARTITION_SUBTYPE_NVS &&
    partition.offset === FLASH_LAYOUT.nvs.offset && partition.size === FLASH_LAYOUT.nvs.size);
}

export function otaSequenceCrc(sequence) {
  // esp_rom_crc32_le(UINT32_MAX, &ota_seq, 4): the ROM inverts its seed, so the register starts at 0.
  let crc = 0;
  for (let shift = 0; shift < 32; shift += 8) {
    crc ^= (sequence >>> shift) & 0xff;
    for (let bit = 0; bit < 8; bit++) crc = crc & 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
  }
  return ~crc >>> 0;
}

// Mirrors the bootloader: the valid otadata entry with the highest sequence selects
// ota[(seq - 1) % count]; without one it boots factory, else ota_0.
export function selectBootPartition(partitions, otadataEntries = []) {
  const apps = partitions.filter((partition) => partition.type === 0);
  const ota = apps.filter((partition) => partition.subtype >= 0x10 && partition.subtype < 0x20)
    .sort((left, right) => left.subtype - right.subtype);
  let sequence;
  for (const entry of otadataEntries) {
    if (!entry || entry.length < 32) continue;
    const view = littleEndian(entry);
    const candidate = view.getUint32(0, true);
    const state = view.getUint32(24, true);
    if (candidate === 0xffffffff || state === OTA_IMG_INVALID || state === OTA_IMG_ABORTED ||
        view.getUint32(28, true) !== otaSequenceCrc(candidate)) continue;
    if (sequence === undefined || candidate > sequence) sequence = candidate;
  }
  if (sequence !== undefined && ota.length) return ota[((sequence - 1) >>> 0) % ota.length];
  return apps.find((partition) => partition.subtype === 0) || ota[0];
}

export function parseAppDescription(bytes) {
  if (!bytes || bytes.length < APP_HEAD_BYTES || bytes[0] !== 0xe9) return null;
  if (littleEndian(bytes).getUint32(0x20, true) !== APP_DESC_MAGIC) return null;
  return { version: cString(bytes.subarray(0x30, 0x50)), project: cString(bytes.subarray(0x50, 0x70)) };
}

// Bytes of a partition table up to and including its MD5 entry, or up to its end marker; undefined
// while the given bytes do not reach either. The bootloader ignores everything after that point.
export function partitionTableLength(bytes) {
  const view = littleEndian(bytes);
  for (let at = 0; at + 32 <= bytes.length; at += 32) {
    const magic = view.getUint16(at, true);
    if (magic === PARTITION_MD5_MAGIC) return at + 32;
    if (magic !== PARTITION_ENTRY_MAGIC) return at;
  }
  return undefined;
}

export async function inspectInstalledFirmware(loader, expectedTable, flashSize, guard = () => {}) {
  const limit = flashBytes(flashSize) || MIN_FLASH_BYTES;
  const tableBytes = expectedTable?.length || MAX_PARTITION_TABLE_BYTES;
  // A read that is skipped or comes back short leaves the installed firmware unknown, never foreign:
  // an unusual flash ID must not push a valid installation into Clean flash.
  const read = async (offset, length) => {
    if (!(offset + length <= limit)) {
      throw errorWithName("FlashReadError", `0x${offset.toString(16)} lies beyond the ${flashSize || "assumed 4MB"} flash.`);
    }
    const bytes = await withTimeout(loader.readFlash(offset, length), FLASH_READ_STEP_TIMEOUT_MS,
      errorWithName("FlashReadTimeoutError", `Reading flash at 0x${offset.toString(16)} timed out.`));
    guard();
    if (!(bytes instanceof Uint8Array) || bytes.length < length) {
      throw errorWithName("FlashReadError", `Reading flash at 0x${offset.toString(16)} returned too few bytes.`);
    }
    return bytes.subarray(0, length);
  };
  let table = new Uint8Array(0);
  while (partitionTableLength(table) === undefined && table.length < tableBytes) {
    const chunk = await read(FLASH_LAYOUT.partitionTable.offset + table.length,
      Math.min(FLASH_READ_CHUNK_BYTES, tableBytes - table.length));
    const joined = new Uint8Array(table.length + chunk.length);
    joined.set(table);
    joined.set(chunk, table.length);
    table = joined;
  }
  const used = partitionTableLength(table) ?? table.length;
  const expectedUsed = expectedTable ? partitionTableLength(expectedTable) ?? expectedTable.length : -1;
  const layoutMatches = used === expectedUsed && table.subarray(0, used).every((byte, index) => byte === expectedTable[index]);
  const partitions = parsePartitionTable(table);
  const otadata = partitions.find((partition) => partition.type === PARTITION_TYPE_DATA && partition.subtype === 0);
  const entries = [];
  if (otadata) {
    for (const sector of [0, OTA_SELECT_SECTOR_BYTES]) entries.push(await read(otadata.offset + sector, 32));
  }
  const app = selectBootPartition(partitions, entries);
  const firmware = app ? parseAppDescription(await read(app.offset, APP_HEAD_BYTES)) : null;
  return { layoutMatches, nvsMatches: hasCompatibleNvs(partitions), firmware, slot: app?.label || "" };
}

// Flash keeps the configuration only for this firmware on an nvs-compatible layout. A check that
// could not read the device leaves both choices to the user.
export function configurationKeepable(installed) {
  if (!installed || installed.error) return true;
  return Boolean(installed.nvsMatches && installed.firmware?.project === PROJECT_NAME);
}

function flashBytes(flashSize) {
  const match = /^([0-9]+)(KB|MB)$/.exec(String(flashSize || ""));
  if (!match) return 0;
  return Number(match[1]) * (match[2] === "MB" ? 0x100000 : 0x400);
}

// A detected size below 4 MB fails before any download or write. An undetected size is reported
// and left to the bootloader, as unusual flash IDs are common on third-party boards.
async function requireFlashCapacity(loader) {
  if (typeof loader.detectFlashSize !== "function") return "";
  const flashSize = await loader.detectFlashSize();
  const bytes = flashBytes(flashSize);
  if (bytes && bytes < MIN_FLASH_BYTES) {
    throw errorWithName("UnsupportedFlashSizeError", `This firmware needs at least 4 MB of flash; the board reports ${flashSize}.`);
  }
  return bytes ? flashSize : "";
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

function makeLoader(ESPLoaderCtor, transport, onLog, operation, trace) {
  return new ESPLoaderCtor({
    transport, baudrate: 115200,
    terminal: terminalAdapter((text) => { if (!operation.cancelled && operation.foreground) onLog(text); }),
    debugLogging: false, enableTracing: trace, serialOptions: ESPTOOL_SERIAL_OPTIONS
  });
}

// With `keepOpen`, a successful check leaves the port open and the chip in the flasher for the live
// log, which resets it into the firmware; otherwise the check resets the chip and closes the port.
export async function probeDevice({
  port, manifest, expectedPartitionTables = null, TransportCtor, ESPLoaderCtor, onLog = () => {}, onStep = () => {},
  onOpened = () => {}, trace = false, keepOpen = false, delay = sleep,
  timeoutMs = DEVICE_PROBE_TIMEOUT_MS + (expectedPartitionTables ? INSPECTION_TIMEOUT_MS : 0),
  cleanupTimeoutMs = TRANSPORT_CLEANUP_TIMEOUT_MS,
  onOwnership
}) {
  let transport;
  let loader;
  let abortable;
  let probed = false;
  const state = ownPortOperation(port, "probe", async (operation) => {
    operation.guard();
    transport = new (sharedPortTransport(TransportCtor))(port);
    transport.onOpened = () => { if (!operation.cancelled && operation.foreground) onOpened(); };
    abortable = makeAbortableTransport(transport, operation.signal);
    loader = makeLoader(ESPLoaderCtor, transport, onLog, operation, trace);
    await loader.main();
    operation.guard();
    if (typeof loader.flashId === "function") await loader.flashId();
    operation.guard();
    const chipFamily = loader.chip && loader.chip.CHIP_NAME;
    if (!chipFamily) throw errorWithName("ChipDetectionError", "The connected ESP chip could not be identified.");
    const build = selectManifestBuild(manifest, chipFamily);
    if (!build) throw errorWithName("UnsupportedChipError", `${chipFamily} is not supported by this firmware.`);
    await assertChipRevisionSupported(loader, chipFamily);
    operation.guard();
    const flashSize = await requireFlashCapacity(loader);
    operation.guard();
    // Without the offer's partition table there is nothing to compare; only the chip is checked. A
    // failed read leaves the installed firmware unknown instead of failing the connection.
    let installed = null;
    const expectedTable = expectedPartitionTables?.get?.(chipFamily);
    if (expectedTable) {
      if (operation.foreground) onStep("Reading partition table and app header");
      try {
        installed = await inspectInstalledFirmware(loader, expectedTable, flashSize, () => operation.guard());
      } catch (error) {
        // A cancelled probe ends in awaitPortOperation regardless of this result.
        installed = { error: errorMessage(error) };
      }
    }
    probed = true;
    return { chipFamily, flashSize, build, installed };
  }, async (operation) => {
    if (operation.cancelled && abortable) await abortable.settleAbort();
    abortable?.dispose();
    // esptool-js's connect attempts pulse EN with IO0 low; even a check that never reached the chip
    // may have left it in download mode, so an open port is always reset into the firmware.
    return settleTransport(port, transport, {
      keepOpen: probed && keepOpen && !operation.cancelled,
      reset: () => resetToUserFirmware(port, delay)
    });
  }, onOwnership);
  return awaitPortOperation(state, timeoutMs, errorWithName(
    "DeviceProbeTimeoutError",
    "The serial device did not answer in flashing mode. Select the ESP chip's native USB port when available; otherwise hold BOOT, tap RESET, then try again."
  ), cleanupTimeoutMs);
}

// Writes the verified parts one address range at a time and the OTA selector last, so a stopped
// update keeps the previous boot selection; after an erase, or when that selection already names
// ota_0, the bootloader's image check is the guard. With `keepOpen`, the port stays open for the live
// log, which starts the firmware; otherwise the install resets the chip itself and closes the port.
export async function flashDevice({
  port, offer, eraseFirst, TransportCtor, ESPLoaderCtor, fetchImpl = fetch, trace = false, keepOpen = false,
  onState = () => {}, onLog = () => {}, onOwnership, delay = sleep,
  timeoutMs = FLASH_OPERATION_TIMEOUT_MS, cleanupTimeoutMs = TRANSPORT_CLEANUP_TIMEOUT_MS
}) {
  requireOffer(offer);
  const manifest = offer.manifest;
  let transport;
  let loader;
  let abortable;
  const state = ownPortOperation(port, "flash", async (operation) => {
    operation.guard();
    transport = new (sharedPortTransport(TransportCtor))(port);
    abortable = makeAbortableTransport(transport, operation.signal);
    loader = makeLoader(ESPLoaderCtor, transport, onLog, operation, trace);
    const report = (value) => { operation.guard(); onState(value); };
    report({ stage: "connecting", percentage: 0, message: "Checking device" });
    await loader.main();
    operation.guard();
    if (typeof loader.flashId === "function") await loader.flashId();
    operation.guard();
    const chipFamily = loader.chip && loader.chip.CHIP_NAME;
    const build = selectManifestBuild(manifest, chipFamily);
    if (!build) throw errorWithName("UnsupportedChipError",
      chipFamily ? `${chipFamily} is not supported by this firmware.` : "The ESP chip could not be identified.");
    await assertChipRevisionSupported(loader, chipFamily);
    operation.guard();
    await requireFlashCapacity(loader);
    operation.guard();
    // The ROM loader erases in 16 KiB blocks: the partition table write would reach into nvs and
    // the OTA selector into phy_init. Only the flasher stub keeps the erase within each part.
    if (loader.IS_STUB !== true) {
      throw errorWithName("FlasherStubError",
        "The flasher stub is not running; writing through the ROM loader could erase NVS, so nothing was written.");
    }
    report({ stage: "preparing", percentage: 0, message: "Loading firmware" });
    const fileArray = await fetchFirmwareParts(build, offer, fetchImpl, operation.signal);
    operation.guard();
    if (eraseFirst) {
      report({ stage: "erasing", percentage: 0, message: "Erasing flash" });
      await loader.eraseFlash();
      operation.guard();
    }
    for (let index = 0; index < fileArray.length; index++) {
      const file = fileArray[index];
      const activating = index === fileArray.length - 1;
      const stage = activating ? "activating" : "writing";
      const message = activating ? "Activating firmware" : "Writing firmware";
      // report() checks for cancellation, so a range never starts after the previous one was stopped.
      report({ stage, percentage: combinedProgress(fileArray, index, 0, file.data.length), message });
      // From here on the stub may erase the OTA selector, which already selects the new app.
      if (activating) operation.activationStarted = true;
      // esptool-js loops over every entry of one writeFlash call; one part per call returns control
      // between address ranges, so a cancellation never starts the next range.
      await loader.writeFlash({
        fileArray: [file], flashSize: "keep", flashMode: "keep", flashFreq: "keep",
        eraseAll: false, compress: true,
        reportProgress(_fileIndex, written, total) {
          operation.guard();
          if (operation.foreground) onState({ stage, percentage: combinedProgress(fileArray, index, written, total), message });
        }
      });
    }
    // Once the OTA selector is written the new app is selected; a cancellation arriving now no
    // longer turns the completed write into a failure.
    if (!operation.cancelled && operation.foreground) onState({ stage: "restarting", percentage: 100, message: "Starting firmware" });
    return { chipFamily, build };
  }, async (operation, outcome) => {
    if (operation.cancelled && abortable) await abortable.settleAbort();
    abortable?.dispose();
    return settleTransport(port, transport, {
      keepOpen: keepOpen && !operation.cancelled,
      reset: () => resetToUserFirmware(port, delay),
      onResetError: (error) => {
        if (outcome.value && operation.foreground) onLog(`Automatic restart unavailable: ${errorMessage(error)}\n`);
      }
    });
  }, onOwnership);
  return awaitPortOperation(state, timeoutMs,
    errorWithName("FlashTimeoutError", "Firmware installation timed out."), cleanupTimeoutMs);
}

export function attachWebInstaller({
  root,
  serial,
  TransportCtor,
  ESPLoaderCtor,
  fetchImpl = fetch,
  cryptoImpl = globalThis.crypto,
  manifestPath = "manifest.json",
  locationImpl = globalThis.location,
  SerialPortCtor = globalThis.SerialPort,
  followLog = true, // open the live log after connecting and after installing
  trace = false, // also log esptool-js's packet trace (diagnostics; the page enables it with ?trace)
  delay = sleep, // reset pulses and retry pauses
  now = () => globalThis.performance?.now?.() ?? Date.now(), // the reconnect grace clock (monotonic)
  // Clean flash deletes the configuration and pairing; it needs its own confirmation.
  confirmEraseImpl = (message) => globalThis.confirm?.(message) === true
}) {
  if (!root) throw new Error("The installer root is missing.");

  const element = (id) => root.querySelector(`#${id}`);
  const connectButton = element("connect-button");
  const disconnectButton = element("disconnect-button");
  const resetButton = element("reset-button");
  const flashButton = element("flash-button");
  const cleanFlashButton = element("clean-flash-button");
  const monitorOutput = element("serial-monitor-output");
  const monitorLive = element("serial-monitor-live");
  const versionValue = element("firmware-version-value");
  const versionMeta = element("firmware-meta");
  const channelValue = element("firmware-channel-value");
  const sourceLink = element("firmware-source-link");
  const buttonLabels = new Map([[flashButton, "Flash"], [cleanFlashButton, "Clean flash"]]);

  let generation = 0;
  let current = null;
  let foreground = null;
  let offer = null;
  let manifest = null;
  let expectedTables = null;
  let monitorPendingLine = "";
  let bootCheck = null;
  // Connect pairs the chosen port (the chooser grants this page access); Disconnect unpairs it again.
  const canForget = supportsSerialForget(serial, SerialPortCtor);
  const serialSupported = Boolean(
    serial && typeof serial.requestPort === "function" && globalThis.isSecureContext
  );

  // With tracing, esptool-js's transport reports every packet; the trace goes to the log, not the console.
  const traceStarted = Date.now();
  const LoggedTransport = trace ? class extends TransportCtor {
    trace(message) { appendWholeLines(`TRACE +${Date.now() - traceStarted} ms ${message}`, "info"); }
  } : TransportCtor;

  // `own` marks the installer's own time-stamped lines, set apart from device output.
  const appendRenderedMonitorLine = ({ text, terminated }, level = serialLogLevel(text), own = false) => {
    const line = monitorOutput.ownerDocument.createElement("span");
    line.className = `installer-monitor-line installer-monitor-line-${level}${own ? " installer-monitor-line-own" : ""}`;
    line.textContent = `${stripSerialAnsi(text)}${terminated ? "\n" : ""}`;
    monitorOutput.append(line);
  };

  // Everything the page reports goes to the log; installer lines carry a time stamp and are rendered
  // whole, so a device line that is still incomplete cannot absorb them.
  function appendWholeLines(text, level) {
    // An unfinished device or esptool line, such as "Connecting....", is shown as it stands first.
    if (monitorPendingLine) {
      appendRenderedMonitorLine({ text: monitorPendingLine.replace(/\r$/, ""), terminated: true });
      monitorPendingLine = "";
    }
    for (const line of text.split("\n")) appendRenderedMonitorLine({ text: line, terminated: true }, level, true);
    settleMonitor();
  }
  const appendStatusLine = (message, level = "status") => {
    const time = new Date().toLocaleTimeString([], { hour12: false });
    appendWholeLines(`[${time}] ${message}`, level);
  };

  // After an install the log waits for the new firmware to name its version.
  const clearBootCheck = () => {
    if (bootCheck?.timer) clearTimeout(bootCheck.timer);
    bootCheck = null;
  };
  const startBootTimer = (session) => {
    if (!bootCheck || bootCheck.session !== session || bootCheck.timer) return;
    const check = bootCheck;
    check.timer = setTimeout(() => {
      if (bootCheck !== check) return;
      bootCheck = null;
      if (isCurrent(session)) {
        appendStatusLine(`${check.version} not seen within ${BOOT_VERIFICATION_TIMEOUT_MS / 1000} s; press Reset or the board's RESET button, or reconnect the cable`, "warning");
      }
    }, BOOT_VERIFICATION_TIMEOUT_MS);
  };

  // The boot check follows the device's own line breaks, so an installer line shown in between
  // cannot split the one console line that names the version.
  const scanBootVersion = (text, flush) => {
    if (!bootCheck || !isCurrent(bootCheck.session)) return "";
    const parsed = splitSerialChunk(bootCheck.pending, text, flush);
    bootCheck.pending = parsed.pending.slice(-512);
    for (const line of parsed.lines) {
      const seen = bootVersion(line.text);
      if (seen) return seen;
    }
    return "";
  };

  const appendMonitor = (text, { flush = false } = {}) => {
    if (!text && !flush) return;
    const seen = scanBootVersion(text, flush);
    const parsed = splitSerialChunk(monitorPendingLine, text, flush);
    monitorPendingLine = parsed.pending;
    parsed.lines.forEach((line) => appendRenderedMonitorLine(line));
    settleMonitor();
    if (!seen) return;
    const expected = bootCheck.version;
    clearBootCheck();
    if (seen === expected) appendStatusLine(`Booted ${seen}`, "ok");
    else appendStatusLine(`Booted ${seen}, expected ${expected}`, "warning");
  };

  // The log follows new lines only while it is scrolled to its end. Scrolling up holds the view
  // until the user scrolls back down to the end.
  let logAtEnd = true;
  monitorOutput.addEventListener("scroll", () => {
    logAtEnd = monitorOutput.scrollHeight - monitorOutput.scrollTop - monitorOutput.clientHeight <= LOG_END_SLACK_PX;
  });
  function settleMonitor() {
    if (monitorOutput.textContent.length + monitorPendingLine.length > MAX_MONITOR_CHARS) {
      // Trim in batches so a busy UART does not force a full DOM rebuild on every following chunk.
      const keep = Math.floor(MAX_MONITOR_CHARS * 0.8);
      const retained = `${monitorOutput.textContent}${stripSerialAnsi(monitorPendingLine)}`.slice(-keep);
      const heightBefore = monitorOutput.scrollHeight;
      monitorOutput.replaceChildren();
      const reparsed = splitSerialChunk("", retained);
      monitorPendingLine = reparsed.pending;
      reparsed.lines.forEach((line) => appendRenderedMonitorLine(line));
      // A held view keeps showing the same lines although older ones above it were dropped.
      if (!logAtEnd) monitorOutput.scrollTop = Math.max(0, monitorOutput.scrollTop - (heightBefore - monitorOutput.scrollHeight));
    }
    if (logAtEnd) monitorOutput.scrollTop = monitorOutput.scrollHeight;
  }

  const setLive = (state, label) => {
    monitorLive.textContent = label;
    monitorLive.dataset.state = state;
  };

  // Flash progress fills the button that started the installation and is also its label.
  const setProgress = (button, percentage) => {
    for (const control of [flashButton, cleanFlashButton]) {
      if (control === button) continue;
      delete control.dataset.progress;
      control.style.removeProperty("--progress");
      control.textContent = buttonLabels.get(control);
    }
    if (!button) return;
    const value = Math.min(100, Math.max(0, Math.round(percentage || 0)));
    button.dataset.progress = String(value);
    button.style.setProperty("--progress", `${value}%`);
    button.textContent = `${buttonLabels.get(button)} ${value}%`;
  };

  const isCurrent = (session) => Boolean(session && current === session && generation === session.id);
  const syncControls = () => {
    const connected = Boolean(current?.compatible);
    const lease = current && serialPortLease(current.port);
    const monitorSelected = Boolean(current?.monitor && lease === current.monitor.lease);
    const monitorOwns = Boolean(monitorSelected && lease.phase === "monitor" &&
      current.monitor.readyFinished && !current.monitor.cancelled);
    const nativePending = Boolean(lease && lease.phase !== "quarantined" && !monitorOwns);
    const busy = Boolean(foreground) || nativePending;
    // Device actions may take the port over from the live log, never from a pending or quarantined
    // native operation. Unavailable controls are greyed out; Connect and Disconnect share one place.
    const portReady = connected && !busy && !(lease && !monitorOwns);
    const linked = connected || foreground?.kind === "probe";
    connectButton.hidden = linked;
    connectButton.disabled = !offer || !expectedTables || !serialSupported || Boolean(foreground);
    disconnectButton.hidden = !linked;
    // A flash or a reset pulse owns native calls the log's lease does not cover; they finish first.
    disconnectButton.disabled = foreground?.kind === "flash" || foreground?.resetting === true;
    resetButton.hidden = false;
    resetButton.disabled = !portReady;
    // Firmware that cannot keep the configuration leaves nothing worth keeping: only Clean flash remains.
    const eraseRequired = Boolean(connected && !current.device?.keepable);
    flashButton.hidden = eraseRequired;
    flashButton.disabled = !portReady || eraseRequired;
    cleanFlashButton.hidden = false;
    cleanFlashButton.disabled = !portReady;
    root.dataset.busy = busy ? "true" : "false";
    root.dataset.ownership = lease?.phase || "idle";
    root.setAttribute?.("aria-busy", busy ? "true" : "false");
    for (const [button, kind] of [[connectButton, "probe"], [flashButton, "flash"], [cleanFlashButton, "flash"], [resetButton, "monitor"]]) {
      button.setAttribute("aria-busy", foreground?.kind === kind || (nativePending && lease.kind === kind) ? "true" : "false");
    }
  };
  // Every lease change is the installer touching a port, including the cleanup after a USB
  // disconnect that precedes a re-enumerated board's connect; it opens the reconnect grace window.
  let lastPortActivity = -Infinity;
  subscribeSerialOwnership(() => {
    lastPortActivity = now();
    syncControls();
  });
  const captureOwnership = (session) => (operation) => {
    session.operation = operation;
    operation.settled.then(() => {
      if (session.operation === operation) session.operation = null;
      syncControls();
    });
    syncControls();
  };
  const setDisconnected = () => {
    root.dataset.connected = "false";
    root.dataset.flashing = "false";
    root.dataset.finished = "false";
    setLive("stopped", "Stopped");
    setProgress(null);
    syncControls();
  };
  // Stopping the log closes the port, unless an install takes it over (`keepOpen`).
  const stopMonitor = async ({ session = current, keepOpen = false } = {}) => {
    const operation = session?.monitor;
    if (!operation) return;
    operation.keepOpen = keepOpen;
    operation.cancel();
    if (isCurrent(session)) {
      setLive("closing", "Closing");
      syncControls();
    }
    let outcome;
    try {
      outcome = await withTimeout(operation.settled, TRANSPORT_CLEANUP_TIMEOUT_MS,
        errorWithName("SerialCleanupTimeoutError", "Serial monitor cleanup timed out."));
    } catch (error) { throw cleanupFailure(error, new Error("Native monitor open, reset or cleanup is still pending."), operation.port); }
    if (outcome.cleanupError) throw cleanupFailure(outcome.error, outcome.cleanupError, operation.port);
    if (outcome.error && outcome.error.name !== "SerialOperationCancelledError") throw outcome.error;
    if (isCurrent(session)) {
      setLive("stopped", "Stopped");
      syncControls();
    }
  };
  const disconnect = async () => {
    const session = current;
    const previousForeground = foreground;
    const disconnectGeneration = ++generation;
    current = null;
    foreground = null;
    monitorPendingLine = "";
    clearBootCheck();
    session?.operation?.cancel();
    previousForeground?.operation?.cancel();
    setDisconnected();
    if (!session) return;
    try {
      if (session.monitor) await stopMonitor({ session });
      else if (session.operation) {
        const outcome = await withTimeout(session.operation.settled, TRANSPORT_CLEANUP_TIMEOUT_MS,
          errorWithName("SerialCleanupTimeoutError", "Serial cleanup is still pending."));
        if (outcome.cleanupError) throw cleanupFailure(outcome.error, outcome.cleanupError, session.port);
      }
      // A port handed on by a check or install that the log never took: the chip may still run the
      // flasher, so start the firmware, then close the port.
      const port = session.port;
      if (serialPortIsOpen(port) && !serialPortLease(port)) {
        const lease = acquireSerialPortLease(port, "close");
        try {
          try { await resetToUserFirmware(port, delay); } catch (_error) { /* a lost device cannot be reset */ }
          await port.close();
        } finally {
          if (serialPortIsOpen(port)) updateSerialPortLease(lease, "quarantined");
          else releaseSerialPortLease(lease);
        }
      }
    } catch (error) {
      if (!current && generation === disconnectGeneration) appendStatusLine(`Disconnect: ${errorMessage(error)}`, "error");
    }
  };
  const deferred = () => {
    let resolve;
    let reject;
    const promise = new Promise((accept, fail) => { resolve = accept; reject = fail; });
    promise.catch(() => {});
    return { promise, resolve, reject };
  };
  const startMonitor = async () => {
    const session = current;
    if (!session?.compatible || foreground || session.monitor || serialPortLease(session.port)) return;
    const port = session.port;
    const token = { kind: "monitor", session };
    foreground = token;
    const readyMonitor = deferred();
    const stop = deferred();
    let reader;
    let readerAttached = false;
    let loop;
    let cancelReader;
    let loopError;
    const cancelCapturedReader = () => {
      if (readerAttached && !cancelReader) {
        cancelReader = Promise.resolve().then(() => { if (readerAttached) return reader.cancel(); });
        cancelReader.catch(() => {});
      }
    };
    let operation;
    try {
      operation = ownPortOperation(port, "monitor", async (owned) => {
        owned.guard();
        // Usually the check or install hands the port over open; the log opens it only on its own.
        if (!serialPortIsOpen(port)) await port.open({ baudRate: 115200, ...ESPTOOL_SERIAL_OPTIONS });
        owned.guard();
        if (!isCurrent(session)) throw errorWithName("SerialOperationCancelledError", "Monitor selection changed.");
        if (!port.readable) throw new Error("The serial input stream is unavailable.");
        reader = port.readable.getReader();
        readerAttached = true;
        const decoder = new TextDecoder();
        loop = (async () => {
          try {
            while (true) {
              const { value, done } = await reader.read();
              if (owned.cancelled || !isCurrent(session)) break;
              if (done) break;
              appendMonitor(decoder.decode(value, { stream: true }));
            }
            if (!owned.cancelled && isCurrent(session)) appendMonitor(decoder.decode(), { flush: true });
          } catch (error) { loopError = error; }
          finally {
            try { reader.releaseLock(); readerAttached = false; } catch (error) { loopError ||= error; }
            stop.resolve();
          }
        })();
        appendStatusLine("Reset to firmware");
        await resetToUserFirmware(port, delay, () => owned.guard());
        owned.guard();
        owned.readyFinished = true;
        readyMonitor.resolve();
        await stop.promise;
        if (loopError) throw loopError;
      }, async (owned) => {
        let error;
        cancelCapturedReader();
        if (cancelReader) { try { await cancelReader; } catch (failure) { error = failure; } }
        if (loop) { try { await loop; } catch (failure) { error ||= failure; } }
        if (owned.keepOpen) return { released: !port.readable?.locked, error };
        if (serialPortIsOpen(port)) {
          try { await port.close(); } catch (failure) { return { released: false, error: error || failure }; }
        }
        return { released: !serialPortIsOpen(port), error };
      }, captureOwnership(session));
      token.operation = operation;
      session.monitor = operation;
      operation.onCancel = () => { stop.resolve(); cancelCapturedReader(); };
      operation.settled.then((outcome) => {
        readyMonitor.reject(outcome.error || outcome.cleanupError || new Error("The serial monitor stopped before it was ready."));
        if (!isCurrent(session) || session.monitor !== operation) return;
        const error = outcome.cleanupError ? cleanupFailure(outcome.error, outcome.cleanupError, port) :
          outcome.error?.name !== "SerialOperationCancelledError" ? outcome.error : null;
        if (error) setLive("error", "Error");
        else setLive("stopped", "Stopped");
        if (error) appendStatusLine(`Monitor stopped: ${errorMessage(error)}`, "error");
        if (!serialPortLease(port)) session.monitor = null;
        syncControls();
      });
      setLive("connecting", "Connecting");
      syncControls();
      await withTimeout(Promise.race([readyMonitor.promise, operation.cancellation]), DEVICE_PROBE_TIMEOUT_MS,
        errorWithName("MonitorOpenTimeoutError", "Opening the serial monitor timed out."));
      if (!isCurrent(session) || operation.cancelled || session.monitor !== operation ||
          serialPortLease(port) !== operation.lease || operation.lease.phase !== "monitor") return;
      setLive("live", "Live");
      appendStatusLine("Monitor 115200 baud");
      startBootTimer(session);
    } catch (error) {
      operation?.cancel(error);
      const reported = operation && !operation.bodyFinished
        ? cleanupFailure(error, new Error("Native monitor open or reset is still pending."), port) : error;
      if (isCurrent(session)) {
        setLive("error", "Error");
        appendStatusLine(`Monitor failed: ${errorMessage(reported)}`, "error");
      }
    } finally {
      if (operation) operation.foreground = false;
      if (foreground === token) foreground = null;
      syncControls();
    }
  };
  const describeInstalled = (installed) => {
    const firmware = installed?.firmware;
    const image = firmware?.project ? `${firmware.project} ${firmware.version} (${installed.slot})` : "no app";
    if (configurationKeepable(installed)) return `Installed: ${image}; configuration can be kept`;
    return `Installed: ${image}; ${installed?.nvsMatches ? "other firmware" : "NVS layout differs"}`;
  };
  // Connect always asks with the browser's chooser; `paired` is a port the browser already lists as
  // paired, reconnected without asking (see reconnectPaired), which may leave a busy port unlogged.
  const connect = async ({ paired = null, quietBusy = false } = {}) => {
    if (!serialSupported || !offer || !expectedTables || foreground || current?.compatible) return;
    const token = { id: ++generation, kind: "probe", session: null };
    foreground = token;
    current = null;
    monitorPendingLine = "";
    clearBootCheck();
    setDisconnected();
    let session = null;
    let result;
    try {
      const port = paired || await serial.requestPort();
      if (foreground !== token || generation !== token.id) return;
      session = { id: token.id, port, compatible: false, device: null, monitor: null, operation: null };
      current = session;
      token.session = session;
      syncControls();
      result = await probeDevice({
        port, manifest, expectedPartitionTables: expectedTables, TransportCtor: LoggedTransport, ESPLoaderCtor, trace,
        keepOpen: followLog, delay,
        onOpened: () => { if (isCurrent(session)) appendStatusLine(paired ? "Probe (paired port)" : "Probe"); },
        onOwnership: (operation) => { token.operation = operation; captureOwnership(session)(operation); },
        onLog: (text) => { if (isCurrent(session)) appendMonitor(text); },
        onStep: (message) => { if (isCurrent(session)) appendStatusLine(message); }
      });
      if (!isCurrent(session) || foreground !== token) return;
      const { installed } = result;
      const info = typeof port.getInfo === "function" ? port.getInfo() : {};
      session.compatible = true;
      session.device = { chipFamily: result.chipFamily, flashSize: result.flashSize, installed, keepable: configurationKeepable(installed) };
      root.dataset.connected = "true";
      appendStatusLine(`${result.chipFamily} · ${result.flashSize || "flash size unknown"} · ${describeSerialConnection(info)}`);
      if (!result.flashSize) appendStatusLine("Flash size not detected; this firmware needs 4 MB", "warning");
      if (!installed) {
        // The offer always carries its partition tables; without one the device was not inspected.
      } else if (installed.error) {
        const reason = /[.!?]$/.test(installed.error) ? installed.error : `${installed.error}.`;
        appendStatusLine(`Installed firmware unknown: ${reason} Use Flash only if the board already runs tesla-key-esp32`, "warning");
      } else {
        appendStatusLine(describeInstalled(installed));
        if (!session.device.keepable) appendStatusLine("Clean flash required", "warning");
        else if (!installed.layoutMatches) appendStatusLine("Partition table differs; Flash also writes the current table", "warning");
      }
    } catch (error) {
      if (foreground !== token || generation !== token.id) return;
      setDisconnected();
      if (error?.name === "NotFoundError") appendStatusLine("No port selected");
      else if (isOpenFailure(error)) {
        if (!quietBusy) {
          appendStatusLine(paired ? "Paired port busy (open in another tab or program)" : "Probe failed: port busy (open in another tab or program)",
            paired ? "warning" : "error");
        }
        return PORT_BUSY;
      } else appendStatusLine(`Probe failed: ${errorMessage(error)}`, "error");
      return;
    } finally {
      if (foreground === token) foreground = null;
      syncControls();
    }
    if (followLog && isCurrent(session)) await startMonitor();
    return result;
  };
  // Restarts the firmware and follows its boot from the first line: the live log keeps its port and
  // only the reset line is pulsed; without a live log, starting one resets the firmware.
  const reset = async () => {
    const session = current;
    if (!session?.compatible || foreground) return;
    const monitor = session.monitor;
    const lease = serialPortLease(session.port);
    if (!monitor || lease !== monitor.lease || lease.phase !== "monitor" || !monitor.readyFinished || monitor.cancelled) {
      if (!monitor) await startMonitor();
      return;
    }
    const token = { kind: "monitor", session, operation: monitor, resetting: true };
    foreground = token;
    syncControls();
    try {
      appendStatusLine("Reset to firmware");
      await resetToUserFirmware(session.port, delay, () => monitor.guard());
    } catch (error) {
      if (isCurrent(session) && !monitor.cancelled) appendStatusLine(`Reset failed: ${errorMessage(error)}`, "error");
    } finally {
      if (foreground === token) foreground = null;
      syncControls();
    }
  };
  const install = async ({ eraseFirst = false } = {}) => {
    const session = current;
    if (!session?.compatible || !offer || foreground) return;
    if (!eraseFirst && !session.device?.keepable) {
      appendStatusLine("Flash refused: Clean flash required", "error");
      syncControls();
      return;
    }
    if (eraseFirst && confirmEraseImpl(
      "Clean flash? This permanently deletes WiFi, VIN, MQTT settings, the private pairing key and BLE sessions. " +
      "Back up secret NVS before continuing. Vehicle pairing will be required again."
    ) !== true) {
      appendStatusLine("Clean flash cancelled; nothing was written");
      return;
    }
    const selectedOffer = offer;
    const { version } = selectedOffer.manifest;
    const token = { kind: "flash", session };
    const progressButton = eraseFirst ? cleanFlashButton : flashButton;
    foreground = token;
    clearBootCheck();
    syncControls();
    let installed = false;
    let flashOperation = null;
    try {
      await stopMonitor({ session, keepOpen: true });
      if (!isCurrent(session) || foreground !== token) return;
      root.dataset.flashing = "true";
      root.dataset.finished = "false";
      setLive("busy", "Flashing");
      setProgress(progressButton, 0);
      appendStatusLine(eraseFirst ? `Clean flash ${version}: erase all` : `Flash ${version}: configuration kept`);
      await flashDevice({
        port: session.port, offer: selectedOffer, eraseFirst, TransportCtor: LoggedTransport, ESPLoaderCtor, fetchImpl, trace,
        keepOpen: followLog, delay,
        onOwnership: (operation) => { flashOperation = operation; token.operation = operation; captureOwnership(session)(operation); },
        onLog: (text) => { if (isCurrent(session)) appendMonitor(text); },
        onState: (state) => { if (isCurrent(session)) setProgress(progressButton, state.percentage); }
      });
      if (!isCurrent(session) || foreground !== token) return;
      root.dataset.flashing = "false";
      root.dataset.finished = "true";
      session.device = { ...session.device, installed: null, keepable: true };
      setProgress(null);
      setLive("stopped", "Stopped");
      appendStatusLine(`Installed ${version}`);
      installed = true;
    } catch (error) {
      if (!isCurrent(session) || foreground !== token) return;
      root.dataset.flashing = "false";
      root.dataset.finished = "false";
      setProgress(null);
      setLive("stopped", "Stopped");
      appendStatusLine(`Flash failed: ${errorMessage(error)}`, "error");
      if (flashOperation?.activationStarted) {
        appendStatusLine(`The OTA selector was being written; ${version} may already be selected. Press Reset and read the version in the log`, "warning");
      }
      // A write that finishes after its deadline has still selected the new app.
      flashOperation?.settled.then((outcome) => {
        if (outcome.value && isCurrent(session)) {
          appendStatusLine(`Firmware ${version} was written completely after all; press Reset to start it`, "warning");
        }
      });
    } finally {
      if (foreground === token) foreground = null;
      syncControls();
    }
    // Follow the boot of the new firmware, or of whatever a failed install left, on the same port.
    if (!followLog || !isCurrent(session)) return;
    if (installed) bootCheck = { session, version, timer: null, pending: "" };
    if (serialPortIsOpen(session.port) && !serialPortLease(session.port)) await startMonitor();
  };
  // The Disconnect button also unpairs: after the port is closed, the browser forgets this page's
  // permission, so the next Connect asks again. Unplugging the device only disconnects.
  const portReleased = (port) => new Promise((resolve) => {
    if (!serialPortLease(port)) return resolve();
    let timer;
    const unsubscribe = subscribeSerialOwnership(() => {
      if (serialPortLease(port)) return;
      clearTimeout(timer);
      unsubscribe();
      resolve();
    });
    timer = setTimeout(() => { unsubscribe(); resolve(); }, PORT_RELEASE_WAIT_MS);
  });
  const disconnectAndUnpair = async () => {
    const port = current?.port;
    await disconnect();
    if (!port || !canForget) return;
    await portReleased(port);
    try {
      await forgetSerialPort(serial, port, { controller: { isReleaseBlocked: () => Boolean(foreground) } });
      appendStatusLine("Port unpaired");
    } catch (error) {
      const feedback = releaseFeedback(error);
      appendStatusLine(feedback.kind === "error" ? `Unpair failed: ${feedback.message}` : `Unpair skipped: ${feedback.message}`,
        feedback.kind === "error" ? "error" : "status");
    }
  };
  // Paired means connected: after a reload or a replug, the one port the browser still lists as
  // paired is reconnected, so the button shows the browser's state. Several paired ports stay for
  // the chooser.
  // connect() ignores this while connected or busy; after a failed check, a replug tries again.
  const reconnectPaired = async () => {
    if (!serialSupported) return;
    let ports = [];
    try {
      ports = typeof serial.getPorts === "function" ? Array.from(await serial.getPorts()) : [];
    } catch (_error) {
      return;
    }
    if (ports.length !== 1) return;
    const [port] = ports;
    // The check itself is the only open; a busy port is retried quietly and gets one log line.
    for (let attempt = 1; attempt <= PAIRED_OPEN_ATTEMPTS; attempt++) {
      if (current?.compatible || foreground || serialPortLease(port)) return;
      if (await connect({ paired: port, quietBusy: attempt < PAIRED_OPEN_ATTEMPTS }) !== PORT_BUSY) return;
      if (attempt < PAIRED_OPEN_ATTEMPTS) await delay(PAIRED_OPEN_RETRY_MS);
    }
  };
  connectButton.addEventListener("click", () => connect());
  disconnectButton.addEventListener("click", disconnectAndUnpair);
  resetButton.addEventListener("click", reset);
  flashButton.addEventListener("click", () => install());
  cleanFlashButton.addEventListener("click", () => install({ eraseFirst: true }));
  if (typeof serial?.addEventListener === "function") {
    serial.addEventListener("connect", () => {
      if (now() - lastPortActivity < RECONNECT_GRACE_MS) {
        if (serialSupported && !current?.compatible && !foreground) {
          appendStatusLine(`USB device reconnected within ${RECONNECT_GRACE_MS / 1000} s of its last use; click Connect to check it`);
        }
        return;
      }
      return reconnectPaired();
    });
    serial.addEventListener("disconnect", (event) => {
      const port = event.port || event.target;
      if (port === current?.port) {
        appendStatusLine("USB disconnected");
        return disconnect();
      }
    });
  }
  setDisconnected();
  if (!serialSupported) appendStatusLine("Web Serial unavailable (Chrome or Edge on a desktop required)", "error");

  const ready = Promise.resolve().then(async () => {
    const manifestUrl = new URL(manifestPath, locationImpl.href).toString();
    const loadedOffer = await loadFirmwareOffer({ manifestUrl, fetchImpl, cryptoImpl });
    // The offered partition tables tell whether an installed layout matches; fetch them before any
    // device is checked so the check itself never waits on the network.
    const tables = new Map();
    for (const build of loadedOffer.manifest.builds) {
      tables.set(build.chipFamily, await fetchPartitionTable(loadedOffer, build, fetchImpl));
    }
    offer = loadedOffer;
    manifest = offer.manifest;
    expectedTables = tables;
    versionValue.textContent = manifest.version;
    channelValue.textContent = channelLabel(manifest.version, locationImpl.pathname);
    sourceLink.textContent = manifest.sourceSha.slice(0, 12);
    sourceLink.href = `${SOURCE_COMMIT_URL}${manifest.sourceSha}`;
    sourceLink.title = `Source commit ${manifest.sourceSha}`;
    versionMeta.hidden = false;
    syncControls();
  }).catch((error) => {
    appendStatusLine(`Manifest error: ${errorMessage(error)}`, "error");
  });

  return {
    connect, reconnectPaired, disconnect, disconnectAndUnpair, reset, install, startMonitor, stopMonitor, ready,
    getSelectedPort: () => current?.port || null,
    isConnected: () => Boolean(current?.compatible)
  };
}
