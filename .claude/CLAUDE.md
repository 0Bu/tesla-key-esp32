# tesla-key-esp32

ESP-IDF 5.x project for the ESP32 family. Acts as a BLE↔HTTP proxy for Tesla vehicles,
API-compatible with TeslaBleHttpProxy (works as evcc BLE vehicle integration). Builds for
**five targets — esp32, esp32s3, esp32c3, esp32c6, esp32c5** — from ONE source tree; CI
builds all five. The first four are exactly what yoziru/tesla-ble declares in its
`idf_component.yml` `targets:` (the Component Manager refuses any other chip). **esp32c5**
(LilyGO T-Dongle-C5, dual-band Wi-Fi 6) is NOT in that upstream list, so it is added by a
local build-time patch of tesla-ble (`scripts/prepare-tesla-ble-c5.sh` clones the pinned tag
and appends esp32c5 to its `targets:`; `main/idf_component.yml` routes ONLY the c5 target
through that local copy — the other four resolve byte-identically from git). Our own code is
target-agnostic; the only C5 blocker was that one manifest line.

> **Deep reference:** this file holds the always-needed essentials. The full narrative for
> telemetry, the MQTT/HA bridge, WiFi/LAN reconnect, sleep/link-state, pairing and OTA lives in
> [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md) — read it on demand when touching those
> areas. User-facing docs: [`README.md`](../README.md), [`docs/README.md`](../docs/README.md),
> [`docs/SECURITY.md`](../docs/SECURITY.md), [`docs/MCP.md`](../docs/MCP.md) (MCP integration
> guide). Keep all of these in sync (the `project-review` skill checks for drift).

## Environment note (Claude Code on the web / remote sandbox)

A cloud session **cannot build** (no working Docker daemon for `scripts/idf-docker.sh`) and
**cannot USB-flash** (no USB passthrough) — it is for editing, review and CI-driven builds.
The `report-capabilities.sh` SessionStart hook prints what the current environment supports.
Real builds/flashes run on a host with Docker + the board attached (see the `flash-esp32`
skill) or in CI (`.github/workflows/build.yml`). The `build-efficiency-check.sh` SessionStart
hook audits the latest post-merge `build` run on main for efficiency regressions (ccache hit
rate, cache hygiene, build-duration/total-run regression, binary-size headroom) and, on a
problem, has the session open an Issue / draft Fix-PR — deduped per run id, never auto-commits.

**But there IS a real local verification loop** — the host-side mock build runs the project's
pure logic with the plain system toolchain (no ESP-IDF/Docker/board), so logic changes can be
*verified*, not just reasoned about, even in a cloud session:

```bash
scripts/run-mock-tests.sh   # compile + run host logic tests in seconds (cmake + g++/clang++)
```

It covers VIN validation, imperial→metric conversion, the `link_state()` four-state machine
(incl. the debounced-ASLEEP asymmetry) and its `/status`/MQTT strings, the per-target
platform/OTA-suffix mapping, the MCP protocol core (version negotiation, method routing,
tool/arg-spec registry, int clamp), the shared command-outcome text, the on-device display
presenter (the priority ladder / SoC gradient / RSSI→bars / SSID-scroll decisions the ST7735
renderer draws), the status-LED ladder (`logic/led_status.hpp`, reading the same shared
`UiSnapshot` + the shared SoC gradient) and the `/events` WebSocket command policy
(`logic/ws_policy.hpp` — frame-length plan / "sub" classification) — all delegated to
IDF-free headers in `main/logic/` so the device runs the same code the test does. CI gates the firmware build on it (`logic-test` job). Add new
hardware-free logic to `main/logic/` and a `CHECK` in `test/test_logic.cpp`. Full detail:
[`test/README.md`](../test/README.md).

## Build & Flash

