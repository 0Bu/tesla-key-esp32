# Architecture reference

Deep internal reference for tesla-key-esp32. This is the **on-demand** companion to
[`.claude/CLAUDE.md`](../.claude/CLAUDE.md): CLAUDE.md carries the always-needed essentials
(build/flash, component map, NVS table, command list, HTTP API, memory constraints); the
full narrative lives here so it isn't reloaded into every session. Read this when working on
telemetry, the MQTT bridge, WiFi/LAN connectivity, sleep/link-state, pairing, OTA, or anything
that touches locks/tasks (the Concurrency contract at the end). Keep both in sync — the
`project-review` skill checks for drift between them.

## Read-only telemetry (detail)

A rotating background poll in `loop_task_fn_` (one domain per ~30 s cycle: climate →
drive → tires → closures, full set ~120 s) refreshes per-domain caches via the
`set_*_state_callback` hooks in `vehicle_telemetry.cpp`. All polls are `NO_WAKE_SKIP`
(read-only, never wake the car) and feed the MQTT/HA bridge — evcc/pairing are unaffected.
These background polls are **paused while a foreground evcc/manual command is in flight**
(`cmd_in_flight_`), so a command is never queued behind a slow/failing poll in the single
BLE FIFO — keeps command latency low on an awake, busy link.

Exposed under `tele` in `/status`, emitted only while the BLE link is up — the MQTT bridge
reads the caches directly, so it keeps publishing regardless (the device's web UI
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
the chip's short tag so "esp32" appears once — `""`/`-s3`/`-c3`/`-c6`/`-c5` for
esp32/esp32s3/esp32c3/esp32c6/esp32c5, picked at compile time by `TESLA_OTA_IMG_SUFFIX` from
`CONFIG_IDF_TARGET_*`) via `esp_https_ota` into the inactive OTA slot, then reboots.
`esp_https_ota` verifies the image chip-id, so a wrong-target image is refused (never
flashed); one manifest `version` covers all targets (CI builds them from one commit).
Triggered from the web UI by tapping the firmware version in the top meta line.
Implemented in `main/ota_update.cpp`.

**Downgrade gate (software anti-rollback).** Just after `esp_https_ota_begin` — before the
bulk download — `ota_task` reads the version from the downloaded image's own app descriptor
(`esp_https_ota_get_img_desc`) and refuses anything not strictly newer than the running
firmware. A valid RSA signature proves *authenticity* but not *freshness*, so without this a
hostile update host could serve an old, legitimately-signed image carrying a since-patched
bug. Reading the **image's** version (not the manifest's) also defeats a host that advertises
a new `version` in `manifest.json` but serves an old `.bin`. No eFuses burned.

**Rollback** is enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`); `main.cpp` defers
`esp_ota_mark_app_valid_cancel_rollback()` to a health gate (`ota_health_gate_task`) that
holds rollback armed until a freshly-flashed image has run healthily for a window
(`kOtaHealthGateS` ≈ 90 s). An image that boots but then crashes/OOM-reboots under load dies
while still `PENDING_VERIFY`, so the bootloader reverts to the previous slot rather than
having committed it at startup.

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
sized to fill 4 MB (the smallest supported flash — the T-Dongle-C5's 16 MB leaves the top
unused) so ONE table serves every target; app at `0x20000`. The `ci-build-all.sh` **app-size gate**
sits at `slot − 32 KB` (0x1e8000): each image's code rounds up to a 64 KB Secure-Boot boundary + a
4 KB signature, and the largest — esp32c6 and esp32c5, ~0x1d1000 signed — clear it comfortably.
**esp32c5 carries the extra on-device display + PSRAM code, and esp32s3 the display code too
(no PSRAM), but both still fit at the base `-Og`** like every target: the Package A size levers
(#154) freed the ~64 KB the display needs, so no `-Os` (which hard-freezes under load — rejected
Package B) is required. **Migration:** a device on the old single-`factory` layout must be USB-reflashed
once via the web installer (full erase → WiFi/VIN/key reset, re-pair). After that, all
updates are OTA and preserve NVS. (Existing 8 MB-table S3 devices keep OTA-updating without
a reflash — OTA writes follow the INSTALLED table, and `ota_0` stays at `0x20000`.)

**One source tree, per-target images.** No per-board source forks — one codebase builds for
esp32 / esp32s3 / esp32c3 / esp32c6 / esp32c5 (`scripts/ci-build-all.sh`). The build deltas are
config-only: target set per build (`idf.py set-target`), flash 4 MB, and the console is native
USB-Serial/JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`) on s3/c3/c6/c5 — absent on the classic
esp32, where it auto-falls-back to UART0. The only per-target sdkconfig files are
`sdkconfig.defaults.esp32` (Secure-Boot chip-rev floor), `sdkconfig.defaults.esp32c5`
(on-device ST7735 display + 8 MB PSRAM; still `-Og`) and `sdkconfig.defaults.esp32s3`
(on-device ST7735 display, no PSRAM — the display auto-enables only on a detected T-Dongle-S3).
The web installer is a
single page whose `manifest.json` carries one build per chipFamily (esp-web-tools auto-selects by
detected chip); OTA is a single channel where each device pulls its own
`tesla-key-esp32<suffix>.bin` (`tesla-key-esp32.bin` for the classic esp32, `-s3`/`-c3`/`-c6`/`-c5`
otherwise). The per-target bootloader offset (0x1000 on the classic
esp32, 0x2000 on esp32c5, 0x0 on s3/c3/c6) is handled automatically by `@flash_args` and the manifest.

