---
name: device-diag
description: Read-only triage of a live, misbehaving tesla-key-esp32 board — pull GET /status and GET /diag, read heap / link_state / WiFi / BLE / MQTT / pairing, and match the symptom to a KNOWN failure signature. Use when asked to "diagnose the device", "what's wrong with the board", "the device is broken/unreachable", "check the device health", "pull /status", "read the diag log", "why can't evcc reach it", "BLE won't connect / connect error 13", "MQTT stopped publishing", "OTA says the image is invalid / signature bad", "it's boot-looping", "it lost its pairing", or to triage a board before deciding whether to reflash. STRICTLY read-only and cache-only: never wakes the car, never sends a command; it diagnoses and hands off (it does not itself flash or command the car).
---

# device-diag — read-only triage of a live board

Nearly every debugging session on a physical `tesla-key-esp32` starts the same way: pull
`GET /status` and `GET /diag`, read the heap / `link_state` / WiFi / BLE / MQTT / pairing
vitals, and match the symptom to a **known failure signature** that's already been root-caused
once. This skill encodes that triage so a fresh session doesn't re-derive it from scratch.

**This skill is STRICTLY read-only and cache-only.** Every request below is a plain `GET` that
reads state the firmware already holds — it **never wakes the car** and **never sends a
command**. `/status`, `/diag` and `/api/proxy/1/version` are all served from RAM caches (the
`vehicle_data`/telemetry caches are refreshed out-of-band by the background poll, never on
request). It **diagnoses and hands off**: when the fix is "reflash", it points at the flash /
recovery skills — it does not flash or command the car itself.

The device host is its mDNS name **`http://tesla-key-esp32.local`** (advertised by the firmware
in [`main/main.cpp`](../../../main/main.cpp)) or the board's IP. Substitute `<host>` below.

## Scope — what this is NOT

| Need | Use instead |
|------|-------------|
| Drive the **evcc command path** end-to-end (issues real signed BLE commands, proves no-timeout) | [`e2e-evcc`](../e2e-evcc/SKILL.md) |
| **Build / USB-flash** a local tree (no merge) | [`flash-esp32`](../flash-esp32/SKILL.md) |
| **Merge → CI → flash/OTA** the signed release | [`ship`](../ship/SKILL.md) |
| **USB-reflash + erase otadata** to recover a bricked / wrong-key device | [`usb-recovery`](../usb-recovery/SKILL.md) (or the signed-artifact section of [`flash-esp32`](../flash-esp32/SKILL.md)) |

This skill is the **passive health read + symptom→cause lookup** in front of all of those.

## Step 1 — pull `/status` (the one JSON that has everything)

`handle_status` in [`main/http_status.cpp`](../../../main/http_status.cpp) returns the same JSON
the web UI receives over the `/events` WebSocket (~2 s push; `build_status_object()` is the shared
builder). Top-level keys: `vin`, `ip`, `version`, `paired` (=
`has_session()`), `reauth`, `key_present`, `wifi{ssid,rssi,std}`, `ble{connected,scanning,rssi,
addr,devices[],connect_fail,car_connectable}`, `mqtt{configured,connected,tls,broker,error}`,
`syslog{configured,resolved,reachable,host,port,error}`,
`tele{climate,drive,tires,closures}` (**only while the BLE link is up**), `link`, `vcsec_sleep`
(the raw, **un-debounced** VCSEC sleep flag, for diagnostics only — not what drives the hero),
plus `vehicle{…}` / `last{…}` / `last_seen_s` charge snapshots, and `last_reboot` (present ONLY when the heap watchdog restarted us — see the signature table).

```bash
curl -s http://<host>/status | jq
```

One-line vitals (note: **there is no heap field in `/status`** — heap comes from the serial log,
see Step 3):

```bash
curl -s http://<host>/status | jq '{
  version, paired, reauth, link, vcsec_sleep,
  wifi: {ssid: .wifi.ssid, rssi: .wifi.rssi},
  ble:  {connected: .ble.connected, rssi: .ble.rssi,
         connect_fail: .ble.connect_fail, car_connectable: .ble.car_connectable},
  mqtt: {configured: .mqtt.configured, connected: .mqtt.connected,
         tls: .mqtt.tls, error: .mqtt.error},
  last_seen_s
}'
```

Confirm firmware version + running chip (a half-applied OTA shows a version mismatch here):