No local ESP-IDF — builds run via `scripts/idf-docker.sh`, which uses the `espressif/idf`
Docker image **pinned to the version CI builds with** (read at runtime from
`.github/workflows/build.yml`, so build/debug never drifts from CI). Flash from the host with
`esptool` (`brew install esptool`), since Docker on macOS has no USB passthrough. The
`flash-esp32` skill wraps both steps (local tree, no merge); the `ship` skill runs the full
delivery instead — squash-merge the PR, follow the post-merge CI, flash the **signed** CI
artifact (or OTA) and verify the device version. When waiting on CI, block on
`gh run watch <run-id> --exit-status` — never sleep-poll `gh run view` in a loop.

```bash
# Build (first run: set-target; afterwards plain `build` stays incremental).
# The wrapper keeps build/ host-owned and pins the ESP-IDF version to CI.
# Pick your chip; CI builds all five via scripts/ci-build-all.sh.
# For esp32c5 first run `scripts/prepare-tesla-ble-c5.sh` once (clones + patches the local
# tesla-ble copy the c5 target resolves against); ci-build-all.sh does this automatically.
scripts/idf-docker.sh idf.py set-target esp32s3 build   # or esp32 / esp32c3 / esp32c6 / esp32c5

# Configure WiFi, VIN (interactive; can also be set later via the setup AP)
scripts/idf-docker.sh idf.py menuconfig   # → Tesla Key Configuration

# Flash from the host (preserves nvs — @flash_args skips nvs@0x9000). Match --chip to
# the target you built; @flash_args already carries the right bootloader offset.
cd build && esptool --chip esp32s3 -p <port> write_flash "@flash_args"   # or esp32 / esp32c3 / esp32c6 / esp32c5
```

## Architecture

