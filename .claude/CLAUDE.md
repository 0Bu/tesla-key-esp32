# tesla-key-esp32

ESP-IDF 5.x project for ESP32-S3. Acts as a BLE↔HTTP proxy for Tesla vehicles,
API-compatible with TeslaBleHttpProxy (works as evcc BLE vehicle integration).

## Build & Flash

```bash
# Prerequisites: ESP-IDF 5.0.1+ sourced
idf.py set-target esp32s3

# Configure WiFi, VIN (required before first build)
idf.py menuconfig   # → Tesla Key Configuration

# Build + Flash + Monitor
idf.py build flash monitor -p /dev/tty.usbserial-*
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

All commands: `charge_start`, `charge_stop`, `set_charging_amps`, `set_charge_limit`,
`wake_up`, `charge_port_door_open/close`, `door_lock/unlock` (sent but rejected by the car
for the Charging-Manager role — present for API completeness), `flash_lights`,
`honk_horn`, `set_sentry_mode`, `auto_conditioning_start/stop`,
`set_scheduled_charging` (`{"enable":bool,"start_minutes":int}` — minutes after local
midnight; daily charge start time. Scheduled *departure* is not exposed: the tesla-ble
version in use registers no builder for `scheduledDepartureAction`).

## Read-only telemetry

A rotating background poll in `loop_task_fn_` (one domain per ~12 s cycle: climate →
drive → tires → closures, full set ~48 s) refreshes per-domain caches via the
`set_*_state_callback` hooks in `vehicle_ctrl.cpp`. All polls are `NO_WAKE_SKIP`
(read-only, never wake the car) and feed the MQTT/HA bridge — evcc/pairing are unaffected.
Exposed under `tele` in `/status` (for the HA bridge and diagnostics; the device's own web
UI is charge/SOC-only and does not render these): `climate` (inside/outside/setpoint °C, on,
preconditioning), `drive` (shift, odometer_km), `tires` (fl/fr/rl/rr bar + warn),
`closures` (locked, door/frunk/trunk/window open, occupant). Numeric fields are emitted
only when the car reported them (proto3 optional) so Home Assistant shows "—"/unknown
otherwise.

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
GET  /api/proxy/1/version                      # {version, platform:"ESP32-S3"}
GET  /ota/check[?ms=<epoch>]                   # start background manifest check (non-blocking); poll /ota/status. ms = browser-clock NTP fallback
POST /ota/update                               # start background self-update (pull, then reboot)
GET  /ota/status                               # poll OTA progress {state,progress,message,available,update_available,current}
```

No HTTP auth / TLS by design (evcc cannot send credentials) — trusted LAN only. See docs/SECURITY.md.

## OTA (self-update)

Pull-based: the device fetches `manifest.json` from a fixed HTTPS URL
(`CONFIG_TESLA_OTA_MANIFEST_URL`, default GitHub Pages), compares its `version` to the
running firmware, and on confirmation downloads the app image
(`CONFIG_TESLA_OTA_FIRMWARE_URL`) via `esp_https_ota` into the inactive OTA slot, then
reboots. Triggered from the web UI by tapping the firmware version in the top meta line.
Implemented in `main/ota_update.cpp`. Rollback is enabled
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`); `main.cpp` calls
`esp_ota_mark_app_valid_cancel_rollback()` after a healthy startup.

Partition layout (`partitions.csv`) is dual-OTA (`otadata` + `ota_0`/`ota_1`, 3 MB each;
app now at `0x20000`). **Migration:** a device on the old single-`factory` layout must be
USB-reflashed once via the web installer (full erase → WiFi/VIN/key reset, re-pair). After
that, all updates are OTA and preserve NVS.

## evcc Integration

```yaml
vehicles:
  - name: Tesla
    type: tesla-ble-http
    url: http://<ESP32-IP>
    vin: <VIN>
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
  charging_state), climate (inside/outside/setpoint °C, on, preconditioning), drive (shift,
  odometer km), tires (fl/fr/rl/rr bar + warn), closures (locked/door/frunk/trunk/window/
  occupant), sleep_state, and device diagnostics (wifi/ble RSSI, ble_link, paired, **last
  boot** (boot-time timestamp), free_heap, firmware). Numeric fields are emitted only when
  the car reported them (proto3 optional), so an unseen value reads "unknown" in HA rather
  than a phantom 0. **Units:** Tesla reports range/rate/odometer imperial; the MQTT bridge
  converts to metric (km, km/h) — only the Tesla-compatible `/api` path keeps miles (evcc).
- **sleep_state** comes from `VehicleController::link_state()` — the *single* source of truth
  shared with the web UI so the two never drift — NOT the raw cached VCSEC string (which only
  updates on a window-gated poll and would pin on `AWAKE` once the car sleeps). Three values:
  `AWAKE` (fresh live telemetry, < 60 s), `ASLEEP` (no live data but the always-on VCSEC
  health poll still answers ⇒ parked & sleeping nearby), and `UNREACHABLE` (the car answers
  *nothing* over BLE ⇒ driven off / out of range / deep sleep — published explicitly instead
  of a phantom `ASLEEP`; nothing heard since boot/re-pair ⇒ omitted so HA shows "unknown").
  The web UI mirrors this exactly: it hides the hero card entirely when `UNREACHABLE` (no
  honest status to show) rather than claiming the car is asleep. Reachability is tracked by a
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

1. **Key deleted on the car side** — auto-detected. Any signed command failing with
   `KEY_NOT_ON_WHITELIST` (substring `"whitelist"` in `make_result_cb_`) sets
   `pairing_lost_`; `auto_pair_task` also runs a periodic signed VCSEC `health_probe_`
   (~30 s) so it's caught even with no evcc traffic. On detection the key is regenerated
   (the old one is useless), session + cache cleared, and pairing restarts.
2. **Key regenerated** (`/gen_keys?force=1`) — `generate_key()` now also clears the
   session + cache and drops the BLE link.
3. **VIN changed** (`/set_vin`) — `reset_for_new_vehicle()` regenerates the key, clears
   session + cache, and forgets the stored `ble_mac` (old car), then reboots. Re-saving
   the same VIN is a no-op for the pairing.

After any of these `has_session()` is false → UI shows "not paired", hides controls/SOC.

## Important Notes

- Private key stored in NVS unencrypted — secure physical access to the device
- Keys are enrolled as **Charging Manager only** (charging + wake); owner role is not exposed (`?role=owner` → `403`). Sole purpose is the evcc BLE integration.
- BLE connection is on-demand; first command after idle takes ~3-5s for scan+connect
- Tesla allows max 3 simultaneous BLE keys per vehicle
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
# Monitor with timestamps
idf.py monitor -p /dev/tty.usbserial-*

# Test command via curl
curl -X POST http://<ESP32-IP>/api/1/vehicles/<VIN>/command/wake_up

# Check vehicle data
curl http://<ESP32-IP>/api/1/vehicles/<VIN>/vehicle_data

# Erase NVS (reset key + sessions)
idf.py erase-flash -p /dev/tty.usbserial-*
```
