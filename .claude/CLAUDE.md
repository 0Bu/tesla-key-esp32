# tesla-key-esp32

ESP-IDF 5.x project for the ESP32 family. Acts as a BLE↔HTTP proxy for Tesla vehicles,
API-compatible with TeslaBleHttpProxy (works as evcc BLE vehicle integration). Builds for
**four targets — esp32, esp32s3, esp32c3, esp32c6** — from ONE source tree; CI
builds all four. That is exactly what yoziru/tesla-ble declares in its
`idf_component.yml` `targets:`, and the Component Manager ENFORCES it at dependency resolution —
which is treated as the definition of "supported": adding a chip upstream omits (esp32c5,
esp32c61) means upstreaming it there, not carrying a locally patched checkout of the crypto
library. That was done for esp32c5 for a while and dropped
([`docs/adr/0004-drop-esp32c5-target.md`](../docs/adr/0004-drop-esp32c5-target.md)).
All four targets also receive the ordered repository-owned tesla-ble patch series under
`patches/tesla-ble/`, applied fail-closed by root CMake after dependency resolution. Its first patch
fixes anti-replay behavior: upstream v5.1.1
detects duplicate CarServer response counters but otherwise processes the replay. Our patch
drops it before it can refresh a cache or complete the next FIFO command.

> **Deep reference:** this file holds the always-needed essentials. The full narrative for
> telemetry, the MQTT/HA bridge, WiFi/LAN reconnect, sleep/link-state, pairing, OTA and the
> concurrency contract (lock hierarchy + task inventory; priorities in `main/task_config.hpp`)
> lives in [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md) — read it on demand when touching
> those areas. User-facing docs: [`README.md`](../README.md), [`docs/README.md`](../docs/README.md),
> [`docs/SECURITY.md`](../docs/SECURITY.md), [`docs/MCP.md`](../docs/MCP.md) (MCP integration
> guide). A cross-cutting catalog of the PLATFORM features this firmware implements — crash
> forensics, the watchdog ladder, signed OTA, config storage, the diagnostic surfaces — is
> [`docs/FEATURES.md`](../docs/FEATURES.md); keep it current with the `feature-docs` skill when a
> technical feature lands or changes (a merge is gated on it whenever the diff reaches `main/`,
> `test/`, `sdkconfig.defaults*`, `partitions.csv` or the CI build workflow).
> Keep all of these in sync (the `project-review` skill checks for drift).

## Environment note (Claude Code on the web / remote sandbox)

A cloud session **cannot build** (no working Docker daemon for `scripts/idf-docker.sh`) and
**cannot USB-flash** (no USB passthrough) — it is for editing, review and CI-driven builds.
The `report-capabilities.sh` SessionStart hook prints what the current environment supports.
Real builds/flashes run on a host with Docker + the board attached (see the `flash-esp32`
skill) or in CI (`.github/workflows/build.yml`). The `build-efficiency-check.sh` SessionStart
hook audits the latest post-merge `build` run on main for efficiency regressions (ccache hit
rate, cache hygiene, build-duration/total-run regression, binary-size headroom) and, on a
problem, has the session open an Issue / draft Fix-PR — deduped per run id, never auto-commits.
Project MCP tooling is an external trust boundary: `.mcp.json` pins Context7 to an exact version,
but never send it secrets, NVS dumps or unredacted vehicle diagnostics; firmware/CI do not need it.

**But there IS a real local verification loop** — the host-side mock build runs the project's
pure logic with the plain system toolchain (no ESP-IDF/Docker/board), so logic changes can be
*verified*, not just reasoned about, even in a cloud session:

```bash
scripts/run-mock-tests.sh   # compile + run host logic tests in seconds (cmake + g++/clang++)
```