```
main.cpp               → WiFi init, NVS init, start all components
ble_client.cpp         → NimBLE GATT client (BleAdapter impl)
                         Scans for UUID 00000211-b2d1-43f0-9b88-960cebf8b91e
                         Write chr: 0212, Notify chr: 0213
nvs_storage.cpp        → NVS StorageAdapter (maps library keys ≤15 chars)
vehicle_ctrl.cpp       → VehicleController core: init/wiring, VIN gate, link_state() glue
vehicle_commands.cpp   → sync command API via semaphores (send_vcsec_/send_infotainment_,
                         make_result_cb_, all user commands)
vehicle_telemetry.cpp  → protobuf parsers, cache callbacks, loop_task (background poll +
                         sleep gating), data queries
vehicle_pairing.cpp    → auto_pair_task, key mgmt/fingerprint, session invalidation,
                         health probe   (split map: vehicle_ctrl_internal.hpp)
http_server.cpp        → esp_http_server on port 80: wildcard dispatch + the handle_all
                         try/catch OOM guard (503) EVERY handler runs under
http_api.cpp           → evcc routes (/api/1/…, /api/proxy/1/version)
http_status.cpp        → web UI (/), /status, /diag, /scan. build_status_object() is the ONE
                         /status-JSON builder shared by GET /status and the /events WS push
http_events.cpp        → /events — WebSocket live-status push for the web UI. The browser holds
                         ONE ws:// socket; a background task pushes build_status_object() every
                         ~2 s (WS-only, replaces the old browser /status interval poll — no poll
                         fallback). Registered RAW (is_websocket) OUTSIDE the handle_all guard, so
                         it guards its own allocations; 8-client registry; "sub" command policy is
                         host-tested logic/ws_policy.hpp
http_ota.cpp           → /ota/check|update|status
http_config.cpp        → /gen_keys, /send_key, /set_time, /set_vin, /set_mqtt, /set_syslog
mcp_server.cpp         → /mcp — MCP server for AI agents (stateless JSON-RPC 2.0;
                         core logic in logic/mcp.hpp, guide in docs/MCP.md)
                         (shared helpers: http_common.cpp; split map: http_handlers.hpp)
diag_log.cpp           → in-RAM console ring served by GET /diag (static .bss buffer); its
                         esp_log capture hook also feeds syslog.cpp, so every captured line
                         is forwarded too
syslog.cpp              → UDP Syslog forwarder (RFC 5424, best-effort) for the diag log.
                         Server from NVS `syslog_uri` (web UI: Connections → Syslog, POST
                         /set_syslog) or CONFIG_TESLA_SYSLOG_SERVER, "host:port"; "" disables.
                         Resolved once at boot (reboots on change, like /set_mqtt); a
                         background task re-resolves DNS + advisory-probes the collector
                         (ARP on-subnet, else ICMP) every ~10s, throttled on a persistent
                         failure. Delivery gates on DNS resolution only, never the advisory
                         probe. Errno-based hard/transient send-failure split in
                         logic/syslog_policy.hpp (host-tested)
provisioning.cpp       → captive setup portal (setup AP) when no WiFi is configured
display.cpp            → on-device ST7735 status panel (LilyGo T-Dongle-C5 + T-Dongle-S3),
                         LANDSCAPE 160x80 (header WiFi bars+SSID | BT+BLE bars + a horizontal SoC
                         battery) OR PORTRAIT 80x160 (two-row header over a VERTICAL battery filling
                         bottom→top); both draw a red→green gradient / charging bolt / "ASLEEP", or a
                         WiFi/BLE search + "Pairing…" animation. Cache-only (never wakes the car).
                         "What to show" (priority ladder / gradient / bars / SSID scroll) is decided
                         by the pure, host-tested presenter logic/display_model.hpp (an Orient axis
                         picks the SSID geometry) reading the shared logic/ui_state.hpp
                         (VehicleController::ui_snapshot()); this file is the thin renderer
                         (draw_landscape / draw_portrait). Each BOOT tap (C5 IO28, S3 IO0) rotates
                         90° through the 4 orientations — landscape/portrait ± their 180° flips
                         (MADCTL {0xC8,0xA8,0x08,0x68}, same framebuffer, offsets swap 1/26↔26/1);
                         the index persists in NVS tesla_cfg/disp_rot (migrates the old disp_flip).
                         Backlight active-LOW; SPI 20 MHz (C5) / 40 MHz (S3); framebuffer in PSRAM on
                         the C5, internal SRAM on the S3. Compiles to a no-op unless
                         CONFIG_TESLA_DISPLAY_ENABLED (sdkconfig.defaults.esp32c5 + .esp32s3); the ONE
                         esp32s3 image auto-detects the T-Dongle-S3 (SD pull-ups) so a generic
                         ESP32-S3 stays panel-less. Font from tools/display_sim.py → main/display_font.h
led_status.cpp         → on-device status LED: the single underside APA102 pixel (T-Dongle-C5/S3)
                         as a colour+animation indicator (WiFi/BLE search, pairing, charging, SoC,
                         OTA, warn/error). Reads the SAME shared logic/ui_state.hpp the display does
                         (one input contract) + a tiny LedAlerts for its latched tiers; ladder +
                         colours host-tested in logic/led_status.hpp. Cache-only (never wakes the
                         car), no MQTT, works without a panel. APA102 bit-bang, no heap. Compiles to
                         a no-op unless CONFIG_TESLA_LED_ENABLED (opt-in, default off; pins from Kconfig)
www/                   → web UI sources: index.html (markup) + style.css + app.js, spliced
                         into ONE self-contained page at build time (inline_assets.cmake,
                         byte-equivalent to the former monolith) and served pre-gzipped
```

## Key Dependency

`yoziru/tesla-ble` v5.1.1 — fetched via IDF Component Manager (see main/idf_component.yml).
After first `idf.py build`, the library lands in `managed_components/yoziru__tesla-ble/`.
Never edit files in `managed_components/` — they are regenerated.

## NVS Namespaces

| Namespace   | Content                                     |
|-------------|---------------------------------------------|
| `tesla_cfg` | WiFi SSID/pass, VIN, BLE MAC, `mqtt_uri`, `syslog_uri`, `last_time`, `disp_rot` (on-device display BOOT-rotation index 0..3; C5/S3; migrates old `disp_flip`) (runtime cfg) |
| `tesla_ble` | Private key (`private_key`), VCSEC session (`sess_vcsec`), Info session (`sess_info`), `key_created`, `paired_at` — the `sess_*` names come from the ≤15-char key mapping in `nvs_storage.cpp` |

## Commands Implemented