```bash
curl -s http://<host>/api/proxy/1/version   # {"version":"<X>-esp32","platform":"ESP32-S3"}
```

`/api/proxy/1/version` (see [`main/http_api.cpp`](../../../main/http_api.cpp)) reports
`version` with a **fixed `-esp32` suffix on all five targets**; the actual chip is the separate
`platform` field (`ESP32` / `ESP32-S3` / `ESP32-C3` / `ESP32-C6` / `ESP32-C5`). Its `version`
(minus the suffix) must equal `/status.version`.

### Reading `link` (the four-state machine)

`link` is `VehicleController::link_state()` stringified by `tk::link_state_web_str` in
[`main/logic/link_state.hpp`](../../../main/logic/link_state.hpp) — the **single source of
truth** shared with the MQTT bridge, so the hero and `sleep_state` can't drift:

| `link` | Means | Triage note |
|--------|-------|-------------|
| `awake` | Fresh live infotainment telemetry (< 60 s) — car is up | `vehicle{}` present; `tele` present |
| `asleep` | No live data **and** VCSEC sleep held ≥ ~120 s (debounced) | Proven sleep; do not "fix" |
| `idle` | Reachable over BLE but **not provably asleep** | Normal parked state ("Parked" in UI) |
| `unreachable` | Answers nothing over BLE | See the split below |

(These are the exact lowercase strings `link_state_web_str` emits — the uppercase `AWAKE`/`IDLE`/…
you may recall are the *MQTT* `sleep_state` values from `link_state_mqtt_str`, not `/status.link`.)
Cold-start (nothing heard since boot) is a **distinct string `unknown`** — not `unreachable` — which
the UI renders as **"Connecting…"** (still warming up — often not a fault); `unreachable` proper
means the car was heard once and then went silent (**"Unreachable"** — drove off / out of range /
another device holds the link). The UI keys that split on the `link` string itself (`s.link==='unreachable'`),
which tracks `last_seen_s` (cold-start has none). **Asymmetry to remember:** a debounced VCSEC
`ASLEEP` is trusted; a VCSEC `AWAKE` is **never** trusted to move `link` to `awake` (that needs live
telemetry), so a wrong `vcsec_sleep:"AWAKE"` can only ever leave `link` at `idle`.

## Step 2 — pull `/diag` (the in-RAM console ring)

`handle_diag` ([`main/http_status.cpp`](../../../main/http_status.cpp)) serves the last ~16 KB of
console output from a static `.bss` ring in
[`main/diag_log.cpp`](../../../main/diag_log.cpp). It is **chunk-streamed** (`diag_log_dump_chunks`
→ `httpd_resp_send_chunk`), never assembled into one big `std::string`, precisely because a large
contiguous allocation on a fragmented heap can throw `std::bad_alloc` → `abort()` → reboot.

```bash
curl -s 'http://<host>/diag'              # dump the ring (default)
curl -s 'http://<host>/diag?verbose=1'    # turn RAW BLE RX logging ON, then reproduce the issue
curl -s 'http://<host>/diag'              # …dump again to capture the raw frames
curl -s 'http://<host>/diag?verbose=0'    # turn verbose back OFF (it's noisy)
curl -s 'http://<host>/diag?clear=1'      # reset the ring (e.g. before a clean repro)
```

Query params (parsed in `handle_diag`): `?verbose=1`/`?verbose=0` toggle raw-RX capture,
`?clear=1` clears the ring. The response header `X-Diag-Verbose` echoes the current mode.

## Step 3 — heap (the binding constraint) comes from the serial log

The limit on this device is the **largest *contiguous* free block, not total free heap** — a few
tens of KB steady-state is normal; a *falling* largest block is heap pressure and a reboot risk.
`/status` does **not** carry heap; read it from the serial console (or **syslog**, which is the
only copy that survives a reboot — the `/diag` ring does not). Three log lines:

- Boot ([`main/main.cpp`](../../../main/main.cpp)): `BOOT reset_reason=<r> free_heap=<n>
  min_free=<n> largest_block=<n>` — `reset_reason` tells you *why* it last restarted (a
  panic/`abort` loop vs a clean power-on). It **cannot see a deliberate restart**, though: an
  `esp_restart()` and a user power-cycle both read SW/POWERON — check `/status.last_reboot`.
