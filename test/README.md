# Host tests

The host suite exercises IDF-free decisions and selected production glue with deterministic stubs.
Fast C++ and normal host checks run without Docker. Strict mode on macOS also requires Docker
for the real Linux browser check. No host check requires USB or a vehicle. Firmware compilation is a separate
[development gate](../docs/DEVELOPMENT.md#validate-a-change).

## Run it

```bash
scripts/run-mock-tests.sh               # available local checks, reports missing capabilities
scripts/run-mock-tests.sh --require-all # fail closed: CI/Stop prerequisites are mandatory
```

Normal mode needs C11 and C++17 compilers, using CMake when present or equivalent direct compilation.
The separate real v6.2.0 tesla-ble/PSA harness requires C++20; pure firmware logic remains C++17.
Strict mode requires Git, Python, Node, CMake, compiler and every gate file, with bounded subprocesses.
Missing tooling is failure, not reduced coverage reported green. A failed check fails its binary:
the pure-logic suite and the tesla-ble harness report every failed check before exiting 1, while the
NVS, runtime-boundary, cJSON OOM and MQTT JSON binaries and the fuzz smoke stop at the first failure.

The real NVS/config tests observe a synthetic serialization buffer through the passive save shim:
maximal 473-byte payloads, complete 512-byte wipe on success/refusal/failure/exception, synchronous
reentry and a bounded controlled thread collision. They preserve the original blob-before-mirrors
fault matrix. No production buffer accessor or live NVS data is used; this does not simulate power
loss, flash durability or prove runtime heap margins.

`clock_runtime_tests` compiles the actual clock shell and date getters with passive RTOS/libc/storage
doubles. It covers writer orderings, typed failures, authority-before-sampling, restore and checkpoint
faults. Pure clock/window/parser/checkpoint decisions and key-date stamp-or-retire callbacks have
registered logic ownership; the latter also runs through the real NVS adapter fault matrix. These
checks do not emulate SDK timekeeping, flash durability, RTC behavior, NTP networking or hardware.

Native runtime tests compile the actual `SemGuard`, `InFlightGuard` and stack-watch shell. They
check normal/standard/unknown-exception unwinding, failed acquisition, independent sampled slots,
monotonic minimum retention and visible zero through passive semaphore/high-water-mark doubles.
These tests do not measure physical task margins or prove scheduler/RTOS timing.

The real-library harness also runs `test_dependency_guards.py` over the exact patched Peer
decrypt body and pinned logging macros/formatter. Synthetic PSA drivers write before returning
update/verify errors with zero reported length; capacity and guard-byte checks reject partial or
removed wipes while preserving successful output. INFO retains ERROR/WARN/INFO and avoids
DEBUG/VERBOSE formatting and argument evaluation; default/debug-enabled controls fail this oracle.
Linux repeats these probes with ASan/UBSan/LSan. This is driver-failure modeling, separate from
the real V1 crypto vectors, ESP-driver behavior and physical key/session preservation.
The runtime source contract pins the 2 KiB shell cap, reviewed vehicle-task allocation, leaf-helper
logging exclusions, lexical Vehicle-lock/wait scopes, and generation/command-owned completion
branches. Mutation canaries test each seam; this is not a general C++ dataflow or interleaving proof.

Iteration-fault tests simulate 15 minutes at vehicle/display error cadences, including alternating
successful iterations. The shipped sink helper runs against throwing/passive sinks; source controls
bind all three catches to task-owned state and a 64-bit monotonic clock. This does not induce a
firmware exception or exercise the physical scheduler and logging transport.

`config_commit_tests` compiles the shipped request and health-commit leases, shared runtime sampler,
raw-state/config confirmation, full timed health task and setup portal, plus unmodified pinned SDK
CRC/query fragments through an explicit passive type/I/O bridge. It exercises five occupied owners,
raw NEW/PENDING/VALID/UNDEFINED, blank/CRC/query errors, timed Wait/Stop/GiveUp, fresh post-owner
refusals, returned/exceptional SDK effects, cache-before-diagnostic ordering, portal outer catches
and partial replies. Controlled timers and task-deletion doubles bound these scenarios; neither
executes the physical scheduler. Pure sampler tests check reader order, short circuits and heap
boundaries; the configuration path keeps its existing offline confirmation policy.
Locked-cJSON tests run actual HTTP handlers across matching/different WiFi submissions and preserve
known image results in normal/degraded responses. The C and C++ objects both run under Linux
sanitizers. The fixture is bound to the toolchain-file hash; a migration requires a fresh SDK review.
These checks model SDK mark effects; they do not execute flash writes, the bootloader, RTOS or
physical restart, and cannot prove runtime heap/stack or key preservation.

`syslog_runtime_tests` compiles the shipped formatter and boot-replay bodies with passive socket,
cache and application-description adapters. It checks BOOT INFO/CRASH ERROR, ordinary-message
classification, default-argument behavior, live dismissal, no replay retry after an allocation
exception, bounded packet formatting and socket/send errors. It opens no socket and does not
establish UDP delivery or live-device behavior. The fast boundary selector also runs this adapter.

`crash_runtime_xtensa_tests` and `crash_runtime_riscv_tests` compile actual `diag_crash.cpp`
against passive SDK/partition/PSA facades. They cover each boot read failure/exception, hash API
failure, foreign/short identity, absent/error partition lookup, wrong installed extent, allocation
failure, request contention, header changes and erase success/error/partial effect. The same tests
run in direct fallback, the fast boundary selector and Linux sanitizers. Facade ABI declarations
are synthetic; only pinned compilation on all four targets establishes SDK layout compatibility.
Boot ERROR records, full-hash fallback, suppressed ordinary/dismissed records and partial log
failures run through the actual shell with a passive severity-aware sink.
The live snapshot borrows immutable boot payload; actual shell tests check pointer ownership,
no payload-copy allocation, retained metadata after erase and the separate export authority.
Pure owned/view presenters must match. Generation suppression has deterministic before/after
cases and a shell contract binding the final acquire load after view construction. This does
not claim an executed erase precisely after view construction; the real header-read/erase
interleaving remains covered separately. Status models, SDK queries and renderers can allocate.

Locked-cJSON tests compile the actual crash HTTP bodies and F031 serializer/outer catch, exhaust
post-erase JSON allocations and fail each header/read/chunk. They verify no second response, clean
failed-stream terminator, automatic retry or false dismissal. No real crash dump or flash is used.

For a narrow CMake iteration:

```bash
cmake -S test -B build_mock
cmake --build build_mock
ctest --test-dir build_mock --output-on-failure
```

CI splits host evidence into `logic-test` and `logic-harness`; both run alongside producer and
independent per-target compiles, and final `build` fails unless both host jobs succeeded. Python gates use
`scripts/with-host-python.sh` (Python ≥3.11). Linux sanitizer
checks use ASan, UBSan and LSan, with deliberate tripwires proving the detectors work, plus a fixed-seed
20,000-case fuzz corpus. Local host evidence is distinct from remote CI.

The build-contract suite runs the real Pages assembler with and without a published ref using its
invoking Bash, including macOS Bash 3.2.

## What's covered

[`logic_test_ownership.json`](logic_test_ownership.json) is the complete machine-checked inventory
of logic headers, concrete test functions and evidence. Each function must have one definition and
one real invocation; headers compile standalone. Firmware delegation is required for production
behavior claims. Mirrored C++/Python/JS presenters need parity checks, not merely copied unit tests.

| Area | Evidence |
|---|---|
| Optional status LED | `test_logic.cpp` tests charge/link/SafeMode priorities, repeated error/warning holds, animation bounds, dimming and APA102 bytes; `test_led_driver_contract.py` compiles actual GPIO/task/OTA-state bodies against passive doubles and checks 256 GPIO frames, 20 OTA-state cases and 16 first task frames, including recovery on/off/on phases with controlled timers, vehicle/session probes forbidden and no OTA status read. It also executes the real CI selection branch with a compiler stub. Linux runs the native probe with sanitizers. `ci-build-all.sh` also builds an isolated enabled S3 variant after its primary gates; physical LED timing/brightness and task margins remain hardware evidence. |
| Decisions/state machines | `test_logic.cpp`: VIN/identity, units, link/sleep, active window/wake retry, automatic same-key enrollment cadence/admission, commands, OTA versions/intake, config/storage, watchdog, admission, asynchronous generations, presenters |
| Real tesla-ble/PSA seam | `scripts/test-tesla-ble-harness.sh --require-all`: pinned library/patches, Nanopb and pinned Mbed TLS4/TF-PSA-Crypto; production frame/key helpers, compiled native CarServer response and identity-cleanup methods, and V1 crypto vectors |
| Presenter mirrors | `check-display-sim-parity.sh`: all 18 native model fields over 14,114 cases, 98 glyph/bitmap records, generated-font provenance, 840 actual native text-primitive pixel/width cases, 820 actual assembly/layout cases with passive drawing/cache doubles, and malformed/empty-golden controls; layout probes check WiFi/LAN search and SafeMode labels as well as signal consumers. `check-ble-row-parity.sh`: actual shipped JavaScript against native BLE/SoC/RSSI rules. Neither proves complete-frame pixel/animation equality, glyph legibility or physical panels. Linux sanitizers also execute the two native display probes. |
| Public protocol vectors | `tesla_protocol_vectors.test.mjs`: VIN, ECDH/HMAC/AES-GCM and patch contracts, public test keys only |
| External JSON contract | Golden status output, command registry/bounds, MQTT registry/fields/presence and exact MCP IDs |

The v6.2.0 real-library harness also checks fresh/forced/established/cleared-secret peer admission,
monotonic session counters, domain request-hash isolation and invalid-domain lookup.
The crypto harness checks official client/session derivation, HMAC/tag, AES-GCM, response-counter
AAD (different from request counter), tampered-tag refusal, PEM compatibility and invalid keys.
Disabling the real PSA RNG must refuse handshake and payload construction without emitting a frame.
Fingerprint derivation remains covered by a host mirror; the IDF crypto shell is not compiled on
the host. The production text formatter is tested directly against the previous representation
and the VIN-journal parser. Extracted actual HTTP adapters reject an empty journal before writes.
The native NVS suite exercises actual config-save ordering, blob/mirror set/commit failures and a
real vector-allocation failure after the length probe. Source pins cover every configuration
read-modify-save refusal and fatal boot configuration validation. These do not simulate NVS durability.
Presenter parity likewise proves decisions, not physical pixels.

SessionInfo continuity checks use the existing published V1 vector for initial and established
refreshes in both domains. Ordinary direct-API cases reject missing, malformed or different keys
before peer, native availability or storage changes, including temporarily invalid sessions.
`test_dependency_guards.py` compiles the actual Client verifier with passive crypto drivers to
check domain/retained-secret selection and serialized-payload bounds, six removed-check controls and a pre-log key-validation
control with fully backed buffers. These selector checks do
not generate protocol messages or establish cryptographic answers; the real-library V1 harness
owns those answers. Explicit peer reset remains a fresh-bootstrap boundary, not a durable key pin.

The native VCSEC harness compiles the production routing and response methods with the real
patched Client, Peer and CommandRunner. It covers status/action/auth/wake admission, proactive
HMAC-verified session repair, failed writes, stale attempts, passive wake progress and zero-inclusive
GCM replay. A full auth → wake → Info auth → payload → authenticated response sequence checks
retirement and ordinary-address restoration. The native RNG fault facade simulates a failed public
RNG status; its normal path delegates to the real helper. This is offline evidence, not hardware or
entropy-failure acceptance.

The same native harness pins all 29 universal fault enum numbers and exercises independently sealed
fault/AAD replies, foreign routing/UUID, bad tags, replay, delayed safe retry, uncertain writes and
10-minute no-resend simulations. It also compiles the actual result callback, timeout, publication
and FIFO invalidation methods against deterministic semaphore/link sinks. Controls cover typed
failure delivery, stale generations, callback exceptions, missing semaphore notification, permanent
rejection allocation failure and committed-success fallback. These are host execution checks, not
physical ATT or live API/vehicle acceptance.

Session-availability cases compile the actual cache, persistence, restore and link-reset methods.
They cover save-false and throwing storage with and without older records, RAM-only command
completion, current-save error recovery, domain isolation, identity gating/epoch changes, ordinary
FIFO cancellation, authenticated repair and throwing link-loss callbacks. Ten simulated minutes of
cache reads must add neither frames nor writes. Restore uses a deterministic clock facade; this
does not prove physical NVS retention or correct persisted-session age conversion.

Wake cases compile the actual wake method, generic VCSEC wrappers, clock helpers and result
callback. The blocking-send handoff uses controlled early failures or the real native BLE driver
with successful/false-after-submission writes. Controlled cache inputs pass through the production
link-state model; they are not live telemetry. Checks cover early definite return, original uncertain
reason retention despite shared-error changes, later Info confirmation, no TX-only reachability
stamp and ten-minute no-resend controls. They do not establish physical wake or vehicle acceptance.

## Beyond the pure-logic binary

The effect/reply suite compiles the actual REST VIN matcher and command handler. It checks exact
case-sensitive configured-VIN acceptance, null/mismatched VIN refusal, and `404` before dispatch.
VIN tokenization is a passive fixture here; these cases do not validate the real URI parser or
contact a vehicle.

Changed-VIN admission also checks the actual handler with its identity guard refused: no reset
transaction, persistence, effects or restart may begin. Literal fault counts and elapsed times pin
the four-fault SafeMode threshold and five-minute heap hold independently of their constants.
Heap-ring cases retain the open bucket and minima across decreasing timestamps. Structural source
checks bind the heap restart cap to its durable counter and the sole NimBLE start to the actual
SafeMode branch; they do not execute the ESP-IDF scheduler or radios.

Provisioning fixtures derive their wrapper and field keys from the actual straight-line
`http_common.cpp` `make_response` producer and fail closed if its shape changes. LAN WiFi/VIN
successes must read the nested boolean; refusals, misleading flat success and wrong-typed values
never trigger a second request. Success output distinguishes accepted configuration from an
immediate reboot and does not claim a changed VIN preserves pairing. These tests mock HTTP
and perform no provisioning or NVS writes.

The BLE/browser parity gate also compares the actual web SoC function, rendered RSSI glyph counts
and warning constant with native helpers: all integer SoC values, clamp cases, signed-byte RSSI
and integer extrema. It preserves float32 intermediates and native rounding ties. Hero tests pin
both consumers of the warning threshold. It does not measure physical display pixels or live RSSI.

The mock runner compiles the locked cJSON intake/OOM matrix when Python is available; strict
mode requires it. A cold cache needs network access to the Espressif registry and archive host.
Prepare it with `bash test/run-cjson-oom-tests.sh` before an offline run. Every run re-hashes the
warm cache against the four lockfiles. The matrix checks the 64-value
request boundary, unchanged allocation counts for valid input, zero parser allocations for a
2 KiB over-budget array, generic/OTA default compatibility, decoded duplicate keys, canonical
numeric strings and leak-free parser failpoints. The fuzz gate compares counted and uncounted
syntax outcomes so the request budget cannot hide malformed/depth/NUL errors. Request-shell
contracts and 12 comment-decoy mutations bind all three caps, terminal rejections, shared duplicate
checks and integer readers to production adapters. These are host checks, not heap measurements
on a device.

API reply contracts bind the REST body/status helper, command502, unknown-tool code, all five
configuration OTA409 guards, notification/exception policies and the MCP cache-only charge gate.
Eleven comment-decoy mutations exercise those bindings. The real-cJSON state builder checks
both available and unavailable charge snapshots; an unavailable snapshot omits all charge fields
even if a producer accidentally leaves its telemetry flags set. Existing pure cache-policy tests
cover the active-window age boundary and deliberate idle last-known data.

The diagnostic adapter exercises actual formatted capture, RAM ring, chunk framing and fixed-buffer
redaction for connection and WiFi block-ack AP addresses, with embedded CR/LF, forged prefixes, wrap
and truncation. It checks that ordinary raw sinks receive the original bytes and known MQTT URI parser errors
reach all three sinks only as a fixed replacement, including failed ring allocation and repeated
initialization. The host log stub maps the early `vprintf` fallback to a named test facade;
it does not depend on libc symbol interposition or disable fortified stdio or sanitizers.
This is qualified V1-callback evidence; it does not prove closed
WiFi-driver callback framing, a physical exported log or complete sanitation.

| Gate | What it verifies |
|---|---|
| `test_runtime_boundaries.cpp` | Production glue with RTOS/NVS/log/ping stubs: post-unlock sinks, safe partial cleanup, durable-before-restart, task/admission handshakes, callback-generation quarantine, W5500 acquisition unwind |
| `test_diagnostic_export_guidance.py` | Actual bug-form diagnostic description, owned export URL, manual/serial sanitation and nine negative wording mutations |
| HTTP security contract and UI request tests | Actual POST gate ordering, bounded provenance headers, socket/live-mDNS ownership, diagnostic Host checks, redaction selectors, exact UI proof header, no CORS grant and both anti-framing headers; 16 source mutations must fail. IPv6 enablement is rejected; these checks do not establish live network/browser attack behavior. |
| `test_runtime_boundary_contract.py` | Shipped CMake source closure and actual task/callback registration; contained or mechanically bounded C boundaries; fixed mailbox/completion ownership, no forbidden work under locks, production JSON/OTA/MQTT wiring and closed source/flag surface |
| `test_charge_presentation.py` | Compiled actual cache/indicator methods with passive RTOS facades: coherent value/stamp, tick zero/wrap, missing sample, lock failure and post-unlock publication. Pure status fixtures independently age charge while common contact stays fresh; browser fixtures verify the retained-data presentation without commands. |
| `test_command_outcomes.py` | Actual start/stop/default producer bodies and both enqueue helpers feed the real runner with passive RTOS/completion/builders. Matching, inverse, default, missing-counter and unauthenticated outcomes retain one callback and no additional payload. Pure runner tests cover queued-ID policy guards, slot reuse and expired/cancelled/cleared requests; actual REST/MCP emitters print and reparse their completion results. No live command or cryptographic frame is sent. |
| `test_patch_incremental.py` | Shipped CMake admission block and unchanged validator functions with synthetic digest constants; ordinary incremental input/inventory changes stop before the forced compiler, while valid revalidation proceeds. Does not compile tesla-ble or establish real dependency bytes. |
| `run-cjson-oom-tests.sh` | Rehashed pinned cJSON; real producers/parser, finite evcc numeric wire fields, every allocation failpoint, exact IDs, ownership and early input release. Effect-aware reply renderer and extracted real handlers/outer catch exercise degraded outcomes, clock-plus-refused-OTA, typed identity recovery, original restart admission and terminal transport failures. |
| `run-mqtt-json-publish-tests.sh` | Every production discovery/state factory, full/minimal branches and all registry rows; build/print/publish faults cannot replace retained data and rearm discovery. Extracted real discovery rounds refresh the configuration address without changing identity/state topics; missing/racing/failed address reads omit it. Network and broker adapters remain passive. |
| `scripts/check-nvs-contract.py` | Exact registered namespace/key/API/owner/retention/secrecy surface, session aliases and direct NVS calls; wrong-name/API/collision/missing-record mutations |
| `scripts/repo-lint.sh` | Syntax/local links, workflow-policy and host-gate mutation canaries, logic ownership and protocol wiring |
| `tools/agent-config/selftest.sh` / `test_codex_setup.py` | Canonical skills/reviewers/hooks, native adapter parity/trust boundaries, read-only scopes, owner contracts, exact content and approval/artifact canaries |
| `scripts/test-build-contracts.sh` | Targets, pins, partitions, generated artifacts, size/stack/effective-build and publication contracts; validator self-tests isolate offsets, digest syntax, unsigned-prefix/size reuse, production pins, signed padding/image binding and complete Release/Pages bytes with deterministic disposable fixtures. Pages controls cover build-state/CLI consumption, bounded backoff beyond 50 seconds, delayed size/hash/404 failures for each of 16 parts, independent complete retries, bounded persistent failures, separate manifest/Release-byte delegation and immutable-input rejection before polling; no real signing key |
| `scripts/test-pr-gates.sh` | Exact-head review records, relevance, canonical Git command boundaries and shared-library loss using fixtures; `ci-heal.sh` refusals with stubbed `git`/`gh` |
| `test_inline_assets.py` | Bounded CSS byte fixtures, legal/annotation/HTML-context preservation, exact CMake output and unchanged JavaScript, raw-marker rejection, stale-output cleanup and real incremental helper dependency. The Linux browser fixture reuses the helper API but independently compares the ordered raw CSSOM with its actual DOM stylesheet; host CMake tests and target build semantics cover the production generator. |
| `web_ui_browser_gate.py` / `browser_cdp.py` and their policy/transport tests | Linux-only assembled page in Chrome/Chromium: console/request failures, degraded poller, escaping, computed contrast, OTA notes and uncertainty; populated 390/light, 768/light and 1200/dark layouts; real trusted CDP Tab navigation, focus-visible ring and exactly one action each for Enter and Space. Controlled Python peers test pipe framing/limits/FD ownership and cleanup separately from DOM behavior; the host contract executes the actual strict shell with passive tools on Linux/Darwin branches and rejects commented, disabled or early-exit test invocations |
| `web_installer.test.mjs` | Actual installer with passive serial/flasher adapters: phase ordering, abort/release, explicit erase confirmation, and `eraseAll:false` on every Keep write. No physical flash or NVS-preservation proof. |

Automatic browser processes run only on Linux. macOS is refused before binary lookup or process
creation, including an explicit `BROWSER_BIN`. Optional local mode reports a skip; `--require-all`
on macOS invokes the Linux container and fails if it cannot complete the actual browser check. `scripts/run-browser-tests-linux.sh` builds a local
Chromium test image and runs the real three-width gate with no network and a read-only source
mount. The image build needs package access; Docker is required. Run the wrapper once to
prepare a cold image before the strict suite, whose container sub-gate retains a 120 s deadline. Linux CI runs the same DOM gate
in the strict host suite. Launch-policy unit tests use controlled processes and do not prove DOM
behavior. Use the provided in-app Browser for an optional offline visual inspection on macOS.

One isolated browser uses a local CDP pipe (no TCP listener) and a 110 s absolute work deadline,
including its 45 s startup allowance; bounded process-group termination fits within the 120 s outer
gate. CSS hover/focus emulation is restored before trusted keyboard events. The keyboard fixture
starts from the document, retains the actual button handler, replaces only its action with a passive
spy and refuses extra requests. This is browser default activation, not physical typing or a full
accessibility audit. Missing browser
capability must be reported and fails strict mode. Deterministic DOM/presenter tests do not replace
real-browser evidence. Validator `--self-test` modes use representative mutations to show rejection;
firmware compilation and signed-artifact checks remain independent.

## What's *not* covered (by design)

The effect-reply gate compiles functions extracted from the shipped HTTP/MCP adapters and handlers,
not rewritten models. Device/storage/vehicle/RTOS operations remain passive doubles. IDF faults are
return codes with simulated partial bytes; generic renderer spies also throw after the terminal
latch. A separately named shell injection throws after a real handler response to exercise the
actual outer catch. These tests do not claim a C IDF function throws. Browser/CLI regressions keep
received uncertainty visible and forbid automatic follow-up mutations on a degraded channel reply.

`test_ota_admission.cpp` compiles the extracted production OTA start/cache functions in the runtime
boundary target. Passive SDK/task counters cover busy owners, pending and unknown image states,
unavailable locking, task/publication failure cleanup, and fresh admission despite a stale cache.
They never execute an OTA worker. Effect-reply tests separately exercise all typed HTTP outcomes
and preserve an earlier clock effect when OTA is definitely refused.

Host stubs do not exercise actual esp_http_server, NimBLE, flash/NVS, device scheduling, OTA rollback
or the car. Pinned cJSON tests use real emitters but deterministic transport/publish seams.
Four-target IDF build, disposable signing, remote CI, bench/USB, OTA and vehicle E2E are separate
proofs and authorization boundaries. Never infer physical acceptance from green host tests.

Vehicle-free bench ingest validates schema-v2 report identity and plausibility, then fingerprints and
uploads that exact report. Firmware signature, NVS preservation and physical observations are operator
declarations. Final normal-workload stack floors are httpd/vehicle ≥1024 B, MQTT ≥768 B and optional
auto-pair ≥1024 B. Recovery starts/ends with zero boot-failure count, includes ≥4 fault resets to latch
safe mode, then a distinct non-fault reboot (≥5 planned), with final normal stack evidence. These are
policy margins, not hardware-derived alarm thresholds.

`test_pr_preview_served.py` exercises the actual key-free served-preview helper and cleanup
decision producer with injected HTTP/time and an inert local GitHub/publisher facade. It checks
four-target signed fixture admission, source/version/authority pins, complete fresh retries,
end-manifest consistency, response/deadline bounds, and required 404/410 evidence after removal.
The build-gate contract also rejects omitted publication and both cleanup invocation points.
These tests do not publish a preview, remove real Pages content or establish live CDN propagation.

## Adding to it

1. Put bounded hardware-free logic in `main/logic/`; firmware delegates to it.
2. Add meaningful normal/boundary/error/transition CHECKs in `test_logic.cpp`, including repeated
   cycles and quiescence for timers/state machines.
3. Register the concrete owner in `logic_test_ownership.json`; mirrored code also needs parity and
   a negative control proving drift is caught.
4. Run the relevant host gate. Add a CMake/direct-fallback target only when real glue or stubs need it.
5. Update the owning documentation and [feature catalog](../docs/FEATURES.md) for a changed contract.

Use `$add-logic-test` for the detailed workflow. Integration-shell changes still need pinned IDF.

The runtime boundary binary also compiles the shipped background-enqueue method with the real
bounded command runner. A staged foreground-start interleaving must leave its queue and builders
untouched; accepted polls retain read-only, no-wake admission. Passive clock/identity inputs do not
exercise FreeRTOS scheduling, BLE transmission or hardware.

`test_bootloader_preflight.py` executes the shared compatibility CLI and the shipped flash guard
against passive esptool metadata/write adapters for all four targets. Older, unknown, unreadable
or wrong-chip metadata must stop before a stubbed write. Consumer-omission controls pin the local,
preview, ship and recovery paths. These tests do not inspect a board, validate a signature or prove
that a collected bootloader belongs to the chosen port; the delivery plan owns that binding.
Reference-catalog cases (no ESP-IDF version reported) copy the actual checker and use explicitly synthetic catalog
digests. Exact image/full-region positives and byte, padding, provenance-shape and downgrade
negatives exercise CLI admission. Real catalog bytes require separate provenance verification;
fixture success does not establish it. See [reference maintenance](../docs/DEVELOPMENT.md#retained-bootloader-references).

The runtime source contract pins the shipped OTA download chain: fresh manifest precheck,
bounded image-descriptor versions, channel eligibility and exact manifest/image equality precede
bulk download; the abort owner is released only to finish, and reboot requires a successful,
complete finish. Mutation controls include removal of the manifest/image check. This is source
integration evidence, not execution of ESP-IDF's TLS, flash writer or rollback machinery.
Both configurable OTA URLs have unconditional compile-time HTTPS assertions tied to the
host-tested scheme predicate. The predicate does not validate a complete URL or TLS peer.