**Run on the Charging-Manager key (actually execute):** `charge_start`, `charge_stop`,
`set_charging_amps`, `set_charge_limit`, `set_scheduled_charging`
(`{"enable":bool,"start_minutes":int}` — minutes after local midnight; daily charge start time.
Scheduled *departure* is not exposed: the tesla-ble version in use registers no builder for
`scheduledDepartureAction`), `charge_port_door_open/close`, `wake_up`.

**Accepted by the API but rejected by the car for the Charging-Manager role** (sent for API
completeness, never execute — the key has no door/body/climate privilege): `door_lock/unlock`,
`flash_lights`, `honk_horn`, `set_sentry_mode`, `auto_conditioning_start/stop`. The firmware
already treats these as role-refused ("authentication failed") and does **not** let that count
toward a pairing revocation (only the health probe / an explicit "whitelist" fault does).

## Read-only telemetry

A rotating background poll (`loop_task_fn_`, one domain per ~30 s: climate → drive → tires →
closures) refreshes per-domain caches via `set_*_state_callback` in `vehicle_telemetry.cpp`. All
polls are `NO_WAKE_SKIP` (never wake the car), feed the MQTT/HA bridge, and are **paused while
a foreground command is in flight** (`cmd_in_flight_`). Exposed under `tele` in `/status`
(`climate`/`drive`/`tires`/`closures`; emitted only while the BLE link is up — the MQTT bridge
reads the caches directly and is unaffected); numeric fields are emitted only when the car
reported them (proto3 optional). **Full field list + Overheat/Defrost chip rules:
[`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).**

## HTTP API

```
GET  /  (alias /index.html)                    # embedded web UI (gzipped into the app binary)
POST /api/1/vehicles/{VIN}/command/{command}   # execute command
GET  /api/1/vehicles/{VIN}/vehicle_data        # charge state
GET  /api/1/vehicles/{VIN}/body_controller_state
GET  /status                                   # web-UI JSON snapshot (wifi, ble, mqtt, syslog, vehicle cache, read-only telemetry under "tele"). Request/response form; the live UI reads /events instead
GET  /events                                   # WebSocket live-status push. Client sends text "sub" → immediate /status snapshot, then a fresh /status frame every ~2 s. The web UI's live feed (WS-only, no interval poll). Non-WS GET → handshake only
POST /scan                                     # start a time-limited BLE discovery scan
POST /mcp                                      # MCP server (Streamable HTTP, stateless JSON-RPC 2.0; GET → 405, no SSE).
                                               # Tools = the run-on-key charging command set + read-only get_vehicle_state
                                               # (cache-only, never wakes the car). Core logic in main/logic/mcp.hpp (host-tested).
GET  /diag                                     # plain-text in-memory diag log (?verbose=1 raw RX / ?verbose=0 off, ?clear=1 reset)
POST /gen_keys[?force=1]                       # generate key (refuses overwrite w/o force)
POST /send_key                                 # pair with vehicle (Charging Manager only)
POST /set_time                                 # set wall clock from the browser ({"ms":<epoch>}); fallback when NTP unreachable
POST /set_vin                                  # persist VIN + reboot
POST /set_mqtt                                 # persist MQTT broker (HA bridge) + reboot ({"broker":"host:port"}; "" disables)
POST /set_syslog                               # persist Syslog server + reboot ({"server":"host:port"}; "" disables)
GET  /api/proxy/1/version                      # {version, platform: running chip — "ESP32"/"ESP32-S3"/"ESP32-C3"/"ESP32-C6"/"ESP32-C5"}
GET  /ota/check[?ms=<epoch>]                   # start background manifest check (non-blocking); poll /ota/status. ms = browser-clock NTP fallback
POST /ota/update                               # start background self-update (pull, then reboot)
GET  /ota/status                               # poll OTA progress {state,progress,message,available,update_available,current}
```

No HTTP auth / TLS by design (evcc cannot send credentials) — trusted LAN only. See docs/SECURITY.md.

## OTA (self-update)

Pull-based: the device fetches `manifest.json` from `CONFIG_TESLA_OTA_MANIFEST_URL` (default
GitHub Pages), compares `version` to the running firmware, and on confirmation downloads its
per-target image `tesla-key-esp32<suffix>.bin` (`""`/`-s3`/`-c3`/`-c6`/`-c5`) via `esp_https_ota`
into the inactive slot, then reboots. `esp_https_ota` verifies the chip-id (wrong-target image
refused). **Downgrade gate:** before the bulk download `ota_task` reads the image's own version
(`esp_https_ota_get_img_desc`) and refuses anything not strictly newer than the running firmware
— a signature proves authenticity, not freshness, so this is the software anti-rollback (no
eFuses). Rollback enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`); `main.cpp` defers
`esp_ota_mark_app_valid_cancel_rollback()` to `ota_health_gate_task` — rollback stays armed until
the new image has run healthily for ≈90 s, so a boots-but-crashes-under-load image is reverted.
Implemented in `main/ota_update.cpp`.

