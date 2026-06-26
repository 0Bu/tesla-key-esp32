# tesla-key-esp32

ESP-IDF 5.x project for the ESP32 family. Acts as a BLE↔HTTP proxy for Tesla vehicles,
API-compatible with TeslaBleHttpProxy (works as evcc BLE vehicle integration). Builds for
the four targets yoziru/tesla-ble supports — **esp32, esp32s3, esp32c3, esp32c6** — from
ONE source tree; CI builds all four. The supported set is bounded by tesla-ble's
`idf_component.yml` `targets:` (the Component Manager refuses any other chip), not by our
code, which is target-agnostic.

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
- **sleep_state** comes from `VehicleController::link_state()` — the *single* source of truth
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
  **only** when `ASLEEP` is a proven fact; for `IDLE` it shows a neutral **"Geparkt"** (parked)
  card (last-known SOC + idle time + the same wake button) that makes no sleep claim; and it hides
  the hero entirely for both `UNREACHABLE` *and* the unknown state (nothing heard since boot —
  the on-demand BLE link hasn't reached the car yet). The momentary BLE row reading
  "Disconnected" is normal (the link is dropped between polls by design) and is not used to
  drive the hero — only `link` is.
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
- **Publishing:** a dedicated `mqtt_pub` task reads the thread-safe caches; on every
  (re)connect it (re)sends discovery + `online` + an immediate snapshot, then republishes
  state every interval. The same active-window gating that lets the car sleep applies to
  the *source* polls, so MQTT keeps serving the last-known (retained) values while asleep.

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
