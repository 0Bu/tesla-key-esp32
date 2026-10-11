# HTTP and MQTT API

Base `http://<ESP32-IP>`. No auth, no TLS — see [SECURITY](SECURITY.md#http-api-exposure). Mutating
browser requests from a foreign or DNS-rebound Origin get `403`; Host must be the device name or
current IP, and state-changing legacy GET forms use the same gate. Headerless evcc/curl clients stay
compatible, so this is not a substitute for the trusted-LAN boundary.

## Commands

```
POST /api/1/vehicles/{VIN}/command/{command}   Content-Type: application/json
```

| Command | Body |
|---------|------|
| `wake_up` | — |
| `charge_start` / `charge_stop` | — |
| `set_charging_amps` | `{"charging_amps": 11}` (whole number 0–48; the car enforces its per-model max) |
| `set_charge_limit` | `{"percent": 80}` (50–100) |
| `charge_port_door_open` / `charge_port_door_close` | — |
| `door_lock` / `door_unlock` | — |
| `flash_lights` / `honk_horn` | — |
| `set_sentry_mode` | `{"on": true}` |
| `auto_conditioning_start` / `auto_conditioning_stop` | — |
| `set_scheduled_charging` | `{"enable": true, "start_minutes": 1380}` (0–1439 minutes after local midnight; 1380 = 23:00) |

- **evcc boolean bodies:** `charge_start` also accepts the JSON scalar `true` and `charge_stop`
  `false` (evcc's generic setter); mismatched booleans and all other non-object bodies are `400`.
- **Validation:** supplied integers must be integral and in range, else `400` — nothing is silently
  clamped. Absent REST fields default to percent=80, enable=false, start_minutes=0 and sentry on=false;
  supplied fields are still validated. MCP has its own required-argument rules.
- **`set_charging_amps` is verified:** `charging_amps` is required. Success means the firmware sent a
  fresh, serialized `ChargeState` request and the car reported *exactly* the requested current.
  Missing/malformed input is `400`; an unreachable car, rejected command or missing/mismatched
  readback is `502`, so controllers such as evcc retry instead of accepting a false success.
- **Failures** keep the Tesla-compatible JSON (`result:false` plus the vehicle/proxy reason) but use
  HTTP `502`, not a misleading `200`.

> A **Charging-Manager** key may only run charging actions + wake. The car therefore **rejects**
> `door_lock`/`door_unlock`, `flash_lights`/`honk_horn`, `set_sentry_mode` and
> `auto_conditioning_*` with an authentication failure — the API accepts them for completeness but
> they never execute. Only `charge_start`/`charge_stop`, `set_charging_amps`, `set_charge_limit`,
> `set_scheduled_charging`, `charge_port_door_open`/`charge_port_door_close` and `wake_up` run (see
> [Security](SECURITY.md#http-api-exposure)).

```json
{ "response": { "result": true, "command": "charge_start",
  "vin": "<VIN>", "reason": "command executed successfully" } }
```

## Vehicle data

```
GET /api/1/vehicles/{VIN}/vehicle_data
```
```json
{ "response": { "result": true, "vin": "<VIN>", "reason": "success",
  "response": {
  "charge_state": { "charging_state": "Charging", "battery_level": 72,
    "usable_battery_level": 72, "charge_limit_soc": 80, "charger_power": 11,
    "charge_rate": 58.3, "charge_amps": 16, "battery_range": 280.5,
    "minutes_to_full_charge": 45 } } } }
```

The evcc response always includes the complete fixed numeric `charge_state` schema; `/status`
and MQTT may instead omit unreported optional fields. `battery_range`/`charge_rate` here remain
miles/mph; MQTT uses km/km/h.

The doubled `response` and `charge_amps` are intentional — they match the Fleet API /
TeslaBleHttpProxy shape evcc parses. `battery_level` is the nominal pack SOC, `usable_battery_level`
the usable SOC matching the Tesla app. While the car is idle the cache may stay available so polling
does not wake it; while charging, or within five minutes of a command, a `ChargeState` older than 30 s
returns HTTP `503` with reason `"stale or unavailable"` instead of posing as live telemetry.

## Body controller state (no wake)

```
GET /api/1/vehicles/{VIN}/body_controller_state
```
```json
{ "response": { "result": true, "vin": "<VIN>", "data": {
  "vehicle_lock_state": "LOCKED", "vehicle_sleep_status": "ASLEEP",
  "user_presence": "NOT_PRESENT" }, "reason": "success" } }
```

## Management

| Endpoint | Purpose |
|---|---|
| `GET /` (`/index.html`) | Web UI (status, pairing, quick commands). |
| `GET /status[?redact=1]` | Full device/vehicle status. Use `redact=1` for sharing; the board MAC remains visible. Export rules below. |
| `POST /scan` | Time-limited BLE discovery scan (fills `ble.devices`). |
| `GET /diag[?verbose=0\|1][&clear=1][&redact=1]` | Plain-text in-memory log. `verbose` changes logging; `clear=1` deletes the log. Export rules below. |
| `GET /coredump[?clear=1]` | Private crash image; `404` when absent. Requires the matching ELF. `clear=1` erases the artifact instead. |
| `POST /crash/dismiss` | Deletes this boot's crash report; a failed erase returns `500` and retains the report. |
| `GET /heap` | `{dt, b0, b_boot, unit:"KiB", scale:10, free[], largest[]}` — 24 h memory trend; interpretation below. |
| `POST /gen_keys[?force=1]` | Generate the ECDSA P-256 key (refuses overwrite without `force`, `409`). Identity mutation is Stable-only: `PendingVerify`, unknown OTA state or an active OTA/update returns `503` before anything changes. |
| `POST /send_key` | Manually trigger pairing (Charging Manager only; normally automatic). |
| `POST /set_vin` | Persist the VIN, regenerate the key, clear the paired BLE MAC/session and reboot. Same Stable-only `503` rule as `/gen_keys`. |
| `POST /set_mqtt` | `{"broker":"host:port"}` or full `"mqtt://…"`; `""` disables. Changed brokers are tested before persistence; failure saves nothing. |
| `POST /set_syslog` | `{"server":"host:port"}` (a bare host defaults to 514; `""` disables) and reboot. |
| `POST /set_wifi` | `{"ssid":"…","pass":"…"}` and reboot, with one-shot credential rollback. curl/`provision.py` only; validation and rollback below. |
| `POST /set_time` | `{"ms":<epoch>}` — set the wall clock from the browser (NTP fallback). |
| `POST /set_ota` | `{"channel":"release"\|"dev"}` — switch the update channel. |
| `GET /ota/check[?ms=<epoch>][&pr=<N>]` | Start a background update check (then poll `/ota/status`); `pr=<N>` targets that PR's preview build. |
| `POST /ota/update[?pr=<N>]` | Start the background self-update (downloads, then reboots). |
| `GET /ota/status` | `{state, progress, message, available, update_available, current, channel, pr}` (`pr` only while a PR build is targeted). |
| `GET /ota/changelog` | Release notes for the offered update: `text/plain`, ≤ 1024 bytes, `no-store`; `204` when none. |
| `GET /api/proxy/1/version` | `{version, platform}` (`"ESP32"`/`"ESP32-S3"`/`"ESP32-C3"`/`"ESP32-C6"`). |
| `POST /mcp` | MCP server for AI agents (Streamable HTTP, stateless JSON-RPC 2.0; `GET` → `405`, no SSE): [MCP.md](MCP.md). |

All of `POST /send_key`, `/set_time`, `/set_mqtt`, `/set_syslog`, `/set_wifi` and `/set_ota` return
`409` while an OTA update is flashing ([ARCHITECTURE](ARCHITECTURE.md#http-request-body-and-allocator-failure-contract)).

### Diagnostic export and heap trend

`/status?redact=1` substitutes `vin`, `ip`, `wifi.ssid`, vehicle/scanned BLE addresses and names,
`mqtt.broker` and `syslog.host` with `"<redacted>"`. All keys remain present for version comparison;
`sys.board_mac` remains visible. `/diag?redact=1` substitutes identifiers per line, retains the board
MAC and discards a wrapped partial first line. Lines over 288 bytes become `"<redacted>"`;
truncated sensitive values are redacted through the line's end. Inspect either export before sharing.

`/diag?verbose=0` disables raw-RX logging; `X-Diag-Verbose` echoes the setting. Adding `clear=1`
deletes the in-memory log. Preserve evidence before using either log or crash deletion.

`/coredump` streams chunked octet-stream and requires the **same build's ELF**, identified by
`/status.last_crash.elf_sha256`:

```bash
esp-coredump info_corefile -c coredump.bin <build>.elf
```

A board flashed before the `coredump` partition existed always returns `404`. Crash dismissal
erases first, then acknowledges; a missing partition is accepted, while other erase errors retain
the report. Deletion uses POST so links/prefetch cannot trigger it. [Export privacy](SECURITY.md#syslog-and-diagnostic-export).

`/heap` samples are tenths of a KiB, oldest first, with `null` for empty buckets. A falling `free[]`
suggests a leak; `largest[]` sinking toward the 4 KiB watchdog floor suggests fragmentation. The
`.noinit` DRAM ring survives restarts and is cleared by a power cut; samples before `b_boot` belong
to an earlier run.

### Configuration checks

`/set_mqtt` connects to a changed non-empty broker before saving: refusal/credentials return `400`,
unreachable/no answer `502`, insufficient contiguous memory `503`. Failure neither persists nor
reboots; an empty broker disables MQTT.

`/set_wifi` accepts an empty password for an open network, 8–63 UTF-8 bytes for WPA2, or exactly
64 ASCII hex characters for a raw PSK. The atomic config keeps the previous credentials for one
rollback. A successful lease drops the backup; sustained authentication refusal restores the old
network on the next boot, reboots and exposes `wifi.rolled_back`. An absent SSID gets 180 s before
rollback. [Shared provisioning contract](USER_GUIDE.md#provision-without-rebuilding).

### Status fields

| Key | Content |
|---|---|
| `vin`, `ip`, `version` | Device identity. |
| `key_present`, `key_fingerprint`, `key_created` | Key state; `key_created` (epoch) is omitted while the clock is unsynced. |
| `paired`, `paired_at`, `reauth` | Pairing state; `paired_at` (epoch) omitted if unknown. |
| `wifi` | `{ssid, rssi, std, rolled_back?}` — always present, empty while no WiFi link holds the lease; `rolled_back` appears only when the last `/set_wifi` was undone by the credential rollback. |
| `eth` | `{link, speed?, full_duplex}` — present **only** while Ethernet carries the lease (`speed` in Mbit, omitted until the PHY negotiates). Carries no MAC, so it needs no `?redact=1` treatment. |
| `ble` | `{connected, scanning, phase?, phase_s?, rssi, addr}` when connected, otherwise includes `devices:[{addr,name,rssi,connectable}]`. `connect_fail?`/`car_connectable?` appear only while actively failing. |
| `link` | `"awake"\|"idle"\|"asleep"\|"unreachable"\|"unknown"` — drives the hero (`"idle"` = reachable but not provably asleep, the "Parked" card). `vcsec_sleep` (`AWAKE\|ASLEEP\|UNKNOWN`) is the raw un-debounced flag, for diagnostics. |
| `vehicle` | `{soc, usable_soc, status, charge_limit, power, amps, actual_amps, volts, phases}` — cached, only when `link=="awake"`, each field only when reported. |
| `last`, `last_seen_s` | Last-known `{soc, usable_soc, status}` for the asleep card; seconds since last contact. |
| `mqtt` | `{configured, connected, tls, broker, error?}`; `broker` is the credential-free `host:port` even if the saved URI has userinfo. |
| `syslog` | `{configured, resolved, reachable, host?, port?, error?}`; `reachable` is an advisory ping hint, never a delivery gate. |
| `ota` | `{channel}` (`"release"\|"dev"`). |
| `tele` | `{climate, drive, tires, closures}` read-only telemetry, emitted only while the BLE link is up. |
| `last_reboot` | `"heap:<n>"` — only when the heap watchdog ended the previous boot (`n` = consecutive such restarts); absent on ordinary boots. |
| `sys` | Always present: `{board_mac, free_heap, min_free_heap, largest_block, uptime_s, wifi_reconnects, reset_reason, safe_mode, stack_min_free_bytes?:{httpd?,vehicle?,auto_pair?,mqtt?}}`. |
| `last_crash` | `{reason, reason_code, fault, coredump, task?, pc?, backtrace?[hex], corrupted?, elf_sha256?}` for a fault reset or an undismissed current-build dump. |

`ble.phase` and `phase_s` appear together: `connecting` counts down to giving up, `waiting` to a
retry, and zero means now. Heap fields are INTERNAL-only; PSRAM cannot mask them. The watchdog
acts on `sys.largest_block`. Per-task stack values are boot minima, omitted until sampled; a
measured zero remains visible. `sys.board_mac` is the physical eFuse identity.

Crash `backtrace`/`corrupted` exist only on Xtensa (esp32/s3). RISC-V (c3/c6) has no on-device
backtrace; downloaded dumps unwind offline on every target.

`/api/proxy/1/version` appends the fixed `-esp32` suffix on every target. Strip it when
comparing to `/status.version`; `platform` identifies the actual chip.

## MQTT topics

**Topics** (node id `teslakey_<vin>` from the lowercase VIN, stable across board changes; changing the
configured vehicle intentionally creates a different HA device):

```
tesla-key/<node>/availability                 online | offline   (LWT, retained)
tesla-key/<node>/charge      {soc,usable_soc,charge_limit,power,amps,range,rate,charging_state,
                              actual_current,current_request,volts,phases,energy_added,
                              minutes_to_full,limit_reason}
tesla-key/<node>/climate     {inside,outside,setpoint,on,preconditioning,
                              cop,cop_cooling,cop_temp,cop_reason,
                              front_defrost,rear_defrost,defrost_mode}
tesla-key/<node>/drive       {shift,odometer}
tesla-key/<node>/tires       {fl,fr,rl,rr,warn}
tesla-key/<node>/closures    {locked,door,frunk,trunk,window,user}
tesla-key/<node>/vehicle     {sleep_status: AWAKE | ASLEEP | IDLE | UNREACHABLE}
tesla-key/<node>/device      {wifi_rssi?,ble_rssi?,ble_connected,paired,boot_time?,free_heap,
                              version,largest_block,min_free_heap,reset_reason,
                              reset_reason_code,crash_dump,safe_mode,wifi_reconnects,
                              mqtt_reconnects,httpd_stack_min_free_bytes?,
                              vehicle_stack_min_free_bytes?,auto_pair_stack_min_free_bytes?,
                              mqtt_stack_min_free_bytes?}
homeassistant/<sensor|binary_sensor>/<node>/<object>/config   (discovery, retained)
```

All state topics are retained JSON. Numeric fields appear only when the car reported them, so an
unseen value shows as *unknown* in HA, not a phantom `0`; range, rate and odometer are converted to
metric (km, km/h). While a parked car sleeps the source polls pause and MQTT keeps serving the
last-known retained values until the next active window.


Configure brokers in the [user guide](USER_GUIDE.md#home-assistant-mqtt). Runtime ownership and publish failure behavior are in [Architecture](ARCHITECTURE.md#home-assistant-mqtt-bridge).
