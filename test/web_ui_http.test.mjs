import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";
import vm from "node:vm";

const appSource = fs.readFileSync(new URL("../main/www/app.js", import.meta.url), "utf8");

function loadUi(opts = {}) {
  const elements = new Map();
  const listeners = {};
  const element = (id) => {
    if (!elements.has(id)) {
      const classes = new Set();
      if (id.endsWith("Modal")) {
        classes.add("modal");
        classes.add("hide");
      }
      elements.set(id, {
        id,
        children: [],
        className: "",
        dataset: {},
        innerHTML: "",
        textContent: "",
        title: "",
        value: "",
        get disabled() { return this._disabled === true; },
        set disabled(v) {
          this._disabled = !!v;
          // like a browser: a control that is disabled while it has focus drops focus to <body>
          if (v && context.document && context.document.activeElement === this) context.document.activeElement = null;
        },
        style: {},
        setAttribute(name, value) { this[name] = String(value); },
        appendChild(child) { this.children.push(child); child.parentNode = this; },
        removeChild(child) { this.children.splice(this.children.indexOf(child), 1); },
        focus() { if (!this.disabled && context.document) context.document.activeElement = this; },
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
      createElement() { return element(`created-${elements.size}`); },
      // opt-in: capture the page's own document-level click/keydown listeners so tests can fire them
      ...(opts.listeners ? { addEventListener(name, fn) { (listeners[name] ||= []).push(fn); } } : {})
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
  const fire = (name, event) => { for (const fn of listeners[name] || []) fn(event); };
  return { context, element, fire };
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
  assert.match(element("toasts").children[0].innerHTML, /HTTP 503/);
  assert.doesNotMatch(element("toasts").children[0].innerHTML, /up to date/);
});

test("OTA status schema, deadline and missed-done idle recovery fail closed", async () => {
  const { context, element } = loadUi();
  context.fetch = async () => ({ ok: true, status: 200, async json() { return { state: "mystery" }; } });
  await assert.rejects(context.otaStatus(), /invalid OTA status response/);

  context.otaBusy = true;
  context.otaPhase = "update";
  context.otaDeadline = Date.now() - 1;
  context.otaPoll();
  assert.match(element("toasts").children[0].innerHTML, /update timed out/);

  let rebootArgs = null;
  context.waitReboot = (expected) => { rebootArgs = { expected }; };
  context.state = { version: "1.4.75" };
  context.otaBusy = true;
  context.otaPhase = "update";
  context.otaExpectedVersion = "1.4.76";
  context.otaProgress({ state: "idle", update_available: false });
  assert.deepEqual(rebootArgs, { expected: "1.4.76" });
  assert.match(element("toasts").children[0].innerHTML, /Verifying/);
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
  assert.doesNotMatch(html, /<button[^>]+id="verLink"[^>]*aria-label=/, "#verLink is named by its visible version and channel text");
  assert.match(html, /<button[^>]+id="verLink"[^>]+title="Change update channel"/);
  assert.match(html, /<button[^>]+id="fwCheckBtn"[^>]+aria-label="Check for firmware updates"/);
  // Action feedback belongs only in the bottom popup, including for sighted users.
  assert.doesNotMatch(html, /id="otaStat"/);
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

test("firmware card exposes update channel modal and update check button", () => {
  const html = fs.readFileSync(new URL("../main/www/index.html", import.meta.url), "utf8");
  assert.match(html, /<button type="button" class="ver-btn" id="verLink" onclick="openChannelSelect\(\)" title="Change update channel"/);
  assert.match(html, /<button type="button" class="ib" id="fwCheckBtn" onclick="openFwUpdate\(\)" aria-label="Check for firmware updates"/);
  assert.match(html, /id="chanModal"[\s\S]*?id="chanOptRelease"[\s\S]*?id="chanOptDev"/);
  assert.match(html, /id="verLink"[\s\S]*?id="fwVer"[\s\S]*?id="fwCheckBtn"/);
});

test("retired dock/tab and <select> channel code stays out of the page", () => {
  const css = fs.readFileSync(new URL("../main/www/style.css", import.meta.url), "utf8");
  const html = fs.readFileSync(new URL("../main/www/index.html", import.meta.url), "utf8");
  for (const token of ["setTab", "TAB_TITLE", "TAB_BTN", "uiTab", "fwVerText", "onChannelChange", "chanSelect", "setBadge", "netWarn"]) {
    assert.equal(appSource.includes(token), false, `app.js must not contain ${token}`);
  }
  for (const token of ["data-tab", "wide-title", "paneTitle", "chan-select"]) {
    assert.equal((html + css + appSource).includes(token), false, `page must not contain ${token}`);
  }
  assert.doesNotMatch(css, /\.cta\b|\.pane-settings\b|\.grab\b|\.fwhead\b|\.fwver\b|\.ti\b|\.badge\b|--shadow-cta/);
});

test("update channel modal updates channel selection with OTA check", async () => {
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

  // Re-selecting current channel (release) closes modal without /set_ota or otaCheck
  await context.selectChannel("release");
  assert.equal(fetchCalls.length, 0, "no network calls or OTA check when re-selecting current channel");

  // Select dev channel via modal selectChannel
  fetchCalls.length = 0;
  await context.selectChannel("dev");
  assert.equal(context.otaChannel, "dev");
  assert.equal(element("fwSub").textContent, "Development channel");

  // Check that POST /set_ota and /ota/check were requested
  const setOta = fetchCalls.find(c => c.url === "/set_ota");
  assert.ok(setOta, "POST /set_ota called");
  assert.equal(JSON.parse(setOta.opts.body).channel, "dev");

  const otaCheck = fetchCalls.find(c => c.url.startsWith("/ota/check"));
  assert.ok(otaCheck, "GET /ota/check called");
  assert.match(otaCheck.url, /\/ota\/check\?ms=\d+/);

  // Switching back to release
  fetchCalls.length = 0;
  await context.selectChannel("release");
  assert.equal(context.otaChannel, "release");
  assert.equal(element("fwSub").textContent, "Release channel");
  assert.ok(fetchCalls.some(c => c.url === "/set_ota" && JSON.parse(c.opts.body).channel === "release"));
});

test("render initializes channel state from running version or status ota channel", () => {
  const { context, element } = loadUi();

  // Release version initializes channel to 'release' and marks initial set
  context.render({ version: "1.4.100" });
  assert.equal(context.otaChannel, "release");
  assert.equal(context.otaChannelInitialSet, true);
  assert.equal(element("fwSub").textContent, "Release channel");

  // New instance for dev version
  const uiDev = loadUi();
  uiDev.context.render({ version: "1.4.100-dev.5" });
  assert.equal(uiDev.context.otaChannel, "dev");
  assert.equal(uiDev.context.otaChannelInitialSet, true);
  assert.equal(uiDev.element("fwSub").textContent, "Development channel");

  // Explicit s.ota.channel takes precedence
  uiDev.context.render({ version: "1.4.100-dev.5", ota: { channel: "release" } });
  assert.equal(uiDev.context.otaChannel, "release");
  assert.equal(uiDev.element("fwSub").textContent, "Release channel");
});

test("climate chips label cached settings without inferring AC power or current activity", () => {
  const { context, element } = loadUi();
  for (const phases of [undefined, 0, 1, 2, 3]) {
    for (const last_seen_s of [undefined, 0, 300, 86400]) {
      context.render({
        paired: true, last_seen_s,
        vehicle: { status: "Stopped", actual_amps: 16, volts: 230, phases },
        tele: { climate: { cop: "On", cop_cooling: true, front_defrost: true } }
      });
      const chips = element("hstats").innerHTML;
      assert.match(chips, /Overheat setting/);
      assert.match(chips, /Defrost \(cached\)/);
      assert.match(chips, /Last reported cabin overheat protection setting/);
      assert.match(chips, /Last reported defrost state/);
      assert.match(chips, /current activity and power are unknown/);
      assert.doesNotMatch(chips, /kW|AC draw|class="stat (warn|info)"/);
    }
  }
});

test("climate settings do not require charger input and reject unknown active states", () => {
  const { context } = loadUi();
  assert.match(context.copChip({ tele: { climate: { cop: "FanOnly" } } }), /Fan only/);
  assert.match(context.defrostChip({ tele: { climate: { rear_defrost: true, defrost_mode: "Max" } } }), /Rear \/ Max/);
  for (const cop of [undefined, "Off", "Unknown", "<script>"]) {
    assert.equal(context.copChip({ tele: { climate: { cop } } }), "");
  }
  for (const defrost_mode of [undefined, "Off", "Unknown", "<script>"]) {
    assert.equal(context.defrostChip({ tele: { climate: { front_defrost: false, rear_defrost: false, defrost_mode } } }), "");
  }
  const escaped = context.copChip({ tele: { climate: { cop: "On", cop_temp: '<img src=x onerror="alert(1)">' } } });
  assert.doesNotMatch(escaped, /<img/);
  assert.match(escaped, /&lt;img/);
  assert.equal(context.copChip({}), "");
  assert.equal(context.defrostChip({}), "");
});

test("charging power remains vehicle-reported alongside explicitly cached climate chips", () => {
  const { context, element } = loadUi();
  context.render({
    paired: true, last_seen_s: 0,
    vehicle: { status: "Charging", power: 11, amps: 16, actual_amps: 16, volts: 230, phases: 2 },
    tele: { climate: { cop: "On", front_defrost: true } }
  });
  assert.match(element("hstats").innerHTML, /Power.*11<small>kW/);
  assert.match(element("hstats").innerHTML, /Defrost \(cached\)/);
  assert.doesNotMatch(context.copChip(context.state) + context.defrostChip(context.state), /kW/);
});

test("awake without fresh charge shows last-known battery and offers no action", () => {
  const { context, element } = loadUi();
  for (const charge_age_s of [undefined, 0, 31, 86400]) {
    context.render({paired: true, link: "awake", last_seen_s: 0, charge_age_s,
                    last: {usable_soc: 0, status: "Charging"}, ble: {connected: true}});
    assert.match(element("hlabel").innerHTML, /Charge reading unavailable/);
    assert.match(element("hstats").innerHTML, /Last battery/);
    assert.doesNotMatch(element("hstats").innerHTML, /Power|kW|Idle/);
    assert.match(element("hstats").innerHTML, />0<\/span>/);
    assert.equal(context.heroActFn, null);
    if (charge_age_s !== undefined) assert.match(element("hstats").innerHTML, /Updated/);
  }
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

test("unpaired connected vehicle explains automatic pairing only in the hero", () => {
  const { context, element } = loadUi();
  context.render({
    paired: false,
    key_present: true,
    vin: "5YJ3E1EA7KF000316",
    ble: { connected: true },
    link: "unknown"
  });
  assert.match(element("hlabel").innerHTML, /Ready to pair/);
  assert.match(element("hsub").textContent, /key is sent automatically/i);
  assert.doesNotMatch(element("hsub").textContent, /^Approve the request/i);
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
  assert.match(element("hstats").innerHTML, /Last battery/);
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
  const rb = element("safeModeBanner");
  assert.equal(rb.classList.contains("show"), true);
  assert.match(rb.querySelector(".bt").innerHTML, /Safe Mode active/);
});

test("recovery uses the hero without a key-send banner, button or browser request", () => {
  const { context, element } = loadUi();
  const requests = [];
  context.fetch = (url) => { requests.push(url); throw new Error("unexpected browser request"); };
  for (let tick = 0; tick < 20; tick++) {
    context.render({
      link: "idle", paired: true, key_present: true, reauth: true,
      vin: "5YJ3E1EA7KF000316", ble: { connected: true }
    });
  }
  assert.equal(element("safeModeBanner").classList.contains("show"), false);
  assert.match(element("hlabel").innerHTML, /Reconnecting/);
  assert.match(element("hsub").textContent, /automatically with your existing key/i);
  assert.match(element("keySub").textContent, /Existing key preserved/);
  assert.equal(context.sendExistingKey, undefined);
  assert.equal(context.bannerAction, undefined);
  assert.deepEqual(requests, []);
  const html = fs.readFileSync(new URL("../main/www/index.html", import.meta.url), "utf8");
  assert.doesNotMatch(html, /reauthBanner|bannerAct|sendExistingKey|Send existing key/);
  const app = fs.readFileSync(new URL("../main/www/app.js", import.meta.url), "utf8");
  assert.doesNotMatch(app, /\/send_key|sendExistingKey/);
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
  for (const size of [63,64,65]) {
    assert.equal(await check(() => context.editMqtt(), "h".repeat(size-5)+":1883"),
      size<=64?"":"Broker too long (max 64 bytes)");
    assert.equal(await check(() => context.editSyslog(), "h".repeat(size-4)+":514"),
      size<=64?"":"Server too long (max 64 bytes)");
  }
  assert.equal(await check(() => context.editMqtt(), "ä".repeat(29)+":1883"), "");
  assert.match(await check(() => context.editMqtt(), "ä".repeat(30)+":1883"), /max 64 bytes/);
  assert.match(await check(() => context.editMqtt(), "192.0.2.20:\t1883"), /spaces/);
  assert.match(await check(() => context.editSyslog(), "192.0.2.30\n:514"), /spaces/);
  // 60 two-byte characters: 60 UTF-16 units but 120+ bytes on the device
  assert.match(await check(() => context.editMqtt(), "\u00e4".repeat(60) + ":1883"), /too long/);
  assert.equal(await check(() => context.editMqtt(), "192.0.2.20:1883"), "");
  assert.equal(await check(() => context.editSyslog(), "192.0.2.30"), "");
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
  assert.equal(context.document.activeElement, element("fwCheckBtn"), "focus returns once the page is live again");
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
  assert.equal(context.document.activeElement, element("fwCheckBtn"));
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

  context.otaBarProgress("indet");
  assert.equal(element("otaBar").classList.contains("indet"), true);
  assert.equal(element("otaFill").style.width, undefined, "no static width is painted for an unknown amount");
  context.otaBarProgress(42);
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

test("OTA starts wait for a complete status body and pause new general polls", async () => {
  for (const action of ["otaCheck", "startOtaUpdate"]) {
    const {context}=loadUi();
    const calls=[];
    const frame={paired:false,vin:"UNKNOWN",key_present:true,version:"1.6.3"};
    let finishBody;
    context.fetch=async url=>{
      calls.push(url);
      if(url.startsWith("/status"))return {ok:true,json:()=>new Promise(resolve=>{finishBody=()=>resolve(frame);})};
      if(url.startsWith("/ota/check"))return {ok:true,status:200,json:async()=>({started:true})};
      if(url.startsWith("/ota/update"))return {ok:true,status:200,json:async()=>({result:true})};
      if(url.startsWith("/ota/status"))return {ok:true,json:async()=>({state:action==="otaCheck"?"checking":"downloading",progress:1,message:"working",available:"1.6.4",current:"1.6.3",update_available:true})};
      throw new Error("unexpected request "+url);
    };
    const status=context.poll();
    await new Promise(resolve=>setImmediate(resolve));
    const started=context[action]();
    assert.equal(context.otaBusy,true,"busy before waiting for the response body");
    assert.equal(context.poll(),status,"an existing status request stays single-flight");
    await new Promise(resolve=>setImmediate(resolve));
    assert.equal(calls.length,1,"no OTA request while status JSON remains unread");
    finishBody();await status;await started;
    await new Promise(resolve=>setImmediate(resolve));
    assert.equal(calls.filter(url=>url.startsWith(action==="otaCheck"?"/ota/check":"/ota/update")).length,1);
    await context.poll();
    assert.equal(calls.filter(url=>url.startsWith("/status")).length,1,"no large status responses during OTA");
    context.otaReset();
    context.fetch=async url=>{calls.push(url);return {ok:true,json:async()=>frame};};
    await context.poll();
    assert.equal(calls.filter(url=>url.startsWith("/status")).length,2,"normal polling resumes after the flow ends");
  }
});

test("a cancelled status-wait cannot launch OTA or cancel its replacement flow", async () => {
  for(const replacement of [false,true]){
    const {context}=loadUi();const calls=[];let finishBody;
    context.fetch=async url=>{
      calls.push(url);
      if(url.startsWith("/status"))return {ok:true,json:()=>new Promise(resolve=>{finishBody=()=>resolve({version:"1.6.3"});})};
      if(url.startsWith("/ota/check"))return {ok:true,status:200,json:async()=>({started:true})};
      if(url.startsWith("/ota/status"))return {ok:true,json:async()=>({state:"idle",progress:0,message:"up to date",available:"1.6.3",current:"1.6.3",update_available:false})};
      throw new Error("unexpected request "+url);
    };
    const status=context.poll();await new Promise(resolve=>setImmediate(resolve));
    const cancelled=context.otaCheck();context.otaReset();
    const current=replacement?context.otaCheck():Promise.resolve();
    await new Promise(resolve=>setImmediate(resolve));
    assert.equal(calls.length,1);
    finishBody();await status;await cancelled;await current;
    assert.equal(calls.filter(url=>url.startsWith("/ota/check")).length,replacement?1:0);
    assert.equal(context.otaBusy,false);
  }
});

test("waiting for status does not renew the original OTA deadline", async () => {
  for(const action of ["otaCheck","startOtaUpdate"]){
    const {context,element}=loadUi();let finishBody;const calls=[];
    let now=Date.now();context.Date=class extends Date{static now(){return now;}};
    context.fetch=async url=>{calls.push(url);return {ok:true,json:()=>new Promise(resolve=>{finishBody=()=>resolve({version:"1.6.3"});})};};
    const status=context.poll();await new Promise(resolve=>setImmediate(resolve));
    const waiting=context[action]();
    await new Promise(resolve=>setImmediate(resolve));
    assert.equal(calls.length,1,"deadline test requires the status-body wait");
    now=context.otaDeadline+1;
    finishBody();await status;await waiting;
    assert.equal(calls.length,1,"expired waits must not start a backend operation");
    assert.equal(context.otaBusy,false);
    assert.match(element("toasts").children[0].innerHTML,/timed out/);
  }
});

test("general status polling stays paused throughout the update prompt", async () => {
  const {context}=loadUi();let calls=0;
  context.feedOk=true;context.otaPrompting=true;
  context.fetch=async()=>{calls++;return {ok:true,json:async()=>({version:"1.6.3"})};};
  await context.poll();assert.equal(calls,0);assert.equal(context.feedOk,true);
  context.otaReset();await context.poll();assert.equal(calls,1);
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

  // Harmonized settings card
  assert.match(html, /<section class="pane pane-setup card rows"[^>]*aria-label="Device settings"/);
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

test("otaProgress throttles download toasts to 10% milestones while maintaining the Firmware row progress bar", () => {
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

test("channel selection triggers /set_ota and otaCheck, immediately reflected in UI", async () => {
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
          available: "1.6.0",
          current: "1.6.0"
        };
      }
    };
    return { ok: true, status: 200, async json() { return {}; } };
  };

  // Initially on release channel
  context.render({ version: "1.6.0", ota: { channel: "release" } });
  assert.equal(element("fwSub").textContent, "Release channel");
  assert.equal(context.getActiveChannel(), "release");

  // User selects Development channel in modal
  await context.selectChannel("dev");
  assert.equal(context.getActiveChannel(), "dev");
  assert.equal(element("fwSub").textContent, "Development channel");

  const setOtaCall = fetchCalls.find(c => c.url === "/set_ota");
  assert.ok(setOtaCall, "POST /set_ota was called");
  assert.equal(JSON.parse(setOtaCall.opts.body).channel, "dev");

  const otaCheckCall = fetchCalls.find(c => c.url.startsWith("/ota/check"));
  assert.ok(otaCheckCall, "otaCheck was called on channel change");

  // Switching back to release
  fetchCalls.length = 0;
  await context.selectChannel("release");
  assert.equal(context.getActiveChannel(), "release");
  assert.equal(element("fwSub").textContent, "Release channel");
  assert.ok(fetchCalls.some(c => c.url === "/set_ota" && JSON.parse(c.opts.body).channel === "release"));
});

test("adaptive action button #fwCheckBtn displays title and aria-label based on otaAvail", () => {
  const { context, element } = loadUi();
  const cb = element("fwCheckBtn");

  // Idle state without update
  context.otaAvail = null;
  context.renderVerLink();
  assert.equal(cb.title, "Check for firmware updates");
  assert.equal(cb["aria-label"], "Check for firmware updates");
  assert.doesNotMatch(cb.className, /avail/);

  // Update available
  context.otaAvail = "1.7.0";
  context.renderVerLink();
  assert.equal(cb.title, "Update 1.7.0 available — tap to install");
  assert.equal(cb["aria-label"], "Install firmware update 1.7.0");
  assert.match(cb.className, /avail/);
});

test("channel switching is responsive and immune to intermediate polling race conditions", async () => {
  const { context, element } = loadUi();
  // Device starts on dev channel
  context.render({ version: "1.6.0-dev-1", ota: { channel: "dev" } });
  assert.equal(context.otaChannel, "dev");
  assert.equal(element("fwSub").textContent, "Development channel");

  // User selects Release via modal: setChannel initiated
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

  const channelPromise = context.selectChannel("release");

  // Immediately, UI reflects release
  assert.equal(context.otaChannel, "release");
  assert.equal(context.otaChannelInFlight, true);
  assert.equal(element("fwSub").textContent, "Release channel");

  // Concurrent /status poll arrives from device still reporting dev
  context.render({ version: "1.6.0-dev-1", ota: { channel: "dev" } });

  // UI MUST NOT revert back to dev
  assert.equal(context.otaChannel, "release");
  assert.equal(element("fwSub").textContent, "Release channel");

  // POST /set_ota finishes
  resolvePost();
  await channelPromise;

  assert.equal(context.otaChannel, "release");
  assert.equal(context.otaChannelInFlight, false);

  // Subsequent status poll now reporting release clears target
  context.render({ version: "1.6.0-dev-1", ota: { channel: "release" } });
  assert.equal(context.otaChannel, "release");
  assert.equal(context.otaChannelTarget, null);
  assert.equal(element("fwSub").textContent, "Release channel");
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


test("hero action is triggered by tapping the gauge button directly", () => {
  const { context, element } = loadUi();
  let wakeTriggered = false;
  context.wakeCar = () => { wakeTriggered = true; };

  const state = {
    paired: true,
    vin: "5YJ3E1EA1JF000001",
    key_present: true,
    link: "asleep",
    last_seen: { soc: 80 }
  };
  context.render(state);

  assert.match(element("hlabel").innerHTML, /Vehicle asleep/);
  assert.equal(element("hsub").textContent, "Tap the icon to wake the car.");

  const hicon = element("hicon");
  assert.match(hicon.innerHTML, /class="gbtn"/);
  assert.match(hicon.innerHTML, /onclick="heroTap\(\)"/);

  context.heroTap();
  assert.equal(wakeTriggered, true, "tapping the gauge executed wakeCar");
});

test("charging highlight never snaps back visibly at 12 o'clock and glides at a calm speed", () => {
  const { context } = loadUi();
  const css = fs.readFileSync(new URL("../main/www/style.css", import.meta.url), "utf8");

  // Keyframes: invisible at both ends of the trip, so the loop's wrap to the arc start is hidden.
  const kf = css.match(/@keyframes flow\{([^@]*?)\n\}/)[1];
  const frames = new Map();
  for (const m of kf.matchAll(/([\d%,\s]+)\{([^}]*)\}/g)) {
    for (const stop of m[1].split(",").map(s => s.trim())) frames.set(stop, { ...(frames.get(stop) || {}), decl: m[2] });
  }
  assert.match(frames.get("0%").decl, /opacity:\s*0/, "starts invisible");
  assert.match(frames.get("100%").decl, /opacity:\s*0/, "ends invisible, right before the wrap");
  assert.match(frames.get("72%").decl, /stroke-dashoffset:\s*var\(--flow-end\)/);
  assert.match(css, /\.gflow\{[^}]*opacity:\s*0[^}]*animation:\s*flow var\(--flow-dur,\s*[\d.]+s\) linear infinite/,
    "constant speed (no ease-in-out stall at the ends), invisible when animations are disabled");

  // Markup: cycle length follows the arc so short and long arcs move at a similar speed, clamped.
  const flow = (pct) => {
    const html = context.gaugeHTML({ mode: "charging", pct, limit: 100 });
    const end = Number(/--flow-end:(-?[\d.]+)px/.exec(html)[1]);
    const dur = Number(/--flow-dur:([\d.]+)s/.exec(html)[1]);
    return { end, dur, speed: -end / (dur * 0.72) };
  };
  const full = flow(99);
  assert.ok(full.dur > 5 && full.dur <= 8, `99 % cycle is slow enough to be calm (${full.dur}s)`);
  assert.ok(full.speed > 100 && full.speed < 180, `glide speed ${full.speed.toFixed(0)} units/s`);
  for (const pct of [7, 25, 60, 100]) {
    const f = flow(pct);
    assert.ok(f.dur >= 3 && f.dur <= 8, `${pct} % cycle ${f.dur}s stays within [3, 8]`);
  }
  assert.doesNotMatch(context.gaugeHTML({ mode: "soc", pct: 60, limit: 100 }), /gflow/, "only charging animates");
});

test("openFwUpdate checks for updates, closes modal and toasts if up to date", async () => {
  const { context, element } = loadUi();
  element("otaModal").classList.add("hide");

  const toasts = [];
  context.toast = (msg, type) => { toasts.push({ msg, type }); };

  context.fetch = async (url) => {
    if (url.startsWith("/ota/check")) {
      return { ok: true, status: 200, async json() { return { started: true }; } };
    }
    if (url.startsWith("/ota/status")) {
      return {
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
    }
    return { ok: true, status: 200, async json() { return {}; } };
  };

  const promise = context.openFwUpdate();
  await promise;

  // After check finds up to date, modal is closed and toast is shown
  assert.equal(element("otaModal").classList.contains("hide"), true);
  assert.equal(toasts.length > 0, true);
  assert.ok(toasts.some(t => /up to date/.test(t.msg)), "toast matches up to date");
});

test("openFwUpdate with update available allows installing through startOtaUpdate", async () => {
  const { context, element } = loadUi();
  element("otaModal").classList.add("hide");

  const postCalls = [];
  context.fetch = async (url, opts) => {
    if (opts && opts.method === "POST") postCalls.push(url);
    if (url.startsWith("/ota/changelog")) {
      return { ok: true, status: 200, async text() { return "- Fix things"; } };
    }
    if (url.startsWith("/ota/status")) {
      return {
        ok: true,
        status: 200,
        async json() {
          return {
            state: "idle",
            update_available: true,
            progress: 0,
            message: "update available",
            available: "1.5.0",
            current: "1.4.0"
          };
        }
      };
    }
    if (url.startsWith("/ota/update")) {
      return { ok: true, status: 200, async json() { return { result: true }; } };
    }
    return { ok: true, status: 200, async json() { return {}; } };
  };

  context.state = { version: "1.4.0" };
  context.otaAvail = "1.5.0";

  // Simulate user confirming install: closeOtaModal(true)
  setTimeout(() => {
    context.closeOtaModal(true);
  }, 10);

  await context.openFwUpdate();
  assert.ok(postCalls.some(u => u.startsWith("/ota/update")), "POST /ota/update called");
});

test("openFwUpdate closes modal on check failure", async () => {
  const { context, element } = loadUi();
  element("otaModal").classList.add("hide");

  context.fetch = async () => ({
    ok: false,
    status: 500,
    async json() { return { reason: "server error" }; }
  });

  await context.openFwUpdate();
  assert.equal(element("otaModal").classList.contains("hide"), true, "modal is closed on failure");
  assert.equal(context.otaBusy, false);
});


test("UI disables settings and firmware controls during active OTA, closes open sheets on start, and prevents tampering", async () => {
  const { context, element } = loadUi();
  const fetches = [];
  context.fetch = async (url, opts) => {
    fetches.push({ url, opts });
    if (url.includes("/ota/status")) {
      return {
        ok: true,
        status: 200,
        async json() {
          return {
            state: "downloading",
            update_available: false,
            progress: 42,
            message: "downloading firmware…",
            available: "1.6.0",
            current: "1.6.0",
            channel: "release"
          };
        }
      };
    }
    if (url.includes("/ota/update")) {
      return {
        ok: true,
        status: 200,
        async json() {
          return { result: true, reason: "update started" };
        }
      };
    }
    return { ok: true, status: 200, async json() { return {}; } };
  };

  context.render({ version: "1.6.0" });
  assert.equal(context.isOtaRunning(), false);
  assert.equal(element("fwCheckBtn").disabled, false);
  assert.equal(element("verLink").disabled, false);
  assert.equal(element("paneSettings").classList.contains("ota-busy"), false);

  // Trigger OTA update
  await context.startOtaUpdate("1.6.0");

  // Upon starting update:
  // 1. Modals/sheets are closed
  assert.equal(element("otaModal").classList.contains("hide"), true);
  assert.equal(element("askModal").classList.contains("hide"), true);
  assert.equal(element("chanModal").classList.contains("hide"), true);

  // 2. isOtaRunning() is true
  assert.equal(context.isOtaRunning(), true);

  // 3. Firmware buttons are disabled
  assert.equal(element("fwCheckBtn").disabled, true);
  assert.equal(element("verLink").disabled, true);
  assert.equal(element("verLink").classList.contains("disabled"), true);

  // 4. Pane settings is marked ota-busy, modal actions and setting buttons disabled
  assert.equal(element("paneSettings").classList.contains("ota-busy"), true);
  assert.equal(element("otaInstall").disabled, true);
  assert.equal(element("vinBtn").disabled, true);
  assert.equal(element("keyBtn").disabled, true);
  assert.equal(element("mqttBtn").disabled, true);
  assert.equal(element("syslogBtn").disabled, true);

  // 5. Attempting to click settings, manipulate channels, or open modals does nothing
  fetches.length = 0;
  await context.editVin();
  await context.genKey();
  await context.editMqtt();
  await context.editSyslog();
  await context.openFwUpdate();
  await context.openChannelSelect();
  await context.selectChannel("dev");
  await context.setChannel("dev");
  await context.otaCheck();
  assert.equal(fetches.length, 0, "no network calls made while OTA is running");
  assert.equal(element("otaModal").classList.contains("hide"), true);
  assert.equal(element("askModal").classList.contains("hide"), true);
  assert.equal(element("chanModal").classList.contains("hide"), true);

  // 6. When OTA resets/finishes, UI returns to normal
  context.otaReset();
  assert.equal(context.isOtaRunning(), false);
  assert.equal(element("fwCheckBtn").disabled, false);
  assert.equal(element("verLink").disabled, false);
  assert.equal(element("paneSettings").classList.contains("ota-busy"), false);
  assert.equal(element("vinBtn").disabled, false);
  assert.equal(element("keyBtn").disabled, false);
  assert.equal(element("mqttBtn").disabled, false);
  assert.equal(element("syslogBtn").disabled, false);
});

test("firmware card displays version with accessible verLink button and channel select modal", () => {
  const html = fs.readFileSync(new URL("../main/www/index.html", import.meta.url), "utf8");
  assert.match(html, /<div class="row row-fw" id="rowFw">[\s\S]*?id="verLink"[\s\S]*?id="fwCheckBtn"/);
  assert.doesNotMatch(html, /id="chanBtn"/);
  assert.match(html, /id="fwSub"/);
  assert.match(html, /<button type="button" class="ver-btn" id="verLink" onclick="openChannelSelect\(\)"/);

  const { context, element } = loadUi();
  context.render({ version: "1.4.0", ota: { channel: "release" } });
  assert.equal(element("fwVer").textContent, "v1.4.0");
  assert.equal(element("fwSub").textContent, "Release channel");
});

test("tapping the update check button shows a toast and no modal when up to date, and opens the changelog modal only when an update is available", async () => {
  const { context, element } = loadUi();
  const toasts = [];
  context.toast = (msg, type) => { toasts.push({ msg, type }); };
  context.state = { version: "1.4.0" };
  element("otaModal").classList.add("hide");

  let statusResponse = {
    state: "idle",
    update_available: false,
    progress: 0,
    message: "up to date",
    available: "1.4.0",
    current: "1.4.0"
  };

  context.fetch = async (url) => {
    if (url.startsWith("/ota/check")) {
      return { ok: true, status: 200, async json() { return { started: true }; } };
    }
    if (url.startsWith("/ota/status")) {
      return { ok: true, status: 200, async json() { return statusResponse; } };
    }
    if (url.startsWith("/ota/changelog")) {
      return { ok: true, status: 200, async text() { return "• Performance improvements\n• BLE fixes"; } };
    }
    return { ok: true, status: 200, async json() { return {}; } };
  };

  // Case 1: Tapping version link when firmware is up to date
  await context.openFwUpdate();
  assert.equal(element("otaModal").classList.contains("hide"), true, "otaModal must remain closed when up to date");
  assert.ok(toasts.some(t => /Checking for updates/.test(t.msg)), "showed checking toast");
  assert.ok(toasts.some(t => /Firmware is up to date/.test(t.msg)), "showed up to date toast");
  assert.equal(context.otaAvail, null);

  // Case 2: Tapping version link when a new version is available
  toasts.length = 0;
  statusResponse = {
    state: "idle",
    update_available: true,
    progress: 0,
    message: "update available",
    available: "1.5.0",
    current: "1.4.0"
  };

  let installPromise = context.openFwUpdate();
  await new Promise(r => setTimeout(r, 20));

  // Modal must now be open with changelog and version info
  assert.equal(element("otaModal").classList.contains("hide"), false, "otaModal opens when update is available");
  assert.equal(element("otaVersionLine").textContent, "v1.4.0 → v1.5.0");
  assert.equal(element("otaChanges").hidden, false);
  assert.equal(element("otaChanges").children.length, 2);
  assert.equal(element("otaChanges").children[0].textContent, "• Performance improvements");
  assert.equal(element("otaInstall").classList.contains("hide"), false);

  // User cancels modal
  context.closeOtaModal(false);
  await installPromise;
  assert.equal(element("otaModal").classList.contains("hide"), true);

  // Case 3: When otaAvail is already set, tapping version link immediately opens otaModal
  context.otaAvail = "1.5.0";
  installPromise = context.openFwUpdate();
  await new Promise(r => setTimeout(r, 20));
  assert.equal(element("otaModal").classList.contains("hide"), false, "otaModal opens immediately when otaAvail is known");
  context.closeOtaModal(false);
  await installPromise;
});

test("askOtaInstall unhides install button and resolves confirmation", async () => {
  const { context, element } = loadUi();
  element("otaInstall").classList.add("hide");

  const installPromise = context.askOtaInstall(
    { current: "1.4.0", available: "1.5.0" },
    "Improve BLE responsiveness\nFix OTA modal"
  );

  assert.equal(element("otaInstall").classList.contains("hide"), false);
  assert.equal(element("otaChanges").hidden, false);
  assert.equal(element("otaChanges").children.length, 2);
  assert.equal(element("otaNoChanges").hidden, true);

  context.closeOtaModal(true);
  const decision = await installPromise;
  assert.equal(decision, true);
});


test("mutation endpoints handle HTTP 409 Conflict gracefully with error toast", async () => {
  const { context } = loadUi();
  const toasts = [];
  context.toast = (msg, kind, scope) => toasts.push({ msg, kind, scope });
  context.state = { vin: "5YJ3E1EA1JF000001", key_present: false, vehicle: { status: "Stopped" } };
  context.askConfirm = async () => true;

  context.fetch = async (url) => {
    if (url === "/gen_keys") {
      return {
        ok: false,
        status: 409,
        async json() { return { result: false, reason: "mutation locked by active session" }; }
      };
    }
    return { ok: true, status: 200, async json() { return {}; } };
  };

  await context.genKey();
  const errToast = toasts.find(t => t.kind === "err" && t.scope === "key");
  assert.ok(errToast, "Error toast displayed on 409 Conflict");
  assert.match(errToast.msg, /mutation locked/);
});

test("Update confirmation dialog survives periodic background polls and does not self-dismiss", async () => {
  const { context, element } = loadUi();
  context.state = { version: "1.4.0", vin: "5YJ3E1EA1JF000001", key_present: true };

  context.fetch = async (url) => {
    if (url.startsWith("/ota/check")) {
      return { ok: true, status: 200, async json() { return { started: true }; } };
    }
    if (url.startsWith("/ota/status")) {
      return {
        ok: true,
        status: 200,
        async json() {
          return {
            state: "idle",
            update_available: true,
            progress: 0,
            message: "update available",
            available: "1.5.0",
            current: "1.4.0",
            channel: "release"
          };
        }
      };
    }
    if (url.startsWith("/ota/changelog")) {
      return { ok: true, status: 200, async text() { return "v1.5.0 changelog notes"; } };
    }
    if (url.startsWith("/ota/update")) {
      return { ok: true, status: 200, async json() { return { result: true }; } };
    }
    return { ok: true, status: 200, async json() { return {}; } };
  };

  // Launch update check which finds v1.5.0 available and opens askOtaInstall
  const checkPromise = context.openFwUpdate();

  // Yield to allow fetch/askOtaInstall to open the modal
  await new Promise(resolve => setTimeout(resolve, 30));

  // Verify dialog is open and waiting for confirmation
  assert.equal(element("otaModal").classList.contains("hide"), false, "otaModal must be visible");
  assert.ok(context.otaDecisionResolve !== null, "otaDecisionResolve must be pending");
  assert.equal(context.isOtaRunning(), false, "isOtaRunning must be false while waiting for user decision");

  // Verify runtime settings configuration buttons are NOT disabled during idle check/prompt
  assert.equal(element("vinBtn").disabled, false, "vinBtn must not be locked during update prompt");
  assert.equal(element("keyBtn").disabled, false, "keyBtn must not be locked during update prompt");
  assert.equal(element("mqttBtn").disabled, false, "mqttBtn must not be locked during update prompt");

  // Simulate multiple periodic background /status poll frames arriving while dialog is open
  for (let i = 0; i < 5; i++) {
    context.render({ version: "1.4.0", vehicle: { status: "Stopped" } });
    context.syncOtaUi();
    assert.equal(element("otaModal").classList.contains("hide"), false, `otaModal must stay open after poll frame ${i + 1}`);
    assert.ok(context.otaDecisionResolve !== null, `otaDecisionResolve must remain pending after poll frame ${i + 1}`);
    assert.equal(context.otaAvail, "1.5.0", "otaAvail must remain preserved");
  }

  // Confirm installation: user taps Install / confirms
  context.closeOtaModal(true);
  await checkPromise;

  // Once installation starts, isOtaRunning() becomes true
  assert.equal(context.isOtaRunning(), true, "isOtaRunning becomes true after user confirms install");
  assert.equal(element("otaModal").classList.contains("hide"), true, "otaModal closes once update starts");
});

test("channel selection modal opens, reflects current channel, and allows switching channels", async () => {
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

  // Initially rendered with release channel
  context.render({ version: "1.4.0", ota: { channel: "release" } });
  assert.equal(element("fwSub").textContent, "Release channel");
  assert.equal(element("chanBadgeRelease").classList.contains("hide"), false);
  assert.equal(element("chanBadgeDev").classList.contains("hide"), true);
  assert.equal(element("chanOptRelease").classList.contains("selected"), true);
  assert.equal(element("chanOptDev").classList.contains("selected"), false);

  // Open channel modal
  context.openChannelSelect();
  assert.equal(element("chanModal").classList.contains("hide"), false, "chanModal is visible");

  // Close channel modal via closeChannelModal
  context.closeChannelModal();
  assert.equal(element("chanModal").classList.contains("hide"), true, "chanModal is hidden");
  assert.equal(context.document.activeElement, element("verLink"), "focus returns to verLink");

  // Select dev channel via selectChannel
  await context.selectChannel("dev");
  assert.equal(context.otaChannel, "dev");
  assert.equal(element("fwSub").textContent, "Development channel");
  assert.equal(element("chanBadgeRelease").classList.contains("hide"), true);
  assert.equal(element("chanBadgeDev").classList.contains("hide"), false);
  assert.equal(element("chanOptRelease").classList.contains("selected"), false);
  assert.equal(element("chanOptDev").classList.contains("selected"), true);
  assert.equal(element("chanModal").classList.contains("hide"), true, "modal closes after selection");
});

test("channel modal is dismissed by the page's real backdrop-click and Escape handlers", () => {
  const { context, element, fire } = loadUi({ listeners: true });

  context.openChannelSelect();
  assert.equal(element("chanModal").classList.contains("hide"), false);
  fire("click", { target: element("chanOptDev") });
  assert.equal(element("chanModal").classList.contains("hide"), false, "a click inside the dialog is not a dismissal");
  fire("click", { target: element("chanBackdrop") });
  assert.equal(element("chanModal").classList.contains("hide"), true, "backdrop click closes the dialog");
  assert.equal(context.document.activeElement, element("verLink"), "focus returns to the control that opened it");

  context.openChannelSelect();
  fire("keydown", { key: "Enter" });
  assert.equal(element("chanModal").classList.contains("hide"), false, "other keys leave it open");
  fire("keydown", { key: "Escape" });
  assert.equal(element("chanModal").classList.contains("hide"), true, "Escape closes the dialog");
  assert.equal(element("wrap").inert, false, "page behind is live again");
});

test("channel modal has no cancel button and selecting active channel is a clean no-op", async () => {
  const html = fs.readFileSync(new URL("../main/www/index.html", import.meta.url), "utf8");
  assert.doesNotMatch(html, /id="chanCancel"/, "chanModal must not have a cancel button");

  const { context, element } = loadUi();

  const fetches = [];
  context.fetch = async (url) => {
    fetches.push(url);
    return { ok: true, status: 200, async json() { return {}; } };
  };

  // Open modal
  context.openChannelSelect();
  assert.equal(element("chanModal").classList.contains("hide"), false);

  // Select active channel (release)
  await context.selectChannel("release");
  assert.equal(element("chanModal").classList.contains("hide"), true, "modal must close");
  assert.equal(fetches.length, 0, "no network calls or update checks initiated on active channel re-selection");
});

test("atomic update check locks settings and prevents concurrent checks and other popups", async () => {
  const { context, element } = loadUi();
  context.state = { version: "1.4.0", vin: "5YJ3E1EA1JF000001", key_present: true };

  let resolveCheck;
  let checkCalls = 0;
  context.fetch = async (url) => {
    if (url.startsWith("/ota/check")) {
      checkCalls++;
      return new Promise((resolve) => {
        resolveCheck = () => resolve({ ok: true, status: 200, async json() { return { started: true }; } });
      });
    }
    if (url.startsWith("/ota/status")) {
      return {
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
    }
    return { ok: true, status: 200, async json() { return {}; } };
  };

  // Trigger update check via openFwUpdate
  const checkPromise = context.openFwUpdate();

  // Check is in-flight
  assert.equal(context.isOtaBusy(), true, "isOtaBusy must be true during check");
  assert.equal(element("fwCheckBtn").disabled, true, "fwCheckBtn must be disabled");
  assert.equal(element("verLink").disabled, true, "verLink must be disabled");
  assert.equal(element("vinBtn").disabled, true, "vinBtn must be disabled");
  assert.equal(element("paneSettings").classList.contains("ota-busy"), true, "paneSettings must have ota-busy class");

  // Attempting concurrent openFwUpdate while busy must be ignored
  await context.openFwUpdate();
  assert.equal(checkCalls, 1, "concurrent openFwUpdate must not start a second check");

  // Attempting to open channel select while busy must be blocked
  context.openChannelSelect();
  assert.equal(element("chanModal").classList.contains("hide"), true, "chanModal must stay hidden while busy");

  // Attempting to open editVin while busy must be blocked
  await context.editVin();
  assert.equal(element("askModal").classList.contains("hide"), true, "askModal must stay hidden while busy");

  // Attempting to open genKey while busy must be blocked
  await context.genKey();
  assert.equal(element("askModal").classList.contains("hide"), true, "genKey must stay hidden while busy");

  // Resolve check
  resolveCheck();
  await checkPromise;

  // After completion (up-to-date), busy is cleared and UI is unlocked
  assert.equal(context.isOtaBusy(), false, "isOtaBusy must be false after check completes");
  assert.equal(element("fwCheckBtn").disabled, false, "fwCheckBtn must be enabled");
  assert.equal(element("verLink").disabled, false, "verLink must be enabled");
  assert.equal(element("vinBtn").disabled, false, "vinBtn must be enabled");
  assert.equal(element("paneSettings").classList.contains("ota-busy"), false, "paneSettings must not have ota-busy class");

  // Popups work normally again
  context.openChannelSelect();
  assert.equal(element("chanModal").classList.contains("hide"), false, "chanModal can open now");
  context.closeChannelModal();
});

function otaFetchStub({ setOta, statusUpdate = false } = {}) {
  return async (url) => {
    if (url === "/set_ota") return setOta ? setOta() : { ok: true, status: 200, async json() { return { response: { result: true, command: "set_ota", vin: "", reason: "" } }; } };
    if (url.startsWith("/ota/check")) return { ok: true, status: 200, async json() { return { started: true }; } };
    if (url.startsWith("/ota/status")) return { ok: true, status: 200, async json() {
      return { state: "idle", update_available: statusUpdate, progress: 0, message: "ok", available: "1.6.0", current: "1.6.0" };
    } };
    return { ok: true, status: 200, async json() { return {}; } };
  };
}

test("focus returns to the control that started a channel change once the UI is idle again", async () => {
  const { context, element } = loadUi();
  context.toast = () => {};
  context.render({ version: "1.6.0", ota: { channel: "release" } });
  context.fetch = otaFetchStub();

  context.openChannelSelect();
  await context.selectChannel("dev");
  assert.equal(context.isOtaBusy(), false);
  assert.equal(context.document.activeElement, element("verLink"), "focus is not left on <body> after the check settles");
  assert.equal(element("verLink").disabled, false);
});

test("focus returns to the update check button after an up-to-date result and after a failed check", async () => {
  const { context, element } = loadUi();
  context.toast = () => {};
  context.render({ version: "1.6.0", ota: { channel: "release" } });

  context.fetch = otaFetchStub();
  element("fwCheckBtn").focus();
  await context.openFwUpdate();
  assert.equal(context.document.activeElement, element("fwCheckBtn"), "up to date");

  context.fetch = async () => { throw new Error("offline"); };
  await context.openFwUpdate();
  assert.equal(context.document.activeElement, element("fwCheckBtn"), "failed check");
});

test("fwRefocus hands focus back only when it was lost, and only once the UI is idle", () => {
  const { context, element } = loadUi();
  context.render({ version: "1.6.0", ota: { channel: "release" } });

  // the user already moved on to another control: focus stays, the pending target is consumed
  element("mqttBtn").focus();
  context.fwFocusId = "verLink";
  context.fwRefocus();
  assert.equal(context.document.activeElement, element("mqttBtn"));
  assert.equal(context.fwFocusId, null);

  // focus was dropped (control disabled while focused): it goes back, without scrolling the page
  let focusArgs;
  const verLink = element("verLink");
  verLink.focus = function (opts) { focusArgs = opts; context.document.activeElement = this; };
  context.document.activeElement = null;
  context.fwFocusId = "verLink";
  context.fwRefocus();
  assert.equal(context.document.activeElement, verLink);
  assert.equal(focusArgs && focusArgs.preventScroll, true);

  // still busy: the target is kept for the moment the flow ends
  context.document.activeElement = null;
  context.fwFocusId = "verLink";
  context.otaBusy = true;
  context.fwRefocus();
  assert.equal(context.document.activeElement, null);
  assert.equal(context.fwFocusId, "verLink");
  context.otaBusy = false;
  context.fwRefocus();
  assert.equal(context.document.activeElement, verLink);
  assert.equal(context.fwFocusId, null);
});

test("a /set_ota request that never settles times out, is reported as unconfirmed, rolls back and unlocks the UI", async () => {
  const { context, element } = loadUi();
  const toasts = [];
  const urls = [];
  context.toast = (msg, type) => toasts.push({ msg, type });
  context.render({ version: "1.6.0", ota: { channel: "release" } });
  context.fetch = (url) => { urls.push(url); return new Promise(() => {}); };
  context.setTimeout = (callback) => { queueMicrotask(callback); return 1; };

  await context.selectChannel("dev");
  assert.equal(context.otaChannel, "release", "optimistic switch is rolled back");
  assert.equal(element("fwSub").textContent, "Release channel");
  assert.equal(context.isOtaBusy(), false, "a hung request must not leave the page locked");
  assert.equal(element("verLink").disabled, false);
  assert.ok(toasts.some(t => t.type === "err" && t.msg === "Update channel change not confirmed — refreshing status"));
  assert.ok(urls.some(u => u.startsWith("/status")), "the device's own answer is re-read instead of guessing");
});

test("a refused channel change rolls back, shows the device's reason and hands focus back", async () => {
  const { context, element } = loadUi();
  const toasts = [];
  const urls = [];
  context.toast = (msg, type) => toasts.push({ msg, type });
  context.render({ version: "1.6.0", ota: { channel: "release" } });

  context.fetch = async (url, opts) => {
    urls.push(url);
    return otaFetchStub({ setOta: () => ({
      ok: false, status: 409,
      async json() { return { response: { result: false, command: "set_ota", vin: "", reason: "an OTA update is in progress" } }; }
    }) })(url, opts);
  };
  await context.selectChannel("dev");
  assert.equal(context.otaChannel, "release", "optimistic switch is rolled back");
  assert.equal(element("fwSub").textContent, "Release channel");
  assert.equal(context.isOtaBusy(), false);
  assert.ok(toasts.some(t => t.type === "err" && t.msg === "Failed to set update channel — an OTA update is in progress"));
  assert.equal(context.document.activeElement, element("verLink"), "focus is handed back after the refusal");
  assert.equal(urls.some(u => u.startsWith("/status")), false, "a definite refusal needs no status refresh");
  assert.equal(urls.some(u => u.startsWith("/ota/check")), false, "no update check after a refused change");

  // the device answered but gave no readable reason: still a refusal, generic text
  toasts.length = 0;
  context.fetch = otaFetchStub({ setOta: () => ({ ok: false, status: 502, async json() { throw new Error("not json"); } }) });
  await context.selectChannel("dev");
  assert.equal(context.otaChannel, "release");
  assert.ok(toasts.some(t => t.type === "err" && t.msg === "Failed to set update channel"));
});

test("channel options expose their state via aria-pressed and hold no block elements inside buttons", () => {
  const html = fs.readFileSync(new URL("../main/www/index.html", import.meta.url), "utf8");
  assert.doesNotMatch(html, /<button[^>]*class="chan-opt"[^>]*>\s*<div/);
  assert.match(html, /id="chanOptRelease" aria-pressed="true"/);
  assert.match(html, /id="chanOptDev" aria-pressed="false"/);
  assert.match(html, /id="chanModal"[^>]*aria-describedby="chanHelp"/);
  assert.match(html, /id="chanHelp"/);

  const { context, element } = loadUi();
  context.render({ version: "1.6.0-dev-1", ota: { channel: "dev" } });
  assert.equal(element("chanOptDev")["aria-pressed"], "true");
  assert.equal(element("chanOptRelease")["aria-pressed"], "false");
  context.render({ version: "1.6.0", ota: { channel: "release" } });
  assert.equal(element("chanOptDev")["aria-pressed"], "false");
  assert.equal(element("chanOptRelease")["aria-pressed"], "true");
});

test("a second tap during the changelog fetch neither opens a second dialog nor loses the version", async () => {
  const { context, element } = loadUi();
  context.toast = () => {};
  context.state = { version: "1.4.0" };
  let changelogCalls = 0;
  let releaseChangelog;
  const changelogGate = new Promise(resolve => { releaseChangelog = resolve; });
  const updates = [];
  context.fetch = async (url) => {
    if (url.startsWith("/ota/check")) return { ok: true, status: 200, async json() { return { started: true }; } };
    if (url.startsWith("/ota/status")) return { ok: true, status: 200, async json() {
      return { state: "idle", update_available: true, progress: 0, message: "update available", available: "1.5.0", current: "1.4.0" };
    } };
    if (url.startsWith("/ota/changelog")) { changelogCalls++; await changelogGate; return { ok: true, status: 200, async text() { return "notes"; } }; }
    if (url.startsWith("/ota/update")) { updates.push(url); return { ok: true, status: 200, async json() { return { result: true }; } }; }
    return { ok: true, status: 200, async json() { return {}; } };
  };

  const flow = context.openFwUpdate();
  await new Promise(resolve => setTimeout(resolve, 20));      // the check is done, the changelog is still loading
  assert.equal(context.otaPrompting, true);
  assert.equal(context.isOtaFlowBusy(), true);
  assert.equal(context.isOtaBusy(), false, "the prompt does not lock the rest of the settings");
  assert.equal(element("vinBtn").disabled, false, "other settings stay usable while the prompt is pending");

  await context.openFwUpdate();
  await context.otaCheck();
  await context.openChannelSelect();
  await context.setChannel("dev");
  assert.equal(changelogCalls, 1, "no second changelog fetch");
  assert.equal(element("chanModal").classList.contains("hide"), true, "no channel dialog on top of the pending update prompt");

  releaseChangelog();
  await new Promise(resolve => setTimeout(resolve, 20));
  assert.equal(element("otaModal").classList.contains("hide"), false);
  context.closeOtaModal(true);
  await flow;
  assert.equal(updates.length, 1);
  assert.equal(context.otaExpectedVersion, "1.5.0", "the install targets the version that was offered");
  assert.equal(context.otaPrompting, false);
});

test("declining the update prompt frees the firmware controls and hands focus back", async () => {
  const { context, element } = loadUi();
  context.toast = () => {};
  context.state = { version: "1.4.0" };
  context.fetch = async (url) => {
    if (url.startsWith("/ota/check")) return { ok: true, status: 200, async json() { return { started: true }; } };
    if (url.startsWith("/ota/status")) return { ok: true, status: 200, async json() {
      return { state: "idle", update_available: true, progress: 0, message: "update available", available: "1.5.0", current: "1.4.0" };
    } };
    if (url.startsWith("/ota/changelog")) return { ok: true, status: 200, async text() { return ""; } };
    return { ok: true, status: 200, async json() { return {}; } };
  };
  element("fwCheckBtn").focus();
  const flow = context.openFwUpdate();
  await new Promise(resolve => setTimeout(resolve, 30));
  assert.equal(element("otaModal").classList.contains("hide"), false);
  context.closeOtaModal(false);
  await flow;
  assert.equal(context.otaPrompting, false);
  assert.equal(context.isOtaBusy(), false);
  assert.equal(element("fwCheckBtn").disabled, false);
  assert.equal(element("verLink").disabled, false);
  assert.equal(context.document.activeElement, element("fwCheckBtn"));
});

test("hero vehicle actions are not blocked by a firmware check or channel change", () => {
  const { context } = loadUi();
  let heroCalls = 0;
  context.heroActFn = () => { heroCalls++; };

  context.otaPhase = "check";
  context.heroTap();
  assert.equal(heroCalls, 1);

  context.otaPhase = null;
  context.otaChannelInFlight = true;
  context.heroTap();
  assert.equal(heroCalls, 2);

  context.otaChannelInFlight = false;
  context.otaPhase = "update";
});

test("desktop and mobile layout styles retain proportions and only the gauge button dispatches", () => {
  const css = fs.readFileSync(new URL("../main/www/style.css", import.meta.url), "utf8");
  // Proportions: 1/3 to 2/3 desktop grid with stretch align and height 100%
  assert.match(css, /grid-template-columns:\s*380px\s+minmax\(0,\s*1fr\)/);
  assert.match(css, /align-items:\s*stretch/);
  assert.match(css, /#paneSettings\s*\{[^}]*height:\s*100%/);
  // Scaled header and logo
  assert.match(css, /clamp\(22px,\s*5\.5vw,\s*28px\)/);
  assert.match(css, /\.logo\s*\{[^}]*width:\s*46px;\s*height:\s*46px;/);
  // Full text wrapping without ellipsis in hero subtext
  assert.match(css, /\.hsub\s*\{[^}]*white-space:\s*normal;\s*overflow-wrap:\s*break-word/);

  const html = fs.readFileSync(new URL("../main/www/index.html", import.meta.url), "utf8");
  const heroTag = html.match(/<section[^>]*id="hero"[^>]*>/)[0];
  assert.doesNotMatch(heroTag, /onclick=|role="button"|tabindex=/);
  const { context, element } = loadUi();
  context.render({paired: true, key_present: true, vin: "5YJ3E1EB8NF123456", link: "asleep"});
  assert.match(element("hicon").innerHTML, /<button[^>]*onclick="heroTap\(\)"[^>]*aria-label="Wake the car/);
  let calls = 0;
  context.heroActFn = () => { calls++; };
  context.heroTap();
  assert.equal(calls, 1);
  assert.equal(context.heroCardTap, undefined);
  assert.doesNotMatch(css, /\.hero\.has-act/);

});

test("num() keeps absent readings absent instead of turning them into 0", () => {
  const { context } = loadUi();
  assert.equal(context.num(null), null);
  assert.equal(context.num(undefined), null);
  assert.equal(context.num(""), null);
  assert.equal(context.num("abc"), null);
  assert.equal(context.num(NaN), null);
  assert.equal(context.num(0), 0, "a genuine zero reading is still a reading");
  assert.equal(context.num("-72"), -72);
  assert.equal(context.num(80), 80);
});

test("a paired vehicle without a reported battery level shows no 0 % gauge", () => {
  const { context, element } = loadUi();
  context.render({ paired: true, key_present: true, vin: "5YJ3E1EB8NF123456",
                   vehicle: { status: "Idle" } });
  const gauge = element("hicon").innerHTML;
  assert.doesNotMatch(gauge, /<b>0<small>%/, "an unknown level must not read as an empty battery");
  assert.doesNotMatch(gauge, /battery 0 %/);
  assert.doesNotMatch(gauge, /garc/, "no arc is drawn for an unknown level");
  assert.match(gauge, /gglyph/, "an unknown-level glyph is shown instead of a number");
  assert.match(element("hsub").textContent, /not reported/);
  // A real level, including a genuine 0 %, is still rendered.
  context.render({ paired: true, key_present: true, vin: "5YJ3E1EB8NF123456",
                   vehicle: { status: "Idle", soc: 0 } });
  assert.match(element("hicon").innerHTML, /<b>0<small>%/);
  context.render({ paired: true, key_present: true, vin: "5YJ3E1EB8NF123456",
                   vehicle: { status: "Idle", soc: 64 } });
  assert.match(element("hicon").innerHTML, /<b>64<small>%/);
});

test("a failed BLE link without any RSSI shows no 0 dBm reading", () => {
  const { context, element } = loadUi();
  context.render({ paired: true, key_present: true, vin: "5YJ3E1EB8NF123456",
                   ble: { connected: false, connect_fail: 9, devices: [] } });
  const row = JSON.stringify([element("bleVal").innerHTML, element("bleSub").innerHTML,
                              element("bleBars") && element("bleBars").innerHTML]);
  assert.doesNotMatch(row, /0 dBm/, "an unknown signal must not read as a perfect 0 dBm link");
});

test("the OTA changelog request has one deadline that also covers a stalled body", async () => {
  const { context } = loadUi();
  let aborted = false;
  context.AbortController = class { constructor() { this.signal = { aborted: false }; }
                                    abort() { aborted = true; this.signal.aborted = true; } };
  // Headers arrive, the body never does: the pre-fix code cleared its timer at the headers.
  context.fetch = async () => ({ ok: true, status: 200, text() { return new Promise(() => {}); } });
  context.setTimeout = (callback) => { queueMicrotask(callback); return 1; };
  const outcome = await Promise.race([
    context.loadOtaChangelog(),
    new Promise((resolve) => setImmediate(() => resolve("hung")))
  ]);
  assert.equal(outcome, "", "a stalled body ends the optional notes instead of hanging the dialog");
  assert.equal(aborted, true, "the stalled exchange is aborted");

  // Normal paths are unchanged.
  const { context: ok } = loadUi();
  ok.fetch = async () => ({ ok: true, status: 200, async text() { return "- fixed things"; } });
  assert.equal(await ok.loadOtaChangelog(), "- fixed things");
  ok.fetch = async () => ({ ok: true, status: 204, async text() { return "ignored"; } });
  assert.equal(await ok.loadOtaChangelog(), "");
  ok.fetch = async () => { throw new Error("offline"); };
  assert.equal(await ok.loadOtaChangelog(), "");
});

test("?pr=<N> on the page selects the PR preview for check and update, strictly", async () => {
  const cases = [
    ["?pr=42", "42"], ["?x=1&pr=7", "7"], ["?pr=1234567", "1234567"],
    ["", ""], ["?pr=", ""], ["?pr=042", ""], ["?pr=0", ""], ["?pr=abc", ""], ["?pr=12345678", ""],
    ["?pr=42x", ""], ["?pr=-1", ""], ["?xpr=42", ""], ["?pr", ""],
    ["?pr=abc&pr=42", ""],   // only the FIRST pr key counts, like the device
  ];
  for (const [search, want] of cases) {
    const { context } = loadUi();
    context.location.search = search;
    assert.equal(context.otaPrSelection(), want, `search ${JSON.stringify(search)}`);
  }

  const { context } = loadUi();
  context.location.search = "?pr=42";
  const urls = [];
  context.fetch = async (url, options) => {
    urls.push({ url, method: (options && options.method) || "GET" });
    if (url.startsWith("/ota/check")) return { ok: true, status: 200, async json() { return { started: true }; } };
    if (url.startsWith("/ota/status")) return { ok: true, status: 200, async json() {
      return { state: "idle", update_available: false, message: "idle", available: "1.6.0", current: "1.6.0" }; } };
    return { ok: true, status: 200, async json() { return { result: true }; } };
  };
  await context.otaCheck();
  const check = urls.find((u) => u.url.startsWith("/ota/check"));
  assert.match(check.url, /^\/ota\/check\?pr=42&ms=\d+$/);

  context.otaAvail = "1.6.0-PR-42";
  await context.startOtaUpdate("1.6.0-PR-42");
  const update = urls.find((u) => u.url.startsWith("/ota/update"));
  assert.equal(update.url, "/ota/update?pr=42");
  assert.equal(update.method, "POST");

  // Without a valid selection the regular channel requests are unchanged.
  const plain = loadUi();
  plain.context.location.search = "?pr=abc";
  const plainUrls = [];
  plain.context.fetch = async (url) => { plainUrls.push(url);
    return { ok: true, status: 200, async json() { return { started: false }; } }; };
  await plain.context.otaCheck();
  assert.match(plainUrls[0], /^\/ota\/check\?ms=\d+$/);
});


test("usable RAM session renders normal vehicle state while durable paired remains false", () => {
  const { context, element } = loadUi();
  const report = Object.freeze({ paired: false, session_available: true, key_present: true,
    vin: "TESTVIN0000000001", link: "awake", ble: { connected: true },
    vehicle: { status: "Charging", soc: 55 } });
  context.render(report);
  assert.equal(report.paired, false, "presentation must not rewrite durable paired");
  assert.match(element("hlabel").innerHTML, /Charging/);
  assert.doesNotMatch(element("hsub").textContent, /pair|send.*key/i);
  context.render({ paired: false, session_available: false, key_present: true,
    vin: "TESTVIN0000000001", ble: { connected: true } });
  assert.match(element("hlabel").innerHTML, /pair/i,
    "unavailable RAM and no durable record keep automatic pairing presentation");
});


test("both JSON helpers send the origin-proof header on GET and POST while preserving caller headers", async () => {
  const { context } = loadUi();
  const requests = [];
  context.fetch = async (url, options) => {
    requests.push({ url, options });
    return { ok: true, status: 200, async json() { return {}; } };
  };
  for (const helper of [context.requestJson, context.requestJsonResult]) {
    await helper("/diag?clear=1");
    const headers = { "Content-Type": "application/json" };
    await helper("/set_time", { method: "POST", headers, body: "{}" });
    assert.deepEqual(headers, { "Content-Type": "application/json" });
  }
  assert.equal(requests.length, 4);
  for (const { options } of requests) {
    assert.equal(options.headers["X-Requested-With"], "XMLHttpRequest");
    if (options.method === "POST") assert.equal(options.headers["Content-Type"], "application/json");
  }
});


test("degraded channel outcomes in either envelope never claim success or start a check", async () => {
  for (const wrapped of [false,true]) for (const outcome of ['unknown','applied_recovery_pending','succeeded']) {
    const {context,element}=loadUi(); const calls=[],messages=[];
    context.otaChannel='release'; context.poll=()=>{};
    context.toast=(message,kind)=>messages.push({message,kind});
    const payload={result:outcome==='succeeded',outcome,reply_degraded:true,reason:'operation outcome unknown; reply degraded'};
    context.fetch=async (url)=>{calls.push(url); return {ok:true,status:200,async json(){return wrapped?{response:payload}:payload;}};};
    element('verLink').focus();
    await context.selectChannel('dev');
    assert.deepEqual(calls,['/set_ota']);
    assert.equal(messages.at(-1).message,payload.reason);
    assert.ok(messages.every(m=>!m.message.includes('Update channel set')));
    assert.equal(context.otaChannelInFlight,false);
    assert.equal(context.document.activeElement,element('verLink'));
    assert.equal(context.fwFocusId,null);
  }
});

test("uncertain OTA starts preserve their reason and never start polling or retrying", async () => {
  for (const action of ['otaCheck','startOtaUpdate']) {
    const {context,element}=loadUi(); const calls=[];
    context.fetch=async (url)=>{ calls.push(url); return {ok:true,status:200,async json(){return {result:false,started:null,outcome:'unknown',reply_degraded:true,reason:'operation outcome unknown; reply degraded'};}}; };
    await context[action]();
    assert.equal(calls.length,1);
    assert.match(calls[0],action==='otaCheck'?/^\/ota\/check/:/^\/ota\/update/);
    assert.match(element('toasts').children[0].innerHTML,/operation outcome unknown; reply degraded/);
  }
});

test("OTA start refusals retain device reasons without polling or retrying", async () => {
  for (const action of ['otaCheck','startOtaUpdate']) {
    for (const status of [409,500,503]) {
      const {context,element}=loadUi(); const calls=[];
      const reason='running firmware is still pending verification <&>';
      context.fetch=async (url,options)=>{
        calls.push({url,options});
        return {ok:false,status,async json(){return {result:false,started:false,reason};}};
      };
      await context[action]();
      assert.equal(calls.length,1);
      assert.equal(calls[0].options.headers['X-Requested-With'],'XMLHttpRequest');
      assert.match(element('toasts').children[0].innerHTML,/pending verification &lt;&amp;&gt;/);
      assert.equal(context.otaBusy,false);
    }
    for (const body of [null,{reason:42},'unreadable']) {
      const {context,element}=loadUi(); const calls=[];
      context.fetch=async (url)=>{ calls.push(url); return {ok:false,status:503,async json(){
        if(body==='unreadable') throw new Error('invalid JSON'); return body;
      }}; };
      await context[action]();
      assert.equal(calls.length,1);
      assert.match(element('toasts').children[0].innerHTML,/HTTP 503/);
    }
  }
});

test("reopening an available PR update retains its preview badge", async () => {
  for (const [search,label] of [
    ['?pr=42','PR preview #42'], ['?x=1&pr=42','PR preview #42'],
    ['?pr=42&pr=garbage','PR preview #42'], ['?pr=42junk','Release'],
    ['?pr=042','Release'], ['?pr=0','Release'], ['?pr=garbage&pr=42','Release'],
    ['?xpr=42','Release'], ['?pr=12345678','Release'],
  ]) {
    const {context,element}=loadUi();
    context.location.search=search; context.otaAvail=label==='Release'?'1.6.0':'1.6.0-PR-42';
    context.fetch=async()=>({ok:false,status:409,async json(){return {result:false,reason:'still pending verification'};}});
    await context.startOtaUpdate(context.otaAvail);
    assert.equal(context.otaBusy,false);
    context.fetch=async()=>({ok:true,status:200,async text(){return 'Preview change';}});
    const opened=context.openFwUpdate();
    await new Promise(resolve=>setImmediate(resolve));
    assert.equal(element('otaChannel').textContent,label,search);
    context.closeOtaModal(false); await opened;
  }
});

test("OTA refusal bodies share the start deadline and cannot leave the UI locked", async () => {
  for (const action of ['otaCheck','startOtaUpdate']) {
    const {context,element}=loadUi(); let expire,body=false,aborted=false,calls=0;
    context.AbortController=class {constructor(){this.signal={};} abort(){aborted=true;}};
    context.setTimeout=(callback,ms)=>{if(ms===context.OTA_HTTP_TIMEOUT_MS) expire=callback; return 1;};
    context.fetch=async()=>{++calls;return {ok:false,status:409,json(){body=true;return new Promise(()=>{});}};};
    const pending=context[action](); await new Promise(resolve=>setImmediate(resolve));
    assert.equal(body,true); assert.equal(typeof expire,'function');
    expire(); await pending;
    assert.equal(aborted,true); assert.equal(calls,1); assert.equal(context.otaBusy,false);
    assert.match(element('toasts').children[0].innerHTML,/request timed out/);
  }
});

test("ordinary HTTP 502 charging uncertainty preserves its reason without retrying", async () => {
  for (const status of ["Stopped", "Charging"]) {
    for (const cause of ["vehicle not reachable", "response timed out", "unreachable", "already_set"]) {
      const { context } = loadUi();
      const calls = [], messages = [];
      const reason = "command outcome unknown; command may have executed; " + cause;
      context.state = { vin: "TESTVIN0000000001", vehicle: { status, soc: 40 } };
      context.poll = () => {};
      context.toast = (message, kind) => messages.push({ message, kind });
      context.fetch = async (url, options) => {
        calls.push({ url, options });
        return { ok: false, status: 502, async json() {
          return { response: { result: false, reason } };
        } };
      };
      await context.toggleCharge();
      assert.equal(calls.length, 1, "an uncertain result must not resend a charging command");
      assert.match(calls[0].url, status === "Charging" ? /command\/charge_stop$/ : /command\/charge_start$/);
      assert.equal(messages.at(-1).message, reason);
      assert.equal(messages.at(-1).kind, "err");
      assert.equal(context.chgBusy, false);
    }
  }
});

test("ordinary HTTP 502 definite charging failures retain their existing presentation", async () => {
  for (const [reason, message, kind] of [
    ["vehicle not reachable", "Car not reachable — is it in range?", "err"],
    ["already_set", "Setting already active on vehicle", "info"],
    ["complete", "Charging is already complete", "info"],
  ]) {
    const { context } = loadUi();
    const messages = [], calls = [];
    context.state = { vin: "TESTVIN0000000001", vehicle: { status: "Stopped", soc: 40 } };
    context.poll = () => {};
    context.toast = (text, type) => messages.push({ text, type });
    context.fetch = async (url) => {
      calls.push(url);
      return { ok: false, status: 502, async json() {
        return { response: { result: false, reason } };
      } };
    };
    await context.toggleCharge();
    assert.equal(calls.length, 1);
    assert.deepEqual(messages.at(-1), { text: message, type: kind });
  }
});

test("degraded command outcomes are not rewritten as vehicle success or range failure", async () => {
  for (const action of ['toggleCharge','wakeCar','genKey']) {
    const {context}=loadUi(); const messages=[],calls=[];
    context.state={vin:'TESTVIN0000000001',key_present:false}; context.poll=()=>{};
    context.toast=(message,kind)=>messages.push({message,kind});
    const payload={result:false,outcome:'unknown',reply_degraded:true,reason:'operation outcome unknown; reply degraded'};
    context.fetch=async (url)=>{calls.push(url);return {ok:true,status:200,async json(){return action==='genKey'?payload:{response:payload};}};};
    await context[action]();
    assert.equal(calls.length,1);
    assert.equal(messages.at(-1).message,payload.reason);
    assert.equal(messages.at(-1).kind,'err');
  }
});


test("boot sends its actual clock fallback through the common mutation header policy", async () => {
  const { context } = loadUi();
  const calls = [];
  context.fetch = async (url, options) => {
    calls.push({ url, options });
    return { ok: true, status: 200, async json() { return { result: true }; } };
  };
  context.poll = () => {};
  context.resumeOta = () => {};
  context.boot();
  await Promise.resolve();
  assert.equal(calls.length, 1);
  const { url, options } = calls[0];
  assert.equal(url, "/set_time");
  assert.equal(options.method, "POST");
  assert.equal(options.headers["Content-Type"], "application/json");
  assert.equal(options.headers["X-Requested-With"], "XMLHttpRequest");
  const body = JSON.parse(options.body);
  assert.deepEqual(Object.keys(body), ["ms"]);
  assert.equal(typeof body.ms, "number");
  assert.ok(Number.isFinite(body.ms) && body.ms > 0);
});


test("stored session date is labeled as first observation rather than pairing event", () => {
  const { context, element } = loadUi();
  context.render({paired: true, paired_at: 1800000000, ble: {connected: true}});
  assert.match(element("vehSub").textContent, /Session first observed/);
  assert.doesNotMatch(element("vehSub").textContent, /Paired [A-Za-z]+ \d/);
});


test("config saves preserve server firmware-verification reasons without retry", async () => {
  for (const method of ["editMqtt", "editSyslog"]) for (const degraded of [false, true]) {
    const { context } = loadUi();
    const calls = [], messages = [];
    const reason = "configuration saved for the next boot; firmware verification is uncertain";
    context.state = { mqtt: { broker: "" }, syslog: { host: "" } };
    context.askText = async () => method === "editMqtt" ? "192.0.2.50:1883" : "192.0.2.50:514";
    context.toast = (message, kind) => messages.push({ message, kind });
    context.poll = () => {};
    context.fetch = async (url) => {
      calls.push(url);
      return { ok: true, status: 200, async json() {
        return { response: { result: true, reason, firmware_verification: "mark_outcome_unknown",
          ...(degraded ? { outcome: "succeeded", reply_degraded: true } : {}) } };
      } };
    };
    await context[method]();
    assert.equal(messages.at(-1).message, reason);
    assert.equal(messages.at(-1).kind, "info");
    assert.equal(calls.length, 1);
  }
});


test("failed and stalled status polls label cached values and recover without overlapping requests", async () => {
  const { context, element } = loadUi();
  const frame = {paired: true, key_present: true, vin: "5YJ3E1EB8NF123456", vehicle: {status: "Idle", soc: 64}};
  context.render(frame);
  const oldGauge = element("hicon").innerHTML;
  context.fetch = async () => { throw new TypeError("offline"); };
  await context.poll();
  assert.equal(context.feedOk, false);
  assert.equal(element("feedStatus").classList.contains("hide"), false);
  assert.match(element("feedStatus").textContent, /unavailable.*last received/i);
  assert.equal(element("hicon").innerHTML, oldGauge);
  let requests = 0, deadline;
  context.setTimeout = fn => { deadline = fn; return 1; };
  context.fetch = async () => { requests++; return new Promise(() => {}); };
  const first = context.poll();
  assert.equal(context.poll(), first);
  assert.equal(requests, 1);
  deadline();
  await first;
  assert.equal(context.feedOk, false);
  assert.equal(context.statusPoll, null);
  context.fetch = async () => ({ok: true, json: async () => frame});
  await context.poll();
  assert.equal(context.feedOk, true);
  assert.equal(element("feedStatus").classList.contains("hide"), true);
});

test("cached battery and elapsed fields reject nonnumeric values and preserve zero", () => {
  const {context} = loadUi();
  for(const value of [null, undefined, "", " ", "no", NaN, Infinity, {}, [], true, false]) {
    const result = context.lastChips({last: {usable_soc: value, soc: value}, last_seen_s: value});
    assert.equal(result.soc, null, String(value));
    assert.equal(result.html, "", String(value));
  }
  assert.equal(context.lastChips({last: {usable_soc: "bad", soc: 0}, last_seen_s: 0}).soc, 0);
  assert.equal(context.lastChips({last: {soc: "64.6"}}).soc, 65);
  assert.equal(context.lastChips({last: {soc: 200}}).soc, 100);
  assert.equal(context.lastChips({last: {soc: -9}}).soc, 0);
});


test("terminal runtime and cached key recovery reasons replace automatic-pairing optimism", () => {
  const {context, element} = loadUi();
  const base = {paired: true, key_present: true, vin: "5YJ3E1EB8NF123456", reauth: true,
                vehicle: {status: "Charging", soc: 70}};
  context.render({...base, sys: {runtime_state: "fatal"}});
  assert.match(element("hlabel").innerHTML, /Device recovery required/);
  assert.equal(context.heroActFn, null);
  assert.doesNotMatch(element("hicon").innerHTML, /onclick=/);
  for(const reason of ["key_reload_required", "key_runtime_mismatch", "vin_transition_pending",
                       "key_cleanup_pending", "key_storage_unavailable"]) {
    context.render({...base, pairing_blocked: reason});
    assert.match(element("hlabel").innerHTML, /Pairing paused/);
    assert.equal(context.heroActFn, null);
    assert.doesNotMatch(element("hsub").textContent, /sent automatically|Reconnecting automatically/);
  }
  context.render({...base, paired: false, reauth: false, vehicle: null, ble: {connected: true}});
  assert.match(element("hsub").textContent, /key is sent automatically/);
});


test("both hero failure branches consume the parity-checked warning threshold", () => {
  for (const paired of [false,true]) {
    const {context,element}=loadUi();
    context.BLE_CONNECT_FAIL_WARN=7;
    for(const failures of [6,7]){
      context.render({paired,key_present:true,vin:"5YJ3E1EA7KF000316",link:"unreachable",
                      ble:{connected:false,connect_fail:failures,devices:[]}});
      assert.equal(/Connection failed/.test(element("hlabel").innerHTML),failures===7);
    }
  }
});


// Recovery faults reuse the existing recovery region; normal enrolment stays automatic.
test("F142 recovery banner distinguishes fatal and cached identity states", () => {
  const {context, element} = loadUi();
  const base = {paired: true, key_present: true, vin: "5YJ3E1EA7KF000316", link: "idle"};
  const banner = element("safeModeBanner");
  for (const [component, escaped] of [
    ["<script>", "&lt;script&gt;"],
    ["<SCRIPT>alert(1)</SCRIPT>", "&lt;SCRIPT&gt;alert(1)&lt;/SCRIPT&gt;"],
    ['<ScRiPt>alert(1)</ScRiPt foo="bar">', '&lt;ScRiPt&gt;alert(1)&lt;/ScRiPt foo=&quot;bar&quot;&gt;'],
    ["<img src=x onerror='alert(1)'>&", "&lt;img src=x onerror=&#39;alert(1)&#39;&gt;&amp;"]
  ]) {
    context.render({...base, identity_state: "ok", sys: {runtime_state: "fatal", fatal_component: component}});
    assert.equal(banner.classList.contains("show"), true);
    assert.equal(banner.querySelector(".bt").innerHTML,
      '<b>Device recovery required</b> — ' + escaped +
      '. Vehicle services are stopped. Retain diagnostics and resolve the cause before restarting.', component);
    assert.equal(banner.querySelector("button").hidden, false);
    assert.equal(context.heroActFn, null);
  }
  for (const identity of ["reboot_required", "cleanup_pending", "vin_transition", "storage_unavailable"]) {
    context.render({...base, identity_state: identity, sys: {runtime_state: "ready"}});
    assert.equal(banner.classList.contains("show"), true, identity);
    assert.match(banner.querySelector(".bt").innerHTML, /Identity recovery/);
    assert.equal(banner.querySelector("button").hidden, true);
    assert.equal(context.heroActFn, null);
  }
  for (const identity of ["ok", "unknown", undefined]) {
    context.render({...base, paired: false, identity_state: identity, ble: {connected: true},
                    sys: {runtime_state: "ready"}});
    assert.equal(banner.classList.contains("show"), false);
    assert.match(element("hsub").textContent, /key is sent automatically/);
  }
});
