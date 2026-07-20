# Architecture reference

Deep internal reference for tesla-key-esp32. This is the **on-demand** companion to
[`.claude/CLAUDE.md`](../.claude/CLAUDE.md): CLAUDE.md carries the always-needed essentials
(build/flash, component map, NVS table, command list, HTTP API, memory constraints); the
full narrative lives here so it isn't reloaded into every session. Read this when working on
telemetry, the MQTT bridge, the web UI live feed, WiFi/LAN connectivity, sleep/link-state, pairing,
OTA, or anything that touches locks/tasks (the Concurrency contract at the end). Keep both in sync —
the `project-review` skill checks for drift between them.

## Web UI live feed (`/events`)

The web UI's live data is **WebSocket-only** — there is no interval polling and no HTTP poll
fallback. On load the browser (`main/www/app.js` `boot()`) opens ONE socket to `ws://<device>/events`
and sends the text `sub`; the device answers with an immediate `/status` snapshot and thereafter a
background task pushes a fresh `/status` frame every ~2 s. Each frame is exactly the object
`GET /status` returns — `build_status_object()` (`http_status.cpp`) is the single builder both paths
share, so the pushed frame and a manual `GET /status` can never drift, and the client just calls the
existing `render()` on each frame (no envelope).

- **Server** (`main/http_events.cpp`): an 8-slot fd registry (mutex-guarded), a broadcast task
  (`ws_broadcast_task`, ~2 s cadence, self-gated on `ws_any_clients()` so it costs nothing when no
  browser is open), and the `/events` handler. The handler is registered **raw**
  (`httpd_register_uri_handler` with `is_websocket=true`) BEFORE the `/*` wildcards — so it is the
  ONE route NOT reached through the `handle_all` try/catch, and it guards its own allocations
  (a `std::bad_alloc` on this memory-tight chip must drop a frame, never unwind through httpd's C
  frames → `std::terminate` → reboot). Needs `CONFIG_HTTPD_WS_SUPPORT=y` (base `sdkconfig.defaults`,
  target-agnostic) and `config.close_fn` (`http_server.cpp`) to drop a closed fd from the registry.
- **Frame policy** is the pure, host-tested `main/logic/ws_policy.hpp`: `ws_frame_plan()` decides
  from a frame's *announced* length — before any payload is read — whether to skip an empty frame,
  read one that fits the 16-byte command buffer, or **close** an oversized one (undrainable: httpd
  leaves its body in the socket and offers no skip, so the stream is desynchronized and dropping the
  connection is the only honest exit — sizing a buffer from a client-asserted 64-bit length would be
  a one-frame OOM). `ws_frame_action()` then classifies the bytes actually read; only `sub`
  subscribes.
- **Send backpressure** (also `ws_policy.hpp`, host-tested) bounds what ONE non-reading subscriber
  can cost. A client that stops reading (suspended laptop, backgrounded tab) leaves its TCP send
  buffer full, so each async send blocks the httpd task for the full `send_wait_timeout`
  (`HTTPD_DEFAULT_CONFIG`: **5 s**) before failing `EAGAIN` — while the broadcast task keeps
  producing every 2 s, each frame owning a heap copy of the whole `/status` JSON. Unbounded, that
  backlog exhausted the heap (69 KB → 8 KB free, largest block 31744 → 544 B in ~7 min) and left
  the device **wedged, not crashed**: no reboot, just `std::bad_alloc` spinning in the vehicle loop
  for hours, unreachable (live incident 2026-07-18). Two bounds prevent it: at most
  `WS_MAX_INFLIGHT` (2) queued-but-uncompleted frames per client — over that the client is skipped
  for the tick, so the backlog has a ceiling — and after `WS_MAX_FAILS` (3) consecutive failed
  completions the subscriber is closed (`httpd_sess_trigger_close`), which also ends the 5 s httpd
  stall it imposed every tick. Closing is cheap: `app.js` reconnects 3 s later, so a laptop that
  merely slept resubscribes on wake.