It covers VIN validation, imperial→metric conversion, the `link_state()` four-state machine
(incl. the debounced-ASLEEP asymmetry) and its `/status`/MQTT strings, the per-target
platform/OTA-suffix mapping, the VIN-stable Home Assistant node identifier with its board-MAC
fallback (`logic/ha_identity.hpp`), the MCP protocol core (version negotiation, method routing,
int clamp), the ONE command registry both command surfaces dispatch through
(`logic/command_registry.hpp` — REST + MCP names, kinds, shared arg bounds and the
command-specific evcc boolean-body compatibility rule), the
`/status` field contract (`logic/status_model.hpp`, golden-emission-pinned — order, key
names, presence rules, value shaping), the shared command-outcome text, the on-device display
presenter (the priority ladder / SoC gradient / RSSI→bars / SSID-scroll decisions the ST7735
renderer draws), the status-LED ladder (`logic/led_status.hpp`, reading the same shared
`UiSnapshot` + the shared SoC gradient), the active-window poll gate
(`logic/active_window.hpp` — charging held open only on fresh contact), the BLE phase countdown
(`logic/ble_phase.hpp` — which phase the Bluetooth row counts down, rounded up and never
vanishing on its last second) and the web UI's Bluetooth-row presenter (`logic/ble_row.hpp` —
which row state and which countdown belongs beside it, mirrored by app.js under a CI parity
check), the HA binary
`value_template` builder (`logic/ha_templates.hpp` — presence-aware `is defined` guard) and the
POST-body reassembly loop (`logic/http_body.hpp` — multi-segment recv + bounded timeout) and the
charging-current ACK/readback plus active-cache freshness rules
(`logic/charge_control.hpp` — a Tesla action ACK is not success until an independent,
fresh ChargeState reports the requested amps; active data older than 30 s is rejected) and the
heap-exhaustion watchdog (`logic/heap_watchdog.hpp` — 4 KB/5 min unbroken hold, OTA-excused,
restart cap + `heap:<n>` breadcrumb round-trip) and the BLE connect-failure classifier
(`logic/connect_outcome.hpp` — scanner verdict → out-of-range / at-BLE-limit / connect-failed,
its log level and the background rate limit; foreground attempts are never suppressed) and the
OTA rollback health gate (`logic/health_gate.hpp` — commit only on a proven link plus an uptime
floor, the setup-mode exemption, give up past the cap) and the MQTT broker contract
(`logic/mqtt_uri.hpp` — the ONE credential-aware scheme rule both the bridge and /set_mqtt's
save-time probe dial, plus that probe's contiguous-heap budget and status mapping) and the
retained memory trend (`logic/heap_history.hpp` — CRC + layout fingerprint before a .noinit ring
is adopted, and the carry that keeps one bucket clock across the restart) — all
delegated to IDF-free headers in `main/logic/` so the device runs the same code the test does. CI gates the firmware build on it (`logic-test` job). Add new
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
# Pick your chip; CI builds all four via scripts/ci-build-all.sh.
# Every patches/tesla-ble/*.patch is applied lexically and idempotently by CMake for every target.
scripts/idf-docker.sh idf.py set-target esp32s3 build   # or esp32 / esp32c3 / esp32c6

# Configure WiFi, VIN (interactive; can also be set later via the setup AP)
scripts/idf-docker.sh idf.py menuconfig   # → Tesla Key Configuration

# Flash from the host (preserves nvs — @flash_args skips nvs@0x9000). Match --chip to
# the target you built; @flash_args already carries the right bootloader offset.
cd build && esptool --chip esp32s3 -p <port> write_flash "@flash_args"   # or esp32 / esp32c3 / esp32c6
```

## Architecture

```
main.cpp               → boot orchestration: NVS init, config/VIN resolve, clock restore,
                         BLE + network bring-up order, start all components
net.cpp / net.hpp      → the ONE network-transport seam. Everything above it (HTTP, MQTT,
                         syslog, mDNS, SNTP, OTA, display, LED) asks tk::net_is_up() /
                         net_kind() / net_active_netif() and never touches esp_wifi. Owns the
                         WiFi station, the endless-reconnect handler, the credential-rollback
                         boot window and the gateway-ICMP ghost-link watchdog (net_wd task).
                         The transport identity (tk::NetLink::{None,Wifi,Eth}) and the
                         watchdog's decision — incl. the rule that a gateway which has NEVER
                         answered ICMP must not trigger recovery — are the host-tested
                         logic/net_link.hpp. Also carries the OPTIONAL W5500 SPI Ethernet
                         backend (CONFIG_TESLA_ETH_ENABLED, esp32s3 only): probes the
                         controller's VERSIONR once at boot, runs it in POLLING mode (the
                         ATOMIC PoE Base routes no INT and no RST line) and, on a lease,
                         never starts WiFi at all — no BLE radio coexistence, ~57 KB of
                         largest-block unspent
