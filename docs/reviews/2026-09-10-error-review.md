# Error and coherence review — tesla-key-esp32 (2026-09-10)

Read-only whole-project defect audit at `dbd92e6d7933b63ae71ecb2051d935bda98575b9`
(`main`, PR #289 merged). Scope and structure follow
[`$project-review`](../../.agents/skills/project-review/SKILL.md). **No source, configuration,
skill or PR state was modified** — this document is the only artifact produced.

The predecessor review, [`2026-09-07`](2026-09-07-project-review.md), is fully closed: both
redaction gaps are fixed (`kDiagRedactions` now carries the interrupted-VIN rule,
`kRedactedStatusFields` is 7 and `ble.devices[].name` is redacted), `usable_soc` reaches all four
documented mirrors, `kWatchFailsToRecover` and `compare_ota_versions()` are named correctly, and
`run-sanitizer-tests.sh` probes the sanitizer runtime before selecting a compiler. This run
therefore starts from a clean slate and concentrates on the sixteen findings landed since
(`R01`–`R15` in `dbd92e6`, plus the Renovate gate exemption in `3f550b7`).

## Summary

Every host gate that can run in this container passes, including the browser gate that the
previous review had to skip. The inventories that the project pins numerically — 21 fixed HTTP
routes plus 3 vehicle routes, 19 NVS records, 55 MQTT discovery rows, 9 MCP tools, 4 targets,
5 ordered tesla-ble patches, partition geometry — all still agree across code, config, tests and
docs, and the project map covers every file in `main/`.

**7 findings: 3 P2, 2 P3, 1 doc-drift, 1 tooling nit.** All three P2s were introduced by the
`R01`–`R04` fix batch, and they share one shape: a safety mechanism was added, but the path that
proves it works was not. A lock invariant is documented and then violated three lines below the
comment restating it; a gate is acquired and its failure ignored; a destructive-action
confirmation is guarded on client state that is null for the first four seconds of every page
load. None is remotely exploitable, and none destroys key material on its own — but the
`/gen_keys` one can un-pair a car from a single tap, which needs physical access to the vehicle
to undo.

## Findings

### [BUG] `ESP_LOGW` runs while `cache_mutex_` is held, against the rule stated in the same file

- **Where:** `main/vehicle_telemetry.cpp:452-463` (the guard at `:452`, the log at `:461`) vs.
  [`docs/ARCHITECTURE.md:1445-1448`](../ARCHITECTURE.md); sink chain
  `main/diag_log.cpp:51-68` → `main/diag_log.cpp:39-47` → `main/syslog.cpp:529-554`
- **What:** the identity-epoch discard branch added by `R02` logs from inside the
  `cache_mutex_` critical section:

  ```c
  tk::MutexGuard g(cache_mutex_);
  if (tk::telemetry_epoch_matches(charge_epoch, identity_epoch_.load(std::memory_order_acquire))) {
      ...
  } else {
      ESP_LOGW(TAG, "discarding charge telemetry from defunct identity epoch (%u vs %u)", ...);
  }
  ```

  `docs/ARCHITECTURE.md` states the rule for this exact lock: "`cache_mutex_` is a **leaf**: held
  only for a plain struct copy/assignment, never while calling out (library, BLE, NVS, **logging**)
  and never while taking another lock." The comment introduced with the same change
  (`main/vehicle_telemetry.cpp:444-447`) restates the intent — "Only bounded result publication is
  serialized" — and the log line contradicts it.
- **Why it matters:** this is not a style point, because `ESP_LOG*` is not a leaf operation on this
  device. `diag_log_init()` installs `diag_vprintf_` as the esp_log sink, so every logged line
  runs `vsnprintf` into a 256-byte frame, then `diag_append_()`, which **takes a second mutex**
  (`s_mtx`, `pdMS_TO_TICKS(20)`), then `syslog_send()`, which does a `string_view::find` and an
  `xQueueSend`. So the branch acquires a lock while holding a lock the architecture declares a
  leaf, and it can hold `cache_mutex_` for up to the diag ring's full 20 ms timeout. Everything
  that reads the caches — `/status`, `/vehicle_data`, the MQTT publisher, the display and LED
  samplers — blocks behind `cache_mutex_` for that window. The ordering is also new and undeclared:
  nothing else in the tree takes `cache_mutex_` before the diag ring mutex, so the lock graph in
  `docs/ARCHITECTURE.md` no longer describes the code.
- **Confidence:** confirmed (static). The guard scope, the log statement, the `diag_vprintf_` hook
  installation and `diag_append_`'s bounded take were each read. The branch itself needs a pairing
  reset racing an in-flight telemetry parse to execute, so the stall is rare; the rule violation
  is unconditional.
- **Fix:** hoist the decision out of the critical section. Capture the comparison result into a
  local under the guard, close the scope, then log:

  ```c
  bool accepted;
  { tk::MutexGuard g(cache_mutex_);
    accepted = tk::telemetry_epoch_matches(charge_epoch, identity_epoch_.load(std::memory_order_acquire));
    if (accepted) { last_known_charge_ = std::move(parsed); ... } }
  if (!accepted) ESP_LOGW(TAG, "discarding charge telemetry from defunct identity epoch ...");
  ```

  `test/test_runtime_boundaries.cpp` already exists to prove "diagnostic HTTP sinks run only after
  the shared ring mutex is released"; the same harness is the natural place for a canary asserting
  no `ESP_LOG*` is reachable under `cache_mutex_`.

### [BUG] One tap can un-pair the car: `genKey()` skips its confirmation before the first `/status`

- **Where:** `main/www/app.js:9` (`var state=null`), `main/www/app.js:708-717` (`genKey`),
  `main/www/index.html:39-44` (the always-live tile), `main/http_config.cpp:126` (`force`),
  `main/vehicle_pairing.cpp:336-348` (the preflight `force` disables)
- **What:** the destructive-action confirmation is conditional on client state that does not exist
  yet:

  ```js
  function genKey(){
    if(state&&state.key_present && !confirm('Regenerate the security key?\n\n...')) return;
    return requestJson('/gen_keys?force=1',{method:'POST'})
  ```

  `state` is `null` until the first successful `/status` render assigns it (`app.js:344`), and
  `boot()` polls once immediately and then every 4 s (`app.js:855-866`). The "Security key" tile is
  static markup in the grid — only the hero card starts hidden (`index.html:64`) — so it is visible
  (showing the `—` placeholder) and clickable from first paint. Tap it in that window and
  `state && state.key_present` is `null`, the `confirm()` is never evaluated, and the request goes
  out immediately.
- **Why it matters:** the request carries `force=1` unconditionally, and `force` is exactly what
  disables the server's own safety net. `generate_key_result()` only probes for an existing key
  `if (!allow_replace)`, so the `409` / `existing_key_refused` path that
  [`docs/README.md:194-195`](../README.md) documents ("Without `force`, `/gen_keys` returns `409`.
  Regenerating un-pairs the vehicle") cannot fire. On a running, paired device the key is replaced,
  the pairing is destroyed, and recovery requires standing at the car to confirm a fresh enrolment
  — the one class of loss `AGENTS.md` singles out ("Pairing keys and sessions must survive ordinary
  OTA. Any NVS erase, forced key generation … is destructive"). The server implements a deliberate
  two-step protocol and its only client opts out of it on every call, leaving the entire guarantee
  resting on a browser dialog that is itself conditional.
- **Confidence:** confirmed (static, both sides). The window is bounded by the first poll's
  round-trip — short on a healthy LAN, seconds on a loaded device or a weak link, and a failed
  first poll leaves `state` null until one succeeds (`poll()`'s `catch` deliberately changes
  nothing on screen).
- **Fix:** two independent changes, both cheap. Make the confirmation unconditional — the
  destructive branch must not be skippable by absent data — and send `force=1` only when the UI
  actually knows a key exists, letting the server's `409` answer the unknown case:
  `requestJson('/gen_keys'+(state&&state.key_present?'?force=1':''), …)`, with the existing `409`
  reason surfaced as a toast. Add a `test/web_ui_http.test.mjs` case that calls `genKey()` with
  `state === null` and asserts a confirmation was requested (and that no `force=1` request was
  issued). Consider also disabling the tile until the first frame lands, which is the same
  reasoning that already hides the hero.

### [BUG] The `ConfigRestart` gate is acquired but never checked, so `R01`'s invariant is unenforced

- **Where:** `main/ota_update.cpp:121-123` (the wrapper), `main/ota_update.hpp:108-112` (the
  contract), call sites `main/http_config.cpp:551`, `:594`, `:673` and `main/provisioning.cpp:252`;
  the CAS itself at `main/logic/health_gate.hpp:84-90`
- **What:** `ota_config_restart_begin()` returns `bool` and every one of its four call sites
  discards it:

  ```c
  ota_confirm_pending_image(tk::OtaRebootClass::SuccessfulUserConfigCommit);
  ota_config_restart_begin();
  vTaskDelay(pdMS_TO_TICKS(800));
  esp_restart();
  ```

  `try_begin()` is a compare-exchange against `Idle`: it returns `false` whenever another owner
  (`Ota`, `IdentityMutation`, `FaultRestart`, `HealthCommit`) already holds the gate. The header
  nevertheless states the outcome unconditionally — "holds the owner until `esp_restart()` so no
  OTA or identity transaction can start during the delayed restart window."
- **Why it matters:** the promise is only true when the acquire happens to succeed, and nothing
  observes whether it did. `handle_set_mqtt`, `handle_set_syslog`, `handle_set_wifi` and the setup
  portal's `POST /save` carry no `OtaIdentityMutationGuard` and are not among the routes
  `http_route_requires_vehicle_runtime()` gates, so they are reachable while an OTA download owns
  the gate. In that case the handler still calls `ota_confirm_pending_image()` — cancelling
  bootloader rollback on a still-unverified image — then sleeps 800 ms and reboots into the middle
  of the download. That is precisely the shutdown-window race `R01` was written to close. Every
  other acquire of the same gate in `ota_update.cpp` consumes its result (`:88` branches on it,
  `:115` stores it in `OtaHealthCommitGuard::held_`, `:45`/`:107` return it); this is the only one
  that does not, so the asymmetry is visible in one file.
- **Confidence:** confirmed (static). `test/test_logic.cpp:3844-3850` covers the pure gate's
  `ConfigRestart` transitions, including a failed `try_begin`, but no test covers a call site's
  behaviour on `false` — because no call site has any.
- **Fix:** decide the policy and make the code say it. Either refuse the reboot when the gate is
  busy (return `503` with a "an update is in progress" reason and leave the configuration saved
  but unapplied until the next restart), or keep rebooting and mark the wrapper
  `[[nodiscard]]` with an explicit, commented `(void)` at sites that genuinely accept the race.
  Whichever is chosen, `ota_confirm_pending_image()` should not run on the losing path: cancelling
  rollback is irreversible and a contested gate is not evidence of a healthy image. Add a host
  case asserting the chosen behaviour when the gate is already owned.

### [INCONSISTENCY] Safe mode reports a paired device as keyless and invites the user to re-key it

- **Where:** `main/vehicle_ctrl.cpp:249-271` (`init_safe_mode`, sets only `config_store_` and
  `vin_`), `main/vehicle_ctrl.hpp:473-475` (`storage_{nullptr}`),
  `main/vehicle_pairing.cpp:743-758` (`has_key`/`has_session`/`key_fingerprint` all return the
  empty answer when `storage_` is null), `main/http_status.cpp:93-97`, `main/www/app.js:595-600`
- **What:** `R03` moved safe-mode boot onto a new inert initializer that never receives the
  `tesla_ble` NVS adapter. Before `dbd92e6`, `vehicle.init(...)` ran unconditionally — in safe mode
  too — with `start_tasks=false`, so `storage_` was wired and the identity accessors read flash
  normally. Now `has_key()`, `has_session()`, `key_fingerprint()`, `key_created_at()` and
  `paired_at()` all short-circuit on the null pointer, and `/status` reports
  `key_present:false`, `paired:false`, `key_fingerprint:""`, `paired_at:0` on a device whose key
  and session are intact in flash. The web UI renders that as `Not generated` with the subtitle
  `tap to generate` on the key tile, and `not paired` on the vehicle tile.
- **Why it matters:** safe mode exists so that a device crashing on the vehicle path "stays fixable
  in a browser" ([`docs/FEATURES.md:29`](../FEATURES.md)) — the browser page is the whole recovery
  surface, and it is now the surface that misreports the device's identity. A user who is already
  troubleshooting is told their key does not exist and invited to make a new one. Tapping through
  is currently harmless: `GenKeys` is in `http_route_requires_vehicle_runtime()`
  (`main/logic/http_route.hpp:118-128`) and safe mode marks the runtime not-ready, so the dispatcher
  answers `503` and the toast reads "Key generation failed" with no explanation. But the guard
  holding that line is the route table, not anything in the safe-mode path, and finding 2 above
  means the confirmation dialog is skipped here as well (`key_present` is false), so the only thing
  between a confused user and a re-key attempt is one dispatcher check. Compounding it, `app.js`
  never reads `sys.safe_mode` at all — the field is emitted at
  `main/logic/status_model.hpp:389` and consumed by MQTT and the docs, but the page renders a
  normal-looking dashboard with no indication that BLE, pairing, commands and MQTT are down.
- **Confidence:** confirmed (static, both the current tree and `dbd92e6^` for the regression half).
- **Fix:** pass the `tesla_ble` adapter to `init_safe_mode()` and assign `storage_` there. Nothing
  in the inert path mutates it — the accessors are existence probes and one read-only PEM parse —
  so the identity display becomes truthful again without constructing `TeslaBLE::Vehicle` or
  starting a task. Separately, and independently worth doing: render `sys.safe_mode` in the UI as
  a banner naming what is down and what to do, which is the one piece of information that page
  exists to deliver in this state. Note also that the safe-mode branch now skips VIN-transition
  recovery entirely (the whole block moved inside the `else`); that may be intended, but nothing
  records the decision.

### [INCONSISTENCY] `validate_query_string` names `CONFIG_HTTPD_MAX_URI_LEN` and hardcodes 128

- **Where:** `main/http_common.cpp:83-92` (the check), `main/http_common.cpp:96` (`char q[128]`),
  `main/http_handlers.hpp:60-63` (the contract), `sdkconfig.defaults:80`
  (`CONFIG_HTTPD_MAX_URI_LEN=512`)
- **What:** the header states the function "[v]alidates that if a query string is present on req,
  it fits within `CONFIG_HTTPD_MAX_URI_LEN`", and `dbd92e6`'s `R04` line says the same. The body
  never references that symbol; it rejects `qlen >= 128`, a bare literal that must equal
  `sizeof(q)` in `query_param_is()` a few lines below, and the configured URI limit is four times
  larger. Two files now have to hold the same magic number in agreement with nothing enforcing it.
- **Why it matters:** the direction of the coupling is the hazard. Lower `q[]` without touching the
  validator and over-long queries pass validation and then truncate inside `query_param_is()`,
  which returns `false` — reintroducing exactly the fail-open `R04` closed, where
  `/status?…&redact=1` answers `200` **unredacted** because the parameter fell off the end. The
  reverse (raising `q[]`) merely keeps rejecting valid requests. Neither is caught today: no test
  ties the two constants together, and no gate asserts the documented `CONFIG_HTTPD_MAX_URI_LEN`
  relationship. Coverage is also partial — `validate_query_string()` is called from `handle_status`
  and `handle_diag` only, while `handle_gen_keys` (`force`), `handle_send_key` (`role`) and
  `handle_coredump` (`clear`) read query parameters without it. Those three happen to fail safe on
  truncation (no force, the restricted role, no erase), so this half is a coverage inconsistency
  rather than a defect — but it is only safe by accident of which value each default is.
- **Confidence:** confirmed (static; all five call sites and both buffers read).
- **Fix:** give the buffer size one name — `inline constexpr size_t kQueryBufBytes = 128;` in
  `http_handlers.hpp` — use it for both `q[]` and the bound, and either reference
  `CONFIG_HTTPD_MAX_URI_LEN` in a `static_assert` (`kQueryBufBytes <= CONFIG_HTTPD_MAX_URI_LEN`) or
  correct the header comment to say what the code actually enforces. Then call the validator from
  every handler that reads a parameter, so "a truncated query is a `400`" is one rule rather than
  five independent accidents.

### [DOC-DRIFT] A `redact.hpp` rule comment points at the wrong file

- **Where:** `main/logic/redact.hpp:145` vs. `main/net.cpp:322` and `main/net.cpp:346`
- **What:** the rule `{"WiFi connected to '", ""}` is attributed to `main.cpp`. A repo-wide grep for
  the phrase returns two hits, both in `main/net.cpp` (the WiFi and the post-rollback association
  paths), and none in `main.cpp`.
- **Why it matters:** the same file states why the attributions exist — "a reworded log line
  silently stops matching and the only symptom is a leak nobody sees" — so a reviewer auditing the
  table is sent to a file that does not contain the line, and the second call site is invisible
  from the comment. This is the last survivor of the class the `2026-09-07` review raised as its
  finding 7; the three attributions named there were corrected and this one was not. The rule
  itself is keyed on the phrase and still fires on both sites, so nothing leaks today.
- **Confidence:** confirmed (`grep -rn "WiFi connected to" main/`).
- **Fix:** change the comment to `net.cpp` and note that there are two sites. A cheap durable
  alternative for the whole table: a host canary that greps `main/` for each rule's marker and
  fails when a marker matches zero source lines — that catches a reworded log line, which is the
  failure the comments are a proxy for.

### [NIT] Editing residue from the `R01`–`R04` batch

- **Where:** `main/provisioning.cpp:231-232`, `main/http_config.cpp:582`, `main/main.cpp:664`,
  `main/vehicle_ctrl.cpp:241`
- **What:** four cosmetic leftovers, none affecting behaviour:
  - a stray double blank line inserted between the rollback comment and `cfg_save` in
    `provisioning.cpp`;
  - `handle_set_syslog`'s probe lambda changed from `[](const std::string&)` to
    `[&](const std::string&)` while capturing nothing;
  - `main.cpp:664` closes the ~165-line safe-mode `else` with a brace at four-space indentation
    that reads as the end of a statement, not of the branch — the block it terminates opens 163
    lines earlier;
  - `vehicle_ctrl.cpp:241` still says "safe mode intentionally never starts them" inside
    `init()`'s `start_tasks == false` branch, which safe mode no longer reaches — it now goes
    through `init_safe_mode()`.
- **Why it matters:** only the last two cost a reader anything, and the brace is the one worth
  fixing: the safe-mode branch is exactly where a future edit will need to know which side of the
  `if` it is on.
- **Confidence:** confirmed.
- **Fix:** drop the blank line and the empty capture; comment the closing brace
  (`}  // !safe_mode`) or lift the branch body into a helper; reword the `init()` comment to say
  the caller defers the tasks.

## Coherence check

| Axis | Verdict |
|---|---|
| HTTP routes: `logic/http_route.hpp` (21 fixed + 3 vehicle) ↔ `docs/README.md` | ✓ consistent |
| Commands: `logic/command_registry.hpp` ↔ `command_exec.cpp` ↔ `docs/README.md` ↔ `docs/MCP.md` (9 tools) | ✓ consistent |
| NVS: `logic/nvs_contract.hpp` (`kEntries` = 19) ↔ `docs/README.md` ↔ `docs/SECURITY.md` ↔ `docs/FEATURES.md` | ✓ consistent |
| MQTT: `logic/mqtt_discovery_registry.hpp` (55 rows) ↔ `ARCHITECTURE.md` / `FEATURES.md` | ✓ consistent |
| Redaction: `/status` field set (7) ↔ `/diag` phrase table ↔ actual log sites | ✓ values consistent; ✗ one stale attribution (finding 6) |
| Targets: `logic/target.hpp` ↔ `platform.hpp` ↔ `ota_update.cpp` ↔ `ci-build-all.sh` / `ci-sign-artifacts.sh` / `build-pages.sh` | ✓ consistent (all four, one list) |
| Partitions: `partitions.csv` ↔ every doc | ✓ consistent |
| Pins: ESP-IDF v5.5.5, tesla-ble v5.1.3, 5 ordered patches, 4 lockfiles ↔ docs ↔ skills | ✓ consistent |
| Lock hierarchy: `docs/ARCHITECTURE.md` "Concurrency" ↔ the guards in `vehicle_*.cpp` | ✗ drifted (finding 1) |
| `/status` identity fields ↔ the state the device is actually in | ✗ drifted in safe mode (finding 4) |
| OTA/identity gate: `logic/health_gate.hpp` ↔ every acquire in `ota_update.cpp` | ✗ one acquire unchecked (finding 3) |
| Project map ↔ tree: every `main/*.cpp` / `main/*.hpp` is mapped | ✓ consistent (`main/logic/*.hpp` via the deliberate glob row) |
| Logic-test ownership: `test/logic_test_ownership.json` ↔ `main/logic/*.hpp` | ✓ consistent (58 headers, gate passes) |
| `version.txt` (1.4.0) as the sole version floor | ✓ consistent |
| CI job graph: `needs:` ↔ every `needs.<job>.outputs.*` reference in `build.yml` | ✓ consistent (`R08`'s move to `logic-test` declares both edges) |

One observation that is not a finding: the Renovate exemption added in `3f550b7`
(`gate_is_renovate_maintenance`, `tools/agent-hooks/pr-gate-lib.sh:49-84`) keys on **paths**, not
on the PR author, so any PR touching only `.github/renovate.json` or
`.github/workflows/renovate.yaml` skips the `$project-review`, `$pr-hygiene` and `$skill-audit`
records. The workflow half is well covered — `scripts/check-workflow-policy.py` pins that
workflow's action SHA, owner, permissions and env, with mutation canaries — so the residual
surface is `.github/renovate.json`, whose contents no gate validates. Worth deciding
deliberately rather than inheriting.

## Evidence

Host/static only. Every other boundary is **not run**.

| Gate | Result |
|---|---|
| `scripts/repo-lint.sh` | PASS — 10 YAML, 6 workflows, 31 documents / 226 local targets, 58 headers, NVS + host-gate contracts |
| `scripts/run-mock-tests.sh` | PASS — incl. 960-vector `ble_row` parity, display-sim parity, 54 web-UI HTTP cases |
| `scripts/run-sanitizer-tests.sh` | PASS — 4735 logic + 961 NVS + 637 runtime-boundary checks, 20 000-case fuzz (seed 6072351259557053785), ASan+UBSan+LSan |
| `scripts/test-build-contracts.sh` | PASS — pins, targets, partitions, signing/Pages/release contracts |
| `scripts/test-pr-gates.sh` | PASS — 112 hook self-tests |
| `tools/agent-config/selftest.sh` | PASS — 52 mutation canaries |
| Web-UI browser gate | PASS — real DOM, console, keyboard, live regions, desktop/mobile layout (this container ships Chromium; the gate's own probe checks `PATH` only, so it needs `BROWSER_BIN` pointed at it) |
| Four-target pinned-IDF build | **not run** |
| Disposable-key signing contract | **not run** |
| Remote CI | **not run** |
| Hardware / USB / flash | **not run** |
| OTA | **not run** |
| Vehicle / live evcc | **not run** |

No compilation, host test or gate result in this report is evidence of hardware, flash, OTA,
signing, release or vehicle behaviour.

## Prioritized actions

1. **Must fix — the `/gen_keys` confirmation gap** (finding 2). It is the only finding whose
   consequence needs physical access to the car to undo, and both halves are a few lines. Treat the
   `force=1`-on-every-call half as the real defect: the server built a two-step protocol and the UI
   should use it.
2. **Must fix — the unchecked `ConfigRestart` acquire** (finding 3). `R01` is not currently doing
   what its header says, and the fix is to pick a policy rather than to add code.
3. **Should fix — the `cache_mutex_` logging** (finding 1). One hoisted local, plus the canary that
   stops it recurring; the lock hierarchy is load-bearing and is documented as normative.
4. **Should fix — safe-mode identity reporting** (finding 4). Pass the adapter through; separately,
   decide whether the recovery page should say it is in safe mode, because today nothing does.
5. **Should fix — the query-length constant** (finding 5). Name it once and assert the relationship
   the header already claims.
6. **Nice to have — the stale `redact.hpp` attribution and the editing residue** (findings 6, 7).

Gate readiness is deliberately **not** asserted: three P2 findings are open, so no
`$project-review` merge record should be stamped from this run. `$pr-hygiene` is a separate axis
and is not established here.
