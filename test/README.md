# Host-side mock build (`build_mock/`)

A small, hardware-free test target that compiles and runs the project's **pure logic** with the
system toolchain — **no ESP-IDF, no Docker, no USB board**. It gives a "run it and see" loop in
seconds in any environment: a local terminal, CI or a remote coding agent. Firmware builds still need
the pinned Docker workflow, and flash/OTA need the right host capabilities plus explicit
authorization; see [`AGENTS.md`](../AGENTS.md).

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

Normal mode needs a C++17 compiler (`g++`/`clang++`); `cmake` is used when present, otherwise
`scripts/run-mock-tests.sh` compiles the pure-logic, NVS-adapter and runtime-boundary binaries
directly with the same sources and flags. Strict mode is what CI and the Stop hook use: it requires
`git`, Python, Node, CMake, a compiler and every named gate file and gives each subprocess a timeout,
so missing tooling is a red gate, not reduced coverage reported as green. The narrow pure-logic binary
alone:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -Imain -o build_mock/logic_tests test/test_logic.cpp
./build_mock/logic_tests
```

The binaries are dependency-free (no gtest) and exit non-zero on the first failed check.

CI runs this as the `logic-test` job (`.github/workflows/build.yml`) next to its sibling `logic-harness`
job; the final `build` check depends on **both**. They run concurrently with each other and with the
four per-target firmware compiles, which only wait for the ~20 s `prepare` job (run mode and release
version), so the pipeline takes as long as the slowest of them instead of their sum. A logic
regression still fails the `build` check. The `logic-test` job also runs `tools/agent-config/selftest.sh`
(runner-neutral agent config, skill frontmatter, reviewer sandboxes, hook wiring, Context7 pin),
repository/workflow lint, deterministic fuzzing, protocol vectors and a real Chrome/Chromium page gate;
`logic-harness` runs the tesla-ble harness, the sanitizer tripwires, the build and PR-gate contract
self-tests.

`scripts/test-tesla-ble-harness.sh --require-all` (`logic-harness` job) builds the real `yoziru/tesla-ble` v5.2.0
with the repository patch series, Nanopb and the exact Mbed TLS 4 / TF-PSA-Crypto commit of the pinned
IDF image (`MBEDTLS_REF`) on the host and runs `test/test_tesla_ble_harness.cpp`. It calls the
production helpers themselves — `tk::build_ble_tx_frame` / `tk::is_well_formed_ble_frame` (the
`drive_command_runner_()` TX path) and `tk::regenerate_private_key()` (behind
`regenerate_key_native_()`) — so a regression fails there, not only in a copy. Its V1 known-answer
tests pin the PSA crypto port (patch 0006) to the protocol bytes: the official client key and ECDH
session key, session-info HMAC and tag, a reference AES-GCM vector and `Peer::encrypt` output,
response metadata with a response counter that differs from the request counter (an AAD from the
request counter and a tampered tag are both refused), the VIN BLE name, a PEM export byte-identical to
the Mbed TLS 3.6 one kept in NVS, and P-384/garbage key refusal. The firmware key fingerprint is
checked through a host mirror of `compute_key_fingerprint_()`, which is IDF code and not compiled on
the host.

## What's covered

The firmware delegates these decision/conversion cores to IDF-free headers under
[`main/logic/`](../main/logic), so the code the device runs is what gets tested:

| Logic | Header | Firmware call sites |
|-------|--------|---------------------|
| VIN plausibility (17 chars, A–Z0–9 ∖ I/O/Q) | `vin.hpp` | `VehicleController::vin_is_plausible`, `/set_vin`, pairing gate |
| Vehicle-stable HA node id (`teslakey_<vin>`) | `ha_identity.hpp` | `mqtt_ha.cpp` topics, `unique_id`, device identifier |
| Imperial → metric (km, km/h, odometer) | `units.hpp` | MQTT/HA bridge, drive telemetry |
| `link_state()` four-state machine, debounced-ASLEEP asymmetry, `/status` `link` + MQTT `sleep_status` strings | `link_state.hpp` | `VehicleController::link_state()`, `http_status.cpp`, `mqtt_ha.cpp` |
| Per-target platform name + OTA image suffix | `target.hpp` | `platform.hpp` (`TK_PLATFORM`), `ota_update.cpp` |
| OTA version grammar/comparison + exact bounded HTTPS-body state machine | `ota_contract.hpp` | `ota_update.cpp` manifest intake and freshness |
| MCP core (version negotiation, JSON-RPC routing, strict integer validation) | `mcp.hpp` | `mcp_server.cpp` |
| Shared command registry (REST + MCP names, kinds, per-surface arg keys with ONE bounds pair, `tools/list` order, evcc boolean-body rule) | `command_registry.hpp` | `http_api.cpp`, `mcp_server.cpp`, `command_exec.cpp` |
| `/status` and evcc `/vehicle_data` field contract (order, keys, presence, shaping; golden emissions for awake+charging / asleep / unreachable+scan / factory-fresh; evcc charge/climate/drive, exact bounded selectors, the exhaustive 8x8 selection x availability table, the 300 s in-window climate freshness boundary, odometer miles and unreported-preconditioning-as-false) | `status_model.hpp`, `vehicle_data.hpp` | `http_status.cpp` `handle_status`, `http_api.cpp` `handle_vehicle_data` |
| Command-outcome text and failure-origin classification behind the soft-desync link backstop | `command_result.hpp` | `http_api.cpp`, `mcp_server.cpp`, `vehicle_commands.cpp` `make_result_cb_` |
| Display presenter (priority ladder, SoC gradient, RSSI→bars, SSID scroll, `Orient` geometry) on the shared UI snapshot | `display_model.hpp`, `ui_state.hpp` | `display.cpp` via `ui_snapshot()` |
| Status-LED priority ladder + latched `LedAlerts` | `led_status.hpp`, `ui_state.hpp` | `led_status.cpp` |
| SoC colour ramp shared by panel and LED | `soc_gradient.hpp` | `display_model.hpp`, `led_status.cpp` |
| Syslog target parse, send-failure classification, RFC 5424 PRI from each line's log level | `syslog_policy.hpp` | `syslog.cpp`, `/set_syslog` |
| BLE connect-failure classifier (current-attempt evidence, level, hourly rate limit, change-of-cause always reported, foreground never suppressed) | `connect_outcome.hpp` | `ble_client.cpp`, `vehicle_commands.cpp` `ensure_connected_` |
| Active-window poll gate (charging holds it open only on FRESH contact) | `active_window.hpp` | `vehicle_telemetry.cpp` |
| One-shot charge poll on self-wake edge + stale-cache bootstrap (retry backoff, episode latch) | `wake_poll.hpp` | `vehicle_telemetry.cpp` |
| VCSEC ASLEEP-run clock and sleep proof (`AWAKE` and `UNKNOWN` end the run; the current raw report must still be `ASLEEP`; tick-wrap safe) | `command_runner.hpp` (`next_asleep_since`, `vcsec_asleep_proven`) | `vehicle_ctrl.hpp` `note_vcsec_sleep_()` / `vcsec_stably_asleep_()`, `vehicle_telemetry.cpp` |
| Key-rotation boot cleanup and journal retirement (every erase attempted, `key_created` before the journal, journal last and only after all succeeded; also run against the real NVS adapter in `test_nvs_storage.cpp`) | `key_rotation.hpp` (`run_key_rotation_boot_cleanup`, `retire_key_rotation_journal`) | `vehicle_ctrl.cpp` `recover_pending_key_rotation_at_boot_()`, `vehicle_pairing.cpp` `finish_key_rotation_cleanup_()` |
| Web UI Bluetooth-row presenter (row state + countdown; `ble.scanning` deliberately not an input) — **spec only**, no firmware TU includes it | `ble_row.hpp` | `main/www/app.js` BLE_ROW region, held to it by `scripts/check-ble-row-parity.sh` |
| BLE phase countdown (round-up seconds, 0 is a real answer, wrap-safe) | `ble_phase.hpp` | `vehicle_ctrl.hpp`, `vehicle_commands.cpp`, `vehicle_pairing.cpp`, `http_status.cpp` |
| HA binary `value_template` builder (presence guard → "unknown", not phantom OFF) | `ha_templates.hpp` | `mqtt_ha.cpp` |
| Exact 55-row HA discovery registry, topic and `unique_id` construction | `mqtt_discovery_registry.hpp` | `mqtt_ha.cpp`; `test_mqtt_json_publish.cpp` materializes every row against the seven state payloads |
| POST-body reassembly with typed Empty/TooLarge/OOM/receive-failure outcomes and a progress-independent 15 s monotonic budget | `http_body.hpp` | `http_common.cpp` `read_body_result` and the config/REST/MCP handlers (2 KiB cap; `POST /save` keeps its own 1024-byte path); fake-clock slow-byte/timeout sequences, final-byte deadline, rollover and release ownership |
| Browser-origin decision for mutations (Host/Origin binding, rebinding/cross-site/null rejection, state-changing GET classification, headerless compatibility) | `http_origin.hpp` | `http_server.cpp` |
| Negotiated ATT write payload (20-byte fallback, MTU−3, 244 cap) | `ble_chunk.hpp` | `ble_client.cpp` |
| Deferred NimBLE LinkUp/LinkDown/RX generation policy | `ble_deferred_event.hpp` | `vehicle_ctrl.cpp`, `vehicle_telemetry.cpp` |
| Allocation-free bounded JSON classification, raw numeric-ID capture, canonical safe-integer bounds | `json_syntax.hpp` | `http_config.cpp`, `http_api.cpp`, `mcp_server.cpp` before `cJSON_Parse` |
| Persisted config transaction order (reject → load → probe → save → respond → restart; empty = disable) | `config_request.hpp` | `http_config.cpp` `/set_mqtt`, `/set_syslog` |
| Charging-current verification (ACK vs fresh exact readback) + active-window ChargeState freshness | `charge_control.hpp` | `vehicle_commands.cpp`, `vehicle_telemetry.cpp` |
| Heap watchdog (4 KB INTERNAL threshold, 5 min unbroken hold, OTA clears the run, tick-wrap safety, 5-restart cap, `heap:<n>` breadcrumb) and its syslog narration | `heap_watchdog.hpp` | `vehicle_telemetry.cpp`, `vehicle_ctrl.cpp` `init()`, `main.cpp` |
| Per-task stack-headroom status contract (never-sampled tasks absent, not zero) | `status_model.hpp` | `stack_watch.cpp` and its four sampling sites |
| OTA rollback health/operation gate (proven link + uptime + heap; setup-mode exemption; cap; shared OTA/identity/FaultRestart/HealthCommit CAS) | `health_gate.hpp` | `main.cpp`, `ota_update.cpp`, `vehicle_telemetry.cpp`, `http_config.cpp` |
| Dual-task start barrier (Creating → Running/Cancelled → acknowledgement) | `task_start_gate.hpp` | `vehicle_ctrl.cpp`, `vehicle_telemetry.cpp`, `vehicle_pairing.cpp` |
| NimBLE hidden-host acknowledgement (timeout terminal against a late callback) | `nimble_start_gate.hpp` | `ble_client.cpp` |
| Syslog startup commit gate | `syslog_start_gate.hpp` | `syslog.cpp` |
| Global runtime admission (`Booting → Ready/SafeMode/Fatal`) | `runtime_admission.hpp` | `main.cpp`, vehicle task entries, HTTP/MCP/command gates, OTA health |
| Generation-owned asynchronous ping completion | `ping_probe.hpp` | `ping_probe.hpp` shell used by `net.cpp`, `syslog.cpp` |
| MQTT broker URI + save-time pre-flight (credential-aware `mqtts://` default, credential-free projection, probe heap budget, outcome→HTTP mapping) | `mqtt_uri.hpp` | `mqtt_ha.cpp`, `http_config.cpp` `handle_set_mqtt` |
| MQTT retained-publish ordering/retry | `main/mqtt_publish_sequence.hpp` | `mqtt_ha.cpp` |
| Retained heap-trend validity (CRC + layout fingerprint, zeroed image invalid, bucket-clock carry) | `heap_history.hpp` (`HeapPersist`) | `heap_trend.cpp` |
| Pure BLE RX/TX framing (2-byte BE length reassembly, timeout, no heuristic recovery) | `rx_framing.hpp` | RX/TX paths in `vehicle_telemetry.cpp` |

