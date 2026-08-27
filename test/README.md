# Host-side mock build (`build_mock/`)

A small, hardware-free test target that compiles and runs the project's **pure logic**
with the system toolchain — **no ESP-IDF, no Docker, no USB board**. It gives a real
"run it and see" loop in seconds, in any environment: a local terminal, CI, or a remote coding
agent. Firmware builds still require the pinned Docker workflow, and flash/OTA require both the
right host capabilities and explicit authorization; see [`AGENTS.md`](../AGENTS.md).

## Run it

```bash
scripts/run-mock-tests.sh               # best available local host suite
scripts/run-mock-tests.sh --require-all # fail closed: every CI/Stop prerequisite is mandatory
```

or manually:

```bash
cmake -S test -B build_mock      # build_mock/ is gitignored
cmake --build build_mock
ctest --test-dir build_mock --output-on-failure
```

Normal mode requires a C++17 compiler (`g++`/`clang++`); `cmake` is used when present, and
`scripts/run-mock-tests.sh` otherwise compiles the pure-logic, NVS-adapter and real runtime-boundary
binaries directly with the same sources/flags. Strict mode is what CI and the Stop hook use: it
requires `git`, Python, Node, CMake, a compiler and every named gate file, and gives every subprocess
a timeout. Missing tooling is therefore a red gate, not reduced coverage reported as green.

