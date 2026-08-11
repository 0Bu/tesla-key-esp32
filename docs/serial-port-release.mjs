function namedError(name, message) {
  const error = new Error(message);
  error.name = name;
  return error;
}

export function supportsSerialForget(serial, SerialPortCtor) {
  return Boolean(
    serial &&
    typeof serial.requestPort === "function" &&
    SerialPortCtor &&
    SerialPortCtor.prototype &&
    typeof SerialPortCtor.prototype.forget === "function"
  );
}

// The chooser is intentional: Web Serial exposes VID/PID, but not the human-readable port name.
// Letting the browser present its native names makes the target unambiguous before its permission
// is revoked. This flow never opens the selected port.
export async function releaseSelectedSerialPort(serial) {
  const port = await serial.requestPort();
  if (!port || typeof port.forget !== "function") {
    throw namedError("NotSupportedError", "This browser cannot forget serial ports.");
  }

  // ESP Web Tools owns and closes a port while its dialog is open. Do not try to tear its locked
  // streams down from outside; after the dialog is closed both attributes are null and forgetting
  // is safe. This also avoids disrupting a flash in progress.
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

export function attachSerialPortRelease({ serial, SerialPortCtor, container, button, status }) {
  if (!container || !button || !status) {
    throw new Error("Serial port release controls are incomplete.");
  }
  if (!supportsSerialForget(serial, SerialPortCtor)) {
    container.hidden = true;
    return false;
  }

  container.hidden = false;
  button.addEventListener("click", async () => {
    button.disabled = true;
    button.setAttribute("aria-busy", "true");
    status.hidden = false;
    status.dataset.kind = "info";
    status.textContent = "Select the serial port to release…";

    try {
      await releaseSelectedSerialPort(serial);
      status.dataset.kind = "success";
      status.textContent = "Serial port released — it is no longer paired with this site.";
    } catch (error) {
      const feedback = releaseFeedback(error);
      status.dataset.kind = feedback.kind;
      status.textContent = feedback.message;
    } finally {
      button.disabled = false;
      button.removeAttribute("aria-busy");
    }
  });
  return true;
}
