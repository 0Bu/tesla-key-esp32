# Host tests

Host tests cover pure logic and selected production code with controlled SDK, transport and storage
effects. For the complete handoff and firmware builds, see [Development](../docs/DEVELOPMENT.md#validate-a-change).

## Run it

```bash
scripts/run-mock-tests.sh
scripts/run-mock-tests.sh --require-all
scripts/run-mock-tests.sh --require-all --profile runtime
scripts/run-mock-tests.sh --require-all --profile browser
```

Strict mode fails on missing tools, gate files or timeouts. It requires Git, CMake, C11/C++17
compilers, Python 3.11+, Node, Ruby, jq and rsync. The separate real-library BLE/PSA harness
requires C++20. Strict browser checks on macOS require Docker.

The two profiles are for local iteration. Both include shared protocol and browser parity checks.
Complete handoff uses the full profile. CTest registers all native targets and wrappers. The full
shell runner executes the runtime-boundary, Ethernet cleanup and F137 direct Python gates once
and excludes those duplicates from its CTest invocation. For local result reuse, see
[Development](../docs/DEVELOPMENT.md#validate-a-change).

### Focused checks and sanitizers

```bash
scripts/run-fast-tests.sh --logic
scripts/run-fast-tests.sh --boundary
scripts/run-fast-tests.sh --all
scripts/run-fast-tests.sh --self-test                  # compiler prerequisite only
cmake -S test -B build_mock
cmake --build build_mock
ctest --test-dir build_mock --output-on-failure
```

Run these sanitizer commands on Linux. On macOS, `scripts/check-local.sh` uses pinned IDF Docker.

```bash
scripts/run-sanitizer-tests.sh --self-test
scripts/run-sanitizer-tests.sh
```

The self-test trips ASan, UBSan and LSan on purpose. The run instruments host targets and a
fixed-seed 20,000-case fuzz corpus. Both must complete for sanitizer evidence.

### Browser prerequisites

Use Linux Chrome/Chromium. On macOS, prepare a cold image with
`scripts/run-browser-tests-linux.sh` before the strict 120 s sub-gate. The wrapper builds a
Chromium image, then runs with a read-only source mount and networking disabled. Package access is
needed for the image build. Native macOS launches are refused even with `BROWSER_BIN`.

`web_ui_browser_gate.py` runs on a local CDP pipe with a 110 s work deadline including 45 s for
startup. It checks populated 390, 768 and 1200 px layouts, console and request failures, contrast,
ordered CSS rules, OTA and evcc sheets, and Tab/Enter/Space activation with passive actions.
`browser_cdp.py` policy/transport tests use controlled peers. DOM checks do not replace a full
accessibility audit. Optional local visual inspection can use an offline fixture in the in-app Browser.

<a id="beyond-the-pure-logic-binary"></a>

## Gate index

The [runner](../scripts/run-mock-tests.sh) owns exact order, timeouts and profiles.

| Gate | Coverage |
|---|---|
| `scripts/repo-lint.sh` | Repository syntax, links, workflow and host gate contracts, ESP-IDF inventory and logic ownership |
| `test_codex_setup.py`, `test_hook_read_contract.py`, `tools/agent-config/selftest.sh` | Canonical policy, native adapters and hook boundaries, with controlled payloads |
| `scripts/test-pr-gates.sh`, `test_change_scope.py`, `test_local_checks.py` | Head-bound records, local receipt invalidation and cleanup, selected profiles and failure propagation |
| `scripts/release-relevance.sh --self-test`, `scripts/test-build-contracts.sh` | Release/Dev/PR selectors, four-target layout and artifact contracts, real Pages assembler, disposable RSA fixtures |
| `test_ci_checkout.py`, `test_deploy_candidate.py` | Filtered exact-source Git checkout and actual consumer steps with disposable fixtures |
| `test_pages_publish.py`, `test_pages_publish_control.py`, `test_release_pages_recovery.py`, `test_pages_changelog.py` | Local Git publication, static marker, immutable recovery and change notes, with controlled Git/API/time |
| `test_dependency_sources.py`, `test_dependency_cmake.py`, `test_dependency_harness.py`, `test_dependency_process.py`, `test_patch_incremental.py` | Exact Git/cache admission, actual CMake/runner order, timeout cleanup and mutation refusals |

Standalone `scripts/test-build-contracts.sh` includes release relevance. Strict host runs relevance
and `--build-and-manifest` as two consecutive gates, each with 120 s. Artifact fixtures use test
keys. They establish neither signed delivery nor live Pages behavior.

<a id="whats-covered"></a>

## Evidence by subsystem

### Runtime, storage and fault adapters

- `test_runtime_boundary_contract.py` binds actual source closure and C task/callback registration.
  Runtime tests exercise lock release, exception containment, restart admission and late callback retirement.
- Native targets cover clock/session time, durable config, NVS blob/mirrors and serialization-buffer
  wipe, heap supervision, stack sampling, Ethernet/portal cleanup, Syslog and both crash architectures.
  `test_identity_recovery_contract.py` runs the actual first-key, reload and restart paths.
- `scripts/check-nvs-contract.py` checks registered namespaces, keys, APIs, owners and retention
  against call sites. Device storage and power-loss behavior require separate evidence.

### Logic, BLE protocol and presentation

- [Logic ownership](logic_test_ownership.json) maps pure headers to executable coverage in
  `test_logic.cpp` or another exact owner. Tests cover sleep, caches, health, commands and retries.
- `scripts/test-tesla-ble-harness.sh --require-all` uses the exact patched library, Nanopb and
  pinned Mbed TLS/PSA. It exercises framing, key/session continuity, vectors, authenticated
  response admission, UUID/counter isolation, VCSEC/wake, deferred RX and actual completion.
  Protocol rationale and departures are in [ADR-0005](../docs/adr/0005-tesla-ble-seam.md).
- `test_ble_lifecycle.py`, `test_command_outcomes.py` and `test_charge_presentation.py` exercise
  actual producers and interleavings with passive RTOS/NimBLE, time and transport adapters.
- `tesla_protocol_vectors.test.mjs` uses public test keys. `check-ble-row-parity.sh` compares
  actual JavaScript with the C++ references for BLE and charge presentation.

### HTTP, provisioning and diagnostics

- `run-cjson-oom-tests.sh` uses the pinned parser and actual producers/handlers. It checks bounds,
  ownership, every allocation failpoint, exact MCP IDs, pre-effect refusals and correlated
  degraded replies after effects. `test_diagnostic_export_guidance.py` checks bug-form guidance.
- `test_provision.py`, `test_capture_wake.py` and `test_evcc_vehicle.rb` cover passive client
  flows and the shipped jq mapping, including setter deadlines and the `Starting` state.
- `test_inline_assets.py` exercises the actual CMake generator and incremental dependency.
  `web_ui_http.test.mjs` tests shipped JavaScript with stubbed DOM/fetch, including copy and focus
  return. The separate [Linux browser gate](#browser-prerequisites) exercises actual DOM behavior.

### OTA, bootloader and restart adapters

- `test_ota_admission.cpp` and the shared restart/OTA fixtures exercise actual raw-state
  interpretation, memory admission, ownership, health, rollback decisions, budget expiry,
  stale offers, loss of transport and uncertain terminal effects against passive SDK doubles.
- The cJSON gate drives actual OTA entry, status, update, notes, channel and raw-upload bodies,
  including prefix and hash validation, manifest lifetime and ownership at commit.
- `test_dev_ota_upload.py` covers source/build/image/review binding and consumed attempts.
  `test_bootloader_preflight.py` checks retained-bootloader admission and USB consumer wiring.
- `web_installer.test.mjs` and `serial_port_release.test.mjs` cover installed-layout decisions,
  one serial open, port leases/quarantine, Clean flash confirmation and activation last.
  Serial and flash effects are passive.

<a id="f137-actual-task-host-layer"></a>
<a id="actual-task-and-runtime-layers"></a>

### MQTT and network task adapters

- `run-mqtt-json-publish-tests.sh` exercises production state/discovery builders and retained
  publish failure handling against a passive broker.
- `test_f137_host.py` extracts actual MQTT/network task bodies. It tests WDT unsubscribe before
  self-delete, parked cleanup, iteration containment, fresh gateway proof and late ping ownership.
  Mutations must compile and fail an actual CHECK, followed by a passing restoration. The runner
  quarantines unknown process cleanup. Sanitizer coverage detects an intentional leak before the
  task fixture's `_Exit`.

### Configuration-maintenance backend and transport

`run-config-repair-backend.sh` exercises actual Store, transaction and UART/USB transport
bodies with synthetic records and passive SDK/PSA. It covers both identity journals, single cfg
commit, non-cfg retention, ambiguous effects and terminal transport behavior across four host
routes. The hash/crypto facade and the terminal parked process do not prove real SHA-256, keys,
ABI, flash durability, serial timing or task reclamation. See
[maintenance qualification](../docs/CONFIG_RECOVERY.md#qualification-and-physical-use-boundaries).

### Served preview adapters

`test_pr_preview_served.py` exercises four-target admission, fresh byte checks, bounded retries
and required 404/410 after removal with controlled HTTP/time and a passive publisher. See
[served acceptance](../docs/DEVELOPMENT.md#served-pr-preview-acceptance) for live evidence.

## What's *not* covered (by design)

Host stubs cannot establish actual SDK scheduling, radio behavior, flash/NVS durability or OTA
rollback. Record four-target IDF compilation, disposable signing, remote CI, browser,
USB/bench, OTA and vehicle/API evidence separately. Bench ingest checks report identity,
stack floors and recovery sequence. Firmware, NVS and physical observations require actual
same-board evidence. [Report policy](../scripts/check-bench-acceptance.py) ·
[Bench procedure](../.agents/skills/deploy-bench/SKILL.md)

## Adding to it

1. Put bounded pure logic in `main/logic/` and link its executable checks in
   [logic ownership](logic_test_ownership.json).
2. Cover boundaries and transitions, including repeats and quiescence for state machines. Mirrored
   code needs parity and a mutation that proves drift is caught.
3. Add an integration target when actual glue needs SDK facades. Run focused checks before the
   full handoff. Changes to firmware integration also need pinned IDF compilation.
4. Update the owner document and [feature catalog](../docs/FEATURES.md) for a changed contract.