- Per-milestone ([`main/main.cpp`](../../../main/main.cpp)): `HEAP @<where> free=<n> largest=<n>
  min=<n>` — where WiFi/HTTP/MQTT each spent their contiguous block.
- Periodic ~30 s ([`main/vehicle_telemetry.cpp`](../../../main/vehicle_telemetry.cpp)
  `loop_task_fn_`): `HEAP free=<n> largest_block=<n> min_free=<n> internal_largest=<n>` — the
  **trend** line, and the one the heap watchdog decides on. On the **esp32c5 read
  `internal_largest`, not `largest_block`**: the latter is the plain `MALLOC_CAP_8BIT` figure,
  which includes the C5's 8 MB PSRAM and can read ~7.8 MB while internal DRAM is at 768 B. The
  watchdog restarts when `internal_largest` stays under 4 KB for 5 unbroken minutes.

The watchdog also **narrates its own escalation**, so a restart can be reconstructed from syslog
alone. Grep these in order — the countdown lines are the proof the shortage was *sustained* rather
than a spike, and their absence before a reboot means something *else* restarted the device:

- `HEAP CRITICAL: … watchdog ARMED, restarting in 300 s unless it recovers` — the run opened.
- `HEAP CRITICAL for <n> s … restarting in <m> s unless it recovers` — one per 30 s sample.
- `HEAP recovered after <n> s critical … watchdog disarmed` — it healed; no restart happened.
- `HEAP critical run (<n> s) cleared: an OTA is in flight …` — excused, *not* healed.
- `HEAP EXHAUSTED for <n> s … RESTARTING DELIBERATELY, watchdog restart <k>/5, breadcrumb
  reboot_why=heap:<k>` — the restart itself, with the state that caused it.
- `HEAP EXHAUSTED … but <n> consecutive watchdog restarts have not fixed it — NOT restarting
  again` — the cap held; the device is up but degraded, and this is logged once per run.
- `BOOT this boot was caused by the firmware itself: reason=heap:<k>` — on the *next* boot,
  matching `/status.last_reboot`.

The times are the **measured** age of the run, not the configured hold, so a fired run reads
somewhat over 300 s (30 s sampling cadence) — that is expected, not drift.

```bash
# Host serial monitor (no local idf.py) — exit Ctrl-A then K.
screen /dev/cu.usbmodemXXXX 115200
```

Use serial as the fallback whenever the board is off the LAN (so `/status`/`/diag` are
unreachable) or you need to see the `BOOT`/`HEAP` lines, a `connect error: <n>`, or an OTA
`image valid, signature bad` that never reaches HTTP.

## Symptom → cause → action

Each row is a signature that's been root-caused before (cite the code site when you report it):

