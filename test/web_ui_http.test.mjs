import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";
import vm from "node:vm";

const appSource = fs.readFileSync(new URL("../main/www/app.js", import.meta.url), "utf8");

function loadUi() {
  const elements = new Map();
  const element = (id) => {
    if (!elements.has(id)) {
      elements.set(id, {
        id,
        children: [],
        className: "",
        dataset: {},
        innerHTML: "",
        textContent: "",
        title: "",
        setAttribute(name, value) { this[name] = String(value); },
        appendChild(child) { this.children.push(child); child.parentNode = this; },
        removeChild(child) { this.children.splice(this.children.indexOf(child), 1); },
        classList: { add() {}, remove() {}, toggle() {} }
      });
    }
    return elements.get(id);
  };
  const context = {
    window: { __TESLA_UI_NO_BOOT__: true },
    document: {
      getElementById: element,
      querySelector() { return null; },
      createElement() { return element(`created-${elements.size}`); }
    },
    location: { reload() {} },
    fetch: async () => { throw new Error("fetch not stubbed"); },
    prompt() { return null; },
    confirm() { return false; },
    setTimeout() { return 1; },
    clearTimeout() {},
    setInterval() { return 1; },
    clearInterval() {},
    AbortController,
    console
  };
  vm.createContext(context);
  vm.runInContext(appSource, context, { filename: "main/www/app.js" });
  return { context, element };
}

test("requestJson rejects HTTP errors without parsing them as success", async () => {
  const { context } = loadUi();
  let parsed = false;
  context.fetch = async () => ({
    ok: false,
    status: 503,
    async json() { parsed = true; return {}; }
  });

  await assert.rejects(context.requestJson("/set_vin"), /HTTP 503/);
  assert.equal(parsed, false);
});

test("requestJsonWithTimeout rejects a hung HTTP request", async () => {
  const { context } = loadUi();
  context.fetch = () => new Promise(() => {});
  context.setTimeout = (callback) => { queueMicrotask(callback); return 1; };
  await assert.rejects(context.requestJsonWithTimeout("/ota/status", {}, 1), /timed out/);
});

test("configuration network failure is reported as failure, never saved", async () => {
  const { context } = loadUi();
  const messages = [];
  context.state = { vin: "UNKNOWN" };
  context.prompt = () => "5YJ3E1EA1JF000001";
  context.fetch = async () => { throw new Error("offline"); };
  context.toast = (message, kind) => messages.push({ message, kind });

  await context.editVin();

  assert.equal(messages.at(-1).kind, "err");
  assert.match(messages.at(-1).message, /no change was confirmed/);
  assert.doesNotMatch(messages.at(-1).message, /saved/i);
});

test("configuration success requires the command response schema", async () => {
  const { context } = loadUi();
  const messages = [];
  context.state = { vin: "UNKNOWN" };
  context.prompt = () => "5YJ3E1EA1JF000001";
  context.fetch = async () => ({ ok: true, status: 200, async json() { return { response: { result: "true", reason: "saved" } }; } });
  context.toast = (message, kind) => messages.push({ message, kind });

  await context.editVin();

  assert.equal(messages.at(-1).kind, "err");
  assert.doesNotMatch(messages.at(-1).message, /saved/i);
});

test("key generation requires a successful result schema", async () => {
  const { context } = loadUi();
  const messages = [];
  context.state = { key_present: false };
  context.toast = (message, kind) => messages.push({ message, kind });
  context.fetch = async () => ({
    ok: true,
    status: 200,
    async json() { return { result: false, reason: "NVS write failed" }; }
  });

  await context.genKey();

  assert.deepEqual(messages.at(-1), { message: "NVS write failed", kind: "err" });
});

test("failed OTA check cannot turn an idle status into up-to-date", async () => {
  const { context, element } = loadUi();
  context.fetch = async () => ({
    ok: false,
    status: 503,
    async json() { return { state: "idle", update_available: false }; }
  });

  await context.otaCheck();

  assert.equal(context.otaBusy, false);
  assert.match(element("otaStat").innerHTML, /check failed/);
  assert.doesNotMatch(element("otaStat").innerHTML, /up to date/);
});

test("OTA status schema, deadline and missed-done idle recovery fail closed", async () => {
  const { context, element } = loadUi();
  context.fetch = async () => ({ ok: true, status: 200, async json() { return { state: "mystery" }; } });
  await assert.rejects(context.otaStatus(), /invalid OTA status response/);

  context.otaBusy = true;
  context.otaPhase = "update";
  context.otaDeadline = Date.now() - 1;
  context.otaPoll();
  assert.match(element("otaStat").innerHTML, /update timed out/);

  let rebootArgs = null;
  context.waitReboot = (expected) => { rebootArgs = { expected }; };
  context.state = { version: "1.4.75" };
  context.otaBusy = true;
  context.otaPhase = "update";
  context.otaExpectedVersion = "1.4.76";
  context.otaProgress({ state: "idle", update_available: false });
  assert.deepEqual(rebootArgs, { expected: "1.4.76" });
  assert.match(element("otaStat").innerHTML, /verifying/);
});

test("OTA versions use the canonical 31-byte firmware descriptor grammar", () => {
  const { context } = loadUi();
  assert.equal(context.otaVersion("1.4.75"), true);
  assert.equal(context.otaVersion("1.4.75-PR-123"), true);
  assert.equal(context.otaVersion("01.4.75"), false);
  assert.equal(context.otaVersion(`1.4.75-${"x".repeat(25)}`), false);
});

test("device page exposes keyboard and live-region semantics", () => {
  const html = fs.readFileSync(new URL("../main/www/index.html", import.meta.url), "utf8");
  assert.match(html, /<button[^>]+id="verLink"[^>]+aria-label="Check for firmware updates"/);
  assert.match(html, /id="otaStat"[^>]+role="status"[^>]+aria-live="polite"/);
  assert.match(html, /id="toasts"[^>]+role="status"[^>]+aria-live="polite"/);
});

test("setup form enforces the shared WiFi credential contract without optimistic success", () => {
  const html = fs.readFileSync(new URL("../main/www/setup.html", import.meta.url), "utf8");
  assert.match(html, /id="pass"[^>]+maxlength="64"/);
  assert.match(html, /8–63 UTF-8 bytes or a 64-digit hexadecimal PSK/);
  assert.match(html, /pb===64&&\/\^\[0-9a-f\]\{64\}\$\/i/);
  assert.match(html, /Recovery setup preserves an existing VIN:[\s\S]*use Change VIN on the device page/);
  assert.doesNotMatch(html, /setTimeout\(function\(\)\{ \$\("form"\)\.classList\.add\('hide'\)/);
});
