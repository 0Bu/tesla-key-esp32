#!/usr/bin/env python3
"""Exercise the assembled device page in a real Linux headless browser."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
# GitHub's shared Linux runners can spend more than 20 seconds cold-starting Chrome under load.
# Two profiles at 45 seconds plus bounded termination grace remain below run-mock-tests.sh's
# 120-second outer gate, while a genuinely hung browser still fails closed.
PROFILE_TIMEOUT_SECONDS = 45
TERMINATION_GRACE_SECONDS = 2


def browser_binary() -> str | None:
    if sys.platform != "linux":
        return None
    configured = os.environ.get("BROWSER_BIN")
    if configured:
        return shutil.which(configured) or (configured if Path(configured).is_file() else None)
    for name in ("google-chrome", "google-chrome-stable", "chromium", "chromium-browser"):
        found = shutil.which(name)
        if found:
            return found
    return None


def assembled_page() -> str:
    html = (ROOT / "main/www/index.html").read_text(encoding="utf-8")
    css = (ROOT / "main/www/style.css").read_text(encoding="utf-8")
    app = (ROOT / "main/www/app.js").read_text(encoding="utf-8")
    css_marker = "/*@@INLINE:style.css@@*/\n"
    js_marker = "//@@INLINE:app.js@@\n"
    if html.count(css_marker) != 1 or html.count(js_marker) != 1:
        raise RuntimeError("inline asset markers must each occur exactly once")
    if css_marker.strip() in css or js_marker.strip() in css or css_marker.strip() in app or js_marker.strip() in app:
        raise RuntimeError("an inline asset contains an assembly marker")

    prelude = r'''
window.__TESLA_UI_NO_BOOT__ = true;
window.__browserGateErrors = [];
// The gate is a synchronous DOM contract. Suppress application polling/toast timers so Chrome's
// dump-dom lifecycle is deterministic and cannot be held open by background UI work.
window.setTimeout = function() { return 1; };
window.clearTimeout = function() {};
window.setInterval = function() { return 1; };
window.clearInterval = function() {};
window.__browserGateFetchCalls = 0;
window.fetch = function() {
  window.__browserGateFetchCalls += 1;
  return Promise.reject(new TypeError("browser-gate offline fixture"));
};
window.addEventListener("error", function(event) {
  window.__browserGateErrors.push(String(event.message || event.error || "window error"));
});
window.addEventListener("unhandledrejection", function(event) {
  window.__browserGateErrors.push(String(event.reason || "unhandled rejection"));
});
const __realConsoleError = console.error.bind(console);
console.error = function() {
  window.__browserGateErrors.push(Array.from(arguments).join(" "));
  __realConsoleError.apply(console, arguments);
};
'''
    checks = r'''
;(async function browserGate() {
  const failures = [];
  const check = (condition, message) => { if (!condition) failures.push(message); };
  const pixel = document.createElement("canvas").getContext("2d", {willReadFrequently:true});
  function luminance(color) {
    pixel.clearRect(0,0,1,1); pixel.fillStyle=color; pixel.fillRect(0,0,1,1);
    const rgba=pixel.getImageData(0,0,1,1).data;
    check(rgba[3]===255,"contrast fixture requires opaque computed colors");
    return [0.2126,0.7152,0.0722].reduce((sum,weight,i)=>{
      const c=rgba[i]/255;
      return sum+weight*(c<=0.04045?c/12.92:Math.pow((c+0.055)/1.055,2.4));
    },0);
  }
  function contrast(foreground,background) {
    const a=luminance(foreground), b=luminance(background);
    return (Math.max(a,b)+0.05)/(Math.min(a,b)+0.05);
  }
  function textContrast(element,label) {
    const style=getComputedStyle(element);
    check(style.filter==="none",label+" contrast requires unfiltered colors");
    check(contrast(style.color,style.backgroundColor)>=4.5,label+" contrast >=4.5");
  }
  try {
    const dark=location.search.includes("profile=mobile");
    check(matchMedia("(prefers-color-scheme: dark)").matches===dark,"isolated browser color scheme");
    // dump-dom may deactivate its window. Exercise the actual CSS cascade deterministically:
    // attributes have the same specificity as these pseudo-classes; declarations stay unchanged.
    const stateRules=[];
    function emulateStates(rules){
      for(const rule of rules){
        if(rule.selectorText && /:(focus-visible|focus|hover)(?![-\w])/.test(rule.selectorText)){
          const original=rule.selectorText;
          stateRules.push([rule,original]);
          rule.selectorText=original.replace(/:(focus-visible|focus|hover)(?![-\w])/g,
            (_,state)=>":is(:"+state+",[data-ui-"+state+"])");
        }
        if(rule.cssRules) emulateStates(rule.cssRules);
      }
    }
    for(const sheet of document.styleSheets) emulateStates(sheet.cssRules);
    check(stateRules.length>0,"actual interactive CSS rules are available");
    check(document.documentElement.lang === "en", "document language");
    check(!!document.querySelector('meta[name="viewport"]'), "viewport metadata");
    const ids = Array.from(document.querySelectorAll("[id]"), element => element.id);
    check(new Set(ids).size === ids.length, "duplicate IDs");

    const buttons = Array.from(document.querySelectorAll("button"));
    check(buttons.length >= 5, "expected interactive controls");
    for (const button of buttons) {
      const name = (button.getAttribute("aria-label") || button.getAttribute("title") || button.textContent || "").trim();
      check(name.length > 0, "button without accessible name");
      check(button.tabIndex >= 0 && !button.disabled, "button is not keyboard focusable");
    }

    const liveRegion = document.getElementById("toasts");
    check(liveRegion && liveRegion.getAttribute("role") === "status", "toasts status role");
    check(liveRegion && liveRegion.getAttribute("aria-live") === "polite", "toasts polite live region");
    const otaStat = document.getElementById("otaStat");
    check(otaStat && !otaStat.hasAttribute("aria-live") && otaStat.getAttribute("aria-hidden") === "true",
          "otaStat is not a second live region");

    check(typeof requestJson === "function" && typeof toast === "function" && typeof render === "function",
          "assembled application functions");
    let requestRejected = false;
    try {
      await requestJson("/browser-gate-offline", {cache: "no-store"});
    } catch (error) {
      requestRejected = error instanceof TypeError && String(error.message).includes("offline fixture");
    }
    check(requestRejected, "requestJson propagates a transport failure");
    feedOk = true;
    await poll();
    check(feedOk === false, "status poll marks the live feed unavailable after transport failure");
    check(window.__browserGateFetchCalls === 2, "browser request-failure fixture call count");
    const feedStatus = document.getElementById("feedStatus");
    check(feedStatus && !feedStatus.classList.contains("hide") && feedStatus.textContent.includes("Device unavailable"),
          "failed status feed has a visible accessible notice");


    toast('<img src=x onerror="console.error(1)">', "err");
    const toastRegion = document.getElementById("toasts");
    check(!toastRegion.querySelector("img"), "toast content remains escaped");
    check(toastRegion.textContent.includes("<img"), "escaped toast remains readable");
    for(const kind of ["info","ok","err"]){
      const notification=toast("Contrast fixture",kind,"contrast");
      textContrast(notification.querySelector(".ic"),kind+" toast icon");
    }
    for(const kind of ["btn primary","ib avail"]){
      const control=document.createElement("button"); control.className=kind; control.textContent="Fixture";
      document.body.appendChild(control); textContrast(control,kind);
      control.setAttribute("data-ui-hover",""); textContrast(control,kind+" hover"); control.remove();
    }

    render({paired: true, last_seen_s: 0,
      vehicle: {status: "Stopped", actual_amps: 16, volts: 230, phases: 2},
      tele: {climate: {cop: "FanOnly", front_defrost: true, rear_defrost: true, defrost_mode: "Max"}}});
    let primaryClicks = 0;
    const actualHeroAction = heroActFn;
    heroActFn = function(){ primaryClicks++; };
    document.getElementById("hero").click();
    document.getElementById("hlabel").click();
    document.getElementById("hstats").click();
    check(primaryClicks === 0, "informational hero clicks never dispatch a vehicle action");
    const primaryButton = document.querySelector("#hicon button");
    check(primaryButton && primaryButton.getAttribute("aria-label"), "primary action is a named button");
    primaryButton.focus();
    check(document.activeElement===primaryButton,"primary action receives DOM focus");
    primaryButton.setAttribute("data-ui-focus","");
    primaryButton.setAttribute("data-ui-focus-visible","");
    const primaryStyle=getComputedStyle(primaryButton);
    check(primaryStyle.outlineStyle!=="none" && parseFloat(primaryStyle.outlineWidth)>=2,
          "primary action has a visible focus ring in the emulated keyboard state");
    check(contrast(primaryStyle.outlineColor,getComputedStyle(document.getElementById("hero")).backgroundColor)>=3,
          "primary action focus ring contrast >=3");
    primaryButton.removeAttribute("data-ui-focus"); primaryButton.removeAttribute("data-ui-focus-visible");
    for(const [rule,original] of stateRules) rule.selectorText=original;
    primaryButton.click();
    check(primaryClicks === 1, "only the gauge button invokes the primary action once");
    heroActFn = actualHeroAction;
    const climateChips = document.getElementById("hstats");
    check(climateChips.textContent.includes("Overheat setting"), "COP is a setting");
    check(climateChips.textContent.includes("Fan only"), "COP setting is readable");
    check(climateChips.textContent.includes("Defrost (cached)"), "defrost remains explicitly cached after fresh charge contact");
    check(!climateChips.textContent.includes("kW"), "charger fields are not attributed to climate power");
    check(Array.from(climateChips.children).every(chip => chip.title.includes("current activity and power are unknown")),
          "climate titles disclose activity and power limits");
    const checkChipBounds = () => {
      for (const chip of climateChips.children) {
        check(chip.scrollWidth <= chip.clientWidth + 1, "chip content fits its tile");
        for (const text of chip.children) {
          check(text.scrollWidth <= text.clientWidth + 1, "chip label/value wraps within its tile");
        }
      }
    };
    checkChipBounds();
    render({paired: true, last_seen_s: 0,
      vehicle: {status: "Charging", power: 11, amps: 16, actual_amps: 16, volts: 230, phases: 2},
      tele: {climate: {cop: "FanOnly", front_defrost: true, rear_defrost: true, defrost_mode: "Max"}}});
    checkChipBounds();
    check(window.__browserGateFetchCalls === 2, "rendering climate does not request vehicle data");

    const notesList = document.getElementById("otaChanges");
    const otaCard = document.querySelector("#otaModal .sheet");
    const update = {current: "1.6.2-PR-400", available: "1.6.3-dev.19", channel: "dev"};
    const visibleNotes = () => {
      const box = notesList.getBoundingClientRect();
      return Array.from(notesList.children).filter(item => {
        const row = item.getBoundingClientRect();
        return row.top >= box.top - 1 && row.bottom <= box.bottom + 1;
      }).length;
    };
    const checkNotesLayout = label => {
      check(visibleNotes() <= 5, label + ": at most five complete entries visible");
      check(notesList.scrollWidth <= notesList.clientWidth + 1, label + ": notes wrap horizontally");
      check(otaCard.scrollHeight <= otaCard.clientHeight + 1, label + ": only the notes need scrolling");
      const cardBox = otaCard.getBoundingClientRect();
      check(cardBox.left >= 0 && cardBox.right <= window.innerWidth + 1 &&
            cardBox.top >= 0 && cardBox.bottom <= window.innerHeight + 1,
            label + ": dialog stays inside the viewport");
      for (const id of ["otaHelp", "otaCancel", "otaInstall"]) {
        const box = document.getElementById(id).getBoundingClientRect();
        check(box.top >= cardBox.top && box.bottom <= cardBox.bottom,
              label + ": help and actions stay visible: " + id);
      }
    };
    for (const count of [0, 1, 5, 6, 13]) {
      const notes = Array.from({length: count}, (_, i) => "Change " + (i + 1));
      askOtaInstall(update, notes.join("\n"));
      check(notesList.children.length === count, "all " + count + " notes retained");
      check(notesList.scrollTop === 0, "new dialog starts at the first note");
      checkNotesLayout(count + " short notes");
      if (count <= 5) {
        check(notesList.hidden === (count === 0), "empty list visibility");
        if (count) check(visibleNotes() === count, "short list fits without truncation");
      } else {
        check(notesList.scrollHeight > notesList.clientHeight, "long list scrolls");
        check(notesList.tabIndex === 0 && notesList.getAttribute("aria-label"), "notes are keyboard reachable and named");
        notesList.focus();
        check(document.activeElement === notesList, "scrolling list receives keyboard focus");
        notesList.scrollTop = notesList.scrollHeight;
        checkNotesLayout("last of " + count + " notes");
        const last = notesList.lastElementChild.getBoundingClientRect();
        check(last.bottom <= notesList.getBoundingClientRect().bottom + 1, "last note can be read");
      }
      closeOtaModal(false);
    }
    askOtaInstall(update, Array.from({length: 13}, (_, i) => i < 5
      ? "Change " + i + " — " + "A wrapped maintenance and reliability improvement. ".repeat(3)
      : "Change " + i).join("\n"));
    checkNotesLayout("mixed wrapped notes");
    notesList.scrollTop = notesList.scrollHeight;
    checkNotesLayout("short notes after wrapped notes");
    otaCard.style.width = "300px";
    window.dispatchEvent(new Event("resize"));
    checkNotesLayout("resized dialog");
    notesList.scrollTop = notesList.scrollHeight;
    checkNotesLayout("resized last notes");
    otaCard.style.width = "";
    closeOtaModal(false);
    askOtaInstall(update, Array.from({length: 6}, (_, i) =>
      "Change " + i + " — " + "A maintenance improvement. ".repeat(3)).join("\n"));
    const wideLimit = parseFloat(notesList.style.maxHeight);
    otaCard.style.width = "260px";
    window.dispatchEvent(new Event("resize"));
    check(parseFloat(notesList.style.maxHeight) > wideLimit, "five-note height follows narrower text wrapping");
    checkNotesLayout("all wrapped notes after resize");
    otaCard.style.width = "";
    closeOtaModal(false);
    check(window.__browserGateFetchCalls === 2, "reading notes does not contact the device");

    const offlineFetch = window.fetch, originalPoll = poll;
    poll = function() {};
    for (const status of ["Stopped", "Charging"]) {
      for (const cause of ["vehicle not reachable", "response timed out", "already_set"]) {
        const requests = [];
        const reason = "command outcome unknown; command may have executed; " + cause;
        state = {vin: "TESTVIN0000000001", vehicle: {status, soc: 40}};
        window.fetch = async function(url, options) {
          requests.push({url, options});
          return {ok: false, status: 502, json: async function() {
            return {response: {result: false, reason}};
          }};
        };
        await toggleCharge();
        const message = toastRegion.querySelector('[data-key="charge"]');
        check(message && message.lastElementChild.textContent === reason,
              "HTTP 502 charging uncertainty remains visible: " + status + "/" + cause);
        check(message && message.classList.contains("err"), "uncertain charge is not informational success");
        check(requests.length === 1 && requests[0].options.method === "POST",
              "uncertain charge sends exactly one command request");
        check(!chgBusy, "uncertain charge releases its UI busy state");
      }
    }
    window.fetch = offlineFetch;
    poll = originalPoll;

    check(getComputedStyle(document.body).display !== "none", "body is rendered");
    check(document.querySelector(".wrap").getBoundingClientRect().width > 0, "main layout has width");
    check(document.documentElement.scrollWidth <= window.innerWidth + 1, "no horizontal overflow");
  } catch (error) {
    failures.push("gate exception: " + (error && error.stack ? error.stack : error));
  }
  failures.push(...window.__browserGateErrors);
  const result = document.createElement("pre");
  result.id = "browser-gate-result";
  if (failures.length) {
    result.dataset.result = "fail";
    result.textContent = "FAIL\n" + failures.join("\n");
    document.title = "BROWSER_GATE_FAIL";
  } else {
    result.dataset.result = "pass";
    result.textContent = "PASS";
    document.title = "BROWSER_GATE_PASS";
  }
  document.body.appendChild(result);
})();
'''
    gate_css = css + "\n*,*::before,*::after{animation:none!important;transition:none!important}\n"
    return html.replace(css_marker, gate_css).replace(js_marker, prelude + app + checks)


def run_profile(browser: str, html: Path, profile: str, size: str, user_data: Path) -> None:
    if sys.platform != "linux":
        raise RuntimeError("automatic browser processes are supported only on Linux")
    command = [
        browser,
        "--headless",
        "--disable-gpu",
        "--disable-background-networking",
        "--disable-breakpad",
        "--disable-component-update",
        "--disable-crash-reporter",
        "--disable-default-apps",
        "--disable-dev-shm-usage",
        "--disable-extensions",
        "--disable-sync",
        "--metrics-recording-only",
        "--mute-audio",
        "--no-first-run",
        f"--user-data-dir={user_data}",
        f"--window-size={size}",
        "--dump-dom",
        html.as_uri() + f"?profile={profile}",
    ]
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        command.insert(1, "--no-sandbox")
    if profile == "mobile":
        command.insert(1, "--force-dark-mode")
    timed_out = False
    with tempfile.TemporaryFile(mode="w+", encoding="utf-8") as stdout_file, \
            tempfile.TemporaryFile(mode="w+", encoding="utf-8") as stderr_file:
        process = subprocess.Popen(
            command,
            text=True,
            stdout=stdout_file,
            stderr=stderr_file,
            start_new_session=True,
        )
        try:
            returncode = process.wait(timeout=PROFILE_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            # A browser can finish --dump-dom while a child process is still draining.
            # Terminate only this test's isolated process group; a completed DOM
            # below is still authoritative, while an incomplete dump remains a hard failure.
            timed_out = True
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                returncode = process.wait(timeout=TERMINATION_GRACE_SECONDS)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                returncode = process.wait(timeout=TERMINATION_GRACE_SECONDS)
        stdout_file.seek(0)
        stderr_file.seek(0)
        stdout = stdout_file.read()
        stderr = stderr_file.read()

    pass_marker = 'id="browser-gate-result" data-result="pass"'
    complete_dom = pass_marker in stdout and "</html>" in stdout.lower()
    if timed_out and not complete_dom:
        raise RuntimeError(f"{profile}: browser timed out before a complete DOM: {stderr[-1000:]}")
    if not timed_out and returncode != 0:
        raise RuntimeError(f"{profile}: browser exited {returncode}: {stderr[-1000:]}")
    if not complete_dom:
        marker = "browser-gate-result"
        at = stdout.find(marker)
        excerpt = stdout[max(0, at - 200):at + 1200] if at >= 0 else stdout[-1200:]
        raise RuntimeError(f"{profile}: DOM/accessibility gate failed: {excerpt}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--require-browser", action="store_true")
    args = parser.parse_args()
    if sys.platform != "linux":
        message = ("web-ui-browser: automatic native browser launch is disabled outside Linux; "
                   "run scripts/run-browser-tests-linux.sh or the Linux CI gate")
        if args.require_browser:
            print(message, file=sys.stderr)
            return 2
        print(message + "; SKIP (no real-browser evidence)")
        return 0
    browser = browser_binary()
    if not browser:
        message = "web-ui-browser: no Chrome/Chromium binary found"
        if args.require_browser:
            print(message, file=sys.stderr)
            return 2
        print(message + "; SKIP (use --require-browser in CI)")
        return 0
    try:
        with tempfile.TemporaryDirectory(prefix="tesla-key-browser-gate-") as directory:
            temporary = Path(directory)
            page = temporary / "device.html"
            page.write_text(assembled_page(), encoding="utf-8")
            run_profile(browser, page, "desktop", "1200,900", temporary / "desktop-profile")
            run_profile(browser, page, "mobile", "390,844", temporary / "mobile-profile")
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"web-ui-browser: {exc}", file=sys.stderr)
        return 1
    print("web-ui-browser: PASS (real DOM, console, emulated focus/hover CSS, light/dark contrast, live regions, desktop/mobile layout)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
