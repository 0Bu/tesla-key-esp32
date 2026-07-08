# Architecture Review — tesla-key-esp32

**Date:** 2026-07-08
**Reviewed at:** `main` @ `fa23c82` (post-#167, firmware line v1.4.x)
**Scope:** whole-firmware architecture — module decomposition, testing strategy, concurrency
model, dependency management, build/release pipeline, security posture. Not a line-by-line
code review (that is `/project-review`); this evaluates the *structure* and proposes
concrete, prioritized implementation steps.

---

## 1. Executive summary

The architecture is in **very good shape for its class** — a single-purpose, memory-tight
embedded firmware. Its standout property is that hard-won operational lessons (OOM aborts,
BLE coexistence, framer desyncs, false-asleep states, pairing loss) have been converted into
*structural* mechanisms — a host-tested pure-logic layer, compile-time guard enforcement,
single-source-of-truth state machines — rather than remaining tribal knowledge.

The main structural debt is the breadth of `VehicleController` (a god object that owns
commands, telemetry, pairing, link-state, UI snapshot *and* generic config persistence).
The main strategic risk is the dependency cliff under `yoziru/tesla-ble` v5.1.1
(ESP-IDF 6 / mbedTLS 4 incompatibility, plus a local build-time patch for the esp32c5
target that must survive every upstream bump).

**Priorities at a glance:**

| # | Proposal | Value | Effort | Risk |
|---|----------|-------|--------|------|
| P1 | Document the concurrency contract (lock hierarchy + task inventory) | High | Low | None |
| P2 | Move config persistence off `VehicleController` | Medium | Low | Low |
| P3 | Extract `/status` + command-dispatch shaping into `logic/` (host-tested), following the MCP pattern | High | Medium | Low |
| P4 | Upstream the esp32c5 target to `yoziru/tesla-ble`; retire the local patch | High | Low (PR) | None |
| P5 | Plan the IDF-6/mbedTLS-4 crypto seam (issue #61) as an ADR now, port later | High | Medium | Medium |
| P6 | Optional bearer token for `/mcp` only | Medium | Low | Low |
| P7 | Owned BLE-ops queue in front of tesla-ble | Medium | High | Medium — **defer** |

Two small known defects are listed in §4 (F7, F8) with one-line fixes.

---

## 2. System context

One ESP32-family device bridges HTTP (evcc, web UI, MQTT/HA, MCP) to a Tesla vehicle over
BLE, using `yoziru/tesla-ble` for the signed-command protocol. Five chip targets build from
one source tree; CI builds, signs and publishes all five; devices self-update via pull OTA.
~8.3 k lines of firmware C++, ~1.1 k lines of vanilla web UI, 171 host-run logic assertions.

```
HTTP (evcc / web UI / MCP)      MQTT (HA discovery)
        │                             │
  http_*.cpp (thin, guarded)     mqtt_ha.cpp
        └──────────┬──────────────────┘
            VehicleController  ←— the hub (see F1)
            ├─ vehicle_commands.cpp   (sync command API)
            ├─ vehicle_telemetry.cpp  (bg poll + caches)
            ├─ vehicle_pairing.cpp    (auto-pair, key lifecycle)
            └─ logic/*.hpp            (pure, host-tested decisions)
        ┌──────────┴──────────┐
   ble_client.cpp        nvs_storage.cpp     ← adapter seams
   (NimBLE GATT)         (NVS keys ≤15 chars)
        │
  yoziru/tesla-ble v5.1.1  (pinned; C5 via local build-time patch)
```

---

## 3. What is architecturally sound

These are strengths worth *protecting* — several are unusual for hobby-scale embedded
projects and should be treated as load-bearing conventions.

**S1 — The host-tested pure-logic layer is the right testing architecture for embedded.**
`main/logic/` holds IDF-free headers (link-state machine, unit conversion, VIN validation,
MCP protocol core, display presenter, LED ladder, SoC gradient); `scripts/run-mock-tests.sh`
compiles and runs them with the plain host toolchain in seconds, and CI gates the firmware
build on it (`logic-test` job). The device runs *the same headers* the test runs — no
mock drift. This is the project's most valuable structural asset. Every new decision
("what to show / send / accept") should keep landing here first.

**S2 — Invariants are enforced by construction, not by comment.** The best example is
`GuardedReq` ([http_handlers.hpp:25](main/http_handlers.hpp)): every HTTP handler takes a
wrapper type that only `handle_all`'s try/catch dispatcher can construct, so registering a
handler that bypasses the OOM guard is a *compile error*. Same spirit: presence flags
(`has_*`) on every telemetry field so "car didn't report it" cannot render as a phantom 0.

**S3 — Single source of truth for derived state.** `link_state()` is one four-state machine
(in `logic/link_state.hpp`, host-tested) feeding both the web-UI hero and MQTT
`sleep_state`; `tk::UiSnapshot` is one input contract feeding both the ST7735 display and
the APA102 LED. The two-sinks bug class ("UI says X, MQTT says Y") is designed out.

**S4 — The OTA/release pipeline is genuinely robust.** Signed images with TOFU trust,
a software downgrade gate (freshness ≠ authenticity — correctly distinguished), rollback
armed until a ~90 s health gate passes (catches boots-but-crashes-under-load), chip-id
verification, one partition table across all five targets, CI size gates per target, and a
build-efficiency audit hook. This is a better update story than many commercial devices.

**S5 — The memory model is explicit and consistently applied.** The binding constraint
(largest *contiguous* free block, not total heap) is named in CLAUDE.md, and the code
follows through: streamed `/diag`, guarded handlers, static long-lived objects, SSO-sized
label strings, rejected whole-build `-Os` after a live A/B proved it freezes.

**S6 — Failure modes are first-class design inputs.** The two-strike auth-fail heuristic
that only the health probe may trip (so role-denied commands can't destroy a pairing), the
`cmd_fail_streak_` link-drop backstop for non-throwing framer desyncs, the WiFi
ghost-association watchdog that re-associates but never reboots (to avoid the setup-portal
trap), the VCSEC-ASLEEP-trusted / VCSEC-AWAKE-distrusted asymmetry — each encodes a
real observed incident.

**S7 — Reproducible builds and doc discipline.** `idf-docker.sh` pins the ESP-IDF image to
the exact CI version at runtime; CLAUDE.md/ARCHITECTURE.md split essentials from deep
reference; PR-gated review/audit skills check for drift.

---

## 4. Findings and risks

**F1 — `VehicleController` is a god object (severity: medium, structural).**
The header is 515 lines; the implementation spans four `.cpp` files sharing private state
via `vehicle_ctrl_internal.hpp`. It owns: command execution, telemetry caches + parsing
callbacks, pairing lifecycle, link-state, UI snapshot assembly, *and* generic NVS config
persistence (`save_config_str/vin/time`, `load_config_str`). The last group is a pure
pass-through to `NvsStorageAdapter` that HTTP handlers reach through `g_vehicle` only
because it is the sole global they have ([http_config.cpp:220](main/http_config.cpp)).
The file split already mirrors the natural seams (commands / telemetry / pairing) — but
they share one mutex web and one blast radius. → P2 (cheap slice), P3 (testability slice).
A full class decomposition is *not* recommended now (see §6).

**F2 — The concurrency contract lives in prose (severity: medium, latent).**
Ten FreeRTOS tasks (priorities 2–5), three mutexes + one signalling semaphore in the
controller, and a dozen atomics. The current code is consistent — the observed order is
`command_mutex_` (outer, RAII) → `vehicle_mutex_` (short critical section) → wait on
`cmd_sem_` — and each primitive carries an excellent comment. But the *hierarchy* is
nowhere stated normatively, task priorities are scattered magic numbers, and nothing stops
a future change from taking `cache_mutex_` while holding `vehicle_mutex_` in one path and
the reverse in another. Deadlock bugs here would present as the worst failure mode this
device has (frozen but not rebooting ⇒ car never sleeps, evcc blind). → P1.

**F3 — Dependency cliff under tesla-ble (severity: high, strategic).**
Three compounding facts: (a) `yoziru/tesla-ble` v5.1.1 is the protocol heart and is pinned;
(b) ESP-IDF 6 (mbedTLS 4 / PSA) breaks its crypto internals (tracked in issue #61), so the
project is held on IDF 5.x with a shrinking support horizon; (c) the esp32c5 target only
exists via a local build-time patch (`prepare-tesla-ble-c5.sh` clones + edits the upstream
manifest), which every upstream bump must re-verify. None of this is urgent today; all of
it gets more expensive the longer it waits. → P4, P5.

**F4 — HTTP-layer logic is not host-testable (severity: medium).**
`http_status.cpp` (318 lines) hand-builds the `/status` JSON that is the de-facto contract
for the web UI; `http_api.cpp` maps command names to controller calls. Neither can run in
the mock build, so contract regressions (field renamed, chip rule changed) surface only on
hardware. The project already solved this pattern once: `mcp_server.cpp` is a thin
transport around host-tested `logic/mcp.hpp`. `/status` and command dispatch deserve the
same treatment. → P3.

**F5 — Unauthenticated `/mcp` expands the command surface (severity: low–medium, by design).**
"No HTTP auth" is a documented, deliberate trade-off because evcc cannot send credentials.
But MCP clients *can* send headers, and `/mcp` exposes the full charging command set to any
LAN device. The blanket no-auth rationale does not actually apply to this one endpoint.
→ P6 (optional, off by default; user decision).

**F6 — Single in-order FIFO under all BLE traffic (severity: low, contained).**
All commands and polls funnel through tesla-ble's single queue; the architecture compensates
with `cmd_in_flight_` (pause polls during a command) and `cmd_fail_streak_` (drop link on
desync storms). These are point fixes for queue-position problems that an owned
serialization layer would solve by construction — but the fixes work, are tested live, and
the command surface is stable. → P7, explicitly deferred.

**F7 — Known defect: display battery render not `has_*`-gated (severity: low).**
Carried follow-up from PR #165: the battery drawing can render a phantom 0 % before the
first charge report. One-line-class fix in the presenter (`logic/display_model.hpp`) +
a `CHECK` in `test/test_logic.cpp` — the infrastructure for exactly this fix already exists.

**F8 — `/status` has no schema/contract test (severity: low, folds into P3).**
`www/app.js` (649 lines) consumes `/status` by field name with no versioning. Fine for a
same-image-ships-both design (UI and firmware update atomically), but a golden-field test
becomes nearly free once P3 lands, and protects the *external* consumers too (anyone
scripting against `/status`).

---

## 5. Proposals with implementation guidance

### P1 — Write down the concurrency contract (do first)

**What:** a `## Concurrency` section in `docs/ARCHITECTURE.md`:

1. **Lock hierarchy (normative):** `command_mutex_` → `vehicle_mutex_` → (`cmd_sem_` wait);
   `cache_mutex_` is a leaf — held only for a struct copy, never while calling out or
   taking another lock. State the rule: *acquire strictly left-to-right, never hold a
   right-hand lock while taking a left-hand one.*
2. **Task inventory table:** name, priority, stack, owner file, purpose — for all ten tasks
   (`vehicle_loop` 5, `auto_pair` 4, `captive_dns` 5, `display` 3, `ota`/`ota_chk` 5,
   `led` 2, `wifi_wd` 4, `ota_gate` 3, `mqtt_pub` 4, plus NimBLE host + httpd).
3. **Atomics doctrine:** one sentence on why flags are atomics (cross-task, no ordering
   dependency) vs. what must sit under a mutex (multi-field structs with `std::string`).

**Also:** replace the numeric priority literals at the `xTaskCreate` sites with named
constants in one header (e.g. `main/task_config.hpp`), so the table and the code cannot
drift and relative priorities are reviewable at a glance.

**Effort:** ~1–2 h. **Risk:** none (docs + constants). **Verify:** build all targets in CI.

### P2 — Move config persistence off `VehicleController`

**What:** `save_config_vin / save_config_str / load_config_str / save_config_time` are
storage pass-throughs, not vehicle behaviour. Give the HTTP layer direct access to the
config store instead of routing through the controller.

**Steps:**
1. `http_server_start(VehicleController&, NvsStorageAdapter& config_store)` — add the
   parameter, stash it as `g_config` beside `g_vehicle` (same idiom, one more seam).
2. Point `http_config.cpp` (`/set_vin`, `/set_mqtt`) and the `/set_time` persistence at
   `g_config` directly.
3. Delete the four pass-through methods from `vehicle_ctrl.hpp`. Keep
   `reset_for_new_vehicle()` on the controller — that one *is* vehicle behaviour.

**Effort:** ~2 h. **Risk:** low (mechanical; no behaviour change). **Verify:**
`run-mock-tests.sh` + CI build + a `/set_mqtt` round-trip on hardware or in `/e2e-evcc`.

### P3 — Extract `/status` and command-dispatch shaping into `logic/` (the MCP pattern)

**What:** repeat the proven `mcp.hpp`/`mcp_server.cpp` split for the two remaining
logic-heavy HTTP surfaces, making the web-UI contract host-testable (F4, F8).

**Steps:**
1. `main/logic/status_model.hpp`: a plain-struct input (extend/reuse `tk::UiSnapshot` +
   the telemetry result structs, which are already IDF-free by design) → an ordered list of
   `{path, value, present}` fields, mirroring today's `/status` shape exactly.
   `http_status.cpp` shrinks to: collect inputs under the existing locks → call the model →
   serialize with cJSON.
2. `main/logic/command_registry.hpp`: the command-name → {kind (vcsec/infotainment),
   arg spec, role-allowed} table currently embedded in `http_api.cpp`, returning a typed
   descriptor the handler executes. The MCP tool registry in `logic/mcp.hpp` already
   encodes half of this — deduplicate so `/api` and `/mcp` share ONE command table
   (they must never disagree about arg ranges again by construction).
3. `test/test_logic.cpp`: golden `CHECK`s — full field list for a representative snapshot
   (awake+charging, asleep, unreachable, unpaired), plus arg-clamp cases via the shared
   registry.

**Effort:** 1–2 days. **Risk:** low — pure refactor with a golden test written *before*
moving code (capture today's `/status` output as the fixture). **Verify:**
`run-mock-tests.sh` (new CHECKs), then diff a live `/status` against the fixture on device.
**Sequencing:** after P2 (smaller controller surface to model).

### P4 — Upstream the esp32c5 target to `yoziru/tesla-ble`

**What:** open a one-line PR upstream adding `esp32c5` to `idf_component.yml` `targets:`
(the project's own patch proves the library is already target-agnostic — the manifest line
was the only blocker). On acceptance + release: drop `prepare-tesla-ble-c5.sh`, the
`third_party/` clone step, and the conditional `rules:` routing in `main/idf_component.yml`;
all five targets then resolve identically from the Component Manager.

**Effort:** minutes to file; cleanup ~1 h when released. **Risk:** none (local patch stays
until then). **Verify:** `ci-build-all.sh` — c5 image byte-comparable expectation, size
gate unchanged. Keep the script until the pinned version actually contains the fix.

### P5 — Decide the IDF-6 crypto strategy now, as an ADR (execute later)

**What:** issue #61 already names the plan ("port crypto, not reimplement"). Turn it into a
short ADR in `docs/` so the decision survives context loss, with the options table:

| Option | Complexity | Risk | Note |
|--------|-----------|------|------|
| A. Shim layer: vendored `tesla-ble` fork replacing legacy `ecp_mul`/`MBEDTLS_PRIVATE` internals with PSA calls | Medium | Medium | Keeps upstream diff reviewable; candidate for upstreaming |
| B. Wait for upstream mbedTLS-4 support | None | Timeline unknown | IDF 5.x EOL is the deadline clock |
| C. Reimplement the crypto in-project | High | High | Rejected — protocol crypto is exactly what you don't fork |

Recommendation: **B with a deadline, then A** — re-check upstream each Renovate cycle; if
IDF 5.x enters its last support year with no upstream movement, start A. The ADR should
name that trigger explicitly so the Renovate reminder has something to check against.

**Effort:** ADR ~1 h now; port is a multi-day effort *later, on the trigger*. 

### P6 — Optional `/mcp` bearer token (user decision)

**What:** a Kconfig + NVS option (`mcp_token`, empty = today's open behaviour). When set,
`mcp_handle_post` requires `Authorization: Bearer <token>` before dispatch; the check lives
in host-testable code (`logic/mcp.hpp` gets a `token_ok(header_value, expected)` — constant-
time compare) with a `CHECK` in the mock suite. evcc is unaffected (`/api` stays open —
that constraint is real). Default **off** to preserve zero-config setup.

**Effort:** ~half a day incl. docs (`docs/MCP.md`, `docs/SECURITY.md`). **Risk:** low.
**Note:** this deliberately does *not* reopen the rejected whole-API token — the scope is
one endpoint whose clients can all send headers.

### P7 — Owned BLE-ops queue (explicitly deferred)

**What would it be:** one owner task serializing *all* tesla-ble access via message passing,
replacing `command_mutex_`/`vehicle_mutex_`/`cmd_in_flight_` coordination by construction
and giving commands true queue priority over polls.

**Why defer:** the current flag-based compensations are live-tested and stable; the command
surface is not growing; the rework touches the most incident-prone code for a structural
(not behavioural) win; and static task+queue memory is a real cost on the tightest targets.
**Trigger to revisit:** a new command class lands (e.g. upstream adds scheduled-departure),
or another queue-position incident occurs despite `cmd_in_flight_`.

### Quick fix (do with the next display change): F7

Gate the battery render on `has_battery_level` in `logic/display_model.hpp`, render the
"no data yet" treatment otherwise, add the `CHECK`. Fits the existing `add-logic-test`
skill workflow exactly.

---

## 6. Considered and rejected

- **Full decomposition of `VehicleController` into CommandChannel / TelemetryStore /
  PairingSupervisor classes** — the four-file split with a shared internal header is a
  pragmatic embedded idiom; the interlocking state (streaks, flags, session lifecycle) is
  cohesive for a reason. P2+P3 shave off the two responsibilities that clearly don't belong;
  going further is churn without a driving defect.
- **Web-UI framework / build step** — 1.1 k lines of vanilla JS spliced into one gzipped
  page is *the right amount of engineering* for this UI; a framework adds flash cost and
  toolchain surface for nothing.
- **Whole-API authentication** — already tried, removed for a hard external constraint
  (evcc cannot send credentials). P6 is scoped to not conflict with this.
- **`-Os` for size headroom** — banned; live A/B showed hard freezes under evcc+BLE load.
  Package-A levers already freed the needed headroom.
- **QEMU-based integration tests** — the host logic layer + live e2e script cover the
  spectrum more cheaply; QEMU can't emulate the NimBLE/vehicle side, which is where the
  real risk lives.

---

## 7. Action plan

1. [ ] **P1** — Concurrency section in `docs/ARCHITECTURE.md` + named task priorities (~2 h)
2. [ ] **F7** — `has_battery_level` gate in the display presenter + CHECK (~1 h)
3. [ ] **P2** — config persistence off `VehicleController` (~2 h)
4. [ ] **P4** — file the upstream `targets: esp32c5` PR against `yoziru/tesla-ble` (minutes; cleanup on release)
5. [ ] **P5** — write the IDF-6 crypto ADR with the explicit start-trigger (~1 h)
6. [ ] **P3** — `logic/status_model.hpp` + shared command registry + golden CHECKs (1–2 days)
7. [ ] **P6** — decide on the `/mcp` token option (user call; ~half a day if yes)
8. [ ] **P7** — no action; revisit on the named triggers

Items 1–5 are individually shippable PRs with no interdependencies (P3 benefits from P2
landing first). Every item verifies through the existing loop: `scripts/run-mock-tests.sh`
locally, the `logic-test` + 5-target build gates in CI.
