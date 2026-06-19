# tesla-key-esp32 — Technical Reference

ESP32-S3 BLE↔HTTP proxy for Tesla vehicles. Exposes a REST API on the LAN, API-compatible
with [TeslaBleHttpProxy](https://github.com/wimaha/TeslaBleHttpProxy); drop-in for the
[evcc](https://evcc.io) `tesla-ble` integration. User guide: [../README.md](../README.md).

## Hardware

ESP32-S3 (BLE 5.0), ≥ 8 MB flash (dual-OTA layout: two 3 MB app slots). No PSRAM
required. ESP32 / S2 / C3 not supported.
USB data cable for flashing.

## Flash prebuilt artifacts

Browser flasher + WiFi/VIN setup: [../README.md](../README.md). The flasher is served on
GitHub Pages (ESP Web Tools / Web Serial), rebuilt and deployed automatically by CI on every
firmware change; each change also publishes a
[GitHub release](https://github.com/0Bu/tesla-key-esp32/releases/latest) with the same bins.

Flash by hand (preserves `nvs`):
```bash
esptool.py --chip esp32s3 write_flash 0x0 bootloader.bin 0x8000 partition-table.bin \
  0x20000 tesla-key-esp32-<version>.bin
```
Clean full flash (erases `nvs`): write `tesla-key-esp32-<version>-merged.bin` at `0x0`.

## Build from source

ESP-IDF 5.0.1+.

```bash
# Toolchain (once)
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && git checkout v5.3.2 && ./install.sh esp32s3
echo 'alias get_idf=". ~/esp/esp-idf/export.sh"' >> ~/.zshrc

# Per shell
get_idf

# Build
git clone https://github.com/0Bu/tesla-key-esp32.git && cd tesla-key-esp32
idf.py set-target esp32s3
idf.py menuconfig          # Tesla Key Configuration: WiFi SSID/pass, VIN (BLE MAC auto)
idf.py build               # first build fetches yoziru/tesla-ble (2–4 min)
idf.py -p <port> flash monitor
```

WiFi/VIN may be left blank in menuconfig and set later via the setup AP. Flash-mode fallback:
hold `BOOT`, tap `RESET`, release `BOOT`, then flash. Exit monitor: `Ctrl+]`.

Boot log:
```
I (500) main: VIN: 5YJ3E1EA1JF000001  BLE MAC: (scan)
I (600) main: WiFi connected to 'MyNetwork'
I (650) main: IP: 192.168.1.42
I (700) http_server: HTTP server started on :80
I (700) main: tesla-key-esp32 running. API on port 80.
```

## Provision without rebuilding

Writes WiFi/VIN to the NVS config partition only:
```bash
python provision.py --port <port> --ssid MyNet --password secret --vin 5YJ3E1EA1JF000001
```

## Upgrading

WiFi, VIN, private key and BLE sessions live in the `nvs` partition (`0x9000`, namespaces
`tesla_cfg` + `tesla_ble`).

- Web flasher / `idf.py flash` / `idf.py app-flash`: `nvs` untouched → data kept.
- `write_flash 0x0 …-merged.bin`, `idf.py erase-flash`: erase whole flash → data lost.

`nvs` offset/size must not change across versions, or old data is stranded.

## Pairing

Mostly automatic, with one manual step at the car. On first boot the device generates an
ECDSA P-256 key (stored in NVS, never leaves the device). While unpaired and the car is in BLE
range, the auto-pair task probes the car and sends a whitelist-add. The car only shows the
pairing dialog on the **touchscreen** while a Tesla NFC keycard is resting on the
center-console card reader — place a card there, then confirm on screen within ~45 s. No Pair
button in the web UI, but the NFC card is required to authorise the enrolment.

- Key fingerprint = `SHA-1(pubkey)[:4]` (e.g. `0E:8A:1D:BE`); shown in the web UI.
- Regenerate: tap the fingerprint in the UI, or `POST /gen_keys?force=1`. Without `force`,
  `/gen_keys` returns `409`. Regenerating un-pairs the vehicle.
- Manual trigger: `POST /send_key` → `{"result":true,"role":"charging_manager",
  "reason":"key sent — confirm the pairing request on the car's screen"}`.
- New key appears as *"Unknown key"* in the car's key list.

Enrolls **Charging Manager** only (charging + wake + read). Owner role disabled
(`/send_key?role=owner` → `403`). Max 3 BLE keys per vehicle.

## HTTP API

Base: `http://<ESP32-IP>`. No auth, no TLS — see [SECURITY.md](SECURITY.md).

### Commands

```
POST /api/1/vehicles/{VIN}/command/{command}   Content-Type: application/json
```

| Command | Body |
|---------|------|
| `wake_up` | — |
| `charge_start` / `charge_stop` | — |
| `set_charging_amps` | `{"charging_amps": 11}` (0–32) |
| `set_charge_limit` | `{"percent": 80}` (50–100) |
| `charge_port_door_open` / `charge_port_door_close` | — |
| `door_lock` / `door_unlock` | — |
| `flash_lights` / `honk_horn` | — |
| `set_sentry_mode` | `{"on": true}` |
| `auto_conditioning_start` / `auto_conditioning_stop` | — |
| `set_scheduled_charging` | `{"enable": true, "start_minutes": 1380}` (minutes after local midnight; 1380 = 23:00) |

```json
{ "response": { "result": true, "command": "charge_start",
  "vin": "5YJ3E1EA1JF000001", "reason": "command executed successfully" } }
```

### Vehicle data

```
GET /api/1/vehicles/{VIN}/vehicle_data
```
```json
{ "response": { "result": true, "vin": "5YJ3E1EA1JF000001", "response": {
  "charge_state": { "charging_state": "Charging", "battery_level": 72,
    "charge_limit_soc": 80, "charger_power": 11, "charge_rate": 58.3,
    "charge_amps": 16, "battery_range": 280.5 } } } }
```
Doubled `response` and `charge_amps` are intentional — they match the Fleet API /
TeslaBleHttpProxy shape evcc parses.

### Body controller state (no wake)

```
GET /api/1/vehicles/{VIN}/body_controller_state
```
```json
{ "response": { "result": true, "data": {
  "vehicle_lock_state": "LOCKED", "vehicle_sleep_status": "ASLEEP",
  "user_presence": "NOT_PRESENT" } } }
```

### Management

```
GET  /                     Web UI (status, pairing, quick commands)
GET  /status               { vin, ip, version, key_present, key_fingerprint,
                             key_created (epoch, omitted if clock unsynced), paired,
                             paired_at (epoch, omitted if unknown), reauth,
                             wifi:{ssid,rssi,std},
                             ble:{connected,scanning,rssi,addr | devices:[{addr,name,rssi}]},
                             vehicle:{soc,status,power,amps} (when connected, cached),
                             mqtt:{configured,connected,broker} (HA bridge),
                             tele:{climate,drive,tires,closures} (read-only telemetry) }
POST /scan                 Time-limited BLE discovery scan (populates ble.devices)
GET  /diag[?verbose=1][?clear=1]   Plain-text in-memory diag log
POST /gen_keys[?force=1]   Generate ECDSA P-256 key (refuses overwrite without force)
POST /send_key             Manually trigger pairing (charging_manager only; normally automatic)
POST /set_vin              Persist VIN and reboot
POST /set_mqtt             Persist the MQTT broker for the HA bridge and reboot
                             ({"broker":"host:port"} or full "mqtt://…"; "" disables MQTT)
POST /set_time             Set the wall clock from the browser ({"ms":<epoch>}) — NTP fallback
GET  /ota/check[?ms=<epoch>]   Start a background update check (then poll /ota/status)
POST /ota/update           Start the background self-update (downloads, then reboots)
GET  /ota/status           Poll OTA progress { state, progress, message, available,
                             update_available, current }
GET  /api/proxy/1/version  Firmware version
```

## evcc Integration

In the evcc UI (**Settings → Vehicles → Add → Custom device**) the fields are flat —
no `vehicles:` wrapper, no list dash; the editor adds those. For hand-edited
`evcc.yaml`, nest the same fields under `vehicles:` as a list item.

```yaml
name: tesla
type: template                      # required when using template:
template: tesla-ble
title: Tesla Key ESP32              # optional
vin: 5YJ3E1EA1JF000001
capacity: 60                        # optional, battery kWh
url: http://tesla-key-esp32.local   # or http://<ESP32-IP>
port: 80                            # device serves on 80 (template default 8080)
```

evcc calls: `GET …/vehicle_data?endpoints=charge_state`,
`POST …/command/{charge_start,charge_stop,set_charging_amps,wake_up}`.
SOC read from `.response.response.charge_state.battery_level`, current from `…charge_amps`.

## Home Assistant (MQTT)

`main/mqtt_ha.cpp` mirrors every cached reading to MQTT using Home Assistant's
[MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery), so a
**Tesla Key** device with all entities appears in HA automatically — no YAML. **Read-only:**
no command topics are subscribed; HA cannot control or wake the car. The bridge runs in its
own task and is independent of evcc, BLE and pairing.

**Enable:** set the broker in the web UI (Connection → MQTT, `IP:PORT`) — stored in NVS
(`mqtt_uri`) and applied after the reboot it triggers. Compile-time defaults / credentials
live in `idf.py menuconfig` → *Tesla Key Configuration*:

| Option | Default | Purpose |
|--------|---------|---------|
| `CONFIG_TESLA_MQTT_BROKER_URI` | `""` | Broker (`mqtt://host:port`; empty = disabled). NVS `mqtt_uri` overrides it. |
| `CONFIG_TESLA_MQTT_USERNAME` / `_PASSWORD` | `""` | Broker auth (optional). |
| `CONFIG_TESLA_MQTT_DISCOVERY_PREFIX` | `homeassistant` | HA discovery prefix. |
| `CONFIG_TESLA_MQTT_BASE_TOPIC` | `tesla-key` | State-topic prefix. |
| `CONFIG_TESLA_MQTT_PUBLISH_INTERVAL_S` | `15` | Republish cadence (also publishes on every reconnect). |

**Topics** (node id `teslakey_<mac3>` from the WiFi MAC, stable across VIN changes):

```
tesla-key/<node>/availability                 online | offline   (LWT, retained)
tesla-key/<node>/charge      {soc,charge_limit,power,amps,range,rate,charging_state}
tesla-key/<node>/climate     {inside,outside,setpoint,on,preconditioning}
tesla-key/<node>/drive       {shift,odometer}
tesla-key/<node>/tires       {fl,fr,rl,rr,warn}
tesla-key/<node>/closures    {locked,door,frunk,trunk,window,user}
tesla-key/<node>/vehicle     {sleep_status}
tesla-key/<node>/device      {wifi_rssi,ble_rssi,ble_connected,paired,uptime,free_heap,version}
homeassistant/<sensor|binary_sensor>/<node>/<object>/config   (discovery, retained)
```

All state topics are retained JSON. Numeric fields are emitted only when the car reported
them (proto3 optional), so an unseen value shows as *unknown* in HA, not a phantom `0`.
While a parked car sleeps the source polls pause (so it can sleep), and MQTT keeps serving
the last-known retained values until the next active window.

## Troubleshooting

**No WiFi** — verify SSID/pass (case-sensitive); WPA2 only, no enterprise; reflash or
`provision.py`.

**BLE doesn't find vehicle** — car within ~10 m, awake; scanning starts after WiFi.
Log: `scanning for Tesla BLE...` → `Tesla '<name>' found: … — connecting`.

**Command times out** (`'charge_start' timed out`) — car in deep sleep; `wake_up` first,
wait 5 s, retry. Stale session: `idf.py erase-flash && idf.py flash`.

**No pairing prompt** — a Tesla NFC keycard must be on the center-console reader for the
dialog to appear; car awake + in range; `key_present: true` in `/status` (else
`/gen_keys?force=1`); watch for `auto-pair: requesting key enrolment` in `/diag`; confirm on
touchscreen within ~45 s, or `POST /send_key` to retrigger.

**Key rejected** — Tesla app → Security → Keys → delete *"Unknown key"*;
`idf.py erase-flash && idf.py flash`; let it re-pair (confirm on screen).

**Serial permission denied (Linux)** — `sudo usermod -aG dialout $USER && newgrp dialout`.

**evcc empty SoC / no current** — `port: 80` set; car reachable over BLE
(`/status` → `ble.connected: true`). Verify shape:
```bash
curl .../api/1/vehicles/<VIN>/vehicle_data | jq '.response.response.charge_state.battery_level'
```
Must return a number. `null` → firmware too old; reflash.

## Security

Full threat model + Flash Encryption / Secure Boot: [SECURITY.md](SECURITY.md).

- Charging Manager key only — cannot unlock doors / drive even with physical access.
- Private key in NVS, **unencrypted by default**; dumpable via USB on a factory S3. Enable
  Flash + NVS Encryption (irreversible).
- API has no auth / TLS by design (evcc cannot send credentials). Trusted LAN only; never
  expose to the internet. Front with a reverse proxy or VLAN if access control is needed.

## Internals

| | |
|---|---|
| BLE protocol | Tesla VCSEC + Infotainment (Protobuf over GATT) |
| Service UUID | `00000211-b2d1-43f0-9b88-960cebf8b91e` |
| Encryption | ECDH + AES-GCM (mbedTLS) |
| Signing | ECDSA P-256 (key in NVS) |
| BLE library | [yoziru/tesla-ble](https://github.com/yoziru/tesla-ble) v5.1.1 |
| BLE stack | NimBLE |
| Fragment size | 20 bytes / BLE write chunk |
| HTTP server | `esp_http_server` :80 |

## License

MIT. [yoziru/tesla-ble](https://github.com/yoziru/tesla-ble) is also MIT.