`ota_update.cpp` `static_assert`s its compile-time image-suffix literal against `tk::image_suffix()`,
so the macro and the host-tested mapping cannot drift.

**Parity harnesses.** The display presenter and the web UI's Bluetooth row are each mirrored outside
C++, and a golden dump keeps the mirror honest:

- `scripts/check-display-sim-parity.sh` compiles `test/display_golden_dump.cpp` to emit
  `tk::display::compose()` decisions for a case set and has `tools/display_sim.py parity` re-decide
  the same inputs in Python. Skipped only where `python3` is absent; the C++ `CHECK`s remain the
  hard gate.
- `scripts/check-ble-row-parity.sh` compiles `test/ble_row_golden_dump.cpp` to dump
  `tk::ble::decide()` over an exhaustive input sweep and has `tools/ble_row_parity.js` re-decide it
  with the JavaScript that actually ships (extracted from the `BLE_ROW` region of `main/www/app.js`,
  not a copy). Skipped only where `node` is absent; CI's runner has it.

## Beyond the pure-logic binary

The suite also has gates outside the single pure-logic translation unit:

- **`test/test_runtime_boundaries.cpp`** links real runtime glue to deterministic FreeRTOS/NVS/log
  stubs. It proves diagnostic chunks run after unlock and never mix ring generations, partial
  MQTT-probe ownership unwinds in callback-safe order, safe-mode storage errors/OOM fail closed, and
  the heap-watchdog restart is authorized only after its breadcrumb is durably written (missing,
  malformed, unreadable and uneraseable breadcrumbs are distinct and close the restart/activity window
  conservatively). It also compiles the production `ping_probe.hpp` against an esp_ping/FreeRTOS stub
  (create/start failure, reply/no-reply, timeout→stop→late-end quarantine, stale-generation rejection)
  and pins NimBLE sync/reset admission, Syslog queue-publication lifetime and reverse-order W5500
  cleanup across every partial acquisition.
