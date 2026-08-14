import assert from "node:assert/strict";
import test from "node:test";

import {
  attachSerialPortRelease,
  grantedSerialPorts,
  releaseFeedback,
  releaseSelectedSerialPort,
  supportsSerialForget
} from "../docs/serial-port-release.mjs";

class ForgetCapablePort {
  forget() {}
}

function fakeButton() {
  return {
    disabled: false,
    attributes: new Map(),
    listeners: new Map(),
    addEventListener(name, handler) { this.listeners.set(name, handler); },
    setAttribute(name, value) { this.attributes.set(name, value); },
    removeAttribute(name) { this.attributes.delete(name); }
  };
}

test("serial forget support requires permission discovery, the chooser and forget API", () => {
  const serial = { getPorts() {}, requestPort() {} };
  assert.equal(supportsSerialForget(serial, ForgetCapablePort), true);
  assert.equal(supportsSerialForget({ requestPort() {} }, ForgetCapablePort), false);
  assert.equal(supportsSerialForget({ getPorts() {} }, ForgetCapablePort), false);
  assert.equal(supportsSerialForget(serial, class {}), false);
});

test("granted serial ports are normalized to an array", async () => {
  const port = {};
  assert.deepEqual(await grantedSerialPorts({ async getPorts() { return [port]; } }), [port]);
  assert.deepEqual(await grantedSerialPorts({ async getPorts() { return new Set([port]); } }), [port]);
});

test("no chooser opens when this site has no granted port", async () => {
  let chooserOpened = false;
  const serial = {
    async getPorts() { return []; },
    async requestPort() { chooserOpened = true; }
  };

  await assert.rejects(releaseSelectedSerialPort(serial), { name: "NotFoundError" });
  assert.equal(chooserOpened, false);
});

test("a single granted closed port is released without opening the chooser", async () => {
  const calls = [];
  const port = {
    readable: null,
    writable: null,
    async forget() { calls.push("forget"); }
  };
  const serial = {
    async getPorts() {
      calls.push("get");
      return [port];
    },
    async requestPort() { calls.push("request"); }
  };

  await releaseSelectedSerialPort(serial);
  assert.deepEqual(calls, ["get", "forget"]);
});

test("the chooser disambiguates multiple previously granted ports", async () => {
  const calls = [];
  const first = { readable: null, writable: null, async forget() { calls.push("first"); } };
  const second = { readable: null, writable: null, async forget() { calls.push("second"); } };
  const serial = {
    async getPorts() { return [first, second]; },
    async requestPort() {
      calls.push("request");
      return second;
    }
  };

  await releaseSelectedSerialPort(serial);
  assert.deepEqual(calls, ["request", "second"]);
});

test("an open port is not released when its owner does not close it", async () => {
  let forgotten = false;
  const port = {
    readable: {},
    writable: {},
    async forget() { forgotten = true; }
  };
  const serial = {
    async getPorts() { return [port]; },
    async requestPort() { return port; }
  };

  await assert.rejects(releaseSelectedSerialPort(serial), { name: "InvalidStateError" });
  assert.equal(forgotten, false);
});

test("the selected installer port is disconnected before its permission is removed", async () => {
  const calls = [];
  const port = {
    readable: {},
    writable: {},
    async forget() { calls.push("forget"); }
  };
  const serial = {
    async getPorts() { calls.push("get"); return [port]; },
    async requestPort() { throw new Error("chooser must not open"); }
  };

  const releasedPort = await releaseSelectedSerialPort(serial, async (selectedPort) => {
    calls.push("disconnect");
    assert.equal(selectedPort, port);
    port.readable = null;
    port.writable = null;
  });

  assert.equal(releasedPort, port);
  assert.deepEqual(calls, ["get", "disconnect", "forget"]);
});

test("a failed disconnect prevents permission removal", async () => {
  let forgotten = false;
  const port = {
    readable: {},
    writable: {},
    async forget() { forgotten = true; }
  };
  const serial = {
    async getPorts() { return [port]; },
    async requestPort() { return port; }
  };
  const disconnectError = Object.assign(new Error("busy"), { name: "InvalidStateError" });

  await assert.rejects(
    releaseSelectedSerialPort(serial, async () => { throw disconnectError; }),
    disconnectError
  );
  assert.equal(forgotten, false);
});

