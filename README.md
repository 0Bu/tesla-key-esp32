# tesla-key-esp32 — Technical Reference

ESP32 BLE↔HTTP proxy for Tesla vehicles (runs on esp32 / esp32s3 / esp32c3 / esp32c6).
Exposes a REST API on the LAN, API-compatible with
[TeslaBleHttpProxy](https://github.com/wimaha/TeslaBleHttpProxy); drop-in for the
[evcc](https://evcc.io) `tesla-ble` integration. User guide: [../README.md](../README.md).

## Hardware

Exactly the four chips yoziru/tesla-ble supports — **esp32, esp32s3, esp32c3, esp32c6** (all
WiFi 2.4 GHz + BLE). The ESP-IDF Component Manager enforces that list at dependency resolution,
so the supported set cannot drift from what the crypto library declares. **≥ 4 MB flash**
(dual-OTA layout: two ~2 MB app slots; a larger flash just leaves the top unused). No PSRAM
required. ESP32-S2 (no Bluetooth) and ESP32-H2 / P4 (no WiFi) cannot run this firmware. A chip
tesla-ble does not declare — esp32c5, esp32c61 — would need upstreaming there first; carrying a
locally patched checkout of the crypto library instead was tried for the C5 and dropped
([adr/0004](adr/0004-drop-esp32c5-target.md)). USB data cable for flashing.

### Wired networking (optional, esp32s3)

The esp32s3 image also drives a **WIZnet W5500 over SPI**, so the device can run on Ethernet —
including **PoE**, which means one cable for power and network and therefore free placement:
this firmware's BLE range to the car is signal-limited, so being able to mount the device where
the car is beats every radio tweak. Verified on an **M5Stack AtomS3 Lite on an ATOMIC PoE Base**
(802.3af, 5 V/1.2 A; SCLK 5 / CS 6 / MISO 7 / MOSI 8).

On a wire the WiFi stack is **never started**: WiFi and BLE share one antenna path, so this
removes radio coexistence entirely (and frees the ~57 KB of contiguous heap the stack holds).
A wired board needs **no WiFi credentials at all** — it comes up on DHCP and the VIN is set in
the web UI. With a controller present but no link, it falls back to WiFi (or the setup portal)
rather than stranding itself, and a cable plugged in later still takes over.

The **same** `esp32s3` image serves a LilyGo T-Dongle-S3, a bare ESP32-S3 and this board — each
is detected at boot. There is no separate build, and nothing to enable on the other three
targets (where the driver is not compiled in).

## Flash prebuilt artifacts

Browser flasher + WiFi/VIN setup: [../README.md](../README.md). The flasher is served on
GitHub Pages (repository-vendored, hash-pinned esptool-js / Web Serial). CI publishes the root and
signed `PR/<N>/` previews through the repository's single branch-backed authority,
`gh-pages:/`; it does not use a Pages Actions artifact/deploy path. Every publishing workflow
validates that Pages API mode/source before signing or branch mutation. Each firmware change also publishes a
[GitHub release](https://github.com/0Bu/tesla-key-esp32/releases/latest) with the same bins.

Flash by hand (needs `brew install esptool`). Use the per-target **merged** image — it bakes
in the correct bootloader offset (0x1000 on the classic esp32, 0x0 on s3/c3/c6), so one command
works for any chip. This erases `nvs` (re-enter WiFi/VIN, re-pair once):
```bash
# <suffix>: "" for esp32, else -s3 / -c3 / -c6 (so "esp32" appears once in the name)
esptool --chip <esp32|esp32s3|esp32c3|esp32c6> write_flash 0x0 \
  tesla-key-esp32<suffix>-<version>-merged.bin
```
To preserve `nvs`, use a provenance-matched signed app from the exact Release/main run, or the
repository's `flash-esp32` workflow with an explicit offline `DEV_SIGNING_KEY_FILE`. That path
verifies signature, target and size, writes only the signed app at `0x20000`, then erases
`otadata` at `0xf000` last. Never flash a local `build/` through `@flash_args`: local build output
is unsigned and crash-loops before `app_main`.

## Build from source

Builds run in the official **ESP-IDF Docker image pinned by both tag and immutable digest**.
`esp-idf-toolchain.txt` is the single toolchain contract read by local Docker, CI and Renovate;
the four committed `dependencies.lock.<target>` files pin each Component Manager graph. There is
no local toolchain to install. Flashing is done from the host with `esptool`, because Docker
Desktop has no USB passthrough.

```bash
brew install esptool                                          # host flasher (once)
git clone https://github.com/0Bu/tesla-key-esp32.git && cd tesla-key-esp32

# Build via the CI-pinned ESP-IDF image (first run pulls it, then materialises
# yoziru/tesla-ble — 2–4 min). CMake applies the repository's ordered, hash-recorded
# patch series automatically. The wrapper keeps build/ host-owned. Pick your chip:
./scripts/idf-docker.sh idf.py set-target esp32s3 build   # or esp32 / esp32c3 / esp32c6

# Or reproduce the complete unsigned four-target CI build + ELF/map/size diagnostics:
./scripts/idf-docker.sh ./scripts/ci-build-all.sh local

# Optional: WiFi SSID/pass + VIN (BLE MAC auto) — interactive
./scripts/idf-docker.sh idf.py menuconfig

# STOP after the build: build/tesla-key-esp32.bin and @flash_args are unsigned.
# For USB delivery, follow the flash-esp32 workflow with an explicit verified signing key,
# or use an exact provenance-matched signed Release/main artifact.
```

WiFi/VIN may be left blank and set later via the setup AP. Flash-mode fallback: hold `BOOT`,
tap `RESET`, release `BOOT`, then flash. Serial log: `screen <port> 115200` (exit `Ctrl-A` `K`).

Boot log:
```
I (500) main: VIN: <VIN>  BLE MAC: (scan)
I (600) main: WiFi connected to 'MyNetwork'
I (650) main: IP: 192.0.2.1
I (700) http_server: HTTP server started on :80
I (700) main: tesla-key-esp32 running. API on port 80.
```

## Provision without rebuilding

Use the device's transactional HTTP path; never generate and flash a partial NVS image. Such an
image spans the complete `0x6000` partition and erases the vehicle private key, pairing sessions and
every omitted setting. On a first boot, join `tesla-key-esp32-setup`, then run:

```bash
python3 provision.py --url http://192.168.4.1 --ssid MyNet --vin '<VIN>'
# password is prompted without echo; automation: --password-stdin or a chmod-600 --password-file
```

For an already reachable device, the same command with
`--url http://tesla-key-esp32.local` uses `POST /set_wifi` and its one-shot rollback. A vehicle
identity change is deliberately separate because it clears the old pairing:

```bash
python3 provision.py --url http://tesla-key-esp32.local --mode lan \
  --vin-only --vin '<VIN>' --confirm-vin-change
```

The retired `--port` path fails closed with a data-loss explanation. For genuine low-level recovery,
make and verify a complete NVS backup before writing anything at `0x9000`.

The WiFi contract is the same in the setup portal, LAN API and host tool: a 1–32-byte UTF-8 SSID,
plus either an explicitly selected open network, an 8–63-byte UTF-8 WPA2 passphrase, or exactly 64
ASCII hexadecimal characters for a raw PSK. Enterprise authentication is not supported.

## Upgrading

WiFi, VIN, private key and BLE sessions live in the `nvs` partition (`0x9000`, namespaces
`tesla_cfg` + `tesla_ble`).

The exact persistence contract is `main/logic/nvs_contract.hpp`; the adapter rejects every
unregistered namespace, key or storage API instead of truncating an unknown name. Records and
their independent owners/retention are:

| Namespace / logical key (stored key where different) | Owner | Retention |
|---|---|---|
| `tesla_cfg/cfg` | HTTP/provisioning atomic config | durable across OTA |
| `tesla_cfg/wifi_ssid`, `wifi_pass`, `vin`, `mqtt_uri`, `syslog_uri` | legacy config mirror | downgrade compatibility |
| `tesla_cfg/last_time` | clock | replaceable cache |
| `tesla_cfg/vin_txn` | VIN transition | recovery journal |
| `tesla_cfg/ble_mac` | BLE discovery | replaceable cache |
| `tesla_cfg/reboot_why` | heap watchdog | recovery journal |
| `tesla_cfg/boot_fails` | boot guard | recovery journal |
| `tesla_cfg/disp_rot` | display | durable across OTA |
| `tesla_cfg/disp_flip` | display | read-only legacy migration |
| `tesla_ble/private_key` | pinned tesla-ble library | durable across OTA |
| `tesla_ble/session_vcsec` (`sess_vcsec`), `session_infotainment` (`sess_info`) | pinned tesla-ble library | replaceable session cache |
| `tesla_ble/paired_at` | pairing | replaceable metadata |
| `tesla_ble/key_created` | pairing | durable across OTA |
| `tesla_ble/key_rotate` | key rotation | recovery journal |

- OTA, or verified signed app-only USB at `0x20000` followed by the `otadata` activation erase:
  `nvs` untouched → data kept.
- Web installer **keep configuration** mode: the four bounded parts do not overlap `nvs` → data kept.
- Web installer factory reset, `esptool … write_flash 0x0 …-merged.bin`, or
  `esptool … erase_flash`: `nvs` erased/overwritten → data lost.

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

Base: `http://<ESP32-IP>`. No auth, no TLS — see [SECURITY.md](SECURITY.md). Mutating browser
requests from a foreign or DNS-rebound Origin are rejected with `403`; Host must be the device
name/current IP, and state-changing legacy GET forms use the same gate. Headerless evcc/curl
clients remain compatible, so this is not a replacement for the trusted-LAN boundary.

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

For evcc compatibility, `charge_start` also accepts the JSON scalar `true` and
`charge_stop` accepts `false`. These are the exact bodies emitted by evcc's generic
boolean setter; mismatched booleans and all other non-object bodies remain HTTP 400.

Supplied integer arguments must be integral and inside the listed range; REST returns HTTP 400
instead of silently clamping to a different command value. Optional omitted fields retain their
documented compatibility defaults.

`charging_amps` is required and must be a whole number. A successful `set_charging_amps`
response means more than a Tesla command acknowledgement: the firmware performs a new,
serialized `ChargeState` request and verifies that the car reports the exact requested
limit. Missing/malformed input returns HTTP 400; an unreachable car, a rejected command,
or a missing/mismatched current readback returns HTTP 502 so controllers such as evcc
retry instead of accepting a false success.

All command failures retain the Tesla-compatible JSON response (`result:false` plus the
vehicle/proxy reason) but use HTTP 502 rather than a misleading HTTP 200.

> A **Charging-Manager** key may only run charging actions + wake. The car therefore **rejects**
> `door_lock` / `door_unlock`, `flash_lights` / `honk_horn`, `set_sentry_mode`, and
> `auto_conditioning_start` / `auto_conditioning_stop` with an authentication failure — these are
> accepted by the API for completeness but never execute. Only `charge_start` / `charge_stop`,
> `set_charging_amps`, `set_charge_limit`, `set_scheduled_charging`, `charge_port_door_open` /
> `charge_port_door_close` and `wake_up` actually run (see [Security](#security)).

```json
{ "response": { "result": true, "command": "charge_start",
  "vin": "<VIN>", "reason": "command executed successfully" } }
```

### Vehicle data

```
GET /api/1/vehicles/{VIN}/vehicle_data
```
```json
{ "response": { "result": true, "vin": "<VIN>", "reason": "success",
  "response": {
  "charge_state": { "charging_state": "Charging", "battery_level": 72,
    "charge_limit_soc": 80, "charger_power": 11, "charge_rate": 58.3,
    "charge_amps": 16, "battery_range": 280.5 } } } }
```
Doubled `response` and `charge_amps` are intentional — they match the Fleet API /
TeslaBleHttpProxy shape evcc parses. While the car is idle, the cache may remain available
so polling does not wake it. While charging or within five minutes of a command, a
`ChargeState` older than 30 seconds is rejected with HTTP 503 and reason
`"stale or unavailable"` instead of being presented as live telemetry.

### Body controller state (no wake)

```
GET /api/1/vehicles/{VIN}/body_controller_state
```
```json
{ "response": { "result": true, "vin": "<VIN>", "data": {
  "vehicle_lock_state": "LOCKED", "vehicle_sleep_status": "ASLEEP",
  "user_presence": "NOT_PRESENT" }, "reason": "success" } }
```

### Management

```
GET  /                     Web UI (status, pairing, quick commands; alias /index.html)
GET  /status               { vin, ip, version, key_present, key_fingerprint,
                             key_created (epoch, omitted if clock unsynced), paired,
                             paired_at (epoch, omitted if unknown), reauth,
                             wifi:{ssid,rssi,std,rolled_back? (only when the last /set_wifi was
                               undone by the credential rollback — presence is the signal)}
                               (always present, empty while no WiFi link holds the lease),
                             eth:{link,speed? (Mbit; omitted until the PHY negotiates),
                                  full_duplex} — present ONLY while an Ethernet link carries
                               the lease, so its presence is the signal that this board is on a
                               wire. Carries no MAC: nothing in it identifies the reporter,
                               which is why it needs no ?redact=1 treatment,
                             ble:{connected,scanning,
                                  phase?,phase_s? (BLE phase countdown, both or neither:
                                    "connecting" = an attempt is running and gives up in phase_s,
                                    "waiting" = the next attempt starts in phase_s; 0 = right now),
                                  rssi,addr
                                  | devices:[{addr,name,rssi,connectable}],
                                  connect_fail?,car_connectable? (only while actively failing)},
                             link: "awake"|"idle"|"asleep"|"unreachable"|"unknown" (drives the
                               hero; "idle" = reachable but not provably asleep — the "Parked" card),
                             vcsec_sleep: "AWAKE"|"ASLEEP"|"UNKNOWN" (raw un-debounced flag, diagnostics),
                             vehicle:{soc,status,charge_limit,power,amps,actual_amps,volts,phases}
                               (only when link=="awake", cached; each field only when reported),
                             mqtt:{configured,connected,tls,broker,error?} (HA bridge;
                               broker is credential-free host:port even when the saved URI
                               contains userinfo),
                             syslog:{configured,resolved,reachable,host?,port?,error?}
                               (UDP diag-log forwarder; reachable is an advisory ping hint,
                               never a delivery gate),
                             tele:{climate,drive,tires,closures} (read-only telemetry;
                               emitted only while the BLE link is up),
                             last:{soc,status} (last-known snapshot for the asleep card),
                             last_seen_s (seconds since last contact),
                             last_reboot: "heap:<n>" (only when the heap watchdog ended the
                                          previous boot, n = consecutive such restarts;
                                          absent on any ordinary boot),
                             sys:{board_mac,free_heap,min_free_heap,largest_block,uptime_s,
                                  wifi_reconnects,reset_reason,safe_mode,
                                  stack_min_free_bytes?:{httpd?,vehicle?,auto_pair?,mqtt?}}
                               (the stack values are each task's historical minimum free bytes
                                for this boot and are omitted until that task has sampled; a
                                genuine measured zero remains visible),
                               sys itself is ALWAYS present —
                               the block a remote triage reads first; the heap figures are
                               INTERNAL-only, so PSRAM on a board variant cannot mask them, and
                               largest_block is the number the heap watchdog acts on;
                               board_mac is the physical eFuse identity and remains visible
                               in ?redact=1 diagnostics),
                             last_crash:{reason,reason_code,fault,coredump,task?,pc?,
                                         backtrace?[hex strings],corrupted?,elf_sha256?}
                               (only when the boot is NOTABLE — a fault reset, or a dump for
                                this build still in flash and not dismissed; its presence
                                is the signal. backtrace/corrupted are XTENSA-ONLY —
                                esp32 and esp32s3 — because ESP-IDF generates no on-device
                                backtrace on RISC-V (esp32c3/c6); the downloaded dump
                                still unwinds offline on every target) }
GET  /status?redact=1      The BUG-REPORT form of the same payload: vin, ip, wifi.ssid,
                             ble.addr (and every scanned neighbour's), mqtt.broker and
                             syslog.host read "<redacted>". The KEY is always kept —
                             omitting a field would forge an "older build" signal;
                             sys.board_mac deliberately remains visible for hardware triage
POST /scan                 Time-limited BLE discovery scan (populates ble.devices)
GET  /diag[?verbose=0|1][?clear=1][?redact=1]   Plain-text in-memory diag log (verbose=0 turns
                             raw-RX logging back off; the X-Diag-Verbose response header echoes
                             the current verbose state for the web UI; redact=1 is the
                             bug-report form — it substitutes VIN/SSID/IP/vehicle-BLE-MAC/
                             broker/syslog-host per LINE, retains Board MAC, and fails closed
                             on a truncated one; a wrapped partial first line is discarded and
                             any logical line over 288 bytes is emitted only as "<redacted>")
GET  /coredump[?clear=1]   Stream the raw crash image (chunked octet-stream; 404 when there is
                             none, and permanently so on a device flashed before the coredump
                             partition existed). Decode it offline against the .elf of the
                             SAME build — /status.last_crash.elf_sha256 identifies which:
                             `esp-coredump info_corefile -c coredump.bin <build>.elf`.
                             clear=1 erases the partition instead of streaming
POST /crash/dismiss        Acknowledge and DELETE this boot's crash report (erase first, mark
                             second). POST, not GET: it destroys the one artifact a bug report
                             needs, so no link or prefetch may reach it. On a device with no
                             `coredump` partition (every OTA-upgraded board — a partition table
                             is not part of an OTA image) there is nothing to erase and the
                             dismissal still succeeds, clearing the fault-reset report; any
                             other erase error is a 500 and leaves the report standing
GET  /heap                 { dt, b0, b_boot, unit:"KiB", scale:10, free[], largest[] } — the
                             board's own 24 h memory trend in tenths of a KiB, oldest sample
                             first, null for a bucket with no sample. Answers what a spot value
                             cannot: a leak is a slope, fragmentation is the two series
                             separating as largest[] sinks toward the 4 KB watchdog floor.
                             The ring survives a restart (it is .noinit DRAM, cleared only by a
                             power cut), so the slope that PRECEDED a heap-watchdog reboot is
                             still there afterwards; b_boot is the bucket this boot began in,
                             so any sample before it came from an earlier run
POST /gen_keys[?force=1]   Generate ECDSA P-256 key (refuses overwrite without force).
                             Identity mutation is Stable-only: PendingVerify, unknown OTA state
                             or an active OTA/update returns 503 before any key is changed
POST /send_key             Manually trigger pairing (charging_manager only; normally automatic)
POST /set_vin              Persist VIN and reboot
                             Identity mutation is Stable-only: PendingVerify, unknown OTA state
                             or an active OTA/update returns 503 before the VIN journal starts
POST /set_mqtt             Verify the MQTT broker, then persist it and reboot
                             ({"broker":"host:port"} or full "mqtt://…"; "" disables MQTT).
                             A changed, non-empty broker is CONNECTED to before it is saved:
                             400 = the broker refused us (credentials), 502 = unreachable or
                             no answer, 503 = too little contiguous memory to run the check.
                             In every failing case nothing is written and nothing reboots
POST /set_syslog           Persist the UDP Syslog server for the diag log and reboot
                             ({"server":"host:port"}; a bare host defaults to port 514;
                             "" disables Syslog)
POST /set_wifi             Change the WiFi credentials over the LAN and reboot
                             ({"ssid":"…","pass":"…"}; an empty pass means an open network;
                             otherwise 8–63 UTF-8 bytes or exactly 64 ASCII hex for a raw PSK).
                             The previous pair is stashed as a ONE-SHOT rollback backup in the
                             same atomic config entry: if the new credentials get a lease the
                             backup is dropped, and if the AP keeps refusing them the next boot
                             restores the old network and reboots onto it, reporting
                             /status.wifi.rolled_back. An SSID that is merely ABSENT (a router
                             still rebooting) is given 180 s before that happens — only a
                             sustained authentication refusal is treated as evidence against
                             the credentials. No web-UI control yet; this is a curl route
POST /set_time             Set the wall clock from the browser ({"ms":<epoch>}) — NTP fallback
GET  /ota/check[?ms=<epoch>]   Start a background update check (then poll /ota/status)
POST /ota/update           Start the background self-update (downloads, then reboots)
GET  /ota/status           Poll OTA progress { state, progress, message, available,
                             update_available, current }
GET  /api/proxy/1/version  { version, platform } (firmware version + running chip: "ESP32"/"ESP32-S3"/"ESP32-C3"/"ESP32-C6")
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
vin: <VIN>
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

**Enable:** set the broker in the web UI (Connections → MQTT, `IP:PORT`) — stored in NVS
(`mqtt_uri`) and applied after the reboot it triggers. Compile-time defaults / credentials
live in `scripts/idf-docker.sh idf.py menuconfig` → *Tesla Key Configuration*:

| Option | Default | Purpose |
|--------|---------|---------|
| `CONFIG_TESLA_MQTT_BROKER_URI` | `""` | Broker (`mqtt://host:port`; empty = disabled). NVS `mqtt_uri` overrides it. |
| `CONFIG_TESLA_MQTT_USERNAME` / `_PASSWORD` | `""` | Broker auth (optional). |
| `CONFIG_TESLA_MQTT_DISCOVERY_PREFIX` | `homeassistant` | HA discovery prefix. |
| `CONFIG_TESLA_MQTT_BASE_TOPIC` | `tesla-key` | State-topic prefix. |
| `CONFIG_TESLA_MQTT_PUBLISH_INTERVAL_S` | `15` | Republish cadence (also publishes on every reconnect). |

**Topics** (node id `teslakey_<vin>` from the lowercase VIN, stable across ESP32 board changes;
changing the configured vehicle intentionally creates a different HA device):

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
tesla-key/<node>/device      {wifi_rssi?,ble_rssi?,ble_connected,paired,boot_time?,free_heap,
                              version,largest_block,min_free_heap,reset_reason,
                              reset_reason_code,crash_dump,safe_mode,wifi_reconnects,
                              mqtt_reconnects,httpd_stack_min_free_bytes?,
                              vehicle_stack_min_free_bytes?,auto_pair_stack_min_free_bytes?,
                              mqtt_stack_min_free_bytes?}
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
`192.0.2.1:514`; a bare host defaults to port 514) — stored in NVS (`syslog_uri`) and
applied after the reboot it triggers. Leave empty to disable. Compile-time default:
`CONFIG_TESLA_SYSLOG_SERVER` (`""`), overridden by the NVS value.

Delivery only requires the hostname/IP to resolve (best-effort UDP — there is no
handshake/ack); `/status.syslog.reachable` is an advisory ARP/ICMP ping hint only, not a
delivery gate, so a collector behind a firewalled-ICMP host still receives lines with
`reachable:false` shown in the UI.

## Private wake capture

`scripts/capture_wake.py http://tesla-key-esp32.local --wake` correlates `/status` transitions
with the live `/diag` ring for a difficult BLE wake diagnosis. By default it substitutes the VIN
and long authenticated-frame hex, writes a new 0600 log in a fresh 0700 temporary directory and
never overwrites an existing file. Use `--output /private/path/wake-capture-case.log` only when you
need a stable location; inside this checkout the name must match the ignored
`wake-capture-*.log` pattern. Full VIN/frame bytes require the conspicuous
`--include-sensitive` opt-in and must not be attached to a public issue. The tool makes a
best-effort `verbose=0` request in `finally`, including when the enable response was lost.

## Troubleshooting

**No WiFi** — verify SSID/pass (case-sensitive); open or WPA2-PSK, no enterprise. Join the setup AP and use
its form or the safe HTTP-only `provision.py`; do not flash a generated partial NVS image.

**BLE doesn't find vehicle** — car within ~10 m, awake; scanning starts after WiFi.
Production log: the origin-aware `BLE connect gave up …` line names whether no current advert was
seen, the advert was non-connectable, or GATT readiness failed. A diagnostic build compiled with
maximum DEBUG additionally shows `scanning for Tesla BLE...` → `Tesla '<name>' found: … — connecting`
and the raw NimBLE/GATT status codes; those per-attempt details are compile-time absent from the
normal INFO build so automatic retries cannot flood syslog.

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
  expose to the internet. Front with a reverse proxy or VLAN if access control is needed. A
  reverse proxy must set its upstream `Host` to the device IP or `.local` name and remove
  `Origin` (or rewrite it to the same device authority); the firmware rejects a forwarded public
  proxy hostname on mutating browser requests. Likewise, use the `.local` name or current IP for
  browser configuration rather than a router-expanded DHCP FQDN.

## Internals

| | |
|---|---|
| BLE protocol | Tesla VCSEC + Infotainment (Protobuf over GATT) |
| Service UUID | `00000211-b2d1-43f0-9b88-960cebf8b91e` |
| Encryption | ECDH + AES-GCM (mbedTLS) |
| Signing | ECDSA P-256 (key in NVS) |
| BLE library | [yoziru/tesla-ble](https://github.com/yoziru/tesla-ble) v5.1.1 + ordered repository patch series (including anti-replay) |
| BLE stack | NimBLE |
| Fragment size | Negotiated ATT MTU − 3 (20-byte safe default until MTU exchange; max 244) |
| HTTP server | `esp_http_server` :80 |

## License

[GNU Affero General Public License v3.0](../LICENSE) (AGPL-3.0).

The firmware compiles in [yoziru/tesla-ble](https://github.com/yoziru/tesla-ble), which is
licensed under **AGPL-3.0**. Because that library is statically linked into a single firmware
image, the resulting binary is a combined/derivative work and must be distributed under the
AGPL-3.0 as a whole — its copyleft propagates to this project, including the §13 network-use
clause (this device runs an HTTP API / web UI, so operators who let others interact with it
over the network must offer them the corresponding source for that firmware version).