**esp32c5 via a local build-time patch of tesla-ble.** esp32 / esp32s3 / esp32c3 / esp32c6 are
exactly the targets yoziru/tesla-ble declares in its `idf_component.yml` `targets:`, and the
ESP-IDF Component Manager enforces that list — it refuses `esp32c5` at dependency resolution,
before compile, and stages nothing into `managed_components/` (so there is nothing to patch after
the fact). The library's code is target-agnostic and already builds for RISC-V (c3/c6); C5 is
RISC-V too, so the only blocker is that one manifest line. Rather than fork upstream or edit the
regenerated `managed_components/`, `scripts/prepare-tesla-ble-c5.sh` clones the exact pinned tag
into `third_party/tesla-ble` (gitignored) and appends `esp32c5` to its `targets:`.
`main/idf_component.yml` then declares the dependency twice under mutually-exclusive `rules:` —
the git dep applies when `target != esp32c5`, a local `path:` dep (the patched checkout) when
`target == esp32c5`. So only C5 routes through the local copy; the other four resolve
byte-identically from git, and CI (`ci-build-all.sh`) runs the prepare step automatically. All
five images are the same tesla-ble revision.

**On-device ST7735 display (LilyGO T-Dongle-C5 and T-Dongle-S3).** Both dongles carry the same
0.96" ST7735 LCD and it IS driven — see `main/display.cpp` (a status panel: WiFi/BLE header + a
SoC battery, or a WiFi/BLE-search / "Pairing…" animation; cache-only, never wakes the car). The
panel is drivable **LANDSCAPE (160x80, horizontal battery)** or **PORTRAIT (80x160, two-row header
over a vertical battery filling bottom→top)** — each BOOT-button tap rotates 90° through the four
orientations (landscape/portrait ± their 180° flips, MADCTL `{0xC8,0xA8,0x08,0x68}` over the SAME
framebuffer with the col/row offsets swapping 1/26↔26/1); the index persists in NVS
`tesla_cfg/disp_rot`. **What to show** — the priority ladder (WiFi-search > pairing > BLE-search >
battery), the SoC gradient, the RSSI→bars mapping and the SSID-scroll offset — is decided by a
pure, host-tested presenter (`main/logic/display_model.hpp`, whose `Orient` axis picks the
per-layout SSID geometry) reading a shared, IDF-free `UiSnapshot` (`main/logic/ui_state.hpp`,
assembled once under the cache lock via `VehicleController::ui_snapshot()`); `display.cpp` is a
thin renderer (`draw_landscape` / `draw_portrait`) that only DRAWS the resulting `Model` — so those
decisions are unit-tested in `test/` without a board (`logic-test` job), the layout constants have
one home, and the status LED consumes the same snapshot.
The rendering is identical on both boards (the layout mirrors
`tools/display_sim.py`, the pixel-exact offline renderer and font source of truth — and that
mirror is no longer by hand: `scripts/check-display-sim-parity.sh`, run by
`scripts/run-mock-tests.sh`, diffs the sim's `decide()` against golden vectors the C++ presenter
emits, so a drift between firmware and sim fails the `logic-test` gate). Only the hardware wiring
differs, from Kconfig/`sdkconfig.defaults.*`:

- **T-Dongle-C5** (ESP32-C5HR8, 16 MB flash, 8 MB PSRAM): framebuffer in PSRAM; SPI 20 MHz; BOOT
  button on GPIO28. Compiled for esp32c5 via `CONFIG_TESLA_DISPLAY_ENABLED` in
  `sdkconfig.defaults.esp32c5`. The C5 has exactly one board, so the display is always on.
- **T-Dongle-S3** (ESP32-S3): framebuffer in ~25 KB internal SRAM (no PSRAM enabled); SPI 40 MHz;
  BOOT button on GPIO0. Compiled for esp32s3 via `sdkconfig.defaults.esp32s3`. Because the single
  esp32s3 image also runs on a **generic ESP32-S3** (no panel), `display_start()` first
  **auto-detects the T-Dongle-S3** by its TF-card socket's external SD pull-ups (a generic S3
  leaves those GPIOs floating) — a majority ≥4/6 HIGH means the dongle; otherwise the display is a
  complete no-op (no framebuffer, no GPIO driven), so a generic S3 boots exactly as before.

On both boards the backlight is active-low, and each tap on the BOOT button rotates the panel 90°,
cycling landscape → portrait → landscape-180° → portrait-180° → … (rotation index in NVS
`tesla_cfg/disp_rot`, migrated from the pre-rotation `disp_flip` bool). The two landscape MADCTLs
(0xA8/0x68) and the (1,26) offsets are HW-verified; the portrait MADCTLs (0xC8/0x08) and (26,1)
offsets follow the standard ST7735 rotation set and want a quick on-device confirm (the 90°
direction is a one-line `+1`→`+3` flip in `rotate_90()` if it turns the wrong way). Compiled to a
no-op stub on the other targets (`#else` in `display.cpp`), so one source tree still serves every board.

**On-device status LED (T-Dongle underside APA102).** A second, independent indicator: the single
addressable RGB pixel on the dongle underside (`main/led_status.cpp`), driven as a colour +
animation status light — WiFi/BLE search (breathing), pairing (pulse), charging (green swell),
a dimmed SoC colour when parked, blue for an OTA in flight, amber/red for warnings/errors. Its
priority ladder lives in the pure, host-tested `logic/led_status.hpp` and reads the **same shared
`UiSnapshot`** the ST7735 presenter consumes (so the panel, the LED, the web-UI hero and MQTT never
disagree about the car's state), plus a tiny LED-only `LedAlerts` for the latched error/warn/OTA
tiers (those hold a transient fault visible for 10–15 s). The SoC colour comes from the shared
`logic/soc_gradient.hpp` ramp — the same table the battery fill uses. Cache-only (never wakes the
car), needs no MQTT and no panel (works on a display-less board), an ~12-byte bit-banged APA102
frame with no heap. Opt-in: a no-op stub unless `CONFIG_TESLA_LED_ENABLED` (default off — not every
board carries this LED); pins/brightness from Kconfig (C5 DI=5/CI=4, T-Dongle-S3 40/39).

**ESP32-C5, 5 GHz WiFi and BLE coexistence.** The C5 is Espressif's dual-band Wi-Fi 6 (2.4 + 5
GHz) RISC-V part, so a natural question is whether running WiFi on 5 GHz reduces the WiFi↔BLE
interference that historically forced `WIFI_PS_MIN_MODEM` on this project (a `WIFI_PS_NONE`
station starves BLE until every GATT connect times out). The honest answer from the silicon:
**per Espressif's C5 RF-coexistence guide the chip has "only one 2.4 GHz ISM band RF module,
shared by" WiFi/BLE/802.15.4**, and the T-Dongle-C5 has a single antenna — so WiFi and BLE still
**time-division multiplex one RF path even with WiFi on 5 GHz**. Moving WiFi to 5 GHz does NOT
remove that airtime contention (the dominant cause of BLE stalls here), and `WIFI_PS_MIN_MODEM`
must stay on regardless. What 5 GHz *can* do is take this device's WiFi off the 2.4 GHz band —
freeing that band for BLE and dodging a congested 2.4 GHz environment (neighbour APs, microwave)
— so BLE reliability *may* improve second-order, depending on how busy the local 2.4 GHz band is.
That is an empirical, per-network question, so the effect is exposed behind an opt-in,
default-off, C5-only Kconfig `CONFIG_TESLA_WIFI_PREFER_5G` (`depends on SOC_WIFI_SUPPORT_5G`):
when set, WiFi init adds an RSSI bonus (`wifi_sta_config.threshold.rssi_5g_adjustment`) so the
existing `BY_SIGNAL` scan prefers the 5 GHz AP of a band-steered SSID. Band mode stays automatic,
so a device out of 5 GHz range still falls back to 2.4 GHz (no reconnect trap). Flip it on and
compare BLE connect success / `link_state` over a few days to A/B test it on your network.