test("cancelling the chooser is reported without an error state", () => {
  assert.deepEqual(releaseFeedback({ name: "NotFoundError" }), {
    kind: "info",
    message: "No port released."
  });
});

test("the UI stays hidden when this site has no granted port", async () => {
  const container = { hidden: false };
  const button = fakeButton();
  const status = { hidden: true, dataset: {}, textContent: "" };
  const serial = {
    async getPorts() { return []; },
    async requestPort() { throw new Error("chooser must not open"); }
  };

  assert.equal(await attachSerialPortRelease({
    serial,
    SerialPortCtor: ForgetCapablePort,
    container,
    button,
    status,
    refreshTarget: null
  }), true);
  assert.equal(container.hidden, true);
});

test("the UI refreshes when a granted device connects or disconnects", async () => {
  let ports = [];
  const serialListeners = new Map();
  const pageListeners = new Map();
  const container = { hidden: false };
  const button = fakeButton();
  const status = { hidden: true, dataset: {}, textContent: "" };
  const port = { readable: null, writable: null, async forget() {} };
  const serial = {
    async getPorts() { return ports; },
    async requestPort() { return port; },
    addEventListener(name, handler) { serialListeners.set(name, handler); }
  };
  const refreshTarget = {
    addEventListener(name, handler) { pageListeners.set(name, handler); }
  };

  await attachSerialPortRelease({
    serial,
    SerialPortCtor: ForgetCapablePort,
    container,
    button,
    status,
    refreshTarget
  });
  assert.equal(container.hidden, true);

  ports = [port];
  await serialListeners.get("connect")();
  assert.equal(container.hidden, false);

  ports = [];
  await serialListeners.get("disconnect")();
  assert.equal(container.hidden, true);

  ports = [port];
  await pageListeners.get("focus")();
  assert.equal(container.hidden, false);
});

test("the UI disconnects before release and disappears after releasing it", async () => {
  let granted = true;
  let releasedCallback = false;
  const calls = [];
  const port = {
    readable: {},
    writable: {},
    async forget() { calls.push("forget"); granted = false; }
  };
  const container = { hidden: true };
  const button = fakeButton();
  const status = { hidden: true, dataset: {}, textContent: "" };
  const serial = {
    async getPorts() { return granted ? [port] : []; },
    async requestPort() { return port; }
  };

  assert.equal(await attachSerialPortRelease({
    serial,
    SerialPortCtor: ForgetCapablePort,
    container,
    button,
    status,
    refreshTarget: null,
    async onBeforeRelease(selectedPort) {
      calls.push("disconnect");
      assert.equal(selectedPort, port);
      port.readable = null;
      port.writable = null;
    },
    async onReleased() { calls.push("released"); releasedCallback = true; }
  }), true);
  assert.equal(container.hidden, false);

  await button.listeners.get("click")();
  assert.equal(granted, false);
  assert.equal(releasedCallback, true);
  assert.equal(container.hidden, true);
  assert.equal(button.disabled, false);
  assert.equal(button.attributes.has("aria-busy"), false);
  assert.equal(status.dataset.kind, "success");
  assert.match(status.textContent, /no longer paired/);
  assert.deepEqual(calls, ["disconnect", "forget", "released"]);
});

test("the UI stays available when another granted port remains", async () => {
  let grantedPorts;
  const first = {
    readable: null,
    writable: null,
    async forget() { grantedPorts = [second]; }
  };
  const second = { readable: null, writable: null, async forget() {} };
  grantedPorts = [first, second];
  const container = { hidden: true };
  const button = fakeButton();
  const status = { hidden: true, dataset: {}, textContent: "" };
  const serial = {
    async getPorts() { return grantedPorts; },
    async requestPort() { return first; }
  };

  await attachSerialPortRelease({
    serial,
    SerialPortCtor: ForgetCapablePort,
    container,
    button,
    status,
    refreshTarget: null
  });
  await button.listeners.get("click")();

  assert.deepEqual(grantedPorts, [second]);
  assert.equal(container.hidden, false);
});
