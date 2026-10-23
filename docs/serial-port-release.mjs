// Serial-port ownership shared by every installer operation. A lease names the one operation that
// may touch a port; it is released only once native closure or hand-over is confirmed, otherwise it
// stays quarantined so no later operation can race a still-pending native call.
const portLeases = new WeakMap();
const releaseRequests = new WeakMap();
const ownershipListeners = new Set();
const SERIAL_PERMISSION_TIMEOUT_MS = 10000;

function namedError(name, message) {
  const error = new Error(message);
  error.name = name;
  return error;
}

function notifyOwnership() {
  for (const listener of ownershipListeners) {
    try { listener(); } catch (_error) {}
  }
}

export function subscribeSerialOwnership(listener) {
  ownershipListeners.add(listener);
  return () => ownershipListeners.delete(listener);
}

export function serialPortLease(port) {
  return port && portLeases.get(port);
}

export function acquireSerialPortLease(port, kind) {
  if (!port || serialPortLease(port)) {
    throw namedError("InvalidStateError", "Port busy: a previous operation is still closing.");
  }
  const lease = { port, kind, phase: kind };
  portLeases.set(port, lease);
  notifyOwnership();
  return lease;
}

export function updateSerialPortLease(lease, phase) {
  if (serialPortLease(lease.port) === lease) {
    lease.phase = phase;
    notifyOwnership();
  }
}

export function releaseSerialPortLease(lease) {
  if (serialPortLease(lease.port) === lease) {
    portLeases.delete(lease.port);
    notifyOwnership();
  }
}

export function serialPortIsOpen(port) {
  return Boolean(port && (port.readable != null || port.writable != null));
}

export function supportsSerialForget(serial, SerialPortCtor) {
  return Boolean(serial && typeof serial.getPorts === "function" &&
    typeof serial.requestPort === "function" && SerialPortCtor &&
    typeof SerialPortCtor.prototype?.forget === "function");
}

async function awaitNativeReleaseStep(request, call, description) {
  let timer;
  const native = Promise.resolve().then(call);
  timer = setTimeout(() => {
    request.failure = namedError("SerialPermissionTimeoutError",
      `${description} timed out. The native browser operation is still pending; no release is confirmed. Wait for it to settle before trying again.`);
    request.rejectForeground(request.failure);
    notifyOwnership();
  }, SERIAL_PERMISSION_TIMEOUT_MS);
  let result;
  try { result = await native; }
  finally { clearTimeout(timer); }
  if (request.failure) throw request.failure;
  return result;
}

async function nativeReleaseGrants(serial, request) {
  const ports = await awaitNativeReleaseStep(request, () => serial.getPorts(), "Reading serial-port permissions");
  return Array.isArray(ports) ? ports : Array.from(ports || []);
}

function checkReleaseAllowed(controller) {
  if (controller?.isReleaseBlocked()) {
    throw namedError("InvalidStateError", "Wait for the installer or reset operation and its cleanup to finish before releasing any serial port.");
  }
}

// Removes this page's permission for one port, which the browser lists as "paired". The port must
// still be granted and closed, and it is held under the shared native-port lease meanwhile, so no
// installer operation can open it while the permission is being removed.
export async function forgetSerialPort(serial, port, { controller = null } = {}) {
  if (releaseRequests.has(serial)) throw namedError("InvalidStateError", "A serial-port release is already in progress.");
  const request = {};
  const expired = new Promise((_, reject) => { request.rejectForeground = reject; });
  expired.catch(() => {});
  releaseRequests.set(serial, request);
  notifyOwnership();
  let lease;
  // This task keeps awaiting the real native promises after the foreground deadline. Its finally
  // owns the request and port lease; the raced caller cannot free them or start a second release.
  const actual = (async () => {
    try {
      if (typeof port?.forget !== "function") throw namedError("NotSupportedError", "This browser cannot forget serial ports.");
      lease = acquireSerialPortLease(port, "release");
      const granted = await nativeReleaseGrants(serial, request);
      checkReleaseAllowed(controller);
      if (!granted.includes(port)) throw namedError("NotFoundError", "This port is not paired with this page.");
      if (serialPortIsOpen(port)) throw namedError("InvalidStateError", "The serial port is still open. Stop its monitor or installer first.");
      await awaitNativeReleaseStep(request, () => port.forget(), "Removing the serial-port permission");
      return port;
    } finally {
      if (lease) releaseSerialPortLease(lease);
      if (releaseRequests.get(serial) === request) releaseRequests.delete(serial);
      notifyOwnership();
    }
  })();
  actual.catch(() => {});
  return Promise.race([actual, expired]);
}

export function releaseFeedback(error) {
  if (error?.name === "NotFoundError") return { kind: "info", message: "port not paired" };
  return { kind: "error", message: error?.message || "unknown error" };
}
