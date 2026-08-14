function namedError(name, message) {
  const error = new Error(message);
  error.name = name;
  return error;
}

export function supportsSerialForget(serial, SerialPortCtor) {
  return Boolean(
    serial &&
    typeof serial.getPorts === "function" &&
    typeof serial.requestPort === "function" &&
    SerialPortCtor &&
    SerialPortCtor.prototype &&
    typeof SerialPortCtor.prototype.forget === "function"
  );
}

export async function grantedSerialPorts(serial) {
  const ports = await serial.getPorts();
  return Array.isArray(ports) ? ports : Array.from(ports || []);
}

// A single granted port can be released directly. Only ask the browser to identify the target
// when this site already has access to multiple ports; never open the chooser merely to discover
// that there is no permission to revoke.
export async function releaseSelectedSerialPort(serial) {
  const grantedPorts = await grantedSerialPorts(serial);
  if (grantedPorts.length === 0) {
    throw namedError("NotFoundError", "This site has no serial-port permission to remove.");
  }

  const port = grantedPorts.length === 1 ? grantedPorts[0] : await serial.requestPort();
  if (!port || typeof port.forget !== "function") {
    throw namedError("NotSupportedError", "This browser cannot forget serial ports.");
  }

  // The inline installer or serial monitor owns the streams while a port is open. Do not tear
  // those locked streams down from outside; after the operation closes them, forgetting is safe.
  // This also avoids disrupting a flash in progress.
  if (port.readable != null || port.writable != null) {
    throw namedError("InvalidStateError", "The serial port is still open.");
  }

  await port.forget();
}

export function releaseFeedback(error) {
  switch (error && error.name) {
    case "NotFoundError":
      return { kind: "info", message: "No port released." };
    case "InvalidStateError":
      return {
        kind: "error",
        message: "Close the installer dialog first, then release the serial port."
      };
    case "NotSupportedError":
      return {
        kind: "error",
        message: "This browser cannot remove serial-port permissions."
      };
    default:
      return {
        kind: "error",
        message: "Could not release the serial port. Close other serial applications and try again."
      };
  }
}

export async function attachSerialPortRelease({
  serial,
  SerialPortCtor,
  container,
  button,
  status,
  refreshTarget = globalThis,
  onReleased = null
}) {
  if (!container || !button || !status) {
    throw new Error("Serial port release controls are incomplete.");
  }
  if (!supportsSerialForget(serial, SerialPortCtor)) {
    container.hidden = true;
    return false;
  }

  const refreshVisibility = async () => {
    try {
      container.hidden = (await grantedSerialPorts(serial)).length === 0;
    } catch (_error) {
      container.hidden = true;
    }
    return !container.hidden;
  };
  const scheduleRefresh = () => refreshVisibility();

  // A grant can change while the native installer/chooser has focus, and a device can be attached
  // or removed without reloading the page. Re-check instead of preserving a stale visible button.
  if (typeof serial.addEventListener === "function") {
    serial.addEventListener("connect", scheduleRefresh);
    serial.addEventListener("disconnect", scheduleRefresh);
  }
  if (refreshTarget && typeof refreshTarget.addEventListener === "function") {
    refreshTarget.addEventListener("focus", scheduleRefresh);
    refreshTarget.addEventListener("pageshow", scheduleRefresh);
  }

  container.hidden = true;
  button.addEventListener("click", async () => {
    button.disabled = true;
    button.setAttribute("aria-busy", "true");
    status.hidden = false;
    status.dataset.kind = "info";
    status.textContent = "Releasing serial port…";

    try {
      await releaseSelectedSerialPort(serial);
      if (typeof onReleased === "function") await onReleased();
      status.dataset.kind = "success";
      status.textContent = "Serial port released — it is no longer paired with this site.";
      await refreshVisibility();
    } catch (error) {
      const feedback = releaseFeedback(error);
      status.dataset.kind = feedback.kind;
      status.textContent = feedback.message;
      await refreshVisibility();
    } finally {
      button.disabled = false;
      button.removeAttribute("aria-busy");
    }
  });

  await refreshVisibility();
  return true;
}
