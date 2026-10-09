# tesla-key-esp32 — Technical Reference

ESP32 BLE↔HTTP proxy for Tesla vehicles (esp32 / esp32s3 / esp32c3 / esp32c6). It exposes a REST API
on the LAN, API-compatible with [TeslaBleHttpProxy](https://github.com/wimaha/TeslaBleHttpProxy), so
it is a drop-in for the [evcc](https://evcc.io) `tesla-ble` integration. User guide:
[../README.md](../README.md). Internals: [ARCHITECTURE.md](ARCHITECTURE.md); threat model and
signing: [SECURITY.md](SECURITY.md); MCP: [MCP.md](MCP.md); feature catalog:
[FEATURES.md](FEATURES.md).

## Hardware

Exactly the four chips `yoziru/tesla-ble` supports — **esp32, esp32s3, esp32c3, esp32c6** (WiFi
2.4 GHz + BLE). The ESP-IDF Component Manager enforces that list at dependency resolution, so the
supported set cannot drift from the crypto library's. Requirements: **≥ 4 MB flash** (dual-OTA
layout, two ~2 MB app slots), no PSRAM, a USB data cable for the first flash. ESP32-S2 (no
Bluetooth) and ESP32-H2/P4 (no WiFi) cannot run this firmware. The classic **esp32 must be chip
revision v3.0 (ECO3) or newer** (standard since ~2020): the signed image requires it
(`CONFIG_ESP32_REV_MIN_3`), so pre-ECO3 silicon such as the ESP32-PICO-D4 is refused by the web
installer before flashing ([SECURITY](SECURITY.md#signed-ota-images)); the other targets have no such
floor. A chip tesla-ble does not declare (esp32c5, esp32c61) would need upstreaming first — a locally
patched checkout was tried for the C5 and dropped ([ADR-0004](adr/0004-drop-esp32c5-target.md)).

### Wired networking (optional, esp32s3)

The esp32s3 image also drives a **WIZnet W5500 over SPI**, so the device can run on Ethernet —
including **PoE** (one cable for power and network). That matters because BLE range to the car is
signal-limited, so mounting the device where the car is beats any radio tweak. Verified pinouts,
probed in order: **M5Stack AtomS3 Lite on an ATOMIC PoE Base** (802.3af; SCLK 5 / CS 6 / MISO 7 /
MOSI 8) and **Waveshare ESP32-S3-ETH** (SCLK 13 / CS 14 / MISO 12 / MOSI 11); custom builds can
prepend a validated four-pin mapping through Kconfig.

On a wire the WiFi stack is **never started**: WiFi and BLE share one antenna path, so this removes
coexistence entirely and frees ~57 KB of contiguous heap. A wired board needs **no WiFi
credentials** — it takes a DHCP lease and the VIN is set in the web UI. With a controller present
but no link it falls back to WiFi (or the setup portal), and a cable plugged in later takes over.
The **same** esp32s3 image serves a LilyGo T-Dongle-S3, a bare ESP32-S3 and both PoE boards, each
detected at boot; the driver is not compiled into the other targets. Design:
[ARCHITECTURE](ARCHITECTURE.md#wifi--lan-connectivity).

## Flash prebuilt artifacts

Browser flasher + WiFi/VIN setup: [../README.md](../README.md). The flasher is served from GitHub
Pages (repository-vendored, hash-pinned esptool-js / Web Serial); CI publishes it from the single
branch-backed authority `gh-pages:/` (root = **Release** channel, `dev/` = **Dev** channel,
`PR/<N>/` = signed previews; see [SECURITY](SECURITY.md#release-pipeline-and-trust-boundaries)).
Official [GitHub Releases](https://github.com/0Bu/tesla-key-esp32/releases/latest) are cut manually and
carry the same signed binaries; a push to `main` publishes only a Dev build. The installer offers
*Keep configuration* (only for an existing dual-OTA layout) or *Erase everything* (default on a first
install).

Flash by hand (needs `brew install esptool`). The per-target **merged** image bakes in the right
bootloader offset (0x1000 on the classic esp32, 0x0 on s3/c3/c6), so one command works for any chip.
This erases `nvs` (re-enter WiFi/VIN, re-pair once):

```bash
# <suffix>: "" for esp32, else -s3 / -c3 / -c6
esptool --chip <esp32|esp32s3|esp32c3|esp32c6> write_flash 0x0 \
  tesla-key-esp32<suffix>-<version>-merged.bin
```

To preserve `nvs`, use a provenance-matched signed app from the exact Release/main run, or the
repository's `flash-esp32` workflow with an explicit offline `DEV_SIGNING_KEY_FILE`; that path
verifies signature, target and size, writes only the signed app at `0x20000`, then erases `otadata` at
`0xf000` last. **Never flash a local `build/` through `@flash_args`:** local output is unsigned and
crash-loops before `app_main`.

## Build from source

Builds run in the official **ESP-IDF Docker image pinned by tag and immutable digest**.
`esp-idf-toolchain.txt` is the single toolchain contract read by local Docker, CI and Renovate, and the
four `dependencies.lock.<target>` files pin each Component Manager graph. There is no local toolchain
to install; flashing happens from the host with `esptool` because Docker Desktop has no USB
passthrough.

```bash
brew install esptool                                          # host flasher (once)
git clone https://github.com/0Bu/tesla-key-esp32.git && cd tesla-key-esp32

# First run pulls the image and materialises yoziru/tesla-ble (2–4 min); CMake applies the ordered,
# hash-recorded patch series automatically. The wrapper keeps build/ host-owned and caps each
# container at 1.5 CPU / 1800 MiB so other local or k3s workloads keep capacity.
./scripts/idf-docker.sh idf.py set-target esp32s3 build       # or esp32 / esp32c3 / esp32c6

# Complete unsigned four-target CI build + ELF/map/size diagnostics:
./scripts/idf-docker.sh ./scripts/ci-build-all.sh local

# Speed-ups:
IDF_FAST_BUILD=1 ./scripts/idf-docker.sh idf.py build         # container-native build volume (avoids macOS VirtioFS latency)
./scripts/idf-docker.sh daemon start                          # warm container (~0.12 s latency); `daemon stop` when done

./scripts/idf-docker.sh idf.py menuconfig                     # optional: WiFi/VIN/MQTT defaults

# Before pushing firmware changes: the size and stack-usage baselines CI enforces after its full build,
# for all four targets, incrementally (the `pre-push` hook runs exactly this; first run per checkout
# compiles each target cold, ~3 min at the default 1.5-CPU cap, later runs take seconds):
./scripts/precheck-firmware.sh                                # or --target esp32s3 ...; --clean drops the cache
```

**Stop after the build:** `build/tesla-key-esp32.bin` and `@flash_args` are unsigned. For USB delivery
follow the `flash-esp32` workflow with an explicit verified signing key, or use an exact
provenance-matched signed Release/main artifact. WiFi/VIN may stay blank and be set later through the
setup AP. Flash-mode fallback: hold `BOOT`, tap `RESET`, release `BOOT`. Serial log:
`screen <port> 115200` (exit `Ctrl-A` `K`). Boot log:

```
I (500) main: VIN: <VIN>  BLE MAC: (scan)
I (600) main: WiFi connected to 'MyNetwork'
I (650) main: IP: 192.0.2.1
I (700) http_server: HTTP server started on :80
I (700) main: tesla-key-esp32 running. API on port 80.
```

## Provision without rebuilding

Use the device's transactional HTTP path; **never generate and flash a partial NVS image** — it spans
the whole `0x6000` partition and erases the private key, pairing sessions and every omitted setting.
On a first boot join `tesla-key-esp32-setup`, then:

```bash
python3 provision.py --url http://192.168.4.1 --ssid MyNet --vin '<VIN>'
# password is prompted without echo; automation: --password-stdin or a chmod-600 --password-file
```

For an already reachable device use `--url http://tesla-key-esp32.local`, which calls
`POST /set_wifi` with its one-shot rollback. A vehicle identity change is deliberately separate
because it clears the old pairing:

```bash
python3 provision.py --url http://tesla-key-esp32.local --mode lan \
  --vin-only --vin '<VIN>' --confirm-vin-change
```

The retired `--port` path fails closed with a data-loss explanation; for genuine low-level recovery,
make and verify a complete NVS backup before writing at `0x9000`. The WiFi contract is identical in
the setup portal, LAN API and host tool: a 1–32-byte UTF-8 SSID plus an explicitly selected open
network, an 8–63-byte UTF-8 WPA2 passphrase, or exactly 64 ASCII hex characters (raw PSK).
Enterprise authentication is not supported.

## Upgrading

WiFi, VIN, private key and BLE sessions live in the `nvs` partition (`0x9000`, size `0x6000`,
namespaces `tesla_cfg` + `tesla_ble`). The exact persistence contract is
`main/logic/nvs_contract.hpp`; the adapter rejects every unregistered namespace, key or storage API
instead of truncating an unknown name.

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

- **Data kept:** OTA; verified signed app-only USB write at `0x20000` followed by the `otadata`
  activation erase; web installer *Keep configuration* (its four bounded parts do not overlap `nvs`).
- **Data lost:** web installer *Erase everything*, `esptool … write_flash 0x0 …-merged.bin`, or
  `esptool … erase_flash`.

`nvs` offset/size must never change across versions, or old data is stranded.

## Pairing

Mostly automatic, with one manual step at the car. On first boot the device generates an ECDSA P-256
key (stored in NVS, never leaves the device). **A VIN must be configured first:** the device finds
your car by its VIN-derived BLE name, so with no VIN the auto-pair task idles (`no VIN configured —
pairing disabled` in the log) and never connects or enrols — by design, so it can never whitelist a key
onto an arbitrary nearby Tesla. Nearby Teslas are still listed (a periodic listing-only scan; `POST
/scan` also works). Set the VIN in the setup AP or with `POST /set_vin`. Once a plausible 17-char VIN
is set, while unpaired and in BLE range, the task probes the car and sends a whitelist-add. The car
shows the pairing dialog on its **touchscreen** only while a Tesla NFC keycard rests on the
center-console reader; place the card, then confirm on screen within ~45 s. There is no Pair button.

- Key fingerprint = `SHA-1(pubkey)[:4]` (e.g. `0E:8A:1D:BE`), shown in the web UI. The new key appears
  as *"Unknown key"* in the car's key list.
- **Regenerate:** the refresh icon on the **Security key** row, or `POST /gen_keys?force=1` (without
  `force` → `409`). Regenerating un-pairs the vehicle.
- **Manual trigger:** `POST /send_key` → `{"result":true,"role":"charging_manager","reason":"key sent —
  confirm the pairing request on the car's screen"}`.
- Enrolls **Charging Manager** only (charging + wake + read); `/send_key?role=owner` → `403`. A Tesla
  keeps at most ~3 *simultaneous* BLE connections (shared across phone keys and fobs) — that limit,
  not a key count, blocks pairing when full.

Invalidation and re-pair rules: [ARCHITECTURE](ARCHITECTURE.md#pairing-lifecycle--invalidation).

## HTTP API

Base `http://<ESP32-IP>`. No auth, no TLS — see [SECURITY](SECURITY.md#http-api-exposure). Mutating
browser requests from a foreign or DNS-rebound Origin get `403`; Host must be the device name or
current IP, and state-changing legacy GET forms use the same gate. Headerless POST clients (evcc, curl)
stay compatible. Mutating GETs accept matching Origin/Referer or allowed fetch-site provenance;
without that browser provenance, send an explicit custom header (e.g. `X-Requested-With`) or use a
POST alias (`POST /ota/check`, `POST /diag`, `POST /coredump`). This is not a substitute for the trusted-LAN boundary.

### Commands

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
| `set_scheduled_charging` | `{"enable": true, "start_minutes": 1380}` (minutes after local midnight; 1380 = 23:00) |

- **evcc boolean bodies:** `charge_start` also accepts the JSON scalar `true` and `charge_stop`
  `false` (evcc's generic setter); mismatched booleans and all other non-object bodies are `400`.
- **Validation:** supplied integers must be integral and in range, else `400` — nothing is silently
  clamped. Omitted optional fields keep their documented compatibility defaults.
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
> [Security](#security)).

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
    "usable_battery_level": 72, "charge_limit_soc": 80, "charger_power": 11,
    "charge_rate": 58.3, "charge_amps": 16, "charge_energy_added": 12.5,
    "battery_range": 280.5, "minutes_to_full_charge": 45 },
  "climate_state": { "is_climate_on": true, "is_preconditioning": true,
    "inside_temp": 21.5, "outside_temp": 5, "driver_temp_setting": 22 } } } }
```

`?endpoints=drive_state` returns `"drive_state": { "odometer": 12345.6 }` (miles).

The doubled `response` and `charge_amps` match the Fleet API / TeslaBleHttpProxy shape evcc
parses. `charge_energy_added` is session energy in kWh; `battery_range` stays in miles for evcc's
conversion, and `minutes_to_full_charge` stays in minutes. Optional `?endpoints=` selects
`charge_state`, `climate_state` and/or `drive_state`; omission selects `charge_state` and
`climate_state` (the TeslaBleHttpProxy default). Combined selectors accept `;` or `,`, including
`%3B` and `%2C`. The exact query key is required; empty, unsupported, duplicate or malformed
selectors and query strings of 128 bytes or more return HTTP `400`. This is a deliberate departure
from TeslaBleHttpProxy, which answers an unsupported endpoint with `503` / `result:false`, falls
back to its default for an empty `endpoints=` value and accepts duplicate selectors, while this
firmware returns `400` for all three (a malformed request is a client error; evcc never sends
them). The firmware also accepts `,` besides `;`, and a selected `drive_state` without a reported
odometer answers `503` with `odometer: 0`, where the proxy answers `200` with 0 (evcc rejects
`<= 0` either way).

Each selected cache must be available for HTTP `200` / `result:true`; failure of any selected
domain returns HTTP `503`, `result:false`, and reason `"stale or unavailable"`. Idle charge cache
may be old so reads do not wake the car. Within five minutes of a command, or while cached
Charging/Starting has live infotainment contact less than 60 s old, charge cache older than 30 s
is unavailable. After that contact expires without a recent command, valid last-known charge
cache may be served again. Climate needs a valid cache; inside the active window a climate cache
older than 300 s is unavailable (evcc uses `is_preconditioning` to keep charging at minimum
current, so a stale in-window value is refused), outside it the last-known cache is served. An
unreported `is_preconditioning` is emitted as `false` (TeslaBleHttpProxy semantics); the other
optional climate fields keep typed zero/false fallbacks. When the window opens, the telemetry
rotation restarts at climate; it is refreshed only by that rotation, so `climate_state` answers
`503` after a reboot until an in-window rotation reaches an awake car: the boot-seeded window (not
seeded after a heap-watchdog restart) normally fills it, while a car asleep through it stays `503`
until the next command or charging window. Drive data needs a valid cache with a reported
odometer; the last-known value is served because the odometer only grows. It is refreshed by the
in-window telemetry rotation and by the wake/bootstrap one-shot, which also enqueues one
`NO_WAKE_SKIP` drive-state poll. These reads never poll or wake the car.

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

| Endpoint | Purpose |
|---|---|
| `GET /` (`/index.html`) | Web UI (status, pairing, quick commands). |
| `GET /status[?redact=1]` | Full device/vehicle status (fields below). `redact=1` is the **bug-report form**: `vin`, `ip`, `wifi.ssid`, `ble.addr` (and every scanned neighbour's, plus scanned vehicle names), `mqtt.broker` and `syslog.host` read `"<redacted>"`; every key is kept (dropping one would forge an "older build" signal) and `sys.board_mac` stays visible for hardware triage. |
| `POST /scan` | Time-limited BLE discovery scan (fills `ble.devices`). |
| `GET` or `POST /diag[?verbose=0\|1][&clear=1][&redact=1]` | Plain-text in-memory diag log. `verbose=0` turns raw-RX logging back off (`X-Diag-Verbose` echoes the state for the web UI). `redact=1` (bug-report form) substitutes VIN/SSID/IP/vehicle-BLE-MAC/broker/syslog-host **per line**, keeps the board MAC, discards a wrapped partial first line and emits any logical line over 288 bytes only as `"<redacted>"`; it fails closed on a truncated one. |
| `GET` or `POST /coredump[?clear=1]` | Streams the raw crash image (chunked octet-stream; `404` when none, permanently so on a device flashed before the `coredump` partition existed). Decode against the `.elf` of the SAME build (`/status.last_crash.elf_sha256` names it): `esp-coredump info_corefile -c coredump.bin <build>.elf`. `clear=1` erases instead. |
| `POST /crash/dismiss` | Acknowledge and DELETE this boot's crash report (erase first, mark second). POST because it destroys the artifact a bug report needs, so no link or prefetch may reach it. With no `coredump` partition (every OTA-upgraded board) there is nothing to erase and dismissal still succeeds; any other erase error is `500` and leaves the report standing. |
| `GET /heap` | `{dt, b0, b_boot, unit:"KiB", scale:10, free[], largest[]}` — the board's 24 h memory trend in tenths of a KiB, oldest first, `null` for an empty bucket. A leak is a slope; fragmentation is `largest[]` sinking toward the 4 KB watchdog floor. The ring survives restarts (it is `.noinit` DRAM, cleared only by a power cut), so the slope that preceded a heap-watchdog reboot is still there; samples before `b_boot` came from an earlier run. |
| `POST /gen_keys[?force=1]` | Generate the ECDSA P-256 key (refuses overwrite without `force`, `409`). Identity mutation is Stable-only: `PendingVerify`, unknown OTA state or an active OTA/update returns `503` before anything changes. |
| `POST /send_key` | Manually trigger pairing (Charging Manager only; normally automatic). |
| `POST /set_vin` | Persist the VIN, regenerate the key, clear the paired BLE MAC/session and reboot. Same Stable-only `503` rule as `/gen_keys`. |
| `POST /set_mqtt` | `{"broker":"host:port"}` or full `"mqtt://…"`; `""` disables. A changed non-empty broker is **connected to before it is saved**: `400` = broker refused us (credentials), `502` = unreachable/no answer, `503` = too little contiguous memory for the check. Any failure writes nothing and does not reboot. |
| `POST /set_syslog` | `{"server":"host:port"}` (a bare host defaults to 514; `""` disables) and reboot. |
| `POST /set_wifi` | `{"ssid":"…","pass":"…"}` and reboot; an empty `pass` means an open network, else 8–63 UTF-8 bytes or 64 ASCII hex. The previous pair is stashed as a **one-shot rollback** in the same atomic config entry: if the new credentials get a lease the backup is dropped, and if the AP keeps refusing them the next boot restores the old network and reboots onto it, reporting `/status.wifi.rolled_back`. A merely ABSENT SSID gets 180 s before that happens; only sustained authentication refusal counts against the credentials. No web-UI control — a curl/`provision.py` route. |
| `POST /set_time` | `{"ms":<epoch>}` — set the wall clock from the browser (NTP fallback). |
| `POST /set_ota` | `{"channel":"release"\|"dev"}` — switch the update channel. |
| `GET` or `POST /ota/check[?ms=<epoch>][&pr=<N>]` | Start a background update check (then poll `/ota/status`); `pr=<N>` targets that PR's preview build. |
| `POST /ota/update[?pr=<N>]` | Start the background self-update (downloads, then reboots). |
| `GET /ota/status` | `{state, progress, message, available, update_available, current, channel, pr}` (`pr` only while a PR build is targeted). |
| `GET /ota/changelog` | Release notes for the offered update: `text/plain`, ≤ 1024 bytes, `no-store`; `204` when none. |
| `GET /api/proxy/1/version` | `{version, platform}` (`"ESP32"`/`"ESP32-S3"`/`"ESP32-C3"`/`"ESP32-C6"`). |
| `POST /mcp` | MCP server for AI agents (Streamable HTTP, stateless JSON-RPC 2.0; `GET` → `405`, no SSE): [MCP.md](MCP.md). |

All of `POST /send_key`, `/set_time`, `/set_mqtt`, `/set_syslog`, `/set_wifi` and `/set_ota` return
`409` while an OTA update is flashing ([ARCHITECTURE](ARCHITECTURE.md#http-request-body-and-allocator-failure-contract)).

**`GET /status` fields.**

| Key | Content |
|---|---|
| `vin`, `ip`, `version` | Device identity. |
| `key_present`, `key_fingerprint`, `key_created` | Key state; `key_created` (epoch) is omitted while the clock is unsynced. |
| `paired`, `paired_at`, `reauth` | Pairing state; `paired_at` (epoch) omitted if unknown. |
| `wifi` | `{ssid, rssi, std, rolled_back?}` — always present, empty while no WiFi link holds the lease; `rolled_back` appears only when the last `/set_wifi` was undone by the credential rollback. |
| `eth` | `{link, speed?, full_duplex}` — present **only** while Ethernet carries the lease (`speed` in Mbit, omitted until the PHY negotiates). Carries no MAC, so it needs no `?redact=1` treatment. |
| `ble` | `{connected, scanning, phase?, phase_s?, rssi, addr}` when connected, else `{…, devices:[{addr,name,rssi,connectable}]}`; `phase`+`phase_s` come as a pair (`"connecting"` gives up in `phase_s`, `"waiting"` retries in `phase_s`, `0` = right now); `connect_fail?` and `car_connectable?` appear only while actively failing. |
| `link` | `"awake"\|"idle"\|"asleep"\|"unreachable"\|"unknown"` — drives the hero (`"idle"` = reachable but not provably asleep, the "Parked" card). `vcsec_sleep` (`AWAKE\|ASLEEP\|UNKNOWN`) is the raw un-debounced flag, for diagnostics. |
| `vehicle` | `{soc, usable_soc, status, charge_limit, power, amps, actual_amps, volts, phases}` — cached, only when `link=="awake"`, each field only when reported. |
| `last`, `last_seen_s` | Last-known `{soc, usable_soc, status}` for the asleep card; seconds since last contact. |
| `mqtt` | `{configured, connected, tls, broker, error?}`; `broker` is the credential-free `host:port` even if the saved URI has userinfo. |
| `syslog` | `{configured, resolved, reachable, host?, port?, error?}`; `reachable` is an advisory ping hint, never a delivery gate. |
| `ota` | `{channel}` (`"release"\|"dev"`). |
| `tele` | `{climate, drive, tires, closures}` read-only telemetry, emitted only while the BLE link is up. |
| `last_reboot` | `"heap:<n>"` — only when the heap watchdog ended the previous boot (`n` = consecutive such restarts); absent on ordinary boots. |
| `sys` | **Always present**, the block a remote triage reads first: `{board_mac, free_heap, min_free_heap, largest_block, uptime_s, wifi_reconnects, reset_reason, safe_mode, stack_min_free_bytes?:{httpd?,vehicle?,auto_pair?,mqtt?}}`. Heap figures are INTERNAL-only (PSRAM cannot mask them) and `largest_block` is what the heap watchdog acts on; stack values are each task's minimum free bytes this boot, omitted until sampled (a measured zero stays visible); `board_mac` is the physical eFuse identity and stays visible under `?redact=1`. |
| `last_crash` | `{reason, reason_code, fault, coredump, task?, pc?, backtrace?[hex], corrupted?, elf_sha256?}` — only when the boot is notable (a fault reset, or a dump for this build still in flash and not dismissed); its presence is the signal. `backtrace`/`corrupted` exist on Xtensa only (esp32, esp32s3) — ESP-IDF generates no on-device backtrace on RISC-V (esp32c3/c6); the downloaded dump still unwinds offline on every target. |

## evcc Integration

In the evcc UI (**Settings → Vehicles → Add → Custom device**) the fields are flat — no `vehicles:`
wrapper, no list dash; the editor adds them. For a hand-edited `evcc.yaml`, nest the same fields under
`vehicles:` as a list item.

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

evcc calls `GET …/vehicle_data?endpoints=charge_state` and
`GET …/vehicle_data?endpoints=climate_state` (newer templates also read the odometer via
`?endpoints=drive_state`, in miles, scaled to km by evcc),
as well as `POST …/command/{charge_start,charge_stop,set_charging_amps,wake_up}`, reading SOC from
`.response.response.charge_state.battery_level`, current from `…charge_amps`, charged energy from
`…charge_energy_added`, and preconditioning status from `.response.response.climate_state.is_preconditioning`.

## Home Assistant (MQTT)

`main/mqtt_ha.cpp` mirrors every cached reading to MQTT using Home Assistant's
[MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery), so a **Tesla Key**
device with all entities appears automatically — no YAML. **Read-only:** no command topics are
subscribed, so HA cannot control or wake the car. The bridge runs in its own task, independent of
evcc, BLE and pairing. Design (TLS rules, registry, publish sequencing):
[ARCHITECTURE](ARCHITECTURE.md#home-assistant-mqtt-bridge).

**Enable:** set the broker in the web UI (**MQTT** row, pencil icon, `IP:PORT`); it is stored in NVS
(`mqtt_uri`) and applied after the reboot it triggers. Compile-time defaults/credentials live in
`scripts/idf-docker.sh idf.py menuconfig` → *Tesla Key Configuration*:

| Option | Default | Purpose |
|--------|---------|---------|
| `CONFIG_TESLA_MQTT_BROKER_URI` | `""` | Broker (`mqtt://host:port`; empty = disabled). NVS `mqtt_uri` overrides it. |
| `CONFIG_TESLA_MQTT_USERNAME` / `_PASSWORD` | `""` | Broker auth (optional). |
| `CONFIG_TESLA_MQTT_DISCOVERY_PREFIX` | `homeassistant` | HA discovery prefix. |
| `CONFIG_TESLA_MQTT_BASE_TOPIC` | `tesla-key` | State-topic prefix. |
| `CONFIG_TESLA_MQTT_PUBLISH_INTERVAL_S` | `15` | Republish cadence (also publishes on every reconnect). |

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

## Syslog

`main/syslog.cpp` forwards the same output as `GET /diag` — the device's console log — to a UDP
Syslog collector (RFC 5424), best-effort. Useful to watch a pairing/reconnect live, or to keep history
past the in-RAM ring's ~16 KB and past a reboot. It is **cleartext and unredacted**
([SECURITY](SECURITY.md#syslog-and-diagnostic-export)); design: [ARCHITECTURE](ARCHITECTURE.md#syslog-forwarder).

**Enable:** set the server in the web UI (**Syslog** row, pencil icon, `IP:PORT`, e.g.
`192.0.2.1:514`; a bare host defaults to 514) — stored in NVS (`syslog_uri`), applied after the reboot
it triggers; empty disables. Compile-time default: `CONFIG_TESLA_SYSLOG_SERVER` (`""`), overridden by
NVS. Delivery only needs the hostname/IP to resolve (UDP, no ack); `/status.syslog.reachable` is an
advisory ARP/ICMP hint, so a collector behind firewalled ICMP still receives lines with
`reachable:false` shown.

## Private wake capture

`scripts/capture_wake.py http://tesla-key-esp32.local --wake` correlates `/status` transitions with the
live `/diag` ring for a difficult BLE wake diagnosis. By default it substitutes the VIN and long
authenticated-frame hex, writes a new 0600 log in a fresh 0700 temp directory and never overwrites an
existing file. Use `--output /private/path/wake-capture-case.log` for a stable location (inside this
checkout the name must match the ignored `wake-capture-*.log` pattern). Full VIN/frame bytes need the
conspicuous `--include-sensitive` opt-in and must never be attached to a public issue. It makes a
best-effort `verbose=0` request in `finally`, even when the enable response was lost.

## Troubleshooting

Quick symptom table for users: [../README.md](../README.md#troubleshooting). Deeper causes:

**No WiFi** — verify SSID/pass (case-sensitive); open or WPA2-PSK, no enterprise. Join the setup AP and
use its form or `provision.py`; do not flash a generated partial NVS image.

**BLE doesn't find the vehicle** — car within ~10 m and awake; scanning starts after WiFi. The
origin-aware `BLE connect gave up …` line says whether no current advert was seen, the advert was
non-connectable or GATT readiness failed. A diagnostic build compiled with maximum DEBUG also shows
`scanning for Tesla BLE...` → `Tesla '<name>' found: … — connecting` and raw NimBLE/GATT codes; those
per-attempt lines are compile-time absent from the normal INFO build so retries cannot flood syslog.

**Command times out** (`'charge_start' timed out`) — car in deep sleep: `wake_up`, wait 5 s, retry. A
stale BLE session recovers by power-cycling or pressing the ESP32's reset button, which forces a clean
reconnection. Do NOT erase flash for session recovery.

**No pairing prompt** — a VIN must be configured (else `/diag` shows `auto-pair: no VIN configured —
pairing disabled`); a Tesla NFC keycard must be on the center-console reader; car awake and in range;
`key_present: true` in `/status` (else `POST /gen_keys?force=1`); watch for `auto-pair: requesting key
enrolment` in `/diag`; confirm on the touchscreen within ~45 s or `POST /send_key` to retrigger.

**Key rejected** — in the Tesla touchscreen / app (Locks/Keys) delete the old key entry. Regenerate
the keypair via Web UI or `POST /gen_keys?force=1`, then trigger pairing with `POST /send_key` and confirm
on the touchscreen with the NFC keycard. A full chip erase (`esptool --chip <target> -p <port> erase_flash`)
is destructive: it permanently wipes the bootloader, partition table, active/standby firmware, WiFi credentials,
configuration, and vehicle keys. Use it only for an intentional, explicitly authorized factory wipe with an
attested recovery plan; restoring device function requires a full initial flash (bootloader, partitions, and
signed app) as described in [ARCHITECTURE.md](ARCHITECTURE.md#flashing--nvs-safety).

**Serial permission denied (Linux)** — `sudo usermod -aG dialout $USER && newgrp dialout`.

**evcc empty SoC / no current** — `port: 80` set and the car reachable (`/status` →
`ble.connected: true`). Verify the shape (must print a number; `null` → firmware too old, reflash):

```bash
curl ".../api/1/vehicles/<VIN>/vehicle_data?endpoints=charge_state" | jq '.response.response.charge_state.battery_level'
```

## Security

Full threat model, Flash Encryption / Secure Boot: [SECURITY.md](SECURITY.md).

- Charging Manager key only — cannot unlock doors or drive, even with physical access.
- The private key is in NVS, **unencrypted by default** and dumpable over USB on a factory board;
  enable Flash + NVS Encryption (irreversible) if that matters.
- The API has no auth or TLS by design (evcc cannot send credentials): trusted LAN only, never the
  internet. A reverse proxy or VLAN can add access control; the proxy must set its upstream `Host` to
  the device IP or `.local` name and remove `Origin` (or rewrite it to that same authority), because
  the firmware rejects a forwarded public hostname on mutating browser requests. Use the `.local` name
  or current IP, not a router-expanded DHCP FQDN, for browser configuration.

## Internals

| | |
|---|---|
| BLE protocol | Tesla VCSEC + Infotainment (Protobuf over GATT) |
| Service UUID | `00000211-b2d1-43f0-9b88-960cebf8b91e` |
| Encryption | ECDH + AES-GCM (Mbed TLS 4 / PSA Crypto) |
| Signing | ECDSA P-256 (key in NVS) |
| BLE library | [yoziru/tesla-ble](https://github.com/yoziru/tesla-ble) v5.2.0 + ordered repository patch series |
| BLE stack | NimBLE |
| Fragment size | 20 bytes: the firmware neither starts nor accepts an MTU exchange, so the ATT MTU stays at 23 ([ADR-0005 §2](adr/0005-tesla-ble-seam.md#2-platform-forced-departures-and-reference-deviations)); the negotiated-MTU path (MTU − 3, max 244) is unreachable in this build |
| HTTP server | `esp_http_server` :80 |

## License

[GNU Affero General Public License v3.0](../LICENSE) (AGPL-3.0). The firmware statically links
[yoziru/tesla-ble](https://github.com/yoziru/tesla-ble), which is AGPL-3.0, so the combined binary is a
derivative work and must be distributed under the AGPL-3.0 as a whole — including the §13 network-use
clause: this device runs an HTTP API/web UI, so operators who let others interact with it over the
network must offer them the corresponding source for that firmware version.