The narrow pure-logic binary alone can still be run as:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -Imain -o build_mock/logic_tests test/test_logic.cpp
./build_mock/logic_tests
```

The test binaries are dependency-free (no gtest) and return non-zero on the first failed check.

CI runs this as the `logic-test` job, a **fast gate the per-target firmware build
depends on** (`.github/workflows/build.yml`) — a logic regression fails in seconds
instead of after four ESP-IDF builds. The same job also runs
`tools/agent-config/selftest.sh`; that parser-based suite checks the runner-neutral agent
configuration, compatibility manifest/fingerprint, skill frontmatter, reviewer sandboxes, hook
wiring, and Context7 pin before any firmware build starts. Repository/workflow lint, sanitizer
tripwires, deterministic fuzzing, protocol vectors and a real Chrome/Chromium page gate run in the
same job before the pinned four-target firmware build.

## What's covered

The firmware delegates these decision/conversion cores to IDF-free headers under
[`main/logic/`](../main/logic), so the same code the device runs is what gets tested:

| Logic | Header | Firmware call sites |
|-------|--------|---------------------|
| VIN plausibility (17-char, A–Z0–9 ∖ I/O/Q) | `logic/vin.hpp` | `VehicleController::vin_is_plausible`, `/set_vin`, pairing gate |
| Vehicle-stable Home Assistant node id (`teslakey_<vin>`) | `logic/ha_identity.hpp` | `mqtt_ha.cpp` discovery topics, state topics, entity `unique_id`, device identifier |
| Imperial → metric (km, km/h, odometer) | `logic/units.hpp` | MQTT/HA bridge, drive-state telemetry |
| `link_state()` four-state machine + the debounced-ASLEEP asymmetry | `logic/link_state.hpp` | `VehicleController::link_state()` |
| `/status` `link` + MQTT `sleep_status` strings | `logic/link_state.hpp` | `http_status.cpp`, `mqtt_ha.cpp` |
| Per-target platform name + OTA image suffix | `logic/target.hpp` | `platform.hpp` (`TK_PLATFORM`), `ota_update.cpp` |
| OTA canonical version/parser comparison plus exact bounded HTTPS-body state machine (fixed/chunked, truncation, cap+1 and completion) | `logic/ota_contract.hpp` | `ota_update.cpp` manifest intake, version validation and freshness comparison |
| MCP protocol core (version negotiation, JSON-RPC method routing, strict integer validation) | `logic/mcp.hpp` | `mcp_server.cpp` (`/mcp` schema + executor) |
| Shared command registry — REST + MCP names, kinds, per-surface arg keys with ONE bounds pair, `tools/list` row order, and the command-specific evcc boolean-body compatibility rule | `logic/command_registry.hpp` | `http_api.cpp` `/command` body validation + dispatch, `mcp_server.cpp` schema + executor, `command_exec.cpp` |
| `/status` field contract — order, key names, presence rules, value shaping (golden emissions for awake+charging / asleep / unreachable+scan / factory-fresh) | `logic/status_model.hpp` (+ `logic/vehicle_data.hpp` inputs) | `http_status.cpp` `handle_status` (gather + cJSON visitor only) |
| Shared command-outcome text (success / Tesla reason / unreachable) | `logic/command_result.hpp` | `http_api.cpp` `/command` reason, `mcp_server.cpp` tools/call result |
| On-device display presenter (hero priority ladder, SoC gradient, RSSI→bars, SSID scroll, landscape/portrait `Orient` geometry) reading the shared UI snapshot | `logic/display_model.hpp`, `logic/ui_state.hpp` | `display.cpp` renderer `draw_landscape`/`draw_portrait` (via `VehicleController::ui_snapshot()`) |
| Status-LED priority ladder (error/OTA/warn/WiFi/pairing/charging/asleep/SoC) reading the same UI snapshot + latched `LedAlerts` | `logic/led_status.hpp`, `logic/ui_state.hpp` | `led_status.cpp` APA102 task (via `VehicleController::ui_snapshot()`) |
| Shared SoC colour ramp (red→amber→green), one table for the panel fill AND the LED | `logic/soc_gradient.hpp` | `logic/display_model.hpp`, `led_status.cpp` |
| Syslog target parse + send-failure classification (host:port split, errno hard/transient) **and the RFC 5424 PRI** (facility `user`; severity from the line's own esp_log level E/W/I/D→3/4/6/7, leading ANSI colour escape skipped, unprefixed library lines stay `info`, degenerate/short input never reads past the end) | `logic/syslog_policy.hpp` | `syslog.cpp`, `/set_syslog` |
| BLE connect-failure classifier (current-attempt target-name + primary-connectability timestamps, never a fresh SCAN_RSP with the 90 s UI snapshot → out-of-range / at-BLE-limit / connect-failed; its log level; raw scan/GAP/GATT sinks restricted to DEBUG; the background rate limit — first occurrence then hourly heartbeat, a change of cause always reported, a success closing the run, the streak saturating instead of wrapping; foreground attempts never suppressed) | `logic/connect_outcome.hpp` | `ble_client.cpp` + `vehicle_commands.cpp` `ensure_connected_` |
| Active-window poll gate (charging held open only on FRESH contact) | `logic/active_window.hpp` | `vehicle_telemetry.cpp` (`loop_task_fn_`) |
| Web UI Bluetooth-row presenter (which of the five row states, and which countdown belongs beside it) — `ble.scanning` deliberately not an input; parity-checked against the JS that ships in the browser | `logic/ble_row.hpp` — **spec only**, no firmware TU includes it; takes raw `/status` fields and derives `has_vin`/`link_known` itself, so no untested adapter sits between JSON and verdict | `main/www/app.js` BLE_ROW region (`bleRowFromStatus`), held to it by `scripts/check-ble-row-parity.sh` |
| BLE phase countdown (which of the two overlapping phases the Bluetooth row counts down; seconds round UP and 0 is a real answer, not "no countdown"; wrap-safe) | `logic/ble_phase.hpp` | `vehicle_ctrl.hpp` `ble_phase()`, `vehicle_commands.cpp` `ensure_connected_`, `vehicle_pairing.cpp` `idle_until_next_health_poll_`, `http_status.cpp` |
| HA binary `value_template` builder (presence-aware `is defined` guard → "unknown" not phantom OFF) | `logic/ha_templates.hpp` | `mqtt_ha.cpp` (discovery) |
| Exact 55-row HA discovery registry (component/object/domain/field+JSON type/metadata/inversion), topic and `unique_id` construction | `logic/mqtt_discovery_registry.hpp` | `mqtt_ha.cpp`; `test_mqtt_json_publish.cpp` materializes every row against the seven full state payloads |
| POST-body reassembly plus typed Empty/TooLarge/OOM/receive-failure outcomes (loop `recv` to `content_len`, bounded timeout retry, route-specific empty handling and 400/413/503 mapping) | `logic/http_body.hpp` | `http_common.cpp` `read_body_result`, config/REST-command/MCP handlers use the normal 2 KiB API cap; no-argument/legacy-optional commands may accept Empty. Provisioning `POST /save` keeps its separate fixed 1024-byte path and maps empty/oversized input to 400 |
| Device-bound browser-origin decision for mutations (device name/current-IP Host binding, same Host/Origin including default ports, DNS-rebinding/cross-site/null/malformed rejection, state-changing GET classification, headerless evcc/curl compatibility) | `logic/http_origin.hpp` | `http_server.cpp` wildcard dispatcher |
| Negotiated ATT write payload (20-byte pre-MTU fallback, MTU−3, 244-byte cap) | `logic/ble_chunk.hpp` | `ble_client.cpp` MTU callback + generation-bound write loop |
| Deferred NimBLE LinkUp/LinkDown/RX generation policy (stale up/data rejected; ordered down barrier retained) | `logic/ble_deferred_event.hpp` | fixed host-event queue in `vehicle_ctrl.cpp`, consumed by `vehicle_telemetry.cpp`; ready is acknowledged only after successful Vehicle state application |
| Allocation-free bounded JSON classification and raw numeric-ID capture, including malformed raw UTF-8 vs >16-level nesting vs escaped-U+0000 vs valid-but-cJSON-OOM, plus canonical JSON-safe-integer boundaries | `logic/json_syntax.hpp` | `http_config.cpp`, `http_api.cpp`, `mcp_server.cpp` before `cJSON_Parse` |
| Persisted one-string config transaction ordering: reject before load/probe/save/restart, preserve an explicit empty disable, and respond before a successful restart | `logic/config_request.hpp` | `http_config.cpp` `/set_mqtt` and `/set_syslog` |
| Charging-current verification (Tesla ACK vs fresh exact readback) + active-window ChargeState freshness gate | `logic/charge_control.hpp` | `vehicle_commands.cpp` `set_charging_amps`, `vehicle_telemetry.cpp` `get_charge_state` |
| Heap-exhaustion watchdog (4 KB INTERNAL `largest_block` threshold, 5 min UNBROKEN hold, recovery resets the clock, OTA clears the run, 32-bit tick-wrap safety, 5-restart cap, `heap:<n>` breadcrumb round-trip + unparseable-input handling) — plus the escalation's **syslog narration**: armed-once-per-run, the measured (not configured) age of a run, the saturating restart countdown, and OTA-excused vs genuinely-recovered | `logic/heap_watchdog.hpp` | `vehicle_telemetry.cpp` `loop_task_fn_`, `vehicle_ctrl.cpp` `init()`, `main.cpp` (boot breadcrumb line) |
| Per-task stack-headroom status contract (one nested byte-valued block; sampled tasks present, never-sampled/safe-mode tasks absent rather than zero) | `logic/status_model.hpp` | `stack_watch.cpp`; samples in `http_server.cpp`, `vehicle_telemetry.cpp`, `vehicle_pairing.cpp`, `mqtt_ha.cpp` |
| OTA rollback health and operation gate (commit on proven link PLUS uptime floor PLUS INTERNAL largest block ≥4 KiB; setup-mode link exemption; give up past cap; shared OTA/identity/FaultRestart/HealthCommit CAS; both mark-valid paths re-check heap after ownership; only persisted rebooting MQTT/Syslog/WiFi/setup saves may confirm early) | `logic/health_gate.hpp` | `main.cpp` `ota_health_gate_task`, `ota_update.cpp` guards/wrappers, `vehicle_telemetry.cpp`, `http_config.cpp`; owner/heap-return mutation canaries; `/set_vin` and `/gen_keys[?force=1]` are 503-blocked during PendingVerify/unknown/active work and never confirm probation |
| Dual-task start barrier (Creating → Running or Cancelled → exact acknowledgement/reset) | `logic/task_start_gate.hpp` | `vehicle_ctrl.cpp`, `vehicle_telemetry.cpp`, `vehicle_pairing.cpp`; fault-injected second-create failure proves zero protected-resource access before cooperative self-delete |
| NimBLE hidden-host-task acknowledgement (Idle → AwaitingSync → Synced/TimedOut, timeout terminal against a late callback) | `logic/nimble_start_gate.hpp` | `ble_client.cpp`; the runtime matrix also pins sync/reset ordering and sticky saturating reset evidence consumed by OTA health |
| Syslog startup commit gate (Waiting → Running/Cancelled, both terminal) | `logic/syslog_start_gate.hpp` | `syslog.cpp`; the runtime matrix proves pre-commit task isolation, unpublished-queue cleanup on create failure, release/acquire publication on success and no deletion after publication |
| Global essential-runtime admission (Booting → Ready/SafeMode/Fatal, terminal fail-closed states) | `logic/runtime_admission.hpp` | `main.cpp`, vehicle task entries, HTTP/MCP/command/telemetry gates and OTA health admission |
| Generation-owned asynchronous ping completion (late/out-of-order callback rejection, retire vs unstarted abandon) | `logic/ping_probe.hpp` | shared `ping_probe.hpp` lifecycle used by `net.cpp` and `syslog.cpp` |
| MQTT broker URI + save-time pre-flight (the credential-aware `mqtts://` default and its explicit-scheme override, host:port plausibility incl. userinfo, credential-free status/log projection, the probe's contiguous-heap budget, and the outcome→HTTP-status mapping that keeps "refused" apart from "unreachable") | `logic/mqtt_uri.hpp` | `mqtt_ha.cpp` `mqtt_ha_start_impl`, `http_config.cpp` `handle_set_mqtt` |
| MQTT retained-publish ordering/retry (Discovery → availability → state, with any build/print/publish failure rearming the full sequence) | `main/mqtt_publish_sequence.hpp` | `mqtt_ha.cpp` publish task and the production cJSON gate |
| Retained memory trend (the CRC + derived layout fingerprint a `.noinit` image must pass before it is adopted — a zeroed image is explicitly INVALID, since that is what SRAM noise most often looks like — and the carry that keeps one bucket clock across a restart, landing the reboot on a bucket boundary and drawing longer downtime as a gap) | `logic/heap_history.hpp` (`HeapPersist`) | `heap_trend.cpp` |

