#!/usr/bin/env python3
#
# capture_wake.py — watch what the Tesla actually returns over BLE when a wake is
# requested, by tailing the ESP32's in-memory /diag log + /status.
#
# Motivation: during a vehicle software update the always-on VCSEC body controller
# keeps answering BLE while the main computer is busy, so the firmware can read the
# car as "asleep" (see docs / link_state()). A wake then either no-ops ("already
# awake") or reports failure. The ONLY way to know which — and what the car really
# sends back — is to capture it live. This tool does that.
#
# What it captures:
#   • /diag (ring buffer of all esp_log output) deduplicated across polls, filtered
#     to the wake-relevant lines. With verbose=1 the firmware also logs each raw RX
#     frame ("RX notify len=N: <hex>") — that hex IS the car's literal answer,
#     including the empty authenticated wake-ack that has no commandStatus.
#   • /status link state (awake/asleep/unreachable/unknown) + charge, printed on
#     every change — the firmware's interpretation, to correlate with the frames.
#   Everything (full new diag text + status snapshots) is also written verbatim to a
#   timestamped logfile for later review.
#
# Lines worth looking for (what they mean):
#   RX notify len=… : <hex>                         raw bytes the car returned
#   Sent Wake Command                               library queued/sent the VCSEC wake
#   Response timeout for command: Wake (attempt x/6) empty ack → library retrying (~1s each)
#   Wake response timeout but vehicle is awake …     library gave up but saw AWAKE → OK
#   'Wake' timed out                                 firmware send_vcsec_ 9s window elapsed
#   vehicleSleepStatus: AWAKE|ASLEEP|UNKNOWN         the car's self-reported sleep flag (DEBUG build only)
#   whitelist / authentication failed                pairing/role fault (NOT expected on a wake)
#
# Usage:
#   scripts/capture_wake.py [ESP32_URL]
#   scripts/capture_wake.py http://tesla-key.local
#   scripts/capture_wake.py http://192.168.1.194 --wake          # also fire one wake_up itself
#   scripts/capture_wake.py --interval 0.5 --duration 120        # faster poll, auto-stop after 2 min
#
# Then tap "Wake" in the web UI (or pass --wake) and watch. Ctrl-C to stop; the tool
# turns verbose RX back off on exit.
#
# Stdlib only — no pip installs. macOS/Linux python3.

import argparse
import datetime as dt
import json
import re
import signal
import sys
import time
import urllib.request
import urllib.error

# Lines we surface to the console. Everything still goes to the logfile regardless.
WAKE_RE = re.compile(
    r"RX notify|TX |"
    r"Wake|wake|"
    r"[sS]leep[sS]tatus|SleepState|"
    r"commandStatus|vehicleStatus|RKE_ACTION|VCSEC|"
    r"Command (completed|failed|skipped)|"
    r"timed out|timeout|"
    r"whitelist|authentication failed|asleep|AWAKE|ASLEEP",
)

stop = False


def _on_signal(_sig, _frame):
    global stop
    stop = True