board.cpp / board.hpp  → runtime board identification for the ONE image per chip. The esp32s3
                         image serves THREE boards: T-Dongle-S3 (ST7735), a bare ESP32-S3, and
                         an M5Stack AtomS3 Lite on an ATOMIC PoE Base (W5500). ONE detector,
                         because display and Ethernet OVERLAP ON A PIN — the panel's SPI clock
                         is GPIO5, the same pin the PoE base uses for SCLK — so the W5500 probe
                         is refused on a detected T-Dongle
patches/tesla-ble/     → reviewed patch on pinned dependency: reject replayed CarServer
                         responses before callbacks/FIFO completion (all four targets)
ble_client.cpp         → NimBLE GATT client (BleAdapter impl)
                         Scans for UUID 00000211-b2d1-43f0-9b88-960cebf8b91e
                         Write chr: 0212, Notify chr: 0213
nvs_storage.cpp        → NVS StorageAdapter (maps library keys ≤15 chars)
vehicle_ctrl.cpp       → VehicleController core: init/wiring, VIN gate, link_state() glue
vehicle_commands.cpp   → sync command API via semaphores (send_vcsec_/send_infotainment_,
                         make_result_cb_, all user commands). ensure_connected_ names WHY a
                         connect failed from the scanner's own verdict and rate-limits the
                         unattended repeats (logic/connect_outcome.hpp, host-tested) —
                         foreground attempts are never suppressed. set_charging_amps keeps its
                         Tesla action + fresh exact ChargeState readback in one serialized
                         command transaction
vehicle_telemetry.cpp  → protobuf parsers, cache callbacks, loop_task (background poll +
                         sleep gating), data queries
vehicle_pairing.cpp    → auto_pair_task, key mgmt/fingerprint, session invalidation,
                         health probe   (split map: vehicle_ctrl_internal.hpp)
http_server.cpp        → esp_http_server on port 80: wildcard dispatch + the handle_all
                         try/catch OOM guard (503) EVERY handler runs under
http_api.cpp           → evcc routes (/api/1/…, /api/proxy/1/version); command names/args
                         resolve via logic/command_registry.hpp (ONE table with the MCP
                         tools; also owns the charge_start=true / charge_stop=false evcc
                         scalar-body exception), execution via command_exec.cpp
command_exec.cpp       → the ONE CmdKind → VehicleController dispatch both command
                         surfaces (/api and /mcp) execute through
http_status.cpp        → web UI (/), /status, /diag, /scan; the /status field contract
                         is decided in logic/status_model.hpp (host-tested, golden-pinned)
                         — build_status_object() only gathers inputs + serializes via cJSON.
                         /status is the web UI's live feed: app.js polls it every 4 s
                         (request/response — the device queues nothing per client, which is
                         deliberate; the earlier WebSocket push wedged the device on
                         2026-07-18 when a subscriber stopped reading)
http_ota.cpp           → /ota/check|update|status
http_config.cpp        → /gen_keys, /send_key, /set_time, /set_vin, /set_mqtt, /set_syslog.
                         /set_mqtt is TEST-BEFORE-PERSIST: a changed broker is dialled (same URI
                         logic/mqtt_uri.hpp gives mqtt_ha) before the NVS write, and the probe
                         refuses itself when the largest CONTIGUOUS internal block cannot afford a
                         second mbedTLS session — a 503 costs a retry, attempting it costs a
                         bad_alloc on the httpd task
mcp_server.cpp         → /mcp — MCP server for AI agents (stateless JSON-RPC 2.0;
                         core logic in logic/mcp.hpp, guide in docs/MCP.md)
                         (shared helpers: http_common.cpp; split map: http_handlers.hpp)
diag_crash.cpp         → ONE-SHOT boot capture of why the last run ended: the reset reason
                         (always — needs no partition, so it works on already-deployed devices)
                         plus, where the `coredump` partition exists, the dump SUMMARY (crashed
                         task / PC / backtrace / app-elf-sha, esp_core_dump_get_summary). Parsed
                         ONCE at boot, never on a request path. An ORPHAN dump — one whose
                         app-elf-sha does not match the running build, which the coredump partition
                         happily keeps across an OTA — is ERASED, so `coredump` means "a dump for
                         THIS firmware is downloadable" rather than "the partition is non-blank";
                         declared foreign only on PROOF (logic/crashinfo.hpp), since the erase
                         destroys the one artifact a panic left. Feeds /status.last_crash, the MQTT
                         diagnostics and the syslog boot replay
