# Architecture reference

Deep internal reference for tesla-key-esp32. This is the **on-demand** companion to
[`.claude/CLAUDE.md`](../.claude/CLAUDE.md): CLAUDE.md carries the always-needed essentials
(build/flash, component map, NVS table, command list, HTTP API, memory constraints); the
full narrative lives here so it isn't reloaded into every session. Read this when working on
telemetry, the MQTT bridge, WiFi/LAN connectivity, sleep/link-state, pairing, or OTA. Keep both in sync — the
`project-review` skill checks for drift between them.

## Read-only telemetry (detail)

A rotating background poll in `loop_task_fn_` (one domain per ~30 s cycle: climate →
drive → tires → closures, full set ~120 s) refreshes per-domain caches via the
`set_*_state_callback` hooks in `vehicle_ctrl.cpp`. All polls are `NO_WAKE_SKIP`
(read-only, never wake the car) and feed the MQTT/HA bridge — evcc/pairing are unaffected.
These background polls are **paused while a foreground evcc/manual command is in flight**
(`cmd_in_flight_`), so a command is never queued behind a slow/failing poll in the single
BLE FIFO — keeps command latency low on an awake, busy link.

Exposed under `tele` in `/status` (for the HA bridge and diagnostics; the device's web UI
renders the Overheat / Defrost chips from `tele.climate` — each shown only when a live AC draw
is available, i.e. while the car is awake and reporting; the car is never woken to populate
them — but the rest of `tele` is HA/diagnostics-only): `climate` (inside/outside/setpoint °C, on, preconditioning, plus
Cabin-Overheat-Protection `cop`/`cop_cooling`/`cop_temp`/`cop_reason` and defrost
`front_defrost`/`rear_defrost`/`defrost_mode` — separate from `is_climate_on`), `drive`
(shift, odometer_km), `tires` (fl/fr/rl/rr bar + warn), `closures` (locked,
door/frunk/trunk/window open, occupant). Numeric fields are emitted only when the car
reported them (proto3 optional) so Home Assistant shows "—"/unknown otherwise.

## OTA (self-update) — detail