def http_get(url, timeout=10):
    req = urllib.request.Request(url, method="GET")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def http_post(url, timeout=20):
    req = urllib.request.Request(url, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def new_suffix(prev, cur):
    """Return the part of `cur` that is new vs `prev`.

    /diag is a wrapping ring buffer dumped oldest->newest: between polls new bytes
    appear at the END and old bytes may drop from the FRONT. So `cur` is
    (surviving tail of prev) + (new suffix), and `cur`'s head is exactly that
    surviving tail -> it is both a suffix of `prev` and a prefix of `cur`. The
    overlap is the longest k with prev[-k:] == cur[:k]; the new content is cur[k:].
    Largest-k-first, so we never under-cut. If nothing matches the buffer turned
    over completely between polls -> treat all of cur as new (may duplicate a little).
    """
    if not prev:
        return ""  # first poll establishes the baseline; don't dump stale history
    for k in range(min(len(prev), len(cur)), 0, -1):
        if prev[-k:] == cur[:k]:
            return cur[k:]
    return cur


def ts():
    return dt.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def main():
    ap = argparse.ArgumentParser(description="Capture what the Tesla returns on a wake request.")
    ap.add_argument("url", nargs="?", default="http://tesla-key.local",
                    help="ESP32 base URL (default http://tesla-key.local)")
    ap.add_argument("--wake", action="store_true",
                    help="actively POST one wake_up after baseline (VIN read from /status)")
    ap.add_argument("--interval", type=float, default=1.0, help="poll interval seconds (default 1.0)")
    ap.add_argument("--duration", type=float, default=0, help="auto-stop after N seconds (0 = until Ctrl-C)")
    args = ap.parse_args()
    base = args.url.rstrip("/")

    signal.signal(signal.SIGINT, _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)

    # Probe + read VIN / initial link.
    try:
        st = json.loads(http_get(base + "/status"))
    except (urllib.error.URLError, OSError, ValueError) as e:
        print(f"ERROR: cannot reach {base}/status — {e}", file=sys.stderr)
        print("Pass the device URL/IP, e.g.:  scripts/capture_wake.py http://192.168.1.194", file=sys.stderr)
        return 2
    vin = st.get("vin", "")
    link = st.get("link", "?")
    paired = st.get("paired", False)

    logname = "wake-capture-" + dt.datetime.now().strftime("%Y%m%d-%H%M%S") + ".log"
    log = open(logname, "w", encoding="utf-8")

    def out(line):
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    out(f"# capture_wake {base}  vin={vin or '(unset)'}  paired={paired}  link={link}")
    out(f"# logfile: {logname}")

    # Clear the ring + enable raw-RX logging so each frame the car sends is captured.
    try:
        http_get(base + "/diag?clear=1&verbose=1")
        out(f"[{ts()}] verbose RX enabled, ring cleared — watching. Tap Wake in the UI (or use --wake).")
    except (urllib.error.URLError, OSError) as e:
        out(f"[{ts()}] WARN: could not set verbose/clear — {e}")

    prev_diag = ""
    last_link = link
    last_charge = None
    started = time.monotonic()
    fired_wake = False

    while not stop:
        # Optionally fire the wake ourselves, once, after the first baseline poll.
        if args.wake and not fired_wake:
            fired_wake = True
            if not vin or vin == "UNKNOWN":
                out(f"[{ts()}] --wake requested but no VIN configured; skipping active wake.")
            else:
                url = f"{base}/api/1/vehicles/{vin}/command/wake_up"
                out(f"[{ts()}] >>> POST {url}")
                try:
                    resp = http_post(url, timeout=30)
                    out(f"[{ts()}] <<< wake_up response: {resp.strip()}")
                except (urllib.error.URLError, OSError) as e:
                    out(f"[{ts()}] <<< wake_up error: {e}")

        # Pull new diag and surface the interesting lines.
        try:
            cur = http_get(base + "/diag")
            delta = new_suffix(prev_diag, cur)
            prev_diag = cur
            for line in delta.splitlines():
                if line.strip():
                    log.write(line + "\n")  # full delta to file
            log.flush()
            for line in delta.splitlines():
                if WAKE_RE.search(line):
                    print(f"[{ts()}] {line.rstrip()}", flush=True)
        except (urllib.error.URLError, OSError) as e:
            out(f"[{ts()}] WARN: /diag fetch failed — {e}")

        # Status transitions (link + charge), printed only on change.
        try:
            st = json.loads(http_get(base + "/status"))
            link = st.get("link", "?")
            veh = st.get("vehicle") or st.get("last") or {}
            charge = (veh.get("soc"), veh.get("status"))
            if link != last_link:
                out(f"[{ts()}] === link: {last_link} -> {link}  (paired={st.get('paired')})")
                last_link = link
            if charge != last_charge and any(c is not None for c in charge):
                out(f"[{ts()}] === charge: soc={charge[0]} status={charge[1]}")
                last_charge = charge
        except (urllib.error.URLError, OSError, ValueError):
            pass

        if args.duration and (time.monotonic() - started) >= args.duration:
            break
        time.sleep(args.interval)

    # Be a good citizen: turn raw RX back off (it adds BLE-task chatter + log volume).
    try:
        http_get(base + "/diag?verbose=0")
    except (urllib.error.URLError, OSError):
        pass
    out(f"[{ts()}] stopped. Full capture in {logname}")
    log.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