- **`test/test_runtime_boundary_contract.py`** derives the C++ inventory from the literal
  `main/CMakeLists.txt` `SRCS` block (nested `.cpp`/`.cc`/`.cxx` included), then the task/callback
  inventory from actual registration calls and callback-bearing structs (including the log vprintf
  hook). Each C boundary must be contained, delegated to a contained method, or restricted to reviewed
  fixed-buffer/C/atomic calls. The same closed inventory pins every tesla-ble callback setter to a thin
  named adapter and gates fixed NimBLE Link/RX deferral, task-owned readiness, zero-wait lifecycle
  locking, post-Vehicle-lock telemetry parsing, fixed command/status completion records, coherent
  charging-current feedback, atomic crash dismissal, whole-snapshot manifest publication, post-unlock
  OTA string materialization, sticky response construction, early owner release, partial-handle
  cleanup, persist-before-restart ordering, the real `/status` and selected `/vehicle_data` emitters,
  cache-only climate and drive getters (the climate freshness stamp written inside the guarded
  epoch branch, the exact `get_vehicle_climate` decision inputs and its lock-failure clear-out-and-
  return-false path, the pairing reset of all four freshness values: charge and climate tick plus
  generation), the drive companion poll gated by `wake_poll_refreshes_drive` and ordered after the
  charge poll, the rising-edge `tele_idx` reset, the shared `build_drive_state_poll`, post-unlock
  heap-adoption/runner-exception diagnostics, the production MQTT
  publish/sequencer and all seven state builders, the default-closed runtime-admission facade, the
  dual vehicle-task handshake (no external delete, TWDT unwind) and both users of the ping helper.
  The OTA check pins bounded body-read ordering, allocation-free syntax/cJSON full-consume/duplicate-
  root validation, body release before version copy and overflow-free comparison. For MQTT it derives
  every `build_*_payload` definition, production call and OOM/success fixture and requires the three
  inventories to be identical; add/remove/miswire/ninth-factory canaries keep it fail closed. All
  shipped sources and headers are scanned for raw cJSON Create/Add bypasses outside `json_builder.hpp`.
  The main-component CMake surface is closed to one literal source registration, the reviewed
  compile/custom-target commands and one exact four-flag warning block (`target_sources`, escaping
  sources, included CMake, property-based attachment and forced `-include` fail).
  The IDF compile-database and effective-build checks are described in the
  [feature catalog](../docs/FEATURES.md#6-build-test-and-ci).
- **`test/run-cjson-oom-tests.sh`** compiles the exact cJSON the firmware links (the
  `espressif/cjson` registry release all four lockfiles pin, re-hashed against the locked component
  hash each run) and injects every n-th allocation failure through the production `/status` emitter,
  full charge/climate/drive/combined REST and representative MCP envelopes, the real `tools/list`
  and vehicle-state producers, the shared
  print/send seam and the parser. It proves bounded emitter depth/finalization, exact safe-integer id
  echo, `jsonrpc:"2.0"` validation, recursive duplicate-key rejection, pre-cJSON UTF-8 rejection,
  status-before-send, exactly one 503 fallback, zero retained input bytes after id capture for a
  maximum-size `tools/list`, and leak freedom. CI reruns it under ASan+UBSan+LSan.
- **`test/run-mqtt-json-publish-tests.sh`** drives all eight production discovery/state factories and
  their full/minimal branches through every cJSON build/print allocation and a publish spy, and checks
  the 55-row registry (unique object/config/`unique_id`, domain→topic, every referenced payload
  field/type, template/inversion/metadata). No failed build or publish may replace a retained
  payload, and every failure must re-arm discovery; ASan+UBSan+LSan in CI.
- **`scripts/check-logic-test-ownership.py`** requires every `main/logic/*.hpp` to name a concrete
  zero-argument host-test function in `test/logic_test_ownership.json`, proves exactly one definition
  and one invocation (comments and string decoys do not count) and compiles every header standalone.
- **`scripts/check-nvs-contract.py`** pins the 19-record namespace/key/API/owner/retention/secrecy
  inventory, the two tesla-ble session mappings and every shipped direct `nvs_*` call; add/remove/
  move/16-byte/collision/wrong-API canaries keep the static gate and runtime adapter fail closed.
- **`scripts/report-firmware-size.py`**, **`check-stack-usage.py`**, **`check-bench-acceptance.py`**,
  **`check-dependency-contract.py`**, **`check-otadata-contract.py`**,
  **`prepare-reused-release.py`** and **`check-workflow-policy.py`** / **`check-build-gate-contract.py`**
  each carry a `--self-test` that mutation-tests their schema, baseline, recovery or DAG contract
  (what they protect: [feature catalog §6](../docs/FEATURES.md#6-build-test-and-ci) and
  [SECURITY](../docs/SECURITY.md#release-pipeline-and-trust-boundaries)).
- **`scripts/run-fuzz-smoke.sh`** runs a fixed-seed 20,000-case property corpus over the bounded
  parsers/codecs; Linux CI recompiles the host binaries and the fuzz driver under ASan+UBSan+LSan
  after proving all three detectors with deliberate tripwires.
- **`test/tesla_protocol_vectors.test.mjs`** checks the public Tesla VIN/ECDH/HMAC/AES-GCM vectors and
  the repository patch contracts that remain (0004 parental-controls trim, 0005 signer.go counter
  alignment; the crypto port 0006 is proven by the C++ harness above). It uses public test keys only.
- **`test/web_ui_browser_gate.py`** assembles the shipped page and checks console errors, rejected
  requests and the poller's degraded state, escaping, keyboard/accessibility semantics and
  desktop/mobile layout in real headless Chrome/Chromium. Each profile has a 45 s cold-start/DOM
  deadline so a loaded runner does not flake, inside the host gate's 120 s fail-closed limit.

## What's *not* covered (by design)

The real `esp_http_server` transport/task, NimBLE, flash/NVS hardware, OTA/rollback on silicon and
vehicle behavior are outside the hardware-free suite. Response and MQTT envelopes use the exact
pinned cJSON behind deterministic transport/publish seams, which is not an on-device allocator,
scheduler or network test. Four-target IDF compilation is a separate gate, and signed delivery, bench
evidence and vehicle E2E are separate authorization/evidence boundaries. The manual bench workflow is
a vehicle-free report *ingest*: its digests identify the report, not firmware bytes, signature
verification, NVS preservation or physical execution
([FEATURES §6](../docs/FEATURES.md#6-build-test-and-ci)).

## Adding to it

Put new hardware-free logic in `main/logic/` (no IDF/FreeRTOS/NimBLE/NVS/cJSON includes, so it stays
host-compilable), have the firmware delegate to it, then add `CHECK(...)` cases in `test_logic.cpp` and
an owner/evidence entry in `test/logic_test_ownership.json`. Add a separate CMake/direct-fallback
target only when real runtime glue or extra stubs are required.
