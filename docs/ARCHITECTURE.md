# Architecture

This document owns runtime data flow, telemetry, sleep/link state, pairing, OTA, networking and
concurrency. [API](API.md) owns external fields; [Security](SECURITY.md) owns trust boundaries;
[Development](DEVELOPMENT.md) owns build contracts. [FEATURES](FEATURES.md) locates mechanisms.

| Layer | Principal owner | Responsibility |
|---|---|---|
| Transport | `ble_client.*` | NimBLE lifecycle, bounded deferred Link/RX records |
| Orchestration | `vehicle_ctrl.*`, `vehicle_commands.cpp`, `vehicle_pairing.cpp`, `vehicle_telemetry.cpp` | Client/session access, serialized commands, pairing, caches |
| Decisions | `main/logic/` | Hardware-free bounded state machines and validation |
| HTTP/MCP | `http_server.cpp`, `http_*.cpp`, `mcp_server.cpp`, `command_exec.cpp` | Contained routing, validation, cache reads, shared command dispatch |
| Presentation | `main/www/`, `display.cpp`, `led_status.cpp`, `mqtt_ha.cpp` | Cached snapshots; no hidden vehicle wake path |
| Platform | `main.cpp`, `net.cpp`, `ota_update.cpp`, `safe_mode.cpp` | Startup admission, connectivity, update/recovery |

## Repository agent architecture

[AGENTS.md](../AGENTS.md) owns runner policy. `.agents/skills/` defines workflows;
`.agents/subagents.json` defines read-only reviewers; `tools/agent-hooks/` contains lexical guards.
`tools/agent-config/` mutation-tests configuration, ownership and exact reviewed skill content.
The trusted-base `pr-policy` workflow evaluates current-head records without executing PR code;
server-side blocking requires repository rules to require its check.

## Web UI

`main/www/` is inlined as one gzipped page. A vehicle hero and settings card show VIN, key,
network/integrations and firmware. The hero becomes visible after the first status response.
The Firmware version opens channel selection; the download icon checks for updates. Settings are
disabled while OTA work runs; modal saves validate input and fail closed.

`app.js` polls `/status?ms=<now>` every 4 s with `no-store`, and after user actions. Rendering diffs
before writing DOM so animations persist. A failed poll retains the last frame but clears `feedOk`
and stops phase countdown claims. Post-update reboot detection uses a separate 1 s poll.
Request/response polling avoids per-client queued status copies and blocked broadcast sends.

## Read-only telemetry and command correctness

`vehicle_loop` rotates climate, drive, tires and closures at roughly 30 s per domain. All background
requests use `NO_WAKE_SKIP`; command/query ownership, including the VCSEC health probe, pauses them.
Each entry point explicitly chooses foreground/background `ConnectOrigin`.

Synchronous tesla-ble callbacks copy fixed nanopb state into latest-value mailboxes under a short
`portMUX`. After releasing `vehicle_mutex_`, the loop parses strings and publishes caches under
`cache_mutex_`. Callbacks neither allocate nor acquire the cache lock.

A non-empty CarServer request UUID must match the outstanding domain request; late/foreign replies
are dropped. Nominal `already_set` errors make idempotent setpoints succeed through the shared
`command_result.hpp` policy.

`set_charging_amps` owns one complete action → ACK → independent ChargeState readback transaction.
Success requires a newer feedback generation, a present current field and an exact requested-current
match. Clear the previous state before parsing; missing fields cannot inherit old readings.
Two missing/mismatched readbacks exhaust the budget. An ACK alone never means success.

`vehicle_data` is cache-only and non-blocking. Idle readings may remain old; while charging or within
five minutes of a command, ChargeState older than 30 s returns `503`. Optional telemetry fields are
omitted when not reported. `/status.tele` requires a live BLE link; MQTT can retain valid caches.