The target mapping is double-locked: `ota_update.cpp` `static_assert`s its compile-time
image-suffix literal against `tk::image_suffix()`, so the macro and the host-tested
mapping cannot drift.

The display presenter is triple-locked: besides the `CHECK`s above, `run-mock-tests.sh` runs
`scripts/check-display-sim-parity.sh`, which compiles `test/display_golden_dump.cpp` to emit the
C++ presenter's decisions for a set of cases and has `tools/display_sim.py parity` re-decide the
same inputs in Python and diff them — so the pixel-exact offline sim (`display_sim.py`) can't
silently drift from the firmware's `tk::display::compose()`. (Skipped only where `python3` is
absent; the C++ `CHECK`s remain the hard gate.)

A second parity harness does the same for the **web UI**: `scripts/check-ble-row-parity.sh`
compiles `test/ble_row_golden_dump.cpp` to dump `tk::ble::decide()`'s verdict over an exhaustive
input sweep, and has `tools/ble_row_parity.js` re-decide the same inputs with the JavaScript that
actually ships — extracted from the `BLE_ROW` region of `main/www/app.js`, not a copy — and diff
them. So the browser cannot silently drift from the host-tested rules for the Bluetooth row.
(Skipped only where `node` is absent; CI's ubuntu-latest runner ships it, so it does run there.)

The suite also has gates outside the single pure-logic translation unit:

- `test/test_runtime_boundaries.cpp` links real runtime glue to deterministic FreeRTOS/NVS/log
  stubs. It proves diagnostic chunks run after unlock and never mix overwritten/cleared ring
  generations, partial MQTT-probe ownership unwinds in callback-safe order, safe-mode storage
  errors/OOM fail closed, and the heap-watchdog restart is authorized only after its breadcrumb is
  durably written. Missing, malformed, unreadable and uneraseable breadcrumbs are distinct and
  close the restart/activity window conservatively. The same executable compiles the production
  `ping_probe.hpp` against an esp_ping/FreeRTOS stub and covers session-create/start failure,
  reply/no-reply, timeout→stop→late-end quarantine, stale-generation rejection and exact cleanup
  before reuse. Its startup fault matrices also pin NimBLE sync/reset admission, the Syslog
  queue-publication lifetime, and reverse-order W5500 cleanup across every partial acquisition;
  a failed Ethernet driver uninstall deliberately preserves the dependent PHY/MAC/SPI tail and
  fails closed.
- `test/test_runtime_boundary_contract.py` derives its C++ source inventory from the literal
  `main/CMakeLists.txt` `SRCS` block (including nested `.cpp`/`.cc`/`.cxx` files), then derives the
  current task/callback inventory from actual registration calls and callback-bearing structs
  (including the log vprintf hook), and requires
  each C boundary to be contained, delegated to a contained method, or mechanically restricted to
  reviewed fixed-buffer/C/atomic calls. The same closed inventory pins every tesla-ble callback
  setter to a thin named adapter, fixed NimBLE Link/RX deferral, task-owned readiness publication,
  post-Vehicle-lock telemetry parsing, fixed command/status completion records, coherent
  charging-current feedback, atomic crash dismissal and post-unlock OTA string materialization.
  Mutations that restore direct host→Vehicle calls, logging/allocation/NVS under the Vehicle lock,
  stale current feedback or mixed OTA/crash state are rejected. It also gates sticky response construction, early request
  owner release, partial-handle cleanup, persist-before-restart ordering, the real `/status` emitter,
  the production MQTT publish/sequencer plus Discovery and all seven state builders, the global
  default-closed runtime-admission facade/route matrix, the dual vehicle-task Creating/Running/
  Cancelled handshake (including no external delete and TWDT unwind), and both production users of
  the generation-owned ping helper. The OTA integration check pins bounded body read ordering,
  allocation-free syntax/cJSON full-consume/duplicate-root validation, body release before bounded
  version copy and overflow-free comparison; bypass and reordering mutations fail closed. For MQTT
  it derives every `build_*_payload` definition, production call and named OOM/success fixture and
  requires the three inventories to be identical; add/remove/miswire/ninth-factory mutation
  canaries keep that inventory fail closed. All shipped
  sources and headers are scanned for raw cJSON Create/Add bypasses outside `json_builder.hpp`.
  The main-component CMake surface is closed to exactly the one literal registration plus the
  reviewed compile/custom-target commands and one exact four-flag private warning block:
  `target_sources`, escaping sources, included/subdirectory CMake, property-based source attachment
  and forced `-include` options fail. The IDF compile-database gate independently closes every
  firmware C/C++ translation unit—not just `__idf_main`—to the repository, current build, pinned
  IDF or locked managed-component roots. Each TU directly invokes the pinned target compiler, uses
  only trusted include roots, and has a current Ninja/GCC dependency record in those same roots;
  main additionally matches its literal source and recursively reviewed header inventory. Each
  final Ninja compile rule equals the compile-database command plus the exact target-bound
  five-token depfile block before `-o`.

  The closed generated-source set pins/reconstructs the project anchor, three Nanopb sources,
  inlined/gzipped installer pages and X.509 assembly plus their exact Ninja producers. Target
  `ar`/`ranlib` archives may contain only compile-database objects; the final pinned-driver ELF link
  accepts only its two build response files, build/IDF search roots and restricted linker scripts;
  the final `.bin` must come from the exact Python/esptool chip/flash/revision/ELF/digest chain.
  External includes/dependencies/objects/archives/`-L`, linker responses/scripts, link launch/pre/
  post chains and altered generated/app producers have negative canaries. Forced include/macro,
  plugins/specs, preprocessor pass-through, prefix/sysroot, macro/`include_next`, line-splice,
  digraph and trigraph mutations also fail. Compiler include/dependency/search variables including
  `LIBRARY_PATH` are rejected by presence even when empty; extra flags are overwritten, every
  caller `CCACHE_*` is rejected and IDF ccache is forced to exactly `0` before self-test.
- `test/run-cjson-oom-tests.sh` compiles the exact ESP-IDF v5.5.5 cJSON source and injects every
  nth allocation failure through the production `/status` emitter, representative REST/MCP
  envelopes, the real `tools/list` and vehicle-state double-print producers, their shared production
  print/send seam and the parser. It proves bounded status-emitter depth/underflow/finalization,
  exact safe-integer ID echo and reparse, `jsonrpc:"2.0"` validation, recursive duplicate-key
  rejection, pre-cJSON raw UTF-8 rejection, status-before-send, exactly one 503 fallback, zero
  retained input bytes after bounded-ID capture for a maximum-body `tools/list`, leak freedom and ownership;
  CI reruns it with ASan+UBSan+LSan.
- `test/run-mqtt-json-publish-tests.sh` drives all eight production discovery/state factories and
  their full/minimal branches through every cJSON build/print allocation and a publish spy. It also
  checks the exact 55-row registry: unique object/config/`unique_id`, domain→state-topic, every
  registry-referenced payload field/type, component/template/inversion and metadata; add/remove/duplicate/
  miswire/field/topic/boolean canaries prove the comparisons go red. No failed build or publish may
  replace a retained payload, and every failure must rearm discovery; CI reruns with
  ASan+UBSan+LSan.
- `scripts/check-logic-test-ownership.py` requires every `main/logic/*.hpp` to name a concrete
  zero-argument host-test function in `test/logic_test_ownership.json`, proves exactly one definition
  and one invocation (comments/string decoys do not count), and compiles every header standalone.
  Removing an invocation while leaving the assertions in an uncalled function is a mutation canary.
- `scripts/check-nvs-contract.py` pins the exact 19-record namespace/logical-key/stored-key/API/
  owner/retention/secrecy inventory, the two tesla-ble session mappings and every shipped direct
  `nvs_*` call. Add/remove/move/16-byte/collision/wrong-API/unknown-API and nested source/header/
  inline-fragment canaries keep both the static gate and runtime adapter fail closed.
- `scripts/report-firmware-size.py` schema-binds reviewed maxima for each target's unsigned app,
  ELF total, flash code plus rodata, static memory, `.bss` and IRAM. Before comparison it rejects
  negative or inconsistent IDF region sums, remaining capacity, ratios and flash totals. Self-test
  growth mutations exercise every raw-image maximum independently and ensure a failed metric does
  not relabel unrelated rows; the projected Secure-Boot-v2 size versus OTA-slot policy remains a
  separate hard gate.
- `scripts/check-stack-usage.py` consumes every repository-owned GCC `.su` frame from all four
  target builds. Baseline schema v2 review-baselines every target/file/function frame at or above
  256 bytes; all frames remain inventoried and subject to the absolute 4096-byte limit, while any
  unbounded dynamic frame fails. This is per-frame compiler evidence, not a call-depth proof.
- `scripts/check-bench-acceptance.py --self-test` keeps the manual report schema fail closed. Every
  normal/final profile, including recovery after its required clear reboot, must contain
  `httpd`/`vehicle`/`mqtt` minima at or above 1024/1024/768 B; optional `auto_pair` must retain
  1024 B when present. Missing-task and one-byte-headroom mutations prove these are enforced policy
  margins, not implied hardware evidence.
- `scripts/check-dependency-contract.py` byte-pins the IDF image, four lockfiles, resolved
  tesla-ble identity and ordered patch series. `scripts/check-otadata-contract.py` requires the
  initial OTA partition to be exactly `0x2000` erased bytes; the merged-layout gate additionally
  rejects overlaps, non-erased gaps (including NVS), trailing bytes and any skipped target loop.
- `scripts/prepare-reused-release.py` mutation-tests the key-free recovery path for a stale Pages
  deployment: all 40 immutable Release assets are read exactly once through directory-relative
  `O_NOFOLLOW` descriptors, and API size/digest plus every later check and staged output are bound to
  those immutable byte snapshots. A deterministic post-validation path swap must not change staged
  bytes. All 28 diagnostics, four signed app aliases/merged slices and ELF checksums must bind before
  Pages staging; every app is cryptographically checked
  against the production-authority pin and every full merged layout is reconstructed through exact
  EOF. No signer or Release upload/mutation is used, while exactly twelve regular, single-link,
  non-empty app/merged aliases are staged for the new SHA-bound Actions recovery artifact; extra,
  missing, wrong-version, zero-length, symlink and hard-link canaries fail closed. The surrounding
  contract also pins one canonical display-version grammar across build, signer, Pages, manifest,
  Release and bench consumers; leading-zero cores and values over 31 bytes fail closed.
- `scripts/check-workflow-policy.py` and `scripts/check-build-gate-contract.py` require the main
  DAG `logic-test → build → independent-rebuild → publish → deploy`. `publish` owns signing and
  immutable Release acceptance, includes the locally 16/16-bound `_site/` in its exact named
  artifact together with the explicit sixteen signer layout inputs, and never writes `gh-pages`;
  `deploy` has no signing Environment/key/OIDC, revalidates the downloaded twelve-file root plus
  site bytes and binds the root/layout/28 diagnostics against fresh metadata for all 40 immutable
  Release assets, then checks current Release/Pages authority and owns
  branch mutation and live acceptance. Main Actions/Draft and signed-preview upload lists contain
  the exact twelve filenames rather than a root BIN glob, with mutation canaries for a removed DAG
  edge, a wildcard and an extra root file.
- `scripts/run-fuzz-smoke.sh` executes a fixed-seed, 20,000-case property corpus over the bounded
  parsers/codecs. Linux CI recompiles the host binaries and the same fuzz driver under
  ASan+UBSan+LSan after proving all three detectors with deliberate tripwires.
- `test/tesla_protocol_vectors.test.mjs` checks the public Tesla VIN/ECDH/HMAC/AES-GCM vectors and
  all three repository patch contracts. Patch 0003 mutation-tests the one-hour interval, separate
  warning/error clocks, `UINT32_MAX` saturation, log-before-reset order, severity branches and
  exactly six migrated RX-recovery callsites. `test/web_ui_browser_gate.py` assembles
  the shipped page and checks console errors, rejected requests plus the poller's degraded state,
  escaping, keyboard/accessibility semantics and desktop/mobile layout in a real headless
  Chrome/Chromium process.

## What's *not* covered (by design)

The real `esp_http_server` transport/task, NimBLE, flash/NVS hardware, OTA/rollback on silicon and
vehicle behavior remain outside the hardware-free host suite. Response and MQTT envelopes use the
exact pinned cJSON implementation behind deterministic transport/publish seams, but that is not an
on-device allocator, scheduler or network acceptance test. Four-target IDF compilation is a
separate gate; signed delivery, bench evidence and vehicle E2E remain separate
authorization/evidence boundaries. The manual bench workflow is an executable, vehicle-free report
ingest: it validates a privacy-safe closed JSON schema, plausibility and equality to four explicit
operator inputs before fingerprinting and uploading that exact report with no intervening step. Its
report SHA-256 and Actions artifact ID/archive digest identify the report; they do not independently
prove firmware bytes, signature verification, NVS
preservation or physical execution. Report schema v2 records initial/final boot-fail counts;
recovery must start and end at zero, include at least four fault resets to latch safe mode, then a
separate non-fault reboot (at least five planned reboots total).

## Adding to it

Put new hardware-free logic in `main/logic/` (keep it free of IDF/FreeRTOS/NimBLE/NVS/cJSON
includes so it stays host-compilable), have the firmware delegate to it, then add `CHECK(...)` cases
in `test_logic.cpp` and an owner/evidence entry in `test/logic_test_ownership.json`. Add a separate
CMake/direct-fallback target only when real runtime glue or additional stubs are required.