safe_mode.cpp          → boot-loop safe mode (logic/boot_guard.hpp): counts CRASH-ONLY boots in NVS
                         (`boot_fails`); past kBootFailThreshold it latches → main.cpp starts WiFi +
                         web UI + OTA ONLY and skips the BLE/vehicle stack and the MQTT bridge, so a
                         board that crashes on the vehicle path stays fixable in a browser instead of
                         needing a USB cable. The heap watchdog's own cap counts only restarts WE
                         chose; a PANIC loop was entirely uncounted before this. Sharper here than
                         elsewhere: every boot re-opens the car's polling window, so a reboot loop
                         drains a parked traction battery. A clean/intentional reboot resets the
                         count, and a NON-safe-mode boot that stays up kBootHealthyS clears it — the
                         healthy timer is deliberately NOT armed while safe mode is latched, since
                         surviving the window with the crashing subsystems switched off says nothing
                         about the fault (arming it there would give a 4-crashes-then-one-quiet-boot
                         cycle, not a latch). Drives /status.sys.safe_mode
heap_trend.cpp         → storage + mutex for the board's own 24-hour memory trend (GET /heap), fed
                         from the SAME two samples loop_task hands the heap watchdog, so the chart a
                         human reads and the threshold the firmware acts on cannot disagree. Fixed
                         ring (~1.2 KB), never heap — a diagnostic must not compete for the
                         largest CONTIGUOUS block it exists to measure. Answers the one question a
                         spot value cannot: is the heap DRIFTING (a leak is a slope; fragmentation is
                         the two lines separating). It lives in **.noinit**, not .bss, so it SURVIVES
                         A RESTART: the heap watchdog's answer to exhaustion IS a restart, so a .bss
                         ring was erased by the one event it exists to explain (same for a panic, the
                         task watchdog and an OTA reboot). Still zero flash writes — NVS persistence
                         stays rejected. The retained image must pass a CRC (config_crc32, so a
                         power-on's SRAM noise is never adopted) and a derived layout fingerprint (an
                         OTA can change the ring's geometry while the bytes stay valid); on any
                         failure the trend starts empty. A carry offset keeps ONE continuous bucket
                         clock across the reboot, which therefore always lands on a bucket boundary
                         — /heap's `b_boot` names it. Mechanics in logic/heap_history.hpp
config_blob.cpp        → the ONE atomic credential/service entry in NVS (logic/config_store.hpp):
                         WiFi creds + the one-shot rollback backup + VIN + mqtt_uri + syslog_uri as
                         a single CRC-checked nvs_set_blob, all-or-nothing across BOTH a write
                         failure and a power cut. Replaces per-key writes whose tear was reportable
                         but not undoable (the setup portal wrote ssid/pass/vin as three commits).
                         READS the legacy per-key layout as a fallback when the blob is absent or
                         fails its CRC — without that, this change would strand every deployed
                         device's WiFi and VIN on the first OTA — and mirrors back to those keys on
                         save so a DOWNGRADE still finds its config. Deliberately excludes ble_mac /
                         last_time / reboot_why / disp_rot: different writers, and a whole-struct
                         writer reverts another owner's field from a stale snapshot
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
                         probe. Errno-based hard/transient send-failure split AND the RFC 5424
                         PRI (facility user, severity from each line's own esp_log level —
                         E/W/I/D → 3/4/6/7, colour-escape tolerant, unprefixed library lines
                         stay info) in logic/syslog_policy.hpp (host-tested). Lines logged
                         BEFORE syslog_start() cannot be forwarded (the queue does not exist
                         yet) — main.cpp samples the boot values early but emits the `BOOT
                         reset_reason=` line as the first line past syslog_start
provisioning.cpp       → captive setup portal (setup AP) when no WiFi is configured
display.cpp            → on-device ST7735 status panel (LilyGo T-Dongle-S3),
                         LANDSCAPE 160x80 (header WiFi bars+SSID | BT+BLE bars + a horizontal SoC
                         battery) OR PORTRAIT 80x160 (two-row header over a VERTICAL battery filling
                         bottom→top); both draw a red→green gradient / charging bolt / "ASLEEP", or a
                         WiFi/BLE search + "Pairing…" animation. Cache-only (never wakes the car).
                         "What to show" (priority ladder / gradient / bars / SSID scroll) is decided
                         by the pure, host-tested presenter logic/display_model.hpp (an Orient axis
                         picks the SSID geometry) reading the shared logic/ui_state.hpp
                         (VehicleController::ui_snapshot()); this file is the thin renderer
                         (draw_landscape / draw_portrait). Each BOOT tap (S3 IO0) rotates
                         90° through the 4 orientations — landscape/portrait ± their 180° flips
                         (MADCTL {0xC8,0xA8,0x08,0x68}, same framebuffer, offsets swap 1/26↔26/1);
                         the index persists in NVS tesla_cfg/disp_rot (migrates the old disp_flip).
                         Backlight active-LOW; SPI 40 MHz; framebuffer in PSRAM where present, else
                         ~25 KB internal SRAM. Compiles to a no-op unless
                         CONFIG_TESLA_DISPLAY_ENABLED (sdkconfig.defaults.esp32s3); the ONE
                         esp32s3 image auto-detects the T-Dongle-S3 (SD pull-ups) so a generic
                         ESP32-S3 stays panel-less. Font from tools/display_sim.py → main/display_font.h
led_status.cpp         → on-device status LED: the single underside APA102 pixel (T-Dongle-S3)
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
| `tesla_cfg` | `cfg` — the **atomic credential/service blob** (`logic/config_store.hpp`, `main/config_blob.cpp`): WiFi SSID/pass + the **one-shot rollback backup** (previous SSID/pass + the `rolled_back` outcome marker), VIN, `mqtt_uri`, `syslog_uri` — ONE CRC-checked `nvs_set_blob`, so a credential save is all-or-nothing across a write failure AND a power cut. The legacy per-key names (`wifi_ssid`/`wifi_pass`/`vin`/…) are still READ as the fallback when the blob is absent (a device that has not saved since upgrading) or fails its CRC, and are mirrored on save so a downgrade still finds its config. Plus the boot-loop **crash counter** `boot_fails` (safe_mode.cpp) and the separate-owner keys: BLE MAC, `last_time`, `reboot_why` (why WE ended the last boot — `heap:<n>` = the heap watchdog, n = consecutive such restarts; read+cleared at boot, surfaced once as `/status.last_reboot`), `disp_rot` (on-device display BOOT-rotation index 0..3; T-Dongle-S3; migrates old `disp_flip`) (runtime cfg) |
| `tesla_ble` | Private key (`private_key`), VCSEC session (`sess_vcsec`), Info session (`sess_info`), `key_created`, `paired_at` — the `sess_*` names come from the ≤15-char key mapping in `nvs_storage.cpp`. The `sess_*` blobs are only REUSABLE across a reboot if the wall clock is restored **before** `VehicleController::init()` (tesla-ble rejects a session older than 1 h and computes the age against `time(nullptr)`, which underflows at 1970) — hence `restore_clock_from_nvs()` early in `main.cpp`, see docs/ARCHITECTURE.md |

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
GET  /status                                   # web-UI JSON snapshot (wifi, ble, mqtt, syslog, vehicle cache, read-only telemetry under "tele"). The web UI's live feed — app.js polls it every 4 s (cache-busted, no-store).
                                               # ALWAYS carries sys{board_mac,free_heap,min_free_heap,largest_block,uptime_s,wifi_reconnects,reset_reason,safe_mode} — board_mac identifies the physical controller while the other fields expose its runtime health.
                                               # last_crash{reason,reason_code,fault,coredump,task,pc,backtrace[],corrupted,elf_sha256} appears ONLY when the boot is notable (a fault reset, or a dump for this build still in flash, not dismissed) — presence is the signal.
GET  /status?redact=1                          # the BUG-REPORT form: the reporter-identifying vehicle/network values (vin, ip, wifi.ssid, ble.addr + every scanned neighbour's, mqtt.broker, syslog.host) read "<redacted>". sys.board_mac deliberately remains visible so the physical controller can be diagnosed. The KEY is always kept — an omitted field forges an "older build" signal. Opt-in per request; the dashboard polls the unredacted payload
POST /scan                                     # start a time-limited BLE discovery scan
POST /mcp                                      # MCP server (Streamable HTTP, stateless JSON-RPC 2.0; GET → 405, no SSE).
                                               # Tools = the run-on-key charging command set + read-only get_vehicle_state
                                               # (cache-only, never wakes the car). Core logic in main/logic/mcp.hpp (host-tested).
GET  /diag                                     # plain-text in-memory diag log (?verbose=1 raw RX / ?verbose=0 off, ?clear=1 reset, ?redact=1 bug-report form)
GET  /coredump[?clear=1]                       # stream the raw crash image (chunked octet-stream; 404 if none). Decode offline against the matching-version .elf; ?clear=1 erases the partition
POST /crash/dismiss                            # acknowledge + DELETE this boot's crash report (erase first, mark second). POST, not GET: it destroys the one artifact a bug report needs
                                               # bug report needs. An erase that finds NO coredump partition (every OTA-upgraded device) is NOT a failure — the dismissal still clears the fault-reset report; any other erase error is a 500 (logic/crashinfo.hpp crash_erase_permits_dismiss).
GET  /heap                                     # the board's 24-hour free/largest-block trend {dt,b0,b_boot,unit,scale,free[],largest[]} — tenths of a KiB, null = no sample.
                                               # The ring is .noinit and SURVIVES a restart (watchdog/panic/OTA), so b_boot names the bucket THIS boot started in, so any sample before it came from an earlier run — and the clock is no longer uptime/dt.
POST /set_wifi                                 # change WiFi credentials over the LAN + reboot ({"ssid","pass"}); stashes the previous pair as a one-shot rollback backup
POST /gen_keys[?force=1]                       # generate key (refuses overwrite w/o force)
POST /send_key                                 # pair with vehicle (Charging Manager only)
POST /set_time                                 # set wall clock from the browser ({"ms":<epoch>}); fallback when NTP unreachable
POST /set_vin                                  # persist VIN + reboot
POST /set_mqtt                                 # verify, then persist MQTT broker (HA bridge) + reboot ({"broker":"host:port"}; "" disables).
                                               # TEST BEFORE PERSIST: a CHANGED non-empty broker is CONNECTED to first (the same URI mqtt_ha will dial, logic/mqtt_uri.hpp), and only a broker that accepts the CONNECT is saved. 400 = refused/bad credentials, 502 = unreachable or no answer, 503 = too little contiguous heap to afford the probe (nothing saved, retry). An unchanged value is a no-op and is never probed.
POST /set_syslog                               # persist Syslog server + reboot ({"server":"host:port"}; "" disables)
GET  /api/proxy/1/version                      # {version, platform: running chip — "ESP32"/"ESP32-S3"/"ESP32-C3"/"ESP32-C6"}
GET  /ota/check[?ms=<epoch>]                   # start background manifest check (non-blocking); poll /ota/status. ms = browser-clock NTP fallback
POST /ota/update                               # start background self-update (pull, then reboot)
GET  /ota/status                               # poll OTA progress {state,progress,message,available,update_available,current}
```

No HTTP auth / TLS by design (evcc cannot send credentials) — trusted LAN only. See docs/SECURITY.md.

## OTA (self-update)

Pull-based: the device fetches `manifest.json` from `CONFIG_TESLA_OTA_MANIFEST_URL` (default
GitHub Pages), compares `version` to the running firmware, and on confirmation downloads its
per-target image `tesla-key-esp32<suffix>.bin` (`""`/`-s3`/`-c3`/`-c6`) via `esp_https_ota`
into the inactive slot, then reboots. `esp_https_ota` verifies the chip-id (wrong-target image
refused). **Downgrade gate:** before the bulk download `ota_task` reads the image's own version
(`esp_https_ota_get_img_desc`) and refuses anything not strictly newer than the running firmware
— a signature proves authenticity, not freshness, so this is the software anti-rollback (no
eFuses). Rollback enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`); `main.cpp` defers
`esp_ota_mark_app_valid_cancel_rollback()` to `ota_health_gate_task`, whose verdict is the
host-tested `logic/health_gate.hpp`. Health is **connectivity plus a floor, not uptime**: the image
must hold a lease (`tk::net_is_up()`, either transport) *and* have run ≥90 s, so both a
boots-but-crashes-under-load image AND a boots-but-never-gets-online image are reverted — the second
is the one no OTA can fix afterwards, and a pure 90 s timer committed it. Past a 600 s cap an
unhealthy image is LEFT pending (no self-restart: that would turn a long router outage into a silent
downgrade); the next reboot rolls it back, and any deliberate `/set_*` save commits it instead via
`ota_confirm_pending_image()`. A device with neither credentials nor a wire is legitimately offline
(setup mode) and counts as healthy. Implemented in `main/ota_update.cpp`.

