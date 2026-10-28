import assert from "node:assert/strict";
import test from "node:test";

import {
  acquireSerialPortLease,
  forgetSerialPort,
  releaseFeedback,
  releaseSerialPortLease,
  serialPortIsOpen,
  serialPortLease,
  subscribeSerialOwnership,
  supportsSerialForget,
  updateSerialPortLease
} from "../docs/serial-port-release.mjs";

class ForgetCapablePort {
  forget() {}
}

test("serial forget support requires permission discovery, the chooser and forget API", () => {
  const serial = { getPorts() {}, requestPort() {} };
  assert.equal(supportsSerialForget(serial, ForgetCapablePort), true);
  assert.equal(supportsSerialForget({ requestPort() {} }, ForgetCapablePort), false);
  assert.equal(supportsSerialForget({ getPorts() {} }, ForgetCapablePort), false);
  assert.equal(supportsSerialForget(serial, class {}), false);
  assert.equal(supportsSerialForget(serial, undefined), false);
});

test("a port is open while either native stream exists", () => {
  assert.equal(serialPortIsOpen({ readable: null, writable: null }), false);
  assert.equal(serialPortIsOpen({ readable: {}, writable: null }), true);
  assert.equal(serialPortIsOpen({ readable: null, writable: {} }), true);
  assert.equal(serialPortIsOpen(null), false);
});

test("one lease per port: busy until released, and every change notifies subscribers", () => {
  const port = {};
  const seen = [];
  const unsubscribe = subscribeSerialOwnership(() => seen.push(serialPortLease(port)?.phase || "free"));
  const lease = acquireSerialPortLease(port, "probe");
  assert.deepEqual([lease.kind, lease.phase, serialPortLease(port)], ["probe", "probe", lease]);
  assert.throws(() => acquireSerialPortLease(port, "flash"), { name: "InvalidStateError", message: /^Port busy/ });
  updateSerialPortLease(lease, "cleanup");
  updateSerialPortLease(lease, "quarantined");
  releaseSerialPortLease(lease);
  assert.equal(serialPortLease(port), undefined);
  unsubscribe();
  acquireSerialPortLease(port, "monitor");
  assert.deepEqual(seen, ["probe", "cleanup", "quarantined", "free"], "an unsubscribed listener hears nothing more");
});

test("a stale lease can neither change nor release its successor", () => {
  const port = {};
  const first = acquireSerialPortLease(port, "probe");
  releaseSerialPortLease(first);
  const second = acquireSerialPortLease(port, "monitor");
  updateSerialPortLease(first, "quarantined");
  releaseSerialPortLease(first);
  assert.equal(serialPortLease(port), second);
  assert.equal(second.phase, "monitor");
  releaseSerialPortLease(second);
});

test("a listener that throws does not stop the others", () => {
  const port = {};
  let heard = 0;
  const stopFaulty = subscribeSerialOwnership(() => { throw new Error("faulty listener"); });
  const stopGood = subscribeSerialOwnership(() => { heard++; });
  releaseSerialPortLease(acquireSerialPortLease(port, "probe"));
  stopFaulty();
  stopGood();
  assert.equal(heard, 2);
});

test("a port is forgotten only while paired, closed, unowned and not blocked, under its own lease", async () => {
  const port = { readable: null, writable: null, forgotten: 0, leaseDuringForget: null };
  port.forget = async () => { port.forgotten++; port.leaseDuringForget = serialPortLease(port)?.kind; };
  const serial = { grants: [port], async getPorts() { return [...this.grants]; } };
  const blocked = { isReleaseBlocked: () => true };

  serial.grants = [];
  await assert.rejects(forgetSerialPort(serial, port), { name: "NotFoundError" });
  serial.grants = [port];
  port.readable = {};
  await assert.rejects(forgetSerialPort(serial, port), { name: "InvalidStateError", message: /still open/ });
  port.readable = null;
  const probe = acquireSerialPortLease(port, "probe");
  await assert.rejects(forgetSerialPort(serial, port), { name: "InvalidStateError", message: /^Port busy/ });
  releaseSerialPortLease(probe);
  await assert.rejects(forgetSerialPort(serial, port, { controller: blocked }), { name: "InvalidStateError" });
  assert.equal(port.forgotten, 0, "no refused request reached the browser");

  assert.equal(await forgetSerialPort(serial, port), port);
  assert.deepEqual([port.forgotten, port.leaseDuringForget], [1, "release"], "forgotten under the release lease");
  assert.equal(serialPortLease(port), undefined, "and the lease is returned afterwards");
});

test("granted ports may be any iterable, and a browser without forget is refused", async () => {
  const port = { readable: null, writable: null, async forget() {} };
  assert.equal(await forgetSerialPort({ async getPorts() { return new Set([port]); } }, port), port);
  await assert.rejects(forgetSerialPort({ async getPorts() { return [port]; } }, { readable: null, writable: null }),
    { name: "NotSupportedError" });
});

test("one release at a time, and a stalled browser call reports a timeout but keeps the port reserved", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout"] });
  let finishForget;
  const port = { readable: null, writable: null, forget: () => new Promise((resolve) => { finishForget = resolve; }) };
  const serial = { async getPorts() { return [port]; } };
  const releasing = forgetSerialPort(serial, port);
  for (let turn = 0; turn < 20 && !finishForget; turn++) await Promise.resolve();
  await assert.rejects(forgetSerialPort(serial, port), { name: "InvalidStateError", message: /already in progress/ });
  t.mock.timers.tick(10000);
  await assert.rejects(releasing, { name: "SerialPermissionTimeoutError", message: /no release is confirmed/ });
  assert.equal(serialPortLease(port)?.kind, "release", "the pending native call still owns the port");
  finishForget();
  for (let turn = 0; turn < 20 && serialPortLease(port); turn++) await Promise.resolve();
  assert.equal(serialPortLease(port), undefined);
});

test("release feedback treats an unpaired port as information", () => {
  assert.deepEqual(releaseFeedback({ name: "NotFoundError" }), { kind: "info", message: "port not paired" });
  assert.deepEqual(releaseFeedback(new Error("denied")), { kind: "error", message: "denied" });
  assert.deepEqual(releaseFeedback(undefined), { kind: "error", message: "unknown error" });
});
