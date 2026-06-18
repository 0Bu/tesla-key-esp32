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
| `tesla_cfg` | WiFi SSID/pass, VIN, BLE MAC (runtime cfg)  |
| `tesla_ble` | Private key, VCSEC session, Info session    |

## Commands Implemented

All commands: `charge_start`, `charge_stop`, `set_charging_amps`, `set_charge_limit`,
`wake_up`, `charge_port_door_open/close`, `door_lock/unlock`, `flash_lights`,
`honk_horn`, `set_sentry_mode`, `auto_conditioning_start/stop`.

## HTTP API

```
POST /api/1/vehicles/{VIN}/command/{command}   # execute command
GET  /api/1/vehicles/{VIN}/vehicle_data        # charge state
GET  /api/1/vehicles/{VIN}/body_controller_state
GET  /status                                   # web-UI JSON (wifi, ble, vehicle cache)
POST /scan                                     # start a time-limited BLE discovery scan
GET  /diag                                     # plain-text in-memory diag log (?verbose=1 raw RX, ?clear=1 reset)
POST /gen_keys[?force=1]                       # generate key (refuses overwrite w/o force)
POST /send_key                                 # pair with vehicle (Charging Manager only)
POST /set_time                                 # set wall clock from the browser ({"ms":<epoch>}); fallback when NTP unreachable
POST /set_vin                                  # persist VIN + reboot
GET  /api/proxy/1/version
GET  /ota/check[?ms=<epoch>]                   # start background manifest check (non-blocking); poll /ota/status. ms = browser-clock NTP fallback
POST /ota/update                               # start background self-update (pull, then reboot)
GET  /ota/status                               # poll OTA progress {state,progress,message,available}
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