**Images are signed** — Secure Boot v2 RSA-3072 scheme *without* hardware Secure Boot
(`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` + `..._RSA_SCHEME` +
`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` in `sdkconfig.defaults`). The running app
verifies the RSA signature before installing an OTA, so a compromised update host can't push
unsigned firmware — no eFuses burned, reversible, web installer still works. Build stays
unsigned (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`); `scripts/ci-build-all.sh` signs each
image with the offline key (CI secret `OTA_SIGNING_KEY` → gitignored `ota_signing_key.pem`).
Trust is TOFU from the running app's signature block — a device on a signed build refuses
unsigned/differently-signed OTAs. Classic esp32 needs chip rev v3.0+ (`CONFIG_ESP32_REV_MIN_3`
in `sdkconfig.defaults.esp32`). **Key lifecycle/rotation: [`docs/SECURITY.md`](../docs/SECURITY.md).**

Partition layout (`partitions.csv`) is dual-OTA (`otadata` + `ota_0`/`ota_1`, ~2 MB each),
sized to fill **4 MB** (smallest supported flash — the T-Dongle-C5's 16 MB just leaves the top
unused) so ONE table serves every target; **app at `0x20000`**. Per-target **bootloader offset**
is handled by `@flash_args` and the manifest — 0x1000 on the classic esp32, **0x2000 on esp32c5**
(its newer flash layout), 0x0 on s3/c3/c6. The `ci-build-all.sh` size gate sits at `slot − 32 KB`
(0x1e8000, below the 0x1f0000 = 2031616 B slot). **esp32c5 carries the on-device display + PSRAM**
(**the largest image** — it alone has display+PSRAM; signed ~0x1e1000, ≈28 KB under the gate) and **esp32s3 carries the display code too**
(no PSRAM), but both stay on the base **`-Og`** like every target: the Package A size levers (#154)
freed the ~64 KB the display needs, so no `-Os`
is required. (`-Os` is banned here — whole-build `-Os` hard-freezes under evcc+BLE load, rejected
Package B.) **Migration + multi-target image details:
[`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).**

## evcc Integration

```yaml
vehicles:
  - name: tesla
    type: template            # required when using template:
    template: tesla-ble       # evcc's TeslaBleHttpProxy-compatible template (this device emulates it)
    vin: <VIN>
    url: http://<ESP32-IP>    # or http://tesla-key-esp32.local
    port: 80                  # this device serves on 80 (template default is 8080)
```

## Home Assistant MQTT bridge

`main/mqtt_ha.cpp` publishes all cached telemetry + device status to MQTT using HA's
MQTT-Discovery convention. **Read-only by design** — no command topics subscribed (the car is
never controlled or woken from HA). Broker URI from NVS `mqtt_uri` (web UI: Connection → MQTT);
empty = disabled. Units are converted to metric (km, km/h) — only the `/api` evcc path keeps
miles. A schemeless broker defaults to plaintext `mqtt://`, but **defaults to `mqtts://` (TLS,
CA-bundle-verified) when credentials are present** (username configured or `user:pass@host`) so
the password isn't sniffable off-LAN; a failed TLS handshake stays disconnected with the reason
in `/status` (`mqtt.error`/`mqtt.tls`) — **no silent plaintext fallback**. **Topics, entity list,
units and publishing detail: [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).**

## Sleep / link-state

