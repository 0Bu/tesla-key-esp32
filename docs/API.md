# HTTP and MQTT API

Base `http://<ESP32-IP>`. No auth, no TLS — see [SECURITY](SECURITY.md#http-api-exposure). Mutating
browser requests require a device-owned Host and matching Origin; foreign/rebound origins get `403`.
Headerless POST clients stay compatible. Mutating GETs require matching Origin/Referer or allowed
fetch-site provenance; otherwise send the `X-Requested-With` header or use the POST
alias (`/ota/check`, `/diag`, `/coredump`). Owned hosts are the shared DHCP boot name, the live mDNS
name (bare or `.local`), and the active or connection-local IPv4 address; arbitrary router-added
DNS suffixes are rejected. Diagnostic GETs `/status`, `/diag`, `/heap`, `/coredump`, `/ota/status`
and `/ota/changelog` require such a Host even without browser headers and even with `redact=1`.
The evcc version, vehicle-data and body-controller routes retain their compatibility behavior;
`GET /mcp` returns a fixed 405. The static UI at `/` and `/index.html` rejects iframe embedding.
This does not replace the trusted-LAN boundary.

## Commands

```
POST /api/1/vehicles/{VIN}/command/{command}   Content-Type: application/json
```

The `{VIN}` path segment must exactly match the configured VIN reported by `/status.vin`,
including case (normally uppercase). A different or lowercased VIN returns `404` before command
dispatch; the same rule applies to vehicle-data routes.

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

- **evcc boolean bodies:** `charge_start` and `wake_up` also accept the JSON scalar `true`, and
  `charge_stop` accepts `false` (evcc's generic setter); mismatched booleans and all other
  non-object bodies are `400`.
- **Validation:** supplied integers must be integral and in range, else `400` — nothing is silently
  clamped. Scheduled charging requires an explicit boolean `enable`; omitting it returns `400`
  before dispatch. Other absent REST fields default to percent=80, start_minutes=0 and sentry on=false;
  supplied fields are still validated. MCP has its own required-argument rules.
  Numeric strings must use canonical decimal integer notation (`"16"`); whitespace, plus signs,
  leading zeroes, negative zero, hexadecimal, exponent and fractional string forms are rejected.
  Integral JSON numbers, including `1e1`, retain the same range checks.
- **JSON request bounds:** commands and configuration accept at most 64 values (root, containers
  and scalars; object names do not count), within the existing 2 KiB/depth-16 bounds. Excess values
  return `400` before parser allocation. Duplicate decoded object keys at any depth also return
  `400`; key comparison remains case-sensitive.
- **`set_charging_amps` is verified:** `charging_amps` is required. Success means the firmware sent a
  fresh, serialized `ChargeState` request and the car reported *exactly* the requested current.
  Missing/malformed input is `400`; an unreachable car, rejected command or missing/mismatched
  readback is `502`, so controllers such as evcc retry instead of accepting a false success.
- **Outcome status:** in a complete reply, success, exact `already_set` and the contextual charging
  no-ops below are HTTP `200` with `result:true`. Every other failed command (car refusal, authentication or role fault,
  asleep or unreachable car, timeout, or a device-side refusal such as a full command queue) keeps
  the Tesla-compatible JSON (`result:false` plus the reason) but uses HTTP `502`, not a misleading
  `200`. Intake and out-of-memory failures before dispatch keep their own codes. If the reply
  cannot be built after the attempt, the degraded HTTP `200` reply carries `outcome:"succeeded"`
  only for a successful command, including `already_set` and these no-ops, otherwise
  `result:false` and `outcome:"unknown"` (see
  [Response classes](#response-classes)).
  vehicle-command's proxy maps these differently (200 for car refusals, otherwise 408/412/500/503
  with an error-only body, or a Fleet server error's own status); see
  [ADR-0005](adr/0005-tesla-ble-seam.md#2-platform-forced-departures-and-reference-deviations).

- **Already satisfied charging requests:** after an authenticated current Infotainment rejection,
  `charge_start` accepts exact reasons `is_charging` or `complete`; `charge_stop` accepts
  `not_charging`. The response is successful without another send. Here `complete` means an
  accepted already-finished no-op, not proof that charging started. Inverse pairs, `requested`,
  unrelated commands and uncertain replies remain failures. This local compatibility policy
  changes neither cached charge telemetry nor the existing exact `already_set` behavior.

- **Uncertain execution:** after possible payload submission, an unconfirmed failure reports
  `command outcome unknown; command may have executed`. The firmware does not automatically send
  another mutating command in that case. A caller issuing another request makes a new command;
  check the vehicle state before repeating an uncertain action. Read-only status queries and
  authenticated temporary rejections have bounded retries. Failure HTTP codes and JSON envelopes
  are unchanged; MCP uses the same reason.
- **Wake confirmation:** `wake_up` succeeds on fresh Infotainment evidence, including the already
  awake case. A definite send failure returns its reason immediately. Possible submission keeps
  waiting within the same deadline without resending; if no confirmation arrives, its original
  uncertain reason is retained. Successful local transmission alone reports an uncertain
  confirmation timeout when the deadline expires and does not prove BLE reachability.
  The default firmware budget is 20 s. In evcc's [stock template](https://github.com/evcc-io/evcc/blob/bf5c276cdb395443fda4601b99151c0a962d0db4/templates/definition/vehicle/tesla-ble.yaml),
  `timeout` applies only to reads; setters retain the [10 s client default](https://github.com/evcc-io/evcc/blob/bf5c276cdb395443fda4601b99151c0a962d0db4/util/request/helper.go#L16)
  and can time out before wake confirmation. The [custom vehicle example](examples/evcc-custom-vehicle.yaml)
  sets each HTTP plugin's timeout to 30 s, including charge enable, current and wake setters.
  Its status expression maps `Starting` to connected/not charging (`B`), so evcc can still send
  a stop while charging is starting; mapping it to disconnected (`A`) suppresses that setter.
  Waiting at the [single HTTP worker](ARCHITECTURE.md#http-request-serialization) can still
  exceed this timeout. Check state before repeating an uncertain action, as described above.

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
    "charge_rate": 58.3, "charge_amps": 16, "charge_energy_added": 12.5, "battery_range": 280.5,
    "minutes_to_full_charge": 45 },
  "climate_state": { "is_climate_on": true, "is_preconditioning": true,
    "inside_temp": 21.5, "outside_temp": 5, "driver_temp_setting": 22 } } } }
```

The doubled `response` and `charge_amps` match the Fleet API/TeslaBleHttpProxy shape.
`battery_level` is nominal SOC; `usable_battery_level` matches the Tesla app. Charge fields form a
complete fixed numeric schema, including session `charge_energy_added` in kWh. `/status` and MQTT
may omit unreported fields. Range/rate remain miles/mph here; MQTT uses km/km/h.
Non-finite charge, climate or odometer numbers use the fixed schema's `0` placeholder,
including after unit conversion; this means an unavailable reading, not a measured zero.

Optional `?endpoints=` selects `charge_state`, `climate_state` and/or `drive_state`. Omission selects
charge and climate. Combine selectors with `;`, `,`, `%3B` or `%2C`. The exact query key is required;
empty, unsupported, duplicate or malformed selectors and queries ≥128 bytes return `400`.
TeslaBleHttpProxy instead uses its default for empty selectors, accepts duplicates and returns
`503` for unsupported selectors; this firmware treats them as client errors. Every selected cache
must be available for `200`/`result:true`; otherwise the response is `503`/`result:false` with
`"stale or unavailable"`. Reads never poll or wake the car.

| Selected domain | Availability and units |
|---|---|
| Charge | Valid idle cache may be old. In the active window, data older than 30 s is unavailable. |
| Climate | Valid cache required; inside the active window it must be ≤300 s old. Unreported `is_preconditioning` is `false`; other optional fields use typed zero/false fallbacks. |
| Drive | Valid cache with reported odometer required; last-known odometer is served in miles. Missing odometer returns `503` with `odometer:0`, where the proxy returns `200` with zero; evcc rejects either. |

The active window is a command in the last five minutes, or cached Charging/Starting with live
Infotainment contact <60 s old. Once that contact expires without a recent command, valid
last-known charge/climate data can be served again. Climate refreshes only through in-window
telemetry, restarted at climate on window entry. Missing/stale climate is retried at the next 30 s
slot, with at most two consecutive attempts before another domain. After reboot it stays unavailable
until a poll obtains a sample; the boot window is not seeded after a heap-watchdog restart.
Drive refreshes through the rotation and a best-effort `NO_WAKE_SKIP` wake/bootstrap one-shot.

## Body controller state (foreground VCSEC request)

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
| `GET` or `POST /diag[?verbose=0\|1][&clear=1][&redact=1]` | Plain-text in-memory log. `verbose` changes logging; `clear=1` deletes the log. Export rules below. |
| `GET` or `POST /coredump[?raw=1\|clear=1]` | Private bounded crash image; default requires current-build verification. `raw=1` explicitly exports an admitted artifact without that verification. `clear=1` erases instead; modes cannot be combined. |
| `POST /crash/dismiss` | Acknowledges after confirmed absence or successful erase. Unconfirmed erase returns `200` with `ok:false`, `result:false`, `outcome:"unknown"`; do not retry automatically. |
| `GET /heap` | `{dt, b0, b_boot, unit:"KiB", scale:10, free[], largest[]}` — 24 h memory trend; interpretation below. |
| `POST /gen_keys[?force=1]` | Generate the ECDSA P-256 key (refuses overwrite without `force`, `409`). Identity mutation is Stable-only: `PendingVerify`, unknown OTA state or an active OTA/update returns `503` before anything changes. |
| `POST /set_vin` | `{"vin":"<17-character VIN>"}`; trims and uppercases the value. An unchanged VIN skips persistence and reboot. A changed VIN rotates identity and reboots; same Stable-only `503` rule as `/gen_keys`. |
| `POST /set_mqtt` | `{"broker":"host:port"}` or full `"mqtt://…"`; `""` disables. Changed brokers are tested before persistence; failure saves nothing. |
| `POST /set_syslog` | `{"server":"host:port"}` (a bare host defaults to 514; `""` disables) and reboot. |
| `POST /set_wifi` | `{"ssid":"…","pass":"…"}` with one-shot credential rollback. Both unchanged and saved credentials return the same acceptance text; only a changed pair is written and requests a reboot. curl/`provision.py` only; validation and rollback below. |
| `POST /set_time` | `{"ms":<epoch milliseconds>}` — temporary browser estimate, neither persisted nor authority for new durable dates. After valid body/schema intake, synchronized NTP makes this a successful no-op. |
| `POST /set_ota` | `{"channel":"release"\|"dev"}` — switch the update channel. |
| `GET` or `POST /ota/check[?ms=<epoch>][&pr=<N>]` | Start a background update check (then poll `/ota/status`); `pr=<N>` targets that PR's preview build. |
| `POST /ota/update[?pr=<N>]` | Start the background self-update (downloads, then reboots). |
| `GET /ota/status` | `{state, progress, message, available, update_available, current, channel, pr}` (`pr` only while a PR build is targeted). |
| `GET /ota/changelog` | Release notes for the offered update: `text/plain`, ≤ 1024 bytes, `no-store`; `204` when none. |
| `GET /api/proxy/1/version` | `{version, platform}` (`"ESP32"`/`"ESP32-S3"`/`"ESP32-C3"`/`"ESP32-C6"`). |
| `POST /mcp` | MCP server for AI agents (Streamable HTTP, stateless JSON-RPC 2.0; `GET` → `405`, no SSE): [MCP.md](MCP.md). |

MQTT and Syslog request-body/schema errors are returned before loading stored configuration.
Changed MQTT/Syslog saves reserve the restart owner before writing; an occupied owner returns `409`
without saving. A successful save returns `firmware_verification`: `already_valid`,
`not_in_probation`, `confirmed`, or an explicit unconfirmed/uncertain code. `result:true` acknowledges
saved configuration, not firmware confirmation. The reason explains refused readiness, BLE host or
heap checks and uncertain SDK marking; the automatic restart still follows. Confirmation and
OTA entry share one configured INTERNAL-largest threshold, sampled freshly under their actual
owners. Their allocator phases can differ; the confirmation query does not reclaim retained Operation
or RTOS memory. See the [recovery reference contract](ARCHITECTURE.md#ota-self-update) for the selected
threshold and measured scope. TLS checks use the separate record-capacity screen rather than reserving
a handshake budget. The legacy `critical_heap`
confirmation code now means insufficient OTA recovery headroom, independently of the unchanged 4,096 B
sustained-critical restart policy. Cleanup-unknown resources leave a known Pending image unconfirmed. Unchanged service
acknowledgements omit the field. Setup `/save` never confirms a candidate and displays any known
unconfirmed/uncertain outcome; `provision.py` preserves that warning.

All of `/set_time`, `/set_mqtt`, `/set_syslog`, `/set_wifi` and `/set_ota` return
`409` while an OTA update is downloading or flashing ([ARCHITECTURE](ARCHITECTURE.md#http-request-body-and-allocator-failure-contract)).

`/gen_keys` also returns `409` during a pending VIN transition, `503` when existing-key storage
cannot be verified, and `500` when rotation is incomplete. Not-committed or ambiguous commits
trigger a recovery reboot; a committed key with pending cleanup uses the supervisor retry path.
`/ota/check` and `/ota/update` return `409` while another device operation owns their shared gate,
`503` when the service is unavailable or ordinary-entry recovery memory is insufficient, and `500`
when worker startup fails. A memory refusal uses `not enough memory for update` in both async replies
and the blocking check result, creates no Operation/task and withdraws a stale update offer. `/ota/update` also
returns `409` while the running image is NEW/PENDING_VERIFY, or `503` if its state cannot be
verified. These refusals create no OTA worker. A manifest check remains available during probation.
The web UI preserves the device's refusal reason; a failed start does not trigger polling or retry.
A retired timed-out writer can become idle after its raw handle is quiescent while its one network
context remains quarantined; a new check/update still returns `409` until real cleanup Ack allows
reclamation. This does not add a status field. A deadline, memory failure or terminal network-cleanup
failure withdraws the update offer. Optional release-note failure is best-effort only after clean
connection closure; details and blocking-SDK limits belong to [Architecture](ARCHITECTURE.md#ota-self-update).

New MQTT and Syslog submissions accept at most 64 UTF-8 bytes; an unchanged stored value remains
a no-write acknowledgement. Existing targets up to 120 bytes survive unrelated saves, including
WiFi changes. WiFi acquires the shared restart lease and requires raw VALID or UNDEFINED before
comparing credentials. Busy, pending, NEW, unsupported or unknown image state refuses every valid
pair uniformly with `409`; unchanged admitted WiFi remains write/restart-free. See
[configuration durability](ARCHITECTURE.md#configuration-durability) for older-writer limits.

Before NTP has committed, `/set_time` accepts only finite milliseconds in the interval from
1700000000000 (inclusive) to January 1 of build year + 10 (exclusive). Clock admission can return
`503` when busy or `500` when the clock syscall fails; neither response claims successful application.
`/ota/check?ms=` accepts canonical unsigned integer milliseconds in the same interval, including
when NTP is already synchronized. Malformed supplied `pr` or `ms` returns `400` before any clock
change or OTA start; an absent `pr` selects the configured channel. Busy clock admission returns
`503`, and failed application `500`, all with `started:false` and no check task start. Without `ms`,
or with a valid timestamp after NTP has committed, a normal check needs no browser write.
An updating-entry snapshot returns `409` before timestamp processing or any clock/check effect;
it does not exclude an update starting later. Degraded replies after a possible clock effect follow
the uncertainty contract below.

### Response classes

| Route class | Status and envelope |
|---|---|
| Vehicle command | Wrapped `response`; `200` for success or `already_set`, `502` for a failed command. Intake errors use `400`/`413`/`503`. |
| `vehicle_data` | Wrapped `response`; `200` when all selected caches are usable; unusable required caches return `503`/`result:false`. Invalid URI/query/selectors retain `400`, VIN mismatch `404`. |
| `body_controller_state` | Wrapped `response`; a completed retrieval attempt returns `200`; inspect `response.result` for its outcome. |
| Configuration | Wrapped `response` for time/VIN/MQTT/Syslog/WiFi. Key generation is flat `{result,reason}`. Channel success is flat `{ok,result,channel}`; channel errors are wrapped. Invalid intake is `400`/`413`, unavailable resources `503`, failed persistence `500`. |
| OTA start | Flat `{started,reason?}` for a check and `{result,reason}` for an update; `200` on start; `409` for busy/probation, `503` for unavailable service, insufficient ordinary-entry recovery memory or unknown image state, `500` for worker startup failure. |
| Pre-effect REST OOM fallback | Flat `{result:false,reason:"out of memory"}`, `503`, even for routes whose normal envelope is wrapped. |
| Degraded mutation reply | HTTP `200` with explicit application outcome, as described below; this status alone does not establish success. |

Vehicle commands, `vehicle_data`, `body_controller_state`, `/gen_keys`, `/set_vin`, `/scan` and
`POST /mcp` return `503` before dispatch when vehicle runtime is unavailable, with
`{"error":"vehicle runtime unavailable","retryable":false}`. This also applies to `/mcp` and is
not a JSON-RPC envelope. Before an effect attempt, contained C++ exceptions return `503 text/plain`
(`out of memory`).

REST/MCP commands, configuration saves, browser-clock application, key/VIN transactions and OTA
starts retain a bounded request-local outcome before response work. If response construction fails
after an attempt, a fixed HTTP `200` JSON reply carries `reply_degraded:true` and one of:

| `outcome` | `result` | Meaning |
|---|---|---|
| `succeeded` | `true` | The existing API operation returned success; this does not add proof of physical vehicle execution, pairing or flash completion. |
| `unknown` | `false` | The primary operation's outcome is unconfirmed; effects may have occurred. |
| `applied_recovery_pending` | `false` | A changed identity is known to be saved, but local recovery remains incomplete. |

The metadata is inside `response` for wrapped routes and at root for flat routes. The reason retains
explicit degradation/uncertainty. A degraded check emits `started:true` only for a proven task start;
otherwise `started:null`, even if the same request may already have changed the browser clock.
`recovery_pending:true` also identifies a recorded identity-recovery continuation. Already-decided
restart handling is attempted at most once even if the response fails. Configuration restart
admission occurs before saving; its request-owned lease remains held through reply handling and
restart. Identity-recovery continuations follow their separate transaction rules and never confirm
a pending image. No mutation is retried by this mechanism.

A header or partial-send failure is terminal: no second reply starts. The project fallback renderer
allocates no memory, but IDF networking can fail. An absent acknowledgement cannot establish either
success or failure. Inspect state before another mutation; arbitrary external client retries and
atomic execution/acknowledgement delivery are outside this contract.

Key enrollment runs automatically with the existing Charging Manager key; it has no HTTP command.
See [pairing](USER_GUIDE.md#pairing-and-identity) for the NFC card and vehicle confirmation steps.

### Diagnostic export and heap trend

`/status?redact=1` substitutes `vin`, `ip`, `wifi.ssid`, vehicle/scanned BLE addresses and names,
`mqtt.broker` and `syslog.host` with `"<redacted>"`. All keys remain present for version comparison;
`sys.board_mac` remains visible. `/diag?redact=1` substitutes identifiers per line, retains the board
MAC and discards a wrapped partial first line. Lines over 288 bytes become `"<redacted>"`;
truncated sensitive values are redacted through the line's end. Known network-identifier formats from
netif, WiFi (including block-ack `<ba-add>` AP addresses), TLS DNS and MQTT URI parsing become a complete replacement line. For recognized complete V1 log records, the ring copy flattens embedded CR/LF while retaining
trailing line endings; this also affects raw `/diag`. Serial and Syslog retain the original bytes except known MQTT URI parse errors, which become
a fixed credential-free line before any sink, even if optional ring allocation fails.
Fragmented driver output and other library or serial output may contain identifiers; inspect
either export before sharing.

`/diag?verbose=0` disables raw-RX logging; `X-Diag-Verbose` echoes the setting. Adding `clear=1`
deletes the in-memory log. Preserve evidence before using either log or crash deletion.

`/coredump` streams at most the admitted image length in 256-byte chunks. Default download
requires a checksum-verified current-build identity. `raw=1` is an explicit forensic export of a
bounded image whose identity/checksum may be unverified; it never exports the whole partition.
Use the dump's matching ELF, identified by `last_crash.dump_elf_sha256` when known. The running
build's hash is `elf_sha256`; foreign artifacts can require a different ELF.

```bash
esp-coredump info_corefile -c coredump.bin <matching-build>.elf
```

Only no query, `raw=1` or `clear=1` is accepted; conflicting/unknown modes return `400` before
deletion. A missing or erased artifact returns `404`, unverified default download `409`, query
failure `503`, and concurrent export/deletion `409`. Failed headers/reads/chunks abort the stream
without a successful terminator or a second JSON response. Boot never erases foreign artifacts.

Clear and dismiss share one deletion owner. Both invalidate current-build download/summary before
an erase attempt. Only confirmed absence or successful erase permits acknowledgement; SDK errors,
including a late not-found result, remain unconfirmed. Dismiss hides the report only on success;
clear leaves a fault-reset report visible. Post-effect allocation failure preserves the request's
known/unknown outcome in the degraded flat response. Neither path retries automatically.
The current web UI has no crash delete/dismiss caller or background raw-download action.
[Export privacy](SECURITY.md#syslog-and-diagnostic-export).

`/heap` samples are tenths of a KiB, oldest first, with `null` for empty buckets. A falling `free[]`
suggests a leak; `largest[]` sinking toward the 4 KiB watchdog floor suggests fragmentation. The
`.noinit` DRAM ring survives restarts and is cleared by a power cut; samples before `b_boot` belong
to an earlier run.

### Configuration checks

VIN/MQTT/Syslog/WiFi/channel mutations return `503` when the complete existing configuration
cannot be read; they preserve the unreadable state instead of saving defaults over it.

`/set_mqtt` connects to a changed non-empty broker before saving: refusal/credentials return `400`,
unreachable/no answer `502`, insufficient contiguous memory `503`. Failure neither persists nor
reboots; an empty broker disables MQTT. A failed durable configuration save returns `500`.

`/set_wifi` accepts an empty password for an open network, 8–63 UTF-8 bytes for WPA2, or exactly
64 ASCII hex characters for a raw PSK. The atomic config keeps the previous credentials for one
rollback. A successful lease drops the backup; sustained authentication refusal restores the old
network on the next boot, reboots and exposes `wifi.rolled_back`. An absent SSID gets 180 s before
rollback. [Shared provisioning contract](USER_GUIDE.md#provision-without-rebuilding).

A confirmed changed-credentials commit clears `wifi.rolled_back` before legacy mirroring or the
HTTP reply. Failed saves and unchanged credentials retain the verdict. A busy configuration-restart
gate refuses the request before any save.

### Status fields

| Key | Content |
|---|---|
| `vin`, `ip`, `version` | Device identity. |
| `key_present`, `key_fingerprint`, `key_created` | Key state; bounded `key_created` (epoch) names this key, or is omitted if unknown. New stamps require NTP. |
| `paired`, `paired_at`, `reauth` | `paired` means a stored VCSEC session; `reauth` is an advisory attention hint and may be true while `paired` is true. `paired_at` (epoch) records the first status observation of a stored session, not the Tesla pairing event. New stamps require NTP; existing dates have unknown provenance. Omitted if unknown. |
| `session_available`, `sessions` | `session_available` means a usable VCSEC session in RAM for the current identity, not vehicle reachability or fresh acceptance. `sessions.vcsec` and `sessions.infotainment` expose `{runtime_known, runtime_usable, persistence}`. Persistence is `unknown`, `unconfirmed`, `confirmed` or `error` for the current session observation; an older stored record does not confirm a failed current save. These cache-only fields do not perform storage or BLE I/O. |
| `wifi` | `{ssid, rssi, std, rolled_back?}` — always present, empty while no WiFi link holds the lease; `rolled_back` appears only when the last `/set_wifi` was undone by the credential rollback. |
| `eth` | `{link, speed?, full_duplex}` — present **only** while Ethernet carries the lease (`speed` in Mbit, omitted until the PHY negotiates). Carries no MAC, so it needs no `?redact=1` treatment. |
| `ble` | `{connected, scanning, phase?, phase_s?, rssi?, addr}` when connected, otherwise includes `devices:[{addr,name,rssi,connectable}]`. `connect_fail?`/`car_connectable?` appear only while actively failing. |
| `link` | `"awake"\|"idle"\|"asleep"\|"unreachable"\|"unknown"` — drives the hero (`"idle"` = reachable but not provably asleep, the "Parked" card). `vcsec_sleep` (`AWAKE\|ASLEEP\|UNKNOWN`) is the raw un-debounced flag, for diagnostics. |
| `vehicle` | `{soc, usable_soc, status, charge_limit, power, amps, actual_amps, volts, phases}` — only when `link=="awake"` and the coherent ChargeState sample is at most 30 seconds old; each field only when reported. Other telemetry does not refresh this age. |
| `last`, `last_seen_s`, `charge_age_s` | Last-known `{soc, usable_soc, status}`; seconds since any infotainment contact, and independently since this ChargeState sample. `charge_age_s` is omitted if no valid timestamped charge sample exists. The web UI identifies retained battery values as last known and offers no charge/wake action while awake without a fresh charge reading. |
| `mqtt` | `{configured, connected, tls, broker, error?}`; `broker` is the credential-free `host:port` even if the saved URI has userinfo. |
| `syslog` | `{configured, resolved, reachable, host?, port?, error?, dropped_lines?}`; `reachable` is an advisory ping hint, never a delivery gate. |
| `ota` | `{channel, verification}`; channel is `"release"\|"dev"`. Verification is the cached last observed running-image state: `stable`, `pending_verify` or `unknown`. Status performs no SDK/flash query for this field; it does not identify an earlier rollback. |
| `tele` | `{climate, drive, tires, closures}` read-only telemetry, emitted only while the BLE link is up. |
| `last_reboot` | `"heap:<n>"` — only when the heap watchdog ended the previous boot (`n` = consecutive such restarts); absent on ordinary boots. |
| `pairing_blocked` | Optional cached reason: `key_reload_required`, `key_runtime_mismatch`, `key_storage_unavailable`, `key_cleanup_pending` or `vin_transition_pending`. Absent during normal pairing. Reading it does not probe storage or change identity. |
| `identity_state` | Cached recovery condition: `ok`, `reboot_required`, `cleanup_pending`, `vin_transition`, `storage_unavailable` or `unknown`. `ok` means Ready with no known local recovery block; it does not prove a usable key, session or vehicle acceptance. |
| `sys` | Always present: `{board_mac, free_heap, min_free_heap, largest_block, uptime_s, wifi_reconnects, reset_reason, safe_mode, runtime_state, fatal_component?, diag_dropped_lines?, stack_min_free_quantum_bytes, stack_min_free_bytes:{<context>:number|null}}`. |
| `last_crash` | Reset/report facts plus `artifact_present`, `raw_available`, `query_error`, `foreign_dump`, `checksum_verified`, `dump_identity` (`current`/`foreign`/`unverified`), and `summary_available`. A summary emits numeric `pc` (including zero), optional `task`/hex-string `backtrace`, and `corrupted`/`truncated`/`isr`; `elf_sha256` and `dump_elf_sha256` distinguish running and dump builds. Present for undismissed faults, artifacts or query errors. |

`sys.stack_min_free_quantum_bytes` is 256. `sys.stack_min_free_bytes` always contains the 27
[registered contexts](../main/logic/stack_context.hpp). A never-sampled context is `null`; a number
is a conservative historical lower bound in bytes, rounded down to a multiple of 256. Thus 0
means a real observation in 0–255 bytes, not an unsampled context or proof of an overflow.
15872 is the saturated lower bound for observations at or above that value. The original
`httpd`, `vehicle`, `auto_pair` and `mqtt` names remain; their formerly optional fields now use
explicit nulls. Callback names identify entry contexts: several contexts may share a task,
and one context may aggregate several caller tasks. MQTT event/probe dispatch and the logger
are such aggregates; they do not establish a minimum for one dedicated SDK task. A snapshot is six
independent atomic observations, not a globally simultaneous measurement. These diagnostics
do not admit commands, maintenance or recovery. The supervisor's separate exact minimum
retains its existing semantics.

`sys.runtime_state` is `booting`, `ready`, `safe_mode` or `fatal`. Fatal services remain stopped for this process; a status endpoint exists only if HTTP startup succeeded. `sys.fatal_component` appears only for Fatal and names the first bounded component category (`clock`, `nvs`, `configuration`, `identity`, `bluetooth`, `http`, `network`, `vehicle`, `ota`, `heap_supervisor`, `application` or `unknown`). State and category come from one atomic cached snapshot; these fields do not probe storage/OTA metadata, restart, send a key or contact the vehicle.

`sys.diag_dropped_lines` counts capture attempts lost to an unavailable or busy diagnostic ring;
`syslog.dropped_lines` counts full-queue losses. Both are boot-local unsigned counters modulo 2^32,
omitted at zero. Clearing the ring does not reset the loss counter. Neither proves UDP delivery.

`ble.rssi` appears only when a live-link RSSI sample is available.
`ble.phase` and `phase_s` appear together: `connecting` counts down to giving up, `waiting` to a
retry, and zero means now. Heap fields are INTERNAL-only; PSRAM cannot mask them. The watchdog
acts on `sys.largest_block`. All stack contexts use the nullable conservative boot minima and
256-byte quantum described above; measured zero remains visible. `sys.board_mac` is the physical
eFuse identity.

Crash `backtrace`/`corrupted` exist only on Xtensa (esp32/s3). RISC-V (c3/c6) has no on-device
backtrace; downloaded dumps unwind offline on every target.

`/api/proxy/1/version` appends the fixed `-esp32` suffix on every target. Strip it when
comparing to `/status.version`; `platform` identifies the actual chip.

A synchronous command occupies the shared HTTP worker; concurrent status/UI requests wait behind
it. See [HTTP serialization](ARCHITECTURE.md#http-request-serialization) when setting client timeouts.

## MQTT topics

**Topics** (node id `teslakey_<vin>` from the lowercase VIN, stable across board changes; changing the
configured vehicle intentionally creates a different HA device). Before a valid VIN is configured,
MQTT uses a temporary board-derived node. Setting a VIN replaces that temporary identity after
reboot. Neither this transition nor a later VIN change deletes the old retained topics. Remove the
old Home Assistant device and its retained discovery entries under
`homeassistant/<sensor|binary_sensor>/<old-node>/`, plus retained state/availability under
`tesla-key/<old-node>/`, using your broker administration tool. Adapt these prefixes if configured
otherwise; clear only the obsolete node. Leaving them retained can recreate stale entities.

Topics:

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
tesla-key/<node>/device      {wifi_rssi?,ble_rssi?,ble_connected,paired,session_available,boot_time?,free_heap,
                              version,largest_block,min_free_heap,reset_reason,
                              reset_reason_code,crash_dump,safe_mode,runtime_state,fatal_component?,identity_state,
                              wifi_reconnects,
                              mqtt_reconnects,stack_min_free_quantum_bytes,
                              <context>_stack_min_free_bytes}
homeassistant/<sensor|binary_sensor>/<node>/<object>/config   (discovery, retained)
```

The device topic uses the same context registry, nulls, quantum and saturated lower-bound
semantics as `sys.stack_min_free_bytes`, flattened as `<context>_stack_min_free_bytes`.
The original four flattened field names remain.

The compatibility field `wifi_reconnects` counts network re-establishments after an outage, including
Ethernet; live WiFi/Ethernet handovers do not increment it. Its discovery label is **Network reconnects**;
the field and entity IDs stay stable.

All state topics are retained JSON. Numeric fields appear only when the car reported them, so an
unseen value shows as *unknown* in HA, not a phantom `0`; range, rate and odometer are converted to
metric (km, km/h). While a parked car sleeps the source polls pause and MQTT keeps serving the
last-known retained values until the next active window.
The device payload's `paired` and `session_available` retain their status-field meanings above;
the broker connection's `availability` topic is independent of either session fact. Its recovery fields use the same cached state names as `/status`; `fatal_component` is absent outside Fatal. They describe device recovery, not fresh vehicle authentication. They add no command topic or discovery entity.


Configure brokers in the [user guide](USER_GUIDE.md#home-assistant-mqtt). Runtime ownership and publish failure behavior are in [Architecture](ARCHITECTURE.md#home-assistant-mqtt-bridge).
