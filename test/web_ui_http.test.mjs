import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";
import vm from "node:vm";

const appSource = fs.readFileSync(new URL("../main/www/app.js", import.meta.url), "utf8");

function loadUi() {
  const elements = new Map();
  const element = (id) => {
    if (!elements.has(id)) {
      const classes = new Set();
      elements.set(id, {
        id,
        children: [],
        className: "",
        dataset: {},
        innerHTML: "",
        textContent: "",
        title: "",
        style: {},
        setAttribute(name, value) { this[name] = String(value); },
        appendChild(child) { this.children.push(child); child.parentNode = this; },
        removeChild(child) { this.children.splice(this.children.indexOf(child), 1); },
        focus() { if (context.document) context.document.activeElement = this; },
        contains(other) {
          if (!other) return false;
          if (other === this) return true;
          return this.children.some(c => (c.contains ? c.contains(other) : c === other));
        },
        classList: {
          add(c) { classes.add(c); },
          remove(c) { classes.delete(c); },
          toggle(c, force) { if (force ?? !classes.has(c)) classes.add(c); else classes.delete(c); },
          contains(c) { return classes.has(c); }
        },
        querySelector(sel) {
          if (!this._subs) this._subs = new Map();
          if (!this._subs.has(sel)) {
            this._subs.set(sel, { innerHTML: "", textContent: "", className: "" });
          }
          return this._subs.get(sel);
        }
      });
    }
    return elements.get(id);
  };
  const context = {
    window: { __TESLA_UI_NO_BOOT__: true },
    document: {
      activeElement: null,
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

test("requestJsonResult returns status, ok and parsed JSON even on HTTP error", async () => {
  const { context } = loadUi();
  context.fetch = async () => ({
    ok: false,
    status: 503,
    async json() { return { result: false, reason: "gate held" }; }
  });

  const res = await context.requestJsonResult("/gen_keys");
  assert.equal(res.ok, false);
  assert.equal(res.status, 503);
  assert.deepEqual(res.json, { result: false, reason: "gate held" });
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
  context.confirm = () => true;
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
  context.confirm = () => true;
  context.fetch = async () => ({ ok: true, status: 200, async json() { return { response: { result: "true", reason: "saved" } }; } });
  context.toast = (message, kind) => messages.push({ message, kind });

  await context.editVin();

  assert.equal(messages.at(-1).kind, "err");
  assert.doesNotMatch(messages.at(-1).message, /saved/i);
});

test("VIN change requires confirmation when key is present or unknown", async () => {
  const { context } = loadUi();
  let confirmed = false;
  let fetchCalled = false;
  context.state = { vin: "UNKNOWN" };
  context.prompt = () => "5YJ3E1EA1JF000001";
  context.confirm = () => { confirmed = true; return false; };
  context.fetch = async () => { fetchCalled = true; return { ok: true, async json() { return {}; } }; };

  await context.editVin();

  assert.equal(confirmed, true);
  assert.equal(fetchCalled, false);
});

test("VIN change skips confirmation when key is known absent", async () => {
  const { context } = loadUi();
  let confirmCalled = false;
  let fetchCalled = false;
  context.state = { vin: "UNKNOWN", key_present: false };
  context.prompt = () => "5YJ3E1EA1JF000001";
  context.confirm = () => { confirmCalled = true; return true; };
  context.fetch = async () => {
    fetchCalled = true;
    return { ok: true, async json() { return { response: { result: true, reason: "saved" } }; } };
  };

  await context.editVin();

  assert.equal(confirmCalled, false);
  assert.equal(fetchCalled, true);
});

test("key generation requires confirmation and omits force when state is null", async () => {
  const { context } = loadUi();
  let confirmed = false;
  let requestedUrl = null;
  context.state = null;
  context.confirm = () => { confirmed = true; return true; };
  context.fetch = async (url) => {
    requestedUrl = url;
    return {
      ok: false,
      status: 409,
      async json() {
        return { result: false, reason: "a key already exists — regenerating un-pairs the vehicle; call /gen_keys?force=1 to replace it" };
      }
    };
  };
  const messages = [];
  context.toast = (message, kind) => messages.push({ message, kind });

  await context.genKey();

  assert.equal(confirmed, true);
  assert.equal(requestedUrl, "/gen_keys");
  assert.deepEqual(messages.at(-1), {
    message: "a key already exists — regenerating un-pairs the vehicle; call /gen_keys?force=1 to replace it",
    kind: "err"
  });
});

test("key generation aborts when user cancels confirmation", async () => {
  const { context } = loadUi();
  let fetchCalled = false;
  context.state = null;
  context.confirm = () => false;
  context.fetch = async () => { fetchCalled = true; return { ok: true, async json() { return { result: true }; } }; };

  await context.genKey();

  assert.equal(fetchCalled, false);
});

test("key generation sends force=1 only when key is known to be present", async () => {
  const { context } = loadUi();
  let requestedUrl = null;
  context.state = { key_present: true };
  context.confirm = () => true;
  context.fetch = async (url) => {
    if (url.startsWith("/gen_keys")) requestedUrl = url;
    return { ok: true, status: 200, async json() { return { result: true }; } };
  };

  await context.genKey();

  assert.equal(requestedUrl, "/gen_keys?force=1");
});

test("key generation skips confirmation and omits force when key is known absent", async () => {
  const { context } = loadUi();
  let confirmCalled = false;
  let requestedUrl = null;
  context.state = { key_present: false };
  context.confirm = () => { confirmCalled = true; return true; };
  context.fetch = async (url) => {
    if (url.startsWith("/gen_keys")) requestedUrl = url;
    return { ok: true, status: 200, async json() { return { result: true }; } };
  };

  await context.genKey();

  assert.equal(confirmCalled, false);
  assert.equal(requestedUrl, "/gen_keys");
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

test("header exposes update channel dropdown button and menu with accessible semantics", () => {
  const html = fs.readFileSync(new URL("../main/www/index.html", import.meta.url), "utf8");
  assert.match(html, /<button[^>]+id="chanBtn"[^>]+aria-label="Update channel"/);
  assert.match(html, /id="chanBtn"[^>]+aria-haspopup="true"/);
  assert.match(html, /id="chanBtn"[^>]+aria-expanded="false"/);
  assert.match(html, /id="chanMenu"[^>]+role="menu"/);
  assert.match(html, /id="optRelease"[^>]+role="menuitemradio"/);
  assert.match(html, /id="optRelease"[^>]+aria-checked="true"/);
  assert.match(html, /id="optDev"[^>]+role="menuitemradio"/);
  assert.match(html, /id="optDev"[^>]+aria-checked="false"/);
  assert.match(html, /id="verLink"[\s\S]*?id="chanBtn"/);
  assert.doesNotMatch(html, /id="hero"[\s\S]*?id="chanBtn"/);
});

test("update channel menu toggles and updates channel selection with OTA check", async () => {
  const { context, element } = loadUi();
  const fetchCalls = [];
  context.fetch = async (url, opts) => {
    fetchCalls.push({ url, opts });
    if (url === "/set_ota") return { ok: true, status: 200, async json() { return { ok: true }; } };
    if (url.startsWith("/ota/check")) return { ok: true, status: 200, async json() { return { started: true }; } };
    if (url.startsWith("/ota/status")) return {
      ok: true,
      status: 200,
      async json() {
        return {
          state: "idle",
          update_available: false,
          progress: 0,
          message: "up to date",
          available: "1.4.0",
          current: "1.4.0"
        };
      }
    };
    return { ok: true, status: 200, async json() { return {}; } };
  };

  const menu = element("chanMenu");
  const btn = element("chanBtn");
  menu.classList.add("hide");

  context.toggleChanMenu();
  assert.equal(menu.classList.contains("hide"), false);
  assert.equal(btn["aria-expanded"], "true");
  assert.equal(context.document.activeElement, element("optRelease"));

  context.toggleChanMenu();
  assert.equal(menu.classList.contains("hide"), true);
  assert.equal(btn["aria-expanded"], "false");
  assert.equal(context.document.activeElement, btn);

  // Keyboard navigation: ArrowDown on button opens menu
  context.handleChanBtnKey({ key: "ArrowDown", preventDefault() {} });
  assert.equal(menu.classList.contains("hide"), false);

  // ArrowDown/ArrowUp toggles focus between release and dev
  context.handleChanKey({ key: "ArrowDown", preventDefault() {} });
  assert.equal(context.document.activeElement, element("optDev"));
  context.handleChanKey({ key: "ArrowUp", preventDefault() {} });
  assert.equal(context.document.activeElement, element("optRelease"));

  // Escape closes menu and returns focus to button
  context.handleChanKey({ key: "Escape", preventDefault() {} });
  assert.equal(menu.classList.contains("hide"), true);
  assert.equal(context.document.activeElement, btn);

  // Re-selecting current channel (release) is a no-op
  const countBefore = fetchCalls.length;
  await context.setChannel("release");
  assert.equal(fetchCalls.length, countBefore, "no network calls when re-selecting current channel");

  // Select dev channel
  await context.setChannel("dev");
  assert.equal(context.otaChannel, "dev");
  assert.match(element("optDev").className, /sel/);
  assert.equal(element("optDev")["aria-checked"], "true");
  assert.doesNotMatch(element("optRelease").className, /sel/);
  assert.equal(element("optRelease")["aria-checked"], "false");
  assert.match(element("optDev").innerHTML, /✓/);

  // Check that POST /set_ota and /ota/check?channel=dev were requested
  const setOta = fetchCalls.find(c => c.url === "/set_ota");
  assert.ok(setOta, "POST /set_ota called");
  assert.equal(JSON.parse(setOta.opts.body).channel, "dev");

  const otaCheck = fetchCalls.find(c => c.url.startsWith("/ota/check"));
  assert.ok(otaCheck, "GET /ota/check called");
  assert.match(otaCheck.url, /\/ota\/check\?ms=\d+/);

  // Switching to release
  await context.setChannel("release");
  assert.equal(context.otaChannel, "release");
  assert.match(element("optRelease").className, /sel/);
  assert.equal(element("optRelease")["aria-checked"], "true");
  assert.doesNotMatch(element("optDev").className, /sel/);
  assert.equal(element("optDev")["aria-checked"], "false");
  assert.match(element("optRelease").innerHTML, /✓/);
});

test("render initializes channel state from running version or status ota channel", () => {
  const { context, element } = loadUi();

  // Release version initializes channel to 'release' and marks initial set
  context.render({ version: "1.4.100" });
  assert.equal(context.otaChannel, "release");
  assert.equal(context.otaChannelInitialSet, true);
  assert.equal(element("optRelease")["aria-checked"], "true");
  assert.equal(element("optDev")["aria-checked"], "false");

  // New instance for dev version
  const uiDev = loadUi();
  uiDev.context.render({ version: "1.4.100-dev.5" });
  assert.equal(uiDev.context.otaChannel, "dev");
  assert.equal(uiDev.context.otaChannelInitialSet, true);
  assert.equal(uiDev.element("optDev")["aria-checked"], "true");
  assert.equal(uiDev.element("optRelease")["aria-checked"], "false");

  // Explicit s.ota.channel takes precedence
  uiDev.context.render({ version: "1.4.100-dev.5", ota: { channel: "release" } });
  assert.equal(uiDev.context.otaChannel, "release");
  assert.equal(uiDev.element("optRelease")["aria-checked"], "true");
});

test("hero card remains visible when vehicle link is unreachable or unknown", () => {
  const { context, element } = loadUi();
  const hero = element("hero");

  // Paired + unreachable: hero must stay visible with vehicle unreachable status
  context.render({ paired: true, link: "unreachable", ble: { connected: false } });
  assert.equal(hero.classList.contains("hide"), false);
  assert.match(element("hlabel").innerHTML, /Vehicle unreachable/);

  // Paired + unknown: hero must stay visible with checking status
  context.render({ paired: true, link: "unknown", ble: { connected: false } });
  assert.equal(hero.classList.contains("hide"), false);
  assert.match(element("hlabel").innerHTML, /Checking status/);

  // Paired + idle: hero must stay visible and render last-known battery chips without error
  context.render({ paired: true, link: "idle", last: { usable_soc: 75 }, last_seen_s: 300, ble: { connected: false } });
  assert.equal(hero.classList.contains("hide"), false);
  assert.match(element("hlabel").innerHTML, /Parked/);
  assert.match(element("hstats").innerHTML, /75/);
});

test("render handles s.link === 'idle' without ReferenceError and sets chips", () => {
  const { context, element } = loadUi();
  context.render({
    link: "idle",
    paired: true,
    key_present: true,
    vin: "5YJSA1E21HF123456",
    last: { usable_soc: 75 },
    last_seen_s: 120
  });
  assert.match(element("hlabel").innerHTML, /Parked/);
  assert.match(element("hstats").innerHTML, /Battery/);
  assert.match(element("hstats").innerHTML, /75/);
  assert.match(element("hstats").innerHTML, /Idle/);
});

test("render displays safe mode banner when sys.safe_mode is active", () => {
  const { context, element } = loadUi();
  context.render({
    link: "idle",
    paired: false,
    key_present: true,
    vin: "5YJ3E1EA7KF000316",
    sys: { safe_mode: true }
  });
  const rb = element("reauthBanner");
  assert.equal(rb.classList.contains("show"), true);
  assert.match(rb.querySelector(".bt").innerHTML, /Safe Mode active/);
});

test("toggleCharge renders server rejection reason on HTTP 502", async () => {
  const { context } = loadUi();
  const messages = [];
  context.state = { vin: "5YJ3E1EA1JF000001", vehicle: { status: "Stopped" } };
  context.toast = (message, kind) => messages.push({ message, kind });
  context.fetch = async () => ({
    ok: false,
    status: 502,
    async json() {
      return { response: { result: false, reason: "action failed: complete" } };
    }
  });

  await context.toggleCharge();

  assert.equal(messages.at(-1).kind, "info");
  assert.equal(messages.at(-1).message, "Charging is already complete");
});

test("wakeCar renders server reason on HTTP 502", async () => {
  const { context } = loadUi();
  const messages = [];
  context.state = { vin: "5YJ3E1EA1JF000001" };
  context.toast = (message, kind) => messages.push({ message, kind });
  context.fetch = async () => ({
    ok: false,
    status: 502,
    async json() {
      return { response: { result: false, reason: "Car not reachable" } };
    }
  });

  await context.wakeCar();

  assert.equal(messages.at(-1).kind, "err");
  assert.equal(messages.at(-1).message, "Wake failed — Car not reachable");
});

test("editVin, editMqtt, editSyslog render server reason on HTTP 4xx/5xx", async () => {
  const { context } = loadUi();
  const messages = [];
  context.toast = (message, kind) => messages.push({ message, kind });

  // editVin HTTP 409
  context.state = { vin: "UNKNOWN" };
  context.prompt = () => "5YJ3E1EA1JF000001";
  context.confirm = () => true;
  context.fetch = async () => ({
    ok: false,
    status: 409,
    async json() {
      return { response: { result: false, reason: "key identity recovery is pending" } };
    }
  });
  await context.editVin();
  assert.equal(messages.at(-1).kind, "err");
  assert.equal(messages.at(-1).message, "key identity recovery is pending");

  // editMqtt HTTP 400
  context.state = { mqtt: { broker: "" } };
  context.prompt = () => "192.168.1.50:1883";
  context.fetch = async () => ({
    ok: false,
    status: 400,
    async json() {
      return { response: { result: false, reason: "broker refused connection" } };
    }
  });
  await context.editMqtt();
  assert.equal(messages.at(-1).kind, "err");
  assert.equal(messages.at(-1).message, "broker refused connection");

  // editSyslog HTTP 400
  context.state = { syslog: { host: "" } };
  context.prompt = () => "192.168.1.50:514";
  context.fetch = async () => ({
    ok: false,
    status: 400,
    async json() {
      return { response: { result: false, reason: "invalid syslog port" } };
    }
  });
  await context.editSyslog();
  assert.equal(messages.at(-1).kind, "err");
  assert.equal(messages.at(-1).message, "invalid syslog port");
});

test("getTargetPr extracts PR number from search query or hash and validates bounds", () => {
  const { context } = loadUi();
  assert.equal(context.getTargetPr(), 0);

  context.location.search = "?pr=326";
  assert.equal(context.getTargetPr(), 326);

  context.location.search = "?foo=bar&pr=42";
  assert.equal(context.getTargetPr(), 42);

  context.location.search = "?pr=0";
  assert.equal(context.getTargetPr(), 0);

  context.location.search = "?pr=-5";
  assert.equal(context.getTargetPr(), 0);

  context.location.search = "?pr=abc";
  assert.equal(context.getTargetPr(), 0);

  context.location.search = "";
  context.location.hash = "#326";
  assert.equal(context.getTargetPr(), 326);

  context.location.hash = "#pr=123";
  assert.equal(context.getTargetPr(), 123);

  context.location.hash = "#invalid";
  assert.equal(context.getTargetPr(), 0);
});

test("OTA check and render incorporate target PR when set", async () => {
  const { context, element } = loadUi();
  context.location.search = "?pr=326";

  context.render({
    ip: "192.0.2.100",
    version: "1.5.4",
    key_present: false
  });

  const vl = element("verLink");
  assert.equal(vl.textContent, "v1.5.4 [PR #326]");
  assert.equal(vl.title, "Tap to check for PR #326 updates");
  assert.equal(vl["aria-label"], "Check for PR #326 firmware updates");

  let requestedUrl = null;
  context.fetch = async (url) => {
    requestedUrl = url;
    return {
      ok: true,
      status: 200,
      async json() { return { started: true }; }
    };
  };

  context.otaCheckPoll = () => {};
  await context.otaCheck();
  assert.match(requestedUrl, /\/ota\/check\?ms=\d+&pr=326/);
});

test("loadOtaChangelog fetches /ota/changelog and returns text or empty string on 204", async () => {
  const { context } = loadUi();
  context.fetch = async (url) => {
    assert.equal(url, "/ota/changelog");
    return { ok: true, status: 200, async text() { return "Line 1\nLine 2"; } };
  };
  const text = await context.loadOtaChangelog();
  assert.equal(text, "Line 1\nLine 2");

  context.fetch = async () => ({ ok: true, status: 204, async text() { return ""; } });
  const empty = await context.loadOtaChangelog();
  assert.equal(empty, "");
});

test("askOtaInstall populates modal fields, changelog items, and resolves on closeOtaModal", async () => {
  const { context, element } = loadUi();
  const status = { current: "1.5.0", available: "1.5.1", channel: "release" };
  const notes = "• Feature A\n• Bugfix B";

  const decisionPromise = context.askOtaInstall(status, notes);

  assert.equal(element("otaModalTitle").textContent, "Firmware update");
  assert.equal(element("otaVersionLine").textContent, "v1.5.0 → v1.5.1");
  assert.equal(element("otaChannel").textContent, "Release");
  const list = element("otaChanges");
  assert.equal(list.children.length, 2);
  assert.equal(list.children[0].textContent, "• Feature A");
  assert.equal(list.children[1].textContent, "• Bugfix B");
  assert.equal(list.hidden, false);
  assert.equal(element("otaNoChanges").hidden, true);
  assert.equal(element("otaModal").classList.contains("hide"), false);

  context.closeOtaModal(true);
  const result = await decisionPromise;
  assert.equal(result, true);
  assert.equal(element("otaModal").classList.contains("hide"), true);
});

test("askOtaInstall handles empty changelog by showing no-changes hint", async () => {
  const { context, element } = loadUi();
  const status = { current: "1.5.0", available: "1.5.1", channel: "dev" };

  const decisionPromise = context.askOtaInstall(status, "");

  assert.equal(element("otaChannel").textContent, "Development");
  const list = element("otaChanges");
  assert.equal(list.children.length, 0);
  assert.equal(list.hidden, true);
  assert.equal(element("otaNoChanges").hidden, false);

  context.closeOtaModal(false);
  const result = await decisionPromise;
  assert.equal(result, false);
});