`VehicleController::link_state()` is the **single source of truth** feeding both the web-UI
hero and MQTT `sleep_state`. Four values:

- `AWAKE` — fresh live infotainment telemetry (< 60 s).
- `ASLEEP` — no live data AND debounced VCSEC sleep proven (≥ `kAsleepDebounceS` ≈ 120 s).
- `IDLE` — reachable over BLE but **not provably asleep** (web UI shows neutral "Parked").
- `UNREACHABLE` — answers nothing over BLE; web UI shows a grey **"Unreachable"** hero + orange
  ping-pong BLE bars (cold-start, nothing heard since boot ⇒ **"Connecting…"**; MQTT omitted/"unknown").

**Asymmetry:** debounced `ASLEEP` is trusted as proof of sleep; a VCSEC `AWAKE` reading is
**never** trusted to claim `AWAKE` (still requires live telemetry) — a wrong VCSEC `AWAKE` can
only leave us in `IDLE`. Touch one sink → keep the other in sync. **Full semantics +
connection-failure ("Connection failed") detection: [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).**

## Pairing lifecycle / invalidation

The web UI keys everything off `paired` (= `has_session()`). Three events clear the session +
cache (`clear_session_and_cache_()`) and force a clean re-pair: **(1)** key deleted on the car —
auto-detected three ways: the **primary** `set_message_callback` observer matching a signed-message
fault (`UNKNOWN_KEY_ID`/`INACTIVE_KEY`/`INVALID_KEY_HANDLE`, fires on a cached session), a
`"whitelist"` reply (`KEY_NOT_ON_WHITELIST`, handshake only) in `make_result_cb_`, and a two-strike
`"authentication failed"` honoured **only** for the periodic VCSEC `health_probe_` (~30 s);
**(2)** `/gen_keys?force=1`; **(3)** `/set_vin` (also forgets old `ble_mac`, reboots). **A plausible
17-char VIN gates pairing entirely** — with no VIN, `auto_pair_task` only runs a listing-only scan
and never connects/enrols. **Full detail: [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).**

## Important Notes

- Private key stored in NVS unencrypted — secure physical access to the device
- Keys are enrolled as **Charging Manager only** (charging + wake); owner role is not exposed (`?role=owner` → `403`). Sole purpose is the evcc BLE integration.
- BLE connection is on-demand; first command after idle takes ~3-5s for scan+connect
- Tesla keeps max ~3 *simultaneous* BLE connections per vehicle (shared by phone keys + fobs);
  that connection limit — not a stored-key count — is what blocks pairing when full (matches the
  `car_connectable`/`ErrMaxConnectionsExceeded` reasoning above)
- Fragment size: 20 bytes per BLE write chunk (safe for all ESP32 BLE MTU configs)
- **Memory is tight — the binding limit is the largest *contiguous* free block, not total
  free heap.** Steady-state it is only a few tens of KB (WiFi + NimBLE + MQTT dominate; see
  the boot heap-attribution log in `main.cpp`). C++ exceptions are enabled, but an *uncaught*
  `std::bad_alloc` (or any throw) unwinds through C frames → `std::terminate` → `abort()` →
  reboot. So: keep HTTP handlers under the `handle_all` try/catch (returns 503 on OOM), never
  build a whole buffer into one big `std::string` (`/diag` streams instead), and treat any new
  large *contiguous* allocation (big JSON, TLS for OTA) as a crash risk to size-check. A reboot
  loop is doubly bad: each boot re-opens the polling window, so a parked car never sleeps.

## Typical Debugging

```bash
# Serial monitor (host; no local idf.py) — exit Ctrl-A then K
screen /dev/cu.usbmodemXXXX 115200

# Test command via curl
curl -X POST http://<ESP32-IP>/api/1/vehicles/<VIN>/command/wake_up

# Check vehicle data
curl http://<ESP32-IP>/api/1/vehicles/<VIN>/vehicle_data

# Erase NVS (reset key + sessions) — host esptool
esptool --chip esp32s3 -p <port> erase_flash   # or esp32 / esp32c3 / esp32c6 / esp32c5
```
