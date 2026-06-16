# esp32-tesla-key

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
POST /gen_keys                                 # generate ECDSA P-256 key
POST /send_key[?role=owner]                    # pair with vehicle
GET  /api/proxy/1/version
```

## evcc Integration

```yaml
vehicles:
  - name: Tesla
    type: tesla-ble-http
    url: http://<ESP32-IP>
    vin: <VIN>
```

## Important Notes

- Private key stored in NVS unencrypted — secure physical access to the device
- `charged_manager` role (default) limits commands to charging + wake; use `?role=owner` for full access
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