**PR preview installer.** Every same-repo PR publishes its **signed** build so a change can be
browser-flashed and tried *before* merge. CI writes the PR's **full self-contained site**
(`build-pages.sh` → the installer page + a per-PR `manifest.json` + same-origin bins) to
`PR/<N>/` on the **`gh-pages` branch**, so `https://0bu.github.io/tesla-key-esp32/PR/<N>/` is a
directly browsable installer for that PR — it detects it is under `/PR/<N>/`, shows a preview
banner, and flashes that PR's own firmware. The **root** page has **no version picker**: it
always flashes **main**. A PR's firmware is reached only by its own `PR/<N>/` page — open the URL
directly, or follow the link posted on the PR. A `gh-pages` branch (not the Actions
Pages artifact) is required because the browser flasher fetches every part in-page and GitHub
release assets carry no CORS headers, so the bins must be same-origin — and the atomic Actions
deploy (main-only, whole-site) can't host per-PR subpaths. Main owns the gh-pages **root**;
each PR owns `PR/<N>/`; both are synced by `scripts/publish-pages-branch.sh` (root sync
preserves the `PR/` tree). Constraints:

- **Signed-only.** Fork PRs get no `OTA_SIGNING_KEY` → build unsigned → **no** preview
  published (an unsigned image crash-loops at boot — see [`SECURITY.md`](SECURITY.md)).
- **Versioning `<latest-tag>-PR-<N>`** (e.g. `1.4.30-PR-157`), stamped from the newest
  released tag. `ver_newer()` parses only `x.y.z` and ignores the suffix, so basing on the
  *latest release* (not `next`) guarantees a later main release compares strictly-newer → the
  PR-flashed device OTA-updates forward to main; a `next` base would collide with the number
  the merge cuts and stall OTA.
- **OTA stays on main.** `CONFIG_TESLA_OTA_MANIFEST_URL` is compile-time and unchanged in PR
  builds, so a PR-flashed device checks OTA against the **main** manifest, never its own
  preview. The real-key signature anchors trust so the main release is accepted.
- **Cleanup.** `.github/workflows/pr-preview-cleanup.yml` removes `PR/<N>/` when the PR closes
  (merged or not).

*(Rollout note: this hosting only goes live once GitHub Pages' source is switched from "GitHub
Actions" to "Deploy from branch: `gh-pages`"; until then the gh-pages branch is populated but
inert and the Actions artifact remains the live site — a zero-downtime migration.)*

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
- **Transport / TLS:** a schemeless entry defaults to plaintext `mqtt://` **unless credentials
  are present** (a configured username, or `user:pass@host` userinfo), in which case it defaults
  to **`mqtts://`** so the password (sent in the clear in the MQTT CONNECT on plain mqtt) is not
  exposed to a sniffer on a broker that lives off the trusted LAN. For `mqtts://` the broker
  certificate is verified against the bundled CA roots (same trust store as OTA); an untrusted
  cert fails the handshake and the bridge stays disconnected — there is **no silent fallback to
  plaintext**. The failure reason surfaces in `/status` (`mqtt.error`) and the web UI. An explicit
  scheme (`mqtt://`/`mqtts://`) is always honored. `/status` also exposes `mqtt.tls`.
