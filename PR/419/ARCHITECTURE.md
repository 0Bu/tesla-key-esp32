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
Native `.codex/agents/*.toml`, `.codex/config.toml` and `.codex/hooks.json` are generated adapters;
`sync-codex.mjs --check` verifies parity. Hook trust and primary-checkout loading are separate from
adapter correctness; reviewer defaults can inherit parent runtime overrides ([Security](SECURITY.md#development-tooling-trust-boundary)).
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

Climate chips show the last reported overheat-protection setting and explicitly cached defrost
state. `/status` has no climate-specific age; other telemetry cannot establish their current
activity. Charger input fields do not establish climate power consumption or an AC phase multiplier.

## Read-only telemetry and command correctness

`vehicle_loop` attempts one climate, drive, tires or closures slot roughly every 30 s, rotating
domains with bounded missing-climate retries. Windowed telemetry uses `NO_WAKE_SKIP`;
the supervisor's non-waking VCSEC health/enrollment requests use `NoWakeFail`.
Command/query ownership, including the VCSEC health probe, pauses windowed telemetry.
Each entry point explicitly chooses foreground/background `ConnectOrigin`.

Synchronous tesla-ble callbacks copy fixed nanopb state into latest-value mailboxes under a short
`portMUX`. After releasing `vehicle_mutex_`, the loop parses strings and publishes caches under
`cache_mutex_`. Callbacks neither allocate nor acquire the cache lock.

A non-empty CarServer request UUID must match the outstanding domain request; late/foreign replies
are dropped. The existing exact `already_set` compatibility rule is shared through
`command_result.hpp`. Each queued request separately defaults to that rule alone. Only actual
charge-start/stop producers select contextual charging no-ops: an authenticated current
Infotainment ERROR with counter evidence may normalize a matching reason once, before the runner
completion callback. REST/MCP consume that completion; they do not broadly reinterpret nominal
errors. [API](API.md#commands) owns the accepted reason pairs and their meaning. No new retry,
telemetry update or late-response admission follows from normalization.

`set_charging_amps` owns one complete action → ACK → independent ChargeState readback transaction.
Success requires a newer feedback generation, a present current field and an exact requested-current
match. Clear the previous state before parsing; missing fields cannot inherit old readings.
Two missing/mismatched readbacks exhaust the budget. An ACK alone never means success.

`vehicle_data` is cache-only and non-blocking; [API](API.md#vehicle-data) owns selectors, units,
typed fallbacks and availability. Charge and climate use the active-window decision (recent command
or live Charging/Starting contact <60 s old); charge expires at 30 s, climate at 300 s inside that
window. Climate reads copy fixed POD plus generation/ticks under `cache_mutex_`, then evaluate
after unlock. Drive reads copy fixed POD without a generation or freshness timestamp; unrelated
strings are never copied. All selected domains must be usable for success.
`/status.tele` requires a live BLE link; MQTT can retain valid caches.

`logic/wake_poll.hpp` supplies one `NO_WAKE_SKIP` charge poll per self-wake episode. It arms after
debounced ASLEEP→AWAKE, or paired+connected with a stale/empty cache. Failed polls retry with
30–300 s exponential backoff. Once fresh, the episode latch prevents repeated idle polls as the
cache ages. Stable sleep/disconnect resets it; awake alone never opens the active window.
A wake/bootstrap fire also enqueues one Drive State Poll; background entries expire after 10 s,
so a slow charge round trip may leave the odometer last-known. The 10 s in-window charge cadence
adds no drive poll. `logic/telemetry_poll.hpp` schedules one in-window telemetry attempt per 30 s,
normally rotating climate → drive → tires → closures and restarting at climate on window entry.
Missing/stale climate gets at most two consecutive attempts before the next other domain; priority
retries preserve the normal rotation's place. Queue refusal retains the pending domain and cadence.
All attempts use `NO_WAKE_SKIP`; post-boot cache availability is described in the API.

## OTA self-update

`ota_update.cpp` coordinates one complete check or install. It owns a dynamic bounded operation
containing the device-operation lease, absolute budget, one mailbox and the raw inactive-slot
writer. A separate `ota_net` task receives only its Job: copied URL, TLS context and one 1024-byte
mailbox block. It cannot write OTA/NVS, publish status, restart or release the owner. The existing
pinned ESP-TLS and `http_parser` components provide transport and incremental HTTP parsing;
`esp_http_client` and `esp_https_ota` are not used by this OTA path. Authentication/publication
remain in [Security](SECURITY.md#signed-ota-images).

Capture the operation origin before lock/job/task/URL/body allocation. Manifest, optional changelog,
image, redirects, TLS retry conditions and trickled bytes share a single nonrenewable budget:
15 s setup + ceil(maximum bytes / 16 KiB/s) + 10 s finalization. Metadata permits two bodies of
8192 bytes; install adds at most the fixed inactive slot size. A declared image size can only
shorten the original budget. The coordinator checks it before and after raw writer effects.
DNS, TLS read/write/destroy and synchronous flash/verify/boot-selector calls are not preemptible
here: this is a network/effect-admission deadline, not a proved hard wall-time limit for an
already-entered SDK operation.

On timeout the coordinator retires the generation, aborts its own raw handle and releases the
operation lease only after writer quiescence. Busy then becomes false, but a stalled network
context remains occupied and charged. Every successor refuses until the old worker's real cleanup
Ack and the coordinator's final access permit reclamation. There is no replacement worker,
foreign TLS close, forced task deletion or reuse of the old Job. Ack is the worker's final Job
access; only global own-stack sampling and self-delete follow. Ack does not prove RTOS stack/TCB
reclamation. Uncertain writer cleanup or committed/uncertain boot selection retains the lease
and parks after a returning restart instead of granting another effect.

Job retirement and connection-close flags remain full-word acquire/release read-modify-writes.
Lock-free targets use `fetch_or`; non-lock-free targets reuse the existing strong CAS primitive,
with acquire observation/failure and acquire/release success. Every retry recomputes the OR from
its observed word, preserving phase and other flags. Already-set bits still perform a real RMW.

Confirmation and OTA entry share the selected INTERNAL-largest threshold **59392 B** from
`logic/ota_recovery_memory.hpp`. Their allocator phases are not identical. OTA entry may reclaim a
clean previous Operation before sampling, before new Operation, task-stack, body or TLS allocations.
Confirmation's read-only reference query leaves clean retained Operation and RTOS task/TCB memory
charged. Each compares a fresh real sample under its actual owner. Neither adds old bytes, a reserve
or a heap delta to make a sample pass. This conservative operating-point policy is not a measured
necessary minimum or universal TLS guarantee; final non-injected bench, build and delivery acceptance
remain separate.

The ordinary S3 cold-workload series completed three manifest transfers with a naturally reached
59392-B operation-entry sample after the same HTTP/UI setup that exposed the old 61440-B refusal.
Their TLS pre-context samples were 32768 B and post-context samples were at least 31744 B, above
the separate 18445-B record screen. The following 103-s quiet interval retained 59392 B at all
sampled ordinary points with increasing uptime and no observed crash. This selects a demonstrated
operating point instead of refusing that recovered state; it does not identify a universal minimum
or prove a TLS allocation margin between samples. Image writing, OTA probation and the physical
allocator histories of the other three targets remain separate, unexercised boundaries of this
series. It does not replace their build or delivery checks.

The earlier S3 fixed-INTERNAL/TLS1.2 AS series performed three manifest-only checks on the private
calibration profile. Post-reclaim operation-entry samples were 61440 B; one ordinary track sample
was 57344 B and would wait/refuse under the selected policy. All sampled ordinary points in the
final 104-s interval were 61440 B; this is not continuous instantaneous coverage. TLS pre/post policy
samples were 31744 B, one header checkpoint 27648 B and a TLS status spot 25600 B; the boot-wide
INTERNAL free-heap low-water reached 10308 B. The earlier AO series separately observed a 24576-B
TLS status spot and 9540-B free-heap low-water. These discrete measures reveal no complete transient
contiguous minimum and do not form an additive allocation ledger. The private profile held 4096 B
and used simultaneous real record probes; that profile is not final non-injected delivery. These
manifest-only observations establish no notes, redirect, image-write/probation or other-target
physical allocator history.

Every TLS leg still samples real INTERNAL largest before context creation and after that allocation,
before connect. These later phases use a separate conservative single-record capacity screen:
max(16384+2048+13, 4096+2048+13) = 18445 B for the pinned fixed INTERNAL TLS1.2 profile. Target static
assertions bind the actual content lengths and reject allocator/buffer/protocol-profile drift. This
screen is not the ordinary-entry requirement, a complete handshake budget or a reservation; other
allocations and fragmentation can still refuse the actual SDK attempt. MQTT retains its separate
existing 48 KiB policy. Null TLS context, bounded body allocation refusal and explicit raw-OTA
NoMemory results retain a memory outcome, including prefix/payload writes and final verification;
each effect is invoked once. A write failure aborts its live handle, while final verification
consumes the handle even on failure. Arbitrary TLS failures are not relabeled OOM. Optional
changelog network/protocol failure is best-effort only after actual Closed. A terminal or uncertain
cleanup failure, OOM or deadline withdraws the offer; failed cleanup keeps the slot quarantined. The sample reserves no memory and is not a measured safe
allocation floor or a guarantee that the subsequent TLS handshake succeeds.

Fixed `ota_mem` INFO lines in the existing bounded diagnostic ring identify the operation by
its start time and generation. They record INTERNAL free bytes and largest contiguous block before
operation allocation, after operation/task/body setup, at both TLS guards, after connect, at
`headers_observed`, after worker-owned destroy and after known cleanup/reclaim eligibility. TLS legs
are sequential; `manifest_ready`, `notes_ready` and `image_ready` identify the requested resource,
and each `tls_pre_init` begins the next leg, including redirects, within that resource. Missing or
dropped lines make that series incomplete. Guard lines preserve
the exact largest-block sample used by that guard; the separately sampled free value is not an
atomic pair or a reservation. `headers_observed` follows the parser's actual completed-header status
transition and can include the first body/write chunk processed in that same feed. Destroy lines
precede the worker's final publication; no Job access is added after Ack. A stalled/failed cleanup
has no invented post-cleanup reference. `reclaim_ready` precedes deletion of the old operation;
it is not fully reclaimed idle and does not prove RTOS task reclamation.
Sampling/log failures are contained. These finite checkpoints support a named bench TLS series;
they do not measure a universal peak or establish a safe allocation floor.

The incremental parser caps total headers at 8192 bytes, 64 fields, 64 field-name bytes, 1023
field-value bytes, chunk metadata at 65536 bytes and chunk lines at 256 bytes; it permits at most
two interim responses and ten redirects. URLs/locations use 1024-byte storage, and metadata bodies
remain bounded as below. Repeated redirects/retries never renew the original deadline. The image
may use EOF framing only because complete image/signature verification remains mandatory.

Manifest intake reserves one bounded body (8192 B). Non-chunked data needs positive bounded
Content-Length and exact completion; chunked data needs non-empty bounded complete transport.
Negative/early-zero reads and truncation fail. Before cJSON, the allocation-free gate requires valid
UTF-8/escapes, ≤16 levels, no U+0000 and one fully consumed document. The root has unique keys and
exactly one string version. Versions use a no-leading-zero numeric core, optional
`[0-9A-Za-z.-]+` suffix and ≤31 bytes; numeric cores compare as digit spans without overflow.
Release body storage before copying version fields.

Before bulk download, the app descriptor must equal the manifest version and satisfy
`logic/ota_contract.hpp` channel/PR eligibility. Ordinary stable updates must be newer; explicit
exceptions allow targeted same-core PR previews, same-core transitions to Dev and return from Dev
or PR/pre-release to Release (including an older stable core from Dev). Dev candidates use increasing `x.y.z-dev.N`; previews use the newest complete
immutable stable Release as `<stable>-PR-<N>`. Preview comparison follows numeric cores, allowing a
later release to advance it. Unparameterized checks return to the selected channel when its core
catches up. ConfigBlob v1 reserved bits store explicit channel choice without breaking downgrade
compatibility; unrelated saves preserve an unset choice. Changelog intake is bounded to 1024 B.

Rollback health uses `logic/health_gate.hpp`: admitted runtime, acknowledged healthy NimBLE host,
proven network lease, the shared ordinary-entry INTERNAL-largest requirement and 90 s uptime. At 600 s with an expected but
missing lease, leave PendingVerify armed; do not restart for a router outage. Setup mode never reaches
Ready, so even its saved configuration cannot confirm an image.

Timed confirmation owns `HealthCommit`. Configuration handlers instead acquire a request-owned
`OtaConfigRestartGuard` before saving and keep its `ConfigRestart` owner through response, outer
catch and the once-only restart. Changed MQTT/Syslog commits borrow this same lease to confirm only
raw PendingVerify after Ready, current NimBLE sync/reset and fresh INTERNAL heap checks. They report
an unconfirmed or uncertain result explicitly; an SDK mark error can follow a durable metadata write.
WiFi refuses uniformly before credential comparison while verification is pending or unknown;
Stable no-ops remain write/restart-free. Setup owns the same save/restart lease but never confirms.
The normal blank-otadata first boot with the pinned no-factory bootloader initializes VALID only
if its metadata write succeeds; a USB app write alone proves neither that state nor the installed
bootloader. Raw UNDEFINED is non-probation, not a confirmation by this request. Query errors fail closed.
Identity mutations require Stable (`OtaIdentityMutationGuard`); pending/unknown/check/update returns
`503` before journals or keys change. VIN/key recovery, credential rollback, watchdogs and brownouts
never confirm an image. Already-valid confirmation is a no-op.

Both confirmation paths use `ota_runtime_health()` and the ordered pure sampler in
`logic/health_gate.hpp`: Ready, current host sync, sticky zero-reset evidence, then fresh INTERNAL
largest block. The timed path keeps its initial advisory sample, then validates the ordinary reference and samples
again after acquiring `HealthCommit`; an earlier observation never authorizes the mark. Only an
actually held owner can read the old Operation pointer: acquire coordinator_done before coordinator
fields, require real worker Ack/writer quiescence and matching slot ownership. An unowned
`ota_runtime_health()` never reads that pointer. A clean retained object stays charged; the query does
not release/delete it. Cleanup-unknown resources refuse confirmation even when a heap sample is high.
ConfigRestart returns its already-known Pending state without a mark in that case. These sequential reads are not an atomic snapshot with the SDK call.
A returned SDK error refreshes the diagnostic cache before fallible logging and stops the timed
task without retry, reporting the code and uncertain outcome. An SDK exception retains the existing
exception path and does not promise a cache refresh or unchanged metadata.

Update admission queries the running image state while holding the same operation gate, before
creating a worker or starting HTTPS. NEW/PENDING_VERIFY refuses with `409`; unknown/error state
refuses with `503`. A manifest-only check can still run during probation. Busy owners return
`409`, service initialization or ordinary-entry memory refusals `503`, and worker startup failures
`500`. A typed entry-memory result preserves the fixed memory reason for blocking checks and both
HTTP starters, withdraws a stale offer with the requested target under its owner, and creates no worker. Worker-creation
failures attempt to publish their terminal error before releasing the owner, so a delayed
publication cannot overwrite a later worker's status.
`/status.ota.verification` reads an atomic last-observed cache only. Startup seeds it before HTTP;
update admission and health/config confirmation refresh it under their owner. This diagnostic
cache neither authorizes an update nor records whether an earlier firmware rolled back.

The app sits at `0x20000` in two `0x1f0000` slots. Layout and projected/actual signing budgets are
[build contracts](DEVELOPMENT.md#build-and-evidence-contracts), not independently chosen OTA values.
Signed previews and the browser installer share the [publication trust boundary](SECURITY.md#release-pipeline-and-trust-boundaries).

### Flashing & NVS safety

Normal OTA writes the installed inactive slot and otadata; verified app-only USB writes the signed
app at `0x20000`, then erases otadata last. Both preserve bootloader, partitions and secret NVS
`0x9000/0x6000`; neither migrates the layout. The browser installer's **Flash** also keeps NVS but
rewrites the bootloader, partition table, app and otadata; [NVS retention](SECURITY.md#nvs-retention)
owns when it is offered.

Whole-chip erase destroys all of them. Recovery needs an explicitly authorized complete verified
write set, network setup and new key enrollment:

| Part | Offset |
|---|---|
| Bootloader | `0x1000` esp32; `0x0` s3/c3/c6 |
| Partition table | `0x8000` |
| Erased initial otadata | `0xf000` |
| Signed app | `0x20000` |

The installer validates all four release-bound manifest parts before writing. Releases publish
signed apps and merged images, not separate layout parts; manual full recovery must verify every
part's source. Merged writes can overwrite NVS and require approval for that loss. App-only recovery
or an unsigned build cannot replace a full write set. [Artifact trust](SECURITY.md#ota-self-update).

## Pinned tesla-ble and native orchestration

NimBLE SMP/link-layer pairing and bonding are compiled out on all four targets
(`CONFIG_BT_NIMBLE_SECURITY_ENABLE=n`); protection is Tesla application crypto and NFC/VCSEC key
enrollment. No host link keys exist. A peer SMP request is dropped with no reply and one `rc=6`
error; that numeric BLE_HS_ENOMEM means unsupported SMP here, not an allocation failure.
Controller `BLE_INIT ... SMP:1` describes its separate unchanged capability. [ATT/SMP departures](adr/0005-tesla-ble-seam.md#2-platform-forced-departures-and-reference-deviations).

All targets use the manifest-pinned v6.3.0 graph and lexical, hash-checked patch series. The firmware
uses `TeslaBLE::Client` directly; `main/logic/` owns framing, UUID/session tracking and an eight-slot
command runner: VCSEC auth → wake → Infotainment auth → payload, with backoff. Strict 2-byte BE
length framing has bounded buffering and inter-chunk timeout; TX preserves the builder's frame.

Patches 0001–0003 are retired. Current patches trim unused Parental Controls (0004), align session
counter `max(local, reported)` plus epoch/time with `signer.go` (0005), and port crypto to PSA with
response-tag verification against the response's counter (0006). Upstream supplies that verification
and counter; the PSA rebase preserves it, failure wiping and a separate response-AAD helper.
Patch 0007 refuses message construction when routing-address or request-UUID randomness fails.
Peer admission requires an initialized shared secret, including restored sessions. Domain request
hashes remain isolated. Encrypted VCSEC requires an initialized peer and non-empty request hash;
missing context, failed tags or repeated carried counters (including zero) drop before protobuf,
sleep updates or callbacks, never falling through to plaintext. Passive plaintext VehicleStatus
keeps its separate path. Native retries rebuild/sign new UUID/counter bytes; upstream Vehicle's
identical-byte retry does not change this policy. Upstream C5 support does not add a firmware target.
The v6.3.0 charging-power estimator belongs to upstream `Vehicle`; native charge telemetry still
reports the vehicle's raw power and phase fields without inferred AC watts or phases.
The v6.3.0 optional poll-result callback also belongs to upstream `Vehicle`; the native
controller does not consume it or treat command completion as fresh telemetry.
[ADR-0005](adr/0005-tesla-ble-seam.md) records all departures;
[ADR-0002](adr/0002-idf6-mbedtls4-crypto-seam.md) records wire/key compatibility.

Transactional key rotation exports bounded PEM, rolls back failed persistence and durably journals
cleanup. An armed `key_rotate` or uncertain probe blocks signing/construction. Interrupted rotation
retires `key_created` before clearing its journal because the timestamp cannot be bound to the key.
VIN transitions use
`vin_txn` so power loss cannot pair a new VIN with an old key/session.

## On-device display and status LED

The optional T-Dongle-S3 ST7735 uses the shared cache-only `UiSnapshot` and pure `display_model.hpp`
presenter. BOOT rotates landscape 160×80/portrait 80×160 and their flips; `disp_rot` persists and
migrates legacy `disp_flip`. The parity gate compares all 18 presenter fields across 14,114 cases,
including signal bars/glyph state, both layouts and integer SoC/RSSI sweeps. It also compares
98 native font/bitmap records and byte-checks the generated font header against the simulator
source. Another 840 cases execute the actual native text primitives against the simulator,
including clipped/outlined text, all byte values and UTF-8 byte widths. The panel has a printable
ASCII font; each unsupported UTF-8 byte becomes a blank cell. Both preview montages include
LAN, setup/search and punctuation examples with complete captions. These checks do not establish
complete-frame pixel equality, physical animation or glyph legibility. A further 820 cases execute
the actual frame-assembly/layout bodies with passive cache/drawing doubles, including SafeMode
cache bypass and number/outline bounds for all integer percentages. The portrait battery interior
leaves room for those outlines.

Once the display has started, SafeMode overrides connection, pairing and charge heroes, hides the
BLE header and shows "SAFE MODE" with "Open web UI" or, without a network lease, "Check network". Its latched input comes from `safe_mode_active()`;
frame assembly bypasses the vehicle cache in recovery. Network header and SSID scrolling remain
available; recovery itself is static. This presentation does not alter the boot latch, start vehicle
work, issue a command or provide an initial-portal screen before display startup.
Landscape geometry is hardware verified; portrait orientation still needs on-device confirmation.

Auto-detection requires ≥4 of 6 TF-socket pull-up pins high; other S3 boards do not allocate the
~25 KiB framebuffer or drive display pins. SPI is 40 MHz, BOOT GPIO0, backlight active-low.
The optional APA102 (DI40/CI39) uses the same snapshot, shared SoC gradient and latched alerts.
Charging requires Awake plus a known SoC; a retained charging flag cannot override Asleep or
an uncertain link. This is not a charge-domain age guarantee. A failed current WiFi signal read
uses the same network-search downgrade as the panel. Independent sampling and alert priorities
can still produce different frames.

Once the LED task has started, latched SafeMode takes priority over network, charging and OTA
indications and blinks red. Recovery sampling reads the latch without consulting the vehicle cache,
durable sessions or the OTA status mutex. It uses the existing error hold; this does not change
startup order, enable the optional LED by default or provide a signal before that task starts.

`logic/led_driver.hpp` owns the 15 s error/10 s warning holds, animation factors and 12-byte
APA102 frame. The task samples alerts about once a second and sends frames about 30 times a
second. Pixel transport is heap-free and disabled by default. The LED's OTA read copies only a
state enum; initial mutex creation and session-presence probes can still allocate. Neither
renderer wakes the car. The sampling helper isolates its snapshot temporary from the persistent
render-loop frame; this does not prove composed stack headroom. The S3 primary CI leg additionally compiles the enabled
LED task in isolation; that unsigned variant never enters publication artifacts or changes defaults.

## Home Assistant MQTT bridge

`mqtt_ha.cpp` reads caches in its own task and subscribes to no command topics. Configuration is in
[User guide](USER_GUIDE.md#home-assistant-mqtt); topics/fields are in [API](API.md#mqtt-topics).
The lowercase VIN-derived node identity survives board replacement.
Each discovery round snapshots the current active interface's IPv4 address once for its
configuration link. A missing/zero address, failed query or different active interface at the
recheck omits the link instead of reusing the boot address. Later rounds refresh it; identity and state
topics stay unchanged. This snapshot does not promise that a lease survives the broker publish.

An explicit scheme is honored. Without one, credentials select `mqtts://`, otherwise `mqtt://`.
TLS verifies bundled CA roots and never falls back to cleartext; status exposes credential-free
broker/error/TLS state. The discovery registry is the sole owner of entity rows and metadata.
Optional fields remain absent, units convert to metric, and presence-aware templates return literal
`None` for unknown. Boot timestamps latch only after NTP establishes clock authority and remain stable
for that boot. Browser and restored estimates do not establish this timestamp. Stack minima create
no discovery entities.

On reconnect: discovery → online availability → state; periodic snapshots follow. Invalid caches
publish `{}` once to remove retained stale readings, then stay quiet. Sticky JSON ownership prevents
partial retained documents; every build/print/publish failure rearms the entire sequence.
The 60 s watchdog is fed at loop boundaries and each completed publish, never around a stuck call.

## Crash diagnostics

`diag_crash.cpp` captures once before service tasks. A bounded shared reader admits the installed
partition header, verifies its SHA-256 through PSA, checks complete serialized ELF identity and
only then reads the pinned target's TCB/frame ABI. SDK boot parsing and DRAM capture remain off.
Foreign or unverified artifacts survive boot; full running/dump hashes and explicit summary flags
keep identity, availability and corruption separate. RISC-V does not invent a stack unwind.

Boot metadata lives in immutable fixed arrays, published before service tasks without dynamic
payload allocation. Live readers query a bounded header, borrow those arrays and recheck atomic
admission after constructing the view. Query errors or invalidation suppress current-build downloads
and summaries for the rest of that boot. Bootstrap uses a nonallocating reset/fault getter.
Boot console/ring output uses the same bounded `CRASH` records as Syslog at ERROR severity.
A notable report without a summary adds the complete running ELF hash separately; foreign dumps
retain their own hash in the header. Ordinary and dismissed reports produce no crash records.
Formatting and partial log failures leave admission unchanged and trigger no retry. Renderers may
allocate; the human multiline helper remains available to host presenters.

Export and deletion share a nonblocking request token held across I/O, without a leaf lock.
Deletion marks the request outcome uncertain and revokes admission before SDK erase; only actual
absence or successful erase permits dismissal. Existing effect-aware HTTP replies preserve known
or unknown outcomes through allocation failure. [API](API.md#diagnostic-export-and-heap-trend)
owns export modes and response classes; no crash data belongs in public evidence.

## Syslog forwarder

The log hook mirrors console lines into the RAM ring and a fixed 24-item forwarding queue.
Each `SyslogMsg` has 256 payload bytes plus a two-byte length (258 bytes per item); FreeRTOS
queue-control storage is additional.
RFC5424 facility user severity follows E/W/I/D/V (3/4/6/7), skipping ANSI prefixes; unrecognized
lines remain info. Firmware boot replay explicitly marks BOOT as INFO and CRASH as ERROR;
message text alone cannot acquire either replay type. Replay snapshots the live crash report, so
a dismissed report is not replayed later. Syslog is best-effort and
[unredacted](SECURITY.md#syslog-and-diagnostic-export).

The ring lock covers bounded copies and sink snapshot only; invoke sinks after unlock. Chunked
exports bind append count and clear epoch, stop on generation changes, discard a wrapped first line
and redact overlong logical lines as a whole. Recognized complete V1 library records flatten
embedded line breaks in the ring copy. A boot-installed sink replaces known credential-bearing MQTT
URI parse errors before every output even if the ring mutex fails; other raw Serial/Syslog records
retain their bytes. The boot owner publishes mutex and previous-sink handles atomically; each
consumer snapshots its handle once. Export limits live in
[API](API.md#diagnostic-export-and-heap-trend). Configuration changes persist then reboot.

DNS resolve/reprobe is throttled to about 10 s. Delivery requires resolution, not the advisory
ARP/ICMP result. Hard route/link errors re-resolve; transient resource errors keep ordinary cadence.
Log only failure/recovery transitions; reject the forwarder's own `syslog:` lines to prevent loops.
`syslog_start_gate.hpp` publishes the process-lifetime queue only after task creation succeeds;
a scheduled consumer waits until commit, and cancelled unpublished resources unwind safely.

## Heap-exhaustion watchdog (last-resort escalation)

`heap_supervisor.cpp` is the sole heap sampler and recovery owner. Its essential task starts after
consuming the typed reboot reason and obtaining BootGuard's verdict, before optional Syslog,
Ethernet or a non-returning setup portal. It needs no Vehicle handle or lock, so Booting, portal,
SafeMode, Fatal and a stalled/dead vehicle loop cannot disable the critical decision. Every 30 s
it samples INTERNAL 8-bit largest block: below 4 KiB for five minutes requests restart, one healthy
sample resets the run. OTA busy does not clear critical elapsed time. A continuous episode grants
at most 300000 ms writer grace from its first critical sample; only an actual INTERNAL largest
block of at least 4096 bytes resets it. Clock regression/expiry is sticky through new attempts.
After grace expires, a busy writer receives an abort request; the supervisor cannot steal its owner
or restart before writer quiescence is acknowledged. A network-only cleanup quarantine cannot
postpone the existing admitted restart. `ota_is_busy()` is atomic state, not a status copy.
The existing hold uses wrap-safe low-32-bit milliseconds from the 64-bit monotonic timer; late
samples retain elapsed critical time. Healthy-maintenance freshness does not shorten that hold.

The same samples feed the optional retained trend after critical evaluation. Its mutex is prepared
before task/readers start; cyclic recording uses checked zero-wait acquisition, dropping a sample
when contended. CRC/layout, retained carry and saturated `logic/uptime.hpp` seconds are unchanged.
No network, storage or logging occurs under the trend's leaf lock. Optional failures cannot bypass
critical evaluation. Periodic INFO logs preserve the historical 8-bit `free`, `largest_block` and
`min_free` fields beside the decisive `internal_largest`; `supervisor_min_stack` labels the
supervisor's own stack observation separately from Vehicle minima.

Restart needs an actual FaultRestart lease, a fresh post-CAS INTERNAL/OTA check, raw Stable
verification and two confirmed commits: one unit in the shared `boot_fails` ledger, then the
separate `reboot_why=heap:<n>` diagnostic. All runtime modes use the same ledger. The fifth used
unit exhausts automatic heap recovery; four units latch SafeMode on the next boot. A verified
heap SW boot preserves its reservation, while a real fault boot adds one unit. Missing live,
malformed, unreadable or unconfirmed counter state grants no reset. A lower visible count cannot
erase the RAM mirror's confirmed floor. A failed reservation or final reason consumes the
five-minute write-retry interval; a confirmed reservation is never undone after reason failure.

Failed strong CAS reports its observed owner. Precommit exits release only their own lease.
After the final confirmed reason, the lease is sealed across logging, the 300 ms Syslog yield,
throws and an unexpectedly returning restart; the task parks rather than repeating the effect.
Pending/unknown image state never restarts or becomes confirmed through fault recovery.
The two commits are not a power-loss transaction: a unit may be consumed without a physical
reset, and no rollback guarantee applies across old firmware, a known Clean exit or shared-NVS
changes.

`heap:<n>` keeps its consecutive-heap diagnostic meaning and immutable boot record. The small
task-local streak can refuse an invalid seed but grants no independent allowance. A confirmed
shared zero after the qualified 30-minute run is required before refilling it. Neither clear
reopens the already-closed automatic vehicle activity window. Known Clean boots and later
explicitly admitted foreground commands retain their separate admission policy.
Only the configuration pointer and by-value diagnostic seed occupy the static startup record;
mutable supervisor policy stays on its allocated task stack. Stack/frame and actual INTERNAL
margin remain separate measurements. Degraded logs share one first/then-five-minute cadence,
independent of owner changes and storage retries.

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
all SPI and auxiliary INT/RST pins it touches against **every later board**, not only earlier mappings. After a positive probe, resources remain in one startup record until activation.
Before the first start attempt, cleanup releases resources only after each prerequisite succeeds;
an unconfirmed step retains the remaining resources and fails boot closed. Once start has been
attempted, cleanup requests a best-effort stop and retains the entire graph for the existing fatal
boot path. A successful stop does not prove that every pending RX callback has exited, so handlers,
driver, MAC/PHY, netif, event group and SPI ownership remain intact until reset. This path neither retries
startup nor falls back to WiFi. Normal active fallback retains the driver for later cable takeover.

No PHY link falls back after 4 s; link-without-lease waits up to three 20 s intervals. A wired
boot that obtains DHCP before fallback never initializes WiFi or consumes a trial-credential backup. W5500 polling
uses 10 ms, no INT/RST pins; the component is S3-only. Ethernet route priority 128 wins off-subnet
traffic; on-subnet lwIP uses the first registered matching netif, so hot-plug may still use WiFi.
Board mappings are owned by [User guide](USER_GUIDE.md#wired-networking-optional-esp32s3).

### Keeping the link up

Before any successful lease, the 30 s boot window, or ten fast WiFi failures within it, ends the
boot attempt. Without usable stored credentials the setup portal then waits for the form. With
stored credentials the portal joins the station as APSTA and keeps retrying them in the background
(`logic/setup_portal.hpp`): first after 30 s, then with doubling backoff up to 5 min. Before the
30 min open-time cap this policy starts no new attempt while a phone is on the setup AP. If the
boot window timed out before the ten-failure budget was spent, the station event handler can
still use its remaining retries; the portal does not suspend that owner. Once a lease (WiFi or wired)
exists, the portal is torn down and the boot continues in place. The open AP closes after 10 min
without a client and 30 min after opening; retries continue. A failed AP close is retried with the
same backoff, and the boot does not continue while the AP is up. Nothing here reboots: a timed restart would revert a pending OTA image,
defeat the crash counter and re-seed the vehicle window. Once online, event-driven reconnect
continues indefinitely. The 30 s gateway watchdog recovers after two failures only if the link
claims up and that transport's gateway answered at least once. Never-answering ICMP is unusable
evidence. Probe setup errors fail open; missing lease/gateway or unanswered probes are unreachable.
Recovery disconnects WiFi (the event handler reconnects) or restarts Ethernet; it never reboots.

Ping state/semaphores are persistent. Each asynchronous session owns a generation. Timeout requests
stop and waits a second bounded interval; without matching end, quarantine as PendingEnd. No handle
delete/reuse/new generation until its worker ends; stale callbacks cannot complete a replacement.

Gateway proof is bound to transport, lease generation and gateway IPv4. Link-down/new-IP changes
reset the failure streak and invalidate in-flight verdicts, even if the next gateway IP is equal.
A watchdog recovery creates a new lease; it must answer once before another recovery. A new ghost
association that never answers therefore stays unrecovered rather than inheriting stale proof.

## Sleep / link-state (the single source of truth)

`VehicleController::link_state()` supplies UI, status and MQTT:

| State | Proof |
|---|---|
| AWAKE | Live Infotainment telemetry younger than 60 s |
| ASLEEP | No live data; uninterrupted VCSEC ASLEEP held about 120 s and BLE reachability younger than 150 s |
| IDLE | Reachable within 150 s, without proven sleep or fresh Infotainment data |
| UNREACHABLE | No BLE reachability within 150 s and no live Infotainment data within 60 s |

Before first evidence the state is unknown/omitted. Raw VCSEC AWAKE cannot prove AWAKE; raw ASLEEP
requires debounce; AWAKE and UNKNOWN both reset its run (`next_asleep_since`/`vcsec_asleep_proven`).
UI labels retained readings as last known; unreachable/unknown offers no action.
Active-window polling needs recent command or fresh charging contact; stale cached charging cannot
keep it open. Non-waking VCSEC `GET_STATUS` health checks outside that window must not open Infotainment or wake.
The request is plaintext; a successful response alone does not prove session authentication.

`ble_phase.hpp` supplies connecting/waiting deadlines together with rounded-up seconds; connecting
wins overlap, and zero means now. The C++ `ble_row.hpp` presenter has shipped-JS parity. Scanning is
not a phase input. The no-VIN listing scan, warm-up connect and manual scan do not arm countdown
deadlines. Local countdowns stop on feed loss.

Connection scheduling uses caller deadlines, a 15 s warm-up throttle and 30 s supervisor idle
after each health attempt. Automatic same-key enrollment additionally reserves its own bounded
backoff. There is no global explicit-wake debounce or exponential reconnect gate; a synchronous
GAP connect-start failure resumes scanning while connect intent remains. Foreground waiters impose
their own deadline; the direct warm-up connect does not arm one.

Ordinary telemetry readers use caches. `body_controller_state` is an explicit live foreground
VCSEC query: it may connect, establish a session and send plaintext `GET_STATUS` without requesting
a wake. `/status` can save an unknown guarded `paired_at` once and query local HCI RSSI. These are
exceptions to cache-only wording, not evidence of fresh authenticated vehicle acceptance.

Failure classification uses adverts observed in the current attempt: no target advert means out of
range, non-connectable means BLE limit, connectable-but-failed means connection failure. Separate
90 s history drives the UI. Background logs report first cause, changes and hourly repeats;
foreground failures always report. Named SCAN_RSP cannot freshen a stale primary connectability bit.
Health/enrollment timeout policies are distinct from foreground command timeouts.

## Pairing lifecycle / invalidation

Pairing requires a plausible 17-character VIN. Without it, the supervisor idles and listing-only
scans show nearby cars without connect/enrol. The supervisor probes status before automatically
sending the existing Charging Manager key. There is no manual enrollment endpoint or UI action.
See [User guide](USER_GUIDE.md#pairing-and-identity).

`paired` means a VCSEC session is stored, not that the car has freshly accepted it. Remote status
faults and command error text set a volatile `reauth` hint; they never erase sessions or rotate
the key. A successful health response clears only the hint, without proving authenticated acceptance.
After a status probe, positively established session absence or a persistent recovery hint permits
same-key enrollment. Both durable and usable RAM sessions suppress enrollment without a recovery
hint, so failed session persistence alone cannot resubmit the key. Storage or runtime-snapshot
errors fail closed. `logic/auto_enrollment.hpp` reserves backoff before
I/O (60, 120, 240, then 300 seconds), including unconfirmed sends and exceptions. Healthy/hint
oscillation does not reset it. The sender rechecks VIN, runtime/key/session state under the command
FIFO and holds Stable-only OTA exclusion through transmission. It uses Background provenance,
NoWakeFail and zero immediate BLE retries; it never opens Infotainment or requests a wake.
Successful local transmission does not prove vehicle approval. Forced key generation and changed VIN are explicit identity changes
that clear sessions/cache; a changed VIN also forgets the BLE MAC and reboots. Same VIN is a no-op.
Successful NVS existence probes are cached; read errors retry instead of becoming absent.

### Cached recovery state and automatic commit reload

`logic/runtime_admission.hpp` packs the terminal admission state and the first coarse fatal
component into one atomic byte. `boot_fatal` publishes that pair before rollback or diagnostic
work. HTTP and MQTT read the shared facade; `logic/identity_recovery.hpp` gives both the same
cached identity-state names. Reading these recovery fields does not probe storage, confirm an image or perform
recovery. `ok` means no known cached recovery block while Ready, not key/session usability or vehicle
acceptance. The web page reuses its recovery notice for terminal/local identity failures and keeps
normal existing-key enrolment automatic.

Only an ambiguous commit begun by the actual locked automatic first-key path publishes
`AutomaticCommitReload`. The pairing task may then reserve one reload after obtaining its own
`ConfigRestart` lease, a finite 100 ms command-mutex admission, fresh Ready/identity facts, raw
VALID/UNDEFINED image metadata and the existing readable `key_rotate` journal. After every
fallible check, an exact Ready-to-Ready+claim CAS in the same atomic byte linearizes this process's
single reload. Fatal winning first refuses it; a later Fatal remains visible with its first component
and does not reopen the consumed claim. No new global object, persistent allowance or image
confirmation is added. Pending, unknown,
missing journal, a boot already recovering that journal, generic runtime mismatch, VIN transition,
cleanup or another owner prevents the attempt. The command transaction mutex permits this journal
read; no Vehicle/leaf cache lock is held. The lease is sealed without throwing immediately after
the claim, before command unlock or any diagnostic. The command mutex is released before logging,
delay or restart. If the local reservation unexpectedly fails after the global claim, the sealed
owner is retained and the caller parks without an effect. Returning/throwing restart also parks;
neither path releases or retries the consumed admission. No image is marked valid by this path.

There is no new NVS schema, key replacement or direct persistence command in the reload helper.
The next boot uses the existing journal-recovery policy, which can remove obsolete sessions and
pairing dates before loading the saved identity. If that recovery leaves no private key, the
existing `HaltAfterEmptyRecovery` rule prevents automatic regeneration. A recovered boot or a
runtime mismatch without automatic-commit provenance cannot earn another restart. These rules
bound this automatic path; they do not prove physical restart, flash durability or identity
preservation. Pre-HTTP configuration recovery is outside this path and requires its separately reviewed
USB recovery procedure.

SessionInfo verification selects its domain explicitly. Existing peer material requires the same
vehicle public key and HMAC authentication with its retained secret, including when only the
session-valid flag is false. Key refusal precedes peer, availability and storage changes. Explicit
peer reset still clears the binding; see [ADR-0005](adr/0005-tesla-ble-seam.md#established-session-continuity-2026-10-23).
A verified SessionInfo establishes the RAM peer and runner tracker before its best-effort NVS
save. A false return or exception records a persistence error without discarding that usable
session or retrying the write. A later ordinary SessionInfo can confirm a successful save.
`logic/session_availability.hpp` defines the bounded per-domain metadata. Vehicle-serialized
updates publish a cache bound to `identity_epoch_`; readers never inspect a live Peer or NVS.
Link loss, identity cleanup and authenticated domain repair invalidate the affected RAM cache.
Ordinary command cancellation preserves it. Temporary identity admission failure hides the cache
until rollback restores the same identity; a new identity cannot inherit the old metadata.
`session_available` reports usable VCSEC RAM only. The supervisor, background eligibility and
UI/display/LED presentation accept durable presence or usable VCSEC RAM, while retaining their
existing recovery, sleep, wake and foreground arbitration. Infotainment availability is diagnostic
only. Public `paired` and its guarded timestamp remain durable facts; persistence errors add no
manual key action or automatic storage retry.

NimBLE posts fixed LinkUp only. The vehicle task owns GATT-ready generation, materializes the MAC
after unlock and persists it once outside shared locks. Startup MAC strings stay startup-only.
Attempt Unix-clock restore **before** loading sessions. The inherited Unix-versus-vehicle-epoch
comparison rejects persisted sessions after successful restore, but an absent/unreadable cache or
failed clock set can leave negative-age acceptance. It is not a reliable 1-hour reuse window.
True session reuse across reboot needs epoch/Unix alignment; [ADR-0005](adr/0005-tesla-ble-seam.md)
records this limitation.

New durable `key_created`, `paired_at` and MQTT `boot_time` require an acquired
`clock_is_authoritative()` before sampling wall time (`time_sync.hpp`). Only a validated successful
NTP application establishes this boot's authority. Browser time is a temporary estimate; it neither
writes `last_time` nor permits durable dates. A restored checkpoint is also non-authoritative.
Historical plausible dates have no stored source marker and remain readable; no provenance is
invented for them. Date getters require canonical decimal seconds above 1600000000 and below
January 1 of build year + 10. The stricter clock/browser floor remains 1700000000.
`paired_at` names the first status observation of a stored session with NTP time, not the vehicle's
pairing event. A failed metadata save may be retried on a later status read.

The strong IDF `sntp_sync_time` extension replaces the pinned weak clock write. NTP and browser
writers share one boot-initialized static operation mutex; NTP applies once and commits its authority
before release, so a later browser writer cannot overwrite that commit. Only IMMED mode is used.
The 100 ms admission wait is not an operation deadline: libc timekeeping locks can wait indefinitely.
Early `gettimeofday` initializes libc's private locks before producers; that SDK bootstrap can
allocate and abort on OOM. No storage, logging or network operation runs under the application clock
gate. The hook catches exceptions before returning to C; failed sync cannot clear prior authority.

Checkpoint restore validates the full decimal value/window and applies it only while the warm clock
is below the clock floor, checking the syscall result. Successful NTP samples offer a checkpoint on
first sync and after six hours since the last confirmed save, with at least five minutes between
attempts. Existing SNTP callbacks supply opportunities; there is no added task/timer or tight retry.
The attempt is recorded before fallible work and save success only after storage confirms it.
Checkpoint NVS/string/log work follows clock-gate release. This policy does not fix the inherited
session-age epoch mismatch or prove RTC behavior on hardware.

## Configuration durability

New MQTT/Syslog targets are limited to 64 UTF-8 bytes across HTTP and the UI. The version1
encoder preserves existing targets up to 120 bytes through unrelated saves, including LAN WiFi
trials and their credential rollback. The unchanged reader retains its 256-byte string sanity
bound. Short-value wire bytes and CRC/layout are unchanged. Unchanged WiFi retains its no-write
success after the shared restart/image-state admission. Explicit service edits can shorten one
target while preserving the other. Setup explicitly replaces WiFi and retires old trial state;
it creates no rollback trial. An older 64-byte writer cannot save a configuration containing longer
targets, so a downgrade retaining them remains unsupported. Values above 120 bytes also remain
unsupported; nothing is silently truncated or disabled.

The encoder owns a private static 512-byte scratch buffer through a nonblocking operation permit.
Busy/reentrant saves return false before encoding, logging or storage and never touch the owner's
bytes. Owned exits, including exceptions, wipe the entire buffer through volatile writes before
releasing the permit. Blob commit and best-effort mirror order are unchanged. This permit waits
on no lock and acquires no vehicle/cache owner; it is not a leaf cache/result lock. Its permanent
RAM and compiler frames are separate from measured runtime INTERNAL largest-block margins.

`cfg_load_state()` accepts one complete CRC-checked blob. Only exact absence permits legacy
per-key loading; present unreadable, CRC/schema-invalid or uncertain blobs fail closed. Saves commit
one coherent blob entry before best-effort legacy mirrors for downgrade compatibility. A failed
write/commit leaves durability unconfirmed; it does not prove the previous blob is intact. A mirror
failure after a confirmed blob commit does not turn that save into a failed operation.

Every read-modify-save route and provisioning uses `cfg_load_for_update()`. With the blob absent,
**any** unreadable legacy key rejects the whole snapshot and every mutation; an armed VIN journal
also blocks boot recovery. Explicitly empty legacy values remain valid. Never replace a committed
identity/settings snapshot with stale mirrors or a partially read legacy configuration.

## HTTP request-body and allocator-failure contract

The shared body reader has a 15 s monotonic total receive budget, checked before/after each read;
progress cannot reset it. A 5 s socket timeout can extend wall duration to about 20 s excluding
scheduling; two consecutive recoverable timeouts remain an additional bound. Late/incomplete bodies
never parse or persist. Both adapters shut down read-side on failure while retaining reply writes.
The wildcard handler advertises Connection: close and retires every body-bearing connection even
on early route/Origin/admission rejection or exceptions. Bodyless requests keep existing behavior;
captive preflight failures return ESP_FAIL to reject unread input.

`read_body_result()` distinguishes empty, oversize, OOM and receive failure (normal cap 2 KiB).

| Case | REST | MCP |
|---|---|---|
| Empty / receive failure / malformed | `400` (persisted-config routes require a body; REST commands accept empty only where the registry declares no argument or the legacy optional boolean) | HTTP 200, `-32700` |
| Oversize (> 2 KiB) | `413` | HTTP `413`, `-32600` |
| Allocation failure (incl. valid JSON whose cJSON tree is null) | `503` | HTTP `503`, `-32603` |
| Nesting > 16 arrays/objects | `400` | HTTP 200, `-32600` (request-complexity limit, not a parse error) |
| More than 64 request values | `400` | HTTP 200, `-32600`, `id:null` |
| Duplicate decoded keys at any depth | `400` | HTTP 200, `-32600` |
| Malformed raw UTF-8 in a string (bad/truncated/overlong, UTF-16 surrogate, > U+10FFFF) | `400` | `-32700` |
| Escaped U+0000 (cJSON's NUL-terminated API cannot keep it for exact ID correlation) | `400` | `-32600` |

No rejected input dispatches or persists. `json_syntax.hpp` validates before cJSON; valid supported
syntax followed by a null parse tree is OOM, not bad input. The three HTTP request materializers
explicitly cap root/container/scalar values at 64; names do not count. An over-budget flag does not
stop syntax validation, so existing malformed/NUL/depth errors retain priority. The generic
materializer's default remains unlimited for OTA manifests. `json_input_validation.hpp` shares
allocation-free decoded-key uniqueness and canonical decimal-string argument checks across REST
and MCP; finite/integral/range checks, boolean rules and registry defaults remain in their adapters. Copy bounded arguments and release body
and parse tree before BLE/probe/NVS/restart. MQTT/Syslog/WiFi use an authoritative config load
and, for MQTT, an optional broker probe. Changed submissions require a ConfigRestart lease before
durable save; WiFi acquires it before credential comparison, including a later no-op. The captive
portal validates its form and performs the authoritative load/VIN guard before the lease, without a
LAN probe. Rejection or a pre-effect failure prevents later steps. A response failure after a durable
save or vehicle command cannot undo that effect; reply atomicity remains a separate limitation
documented in [API](API.md#response-classes). Empty MQTT/Syslog values intentionally disable.
The separate captive `POST /save` has a fixed 1024 B limit and returns 400 before parsing on empty,
oversize or receive failure.

Sticky `JsonBuilder` failure discards the entire tree. `json_http_reply` sets no success status or
sends bytes before printing succeeds. Pre-effect OOM sets 503 before one fixed fallback.
Mutation handlers also carry a trivial request-owned `HttpEffectReply` through `GuardedReq`; it
outlives handler unwind and stores primary outcome, possible effects, a validated fixed RPC ID and
an independent already-decided restart continuation. `logic/effect_reply.hpp` owns the pure state.
`http_effect_reply.hpp` retains normal complete replies and supplies allocation-free project JSON
on post-effect construction failure. The terminal latch precedes header setup, and every checked
header/chunk failure stops the attempt. The outer catch cannot start another reply after that latch.
Continuation consumption precedes the once-only restart attempt, including on response failure.
Configuration-restart continuations retain their already-held ConfigRestart owner; identity-recovery
continuations follow their separate transaction rules. There is no effect retry, no borrowed handler-local data and no global outcome snapshot.
Networking allocations, actual receipt and composed runtime stack headroom still require device
validation; [API](API.md#response-classes) owns the wire outcome semantics.
Pinned-cJSON allocation matrices verify these production seams, not the device scheduler/allocator.

While downloading/flashing, time/MQTT/Syslog/WiFi/channel mutations return 409 before body
intake. This is an entry snapshot, not atomic exclusion. An OTA check holds the shared operation
gate without setting the download/flash flag. Changed MQTT/Syslog submissions and WiFi admission
require ConfigRestart ownership and return 409 before saving when the check owns the gate.
Unchanged MQTT/Syslog acknowledgements need no restart lease. Key/VIN mutations return 503;
time/channel retain their entry-snapshot guard. Crash clear/dismiss routes are outside this guard.
External fields/errors are in [API](API.md).

## MCP endpoint (/mcp)

[MCP](MCP.md) owns the wire contract. REST and MCP share `command_registry.hpp` bounds/names/kinds
and `command_exec.cpp` dispatch. MCP requires arguments; REST retains only declared compatibility
defaults. Both reject fractional/out-of-range integers before casts.

Release input trees after fixed ID/argument capture, before blocking work or response construction.
Static registry strings use cJSON references; sticky ownership prevents partial envelopes, and print
OOM before a command attempt maps to 503; post-attempt reply degradation preserves the correlated
outcome. The production tool/state builders are covered by pinned-cJSON allocation tests.
Every handler enters through contained `handle_all` and `GuardedReq`. MCP cached charge reads
use the same `get_charge_state` availability gate as REST and emit `charge_available`; unavailable
telemetry is omitted. The shared admission/exception fallbacks are not JSON-RPC replies; see [MCP](MCP.md#json-rpc-error-codes).

## Concurrency (normative contract)

New code must fit this contract or revise it with implementation and evidence in the same change.

Vehicle, display and LED iteration catches retain task-owned fault-report state across successful
iterations. The first fault logs immediately; later reports are at least 30 monotonic seconds apart
and include a saturating suppressed-error count. The shared helper advances state before calling
the sink and contains sink exceptions. This changes reporting only; task delays and arbitration stay
unchanged, and an outer terminal catch still reports its once-only task exit. Each task keeps its
16-byte reporting state locally, outside the iteration, without increasing controller storage. The optional
telemetry round keeps its snapshot and builders in a separate non-inlined frame; the loop retains
its scheduling/attempt timestamp and exception boundary. Compiler frames do not bound composed
runtime call depth.

### Lock hierarchy (`VehicleController`)

| Primitive | Kind | Protects |
|---|---|---|
| `command_mutex_` | mutex, RAII | one whole command/query transaction and command FIFO generation; for `set_charging_amps`, the action and the verifying ChargeState poll are one transaction |
| `vehicle_mutex_` | mutex, RAII (`SemGuard`) | runtime `client_` and command-runner access (payload build/dispatch, `drive_command_runner_`, `process_rx_frame_`); exclusive pre-publication initialization is described below |
| `cache_mutex_` | mutex, RAII, leaf | the `last_known_*` caches (`std::string` members ⇒ an unlocked copy is torn-read UB) |
| `result_mutex_` | mutex, RAII, leaf | the externally visible `last_error_` snapshot HTTP/MCP read after a foreground command |
| `CommandCompletion::sem` | per-request binary semaphore, shared ownership | fixed callback result/error record and generation-checked completion, signalled even after its catch-all |
| `StatusCompletion::sem` | per-request binary semaphore, shared ownership | fixed status-success fields; the C++ runner failure callback may allocate its request-owned `std::string error` before signalling |
| `ble_events_.mux` | dedicated `portMUX`, bounded POD only | every ready/queue push, pop and clear; same-generation adjacent RX bytes pack into16 static244-byte cells, with ordered whole link barriers; unlock before generation/Client/runner/storage/link work |
| `telemetry_pending_mux_` | `portMUX`, innermost, POD only | the fixed latest-value nanopb mailboxes, pending mask and charging-amps feedback generation written synchronously by tesla-ble callbacks |

Order for identity/OTA-coordinated transactions: shared operation-gate admission →
`command_mutex_` → `vehicle_mutex_`. The operation gate is an atomic exclusive owner, not a
mutex held while waiting. Ordinary commands start at command ownership. Cache/result locks are
independent leaves. `client_` has no public raw-pointer accessor; only controller-owned paths use it.
`init()` constructs the Client and sets/loads/probes its identity before publishing BLE callbacks
or starting controller tasks; those exclusive startup calls need no Vehicle lock. After publication,
controller-owned runtime Client/runner access requires Vehicle ownership.
The deferred mailbox owns its bytes; no callback-local buffer or mbuf pointer survives return.
A callback of1..244 bytes is accepted completely or refused before mutation. The Vehicle consumer
copies at most20 RX bytes per pop into an independent result, retaining the head offset until the
cell is exhausted. One fresh2050-byte framed response occupies nine cells even with one-byte
notifications. This is not an unlimited-burst, scheduling or allocator-capacity guarantee. Old
LinkDown remains a barrier; LinkUp/RX require the current stable generation. An observed overflow
clears/reset/disconnects fail-closed; flag publication, queue clear and the entire drain are not one
atomic transaction. Continuous producer traffic is not a new total-drain time bound.
The pending mux is innermost and permits bounded POD copies only: no semaphore, allocation, logging,
parsing or callbacks. Use `tk::SemGuard`/`MutexGuard` and `InFlightGuard` for unwind-safe ownership.

- Hold Vehicle lock only for Client/runner access, never across completion-semaphore wait.
- Vehicle ownership includes synchronous BLE chunk writes (with 10 ms inter-chunk delays) and
  best-effort session persistence after verified peer/tracker establishment. These operations can
  delay another Vehicle-lock owner; the lock is not a bounded-latency cache lock. No completion
  semaphore wait is permitted there.
- Background enqueue rechecks `cmd_in_flight_` under Vehicle ownership after the scheduler's earlier
  snapshot, so a foreground start in that gap prevents admission.
- Cache snapshots are copied without calls out; parse/publish after releasing Vehicle lock.
- `clear_session_and_cache_()` acquires Vehicle internally; do not call it while holding that lock.
- Request-local shared completion survives timeout; generation invalidation prevents late completion
  of another request. `CommandCompletion` callbacks write fixed fields and signal even after their
  catch-all, without logging. `StatusCompletion` has a separate allocatable runner-error path; its
  success callback copies fixed fields. Runner callbacks execute synchronously under Vehicle
  ownership: normally on the Vehicle task, or in the controller caller during cancellation/clear.
  They are not NimBLE-host or SDK C callbacks.
- Command task publishes result text after waiting. `cmd_in_flight_` is set under command ownership;
  it pauses background work but orders no other fields.

### Other synchronization owners

These are separate subsystem owners, not a single global nesting order. Keep diagnostic/cache
snapshots as leaves; do not call network, storage, logging or an external sink under their locks.

| Primitive | Protects / permitted context |
|---|---|
| `BleClient::scan_mutex_` | fixed nearby/primary-advertisement caches; host callbacks try with zero wait, task readers use bounded waits |
| `BleClient::intent_mutex_` | connect/cancel, discovery and CCCD readiness transitions; host callbacks try with zero wait and fail/drop/retry on contention |
| `BleClient::write_mutex_` | outbound BLE chunk sequence; task-side bounded acquisition, including calls made under Vehicle ownership |
| `BleClient::client_mutex_` | peer-address metadata; zero-wait callback publication and bounded task snapshots |
| diagnostic `s_mtx` | ring bytes, append/clear generation and sink snapshots; capture uses zero wait and counts dropped lines, sinks run after unlock |
| Syslog `s_status_mtx` | resolved/reachable/error snapshot; error strings may allocate, so RAII releases on failure |
| Syslog queue and `s_ping` | the boot-published fixed queue lives until reboot; persistent generation-owned ping completion is not deleted/reused until its exact end acknowledgement |
| OTA `s_lock` | bounded `OtaStatusPod` copy/update; string conversion and SDK/network/storage work run after unlock |
| OTA shared operation gate | mutually exclusive Ota, IdentityMutation, FaultRestart, HealthCommit or ConfigRestart owner; refusals do not acquire another owner's operation |
| heap-trend `mutex()` | retained ring adoption, carry, seal and bounded snapshots; adoption logging runs after unlock, with no NVS or network operation under this lock |

The diagnostic/OTA mutex handles are atomically published; each consumer takes one handle
snapshot. Boot-owned handles and fixed queues are not deleted after publication. Shared completion
records and the ping end acknowledgement separately own their asynchronous lifetime.

### Task fault containment and explicit watchdog retirement

The MQTT publisher explicitly confirms current-task TWDT removal or absence before self-deletion;
unconfirmed cleanup parks that same task, emits one contained diagnostic and attempts feeding,
without replaying discovery/state. Self-delete does not imply C++ scope unwinding. The network
watchdog contains per-iteration failures, discards its local failure/gateway proof, and requires a
new matching reply before another recovery. It preserves the persistent asynchronous ping owner,
semaphore and generation while a late completion is outstanding. Unknown SDK cleanup remains
unknown; host facades do not establish target WDT feeding or network timing.

### HTTP request serialization

One `esp_http_server` task runs the synchronous handlers. A command can occupy it for its command
budget (normally up to 20 seconds), and automatic enrollment uses a separate background task.
Status, diagnostics, UI fetches and other clients wait while that worker is occupied. Identity
operations can also wait for command ownership, so the command budget is not a universal upper
bound on every HTTP response. Client timeouts must allow queueing plus the requested operation;
a client timeout does not prove a vehicle command failed and must not trigger automatic replay.
These are source-level scheduling limits, not measured worst-case hardware latency.

### Task inventory

Project-created task priorities come from `task_config.hpp`; stacks remain at creation sites.
The narrative inventory requires review alongside source changes; it is not generated evidence.

| Task | Priority | Stack | Created in | Purpose |
|---|---|---|---|---|
| `vehicle_loop` | `kPrioVehicleLoop` = 5 | 10240 | `vehicle_ctrl.cpp` (fn: `vehicle_telemetry.cpp`) | drain fixed NimBLE Link/RX events, drive `drive_command_runner_()`, parse deferred telemetry after unlock, rotating NO_WAKE poll, sleep gating, BLE-fault link reset |
| `captive_dns` | `kPrioCaptiveDns` = 5 | 4096 | `provisioning.cpp` | captive-portal DNS (setup-AP mode only; vehicle stack not running) |
| `ota` | `kPrioOta` = 5 | 8192 | `ota_update.cpp` | transient install coordinator; sole OTA writer/lease owner |
| `ota_chk` | `kPrioOtaCheck` = 5 | 8192 | `ota_update.cpp` | transient metadata coordinator; same operation/lease rules |
| `ota_net` | `kPrioOta` = 5 | 8192 | `ota_update.cpp` (fn: `ota_transport.cpp`) | one network-only context; quarantined until actual cleanup Ack |
| `auto_pair` | `kPrioAutoPair` = 4 | 8192 | `vehicle_ctrl.cpp` (fn: `vehicle_pairing.cpp`) | pairing supervisor: status/health probes and bounded automatic same-key enrollment |
| `net_wd` | `kPrioWifiWatchdog` = 4 | 3072 | `net.cpp` | ghost-link watchdog (forces the active transport to re-establish, never reboots) |
| `mqtt_pub` | `kPrioMqttPub` = 4 | 6144 | `mqtt_ha.cpp` | MQTT/HA publisher (reads the caches) |
| `display` | `kPrioDisplay` = 3 | 6144 | `display.cpp` | ST7735 renderer (`CONFIG_TESLA_DISPLAY_ENABLED` builds) |
| `ota_gate` | `kPrioOtaGate` = 3 | 3072 | `main.cpp` | one-shot OTA rollback health gate (polls every 5 s; commits on proven link and the shared `ImageConfirmation` INTERNAL-largest requirement from `logic/ota_recovery_memory.hpp` past 90 s under the shared owner, gives up at 600 s) |
| `heapsup` | `kPrioHeapSupervisor` = 4 | 3072 | `heap_supervisor.cpp` | essential early independent INTERNAL heap sampling/recovery and qualified healthy-counter maintenance; own stack observation, no Vehicle handle |
| `syslog_task` | `kPrioSyslog` = 3 | 6144 | `syslog.cpp` | best-effort UDP Syslog forwarder (opt-in; degraded-not-fatal on a failed start) |
| `led` | `kPrioLed` = 2 | 3072 | `led_status.cpp` | APA102 status LED (`CONFIG_TESLA_LED_ENABLED` builds) |

The two OTA allocations reuse the existing per-role 8192-byte value. Concurrent coordinator plus
network stacks, dynamic operation, TLS peak, new translation units and composed frames need
separate four-target measurements; this inventory grants no RAM/flash/frame budget increase.
Neither OTA role is an essential HTTP, health-gate or heap-supervisor task.

Vehicle/auto-pair creation uses Creating→Running/Cancelled with acknowledgement. Before release,
entries only wait one tick; on second-create failure the first self-deletes, never external deletion
across cores. Both then wait for global runtime admission; TWDT ownership unwinds before self-delete.

IDF/library tasks also execute project code. Sizes below are configuration/creation values, not
measured headroom. The listed SDK stack settings are shared by the pinned four-target defaults;
review generated `sdkconfig` when changing the SDK or target configuration.

| Context | Stack source | Project work |
|---|---|---|
| `app_main` | `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` | startup and ownership publication |
| NimBLE host | `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=8192` | GAP/GATT/lifecycle callbacks; Link/RX adapters copy bounded bytes/barriers into the static mailbox, remaining callbacks use zero-wait locks |
| primary HTTP server | `http_server.cpp`: `config.stack_size=8192` | synchronous REST/MCP/UI/diagnostic handlers |
| setup HTTP server | `provisioning.cpp`: unchanged `HTTPD_DEFAULT_CONFIG()` stack | setup form handlers, while the vehicle stack is down |
| ESP timer task | `CONFIG_ESP_TIMER_TASK_STACK_SIZE=3584` | `ble_scan` timeout callback (not an ISR or the FreeRTOS timer service) |
| default event loop | `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304` | WiFi/IP/Ethernet lifecycle callbacks |
| ESP-MQTT client tasks | locked component `MQTT_TASK_STACK`, default 6144; project does not override | publisher events and the transient configuration broker-probe callback in `http_config.cpp` |
| ping session task | project leaves `ESP_PING_DEFAULT_CONFIG()` task settings unchanged | network-watchdog and Syslog ping completion callbacks |
| lwIP task | `CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=3072` | SNTP time-sync callback, including time formatting and the guarded timestamp save |

The SDK also owns the FreeRTOS timer-service (`CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=2048`)
and driver tasks. A configured stack allocation
does not prove callback headroom. The runtime-boundary inventory checks registrations/containment;
it does not generate this table or establish maximum composed stack use.

### Live stack-headroom evidence

`stack_watch.cpp` caches current-task historical high-water samples in 27 explicit context lanes.
The 24-byte bank publishes packed lanes with strong compare-exchange, recomputing after contention.
On the single-core C3, the pinned SDK implements 32-bit atomics with short interrupt-masked POD
operations; other targets require always-lock-free 32-bit atomics. Stack queries happen before
publication, and atomic operations perform no allocation, logging or task-mutex wait.
The four original aliases retain their names; values are conservative 256-byte lower bounds,
measured zero is distinct from unknown, and saturation means at least 15872 bytes. Consumers emit the
adjacent `stack_min_free_quantum_bytes:256` metadata and never use the diagnostic bank as health,
restart or command admission. `test/stack_context_hook_owners.json` owns the exact 44 entry-to-lane
routes, including separate fixed PingNet/PingSyslog callbacks without a larger callback argument.
Actual task entries, iteration scopes and explicit no-unwind delete/restart samples complement
callback exit scopes placed before local guards. Inner BLE enqueue-only adapters remain unchanged
and are covered by their outer NimBLE task callbacks. A logger sample is a diagnostic aggregate,
not proof of an otherwise unregistered caller task: its exit helper refuses ISR, masked-critical,
nonrunning-scheduler and missing-current-task contexts before scanning. The pinned own-stack scan
and atomic update chain performs no logger, allocator or application-lock operation; no recursion
flag/TLS or foreign task handle is added. Unsafe/unexercised contexts remain unknown. Added scan
work, caller-held SDK-lock cost, task frames, RAM/Flash and measured target minima require their
own acceptance; configured stack sizes, finite source checks and host measurements cannot prove
them. The supervisor's separate exact own-stack field keeps its existing semantics.
All 27 contexts are present in status and MQTT; never-sampled contexts are null. A measured
zero lower bound represents 0–255 free bytes and does not by itself prove stack overflow.
The OTA network task has its own lane. Parser callbacks execute in their coordinator caller
and use a separate OtaParser entry-context aggregate, not the network-task lane.
Retrospective minima and compiler frame baselines prove different things. Bench policy floors are described in the
[test guide](../test/README.md#whats-not-covered-by-design).

### Atomics doctrine

Use atomics for independent cross-task scalars; multi-field snapshots and strings need a mutex.
`volatile` creates no happens-before edge. Transport, diagnostic, BLE and MQTT scalar flags follow
this rule; flags do not make surrounding mutable state safe. Default operations use sequential
consistency. Explicit acquire/release pairs must name the ownership/publication edge they establish;
relaxed counters/independent scalar samples must not order another field's publication or lifetime.
A change in memory order requires review of both producer and consumer.

### Exception containment (normative)

Exceptions cannot escape C/RTOS frames. Vehicle, MQTT, display and LED loops contain expected
iteration failures with RAII cleanup and retry delays; their outer task boundaries may still stop.
The network watchdog has an outer terminal catch and deletes its task after an unexpected
exception; it does not automatically resume. One-shot OTA jobs publish terminal error. Callbacks catch locally or
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
registration, early heap supervisor and OTA gate task. Fatal PendingVerify yields while a temporary
operation owner is held, then freshly queries metadata under its own FaultRestart lease and marks
invalid without confirmation. An uncertain SDK rollback effect retains the lease; Stable/unknown
metadata releases it and parks with existing diagnostics rather than entering a reboot loop.
VIN-journal repair and automatic WiFi rollback hold ConfigRestart across their entire mutation and
committed restart; neither can confirm an image. Initial-key recovery retains its existing identity
owner without recursively acquiring another lease. Optional MQTT/Syslog/display/LED
contain start failures, unwind partial resources and report disabled status.

Fatal diagnostics use only the sinks already started: an NVS failure can halt before networking,
Syslog or HTTP exists. Retain the serial boot record and its failed component; a missing web UI
does not distinguish that halt from a network fault. See [recovery steps](USER_GUIDE.md#safe-mode-and-fatal-startup).

### Boot-loop safe mode

Four shared unhealthy-restart units before a qualified 30-minute normal run latch recovery:
networking, web UI and OTA remain, while BLE, vehicle, auto-pair and MQTT stay down. The existing
canonical-decimal `tesla_cfg/boot_fails` record is the common ledger, saturating at100. Fault
boots increment even beside a heap breadcrumb; verified heap SW boots preserve their prior
charge. Non-Clean legacy enrollment takes the maximum of the verified old counter and heap
streak before charging a current fault. This conservative floor cannot reconstruct the unknown
historical sum; the single five-unit bound starts at that enrollment point. No new NVS key or
encoding is introduced. Counter read/write/type/parse or exception failure enters SafeMode;
unknown reset/reason evidence cannot lower a verified count or grant automatic recovery. Reason
Error also vetoes healthy clear. Known Clean configuration/OTA/power/reset exits still require
a valid counter read and confirmed zero commit.

`logic/heap_supervision.hpp` qualifies recovery maintenance from 64-bit monotonic observations:
Ready acquired before any BLE singleton read, current host sync, zero sticky host resets, a successful
Vehicle iteration within 60 s, a network lease, INTERNAL largest block at least 4 KiB and no OTA.
Sleeping/unpaired operation is eligible without a probe or wake. Known runner/parse failures do not
publish successful progress; a terminal vehicle boundary first marks Fatal. Missing, backward,
ineligible or more-than-60-second-gapped samples restart this healthy interval.

Only this full interval can authorize the pending shared-counter clear. The actual HealthCommit
owner, a second fresh health/elapsed check and a valid present counter precede the durable zero;
only its confirmed success refills the RAM mirror and diagnostic streak. Failed/throwing or
false-visible commits stay pending and retry no faster than five minutes. SafeMode, portal,
Fatal, stale progress and raw uptime earn no clear. A normal boot may complete this healthy
repair after a failed reservation/reason, without changing its live runtime admission to
pseudo-SafeMode. Threshold four governs the next boot's admission.

From verified zero, alternating heap/fault events share five units: H/F/H/F/H reaches SafeMode
at four, permits the last Stable heap recovery at five, then refuses another. An all-heap path
has the same cap; an actual fault consumes that same remaining credit. This bounds the cited
loop when its fault-producing work is disabled in recovery. It cannot prevent externally forced
resets, unrelated faults still occurring in SafeMode, later faults after a fully qualified
healthy interval, or explicit known Clean recovery exits. Host multi-boot tests preserve one
fake durable store and execute the actual startup, reservation and owner programs; they are not
physical power-cut or live-vehicle evidence.

The permanent 3072-byte supervisor replaces a temporary 2560-byte timer. No steady-state RAM saving
or worst-path stack margin follows from that replacement: static state, task/TCB allocation, startup
overlap, INTERNAL largest block and own runtime minimum require separate measurement. Repeated
resets or firmware rollback do not repair shared NVS. Preserve failure evidence and follow the
[recovery steps](USER_GUIDE.md#safe-mode-and-fatal-startup).

### Deferred: owned BLE-ops queue

A single owner queue could replace current command/Vehicle-lock coordination, but adds static cost
and migration risk. Revisit when a new command class or another queue-position incident defeats
current arbitration; it is not implemented behavior.
