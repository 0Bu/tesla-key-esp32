# tesla-key-esp32

ESP-IDF 5.x project for the ESP32 family. Acts as a BLE↔HTTP proxy for Tesla vehicles,
API-compatible with TeslaBleHttpProxy (works as evcc BLE vehicle integration). Builds for
the four targets yoziru/tesla-ble supports — **esp32, esp32s3, esp32c3, esp32c6** — from
ONE source tree; CI builds all four. The supported set is bounded by tesla-ble's
`idf_component.yml` `targets:` (the Component Manager refuses any other chip), not by our
code, which is target-agnostic.

> **Deep reference:** this file holds the always-needed essentials. The full narrative for
> telemetry, the MQTT/HA bridge, sleep/link-state, pairing and OTA lives in
> [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md) — read it on demand when touching those
> areas. User-facing docs: [`README.md`](../README.md), [`docs/README.md`](../docs/README.md),
> [`docs/SECURITY.md`](../docs/SECURITY.md). Keep all of these in sync (the `project-review`
> skill checks for drift).

## Environment note (Claude Code on the web / remote sandbox)

A cloud session **cannot build** (no working Docker daemon for `scripts/idf-docker.sh`) and
**cannot USB-flash** (no USB passthrough) — it is for editing, review and CI-driven builds.
The `report-capabilities.sh` SessionStart hook prints what the current environment supports.
Real builds/flashes run on a host with Docker + the board attached (see the `flash-esp32`
skill) or in CI (`.github/workflows/build.yml`).

**But there IS a real local verification loop** — the host-side mock build runs the project's
pure logic with the plain system toolchain (no ESP-IDF/Docker/board), so logic changes can be
*verified*, not just reasoned about, even in a cloud session:

```bash
scripts/run-mock-tests.sh   # compile + run host logic tests in seconds (cmake + g++/clang++)
```

It covers VIN validation, imperial→metric conversion, the `link_state()` four-state machine
(incl. the debounced-ASLEEP asymmetry) and its `/status`/MQTT strings, and the per-target
platform/OTA-suffix mapping — all delegated to IDF-free headers in `main/logic/` so the device
runs the same code the test does. CI gates the firmware build on it (`logic-test` job). Add new
hardware-free logic to `main/logic/` and a `CHECK` in `test/test_logic.cpp`. Full detail:
[`test/README.md`](../test/README.md).

## Build & Flash

No local ESP-IDF — builds run via `scripts/idf-docker.sh`, which uses the `espressif/idf`
Docker image **pinned to the version CI builds with** (read at runtime from
`.github/workflows/build.yml`, so build/debug never drifts from CI). Flash from the host with
`esptool` (`brew install esptool`), since Docker on macOS has no USB passthrough. The
`flash-esp32` skill wraps both steps.

```bash
# Build (first run: set-target; afterwards plain `build` stays incremental).
# The wrapper keeps build/ host-owned and pins the ESP-IDF version to CI.
# Pick your chip; CI builds all four via scripts/ci-build-all.sh.
scripts/idf-docker.sh idf.py set-target esp32s3 build   # or esp32 / esp32c3 / esp32c6

# Configure WiFi, VIN (interactive; can also be set later via the setup AP)
scripts/idf-docker.sh idf.py menuconfig   # → Tesla Key Configuration

# Flash from the host (preserves nvs — @flash_args skips nvs@0x9000). Match --chip to
# the target you built; @flash_args already carries the right bootloader offset.
cd build && esptool --chip esp32s3 -p <port> write_flash "@flash_args"   # or esp32 / esp32c3 / esp32c6
```

## Architecture

```
main.cpp          → WiFi init, NVS init, start all components
ble_client.cpp    → NimBLE GATT client (BleAdapter impl)
                    Scans for UUID 00000211-b2d1-43f0-9b88-960cebf8b91e
                    Write chr: 0212, Notify chr: 0213
nvs_storage.cpp   → NVS StorageAdapter (maps library keys ≤15 chars)
vehicle_ctrl.cpp  → TeslaBLE::Vehicle wrapper, sync command API via semaphores
http_server.cpp   → esp_http_server on port 80
```

## Key Dependency

`yoziru/tesla-ble` v5.1.1 — fetched via IDF Component Manager (see main/idf_component.yml).
After first `idf.py build`, the library lands in `managed_components/yoziru__tesla-ble/`.
Never edit files in `managed_components/` — they are regenerated.

## NVS Namespaces

| Namespace   | Content                                     |
|-------------|---------------------------------------------|
| `tesla_cfg` | WiFi SSID/pass, VIN, BLE MAC, `mqtt_uri`, `last_time` (runtime cfg) |
| `tesla_ble` | Private key, VCSEC session, Info session, `key_created`, `paired_at` |

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
closures) refreshes per-domain caches via `set_*_state_callback` in `vehicle_ctrl.cpp`. All
polls are `NO_WAKE_SKIP` (never wake the car), feed the MQTT/HA bridge, and are **paused while
a foreground command is in flight** (`cmd_in_flight_`). Exposed under `tele` in `/status`
(`climate`/`drive`/`tires`/`closures`); numeric fields are emitted only when the car reported
them (proto3 optional). **Full field list + Overheat/Defrost chip rules:
[`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).**

## HTTP API

```
POST /api/1/vehicles/{VIN}/command/{command}   # execute command
GET  /api/1/vehicles/{VIN}/vehicle_data        # charge state
GET  /api/1/vehicles/{VIN}/body_controller_state
GET  /status                                   # web-UI JSON (wifi, ble, mqtt, vehicle cache, read-only telemetry under "tele")
POST /scan                                     # start a time-limited BLE discovery scan
GET  /diag                                     # plain-text in-memory diag log (?verbose=1 raw RX / ?verbose=0 off, ?clear=1 reset)
POST /gen_keys[?force=1]                       # generate key (refuses overwrite w/o force)
POST /send_key                                 # pair with vehicle (Charging Manager only)
POST /set_time                                 # set wall clock from the browser ({"ms":<epoch>}); fallback when NTP unreachable
POST /set_vin                                  # persist VIN + reboot
POST /set_mqtt                                 # persist MQTT broker (HA bridge) + reboot ({"broker":"host:port"}; "" disables)
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
refused). Rollback enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`); `main.cpp` calls
`esp_ota_mark_app_valid_cancel_rollback()` after a healthy start. Implemented in
`main/ota_update.cpp`.

Partition layout (`partitions.csv`) is dual-OTA (`otadata` + `ota_0`/`ota_1`, ~2 MB each),
sized to fill **4 MB** (smallest supported flash) so ONE table serves every target; **app at
`0x20000`**. Per-target bootloader offset (0x1000 on classic esp32, 0x0 elsewhere) is handled
by `@flash_args` and the manifest. **Migration + multi-target image details:
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
miles. **Topics, entity list, units and publishing detail:
[`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).**

## Sleep / link-state

`VehicleController::link_state()` is the **single source of truth** feeding both the web-UI
hero and MQTT `sleep_state`. Four values:

- `AWAKE` — fresh live infotainment telemetry (< 60 s).
- `ASLEEP` — no live data AND debounced VCSEC sleep proven (≥ `kAsleepDebounceS` ≈ 120 s).
- `IDLE` — reachable over BLE but **not provably asleep** (web UI shows neutral "Geparkt").
- `UNREACHABLE` — answers nothing over BLE. (Nothing heard since boot ⇒ omitted/"unknown".)

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
esptool --chip esp32s3 -p <port> erase_flash   # or esp32 / esp32c3 / esp32c6
```