`logic/wake_poll.hpp` supplies one `NO_WAKE_SKIP` charge poll per self-wake episode. It arms after
debounced ASLEEP→AWAKE, or paired+connected with a stale/empty cache. Failed polls retry with
30–300 s exponential backoff. Once fresh, the episode latch prevents repeated idle polls as the
cache ages. Stable sleep/disconnect resets it; being awake alone never opens the active window.

## OTA self-update

`ota_update.cpp` pulls a fixed HTTPS manifest and the compile-time target suffix, writes the inactive
slot through `esp_https_ota`, verifies chip/version/signature, then reboots. Authentication and
publication are in [Security](SECURITY.md#signed-ota-images).

Manifest intake reserves one bounded body (8192 B). Non-chunked data needs positive bounded
Content-Length and exact completion; chunked data needs non-empty bounded complete transport.
Negative/early-zero reads and truncation fail. Before cJSON, the allocation-free gate requires valid
UTF-8/escapes, ≤16 levels, no U+0000 and one fully consumed document. The root has unique keys and
exactly one string version. Versions use a no-leading-zero numeric core, optional
`[0-9A-Za-z.-]+` suffix and ≤31 bytes; numeric cores compare as digit spans without overflow.
Release body storage before copying version fields.

The downloaded app descriptor is checked before bulk download, so a new manifest cannot smuggle
old bytes. Normal candidates must be newer; an explicitly selected Dev→Release transition permits
a return to production. Dev candidates use increasing `x.y.z-dev.N`; previews use the newest complete
immutable stable Release as `<stable>-PR-<N>`. Preview comparison follows numeric cores, allowing a
later release to advance it. Unparameterized checks return to the selected channel when its core
catches up. ConfigBlob v1 reserved bits store explicit channel choice without breaking downgrade
compatibility; unrelated saves preserve an unset choice. Changelog intake is bounded to 1024 B.

Rollback health uses `logic/health_gate.hpp`: admitted runtime, acknowledged healthy NimBLE host,
proven network lease, INTERNAL largest block ≥4 KiB and 90 s uptime. At 600 s with an expected but
missing lease, leave PendingVerify armed; do not restart for a router outage. Setup mode never reaches
Ready, so even its saved configuration cannot confirm an image.

Timed confirmation and successful user MQTT/Syslog/WiFi commits acquire `HealthCommit` against
OTA/identity/`FaultRestart`, then resample heap. Busy/critical states leave rollback armed.
Identity mutations require Stable (`OtaIdentityMutationGuard`); pending/unknown/check/update returns
`503` before journals or keys change. VIN/key recovery, credential rollback, watchdogs and brownouts
never confirm an image. Already-valid confirmation is a no-op.

The app sits at `0x20000` in two `0x1f0000` slots. Layout and projected/actual signing budgets are
[build contracts](DEVELOPMENT.md#build-and-evidence-contracts), not independently chosen OTA values.
Signed previews and the browser installer share the [publication trust boundary](SECURITY.md#release-pipeline-and-trust-boundaries).

## Pinned tesla-ble and native orchestration

All targets use the manifest-pinned v5.2.0 graph and lexical, hash-checked patch series. The firmware
uses `TeslaBLE::Client` directly; `main/logic/` owns framing, UUID/session tracking and an eight-slot
command runner: VCSEC auth → wake → Infotainment auth → payload, with backoff. Strict 2-byte BE
length framing has bounded buffering and inter-chunk timeout; TX preserves the builder's frame.

Patches 0001–0003 are retired. Current patches trim unused Parental Controls (0004), align session
counter `max(local, reported)` plus epoch/time with `signer.go` (0005), and port crypto to PSA with
response-tag verification against the response's counter (0006).
[ADR-0005](adr/0005-tesla-ble-seam.md) records all departures;
[ADR-0002](adr/0002-idf6-mbedtls4-crypto-seam.md) records wire/key compatibility.

Transactional key rotation exports bounded PEM, rolls back failed persistence and durably journals
cleanup. An armed `key_rotate` or uncertain probe blocks signing/construction. VIN transitions use
`vin_txn` so power loss cannot pair a new VIN with an old key/session.

## On-device display and status LED

The optional T-Dongle-S3 ST7735 uses the shared cache-only `UiSnapshot` and pure `display_model.hpp`
presenter. BOOT rotates landscape 160×80/portrait 80×160 and their flips; `disp_rot` persists and
migrates legacy `disp_flip`. Layout, RSSI bars, scroll and SoC decisions have Python-simulator parity.
Landscape geometry is hardware verified; portrait orientation still needs on-device confirmation.

Auto-detection requires ≥4 of 6 TF-socket pull-up pins high; other S3 boards do not allocate the
~25 KiB framebuffer or drive display pins. SPI is 40 MHz, BOOT GPIO0, backlight active-low.
The optional APA102 (DI40/CI39) uses the same snapshot, shared SoC gradient and latched alerts;
it is heap-free and disabled by default. Neither renderer wakes the car.

## Home Assistant MQTT bridge

`mqtt_ha.cpp` reads caches in its own task and subscribes to no command topics. Configuration is in
[User guide](USER_GUIDE.md#home-assistant-mqtt); topics/fields are in [API](API.md#mqtt-topics).
The lowercase VIN-derived node identity survives board replacement.

An explicit scheme is honored. Without one, credentials select `mqtts://`, otherwise `mqtt://`.
TLS verifies bundled CA roots and never falls back to cleartext; status exposes credential-free
broker/error/TLS state. The discovery registry is the sole owner of entity rows and metadata.
Optional fields remain absent, units convert to metric, and presence-aware templates return literal
`None` for unknown. Boot timestamps need synchronized time; stack minima create no discovery entities.

On reconnect: discovery → online availability → state; periodic snapshots follow. Invalid caches
publish `{}` once to remove retained stale readings, then stay quiet. Sticky JSON ownership prevents
partial retained documents; every build/print/publish failure rearms the entire sequence.
The 60 s watchdog is fed at loop boundaries and each completed publish, never around a stuck call.

## Syslog forwarder

The log hook mirrors console lines into the RAM ring and a fixed 24×256 B forwarding queue.
RFC5424 facility user severity follows E/W/I/D/V (3/4/6/7), skipping ANSI prefixes; unrecognized
lines remain info. Syslog is best-effort and [unredacted](SECURITY.md#syslog-and-diagnostic-export).

The ring lock covers bounded copies and sink snapshot only; invoke sinks after unlock. Chunked
exports bind append count and clear epoch, stop on generation changes, discard a wrapped first line
and redact overlong logical lines as a whole. Configuration changes persist then reboot.

DNS resolve/reprobe is throttled to about 10 s. Delivery requires resolution, not the advisory
ARP/ICMP result. Hard route/link errors re-resolve; transient resource errors keep ordinary cadence.
Log only failure/recovery transitions; reject the forwarder's own `syslog:` lines to prevent loops.
`syslog_start_gate.hpp` publishes the process-lifetime queue only after task creation succeeds;
a scheduled consumer waits until commit, and cancelled unpublished resources unwind safely.

## Heap-exhaustion watchdog (last-resort escalation)

`logic/heap_watchdog.hpp` samples INTERNAL 8-bit largest block at the 30 s heap site. Below 4 KiB
continuously for five minutes arms restart; one healthy sample resets the run. OTA busy clears it.
Read `ota_is_busy()` atomically, never an allocating status copy.

Restart requires durable `reboot_why=heap:<n>` first, followed by fixed-buffer logging and a 300 ms
yield for Syslog. It never confirms PendingVerify. After five consecutive restarts, stop the ladder.
Watchdog boots suppress the ordinary initial active window; only exact NOT_FOUND means ordinary
boot. Malformed/read/erase errors close the ladder conservatively. `/status.last_reboot` retains
cause evidence. Failed persistence/throws cannot authorize an unrecorded restart.

### Why a restart, and not in-place recovery

Subsystem teardown cannot reliably reclaim fragmented live heap while callbacks still own resources.
Use bounded local recovery for transient OOM and the capped, recorded restart only for sustained
failure. The watchdog's recovery operation must not create a vehicle-draining boot loop.

## WiFi / LAN connectivity

`net.cpp` owns both transports; `net_link.hpp` derives the active one from leases, with Ethernet
priority. Both leases may coexist after hot-plug. WiFi RSSI/standard queries require a WiFi lease;
each transport keeps its own gateway-reachability baseline.

### Which transport comes up

ESP32-S3 probes W5500 candidates in configured order. Cache T-Dongle detection before any
W5500 pin drive: its display shares GPIO5 with the first W5500 candidate. Every candidate must audit
all SPI and auxiliary INT/RST pins it touches against **every later board**, not only earlier mappings. After a positive probe, resources remain in one startup record until activation;
failures unwind in reverse order. If uninstall fails, retain resources the driver may still call and
fail boot closed. Active fallback retains the driver for later cable takeover.

No PHY link falls back after 4 s; link-without-lease waits up to three 20 s intervals. A wired
boot never initializes WiFi and does not consume a trial-credential rollback backup. W5500 polling
uses 10 ms, no INT/RST pins; the component is S3-only. Ethernet route priority 128 wins off-subnet
traffic; on-subnet lwIP uses the first registered matching netif, so hot-plug may still use WiFi.
Board mappings are owned by [User guide](USER_GUIDE.md#wired-networking-optional-esp32s3).

### Keeping the link up

Before any successful lease, ten fast WiFi failures allow setup fallback. Once online, event-driven
reconnect continues indefinitely. The 30 s gateway watchdog recovers after two failures only if the
link claims up and that transport's gateway answered at least once. Never-answering ICMP is unusable
evidence. Probe setup errors fail open; missing lease/gateway or unanswered probes are unreachable.
Recovery disconnects WiFi (the event handler reconnects) or restarts Ethernet; it never reboots.

Ping state/semaphores are persistent. Each asynchronous session owns a generation. Timeout requests
stop and waits a second bounded interval; without matching end, quarantine as PendingEnd. No handle
delete/reuse/new generation until its worker ends; stale callbacks cannot complete a replacement.

## Sleep / link-state (the single source of truth)

`VehicleController::link_state()` supplies UI, status and MQTT:

| State | Proof |
|---|---|
| AWAKE | Live Infotainment telemetry younger than 60 s |
| ASLEEP | No live data; VCSEC ASLEEP held about 120 s and signed reachability younger than 150 s |
| IDLE | Reachable within 150 s, without proven sleep or fresh Infotainment data |
| UNREACHABLE | No signed round trip for at least 150 s |

Before first evidence the state is unknown/omitted. Raw VCSEC AWAKE cannot prove AWAKE; raw ASLEEP
requires debounce. UI labels retained readings as last known; unreachable/unknown offers no action.
Active-window polling needs recent command or fresh charging contact; stale cached charging cannot
keep it open. Signed VCSEC health checks outside that window must not open Infotainment or wake.

`ble_phase.hpp` supplies connecting/waiting deadlines together with rounded-up seconds; connecting
wins overlap, and zero means now. The C++ `ble_row.hpp` presenter has shipped-JS parity. Scanning is
not a phase input; only the no-VIN listing scan has no countdown. Local countdowns stop on feed loss.

Failure classification uses adverts observed in the current attempt: no target advert means out of
range, non-connectable means BLE limit, connectable-but-failed means connection failure. Separate
90 s history drives the UI. Background logs report first cause, changes and hourly repeats;
foreground failures always report. Named SCAN_RSP cannot freshen a stale primary connectability bit.
Health/enrollment timeout policies are distinct from foreground command timeouts.

## Pairing lifecycle / invalidation

Pairing requires a plausible 17-character VIN; without it, auto-pair idles and listing-only scans
show nearby cars without connect/enroll. Pairing/keycard operation is in [User guide](USER_GUIDE.md#pairing-and-identity).

`paired` means stored VCSEC session exists. Invalidation regenerates the key and clears session/cache
when: (1) signed UNKNOWN_KEY_ID/INACTIVE_KEY/INVALID_KEY_HANDLE, a whitelist fault, or two consecutive
health-probe authentication failures prove deletion; (2) forced generation; (3) changed VIN, also
forgetting BLE MAC and rebooting. Role-denied user commands never count as revocation. Same VIN is
no-op. Successful NVS existence probes are cached; read errors retry instead of becoming absent.

NimBLE posts fixed LinkUp only. The vehicle task owns GATT-ready generation, materializes the MAC
after unlock and persists it once outside shared locks. Startup MAC strings stay startup-only.
Restore persisted Unix time **before** loading sessions. Upstream compares Unix now against vehicle
clock/uptime and rejects old sessions; an uninitialized 1970 clock could keep them erroneously.
True session reuse across reboot needs upstream epoch/Unix alignment, not a local time workaround.

Durable `key_created`, `paired_at` and MQTT `boot_time` require `clock_is_authoritative()`
(`time_sync.hpp`). Restored `last_time` can reject stale sessions but is not authoritative; SNTP or
explicit browser time synchronization establishes authority. Do not publish restored cache time as
a measured event timestamp.

## HTTP request-body and allocator-failure contract

`read_body_result()` distinguishes empty, oversize, OOM and receive failure (normal cap 2 KiB).

| Case | REST | MCP |
|---|---|---|
| Empty / receive failure / malformed | `400` (persisted-config routes require a body; REST commands accept empty only where the registry declares no argument or the legacy optional boolean) | HTTP 200, `-32700` |
| Oversize (> 2 KiB) | `413` | HTTP `413`, `-32600` |
| Allocation failure (incl. valid JSON whose cJSON tree is null) | `503` | HTTP `503`, `-32603` |
| Nesting > 16 arrays/objects | `400` | HTTP 200, `-32600` (request-complexity limit, not a parse error) |
| Malformed raw UTF-8 in a string (bad/truncated/overlong, UTF-16 surrogate, > U+10FFFF) | `400` | `-32700` |
| Escaped U+0000 (cJSON's NUL-terminated API cannot keep it for exact ID correlation) | `400` | `-32600` |

No rejected input dispatches or persists. `json_syntax.hpp` validates before cJSON; valid supported
syntax followed by a null parse tree is OOM, not bad input. Copy bounded arguments and release body
and parse tree before BLE/probe/NVS/restart. Config order is load → probe → durable save → respond →
restart; failure prevents later steps. Empty MQTT/Syslog values intentionally disable.
The separate captive `POST /save` has a fixed 1024 B limit and returns 400 before parsing on empty,
oversize or receive failure.

Sticky `JsonBuilder` failure discards the entire tree. `json_http_reply` sets no success status or
sends bytes before printing succeeds; OOM sets 503 before exactly one fixed fallback.
Pinned-cJSON allocation matrices verify these production seams, not the device scheduler/allocator.

While downloading/flashing, send_key/time/MQTT/Syslog/WiFi/channel mutations return 409 before body
intake. This is an entry snapshot, not atomic exclusion. A check holds the shared OTA gate but does
not trip 409: config can save with reboot postponed, while key/VIN mutations return 503. Crash
clear/dismiss routes are outside this guard. External fields/errors are in [API](API.md).

## MCP endpoint (/mcp)

[MCP](MCP.md) owns the wire contract. REST and MCP share `command_registry.hpp` bounds/names/kinds
and `command_exec.cpp` dispatch. MCP requires arguments; REST retains only declared compatibility
defaults. Both reject fractional/out-of-range integers before casts.

Release input trees after fixed ID/argument capture, before blocking work or response construction.
Static registry strings use cJSON references; sticky ownership prevents partial envelopes, and print
OOM maps to 503. The production tool/state builders are covered by pinned-cJSON allocation tests.
Every handler enters through contained `handle_all` and `GuardedReq`.

## Concurrency (normative contract)

New code must fit this contract or revise it with implementation and evidence in the same change.

### Lock hierarchy (`VehicleController`)

| Primitive | Kind | Protects |
|---|---|---|
| `command_mutex_` | mutex, RAII | one whole command/query transaction and command FIFO generation; for `set_charging_amps`, the action and the verifying ChargeState poll are one transaction |
| `vehicle_mutex_` | mutex, RAII (`SemGuard`) | **every** call into `client_` and command-runner state (payload build/dispatch, `drive_command_runner_`, `process_rx_frame_`) |
| `cache_mutex_` | mutex, RAII, leaf | the `last_known_*` caches (`std::string` members ⇒ an unlocked copy is torn-read UB) |
| `result_mutex_` | mutex, RAII, leaf | the externally visible `last_error_` snapshot HTTP/MCP read after a foreground command |
| `CommandCompletion::sem` | per-request binary semaphore, shared ownership | signals one request-local fixed completion record; a timed-out callback cannot address stack storage or a later request's semaphore |
| `telemetry_pending_mux_` | `portMUX`, innermost, POD only | the fixed latest-value nanopb mailboxes, pending mask and charging-amps feedback generation written synchronously by tesla-ble callbacks |

Order: `command_mutex_` → `vehicle_mutex_`. Cache/result locks are independent leaves.
The pending mux is innermost and permits bounded POD copies only: no semaphore, allocation, logging,
parsing or callbacks. Use `tk::SemGuard`/`MutexGuard` and `InFlightGuard` for unwind-safe ownership.

- Hold Vehicle lock only for Client/runner access, never across completion-semaphore wait.
- Cache snapshots are copied without calls out; parse/publish after releasing Vehicle lock.
- `clear_session_and_cache_()` acquires Vehicle internally; do not call it while holding that lock.
- Request-local shared completion survives timeout; generation invalidation prevents late completion
  of another request. Callback writes fixed fields and signals even after catch-all, without logging.
- Command task publishes result text after waiting. `cmd_in_flight_` is set under command ownership;
  it pauses background work but orders no other fields.

### Task inventory

Priorities come only from `task_config.hpp`; stacks remain at creation sites.

| Task | Priority | Stack | Created in | Purpose |
|---|---|---|---|---|
| `vehicle_loop` | `kPrioVehicleLoop` = 5 | 8192 | `vehicle_ctrl.cpp` (fn: `vehicle_telemetry.cpp`) | drain fixed NimBLE Link/RX events, drive `drive_command_runner_()`, parse deferred telemetry after unlock, rotating NO_WAKE poll, sleep gating, BLE-fault link reset |
| `captive_dns` | `kPrioCaptiveDns` = 5 | 4096 | `provisioning.cpp` | captive-portal DNS (setup-AP mode only; vehicle stack not running) |
| `ota` | `kPrioOta` = 5 | 8192 | `ota_update.cpp` | OTA download + flash (transient) |
| `ota_chk` | `kPrioOtaCheck` = 5 | 8192 | `ota_update.cpp` | OTA manifest check (transient) |
| `auto_pair` | `kPrioAutoPair` = 4 | 8192 | `vehicle_ctrl.cpp` (fn: `vehicle_pairing.cpp`) | pairing supervisor: enrol / re-pair / health probe |
| `net_wd` | `kPrioWifiWatchdog` = 4 | 3072 | `net.cpp` | ghost-link watchdog (forces the active transport to re-establish, never reboots) |
| `mqtt_pub` | `kPrioMqttPub` = 4 | 6144 | `mqtt_ha.cpp` | MQTT/HA publisher (reads the caches) |
| `display` | `kPrioDisplay` = 3 | 6144 | `display.cpp` | ST7735 renderer (`CONFIG_TESLA_DISPLAY_ENABLED` builds) |
| `ota_gate` | `kPrioOtaGate` = 3 | 3072 | `main.cpp` | one-shot OTA rollback health gate (polls every 5 s; commits on proven link + INTERNAL largest block ≥ 4 KiB past 90 s under the shared owner, gives up at 600 s) |
| `safe_gate` | `kPrioOtaGate` = 3 | 2560 | `safe_mode.cpp` | one-shot healthy-window timer: after 30 s under the full normal workload it durably clears the crash-boot counter; latched safe mode never creates it |
| `syslog_task` | `kPrioSyslog` = 3 | 6144 | `syslog.cpp` | best-effort UDP Syslog forwarder (opt-in; degraded-not-fatal on a failed start) |
| `led` | `kPrioLed` = 2 | 3072 | `led_status.cpp` | APA102 status LED (`CONFIG_TESLA_LED_ENABLED` builds) |

Vehicle/auto-pair creation uses Creating→Running/Cancelled with acknowledgement. Before release,
entries only wait one tick; on second-create failure the first self-deletes, never external deletion
across cores. Both then wait for global runtime admission; TWDT ownership unwinds before self-delete.

IDF owns NimBLE/httpd/timer/network tasks. NimBLE Link/RX adapters copy bounded records into a static
queue; the vehicle task parses them and publishes ready state. Remaining lifecycle callbacks use
zero-wait mutexes and fail/drop/retry rather than block the host.

### Live stack-headroom evidence

`stack_watch.cpp` samples minimum free bytes from each owning httpd/vehicle/auto-pair/MQTT task.
Never-started tasks are omitted; real zero remains visible in status and MQTT. Retrospective minima
and compiler frame baselines prove different things. Bench policy floors are described in the
[test guide](../test/README.md#whats-not-covered-by-design).

### Atomics doctrine

Use atomics for independent cross-task scalars; multi-field snapshots and strings need a mutex.
`volatile` creates no happens-before edge. Transport, diagnostic, BLE and MQTT scalar flags follow
this rule; flags do not make surrounding mutable state safe.

### Exception containment (normative)

Exceptions cannot escape C/RTOS frames. Long-running tasks catch at each iteration, preserve RAII
invariants, delay and continue. One-shot OTA jobs publish terminal error. Callbacks catch locally or
have a mechanical fixed-buffer/C/atomic audit and return valid API results. Every throwing boundary
has terminal `catch (...)`; standard-exception detail may precede it. Catching does not prevent a
blocked callback, so host/timer locks remain zero-wait. `app_main` contains fatal startup failures.

`test_runtime_boundary_contract.py` derives tasks/callbacks from shipped sources and actual
registrations, rejecting hidden or runtime-selected boundaries. Host tests prove containment
structure and deterministic seams; the four-target IDF build proves C-frame integration.

### Startup failure policy (normative)

`runtime_admission.hpp` permits Booting→Ready only after all essential services and both task
allocations succeed. SafeMode/Fatal are terminal. Vehicle-active HTTP/MCP and commands fail 503
before Ready; OTA health cannot spend rollback on partial boot. NimBLE admission also requires
bounded host-sync acknowledgement; timeout stays terminal against late callbacks. Any post-admission
host reset or loss of sync blocks confirmation.

Essential: NVS, controller primitives/tasks, network primitives/watchdog, NimBLE, primary HTTP
registration and OTA gate task. Fatal PendingVerify marks invalid and reboots to the prior slot;
fatal stable boot halts with diagnostics rather than looping. Optional MQTT/Syslog/display/LED
contain start failures, unwind partial resources and report disabled status.

### Boot-loop safe mode

Four consecutive fault boots latch recovery mode: networking, web UI and OTA remain, while BLE,
vehicle, auto-pair and MQTT stay down. NVS read/write/malformed-value/exception failures enter safe
mode conservatively. The 30 s healthy counter-clear timer runs only under the full normal workload,
never while latched; recovery uptime is not proof the faulty subsystem works. A non-fault intentional
configuration/OTA/power/reset reboot clears the sequence. `boot_guard.hpp` and `safe_mode.cpp` own
this policy; preserving OTA rollback alone cannot recover bad shared NVS configuration.

### Deferred: owned BLE-ops queue

A single owner queue could replace current command/Vehicle-lock coordination, but adds static cost
and migration risk. Revisit when a new command class or another queue-position incident defeats
current arbitration; it is not implemented behavior.