- **Node id:** `teslakey_<mac3>` from the WiFi STA MAC (stable across VIN changes).
- **Topics:** `<base>/<node>/{charge,climate,drive,tires,closures,vehicle,device}` (retained
  JSON), availability/LWT `<base>/<node>/availability` (`online`/`offline`). Discovery
  configs under `<prefix>/<sensor|binary_sensor>/<node>/<object>/config` (retained).
- **Entities:** charge (soc, charge_limit, power, amps, range **km**, rate **km/h**,
  charging_state, plus extended read-only enrichment: actual_current/current_request **A**
  (delivered vs requested), volts **V** at the charger, charger phases, energy_added **kWh**
  session, minutes_to_full,
  charge limit_reason — HA bridge only, never on the `/api` evcc path), climate
  (inside/outside/setpoint °C, on, preconditioning, plus Cabin-Overheat-Protection
  cop/cop_cooling/cop_temp/cop_reason and defrost front_defrost/rear_defrost/defrost_mode),
  drive (shift,
  odometer km), tires (fl/fr/rl/rr bar + warn), closures (locked/door/frunk/trunk/window/
  occupant), sleep_state, and device diagnostics (wifi/ble RSSI, ble_link, paired, **last
  boot** (boot-time timestamp), free_heap, firmware). Numeric fields — and the single
  booleans on/preconditioning/cop_cooling/defrost/locked/occupant — are emitted only when
  the car reported them (proto3 optional), so an unseen value reads "unknown" in HA rather
  than a phantom 0. The aggregates warn and door/frunk/trunk/window fold several per-wheel/
  per-opening booleans with present-AND-true semantics (an unreported part counts as
  no-warning/closed by design). **Units:** Tesla reports range/rate/odometer imperial; the MQTT bridge
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
  (watchdog not yet initialised, unparseable gateway, or `esp_ping` session-create error →
  "reachable"); a *missing* DHCP lease/gateway, or a gateway that is up but does not answer
  echo, is treated as unreachable — which is why the baseline guard matters. It
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
shows "unknown" (strictly: the state topics are retained, so until the first post-reboot
publish replaces them HA may still show the pre-reboot value; a fresh install shows
"unknown" immediately). **Asymmetry (important):** `link_state()` trusts the VCSEC flag's *debounced
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
so no stale data is shown (`clear_session_and_cache_()` in `vehicle_pairing.cpp`):

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

## MCP endpoint (/mcp)

`main/mcp_server.cpp` exposes the device to MCP (Model Context Protocol) clients — Claude
Desktop/Code, VS Code, or any agent framework — over the existing `esp_http_server` on
port 80, so an LLM agent can read state and drive charging without an extra proxy process.
This section covers the firmware-internal design; the **user-facing integration guide**
(wire examples, client configs, troubleshooting) is [`MCP.md`](MCP.md).

**Transport — Streamable HTTP, stateless profile.** `POST /mcp` carries exactly one
JSON-RPC 2.0 message and is answered with `application/json`:

- No SSE stream and no server-initiated requests — `GET /mcp` returns `405` with
  `Allow: POST`. A long-lived stream would pin one of the few httpd sockets and the
  device has no server-push use case.
- No `Mcp-Session-Id` — every request is self-contained; the `MCP-Protocol-Version`
  header is ignored (nothing version-dependent happens after `initialize`).
- Notifications (`notifications/*`) and stray client responses (a message without an
  `id`) are acknowledged with `202 Accepted` and no body, per the transport spec. A
  method-less, id-less message (`{}`) is NOT a notification — it gets `-32600` so a
  broken client isn't left waiting for a reply that never comes.
- JSON-RPC **batches are rejected** (`-32600`) — protocol `2025-06-18` removed them, and
  the single-message parse keeps the heap cost bounded (2 KB body cap, same as the REST
  endpoints).