**Images are signed** — Secure Boot v2 RSA-3072 scheme *without* hardware Secure Boot
(`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` + `..._RSA_SCHEME` +
`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` in `sdkconfig.defaults`). The running app
verifies the RSA signature before installing an OTA, so a compromised update host can't push
unsigned firmware — no eFuses burned, reversible, web installer still works. Build stays
unsigned (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`); the unprivileged
`scripts/ci-build-all.sh` produces only data, and trusted `scripts/ci-sign-artifacts.sh` signs it
with the protected key (CI secret `OTA_SIGNING_KEY` → transient gitignored
`ota_signing_key.pem`). PR CI exercises that signer with a disposable RSA-3072 key, never the real
one. Signed PR previews are opt-in via `signed-preview` plus Environment approval, with a final
state/SHA/label check immediately before publish and event+scheduled cleanup under the same lock.
Trust is TOFU from the running app's signature block — a device on a signed build refuses
unsigned/differently-signed OTAs. Classic esp32 needs chip rev v3.0+ (`CONFIG_ESP32_REV_MIN_3`
in `sdkconfig.defaults.esp32`). **Key lifecycle/rotation: [`docs/SECURITY.md`](../docs/SECURITY.md).**

Partition layout (`partitions.csv`) is dual-OTA (`otadata` + `ota_0`/`ota_1`, ~2 MB each),
sized to fill **4 MB** (smallest supported flash; a larger one just leaves the top
unused) so ONE table serves every target; **app at `0x20000`**. Per-target **bootloader offset**
is handled by `@flash_args` and the manifest — 0x1000 on the classic esp32, 0x0 on s3/c3/c6.
The `ci-build-all.sh` size gate sits at `slot − 32 KB`
(0x1e8000, below the 0x1f0000 = 2031616 B slot); it is checked on the **signed** image, whose code
is first padded up to a 64 KB Secure-Boot boundary before a 4 KB signature sector is appended — so
crossing a boundary costs a full 64 KB block, which is what made esp32c5 undeliverable
([ADR-0004](../docs/adr/0004-drop-esp32c5-target.md)). **esp32c6 is the largest image**
(signed 0x1e1000, ≈28 KB under the gate — it sat ~3 KB under a 64 KB Secure-Boot boundary and a 3 KB change crossed it); **esp32s3 carries the display code** and sits at
0x191000. All stay on the base **`-Og`**: the Package A size levers (#154)
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

The Tesla-compatible response shape remains
`.response.response.charge_state.charge_amps`. `vehicle_data` is cache-only and never blocks:
idle/asleep values remain readable so polling does not wake the car, while charging or a command
in the last five minutes requires a ChargeState no older than 30 s (otherwise HTTP 503).
`set_charging_amps` requires an integer `charging_amps` body (HTTP 400 otherwise), then treats
Tesla's action ACK as provisional: a second serialized ChargeState response must report the exact
requested value. Rejects, timeouts, missing readback and mismatches return HTTP 502 so evcc retries
instead of accepting false success. Every command failure keeps its JSON reason but uses HTTP 502
rather than transport-level success.

## Home Assistant MQTT bridge

`main/mqtt_ha.cpp` publishes all cached telemetry + device status to MQTT using HA's
MQTT-Discovery convention. **Read-only by design** — no command topics subscribed (the car is
never controlled or woken from HA). Broker URI from NVS `mqtt_uri` (web UI: Connections → MQTT);
empty = disabled. The lowercased validated VIN is the MQTT node, discovery `unique_id` prefix and
HA device identifier, so replacing the ESP32 preserves the same entities while the vehicle is
unchanged. Only a missing/invalid VIN falls back to the board MAC until provisioning is complete.
Units are converted to metric (km, km/h) — only the `/api` evcc path keeps
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
- `UNREACHABLE` — answers nothing over BLE; the web UI **hides the hero card** (as it does for the
  cold-start unknown state) and signals it on the BLE row: orange ping-pong bars + orange MAC
  (MQTT omitted/"unknown").

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
- **Those guards all mean "recover and continue" — which is right for a TRANSIENT shortage and
  hangs the device on a permanent one.** A wedge (2026-07-18: `bad_alloc` out of `loop()` every
  50 ms for ten hours, HTTP/MQTT/BLE all dead, no reboot) is worse than a crash, because a crash
  at least restarts. The one escalation is `logic/heap_watchdog.hpp`, sampled by `loop_task`:
  **INTERNAL** `largest_block` (`8BIT|INTERNAL` — plain `8BIT` would include any PSRAM and
  make it a silent no-op there) under **4 KB continuously for 5 min** (never a single
  `bad_alloc`; excused while an OTA holds its TLS buffers) ⇒ log loudly, persist
  `reboot_why=heap:<n>` to NVS, restart. Capped at **5 consecutive** restarts, and such a boot
  does NOT seed the active polling window (else a loop keeps a parked car awake). The next boot
  reports it once in `/status` as `last_reboot`. The escalation **narrates itself to syslog**
  (armed → per-sample countdown → recovered/excused/fired → the `BOOT` line on the way back up),
  because syslog is the only post-mortem source that survives the restart. **Why a restart and
  not in-place recovery — the researched rejection of subsystem deinit/reinit, ballast blocks,
  the failed-alloc hook and defragmentation, with sources: [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).**

## Crash forensics + the recovery ladder

Four mechanisms, deliberately separate, because each is blind to what the others catch. Full
catalog: [`docs/FEATURES.md`](../docs/FEATURES.md).

- **Heap watchdog** (`logic/heap_watchdog.hpp`) — internal `largest_block` under 4 KB for 5 unbroken
  minutes ⇒ deliberate restart, `reboot_why=heap:<n>`, capped at 5. Catches the WEDGE.
- **Task watchdog** (60 s, `PANIC=y`; `vehicle_loop` + `mqtt_pub` subscribed) — catches a task
  blocked forever on a semaphore, the BLE stack or a socket, with the heap looking perfectly
  healthy. The budget is sized against the LONGEST legitimate block, which is not the 50 ms loop
  cadence but the vehicle mutex (20 s for a command, 30 s for `pair()`).
- **Safe mode** (`logic/boot_guard.hpp`, `safe_mode.cpp`) — 4 consecutive CRASH boots ⇒ WiFi + web UI
  + OTA only. The heap watchdog's cap counts only restarts WE chose; a panic loop was uncounted
  before this, and on this device every boot re-opens the car's polling window. It LATCHES: only a
  non-fault reset (an OTA install, a `/set_*` save, a power-cycle) clears the counter, so it ends
  when someone acts on it.
- **Crash capture** (`diag_crash.cpp`, `logic/crashinfo.hpp`) — reset reason always; the core-dump
  summary where the partition exists. Surfaced on `/status.last_crash`, over MQTT, and replayed to
  syslog once per boot (`logic/bootlog.hpp`) because `/diag` is RAM and does not survive the reboot
  it would explain. The BACKTRACE is **Xtensa-only** (esp32/esp32s3): IDF declares
  `esp_core_dump_bt_info_t` per ARCHITECTURE, and on RISC-V (c3/c6) it is a raw stack dump rather
  than an unwound PC array — so there `last_crash` carries reason/task/PC/elf_sha and leaves the
  unwinding to the offline decoder reading `GET /coredump`. Two of four targets are RISC-V, so this
  is half the fleet, not an edge case.

**The coredump partition does NOT reach already-deployed devices** — a partition table is not part of
an OTA image. Those boards keep reporting the reset REASON (which needs no partition) and simply
report `coredump:false`; only a USB/web-installer full flash adds it. That is a supported state.

## Typical Debugging

```bash
# Serial monitor (host; no local idf.py) — exit Ctrl-A then K
screen /dev/cu.usbmodemXXXX 115200

# Test command via curl
curl -X POST http://<ESP32-IP>/api/1/vehicles/<VIN>/command/wake_up

# Check vehicle data
curl http://<ESP32-IP>/api/1/vehicles/<VIN>/vehicle_data

# Pull a crash dump (if any) + decode it offline against the matching-version .elf
curl http://<ESP32-IP>/coredump -o coredump.bin

# The board's own 24-hour memory trend — is the heap DRIFTING? (a leak is a slope;
# fragmentation is free[] holding steady while largest[] sinks toward the 4 KB floor)
curl http://<ESP32-IP>/heap | jq

# A shareable bug report: vehicle/network identifiers are redacted; sys.board_mac stays diagnostic
curl "http://<ESP32-IP>/status?redact=1" | jq
curl "http://<ESP32-IP>/diag?redact=1"

# Erase NVS (reset key + sessions) — host esptool
esptool --chip esp32s3 -p <port> erase_flash   # or esp32 / esp32c3 / esp32c6
```
