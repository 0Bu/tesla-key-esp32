#!/usr/bin/env python3
"""Exercise the assembled device page in a real, dependency-free headless browser."""

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


def browser_binary() -> str | None:
    configured = os.environ.get("BROWSER_BIN")
    if configured:
        return shutil.which(configured) or (configured if Path(configured).is_file() else None)
    for name in ("google-chrome", "google-chrome-stable", "chromium", "chromium-browser"):
        found = shutil.which(name)
        if found:
            return found
    mac = Path("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")
    return str(mac) if mac.is_file() else None


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
  try {
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

    for (const id of ["otaStat", "toasts"]) {
      const region = document.getElementById(id);
      check(region && region.getAttribute("role") === "status", id + " status role");
      check(region && region.getAttribute("aria-live") === "polite", id + " polite live region");
    }

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

    toast('<img src=x onerror="console.error(1)">', "err");
    const toastRegion = document.getElementById("toasts");
    check(!toastRegion.querySelector("img"), "toast content remains escaped");
    check(toastRegion.textContent.includes("<img"), "escaped toast remains readable");

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
            returncode = process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            # Chrome for Testing on macOS can finish --dump-dom and then remain alive while its
            # updater/crash helper drains. Terminate the isolated process group; a completed DOM
            # below is still authoritative, while an incomplete dump remains a hard failure.
            timed_out = True
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                returncode = process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                returncode = process.wait(timeout=2)
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
    print("web-ui-browser: PASS (real DOM, console, keyboard, live regions, desktop/mobile layout)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
