import assert from "node:assert/strict";
import test from "node:test";

import {
  attachSerialPortRelease,
  releaseFeedback,
  releaseSelectedSerialPort,
  supportsSerialForget
} from "../docs/serial-port-release.mjs";

class ForgetCapablePort {
  forget() {}
}

test("serial forget support requires the chooser and forget API", () => {
  assert.equal(supportsSerialForget({ requestPort() {} }, ForgetCapablePort), true);
  assert.equal(supportsSerialForget({}, ForgetCapablePort), false);
  assert.equal(supportsSerialForget({ requestPort() {} }, class {}), false);
});

test("the explicitly selected closed port is released", async () => {
  const calls = [];
  const port = {
    readable: null,
    writable: null,
    async forget() { calls.push("forget"); }
  };
  const serial = {
    async requestPort() {
      calls.push("request");
      return port;
    }
  };

  await releaseSelectedSerialPort(serial);
  assert.deepEqual(calls, ["request", "forget"]);
});

test("an open port is not interrupted or released", async () => {
  let forgotten = false;
  const serial = {
    async requestPort() {
      return {
        readable: {},
        writable: {},
        async forget() { forgotten = true; }
      };
    }
  };

  await assert.rejects(releaseSelectedSerialPort(serial), { name: "InvalidStateError" });
  assert.equal(forgotten, false);
});

test("cancelling the chooser is reported without an error state", () => {
  assert.deepEqual(releaseFeedback({ name: "NotFoundError" }), {
    kind: "info",
    message: "No port released."
  });
});

test("the UI exposes the control and reports a released port", async () => {
  let click;
  let forgotten = false;
  const container = { hidden: true };
  const button = {
    disabled: false,
    attributes: new Map(),
    addEventListener(name, handler) { if (name === "click") click = handler; },
    setAttribute(name, value) { this.attributes.set(name, value); },
    removeAttribute(name) { this.attributes.delete(name); }
  };
  const status = { hidden: true, dataset: {}, textContent: "" };
  const serial = {
    async requestPort() {
      return {
        readable: null,
        writable: null,
        async forget() { forgotten = true; }
      };
    }
  };

  assert.equal(attachSerialPortRelease({
    serial,
    SerialPortCtor: ForgetCapablePort,
    container,
    button,
    status
  }), true);
  assert.equal(container.hidden, false);

  await click();
  assert.equal(forgotten, true);
  assert.equal(button.disabled, false);
  assert.equal(button.attributes.has("aria-busy"), false);
  assert.equal(status.dataset.kind, "success");
  assert.match(status.textContent, /no longer paired/);
});
