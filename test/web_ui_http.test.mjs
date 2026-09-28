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
            this._subs.set(sel, {
              innerHTML: "",
              textContent: "",
              className: "",
              focus() { if (context.document) context.document.activeElement = this; }
            });
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
  context.askText = async () => "5YJ3E1EA1JF000001";
  context.askConfirm = async () => true;
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
  context.askText = async () => "5YJ3E1EA1JF000001";
  context.askConfirm = async () => true;
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
  context.askText = async () => "5YJ3E1EA1JF000001";
  context.askConfirm = async () => { confirmed = true; return false; };
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
  context.askText = async () => "5YJ3E1EA1JF000001";
  context.askConfirm = async () => { confirmCalled = true; return true; };
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
  context.askConfirm = async () => { confirmed = true; return true; };
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
  context.askConfirm = async () => false;
  context.fetch = async () => { fetchCalled = true; return { ok: true, async json() { return { result: true }; } }; };

  await context.genKey();

  assert.equal(fetchCalled, false);
});

test("key generation sends force=1 only when key is known to be present", async () => {
  const { context } = loadUi();
  let requestedUrl = null;
  context.state = { key_present: true };
  context.askConfirm = async () => true;
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
  context.askConfirm = async () => { confirmCalled = true; return true; };
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
  // #toasts is the single live region; #otaStat must not announce the same OTA text twice.
  assert.match(html, /id="otaStat"[^>]+aria-hidden="true"/);
  assert.doesNotMatch(html, /id="otaStat"[^>]+aria-live/);
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

test("firmware pane exposes the update channel as an accessible radio group", () => {
  const html = fs.readFileSync(new URL("../main/www/index.html", import.meta.url), "utf8");
  assert.match(html, /role="radiogroup"[^>]+aria-label="Update channel"/);
  assert.match(html, /id="optRelease"[^>]+role="radio"[^>]+aria-checked="true"/);
  assert.match(html, /id="optDev"[^>]+role="radio"[^>]+aria-checked="false"/);
  assert.match(html, /id="fwVer"[\s\S]*?id="verLink"[\s\S]*?id="optRelease"/);
});

test("section tabs switch the visible pane and mark the current tab", () => {
  const { context, element } = loadUi();
  context.setTab("fw");
  assert.equal(element("wrap")["data-tab"], "fw");
  assert.equal(element("tabFw")["aria-current"], "page");
  assert.equal(element("paneTitle").textContent, "Firmware");
  context.setTab("bogus");
  assert.equal(element("wrap")["data-tab"], "fw");
});

test("update channel control updates channel selection with OTA check", async () => {
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

  // Arrow keys move focus between the two options without selecting
  context.document.activeElement = element("optRelease");
  context.handleChanKey({ key: "ArrowRight", preventDefault() {} });
  assert.equal(context.document.activeElement, element("optDev"));
  context.handleChanKey({ key: "ArrowLeft", preventDefault() {} });
  assert.equal(context.document.activeElement, element("optRelease"));
  assert.equal(fetchCalls.length, 0, "focus movement alone never changes the channel");

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
  context.askText = async () => "5YJ3E1EA1JF000001";
  context.askConfirm = async () => true;
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
  context.askText = async () => "192.0.2.50:1883";
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
  context.askText = async () => "192.0.2.50:514";
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

test("a device running a PR build still checks its own channel unless a PR is chosen", async () => {
  const { context, element } = loadUi();
  context.render({ ip: "192.0.2.100", version: "1.6.0-PR-337", key_present: false });

  assert.equal(context.getTargetPr(), 0);
  assert.notEqual(context.getActiveChannel(), "pr");
  assert.equal(element("optPr").classList.contains("hide"), false, "PR Preview stays offered on a PR build");

  let requestedUrl = null;
  context.fetch = async (url) => {
    if (String(url).includes("/ota/check")) requestedUrl = url;
    return { ok: true, status: 200, async json() { return { started: true }; } };
  };
  context.otaCheckPoll = () => {};
  await context.otaCheck();
  assert.match(requestedUrl, /\/ota\/check\?ms=\d+$/, "unparameterized check reaches the next release");
});

test("broker and syslog validation mirror the firmware whitespace set and byte length", async () => {
  const { context, element } = loadUi();
  const check = async (open, value) => {
    const p = open();
    element("askInput").value = value;
    context.askValidate();
    const err = element("askErr").classList.contains("hide") ? "" : element("askErr").textContent;
    context.askClose(null);
    await p.catch(() => {});
    return err;
  };
  context.state = {};
  assert.match(await check(() => context.editMqtt(), "192.0.2.20:\t1883"), /spaces/);
  assert.match(await check(() => context.editSyslog(), "192.0.2.30\n:514"), /spaces/);
  // 60 two-byte characters: 60 UTF-16 units but 120+ bytes on the device
  assert.match(await check(() => context.editMqtt(), "\u00e4".repeat(60) + ":1883"), /too long/);
  assert.equal(await check(() => context.editMqtt(), "192.0.2.20:1883"), "");
  assert.equal(await check(() => context.editSyslog(), "192.0.2.30"), "");
});

test("OTA check and render incorporate target PR when set", async () => {
  const { context, element } = loadUi();
  context.location.search = "?pr=326";

  context.render({
    ip: "192.0.2.100",
    version: "1.5.4",
    key_present: false
  });

  assert.equal(element("fwVer").textContent, "v1.5.4");
  assert.equal(element("ipline").textContent, "192.0.2.100 · v1.5.4");
  assert.equal(element("verLink").classList.contains("hide"), false);

  let requestedUrl = null;
  context.fetch = async (url) => {
    if (String(url).includes("/ota/check")) requestedUrl = url;
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

test("askText resolves the entered text on submit and null on cancel", async () => {
  const { context, element } = loadUi();
  const typed = context.askText({ title: "Syslog server", value: "192.0.2.30:514" });
  assert.equal(element("askModal").classList.contains("hide"), false);
  assert.equal(element("askInput").value, "192.0.2.30:514");
  element("askInput").value = " 192.0.2.31:514 ";
  context.askSubmit();
  assert.equal(await typed, " 192.0.2.31:514 ");
  assert.equal(element("askModal").classList.contains("hide"), true);

  const cancelled = context.askText({ title: "MQTT broker" });
  context.askClose(context.askCancelValue);
  assert.equal(await cancelled, null);
});

test("askConfirm resolves true on confirm, false on cancel, and a new sheet cancels the old one", async () => {
  const { context } = loadUi();
  const yes = context.askConfirm({ title: "Regenerate the security key?" });
  context.askSubmit();
  assert.equal(await yes, true);

  const first = context.askConfirm({ title: "first" });
  const second = context.askText({ title: "second" });
  assert.equal(await first, false, "opening a second sheet settles the first as cancelled");
  context.askClose(context.askCancelValue);
  assert.equal(await second, null);
});

test("an open sheet or OTA dialog makes the page behind it inert until both are closed", async () => {
  const { context, element } = loadUi();
  element("askModal").classList.add("hide");
  element("otaModal").classList.add("hide");
  const answer = context.askText({ title: "MQTT broker" });
  assert.equal(element("wrap").inert, true, "Tab and screen readers stay inside the sheet");
  context.askOtaInstall({ current: "1.5.0", available: "1.5.1" }, "");
  context.askClose(context.askCancelValue);
  assert.equal(await answer, null);
  assert.equal(element("wrap").inert, true, "the OTA dialog is still open");
  context.closeOtaModal(false);
  assert.equal(element("wrap").inert, false);
  assert.equal(context.document.activeElement, element("verLink"), "focus returns once the page is live again");
});

test("an OTA dialog that pops up over an open sheet never takes its focus, and focus stays reachable", async () => {
  const { context, element } = loadUi();
  element("askModal").classList.add("hide");
  element("otaModal").classList.add("hide");
  element("wrap").appendChild(element("vinBtn"));
  element("vinBtn").focus();
  const answer = context.askText({ title: "Vehicle VIN" });
  assert.notEqual(context.document.activeElement, element("askInput"), "no auto-focus in the field");
  const card = element("askModal").querySelector(".modal-card");
  assert.equal(context.document.activeElement, card, "the modal card on top receives focus without auto-focusing the input");
  const decision = context.askOtaInstall({ current: "1.5.0", available: "1.5.1" }, "");
  assert.equal(context.document.activeElement, card, "the modal card on top keeps focus");
  context.askClose(context.askCancelValue);
  assert.equal(await answer, null);
  assert.equal(context.document.activeElement, element("otaInstall"), "not the inert page behind the dialog");
  context.closeOtaModal(false);
  assert.equal(await decision, false);
  assert.equal(context.document.activeElement, element("verLink"));
});

test("modal field selection selects content on first activation and preserves native caret on subsequent clicks", () => {
  const { context } = loadUi();
  let selected = 0;
  const selectableField = {
    matches: (sel) => sel.includes(".sheet input") || sel.includes(".modal-card input"),
    value: "5YJ3E1EA1JF000001",
    selectionStart: 17,
    selectionEnd: 17,
    select: () => {
      selected++;
      selectableField.selectionStart = 0;
      selectableField.selectionEnd = selectableField.value.length;
    },
    setSelectionRange: (start, end) => {
      selectableField.selectionStart = start;
      selectableField.selectionEnd = end;
    },
  };
  const otherField = {};
  const listeners = {};
  const doc = {
    activeElement: otherField,
    addEventListener: (name, fn) => { listeners[name] = fn; }
  };
  context.wireModalFieldSelection(doc);

  // First tap: inactive field -> pointerdown, focusin, click selects all
  listeners.pointerdown({ target: selectableField });
  doc.activeElement = selectableField;
  listeners.focusin({ target: selectableField });
  listeners.click({ target: selectableField });
  assert.equal(selected, 2, "activating an inactive modal field leaves complete content selected");

  // Second tap while active: preserves native caret placement
  listeners.pointerdown({ target: selectableField });
  listeners.click({ target: selectableField });
  assert.equal(selected, 2, "clicking an already-active modal field must not re-select");
  assert.equal(selectableField.selectionStart, selectableField.value.length,
    "previous selection collapses before native caret placement on second tap");
  assert.equal(selectableField.selectionEnd, selectableField.selectionStart,
    "second tap leaves a collapsed caret instead of full selection");

  // Once blurred, next tap is a new activation and selects all again
  doc.activeElement = otherField;
  listeners.pointerdown({ target: selectableField });
  doc.activeElement = selectableField;
  listeners.focusin({ target: selectableField });
  listeners.click({ target: selectableField });
  assert.equal(selected, 4, "first tap after blur selects complete content again");
});

test("a destructive confirmation starts on Cancel and an indeterminate OTA phase shows no fake progress", () => {
  const { context, element } = loadUi();
  context.askConfirm({ title: "Regenerate the security key?", destructive: true });
  assert.equal(context.document.activeElement, element("askCancel"));
  context.askClose(false);
  context.askConfirm({ title: "Keep going?" });
  assert.equal(context.document.activeElement, element("askOk"));
  context.askClose(false);

  context.otaInline("", "", "indet");
  assert.equal(element("otaBar").classList.contains("indet"), true);
  assert.equal(element("otaFill").style.width, undefined, "no static width is painted for an unknown amount");
  context.otaInline("", "", 42);
  assert.equal(element("otaBar").classList.contains("indet"), false);
  assert.equal(element("otaFill").style.width, "42%");
});

test("VIN change decides on confirmation from the state current after input", async () => {
  const { context } = loadUi();
  let confirmCalled = false;
  let fetchCalled = false;
  context.state = { vin: "UNKNOWN", key_present: true };
  context.askText = async () => { context.state = { vin: "UNKNOWN", key_present: false }; return "5YJ3E1EA1JF000001"; };
  context.askConfirm = async () => { confirmCalled = true; return false; };
  context.fetch = async () => {
    fetchCalled = true;
    return { ok: true, async json() { return { response: { result: true, reason: "saved" } }; } };
  };

  await context.editVin();

  assert.equal(confirmCalled, false, "no key any more, so nothing to confirm");
  assert.equal(fetchCalled, true);
});

test("firmware pane replaces fwHost with changelog section", () => {
  const html = fs.readFileSync(new URL("../main/www/index.html", import.meta.url), "utf8");
  assert.doesNotMatch(html, /id="fwHost"/);
  assert.match(html, /id="fwChangelog"/);
  assert.match(html, /id="fwChanges"[^>]+hidden/);
  assert.match(html, /id="fwNoChanges"[^>]+hidden/);
  assert.match(html, /id="optRelease"[\s\S]*?id="optDev"[\s\S]*?id="fwChangelog"/);
});

test("renderInstalledChangelog populates fwChanges and toggles fwNoChanges", () => {
  const { context, element } = loadUi();
  const list = element("fwChanges");
  const noChanges = element("fwNoChanges");

  context.renderInstalledChangelog("• Improved BLE pairing\n• Web UI enhancements");
  assert.equal(list.hidden, false);
  assert.equal(noChanges.hidden, true);
  assert.equal(list.children.length, 2);
  assert.equal(list.children[0].textContent, "• Improved BLE pairing");
  assert.equal(list.children[1].textContent, "• Web UI enhancements");

  context.renderInstalledChangelog("");
  assert.equal(list.hidden, true);
  assert.equal(noChanges.hidden, false);
  assert.equal(list.children.length, 0);
});

test("loadInstalledChangelog fetches remote changelog.json on release channel when matching version", async () => {
  const { context, element } = loadUi();
  let requestedUrl = null;
  context.fetch = async (url) => {
    requestedUrl = url;
    if (url === "https://0bu.github.io/tesla-key-esp32/changelog.json") {
      return {
        ok: true,
        status: 200,
        async json() {
          return { version: "1.6.0", changelog: "Release feature note" };
        }
      };
    }
    throw new Error("unexpected URL: " + url);
  };

  context.state = { version: "1.6.0" };
  context.otaChannel = "release";
  const text = await context.loadInstalledChangelog();
  assert.equal(requestedUrl, "https://0bu.github.io/tesla-key-esp32/changelog.json");
  assert.equal(text, "Release feature note");

  await context.updateInstalledChangelog(true);
  const list = element("fwChanges");
  assert.equal(list.hidden, false);
  assert.equal(list.children.length, 1);
  assert.equal(list.children[0].textContent, "Release feature note");
});

test("loadInstalledChangelog filters dev entries by running version on dev channel", async () => {
  const { context, element } = loadUi();
  context.fetch = async (url) => {
    if (url === "https://0bu.github.io/tesla-key-esp32/dev/changelog.json") {
      return {
        ok: true,
        status: 200,
        async json() {
          return {
            version: "1.5.9-dev.2",
            changelog: "v1.5.9-dev.1 — Dev note 1\nv1.5.9-dev.2 — Dev note 2"
          };
        }
      };
    }
    throw new Error("unexpected URL: " + url);
  };

  context.state = { version: "1.5.9-dev.1" };
  context.otaChannel = "dev";
  const text = await context.loadInstalledChangelog();
  assert.equal(text, "Dev note 1");

  await context.updateInstalledChangelog(true);
  const list = element("fwChanges");
  assert.equal(list.hidden, false);
  assert.equal(list.children.length, 1);
  assert.equal(list.children[0].textContent, "Dev note 1");
});

test("installed changelog shows only notes that exactly match the running version", async () => {
  const { context } = loadUi();
  const feed = {};
  context.fetch = async (url) => ({ ok: true, status: 200, async json() { return feed[url]; } });

  // Dev build absent from the dev feed: nothing, never the whole dev history
  feed["https://0bu.github.io/tesla-key-esp32/dev/changelog.json"] = {
    version: "1.5.9-dev.3", changelog: "v1.5.9-dev.2 — Dev note 2\nv1.5.9-dev.3 — Dev note 3"
  };
  context.state = { version: "1.5.9-dev.1" };
  assert.equal(await context.loadInstalledChangelog(), "");

  // Release feed for another version, or without a version: nothing
  feed["https://0bu.github.io/tesla-key-esp32/changelog.json"] = { version: "1.6.1", changelog: "Newer note" };
  context.state = { version: "1.6.0" };
  assert.equal(await context.loadInstalledChangelog(), "");
  feed["https://0bu.github.io/tesla-key-esp32/changelog.json"] = { changelog: "Unversioned note" };
  assert.equal(await context.loadInstalledChangelog(), "");
  feed["https://0bu.github.io/tesla-key-esp32/changelog.json"] = { version: "1.6.0", changelog: "Exact note" };
  assert.equal(await context.loadInstalledChangelog(), "Exact note");
});

test("the sticky wake toast resolves from the poll path once the car reports data", async () => {
  const { context, element } = loadUi();
  context.toast("Wake sent · waiting for the car…", "load", "wake");
  context.waking = true;
  context.settleWakeToast();
  assert.match(element("toasts").children[0].className, /\bload\b/, "still waiting while waking");

  context.waking = false;     // render() clears it when vehicle data arrives
  context.settleWakeToast();
  const t = element("toasts").children[0];
  assert.match(t.className, /\bok\b/);
  assert.match(t.innerHTML, /Car is awake/);

  // No pending wake toast: nothing is announced
  context.settleWakeToast();
  assert.equal(element("toasts").children.length, 1);
});

test("getInstalledChangelogUrl targets directory matching running version", () => {
  const { context } = loadUi();
  context.state = { version: "1.4.0-PR-42" };
  assert.equal(context.getInstalledChangelogUrl(), "https://0bu.github.io/tesla-key-esp32/PR/42/changelog.json");

  context.state = { version: "1.5.0-dev.1" };
  assert.equal(context.getInstalledChangelogUrl(), "https://0bu.github.io/tesla-key-esp32/dev/changelog.json");

  context.state = { version: "1.6.0" };
  assert.equal(context.getInstalledChangelogUrl(), "https://0bu.github.io/tesla-key-esp32/changelog.json");
});

test("loadInstalledChangelog does not fall back to /ota/changelog when remote fetch fails", async () => {
  const { context, element } = loadUi();
  context.fetch = async (url) => {
    if (url === "https://0bu.github.io/tesla-key-esp32/changelog.json") {
      throw new Error("offline");
    }
    if (url === "/ota/changelog") {
      return {
        ok: true,
        status: 200,
        async text() { return "Fallback local note"; }
      };
    }
    throw new Error("unexpected URL: " + url);
  };

  context.state = { version: "1.6.0" };
  const text = await context.loadInstalledChangelog();
  assert.equal(text, "");

  await context.updateInstalledChangelog(true);
  const list = element("fwChanges");
  assert.equal(list.hidden, true);
  assert.equal(element("fwNoChanges").hidden, false);
});

test("updateInstalledChangelog avoids duplicate fetch when key is unchanged unless forced", async () => {
  const { context } = loadUi();
  let fetchCount = 0;
  context.fetch = async () => {
    fetchCount++;
    return { ok: true, status: 200, async json() { return { version: "1.6.0", changelog: "Note" }; } };
  };

  context.state = { version: "1.6.0" };
  context.otaChannel = "release";

  await context.updateInstalledChangelog(false);
  assert.equal(fetchCount, 1);

  await context.updateInstalledChangelog(false);
  assert.equal(fetchCount, 1, "did not fetch second time because key was unchanged");

  await context.updateInstalledChangelog(true);
  assert.equal(fetchCount, 2, "fetched when forced");
});

test("installed changelog is not refetched every poll: no-match is cached, failures back off", async () => {
  const { context } = loadUi();
  let fetchCount = 0;
  let mode = "nomatch";
  context.fetch = async () => {
    fetchCount++;
    if (mode === "offline") throw new TypeError("offline");
    return { ok: true, status: 200, async json() { return { version: "1.6.1", changelog: "Other build" }; } };
  };

  // Read fine, but no notes for the running build: cached like a hit
  context.state = { version: "1.6.0" };
  assert.equal(await context.updateInstalledChangelog(false), "");
  await context.updateInstalledChangelog(false);
  assert.equal(fetchCount, 1);

  // Unreadable feed: one attempt, then quiet until the retry window passes
  mode = "offline";
  context.state = { version: "1.6.2" };
  await context.updateInstalledChangelog(false);
  await context.updateInstalledChangelog(false);
  await context.updateInstalledChangelog(false);
  assert.equal(fetchCount, 2);
  context.installedChangelogRetryAt = Date.now() - 1;
  await context.updateInstalledChangelog(false);
  assert.equal(fetchCount, 3);

  // Concurrent render frames share one in-flight fetch
  mode = "nomatch";
  context.state = { version: "1.6.3" };
  context.installedChangelogRetryAt = 0;
  const a = context.updateInstalledChangelog(false);
  const b = context.updateInstalledChangelog(false);
  await Promise.all([a, b]);
  assert.equal(fetchCount, 4);
});

test("render triggers updateInstalledChangelog and populates fwChanges", async () => {
  const { context, element } = loadUi();
  context.fetch = async (url) => {
    return {
      ok: true,
      status: 200,
      async json() {
        return { version: "1.6.0", changelog: "Line A\nLine B" };
      }
    };
  };

  context.render({
    version: "1.6.0",
    ota: { channel: "release" }
  });

  await new Promise(resolve => setTimeout(resolve, 10));

  const list = element("fwChanges");
  assert.equal(list.hidden, false);
  assert.equal(list.children.length, 2);
  assert.equal(list.children[0].textContent, "Line A");
  assert.equal(list.children[1].textContent, "Line B");
  assert.equal(element("fwNoChanges").hidden, true);
});

test("loadInstalledChangelog handles 204 and network errors gracefully", async () => {
  const { context, element } = loadUi();
  context.fetch = async () => ({ ok: true, status: 204, async text() { return ""; } });

  context.state = { version: "1.6.0" };
  context.otaChannel = "release";
  await context.updateInstalledChangelog(true);

  const list = element("fwChanges");
  const noChanges = element("fwNoChanges");
  assert.equal(list.hidden, true);
  assert.equal(noChanges.hidden, false);

  // Network error path
  context.fetch = async () => { throw new Error("network down"); };
  await context.updateInstalledChangelog(true);
  assert.equal(list.hidden, true);
  assert.equal(noChanges.hidden, false);

  // Proves F8: failure is not cached forever; once the retry window has passed the next render retries
  context.fetch = async () => ({
    ok: true,
    status: 200,
    async json() { return { version: "1.6.0", changelog: "Recovered changelog note" }; }
  });
  await context.updateInstalledChangelog(false);
  assert.equal(list.hidden, true, "no immediate refetch inside the retry window");
  context.installedChangelogRetryAt = Date.now() - 1;
  await context.updateInstalledChangelog(false);
  assert.equal(list.hidden, false);
  assert.equal(list.children.length, 1);
  assert.equal(list.children[0].textContent, "Recovered changelog note");
});

test("setChannel preserves installed changelog and does not reload on channel change", async () => {
  const { context, element } = loadUi();
  let changelogFetchCount = 0;
  context.fetch = async (url) => {
    if (url.includes("changelog.json")) {
      changelogFetchCount++;
      return { ok: true, status: 200, async json() { return { version: "1.6.0", changelog: "Installed note" }; } };
    }
    if (url === "/set_ota") {
      return { ok: true, status: 200, async json() { return { result: true }; } };
    }
    return { ok: true, status: 200, async json() { return {}; } };
  };
  context.otaCheck = () => Promise.resolve();

  context.state = { version: "1.6.0" };
  await context.updateInstalledChangelog(true);
  assert.equal(changelogFetchCount, 1);

  await context.setChannel("dev");
  assert.equal(context.otaChannel, "dev");
  assert.equal(changelogFetchCount, 1, "did not refetch changelog on channel change to dev");

  await context.setChannel("pr");
  assert.equal(changelogFetchCount, 1, "did not refetch changelog on channel change to pr");
});

test("toast enforces single popup policy, load spinner icon, in-place transition, and cleans up timers and keys", () => {
  const { context, element } = loadUi();
  const c = element("toasts");

  // Mock timer infrastructure to test the leaving lifecycle
  const timers = new Map();
  let nextId = 1;
  context.setTimeout = (fn, delay) => {
    const id = nextId++;
    timers.set(id, { fn, delay });
    return id;
  };
  context.clearTimeout = (id) => {
    timers.delete(id);
  };
  const fireTimers = (predicate) => {
    for (const [id, entry] of Array.from(timers.entries())) {
      if (!predicate || predicate(entry)) {
        timers.delete(id);
        entry.fn();
      }
    }
  };

  // Initial loading toast
  const t1 = context.toast("Checking for updates…", "load", "ota");
  assert.equal(c.children.length, 1);
  assert.match(t1.className, /toast load/);
  assert.match(t1.innerHTML, /class="otaspin"/);
  assert.match(t1.innerHTML, /Checking for updates…/);
  assert.equal(t1.dataset.key, "ota");

  // In-place transition to success
  const t2 = context.toast("Up to date", "ok", "ota");
  assert.equal(c.children.length, 1);
  assert.equal(t1, t2, "same DOM element updated in-place");
  assert.match(t2.className, /toast ok/);
  assert.match(t2.innerHTML, /✓/);
  assert.match(t2.innerHTML, /Up to date/);
  assert.equal(t2.dataset.key, "ota");

  // A different action replaces the popup smoothly
  const t3 = context.toast("Saving VIN…", "load", "vin");
  assert.equal(c.children.length, 1);
  assert.equal(t2, t3, "same single popup updated in-place for new action");
  assert.match(t3.className, /toast load/);
  assert.match(t3.innerHTML, /Saving VIN…/);
  assert.equal(t3.dataset.key, "vin");

  // Action completes successfully
  const t3b = context.toast("VIN saved", "ok", "vin");
  assert.equal(t3b, t3);

  // Action without key clears dataset.key on reused element
  const t4 = context.toast("Network restored", "ok");
  assert.equal(t4, t3);
  assert.equal(t4.dataset.key, undefined, "stale key is cleared when omitted");

  // Test leaving race condition:
  // 1) 3000ms timer fires -> t4 gets class 'leaving' and schedules 230ms removal timer
  fireTimers(e => e.delay === 3000);
  assert.equal(t4.classList.contains("leaving"), true);
  assert.equal(timers.size, 1);

  // 2) Rapid toast arrives during leaving animation
  const t5 = context.toast("Immediate new alert", "info");
  assert.equal(t5, t4, "reused during leaving");
  assert.equal(t5.classList.contains("leaving"), false);

  // 3) Fire any old timers (the old 230ms should have been cleared!)
  fireTimers(e => e.delay === 230);
  assert.equal(c.children.length, 1, "toast was not prematurely removed by cancelled leaving timer");
  assert.equal(t5.parentNode, c, "toast remains attached in DOM");

  // 4) New 3000ms and 230ms cycle completes normally
  fireTimers(e => e.delay === 3000);
  assert.equal(t5.classList.contains("leaving"), true);
  fireTimers(e => e.delay === 230);
  assert.equal(c.children.length, 0, "toast successfully removed after full cycle");
});

test("keyed toasts protect active loading toast from premature dismissal by unrelated toasts", () => {
  const { context, element } = loadUi();
  const c = element("toasts");

  const timers = new Map();
  let nextId = 1;
  context.setTimeout = (fn, delay) => {
    const id = nextId++;
    timers.set(id, { fn, delay });
    return id;
  };
  context.clearTimeout = (id) => {
    timers.delete(id);
  };
  const fireTimers = (predicate) => {
    for (const [id, entry] of Array.from(timers.entries())) {
      if (!predicate || predicate(entry)) {
        timers.delete(id);
        entry.fn();
      }
    }
  };

  // 1) Start sticky loading toast with key 'ota'
  const otaLoad = context.toast("Downloading update… 20%", "load", "ota");
  assert.equal(c.children.length, 1);
  assert.equal(otaLoad.dataset.key, "ota");
  assert.match(otaLoad.className, /toast load/);

  // 2) Unrelated transient toast arrives with key 'channel'
  const chanToast = context.toast("Update channel set to Development", "ok", "channel");
  // Sticky ota toast must NOT be overwritten or dismissed! Both coexist in #toasts.
  assert.equal(c.children.length, 2);
  assert.equal(otaLoad.parentNode, c, "ota loading toast preserved");
  assert.equal(chanToast.parentNode, c, "channel toast attached");
  assert.notEqual(otaLoad, chanToast);

  // 3) Channel toast auto-dismisses after 3000ms + 230ms leaving
  fireTimers(e => e.delay === 3000);
  assert.equal(chanToast.classList.contains("leaving"), true);
  assert.equal(otaLoad.classList.contains("leaving"), false, "ota toast not leaving");
  fireTimers(e => e.delay === 230);
  assert.equal(c.children.length, 1);
  assert.equal(c.children[0], otaLoad, "only sticky ota toast remains");

  // 4) Keyed progress update updates the sticky toast in-place
  const otaProgress = context.toast("Downloading update… 50%", "load", "ota");
  assert.equal(c.children.length, 1);
  assert.equal(otaProgress, otaLoad, "updated in-place");
  assert.match(otaProgress.innerHTML, /50%/);

  // 5) OTA finishes: in-place transition to success, then auto-dismiss
  const otaDone = context.toast("Updated to v1.6.0", "ok", "ota");
  assert.equal(otaDone, otaLoad);
  assert.match(otaDone.className, /toast ok/);
  fireTimers(e => e.delay === 3000);
  assert.equal(otaDone.classList.contains("leaving"), true);
  fireTimers(e => e.delay === 230);
  assert.equal(c.children.length, 0, "all toasts cleared after full cycle");
});

test("render during waking state stops waking spinner without firing toast side-effect", () => {
  const { context } = loadUi();
  let toastCalls = [];
  context.toast = (msg, type, key) => { toastCalls.push({ msg, type, key }); };

  context.waking = true;
  let cleared = false;
  context.wakeTimeout = 999;
  context.clearTimeout = (id) => { if (id === 999) cleared = true; };

  context.render({
    paired: true,
    vin: "5YJ3E1EB8NF123456",
    vehicle: {
      soc: 80,
      status: "Online"
    }
  });

  assert.equal(context.waking, false, "waking cleared");
  assert.equal(cleared, true, "wake timeout cleared");
  assert.equal(toastCalls.length, 0, "no toast fired during render");
});

test("otaCheck triggers load toast and resolves to up to date toast when idle", async () => {
  const { context, element } = loadUi();
  context.fetch = async (url) => {
    if (url.startsWith("/ota/check")) return { ok: true, status: 200, async json() { return { started: true }; } };
    if (url.startsWith("/ota/status")) return {
      ok: true, status: 200, async json() {
        return {
          state: "idle",
          update_available: false,
          progress: 0,
          message: "idle",
          available: "1.6.0",
          current: "1.6.0"
        };
      }
    };
    return { ok: true, status: 200, async json() { return {}; } };
  };

  await context.otaCheck();

  const toasts = element("toasts");
  assert.equal(toasts.children.length, 1);
  const toastEl = toasts.children[0];
  assert.match(toastEl.className, /toast ok/);
  assert.match(toastEl.innerHTML, /Firmware is up to date/);
});

test("index.html defines SVG symbols, harmonizes setup rows, and drops dead button labels", () => {
  const html = fs.readFileSync(new URL("../main/www/index.html", import.meta.url), "utf8");
  // SVG defs and symbols
  assert.match(html, /<svg style="display:none"[^>]*>\s*<defs>/);
  assert.match(html, /<symbol id="ic-bolt"/);
  assert.match(html, /<symbol id="ic-pencil"/);
  assert.match(html, /<symbol id="ic-car"/);
  assert.match(html, /<symbol id="ic-key"/);
  assert.match(html, /<symbol id="ic-wifi"/);
  assert.match(html, /<symbol id="ic-warn"/);
  assert.match(html, /<symbol id="ic-bt"/);

  // use href references
  assert.match(html, /<use href="#ic-bolt"/);
  assert.match(html, /<use href="#ic-pencil"/);
  assert.match(html, /<use href="#ic-car"/);
  assert.match(html, /<use href="#ic-key"/);
  assert.match(html, /<use href="#ic-wifi"/);

  // Dead text spans removed
  assert.doesNotMatch(html, /id="vinBtnTx"/);
  assert.doesNotMatch(html, /id="keyBtnTx"/);

  // Harmonized setup tab
  assert.match(html, /<section class="pane pane-setup card rows" aria-label="Setup">/);
  assert.match(html, /<div class="row" id="rowVeh">[\s\S]*?id="vehVal"[\s\S]*?id="vinBtn"/);
  assert.match(html, /<div class="row" id="rowKey">[\s\S]*?id="keyVal"[\s\S]*?id="keyBtn"/);
});

test("askOpen uses adaptive focus: input on desktop pointer:fine, card container on mobile", async () => {
  const { context, element } = loadUi();
  let selected = false;
  element("askInput").select = () => { selected = true; };

  // Desktop environment: pointer: fine
  context.window.matchMedia = (query) => ({
    matches: query === "(pointer: fine)"
  });
  context.askText({ title: "Edit VIN", value: "5YJ3E1EA1JF000001" });
  assert.equal(context.document.activeElement, element("askInput"), "desktop focuses askInput");
  assert.equal(selected, true, "desktop selects askInput content immediately");
  context.askClose(null);

  // Touch/mobile environment: pointer: coarse / not fine
  selected = false;
  context.window.matchMedia = (query) => ({
    matches: false
  });
  const card = element("askModal").querySelector(".modal-card");
  context.askText({ title: "Edit VIN", value: "5YJ3E1EA1JF000001" });
  assert.equal(context.document.activeElement, card, "touch keeps container focus");
  assert.equal(selected, false, "touch does not select input text or jump keyboard");
  context.askClose(null);
});

test("otaProgress throttles download toasts to 10% milestones while maintaining smooth inline progress", () => {
  const { context, element } = loadUi();
  const c = element("toasts");

  context.otaBegin("update", 60000);

  // 0% -> initial download toast
  context.otaProgress({ state: "downloading", progress: 0 });
  assert.equal(c.children.length, 1);
  assert.match(c.children[0].innerHTML, /Downloading update… 0%/);
  assert.equal(element("otaFill").style.width, "0%");

  // 4% -> inline bar updates smoothly, toast does NOT update
  context.otaProgress({ state: "downloading", progress: 4 });
  assert.match(c.children[0].innerHTML, /Downloading update… 0%/);
  assert.equal(element("otaFill").style.width, "4%");

  // 8% -> toast still at 0%, inline bar at 8%
  context.otaProgress({ state: "downloading", progress: 8 });
  assert.match(c.children[0].innerHTML, /Downloading update… 0%/);
  assert.equal(element("otaFill").style.width, "8%");

  // 10% -> 10% milestone triggers toast update
  context.otaProgress({ state: "downloading", progress: 10 });
  assert.match(c.children[0].innerHTML, /Downloading update… 10%/);
  assert.equal(element("otaFill").style.width, "10%");

  // 17% -> toast still at 10%, inline bar at 17%
  context.otaProgress({ state: "downloading", progress: 17 });
  assert.match(c.children[0].innerHTML, /Downloading update… 10%/);
  assert.equal(element("otaFill").style.width, "17%");

  // 25% -> 20% milestone triggers toast update
  context.otaProgress({ state: "downloading", progress: 25 });
  assert.match(c.children[0].innerHTML, /Downloading update… 20%/);
  assert.equal(element("otaFill").style.width, "25%");

  // 105% (out-of-bounds progress) -> clamps to 100% milestone and width
  context.otaProgress({ state: "downloading", progress: 105 });
  assert.match(c.children[0].innerHTML, /Downloading update… 100%/);
  assert.equal(element("otaFill").style.width, "100%");

  // Phase transition: 'done'
  context.otaExpectedVersion = "1.6.0";
  context.otaProgress({ state: "done" });
  assert.match(c.children[0].innerHTML, /Verifying &amp; rebooting…/);
  assert.equal(element("otaFill").style.width, "100%");
});

test("firmware version formatting and header ipline display", () => {
  const { context, element } = loadUi();

  // Verbatim version formatting (F13: no invented suffixes)
  assert.equal(context.formatVerVerbatim("1.6.0"), "v1.6.0");
  assert.equal(context.formatVerVerbatim("v1.6.0"), "v1.6.0");
  assert.equal(context.formatVerVerbatim("1.5.9-dev.1"), "v1.5.9-dev.1");
  assert.equal(context.formatVerVerbatim("1.6.0-pr-333"), "v1.6.0-pr-333");
  assert.equal(context.formatVerVerbatim(""), "");
  assert.equal(context.formatVerVerbatim(null), "");

  // Header ipline rendering: <ip> · <version>
  context.render({
    ip: "192.0.2.1",
    version: "1.6.0",
    key_present: false
  });
  assert.equal(element("ipline").textContent, "192.0.2.1 · v1.6.0");
  assert.equal(element("fwVer").textContent, "v1.6.0");
});

test("PR preview FW is only offered when matching PRs exist, and inline list installs PR FW", async () => {
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
          update_available: context.activePr === 337,
          progress: 0,
          message: context.activePr === 337 ? "update available" : "up to date",
          available: context.activePr === 337 ? "1.6.0-PR-337" : "1.6.0",
          current: "1.6.0"
        };
      }
    };
    if (url === "https://0bu.github.io/tesla-key-esp32/PR/337/manifest.json") return { ok: true, status: 200, async json() { return { version: "1.6.0-PR-337" }; } };
    if (url === "https://0bu.github.io/tesla-key-esp32/PR/337/changelog.json") return { ok: true, status: 200, async text() { return "PR 337 changes"; } };
    if (url.startsWith("/ota/update")) return { ok: true, status: 200, async json() { return { result: true }; } };
    return { ok: true, status: 200, async json() { return {}; } };
  };

  // Initially, no PRs in prList and running release firmware: PR Preview is NOT offered
  context.prList = [];
  context.renderChanMenu();
  assert.equal(element("optPr").classList.contains("hide"), true, "optPr is hidden when no PRs exist");
  assert.equal(element("prListWrap").classList.contains("hide"), true);

  // Keyboard navigation only cycles through visible options (Release <-> Development)
  context.document.activeElement = element("optRelease");
  context.handleChanKey({ key: "ArrowRight", preventDefault() {} });
  assert.equal(context.document.activeElement, element("optDev"));
  context.handleChanKey({ key: "ArrowRight", preventDefault() {} });
  assert.equal(context.document.activeElement, element("optRelease"), "ArrowRight skips hidden optPr");

  // When matching PRs with preview builds exist, optPr is offered
  context.prList = [
    { number: 337, title: "Feature PR 337", ready: true }
  ];
  context.renderChanMenu();
  assert.equal(element("optPr").classList.contains("hide"), false, "optPr is visible when matching PR exists");

  // Keyboard navigation now cycles through all 3 options
  context.document.activeElement = element("optDev");
  context.handleChanKey({ key: "ArrowRight", preventDefault() {} });
  assert.equal(context.document.activeElement, element("optPr"));
  context.handleChanKey({ key: "ArrowRight", preventDefault() {} });
  assert.equal(context.document.activeElement, element("optRelease"));

  // Selecting PR Preview channel reveals inline list and keeps verLink visible
  await context.setChannel("pr");
  assert.equal(context.getActiveChannel(), "pr");
  assert.equal(element("optPr")["aria-checked"], "true");
  assert.match(element("optPr").className, /sel/);
  assert.equal(element("prListWrap").classList.contains("hide"), false);
  assert.equal(element("verLink").classList.contains("hide"), false, "verLink is visible in PR mode");

  // PR rows rendered with install button
  const rows = element("prRows").children;
  assert.equal(rows.length, 1);
  const row = rows[0];
  assert.equal(row.children[0].textContent, "#337");
  assert.equal(row.children[1].textContent, "Feature PR 337");
  const installBtn = row.children[2];
  assert.equal(installBtn.textContent, "Install");
  assert.equal(!installBtn.disabled, true);

  // Modal dialog #prModal is completely absent from DOM
  const indexHtml = fs.readFileSync(new URL("../main/www/index.html", import.meta.url), "utf8");
  assert.equal(indexHtml.includes('id="prModal"'), false, "prModal dialog has been removed from index.html");

  // Clicking Install triggers askOtaInstall modal
  context.state = { version: "1.6.0" };
  let installPromise = context.installPr(337);
  // Wait microtask
  await new Promise(r => setTimeout(r, 10));
  assert.equal(element("otaModal").classList.contains("hide"), false);
  assert.match(element("otaModalTitle").textContent, /PR #337/);
  assert.equal(element("otaVersionLine").textContent, "v1.6.0 → v1.6.0-PR-337");

  // Confirming OTA install kicks off POST /ota/update?pr=337 and waitReboot
  let rebootArgs = null;
  context.waitReboot = (expected) => { rebootArgs = { expected }; };
  context.closeOtaModal(true);
  await installPromise;
  const otaUpdateCall = fetchCalls.find(c => c.url.includes("/ota/update?pr=337"));
  assert.ok(otaUpdateCall, "POST /ota/update?pr=337 was called");
  assert.equal(context.otaExpectedVersion, "1.6.0-PR-337");
  assert.deepEqual(rebootArgs, { expected: "1.6.0-PR-337" });

  // Switching back to release clears activePr, hides prListWrap, restores verLink, and POSTs /set_ota
  fetchCalls.length = 0;
  await context.setChannel("release");
  assert.equal(context.activePr, 0);
  assert.equal(context.getActiveChannel(), "release");
  assert.equal(element("optRelease")["aria-checked"], "true");
  assert.equal(element("prListWrap").classList.contains("hide"), true);
  assert.equal(element("verLink").classList.contains("hide"), false);
  const setOtaRelease = fetchCalls.find(c => c.url === "/set_ota");
  assert.ok(setOtaRelease, "POST /set_ota requested on channel switch");
});

test("state preservation across periodic /status polls in PR preview mode", async () => {
  const { context, element } = loadUi();
  context.prList = [{ number: 337, title: "PR 337", ready: true }];
  context.activePr = 337;

  // Simulate periodic status poll reporting release channel from device
  context.render({
    ip: "192.0.2.1",
    version: "1.6.0",
    ota: { channel: "release" },
    key_present: true
  });

  // PR preview channel state remains decoupled and preserved
  assert.equal(context.getActiveChannel(), "pr");
  assert.equal(element("optPr")["aria-checked"], "true");
  assert.match(element("optPr").className, /sel/);
  assert.equal(element("optRelease")["aria-checked"], "false");
  assert.equal(element("fwVer").textContent, "v1.6.0");
  assert.equal(element("ipline").textContent, "192.0.2.1 · v1.6.0");

  // Now simulate page loaded with query param ?pr=42, user switching to release, and subsequent status poll
  context.location.search = "?pr=42";
  context.fetch = async () => ({ ok: true, status: 200, async json() { return { ok: true }; } });
  await context.setChannel("release");
  assert.equal(context.activePr, 0);
  assert.equal(context.getActiveChannel(), "release");

  // Subsequent background poll must NOT resurrect activePr=42 from location.search
  context.render({
    ip: "192.0.2.1",
    version: "1.6.0",
    ota: { channel: "release" },
    key_present: true
  });
  assert.equal(context.activePr, 0);
  assert.equal(context.getActiveChannel(), "release");
  assert.equal(element("optRelease")["aria-checked"], "true");
  assert.equal(element("optPr")["aria-checked"], "false");
});

test("adaptive action button #verLink and #fwBadge display based on otaAvail and activePr", () => {
  const { context, element } = loadUi();
  const vl = element("verLink");
  const tx = element("verLinkText");
  const fb = element("fwBadge");

  // Idle state without PR
  context.otaAvail = null;
  context.activePr = 0;
  context.renderVerLink();
  assert.equal(tx.textContent, "Check for updates");
  assert.equal(fb.textContent, "Up to date");
  assert.doesNotMatch(vl.className, /avail/);

  // Idle state with PR 337
  context.activePr = 337;
  context.renderVerLink();
  assert.equal(tx.textContent, "Check PR Updates");
  assert.equal(fb.textContent, "Up to date");
  assert.doesNotMatch(vl.className, /avail/);

  // Checking phase
  context.otaBusy = true;
  context.renderVerLink();
  assert.equal(fb.textContent, "Checking…");
  assert.equal(fb.className, "fw-badge busy");

  // Update available without PR
  context.otaBusy = false;
  context.activePr = 0;
  context.otaAvail = "1.7.0";
  context.renderVerLink();
  assert.equal(tx.textContent, "Install v1.7.0");
  assert.match(vl.className, /avail/);
  assert.equal(fb.textContent, "Update available");
  assert.equal(fb.className, "fw-badge avail");

  // Update available with PR 337
  context.activePr = 337;
  context.renderVerLink();
  assert.equal(tx.textContent, "Install PR #337");
  assert.match(vl.className, /avail/);
  assert.equal(fb.textContent, "Update available");

  // Live lifecycle transitions update badge directly without manual renderVerLink
  context.otaAvail = null;
  context.otaBegin("check", 10000);
  assert.equal(fb.textContent, "Checking…");
  assert.equal(fb.className, "fw-badge busy");

  context.otaBegin("update", 10000);
  assert.equal(fb.textContent, "Updating…");
  assert.equal(fb.className, "fw-badge busy");

  context.otaReset();
  assert.equal(fb.textContent, "Up to date");
  assert.equal(fb.className, "fw-badge ok");
});

test("PR discovery loads from GitHub API, caches in sessionStorage, and probes readiness", async () => {
  const { context, element } = loadUi();
  const storage = new Map();
  context.sessionStorage = {
    getItem(k) { return storage.get(k) ?? null; },
    setItem(k, v) { storage.set(k, String(v)); },
    removeItem(k) { storage.delete(k); },
    clear() { storage.clear(); }
  };

  const requested = [];
  context.fetch = async (url, opts) => {
    requested.push(url);
    let origin = "";
    try { origin = new URL(url).origin; } catch {}
    if (origin === "https://api.github.com") {
      return {
        ok: true,
        status: 200,
        async json() {
          return [
            { number: 101, title: "Feature A", updated_at: "2026-09-20T10:00:00Z", user: { login: "dev1" } },
            { number: 102, title: "Feature B", updated_at: "2026-09-21T10:00:00Z", user: { login: "dev2" } }
          ];
        }
      };
    }
    if (url === "https://0bu.github.io/tesla-key-esp32/PR/101/manifest.json") {
      return { ok: true, status: 200 };
    }
    if (url === "https://0bu.github.io/tesla-key-esp32/PR/102/manifest.json") {
      return { ok: false, status: 404 };
    }
    return { ok: false, status: 500 };
  };

  const list = await context.loadPrList(true);
  // Only PR 101 with ready preview build is offered; PR 102 (no build) is filtered out
  assert.equal(list.length, 1);
  assert.equal(list[0].number, 101);
  assert.equal(list[0].ready, true);

  // Cached in sessionStorage
  assert.ok(storage.has("tk_pr_cache"));
  // One API request plus one HEAD per listed PR; no request for an index CI never publishes
  assert.equal(requested.length, 3);
  assert.equal(requested.some((u) => u.endsWith("/PR/index.json")), false);

  // A plain 403 (forbidden, not a quota) is an error, not the rate-limit banner
  const headers = (remaining) => ({ get(k) { return k.toLowerCase() === "x-ratelimit-remaining" ? remaining : null; } });
  context.fetch = async () => ({ ok: false, status: 403, headers: headers("12") });
  await context.loadPrList(true);
  assert.equal(element("prRateLimitBanner").classList.contains("hide"), true);

  // Primary rate limit: 403 with an exhausted quota
  context.fetch = async () => ({ ok: false, status: 403, headers: headers("0") });
  await context.loadPrList(true);
  assert.equal(element("prRateLimitBanner").classList.contains("hide"), false);

  // Secondary rate limit: 429
  element("prRateLimitBanner").classList.add("hide");
  context.fetch = async () => ({ ok: false, status: 429, headers: headers(null) });
  await context.loadPrList(true);
  assert.equal(element("prRateLimitBanner").classList.contains("hide"), false);
});

test("channel switching is responsive and immune to intermediate polling race conditions", async () => {
  const { context, element } = loadUi();
  // Device starts on dev channel
  context.render({ version: "1.6.0-dev-1", ota: { channel: "dev" } });
  assert.equal(context.otaChannel, "dev");
  assert.equal(element("optDev")["aria-checked"], "true");
  assert.equal(element("optRelease")["aria-checked"], "false");

  // User taps Release: setChannel initiated
  let resolvePost;
  context.fetch = async (url) => {
    if (url === "/set_ota") {
      return new Promise((r) => {
        resolvePost = () => r({ ok: true, status: 200, async json() { return { response: { result: true } }; } });
      });
    }
    if (url.startsWith("/ota/check")) {
      return { ok: true, status: 200, async json() { return { started: true }; } };
    }
    if (url.startsWith("/ota/status")) {
      return { ok: true, status: 200, async json() { return { state: "idle" }; } };
    }
    return { ok: true, status: 200, async json() { return {}; } };
  };

  const channelPromise = context.setChannel("release");

  // Immediately, UI reflects release
  assert.equal(context.otaChannel, "release");
  assert.equal(context.otaChannelInFlight, true);
  assert.equal(element("optRelease")["aria-checked"], "true");
  assert.equal(element("optDev")["aria-checked"], "false");

  // Concurrent /status poll arrives from device still reporting dev
  context.render({ version: "1.6.0-dev-1", ota: { channel: "dev" } });

  // UI MUST NOT revert back to dev
  assert.equal(context.otaChannel, "release");
  assert.equal(element("optRelease")["aria-checked"], "true");
  assert.equal(element("optDev")["aria-checked"], "false");

  // POST /set_ota finishes
  resolvePost();
  await channelPromise;

  assert.equal(context.otaChannel, "release");
  assert.equal(context.otaChannelInFlight, false);

  // Subsequent status poll now reporting release clears target
  context.render({ version: "1.6.0-dev-1", ota: { channel: "release" } });
  assert.equal(context.otaChannel, "release");
  assert.equal(context.otaChannelTarget, null);
  assert.equal(element("optRelease")["aria-checked"], "true");
});

test("askModal live validation enforces valid input, shows errors, and blocks save", async () => {
  const { context, element } = loadUi();
  context.state = { vin: "5YJ3E1EA1JF000001", key_present: true };

  // 1. editVin validation
  let vinSaved = null;
  const vinPromise = context.editVin();

  // Initially opened with valid current VIN
  assert.equal(element("askModal").classList.contains("hide"), false);
  assert.equal(element("askInput").value, "5YJ3E1EA1JF000001");
  assert.equal(element("askOk").disabled, false);
  assert.equal(element("askErr").classList.contains("hide"), true);

  // User deletes characters -> 16 chars (invalid)
  element("askInput").value = "5YJ3E1EA1JF00000";
  context.askValidate();
  assert.equal(element("askOk").disabled, true);
  assert.equal(element("askErr").classList.contains("hide"), false);
  assert.match(element("askErr").textContent, /17 characters/i);
  assert.equal(element("askInput").classList.contains("invalid"), true);

  // Calling askSubmit() while invalid MUST NOT close or save
  context.askSubmit();
  assert.equal(element("askModal").classList.contains("hide"), false);

  // User enters invalid character (e.g. letter 'I')
  element("askInput").value = "5YJ3E1EA1JF00000I";
  context.askValidate();
  assert.equal(element("askOk").disabled, true);
  assert.equal(element("askErr").classList.contains("hide"), false);
  assert.match(element("askErr").textContent, /I, O, Q/i);

  // User enters valid new VIN
  element("askInput").value = "5YJ3E1EA1JF000001";
  context.askValidate();
  assert.equal(element("askOk").disabled, false);
  assert.equal(element("askErr").classList.contains("hide"), true);
  assert.equal(element("askInput").classList.contains("invalid"), false);

  // Close with cancel to clean up
  context.askClose(null);
  await vinPromise;
  assert.equal(element("askModal").classList.contains("hide"), true);
  assert.equal(element("askOk").disabled, false);

  // 2. editMqtt validation
  const mqttPromise = context.editMqtt();
  assert.equal(element("askModal").classList.contains("hide"), false);

  // Empty broker is valid (disables MQTT)
  element("askInput").value = "";
  context.askValidate();
  assert.equal(element("askOk").disabled, false);
  assert.equal(element("askErr").classList.contains("hide"), true);

  // Invalid broker with space
  element("askInput").value = "192.0.2.20 1883";
  context.askValidate();
  assert.equal(element("askOk").disabled, true);
  assert.equal(element("askErr").classList.contains("hide"), false);
  assert.match(element("askErr").textContent, /spaces not allowed/i);

  // Invalid broker missing port
  element("askInput").value = "192.0.2.20";
  context.askValidate();
  assert.equal(element("askOk").disabled, true);
  assert.equal(element("askErr").classList.contains("hide"), false);
  assert.match(element("askErr").textContent, /host:port/i);

  // Valid broker
  element("askInput").value = "192.0.2.20:1883";
  context.askValidate();
  assert.equal(element("askOk").disabled, false);
  assert.equal(element("askErr").classList.contains("hide"), true);

  context.askClose(null);
  await mqttPromise;

  // 3. editSyslog validation
  const syslogPromise = context.editSyslog();
  assert.equal(element("askModal").classList.contains("hide"), false);

  // Invalid scheme
  element("askInput").value = "udp://192.0.2.30:514";
  context.askValidate();
  assert.equal(element("askOk").disabled, true);
  assert.equal(element("askErr").classList.contains("hide"), false);
  assert.match(element("askErr").textContent, /no scheme/i);

  // Valid syslog
  element("askInput").value = "192.0.2.30:514";
  context.askValidate();
  assert.equal(element("askOk").disabled, false);
  assert.equal(element("askErr").classList.contains("hide"), true);

  context.askClose(null);
  await syslogPromise;
});
