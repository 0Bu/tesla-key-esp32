# Host tests

The host suite exercises IDF-free decisions and selected production glue with deterministic stubs.
It does not require Docker, USB or a vehicle. Firmware compilation is a separate
[development gate](../docs/DEVELOPMENT.md#validate-a-change).

## Run it

```bash
scripts/run-mock-tests.sh               # available local checks, reports missing capabilities
scripts/run-mock-tests.sh --require-all # fail closed: CI/Stop prerequisites are mandatory
```

Normal mode needs a C++17 compiler, using CMake when present or equivalent direct compilation.
The separate real v6.2.0 tesla-ble/PSA harness requires C++20; pure firmware logic remains C++17.
Strict mode requires Git, Python, Node, CMake, compiler and every gate file, with bounded subprocesses.
Missing tooling is failure, not reduced coverage reported green. A failed check fails its binary:
the pure-logic suite and the tesla-ble harness report every failed check before exiting 1, while the
NVS, runtime-boundary, cJSON OOM and MQTT JSON binaries and the fuzz smoke stop at the first failure.

For a narrow CMake iteration:

```bash
cmake -S test -B build_mock
cmake --build build_mock
ctest --test-dir build_mock --output-on-failure
```

CI splits host evidence into `logic-test` and `logic-harness`; both run alongside producer and
independent per-target compiles, and final `build` requires both host jobs. Python gates use
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
| Decisions/state machines | `test_logic.cpp`: VIN/identity, units, link/sleep, active window/wake retry, commands, OTA versions/intake, config/storage, watchdog, admission, asynchronous generations, presenters |
| Real tesla-ble/PSA seam | `scripts/test-tesla-ble-harness.sh --require-all`: pinned library/patches, Nanopb and pinned Mbed TLS4/TF-PSA-Crypto; production frame/key helpers and V1 crypto vectors |
| Presenter mirrors | `check-display-sim-parity.sh`, `check-ble-row-parity.sh`: golden C++ decisions against simulator and shipped JavaScript |
| Public protocol vectors | `tesla_protocol_vectors.test.mjs`: VIN, ECDH/HMAC/AES-GCM and patch contracts, public test keys only |
| External JSON contract | Golden status output, command registry/bounds, MQTT registry/fields/presence and exact MCP IDs |

The v6.2.0 real-library harness also checks fresh/forced/established/cleared-secret peer admission,
monotonic session counters, domain request-hash isolation and invalid-domain lookup.
The crypto harness checks official client/session derivation, HMAC/tag, AES-GCM, response-counter
AAD (different from request counter), tampered-tag refusal, PEM compatibility and invalid keys.
Disabling the real PSA RNG must refuse handshake and payload construction without emitting a frame.
Firmware key fingerprint uses a host mirror because the IDF function is not compiled on the host.
Presenter parity likewise proves decisions, not physical pixels.

## Beyond the pure-logic binary

| Gate | What it verifies |
|---|---|
| `test_runtime_boundaries.cpp` | Production glue with RTOS/NVS/log/ping stubs: post-unlock sinks, safe partial cleanup, durable-before-restart, task/admission handshakes, callback-generation quarantine, W5500 acquisition unwind |
| `test_runtime_boundary_contract.py` | Shipped CMake source closure and actual task/callback registration; contained or mechanically bounded C boundaries; fixed mailbox/completion ownership, no forbidden work under locks, production JSON/OTA/MQTT wiring and closed source/flag surface |
| `run-cjson-oom-tests.sh` | Rehashed pinned cJSON; every allocation failure in real status/MCP producers and REST envelopes, parser and reply seam; sticky ownership, complete response or one 503, exact IDs, early input release, no leaks |
| `run-mqtt-json-publish-tests.sh` | Every production discovery/state factory, full/minimal branches and all registry rows; build/print/publish faults cannot replace retained data and rearm discovery |
| `scripts/check-nvs-contract.py` | Exact registered namespace/key/API/owner/retention/secrecy surface, session aliases and direct NVS calls; wrong-name/API/collision/missing-record mutations |
| `scripts/repo-lint.sh` | Syntax/local links, workflow-policy and host-gate mutation canaries, logic ownership and protocol wiring |
| `tools/agent-config/selftest.sh` / `test_codex_setup.py` | Canonical skills/reviewers/hooks, native adapter parity/trust boundaries, read-only scopes, owner contracts, exact content and approval/artifact canaries |
| `scripts/test-build-contracts.sh` | Targets, pins, partitions, generated artifacts, size/stack/effective-build and publication contracts; no real signing key |
| `scripts/test-pr-gates.sh` | Exact-head review records, relevance, canonical Git command boundaries and shared-library loss using fixtures; `ci-heal.sh` refusals with stubbed `git`/`gh` |
| `web_ui_browser_gate.py` | Actual assembled page in Chrome/Chromium: console/request failures, degraded poller, escaping, keyboard/accessibility and desktop/mobile layout |

Browser profiles have a 45 s cold-start/DOM deadline within the 120 s outer gate. Missing browser
capability must be reported and fails strict mode. Deterministic DOM/presenter tests do not replace
real-browser evidence. Validator `--self-test` modes use representative mutations to show rejection;
firmware compilation and signed-artifact checks remain independent.

## What's *not* covered (by design)

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

## Adding to it

1. Put bounded hardware-free logic in `main/logic/`; firmware delegates to it.
2. Add meaningful normal/boundary/error/transition CHECKs in `test_logic.cpp`, including repeated
   cycles and quiescence for timers/state machines.
3. Register the concrete owner in `logic_test_ownership.json`; mirrored code also needs parity and
   a negative control proving drift is caught.
4. Run the relevant host gate. Add a CMake/direct-fallback target only when real glue or stubs need it.
5. Update the owning documentation and [feature catalog](../docs/FEATURES.md) for a changed contract.

Use `$add-logic-test` for the detailed workflow. Integration-shell changes still need pinned IDF.