| Symptom (from `/status` + `/diag`/serial) | Cause | Action (this skill hands off) |
|---|---|---|
| **Every BLE connect times out.** `/diag`/serial shows `connect error: 13` (`BLE_HS_ETIMEOUT`); `/status` has `ble.connected:false`, `ble.connect_fail` climbing, often `ble.car_connectable:false`. Scan still *finds* the car. | A **second board** holding the car's single BLE link (Tesla allows ~3 simultaneous, and a proxy holds it continuously), **or** signal-limited RX (weak advert RSSI). **Not a firmware bug** — the connect path is unchanged and the scan matches the VIN. `connect error: %d` is logged in [`main/ble_client.cpp`](../../../main/ble_client.cpp) (`BLE_GAP_EVENT_CONNECT`, status ≠ 0). | Run **one board per car** — power off the other holder, then retry. If truly alone, treat as an antenna/range problem (physical), not a flash. |
| **`/status` reports `last_reboot:"heap:<n>"`** — the device is healthy NOW, with a low uptime nobody triggered. `n` = consecutive watchdog restarts; ≥2 means restarting is not fixing it, and at 5 it stops restarting. | The **heap-exhaustion watchdog** restarted us: `largest_block` stayed under 4 KB for 5 unbroken minutes, i.e. the heap was gone and not coming back ([`main/logic/heap_watchdog.hpp`](../../../main/logic/heap_watchdog.hpp), sampled in `vehicle_telemetry.cpp` `loop_task_fn_`). The restart is the *symptom being handled*, not the fault — something leaked. The field is set once per boot and absent on every ordinary one. | Find the leak, don't just restart. Pull the `HEAP free=… largest_block=…` trend from **syslog** (the in-RAM `/diag` ring does not survive the reboot) and look for what ran before the decline — the known one was an unbounded `/events` WS backlog. If it recurs, that is a real leak to fix, not a watchdog to loosen. |
| **Device unreachable for hours, then fine after a power-cycle**, with no `last_reboot` and no crash in the logs. | A **wedge**: on a firmware without the heap watchdog (pre-1.4.54) a permanently exhausted heap made every OOM guard "recover and continue" forever — `bad_alloc` out of `loop()` ~20×/s, nothing serving, no reboot. | Confirm from syslog (`largest_block` collapsed and never recovered + a `bad_alloc` storm). The escalation now restarts after 5 min; if the device is on an older image, update it. |
| **MQTT stopped publishing** / serial shows esp-mqtt `select() timeout`, **while uptime keeps climbing and BLE polls are fine.** `/status`: `mqtt.connected:false` (and often an `mqtt.error`). | The board **fell off WiFi** (ghost association / missed deauth) — it's not a crash. `main.cpp` runs an endless runtime reconnect + gateway-ping watchdog, but it can sit disconnected. | Fastest re-associate is a **USB reset / power-cycle**. Do **not** reflash for this. If `mqtt.error`/`mqtt.tls` shows a **TLS handshake** failure it stays disconnected on purpose — **no silent plaintext fallback** ([`main/mqtt_ha.cpp`](../../../main/mqtt_ha.cpp)); fix the broker URI/CA, see [`docs/SECURITY.md`](../../../docs/SECURITY.md). |
| **OTA reaches 100% then fails** `"downloaded image is invalid"`; serial: `image valid, signature bad` / `ESP_ERR_OTA_VALIDATE_FAILED`. | The running image's **TOFU trust anchor was signed with a different key** than the published images (e.g. a local/dev-key build), so it rejects every CI-signed OTA. Signature proves authenticity, not freshness — this is the anchor mismatch, not a corrupt download. Mapped in [`main/ota_update.cpp`](../../../main/ota_update.cpp) (`ESP_ERR_OTA_VALIDATE_FAILED`). | **USB-reflash the published CI `.bin` + erase otadata** (keeps NVS/pairing) → [`usb-recovery`](../usb-recovery/SKILL.md) / the signed-artifact section of [`flash-esp32`](../flash-esp32/SKILL.md). |
| **Boot-loops immediately** (never reaches `app_main`); `BOOT reset_reason` shows repeated resets after flashing a **locally-built** image. | An **unsigned** app `abort()`s at startup under the signed-OTA config (`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`, `check_signature_on_update_check`, all targets). It does **not** boot-and-TOFU. | Flash a **signed** image — the CI artifact, or sign the local build (see the ⚠️ warning in [`flash-esp32`](../flash-esp32/SKILL.md)) / [`usb-recovery`](../usb-recovery/SKILL.md). |
| **`paired` flips to `false` unexpectedly** (`/status`: `paired:false`, usually `reauth:true`). | **Pairing invalidation** — the session was cleared and a re-pair forced. Three causes ([`.claude/CLAUDE.md`](../../../.claude/CLAUDE.md) "Pairing lifecycle"; [`main/vehicle_pairing.cpp`](../../../main/vehicle_pairing.cpp) / [`main/vehicle_ctrl.cpp`](../../../main/vehicle_ctrl.cpp)): (1) key deleted on the car (signed-msg fault / `"whitelist"` reply / two-strike health-probe `"authentication failed"`), (2) `/gen_keys?force=1`, (3) `/set_vin`. Note the evcc path is safe — only the health probe / a `"whitelist"` fault trips revocation, not user role-denied commands. | Re-pair: place a Tesla **NFC keycard on the console reader** and `POST /send_key` (Charging-Manager role). Full lifecycle in [`docs/ARCHITECTURE.md`](../../../docs/ARCHITECTURE.md). Nothing to flash. |

## After triage

If the read established the device is **healthy** (paired, BLE up, heap steady), prove the evcc
runtime path with no timeouts via [`e2e-evcc`](../e2e-evcc/SKILL.md). If the action column says
**reflash**, hand off to [`usb-recovery`](../usb-recovery/SKILL.md) or
[`flash-esp32`](../flash-esp32/SKILL.md) — **this skill never flashes and never commands the
car.**