- **Client** (`app.js`): `render(JSON.parse(frame))` on each message; on close, keep the last
  rendered state and reconnect after 3 s. `poll()` is repurposed to `ws.send("sub")` — an
  on-demand snapshot request after a user action (charge/wake/gen-key), so the UI refreshes at once
  instead of waiting for the next push, still purely over the socket. `waitReboot()` still does a
  plain `GET /status` — that is post-OTA reboot detection, a different concern from the live feed.

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
having committed it at startup. A **deliberate, user-initiated reboot** inside that window is a
different case, though: the three config handlers that reboot (`/set_vin`, `/set_mqtt`,
`/set_syslog`), the setup-portal save **and the heap watchdog's deliberate restart** call
`ota_confirm_pending_image()` first — a restart we chose is proof the image runs, so it must not
look like a failed boot and roll the update back. It is a no-op on a normal boot / already-valid image. (An unattended
brownout/power-cycle in the window still reverts — that is the crash-safety net working as intended.)

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
4 KB signature, and the largest — esp32c5 (the only target with display+PSRAM), ~0x1e1000 signed — clears it by ~28 KB.
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
five images are the same tesla-ble revision. This patch is planned debt: the retirement plan
(one-line upstream `targets:` PR, then drop the script/routing) is
[`adr/0001-esp32c5-target-upstreaming.md`](adr/0001-esp32c5-target-upstreaming.md); the wider
tesla-ble dependency strategy (IDF-6 / Mbed TLS 4 crypto seam, issue #61) is
[`adr/0002-idf6-mbedtls4-crypto-seam.md`](adr/0002-idf6-mbedtls4-crypto-seam.md).

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

- **Config:** broker URI from NVS `mqtt_uri` (web UI: Connections → MQTT, stores `host:port`)
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

## Syslog forwarder

`main/syslog.cpp` forwards the same in-RAM diag log served by `GET /diag` (`main/diag_log.cpp`)
to a UDP Syslog collector, best-effort, framed as RFC 5424 (`<14>1 - tesla-key-esp32 - - - -
<message>`, facility `user`/priority `info` throughout — there is no per-line severity mapping).
The capture point is `diag_log.cpp`'s `esp_log_set_vprintf` hook, which already mirrors every
`ESP_LOG*` line (from this firmware **and** ESP-IDF/NimBLE internals — NimBLE is pre-throttled
to `WARN` there) into the `/diag` ring; the same call also queues the line for Syslog, so there
is exactly one capture point to keep in sync, not two.

- **Config:** one NVS string, `syslog_uri` (`tesla_cfg` namespace) — a bare `"host:port"`, no
  scheme (a bare host defaults to port 514); `""` disables forwarding. Falls back to
  `CONFIG_TESLA_SYSLOG_SERVER` (Kconfig, default empty). Set from the web UI (Connections →
  Syslog card, pencil icon → `POST /set_syslog`, `{"server":"host:port"}`) or NVS/Kconfig
  directly. Resolved **once**, at `syslog_start()` (called early in `app_main`, before WiFi) —
  like the MQTT bridge, a config change persists then reboots to apply, so there is nothing to
  re-read at runtime.
- **Delivery:** a background task queues lines (fixed 24-deep queue of 256-byte messages —
  small on purpose, since the queue is one contiguous heap allocation and this device's binding
  memory limit is the largest *contiguous* free block) and, once WiFi is up, resolves the
  target via `getaddrinfo()` and forwards. Re-resolve + re-probe is throttled to a ~10s cadence
  (`have_checked`, not `!resolved` — a persistently failing DNS/host must not re-run
  `getaddrinfo()`+ping every loop). **Delivery gates on DNS resolution only** (`resolved`) —
  never on the reachability probe below — since Syslog is inherently best-effort UDP.
- **Reachability probe (advisory only, never a delivery gate):** ARP for an on-subnet host (L2,
  works even when the collector firewalls ICMP), else a 2-echo ICMP ping (800 ms timeout each).
  Surfaced in `/status.syslog.reachable` purely as a UI hint ("Enabled · not answering ping").
- **Send-failure handling** (`logic/syslog_policy.hpp`, host-tested): an errno from
  `sendto()`/`socket()` is classified HARD (routing/host-down errors — `ENETUNREACH`,
  `EHOSTUNREACH`, `ENETDOWN`, `EHOSTDOWN`, `EADDRNOTAVAIL` — re-resolve + re-probe immediately)
  or TRANSIENT (everything else, incl. `ENOMEM`/`ENOBUFS`/`EAGAIN` — hold the destination, let
  the ordinary cadence re-check). Getting this wrong the other way — clearing the throttle on
  every failure — turns a chatty diag stream (a busy BLE poll can log several lines a second)
  into a `getaddrinfo()`+ping storm that runs hardest exactly when the link is worst; the
  handler also logs the failing/recovering *transition* only, never per-line, for the same
  reason. Mirrors an equivalent module in the sibling `daikin-altherma-esp32` project, where
  this exact storm was diagnosed on a live board.
- **Loop guard:** `syslog_send()` drops any captured line containing the `"syslog:"` tag this
  module's own `ESP_LOGx(TAG, ...)` calls render (`TAG = "syslog"`) — otherwise this module's
  own "send failed" diagnostics would themselves be queued for (failing) delivery, feeding the
  exact storm the paragraph above avoids.
- **Status:** `syslog_status()` → `/status.syslog` (`configured`/`resolved`/`reachable`/`host`/
  `port`/`error`), read by the web UI's Connections card exactly like the MQTT row.

## Heap-exhaustion watchdog (the last-resort escalation)

Every OOM guard in this firmware turns "out of memory" into **recover and continue** — the
`handle_all` try/catch answers 503, the BLE parse guards reset the link, the `/events` broadcast
drops a frame. That is correct for a **transient** shortage and must stay. What was missing is the
next question: *what if it never recovers?*

On **2026-07-18** a non-reading WebSocket subscriber exhausted the heap (fixed separately by the
`/events` send backpressure). The device then sat at `free=14820`, `largest_block=768` — against a
healthy 31744 — for **ten hours**. It never crashed and never rebooted: `vehicle_->loop()` threw
`std::bad_alloc`, the handler reset the BLE link, the next 50 ms iteration threw again, ~20×/s all
night. HTTP could not serve, MQTT could not reconnect, and the WiFi watchdog was itself dead
(`ping_sock: create ping task failed` — it could no longer allocate its own task). **A hang is the
worst failure shape available**: a crash reboots in seconds, a wedge looks like a powered-off
device, heals never, and reports nothing.

The escalation lives in the pure, host-tested `main/logic/heap_watchdog.hpp`, sampled by
`loop_task_fn_` at the existing 30 s heap-log site:

- **Trigger:** `largest_block` below **`kHeapCriticalBytes` (4 KB)** *continuously* for
  **`kHeapCriticalHoldMs` (5 min)**. Healthy steady state is 31744 B and the wedge sat at
  480–1536 B, so the threshold separates them without a tight estimate of either.
- **`largest_block`, never `free`.** The binding limit on this chip is the largest contiguous
  block. During the incident `free` held a plausible-looking ~16 KB the whole time — a total-free
  test would never have fired.
- **`MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL`, never plain `8BIT`.** `heap_caps_*` reports the max
  across every heap carrying the cap, and the **esp32c5 registers 8 MB of PSRAM** into `8BIT`
  (`CONFIG_SPIRAM_USE_MALLOC`). A C5 in the exact wedge — internal DRAM at 768 B — would read
  ~7.8 MB and never trigger, making the watchdog a silent no-op on the one target with the extra
  RAM. The thresholds are internal-DRAM numbers and only mean anything against an internal-DRAM
  sample. The `HEAP` log line carries `internal_largest=` alongside the historical `8BIT` figures
  (identical on the four PSRAM-less targets, so old captures stay comparable).
- **An unbroken run, never a single `bad_alloc`.** One failed allocation is exactly the transient
  the other guards absorb. A single recovered sample resets the clock. This is what keeps the
  watchdog from becoming a reboot loop — which matters because each boot re-opens the active
  polling window, so a rebooting device keeps a parked car awake. (That objection is weaker than
  it looks: the wedged state defeats car sleep too, and harder — it reset the BLE link at 20 Hz
  for ten hours.)
- **Excused during an OTA.** `esp_https_ota` holds the largest allocations this firmware ever
  makes, so a low reading there proves nothing, and a restart mid-install is the one reboot that
  could leave a half-written slot. An OTA **clears** the run rather than skipping the sample —
  skipping would let a run that began *before* the download resume its clock and fire during it.
  Queried via `ota_is_busy()` (one atomic), never `ota_get_status()` (copies `std::string`s and
  can throw on the very heap this is deciding about).
- **On fire:** `ESP_LOGE`, persist `reboot_why=heap:<n>` to NVS `tesla_cfg`,
  `ota_confirm_pending_image()` so a deliberate restart inside the ~90 s OTA health gate is not
  mistaken for a failed boot, a **300 ms `vTaskDelay`**, then `esp_restart()`. The delay is not
  cosmetic: `syslog_send()` only *queues*, and its task runs at priority 3 against `loop_task`'s
  5, so without a yield the final message dies in the queue on a single-core target — and the
  `/diag` ring does not survive the reboot either. Skipping it would throw away the one line that
  explains the restart.
- **Bounded:** after **`kHeapMaxConsecutiveRestarts` (5)** consecutive watchdog restarts it stops
  restarting and says so once. Five cycles prove a restart is not fixing this one, and continuing
  would cycle the radios every ~10 min indefinitely. Relatedly, a boot that followed a watchdog
  restart **does not seed the active polling window** (`vehicle_ctrl.cpp` `init()`) — that seeding
  is what would make a restart loop expensive for a parked car, and a self-healed device has no
  user waiting on a warm cache. The count resets on any ordinary boot (power cycle, crash, OTA),
  since those leave no breadcrumb, so this only ever bounds a genuine loop.
- **Afterwards:** `main.cpp` takes the NVS breadcrumb once at boot (read + clear) and holds it for
  the process; `/status` reports it as **`last_reboot`** (omitted on every ordinary boot). This
  exists because `esp_reset_reason()` cannot tell a deliberate `esp_restart()` from a user power
  cycle — both read SW/POWERON — so without it a device that self-heals unattended at 04:00 leaves
  no trace, and the next investigation starts from scratch.

Nothing on this path allocates **in our code** — the `"heap:<n>"` breadcrumb is at most 9 chars and
lands in `std::string`'s inline buffer — and the one call that can allocate internally (NVS)
returns `ESP_ERR_NO_MEM` rather than throwing and is `try`-guarded anyway. That matters because
this code runs precisely when allocation is failing: a throw escaping here would unwind into the
net-less loop task and turn a wedge into an `abort()`.

### Why a restart, and not in-place recovery

Rebooting is the crude answer, so it was only adopted after the alternatives were researched and
found worse. The short version: **ESP-IDF offers no reliable way to reclaim a wedged heap from
inside the running image**, and the teardown paths one would have to call are themselves among the
least reliable code in the SDK. Evidence for each candidate:

- **Subsystem teardown/reinit** (`httpd_stop`/`httpd_start`, `esp_mqtt_client_destroy`,
  `esp_wifi_deinit`, `nimble_port_deinit`) — the obvious "smarter" fix, with the worst track
  record. `nimble_port_deinit` leaked *twice in a row*: Espressif fixed one leak, and a second,
  independent one survived it ([esp-idf#8136](https://github.com/espressif/esp-idf/issues/8136)).
  `esp_wifi_deinit` leaked per cycle on IDF 5.0/5.1 (a regression absent in 4.4, fixed in 5.2 —
  [#12014](https://github.com/espressif/esp-idf/issues/12014)), was reproducibly crash-prone in the
  3.x era ([#2050](https://github.com/espressif/esp-idf/issues/2050)), and
  [esp32.com t=13189](https://esp32.com/viewtopic.php?t=13189) documents a supplicant callback no
  deinit path ever frees. `esp_http_server` leaked **~16.7 KB per stop/start cycle** on IDF 5.3
  because `httpd_stop()` never released the with-caps task stacks — and the *first upstream fix for
  that* crashed on an assert ([#14266](https://github.com/espressif/esp-idf/issues/14266)). The
  pattern is not "one bad release": whether deinit returns its memory depends on the exact IDF
  revision and is patched reactively. On top of that, deinit paths **allocate**, and this code runs
  when allocation is already failing — a throw there unwinds into the net-less loop task and
  `abort()`s, trading our controlled restart for an uncontrolled one *without* the breadcrumb. The
  crash-only literature names this directly: a restart is trustworthy only when it is implemented
  outside the failing component and does not run that component's own code, and rarely-exercised
  cleanup paths are unreliable *because* they are rarely exercised
  ([Candea & Fox, HotOS-IX](https://www.usenix.org/legacy/events/hotos03/tech/full_papers/candea/candea.pdf)).
- **A ballast / rainy-day block** freed under pressure is a real pattern — libstdc++ ships one (a
  preallocated emergency pool so `std::bad_alloc` can still be *thrown* once malloc fails, which is
  also the most likely explanation for how the wedged device kept throwing for ten hours instead of
  crashing). Rejected here because it costs permanent internal DRAM out of a ~31 KB steady-state
  largest block, and this restart path needs no headroom: it is allocation-free by construction.
- **`heap_caps_register_failed_alloc_callback()`** is official, but it runs *synchronously inside
  the allocator*, in the context of whatever task or ISR hit the failure; it takes the size, the
  caps and the function name, returns nothing, and cannot satisfy or retry the allocation. ESP-IDF
  documents no constraints on what is safe inside it. That makes it a sensor, not an actor — and
  the 30 s sample site already covers the sensing. Espressif's own built-in escalation for exactly
  this condition is `CONFIG_HEAP_ABORT_WHEN_ALLOCATION_FAILS` → `esp_system_abort()`, i.e. a reset
  with *less* ceremony than ours.
- **Defragmentation** does not exist: the heap docs treat fragmentation-induced failure as expected
  behaviour and offer no compaction, and part of the TLSF allocator lives in chip ROM, so even
  allocator-level improvements cannot reach released branches. Espressif's only in-place lever is
  carving dedicated regions for critical objects (`heap_caps_add_region_with_caps`), and a
  MicroPython maintainer judged split-heap of marginal value on ESP32 once WiFi and BLE must
  coexist ([micropython#8940](https://github.com/micropython/micropython/issues/8940)). This
  project already does the containment half — the static `/diag` ring, the gzipped web UI, the
  PSRAM framebuffer on the C5 — but that is *prevention*, and prevention has no answer for the
  morning after.
- **What shipping ESP32 firmware actually does** is bounded restarting, not in-place repair.
  ESPHome's [safe mode](https://esphome.io/components/safe_mode/) is a restart ladder: a persisted
  boot-failure counter (default 10), a boot that only counts as "good" after a healthy-uptime
  window, a degraded mode keeping just logging/network/OTA — and it reboots *again* after five
  minutes. [Tasmota](https://tasmota.github.io/docs/Device-Recovery/) counts restarts and escalates
  to a settings wipe and finally a reflash. Neither attempts to heal a live heap.

So the reboot is the *last* rung, and the parts that make it defensible are the hygiene around it,
all of which match the same literature: a **long unbroken trigger** so transients never reach it, a
**restart cap** (Candea & Fox prescribe a maximum retry limit precisely to prevent reboot cycles),
and a **breadcrumb persisted before the reset**, which Memfault's watchdog guidance names as the
thing that separates a diagnosable reset from a mystery.

**What is deliberately not here:** a *load-shedding rung* before the restart — closing `/events`
subscribers and stopping MQTT to release the heap this firmware itself owns, and only restarting if
`largest_block` does not recover. That would have healed the 2026-07-18 incident in place, since
the leak was in our own WS structures. It is the strongest candidate for a next rung, but it only
helps for leaks in structures *we* know about, and it stays out of this change until a second
incident shows the pattern is worth the complexity.

### Reading it in syslog

Syslog is the only post-mortem source that outlives the restart — the `/diag` ring is RAM and
`esp_reset_reason()` cannot tell a deliberate `esp_restart()` from a power cycle. So the escalation
narrates itself; a reader should be able to reconstruct the whole decision from these lines alone:

| Line | Meaning |
|------|---------|
| `HEAP free=… largest_block=… min_free=… internal_largest=…` | the ordinary 30 s trend line, always present |
| `HEAP CRITICAL: … watchdog ARMED, restarting in 300 s unless it recovers` | the countdown just opened |
| `HEAP CRITICAL for <n> s … restarting in <m> s unless it recovers` | still critical, one line per 30 s sample — this is the proof the shortage was *sustained*, not a spike |
| `HEAP recovered after <n> s critical … watchdog disarmed` | the run ended on its own; no restart |
| `HEAP critical run (<n> s) cleared: an OTA is in flight …` | the run was excused, not healed |
| `HEAP EXHAUSTED for <n> s … RESTARTING DELIBERATELY (watchdog restart <k>/5, reboot_why=heap:<k>; …)` | the restart, with the state that caused it |
| `HEAP EXHAUSTED … but <n> consecutive watchdog restarts have not fixed it — NOT restarting again` | the cap held; the device stays up degraded |
| `BOOT this boot was caused by the firmware itself: reason=heap:<k> …` | logged on the *next* boot, closing the loop |

The elapsed times are the **measured** age of the critical run, not the configured hold, so a fired
run legitimately reads somewhat over 300 s (it is sampled on a 30 s cadence).

**Keep any line on this path under ~230 characters.** `diag_log.cpp`'s capture hook formats into a
256-byte *stack* buffer (deliberately — it must not allocate on the heap it is reporting about),
and anything longer reaches both `/diag` and syslog **cut off mid-sentence**. The restart line is
the one that must never be truncated, so it carries the state and a pointer here, not the
reasoning itself. The `BOOT` line is
emitted *after* `syslog_start()` on purpose: `syslog_send()` is a no-op before that, so logging it
at the point the breadcrumb is read — which is where it naturally belongs, and where it used to be
— would confine the one line explaining an unattended 04:00 self-heal to the RAM ring that the
restart just erased.

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
reached the car yet) it **hides the hero card entirely**. Both states know nothing current about the
car, and a hero filled with a retained battery percentage and an idle timer reads as live status;
withholding the card is the honest form, and the state is still signalled — never as a sleep claim —
on the BLE row. In that same
unknown/unreachable state the BLE connection row drops its green and animates an orange
ping-pong across the signal bars (a darker-orange crest bouncing edge→edge over a light-orange
base) with an orange MAC, flagging "connected but stateless" at a glance. The momentary BLE row
reading "Disconnected" is normal (the link is dropped between polls by design) and is not used
to drive the hero — only `link` is.

**BLE phase countdown (the Bluetooth row's "(7s left)" / "(retry in 22s)").** The row names
which phase the radio is in and counts it down, so an idle-looking device visibly explains
itself instead of sitting on a static label. `/status.ble` carries `phase` + `phase_s` — always both or
neither — decided by the pure, host-tested `main/logic/ble_phase.hpp` from two
independently-armed deadlines:

- **`connecting`** — a scan/connect attempt is running and gives up in `phase_s`. Armed by
  `ensure_connected_` (`vehicle_commands.cpp`), the ONE place any attempt is started and
  bounded, so every command, telemetry poll and health probe gets the countdown for free.
- **`waiting`** — no attempt is running; the next one starts in `phase_s`. Armed by
  `idle_until_next_health_poll_` (`vehicle_pairing.cpp`), which owns both the wait and the
  countdown for it — they come from one constant, so the row cannot promise a retry at a time
  the loop doesn't retry.

The two phases **overlap** routinely: a command, or `loop_task`'s warm-up connect, starts an
attempt in the middle of auto-pair's idle wait. `connecting` therefore outranks `waiting` (the
attempt is the more specific truth), and because neither deadline clears the other, the idle
wait's countdown reappears when the attempt ends instead of the row going bare. In the web UI
each row's countdown node declares the one phase it will render, so "Searching…" can never be
suffixed with the *retry* countdown.

**The row's state is decided by a presenter, not inline in the UI.** `main/logic/ble_row.hpp`
(`tk::ble::decide`) maps the `/status` `ble` block to one of five row states plus the countdown
that belongs beside it, and `main/www/app.js` only renders that decision. The two are kept
identical by `scripts/check-ble-row-parity.sh`, which dumps the C++ decision over an exhaustive
input sweep and re-decides it with the JavaScript that actually ships — the same arrangement
`display_model.hpp` has with `tools/display_sim.py`. This row was the last UI surface still
deciding "what to show" in untestable browser code, and it is where three rounds of user-visible
bugs landed.

**The label follows the phase, not `ble.scanning`** — enforced structurally: `scanning` is not a
field of `RowInputs` at all, so a label driven off it is unrepresentable rather than merely
tested-for. The radio also runs a background warm-up
scan (`loop_task`) that has no deadline of its own and lasts straight through the idle wait, so
keying the label off the raw scanning flag flipped the row to "Searching…" in the middle of a
"retry in …" countdown — label and number describing different phases, which read as the row
jumping around. `phase === "connecting"` is now the ONE thing that says "Searching…"; everything
else is "Disconnected". Each row's countdown node names the single phase it will render, so a
mismatch shows nothing rather than a foreign number.

The time sits at the row's **right edge** (`margin-left:auto`), in the column the tile rows put
their edit pencil in, and stays muted in every phase so it reads as one steady right-hand column
instead of recolouring with the label. The disconnected row draws **outlined, unfilled** signal
bars — an empty gauge rather than a dimmed reading; the searching row uses the same amber
(`--warn-base`) the "link up, nothing known yet" bars already use, so the BLE row
has one amber "in-between" language. Wi-Fi's own search stays green.

`phase_s` rounds **up** and `0` is a real value meaning "right now" — never "no countdown".
Gating on `> 0` (or truncating) is what made the first cut of this drop its last second and
flash a bare "Disconnected" between every cycle. `app.js` ticks the number down locally once a
second between the ~2 s `/events` pushes, resyncing to the device only on a phase change or a
≥2 s disagreement so the two clocks can't jitter the number back upward; it paints into a
dedicated node with `textContent`, because rewriting the row through `setHTML` every second
would re-create the bar `<rect>`s and restart their CSS fill animation on every tick.

Not every scan is a phase. `loop_task`'s warm-up connect calls `BleClient::connect()` directly
rather than going through `ensure_connected_`, so it arms nothing and just leaves the radio
scanning, on and off, straight through the idle wait — under the label-follows-phase rule that
correctly reads as "Disconnected · retry in …", because the link *is* down and the next real
attempt *is* scheduled. The one row that shows "Searching…" with no countdown is the no-VIN
listing-only scan, which has no pairing schedule to count down at all.

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

**One spec table drives everything — including the REST surface.** The command registry in
`logic/command_registry.hpp` (`kCommands`, `CmdArg`, `kCmdMaxArgs`) carries each command's
REST name, MCP tool name + description, AND each argument's per-surface keys with ONE
shared `{lo,hi}` bounds pair. The advertised `tools/list` JSON schema, the MCP executor's
validation, and the REST `/command` clamp (`http_api.cpp`) are all generated from that
table, so schema-vs-enforcement drift — and any `/api`-vs-`/mcp` disagreement about names
or ranges — is impossible by construction. Surface semantics stay deliberately different:
MCP is strict — an absent required argument OR a present-but-unparseable one is a `-32602`
protocol error (silently defaulting `set_scheduled_charging`'s `enable` would *disable*
the schedule and report success); loose-but-unambiguous encodings are coerced (numeric
strings for ints, 0/1 for bools); parsed integers are clamped to the spec bounds before
the int cast (UB guard) — while REST stays lenient for TeslaBleHttpProxy compat (absent →
the spec's `api_default`). Both surfaces execute through the single kind→controller
dispatch in `command_exec.cpp`. The registry, method routing, version table, clamp and the
shared command-outcome text (`logic/command_result.hpp`, also used by the REST
`/command` reason so the two paths can never diverge) are IDF-free and covered by the
host mock build (`test/test_logic.cpp`, `test_mcp` — including a pin on the `tools/list`
row order, which is the registry's table order). The tool set itself — exactly the
run-on-key charging commands plus cache-only `get_vehicle_state`, role-refused commands
deliberately absent (`mcp_name == nullptr`) — is documented with the full per-tool table
in [`MCP.md`](MCP.md#tools).

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
| `ws_bcast` | `kPrioWsBroadcast` = 4 | 6144 | `http_events.cpp` | `/events` live-status push (self-gated on `ws_any_clients()`) |
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
