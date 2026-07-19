# tesla-key-esp32 — Technical Reference

ESP32 BLE↔HTTP proxy for Tesla vehicles (runs on esp32 / esp32s3 / esp32c3 / esp32c6 / esp32c5).
Exposes a REST API on the LAN, API-compatible with
[TeslaBleHttpProxy](https://github.com/wimaha/TeslaBleHttpProxy); drop-in for the
[evcc](https://evcc.io) `tesla-ble` integration. User guide: [../README.md](../README.md).

## Hardware

The four chips yoziru/tesla-ble supports — **esp32, esp32s3, esp32c3, esp32c6** (all WiFi 2.4
GHz + BLE) — plus **esp32c5** (dual-band Wi-Fi 6, e.g. the LilyGO T-Dongle-C5), which tesla-ble's
`targets:` omits but which is enabled here via a local build-time patch (see
[ARCHITECTURE.md](ARCHITECTURE.md) → "esp32c5 via a local build-time patch"). **≥ 4 MB flash**
(dual-OTA layout: two ~2 MB app slots; a larger flash — the T-Dongle-C5 has 16 MB — just leaves
the top unused). No PSRAM required. ESP32-S2 (no Bluetooth) and ESP32-H2 / P4 (no WiFi) cannot run
this firmware; ESP32-C61 would need the same local-patch treatment as C5. USB data cable for
flashing.

## Flash prebuilt artifacts

Browser flasher + WiFi/VIN setup: [../README.md](../README.md). The flasher is served on
GitHub Pages (ESP Web Tools / Web Serial), rebuilt and deployed automatically by CI on every
firmware change; each change also publishes a
[GitHub release](https://github.com/0Bu/tesla-key-esp32/releases/latest) with the same bins.

Flash by hand (needs `brew install esptool`). Use the per-target **merged** image — it bakes
in the correct bootloader offset (0x1000 on the classic esp32, 0x2000 on esp32c5, 0x0 on s3/c3/c6), so one command
works for any chip. This erases `nvs` (re-enter WiFi/VIN, re-pair once):
```bash
# <suffix>: "" for esp32, else -s3 / -c3 / -c6 / -c5 (so "esp32" appears once in the name)
esptool --chip <esp32|esp32s3|esp32c3|esp32c6|esp32c5> write_flash 0x0 \
  tesla-key-esp32<suffix>-<version>-merged.bin
```
To preserve `nvs`, flash the separate parts from a local `build/` instead:
`cd build && esptool --chip <target> write_flash "@flash_args"`.

## Build from source

Builds run in the official **ESP-IDF Docker image, pinned to the version CI uses**
(`scripts/idf-docker.sh` reads it from `.github/workflows/build.yml`, so it never drifts) —
no local toolchain to install. Flashing is done from the host with `esptool`, because Docker
Desktop has no USB passthrough.

```bash
brew install esptool                                          # host flasher (once)
git clone https://github.com/0Bu/tesla-key-esp32.git && cd tesla-key-esp32

# Build via the CI-pinned ESP-IDF image (first run pulls it, then fetches
# yoziru/tesla-ble — 2–4 min). The wrapper keeps build/ host-owned. Pick your chip:
./scripts/idf-docker.sh idf.py set-target esp32s3 build   # or esp32 / esp32c3 / esp32c6
# For esp32c5, run scripts/prepare-tesla-ble-c5.sh once first (patches a local tesla-ble copy):
#   ./scripts/prepare-tesla-ble-c5.sh && ./scripts/idf-docker.sh idf.py set-target esp32c5 build

# Optional: WiFi SSID/pass + VIN (BLE MAC auto) — interactive
./scripts/idf-docker.sh idf.py menuconfig

# Flash from the host (preserves nvs — @flash_args skips nvs@0x9000). Use the same
# --chip you built for; @flash_args already has the right bootloader offset.
cd build && esptool --chip esp32s3 -p <port> write_flash "@flash_args"   # or esp32 / esp32c3 / esp32c6 / esp32c5
```

WiFi/VIN may be left blank and set later via the setup AP. Flash-mode fallback: hold `BOOT`,
tap `RESET`, release `BOOT`, then flash. Serial log: `screen <port> 115200` (exit `Ctrl-A` `K`).

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

- Web flasher / host `esptool … write_flash "@flash_args"`: `nvs` untouched → data kept.
- `esptool … write_flash 0x0 …-merged.bin`, `esptool … erase_flash`: erase whole flash → data lost.

`nvs` offset/size must not change across versions, or old data is stranded.

## Pairing

Mostly automatic, with one manual step at the car. On first boot the device generates an
ECDSA P-256 key (stored in NVS, never leaves the device). **A VIN must be configured first** —
the device targets your car by its VIN-derived BLE name, so without a VIN the auto-pair task
stays idle (it logs `no VIN configured — pairing disabled` and does **not** connect or enrol;
this is by design, so it can never whitelist a key onto an arbitrary nearby Tesla). Nearby
Teslas are still listed — the device runs a periodic listing-only scan, so the web UI shows them
sorted by signal without a manual `/scan` (which also still works). Set the VIN via the setup AP
or `POST /set_vin`. Once a
plausible 17-char VIN is set, while unpaired and the car is in BLE range, the auto-pair task
probes the car and sends a whitelist-add. The car only shows the pairing dialog on the
**touchscreen** while a Tesla NFC keycard is resting on the center-console card reader — place
a card there, then confirm on screen within ~45 s. No Pair button in the web UI, but the NFC
card is required to authorise the enrolment.

- Key fingerprint = `SHA-1(pubkey)[:4]` (e.g. `0E:8A:1D:BE`); shown in the web UI.
- Regenerate: tap the fingerprint in the UI, or `POST /gen_keys?force=1`. Without `force`,
  `/gen_keys` returns `409`. Regenerating un-pairs the vehicle.
- Manual trigger: `POST /send_key` → `{"result":true,"role":"charging_manager",
  "reason":"key sent — confirm the pairing request on the car's screen"}`.
- New key appears as *"Unknown key"* in the car's key list.

Enrolls **Charging Manager** only (charging + wake + read). Owner role disabled
(`/send_key?role=owner` → `403`). A Tesla keeps at most ~3 *simultaneous* BLE connections
(shared across phone keys and fobs) — that connection limit, not a key count, is what blocks
pairing when full.

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
| `set_charging_amps` | `{"charging_amps": 11}` (0–48; the car enforces its per-model max) |
| `set_charge_limit` | `{"percent": 80}` (50–100) |
| `charge_port_door_open` / `charge_port_door_close` | — |
| `door_lock` / `door_unlock` | — |
| `flash_lights` / `honk_horn` | — |
| `set_sentry_mode` | `{"on": true}` |
| `auto_conditioning_start` / `auto_conditioning_stop` | — |
| `set_scheduled_charging` | `{"enable": true, "start_minutes": 1380}` (minutes after local midnight; 1380 = 23:00) |

> A **Charging-Manager** key may only run charging actions + wake. The car therefore **rejects**
> `door_lock` / `door_unlock`, `flash_lights` / `honk_horn`, `set_sentry_mode`, and
> `auto_conditioning_start` / `auto_conditioning_stop` with an authentication failure — these are
> accepted by the API for completeness but never execute. Only `charge_start` / `charge_stop`,
> `set_charging_amps`, `set_charge_limit`, `set_scheduled_charging`, `charge_port_door_open` /
> `charge_port_door_close` and `wake_up` actually run (see [Security](#security)).

```json
{ "response": { "result": true, "command": "charge_start",
  "vin": "5YJ3E1EA1JF000001", "reason": "command executed successfully" } }
```

### Vehicle data

```
GET /api/1/vehicles/{VIN}/vehicle_data
```
```json
{ "response": { "result": true, "vin": "5YJ3E1EA1JF000001", "reason": "success",
  "response": {
  "charge_state": { "charging_state": "Charging", "battery_level": 72,
    "charge_limit_soc": 80, "charger_power": 11, "charge_rate": 58.3,
    "charge_amps": 16, "battery_range": 280.5 } } } }
```
Doubled `response` and `charge_amps` are intentional — they match the Fleet API /
TeslaBleHttpProxy shape evcc parses. `reason` is `"success"` or `"stale or unavailable"`
(cache served while the car sleeps — evcc reads `charge_state`, not `result`).

### Body controller state (no wake)

```
GET /api/1/vehicles/{VIN}/body_controller_state
```
```json
{ "response": { "result": true, "vin": "5YJ3E1EA1JF000001", "data": {
  "vehicle_lock_state": "LOCKED", "vehicle_sleep_status": "ASLEEP",
  "user_presence": "NOT_PRESENT" }, "reason": "success" } }
```

### Management

```
GET  /                     Web UI (status, pairing, quick commands; alias /index.html)
GET  /status               { vin, ip, version, key_present, key_fingerprint,
                             key_created (epoch, omitted if clock unsynced), paired,
                             paired_at (epoch, omitted if unknown), reauth,
                             wifi:{ssid,rssi,std},
                             ble:{connected,scanning,rssi,addr
                                  | devices:[{addr,name,rssi,connectable}],
                                  connect_fail?,car_connectable? (only while actively failing)},
                             link: "awake"|"idle"|"asleep"|"unreachable"|"unknown" (drives the
                               hero; "idle" = reachable but not provably asleep — the "Parked" card),
                             vcsec_sleep: "AWAKE"|"ASLEEP"|"UNKNOWN" (raw un-debounced flag, diagnostics),
                             vehicle:{soc,status,charge_limit,power,amps,actual_amps,volts,phases}
                               (only when link=="awake", cached; each field only when reported),
                             mqtt:{configured,connected,tls,broker,error?} (HA bridge),
                             syslog:{configured,resolved,reachable,host?,port?,error?}
                               (UDP diag-log forwarder; reachable is an advisory ping hint,
                               never a delivery gate),
                             tele:{climate,drive,tires,closures} (read-only telemetry;
                               emitted only while the BLE link is up),
                             last:{soc,status} (last-known snapshot for the asleep card),
                             last_seen_s (seconds since last contact),
                             last_reboot: "heap:<n>" (only when the heap watchdog ended the
                                          previous boot, n = consecutive such restarts;
                                          absent on any ordinary boot) }
GET  /events               WebSocket live-status feed for the web UI: client sends "sub",
                             device pushes the /status JSON (~2 s). WS-only, no poll fallback.
                             A plain (non-WebSocket) GET completes the handshake only.
POST /scan                 Time-limited BLE discovery scan (populates ble.devices)
GET  /diag[?verbose=0|1][?clear=1]   Plain-text in-memory diag log (verbose=0 turns raw-RX
                             logging back off; the X-Diag-Verbose response header echoes the
                             current verbose state for the web UI)
POST /gen_keys[?force=1]   Generate ECDSA P-256 key (refuses overwrite without force)
POST /send_key             Manually trigger pairing (charging_manager only; normally automatic)
POST /set_vin              Persist VIN and reboot
POST /set_mqtt             Persist the MQTT broker for the HA bridge and reboot
                             ({"broker":"host:port"} or full "mqtt://…"; "" disables MQTT)
POST /set_syslog           Persist the UDP Syslog server for the diag log and reboot
                             ({"server":"host:port"}; a bare host defaults to port 514;
                             "" disables Syslog)
POST /set_time             Set the wall clock from the browser ({"ms":<epoch>}) — NTP fallback
GET  /ota/check[?ms=<epoch>]   Start a background update check (then poll /ota/status)
POST /ota/update           Start the background self-update (downloads, then reboots)
GET  /ota/status           Poll OTA progress { state, progress, message, available,
                             update_available, current }
GET  /api/proxy/1/version  { version, platform } (firmware version + running chip: "ESP32"/"ESP32-S3"/"ESP32-C3"/"ESP32-C6"/"ESP32-C5")
POST /mcp                  MCP server for AI agents (Streamable HTTP, stateless JSON-RPC 2.0;
                             GET → 405, no SSE). Tools = charging command set + read-only
                             get_vehicle_state (cache-only, never wakes the car) — full
                             integration guide with wire + client examples: MCP.md
```

> **MCP (AI agents):** the complete integration guide — transport details, tool
> reference, curl wire examples, and ready-to-use client configs for Claude
> Code/Desktop, VS Code and the Python SDK — lives in [`MCP.md`](MCP.md).

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
live in `scripts/idf-docker.sh idf.py menuconfig` → *Tesla Key Configuration*:

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
tesla-key/<node>/charge      {soc,charge_limit,power,amps,range,rate,charging_state,
                              actual_current,current_request,volts,phases,energy_added,
                              minutes_to_full,limit_reason}
tesla-key/<node>/climate     {inside,outside,setpoint,on,preconditioning,
                              cop,cop_cooling,cop_temp,cop_reason,
                              front_defrost,rear_defrost,defrost_mode}
tesla-key/<node>/drive       {shift,odometer}
tesla-key/<node>/tires       {fl,fr,rl,rr,warn}
tesla-key/<node>/closures    {locked,door,frunk,trunk,window,user}
tesla-key/<node>/vehicle     {sleep_status: AWAKE | ASLEEP | IDLE | UNREACHABLE}
tesla-key/<node>/device      {wifi_rssi,ble_rssi,ble_connected,paired,boot_time,free_heap,version}
homeassistant/<sensor|binary_sensor>/<node>/<object>/config   (discovery, retained)
```

All state topics are retained JSON. Numeric fields are emitted only when the car reported
them (proto3 optional), so an unseen value shows as *unknown* in HA, not a phantom `0`.
While a parked car sleeps the source polls pause (so it can sleep), and MQTT keeps serving
the last-known retained values until the next active window.

## Syslog

`main/syslog.cpp` forwards the same output as `GET /diag` — the device's console log — to a
UDP Syslog collector (RFC 5424), best-effort. Useful to watch a pairing/reconnect live, or to
keep history past the in-RAM ring's ~16 KB / a reboot.

**Enable:** set the server in the web UI (Connections → Syslog, `IP:PORT`, e.g.
`192.168.1.22:514`; a bare host defaults to port 514) — stored in NVS (`syslog_uri`) and
applied after the reboot it triggers. Leave empty to disable. Compile-time default:
`CONFIG_TESLA_SYSLOG_SERVER` (`""`), overridden by the NVS value.

Delivery only requires the hostname/IP to resolve (best-effort UDP — there is no
handshake/ack); `/status.syslog.reachable` is an advisory ARP/ICMP ping hint only, not a
delivery gate, so a collector behind a firewalled-ICMP host still receives lines with
`reachable:false` shown in the UI.

## Troubleshooting

**No WiFi** — verify SSID/pass (case-sensitive); WPA2 only, no enterprise; reflash or
`provision.py`.

**BLE doesn't find vehicle** — car within ~10 m, awake; scanning starts after WiFi.
Log: `scanning for Tesla BLE...` → `Tesla '<name>' found: … — connecting`.

**Command times out** (`'charge_start' timed out`) — car in deep sleep; `wake_up` first,
wait 5 s, retry. Stale session: `esptool --chip <target> -p <port> erase_flash`.

**No pairing prompt** — a VIN must be configured (else `/diag` shows `auto-pair: no VIN
configured — pairing disabled` and the device never connects; set it via the setup AP or
`POST /set_vin`); a Tesla NFC keycard must be on the center-console reader for the dialog to
appear; car awake + in range; `key_present: true` in `/status` (else `/gen_keys?force=1`);
watch for `auto-pair: requesting key enrolment` in `/diag`; confirm on touchscreen within
~45 s, or `POST /send_key` to retrigger.

**Key rejected** — Tesla app → Security → Keys → delete *"Unknown key"*;
`esptool --chip <target> -p <port> erase_flash`; let it re-pair (confirm on screen).

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

[GNU Affero General Public License v3.0](../LICENSE) (AGPL-3.0).

The firmware compiles in [yoziru/tesla-ble](https://github.com/yoziru/tesla-ble), which is
licensed under **AGPL-3.0**. Because that library is statically linked into a single firmware
image, the resulting binary is a combined/derivative work and must be distributed under the
AGPL-3.0 as a whole — its copyleft propagates to this project, including the §13 network-use
clause (this device runs an HTTP API / web UI, so operators who let others interact with it
over the network must offer them the corresponding source for that firmware version).