Pull-based: the device fetches `manifest.json` from a fixed HTTPS URL
(`CONFIG_TESLA_OTA_MANIFEST_URL`, default GitHub Pages), compares its `version` to the
running firmware, and on confirmation downloads ITS per-target app image
(`CONFIG_TESLA_OTA_FIRMWARE_BASE_URL` + `tesla-key-esp32<suffix>.bin`, where `<suffix>` is
the chip's short tag so "esp32" appears once — `""`/`-s3`/`-c3`/`-c6` for
esp32/esp32s3/esp32c3/esp32c6, picked at compile time by `TESLA_OTA_IMG_SUFFIX` from
`CONFIG_IDF_TARGET_*`) via `esp_https_ota` into the inactive OTA slot, then reboots.
`esp_https_ota` verifies the image chip-id, so a wrong-target image is refused (never
flashed); one manifest `version` covers all targets (CI builds them from one commit).
Triggered from the web UI by tapping the firmware version in the top meta line.
Implemented in `main/ota_update.cpp`. Rollback is enabled
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`); `main.cpp` calls
`esp_ota_mark_app_valid_cancel_rollback()` after a healthy startup.

**Image signature.** Builds use the Secure Boot v2 RSA-3072 signature scheme *without*
hardware Secure Boot (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` + `..._RSA_SCHEME` +
`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`): every OTA image is signed and the running
app verifies the signature before installing, so a compromised update host can't push
unsigned firmware. No eFuses are burned (reversible, no brick risk, web installer still
works); trust is bootstrapped from the running app's signature block (TOFU). The build is
unsigned (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`) — `scripts/ci-build-all.sh` signs
each image with the offline key (CI secret `OTA_SIGNING_KEY`). Classic esp32 needs chip rev
v3.0+ (`CONFIG_ESP32_REV_MIN_3` in `sdkconfig.defaults.esp32`). **Full key lifecycle +
rotation: [`SECURITY.md`](SECURITY.md).**

Partition layout (`partitions.csv`) is dual-OTA (`otadata` + `ota_0`/`ota_1`, ~2 MB each),
sized to fill 4 MB (the smallest supported flash) so ONE table serves every target; app at
`0x20000`. **Migration:** a device on the old single-`factory` layout must be USB-reflashed
once via the web installer (full erase → WiFi/VIN/key reset, re-pair). After that, all
updates are OTA and preserve NVS. (Existing 8 MB-table S3 devices keep OTA-updating without
a reflash — OTA writes follow the INSTALLED table, and `ota_0` stays at `0x20000`.)

**One source tree, per-target images.** No per-board source forks — one codebase builds for
esp32 / esp32s3 / esp32c3 / esp32c6 (`scripts/ci-build-all.sh`; the set is bounded by
tesla-ble's `targets:`). The build deltas are config-only: target set per build
(`idf.py set-target`), flash 4 MB, and the console is native USB-Serial/JTAG
(`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`) on s3/c3/c6 — absent on the classic esp32, where it
auto-falls-back to UART0 (no per-target sdkconfig needed). The web installer is a single
page whose `manifest.json` carries one build per chipFamily (esp-web-tools auto-selects by
detected chip); OTA is a single channel where each device pulls its own
`tesla-key-esp32<suffix>.bin` (`tesla-key-esp32.bin` for the classic esp32, `-s3`/`-c3`/`-c6`
otherwise). The per-target bootloader offset (0x1000 on the classic
esp32, 0x0 elsewhere) is handled automatically by `@flash_args` and the manifest.

## Home Assistant MQTT bridge

`main/mqtt_ha.cpp` publishes **all** cached telemetry + device status to an MQTT broker
using HA's MQTT-Discovery convention, so every entity auto-appears in Home Assistant
grouped under one device. **Read-only by design** — no command topics are subscribed
(the car is never controlled or woken from HA). Independent of evcc/BLE/pairing.

- **Config:** broker URI from NVS `mqtt_uri` (web UI: Connection → MQTT, stores `host:port`)
  overriding `CONFIG_TESLA_MQTT_BROKER_URI`; empty = disabled (bridge is a no-op).
  Optional `CONFIG_TESLA_MQTT_USERNAME`/`PASSWORD`, `CONFIG_TESLA_MQTT_DISCOVERY_PREFIX`
  (default `homeassistant`), `CONFIG_TESLA_MQTT_BASE_TOPIC` (default `tesla-key`),
  `CONFIG_TESLA_MQTT_PUBLISH_INTERVAL_S` (default 15). `/set_mqtt` reboots to re-init.
- **Node id:** `teslakey_<mac3>` from the WiFi STA MAC (stable across VIN changes).
- **Topics:** `<base>/<node>/{charge,climate,drive,tires,closures,vehicle,device}` (retained
  JSON), availability/LWT `<base>/<node>/availability` (`online`/`offline`). Discovery
  configs under `<prefix>/<sensor|binary_sensor>/<node>/<object>/config` (retained).
- **Entities:** charge (soc, charge_limit, power, amps, range **km**, rate **km/h**,
  charging_state, plus extended read-only enrichment: actual_current/current_request **A**
  (delivered vs requested), charger phases, energy_added **kWh** session, minutes_to_full,
  charge limit_reason — HA bridge only, never on the `/api` evcc path), climate
  (inside/outside/setpoint °C, on, preconditioning, plus Cabin-Overheat-Protection
  cop/cop_cooling/cop_temp/cop_reason and defrost front_defrost/rear_defrost/defrost_mode),
  drive (shift,
  odometer km), tires (fl/fr/rl/rr bar + warn), closures (locked/door/frunk/trunk/window/
  occupant), sleep_state, and device diagnostics (wifi/ble RSSI, ble_link, paired, **last
  boot** (boot-time timestamp), free_heap, firmware). Numeric fields are emitted only when
  the car reported them (proto3 optional), so an unseen value reads "unknown" in HA rather
  than a phantom 0. **Units:** Tesla reports range/rate/odometer imperial; the MQTT bridge
  converts to metric (km, km/h) — only the Tesla-compatible `/api` path keeps miles (evcc).
- **Publishing:** a dedicated `mqtt_pub` task reads the thread-safe caches; on every
  (re)connect it (re)sends discovery + `online` + an immediate snapshot, then republishes
  state every interval. The same active-window gating that lets the car sleep applies to
  the *source* polls, so MQTT keeps serving the last-known (retained) values while asleep.

## WiFi / LAN connectivity (reconnect + watchdog)

The STA→LAN link (distinct from the car BLE link-state below) is kept up by two layers in
`main.cpp`:

- **Event-driven reconnect.** `wifi_event_handler` reconnects on every
  `WIFI_EVENT_STA_DISCONNECTED`. The boot-time path keeps the original budget: if the device
  has **never** held an IP (`s_wifi_ever_connected == false`) and the `MAX_RETRY` (10) fast
  attempts are spent, it sets `WIFI_FAIL_BIT` so `wifi_connect()` times out and falls back to
  the **setup portal** (the credentials are presumed wrong). But once the device has been
  online at least once, a later drop reconnects **forever** — the credentials are known-good,
  so surrendering would only strand the device. (The old code gave up after 10 retries in
  *all* cases, which is exactly how a 3.5 h router outage left the board reachable-over-BLE
  but off the LAN, recoverable only by a manual USB reset.)

- **Connectivity watchdog** (`wifi_watchdog_task`, ~30 s cadence). The event path cannot catch
  a **missed-deauth "ghost" association**: the stack still believes it is connected (holds the
  IP, keeps emitting TCP that times out — e.g. MQTT `esp-tls select() timeout`) but the AP
  forwards nothing and **no disconnect event ever fires**, so the handler never runs. The
  watchdog ICMP-echoes the **default gateway** only while the link believes it is up; after
  `kWdFailToReassoc` (2) consecutive failures (~60 s) it forces **one** `esp_wifi_disconnect()`
  — the endless-retry handler then reconnects with the known-good credentials (so the watchdog
  never calls `esp_wifi_connect()` itself, avoiding a cross-task double-connect). A stack that
  *gave up* needs no help here — that path is owned by the handler. Two guards keep it from
  ever harming a healthy link: it acts **only** when the link still believes it is up (a known-
  down link is the handler's job, and forcing a disconnect there would only churn the shared
  WiFi/BLE radio), and **only** if the gateway has answered ICMP **at least once**
  (`s_gw_ever_reachable`) — a gateway that never replies (a router/firewall dropping LAN ICMP)
  is treated as "ICMP not a usable signal here", never as "link dead", so it cannot trigger a
  perpetual ~60 s re-association loop. The probe **fails open only on its own setup failure**
  (no lease, no semaphore, session-create error → "reachable"); a gateway that is up but does
  not answer echo is treated as unreachable, which is why the baseline guard matters. It
  **never reboots** — a reboot during an AP outage would hit the 30 s boot timeout and drop
  into the setup portal, abandoning good credentials. (Implementation note: the ICMP probe's
  control block + semaphore are file-scope persistent because `esp_ping`'s worker thread is not
  joined on teardown and its completion callback always runs — a per-call stack frame would be
  a use-after-free if a probe timed out while that thread was still alive.)

## Sleep / link-state (the single source of truth)

**sleep_state** comes from `VehicleController::link_state()` — the *single* source of truth
shared with the web UI so the two never drift. Four published values:
`AWAKE` (fresh live infotainment telemetry, < 60 s), `ASLEEP` (no live data AND **proven,
debounced** sleep — the car's own VCSEC sleep flag, read from the library's
`Vehicle::sleep_state()` and sampled in `loop_task`, has held `ASLEEP` for ≥ `kAsleepDebounceS`
≈ 120 s while still reachable, so a Cabin-Overheat-Protection `AWAKE↔ASLEEP` flap (~60 s)
can't trip it), `IDLE` (reachable over BLE but **not provably asleep** — we stopped polling
the infotainment domain to let the car sleep and the VCSEC flag hasn't confirmed; we honestly
don't know, so we never claim sleep), and `UNREACHABLE` (the car answers *nothing* over BLE ⇒
driven off / out of range / deep sleep). Nothing heard since boot/re-pair ⇒ omitted so HA
shows "unknown". **Asymmetry (important):** `link_state()` trusts the VCSEC flag's *debounced
`ASLEEP`* as positive proof of sleep, but **never** trusts its `AWAKE` reading to claim
`AWAKE` (a parked car reports VCSEC `AWAKE` while its infotainment sleeps — the old
`wake_up()` trap); `AWAKE` still requires live infotainment telemetry, so a wrong VCSEC
`AWAKE` can only leave us in `IDLE`, never falsely `AWAKE`. The raw (un-debounced) flag is
also surfaced as `vcsec_sleep` in `/status` for diagnostics.

The web UI mirrors this exactly: it shows the "Vehicle asleep" hero (with the wake button)
**only** when `ASLEEP` is a proven fact; for `IDLE` it shows a neutral **"Parked"**
card (last-known SOC + idle time + the same wake button) that makes no sleep claim; and for both
`UNREACHABLE` *and* the unknown state (nothing heard since boot — the on-demand BLE link hasn't
reached the car yet) it shows a neutral grey hero with the orange Bluetooth glyph rather than a
blank area: **"Unreachable"** (no recent answer over BLE, with last-known SOC + idle time) or
**"Connecting…"** (no signed round-trip yet; the subtitle claims "Bluetooth connected" only
when the momentary GATT link is actually up, else "Reaching your Tesla over Bluetooth…") —
never a sleep claim. In that same
unknown/unreachable state the BLE connection row drops its green and animates an orange
ping-pong across the signal bars (a darker-orange crest bouncing edge→edge over a light-orange
base) with an orange MAC, flagging "connected but stateless" at a glance. The momentary BLE row
reading "Disconnected" is normal (the link is dropped between polls by design) and is not used
to drive the hero — only `link` is.

**Connection-failure detection (web-UI hero "Connection failed").** When the target car's
advert is heard but the BLE link won't come up after repeated tries, `/status.ble` carries
`connect_fail` (consecutive recent failures; only while actively failing) and `car_connectable`
(the target advert's connectable flag). `car_connectable=false` ⇒ the car advertises
**non-connectable** ⇒ it is at its ~3-device BLE-connection limit — mirroring tesla-ble's
upstream `vehicle-command`, whose BLE transport raises `ErrMaxConnectionsExceeded` off the same
`Connectable` flag (the *connect timeout itself* carries no reason). The web UI shows a
"Connection failed" hero (orange Bluetooth glyph) — "too many Bluetooth devices connected" when
`car_connectable=false`, else "move closer / disconnect other devices" — in **both** the setup
flow *and* the paired state (a paired device that can't get a slot says so instead of hiding the
hero). The signal windows are 90 s so they stay stable across a paired device's ~30-40 s
health-probe cadence. `/status.ble.devices[]` also carries per-device `connectable`, and the
no-VIN screen lists nearby Teslas (bars · dBm · MAC, sorted by signal) from the periodic
listing-only scan. Hero glyphs: grey Bluetooth = "Set up needed", grey NFC-card = "Pairing",
orange Bluetooth = "Connection failed".

Reachability is tracked by a
`last_reachable_ticks_` clock stamped on every successful signed round-trip, incl. the idle
health poll. **last boot** is published as an ISO-8601 timestamp (device_class
`timestamp`) so HA shows an auto-scaling relative "x minutes/days ago" instead of a raw
seconds counter; only emitted once the wall clock is NTP-synced.

## Pairing lifecycle / invalidation

The web UI keys everything (control buttons, SOC) off `paired` (= `has_session()`, the
stored VCSEC session in NVS). Three events invalidate a pairing and force a clean re-pair
so no stale data is shown (`clear_session_and_cache_()` in `vehicle_ctrl.cpp`):

1. **Key deleted on the car side** — auto-detected three ways: (a) the **primary** detector,
   the `set_message_callback` observer in `vehicle_ctrl.cpp`, matches a signed-message fault
   (`UNKNOWN_KEY_ID`/`INACTIVE_KEY`/`INVALID_KEY_HANDLE`) — the path that actually fires on an
   already-established (cached) session, e.g. the background charge poll; (b) any reply whose
   message contains `"whitelist"` (`KEY_NOT_ON_WHITELIST`, only during a session-info handshake)
   in `make_result_cb_`; (c) a two-strike `"authentication failed"` honoured **only** for the
   periodic signed VCSEC `health_probe_` (~30 s), so a deletion is caught even with no evcc
   traffic while a role-denied user command can never trip it. Each sets `pairing_lost_`; on
   detection the key is regenerated (the old one is useless), session + cache cleared, and
   pairing restarts.
2. **Key regenerated** (`/gen_keys?force=1`) — `generate_key()` now also clears the
   session + cache and drops the BLE link.
3. **VIN changed** (`/set_vin`) — `reset_for_new_vehicle()` regenerates the key, clears
   session + cache, and forgets the stored `ble_mac` (old car), then reboots. Re-saving
   the same VIN is a no-op for the pairing.

After any of these `has_session()` is false → UI shows "not paired", hides controls/SOC.

**A configured VIN gates pairing entirely.** The device targets the car by its VIN-derived
BLE name (`S<hex>C`), so `auto_pair_task` first checks `has_plausible_vin()` (17-char VIN;
`VehicleController::vin_is_plausible`, the same validator the web UI / `POST /set_vin` use).
With no VIN it logs once (`auto-pair: no VIN configured — pairing disabled`) and idles — it
does **not** spin connect attempts, but it *does* run a periodic listing-only discovery scan so
the web UI shows nearby Teslas (sorted by signal) live without a manual `/scan`. `set_target_vin`
is given an empty target so the scanner lists nearby Teslas but never connects or enrols on one.
This is the *design*
that stops the device whitelisting its Charging-Manager key onto an arbitrary nearby Tesla — it
no longer depends on the `"UNKNOWN"` placeholder hashing to a name that happens never to
collide (the placeholder is kept out of the matching path). The web UI already shows "Add the
vehicle VIN below to begin." when no VIN is set, so it never implies pairing without one.