**Version negotiation** (`tk::mcp_negotiate_version`, `main/logic/mcp.hpp`): supported
revisions are `2025-06-18` and `2025-03-26`; a request for anything else is answered with
our latest supported revision, per the MCP lifecycle spec (the client disconnects if it
can't proceed). **Methods:** `initialize` (capabilities: `tools` only), `ping`,
`tools/list`, `tools/call`; everything else → `-32601`.

**One spec table drives everything.** The tool registry in `logic/mcp.hpp` (`kMcpTools`)
carries name, description AND each argument's key/type/required/bounds (`McpArg`,
`kMcpMaxArgs`). The advertised `tools/list` JSON schema and the executor's validation are
both generated from that table, so schema-vs-enforcement drift is impossible by
construction: an absent required argument OR a present-but-unparseable one is a `-32602`
protocol error (silently defaulting `set_scheduled_charging`'s `enable` would *disable*
the schedule and report success); loose-but-unambiguous encodings are coerced (numeric
strings for ints, 0/1 for bools); parsed integers are clamped to the spec bounds before
the int cast (UB guard). The registry, method routing, version table, clamp and the
shared command-outcome text (`logic/command_result.hpp`, also used by the REST
`/command` reason so the two paths can never diverge) are IDF-free and covered by the
host mock build (`test/test_logic.cpp`, `test_mcp`). The tool set itself — exactly the
run-on-key charging commands plus cache-only `get_vehicle_state`, role-refused commands
deliberately absent — is documented with the full per-tool table in
[`MCP.md`](MCP.md#tools).

**Heap safety:** `tools/list` is the endpoint's largest response (~1.5 KB serialized) and
`cJSON_PrintUnformatted` builds it in one contiguous block — the crash-risk currency on
this heap — so tool descriptions stay terse and the tool set small; the static registry
strings are attached via `cJSON_CreateStringReference` (no per-request strdup of
`.rodata`). The send path carries the same NULL-print → 503 guard as `send_json` (plus an
envelope-OOM guard that frees the orphaned payload), and both handlers are dispatched
inside `http_server.cpp`'s `handle_all` try/catch.

**Security posture:** identical to the rest of the HTTP API — no auth, no TLS, trusted
LAN only (see [`SECURITY.md`](SECURITY.md)). The endpoint grants nothing the open REST
API doesn't already expose; the enrolled key stays Charging Manager only. Client
configuration lives in [`MCP.md`](MCP.md#client-integration).

## Concurrency (normative contract)

This section is the **rule**, not a description: new code either fits it or changes it here
first, in the same PR. Deadlock is this device's worst failure mode — frozen but not
rebooting (no panic, so no reboot), evcc blind, and the polling window stuck open so a
parked car never sleeps.

### Lock hierarchy (`VehicleController`)

Four primitives, created in `VehicleController::init` (`vehicle_ctrl.cpp`); the RAII guards
live in `vehicle_ctrl_internal.hpp` (`tk::MutexGuard`, `tk::InFlightGuard`):

| Primitive | Kind | Protects |
|---|---|---|
| `command_mutex_` | mutex, RAII | one whole command/query cycle: exclusive use of `cmd_sem_`, `last_result_`, `last_error_` |
| `vehicle_mutex_` | mutex, take/give | **every** call into the tesla-ble `vehicle_` object (send, `loop()`, `on_rx_data`, `set_connected`) |
| `cmd_sem_` | binary semaphore | signals "result callback ran" from the BLE RX task to the waiting command |
| `cache_mutex_` | mutex, RAII, leaf | the `last_known_*` caches (`std::string` members ⇒ an unlocked copy is torn-read UB) |

**Normative order:** `command_mutex_` → `vehicle_mutex_` → `cache_mutex_`. Acquire strictly
left-to-right; never take a lock while holding one to its right. Corollaries, each load-bearing
today:

- `vehicle_mutex_` is held only for the library call itself — **never across the `cmd_sem_`
  wait** (the RX task needs it to deliver the very result being waited for; holding it would
  deadlock every command into its timeout).
- `cache_mutex_` is a **leaf**: held only for a plain struct copy/assignment, never while
  calling out (library, BLE, NVS, logging) and never while taking another lock. It *is*
  legitimately taken while `vehicle_mutex_` is held — the RX parse path
  (`on_rx_data`/`loop()` under `vehicle_mutex_`) synchronously fires the `set_*_state_callback`
  cache writers — which is exactly the vehicle→cache order above.
- `clear_session_and_cache_()` takes `vehicle_mutex_` internally, so it must **not** be entered
  while holding it (non-recursive mutex ⇒ self-deadlock; see the comment at its definition).
- `last_result_`/`last_error_` need no lock of their own: the RX callback writes them **before**
  giving `cmd_sem_`, and the command task reads them only **after** taking it — the semaphore
  is the ordering edge. `command_mutex_` guarantees a single waiter at a time.
- `cmd_in_flight_` (atomic, `tk::InFlightGuard`) is set only under `command_mutex_`; `loop_task`
  reads it to pause background polls — a flag, not a lock; it orders nothing.

### Task inventory

Application-task priorities are declared **only** in [`main/task_config.hpp`](../main/task_config.hpp)
(`tk::kPrio*`) so relative order is reviewable in one place; stack sizes stay at the
`xTaskCreate` sites with their sizing rationale. Current inventory:

| Task | Priority | Stack | Created in | Purpose |
|---|---|---|---|---|
| `vehicle_loop` | `kPrioVehicleLoop` = 5 | 8192 | `vehicle_ctrl.cpp` (fn: `vehicle_telemetry.cpp`) | pump `vehicle_->loop()`, rotating NO_WAKE telemetry poll, sleep gating, BLE-fault link reset |
| `captive_dns` | `kPrioCaptiveDns` = 5 | 4096 | `provisioning.cpp` | captive-portal DNS (setup-AP mode only; vehicle stack not running) |
| `ota` | `kPrioOta` = 5 | 8192 | `ota_update.cpp` | OTA download + flash (transient) |
| `ota_chk` | `kPrioOtaCheck` = 5 | 8192 | `ota_update.cpp` | OTA manifest check (transient) |
| `auto_pair` | `kPrioAutoPair` = 4 | 8192 | `vehicle_ctrl.cpp` (fn: `vehicle_pairing.cpp`) | pairing supervisor: enrol / re-pair / health probe |
| `wifi_wd` | `kPrioWifiWatchdog` = 4 | 3072 | `main.cpp` | ghost-association watchdog (re-associate, never reboot) |
| `mqtt_pub` | `kPrioMqttPub` = 4 | 6144 | `mqtt_ha.cpp` | MQTT/HA publisher (reads the caches) |
| `display` | `kPrioDisplay` = 3 | 6144 | `display.cpp` | ST7735 renderer (`CONFIG_TESLA_DISPLAY_ENABLED` builds) |
| `ota_gate` | `kPrioOtaGate` = 3 | 3072 | `main.cpp` | one-shot OTA rollback health gate (~90 s) |
| `led` | `kPrioLed` = 2 | 3072 | `led_status.cpp` | APA102 status LED (`CONFIG_TESLA_LED_ENABLED` builds) |

Not in the table (ESP-IDF-owned, priorities from IDF Kconfig, not `task_config.hpp`): the
**NimBLE host task** — it runs the RX data callback, i.e. `vehicle_mutex_` → parse →
`cache_mutex_` and every `set_*_callback` lambda; the **esp_http_server task** — it runs every
HTTP/MCP handler, i.e. the `command_mutex_` cycles and cache copies; plus the usual esp_timer /
WiFi / LwIP system tasks.

### Atomics doctrine

A member is a `std::atomic` **only** when it is a single scalar — flag, counter, or tick
stamp — crossing tasks with no multi-field consistency requirement (`pairing_lost_`,
`cmd_in_flight_`, `cmd_fail_streak_`, `last_contact_ticks_`, …). Anything read or written as
a *group* — above all structs holding `std::string` — goes under a mutex (`cache_mutex_` for
the caches). The test: if two fields must be observed consistently together, that is a mutex,
not two atomics.

### Deferred: owned BLE-ops queue

The structural alternative to the flags above: one owner task serializing *all* tesla-ble
access via message passing, replacing the `command_mutex_`/`vehicle_mutex_`/`cmd_in_flight_`
coordination by construction and giving commands true queue priority over background polls.
**Deliberately deferred** (architecture review 2026-07, P7): the current compensations
(`cmd_in_flight_` poll pause, `cmd_fail_streak_` link-drop backstop) are live-tested and
stable, the command surface is not growing, and the rework would touch the most
incident-prone code for a structural — not behavioural — win, while costing static
task+queue memory on the tightest targets. **Revisit triggers:** (a) a new command class
lands (e.g. upstream tesla-ble registers `scheduledDepartureAction`), or (b) another
queue-position incident occurs despite `cmd_in_flight_`.
