# Project review — tesla-key-esp32 (2026-09-13)

Read-only whole-project coherence audit at `ca62409023579806411d95b9e4db36a9ab57fe2b`
(`main`, PR #296 merged). Scope and structure follow
[`$project-review`](../../.agents/skills/project-review/SKILL.md). **No source, configuration,
skill, workflow or PR state was modified** — this document is the only artifact produced. No
device, vehicle, NVS dump, pairing key, live session or production signing key was accessed;
there was no flash, OTA, release or vehicle command.

## Summary

The predecessor backlog is **fully closed**. Every open item recorded in
[`2026-09-11`](2026-09-11-error-review.md) — `F01` (VIN journal removed before its cleanup was
proven), `F02` (RSAVP1 range check), `F03` (pre-push hook auditing a fabricated push), `F14`
(`$mock-test` missing from both sibling checklists), the three surviving `F15` rows and `A01`–`A03`
— was verified fixed in the tree, and every host gate that can run in this container passes.
The pinned inventories still agree across code, config, tests and docs: 21 fixed HTTP routes plus
3 vehicle routes, 16 commands / 9 MCP tools, 19 NVS records, 55 MQTT discovery rows, 4 targets,
5 ordered `tesla-ble` patches, and the numeric invariants (health gate 90 s / 600 s, heap watchdog
4 KB / 5 min / 5 restarts, link state 60 / 120 / 150 s, active window 5 min, ChargeState 30 s,
safe-mode threshold 4, heap ring 288 samples, app policy limit `0x1e8000`) match the constants
they describe.

**6 findings: 1 P2, 2 P3, 1 NIT, 2 SKILL-DRIFT.** The headline is a browser-CSRF gate that can be
stepped around by changing the *case* of a query key: ESP-IDF's `httpd_query_key_value()` matches
parameter names case-insensitively, but the classifier that decides whether a GET is
state-changing compares them case-sensitively. `GET /diag?CLEAR=1`, `?VERBOSE=0|1` and
`GET /coredump?CLEAR=1` therefore execute their mutation with the gate never consulted — including
the core-dump erase, the exact artifact `/crash/dismiss` was deliberately made a POST to protect.
The two P3s are the same shape as each other: the new authoritative-clock contract introduced with
`main/time_sync.hpp` was applied to one of two sibling timestamp writers, and the hardware-entropy
window documented in `main.cpp` was not extended to the new safe-mode key parse. Neither touches
key material, wire behaviour, partitions or the release path.

## Findings

### [BUG] P2 — the browser-CSRF gate is bypassed by a case-differing query key

- **Where:** classifier `main/logic/http_origin.hpp:112` (inside `query_has_exact`, used by
  `mutation_origin_required` at `:124-133`) vs. handler-side parameter read
  `main/http_common.cpp:95-102` (`query_param_is` → `httpd_query_key_value`); dispatch
  `main/http_server.cpp:85-87`; sinks `main/http_status.cpp:265` (`diag_log_clear()`), `:266-267`
  (`diag_set_verbose`), `:380` (`esp_core_dump_image_erase()`); documented promise
  `docs/SECURITY.md:102-103`; test gap `test/test_logic.cpp:3981-3992`.
- **What:** `GET` is normally read-only, so four legacy routes are classified as mutating by
  inspecting the raw query string:

  ```cpp
  // main/logic/http_origin.hpp:112
  if (equals != std::string_view::npos && item.substr(0, equals) == key && ...
  ```

  That key comparison is **case-sensitive**. The handler that actually performs the mutation reads
  the same parameter through ESP-IDF's `httpd_query_key_value()`, which matches the key with
  `strncasecmp()` — **case-insensitive** (verified against
  `espressif/esp-idf@v5.5.5 components/esp_http_server/src/httpd_parse.c`; the pin is
  `esp-idf-toolchain.txt`). The two layers therefore disagree about what the request asks for:

  | Request | `mutation_origin_required()` | handler mutates |
  |---|---|---|
  | `GET /diag?clear=1` | true (gated) | yes |
  | `GET /diag?CLEAR=1` | **false (ungated)** | **yes** |
  | `GET /diag?VERBOSE=1` | **false** | **yes** |
  | `GET /diag?VERBOSE=0` | **false** | **yes** |
  | `GET /coredump?CLEAR=1` | **false** | **yes** |
  | `GET /diag?CLEAR=1&clear=0` | **false** | **yes** (IDF takes the first case-insensitive hit) |

  `/ota/check` is unaffected: it is gated on the path alone, with no query predicate.
- **Why it matters:** a foreign web origin can make a LAN user's browser issue a plain no-preflight
  cross-site GET (`<img src="http://tesla-key-esp32.local/coredump?CLEAR=1">`) and the device
  erases the core-dump partition, wipes the `/diag` ring, or switches raw-RX verbose logging on.
  The `403` that `handle_all_dispatch` would have returned is never reached, because the request
  was never classified as mutating. This defeats a security control the project states it provides
  (`docs/SECURITY.md:102-103` lists exactly these three lowercase forms as covered), and it
  destroys the forensic artifacts a bug report depends on — the same destruction for which
  `/crash/dismiss` was deliberately made POST-only ("it destroys the one artifact a bug report
  needs, so no link or prefetch may reach it", `docs/README.md`). It is not remote code execution,
  key loss or a vehicle action, and a raw LAN peer could already call these endpoints; the loss is
  the browser-CSRF mitigation itself.
- **Confidence:** **confirmed.** Reproduced on the host by compiling the unmodified
  `main/logic/http_origin.hpp` against a faithful transcription of ESP-IDF v5.5.5's
  `httpd_query_key_value()` key matching. Output (scratchpad only, nothing added to the repo):

  ```text
  /diag?clear=1                gate_required=true  handler_mutates=true
  /diag?CLEAR=1                gate_required=false handler_mutates=true  <<< UNGATED MUTATION
  /diag?Clear=1                gate_required=false handler_mutates=true  <<< UNGATED MUTATION
  /diag?VERBOSE=1              gate_required=false handler_mutates=true  <<< UNGATED MUTATION
  /diag?VERBOSE=0              gate_required=false handler_mutates=true  <<< UNGATED MUTATION
  /coredump?CLEAR=1            gate_required=false handler_mutates=true  <<< UNGATED MUTATION
  /coredump?clear=1            gate_required=true  handler_mutates=true
  /diag?CLEAR=1&clear=0        gate_required=false handler_mutates=true  <<< UNGATED MUTATION
  ```

  Not exercised: on-device HTTP, a real browser against real firmware, remote CI.
- **Fix:** make the classifier's key comparison match the parser it is guarding. `http_origin.hpp`
  already carries `ascii_iequal()` a few lines above, so this is local:

  ```cpp
  // main/logic/http_origin.hpp — query_has_exact()
  if (equals != std::string_view::npos && ascii_iequal(item.substr(0, equals), key) &&
      item.substr(equals + 1) == value) {
  ```

  Keep the **value** comparison case-sensitive: `httpd_query_key_value()` copies the value
  verbatim (no percent-decoding, no case folding), so `strcmp(val, "1")` in `query_param_is` is
  already exact and the two sides then agree in both directions. Move together with it:
  `test/test_logic.cpp` (`test_http_origin`) — add `CHECK(tk::mutation_origin_required(false,
  "/diag?CLEAR=1"))`, `"/diag?Verbose=0"`, `"/coredump?CLEAR=1"`, and keep the existing negative
  cases (`"/diag?clear=0&verbose=2"`, `"/diag?next=clear=1"`) so a blanket case-fold cannot widen
  the gate by accident. `docs/SECURITY.md:102-103` should say the key match is case-insensitive so
  the next reader does not re-derive this. No change is required in `main/http_common.cpp`,
  `main/http_status.cpp` or the route table.

### [INCONSISTENCY] P3 — `key_created` is still stamped from the non-authoritative NVS clock

- **Where:** `main/vehicle_pairing.cpp:467-481` (the `kKeyCreated` write in
  `generate_key_locked_()`) vs. its sibling `main/vehicle_pairing.cpp:728-747` (`paired_at()`, gate at `:740`);
  contract `main/time_sync.hpp:3-6`; stale call-site comment `main/main.cpp:487`; consumer
  `main/logic/status_model.hpp:206`.
- **What:** `d86fd48` introduced an explicit authority distinction — a clock restored from NVS is
  *not* authoritative, because it is "historical time from a prior shutdown/sync" — and applied it
  to `paired_at()`:

  ```cpp
  // main/vehicle_pairing.cpp:739-740 — gated
  time_t now = time(nullptr);
  if (clock_is_authoritative() && now > 1600000000) { ... }
  ```

  The `kKeyCreated` write three hundred lines earlier was not changed and still stamps
  `time(nullptr)` unconditionally, with a comment that describes the superseded rule ("Wall-clock
  comes from the browser (POST `/set_time`) or the NVS-cached time"). `restore_clock_from_nvs()`
  runs at `main/main.cpp:494`, long before NTP and before any browser can visit, so on such a boot
  `time(nullptr)` already returns a plausible-looking past timestamp.
- **Why it matters:** on a board that reboots onto a network which blocks NTP and is not opened in
  a browser (a heap-watchdog restart, a power cut, a headless evcc-only install), an automatic
  re-key — the auto-pair path that runs when the car deleted our key — writes `key_created` as the
  moment of the *previous* sync. `/status.key_created` and the web UI's key card then show a
  creation date that predates the reboot which created the key, and the value is durable: nothing
  rewrites `kKeyCreated` until the next rotation. The two timestamps sit side by side in the same
  card and now obey different authority rules, which is precisely the drift `time_sync.hpp` was
  added to remove. No key material, pairing or command path is affected.
- **Confidence:** **confirmed** by source path (`restore_clock_from_nvs` at `main.cpp:494` →
  `generate_key_locked_()` at `vehicle_pairing.cpp:473` with no authority predicate). The *field
  frequency* of an NTP-blocked re-key boot is not measured here.
- **Fix:** gate the stamp the same way as its sibling and let the existing
  `kEpochPlausibleFloor` omission carry the unstamped case:

  ```cpp
  // main/vehicle_pairing.cpp — generate_key_locked_()
  time_t now = time(nullptr);
  if (clock_is_authoritative() && now > 1600000000) { /* save kKeyCreated */ }
  ```

  Then decide whether a later authoritative sync should back-fill it (as `paired_at()` does on
  first observation) or whether an unknown creation date is preferable — and say which in the
  comment. Move together with it: the comment at `main/vehicle_pairing.cpp:467-470`, and
  `main/main.cpp:487`, which still lists "the `key_created`/`paired_at` stamps" as legitimate
  consumers of the restored clock — no longer true for `paired_at`, and the point of this fix for
  `key_created`.

### [INCONSISTENCY] P3 — the safe-mode key parse runs outside the documented entropy window

- **Where:** `main/vehicle_ctrl.cpp:276-281` (`init_safe_mode` → `compute_key_fingerprint_()`) vs.
  the window opened at `main/main.cpp:519-525` and closed at `:674`; the safe-mode branch is
  `main/main.cpp:512-517`.
- **What:** `main.cpp` states the invariant in full:

  > "The DRBG is seeded exactly once at that point … Keep SAR-ADC entropy active across BOTH
  > controller construction/key load and a possible first-boot key generation; WiFi/BLE are not
  > running yet and therefore cannot supply RF entropy themselves."

  `bootloader_random_enable()` is called only in the non-safe-mode branch (`main.cpp:525`).
  `d86fd48` added a private-key load to the **safe-mode** path: `init_safe_mode()` now calls
  `compute_key_fingerprint_()`, which runs `mbedtls_ctr_drbg_seed()` and `mbedtls_pk_parse_key()`
  on the stored PEM — a key load outside the very window the comment says must cover it, with
  neither RF nor SAR-ADC entropy enabled.
- **Why it matters:** two separate things, both bounded. (1) The DRBG output here is used only as
  the RNG argument to `mbedtls_pk_parse_key()` (blinding) and the result is a public fingerprint,
  so **no key material is weakened** — this is an invariant/coherence defect, not a crypto break.
  (2) The larger concern is that safe mode exists so a board that keeps crashing stays fixable in
  a browser, and this adds an allocating, throwing mbedtls parse to that recovery path *before*
  the HTTP server starts. It is contained (`app_main`'s top-level `try/catch` at
  `main/main.cpp:277` → `boot_fatal`), but containment here means the web UI never comes up, which
  is the one outcome safe mode is designed to prevent. Heap at that point is at its most plentiful
  (no WiFi, BLE, MQTT), so the probability is low.
- **Confidence:** **confirmed** for the missing entropy window (single `bootloader_random_enable()`
  call site, inside the `else` branch). **Suspected** for the recovery-path risk — verify by
  instrumenting `compute_key_fingerprint_()`'s peak allocation on a pinned IDF build, which this
  container did not run.
- **Fix:** pick one and state it. Either bracket the safe-mode branch with
  `bootloader_random_enable()` / `bootloader_random_disable()` exactly as the normal branch does,
  or — cheaper and arguably better for a recovery path — skip the fingerprint pre-warm in
  `init_safe_mode()` entirely and let `key_fingerprint()` compute it lazily on the first `/status`
  (by then WiFi is up and supplying RF entropy, and a throw lands inside `handle_all`'s
  containment net as a 503 instead of halting boot). Update the comment at `main/main.cpp:519-524`
  to name whichever load sites the window is required to cover.

### [SKILL-DRIFT] `main/time_sync.hpp` is absent from the project map

- **Where:** [`.agents/skills/project-review/SKILL.md`](../../.agents/skills/project-review/SKILL.md)
  *Project map* table (no row names it); the file is `main/time_sync.hpp`, added by `d86fd48`.
- **What:** the skill's own self-check requires that every `main/*.{cpp,hpp}` land in the project
  map, and calls an unmapped file "the signal that a whole subsystem appeared without the skill
  noticing". `main/time_sync.hpp` is the new clock-authority seam
  (`clock_is_authoritative()` / `mark_clock_authoritative()` / `clock_synced_via_ntp()`) with four
  consumers — `main/main.cpp`, `main/http_handlers.hpp`, `main/mqtt_ha.cpp`,
  `main/vehicle_pairing.cpp` — and it is the contract the two P3 findings above turn on. It is the
  only unmapped file: every other `main/*` header/source resolves to a map row, and
  `main/logic/*.hpp` is covered by the wildcard row plus `test/logic_test_ownership.json`.
- **Why it matters:** the next reviewer builds the intended model from this table. A cross-cutting
  contract that is not in it is one nobody is asked to re-derive — which is exactly how the
  `key_created` half of it was missed.
- **Confidence:** confirmed (mechanical sweep of `main/*.cpp`, `main/*.hpp`, `main/*.h` against the
  table).
- **Fix:** add a *Clock authority* row to the *Project map*, e.g. `main/time_sync.hpp` +
  `main/main.cpp` (`on_time_sync`, `restore_clock_from_nvs`) — "the ONE seam deciding whether the
  wall clock is authoritative (SNTP or browser `/set_time`) as opposed to merely restored from NVS;
  every durable wall-clock stamp (`key_created`, `paired_at`, MQTT `boot_time`) must consult it."
  Then add the matching cross-cutting link: **new durable wall-clock stamp → gate it on
  `clock_is_authoritative()` AND cover it in the `/status` plausibility floor.**

### [SKILL-DRIFT] `doc_drift_checker` omits three cross-cutting links this skill owns

- **Where:** [`.codex/agents/doc_drift_checker.toml`](../../.codex/agents/doc_drift_checker.toml)
  (the `developer_instructions` fact-class list) vs. the *Cross-cutting consistency* section of
  [`$project-review`](../../.agents/skills/project-review/SKILL.md).
- **What:** `$project-review` states that this agent's enumeration "must stay a subset of — and
  agree with — the *Cross-cutting consistency* section above; a link added here that it lacks (or a
  stale one it still lists) is `SKILL-DRIFT`." Three links are not named by the agent:
  **sleep/link-state change** (the `link_state()` single source feeding *both* the web-UI hero and
  MQTT `sleep_state`, with an exhaustive MQTT switch and every web state incl. `unknown`),
  **WiFi/LAN reconnect or watchdog change** (`main/net.cpp` + `main/logic/net_link.hpp` + the
  "WiFi / LAN connectivity" numbers in `docs/ARCHITECTURE.md`), and **MQTT transport / TLS-default
  change** (`mqtt_ha_start`'s schemeless-broker rule → `/status.mqtt.tls` → the web UI's
  "· secured" row → three docs). All three predate the agent's last edit (`fed9083`, 2026-09-06),
  so this is standing lag, not a fresh regression.
- **Why it matters:** these are the two historically most-broken links in the repo (the `unknown`
  → false-"asleep" fall-through, and the watchdog-must-never-reboot rule), and the agent is the
  fast lens a targeted diff review delegates to. Its generic instruction still points at the right
  documents, which is why the impact is limited to reduced prescribed coverage rather than a wrong
  answer.
- **Confidence:** confirmed for the absence of the three links; the judgement that the agent's
  thematic granularity *should* mirror this skill's link granularity is the skill's own stated rule,
  not an independent fact.
- **Fix:** add the three bullets to `doc_drift_checker.toml` in the vocabulary it already uses, and
  — because the skill requires the pair to move together — re-read this section whenever the
  cross-cutting list changes. Do not edit either during a review-only pass.

### [NIT] the key-fingerprint cache is not invalidated when durable identity becomes unknown

- **Where:** `main/vehicle_pairing.cpp:805-820` (`key_fingerprint()`), cache writers
  `main/vehicle_ctrl.cpp:201-204` / `:278-281` and `main/vehicle_pairing.cpp:462-466`;
  the ambiguous branch `main/vehicle_pairing.cpp:449-460`.
- **What:** `d86fd48` made `key_fingerprint()` serve a cached string (a good change — it removed a
  full PEM load + mbedtls parse from every 4 s `/status` poll). The cache is written at `init()`,
  at `init_safe_mode()` and after a *committed* rotation. It is **not** cleared on the
  `CommitUnknown` branch, where the code itself says "tesla-ble may restore the old RAM key while
  flash already contains the new one, so neither runtime fingerprint can classify durable
  identity". `/status.key_fingerprint` therefore keeps reporting the pre-rotation value until the
  mandated reboot, where the pre-change code would have read whatever is durably in NVS.
- **Why it matters:** confined to a diagnostic display. The decision paths are already fenced:
  `key_reload_required_` blocks `/set_vin` staging (`reset_for_new_vehicle()` refuses with
  `IdentityRecoveryPending`) and `key_runtime_safe_` blocks signing, and the boot-time VIN-recovery
  classifier reads a cache that `init()` pre-warms *after* `recover_pending_key_rotation_at_boot_()`.
  So no decision is made on the stale value; only the number a human reads is stale.
- **Confidence:** confirmed by reading the writer/reader set; no runtime reproduction (it needs an
  ambiguous NVS commit failure).
- **Fix:** clear `key_fingerprint_cache_` under `cache_mutex_` on the same branch that sets
  `key_reload_required_.store(true)`, so the reader falls through to the storage-backed path and
  reports the durable value.

## Coherence check

| Axis | Result |
|---|---|
| Routes: `logic/http_route.hpp` (21 fixed + 3 vehicle) ↔ `docs/README.md` | ✓ consistent |
| Commands: `logic/command_registry.hpp` (16 rows) ↔ `docs/README.md` ↔ `command_exec.cpp` | ✓ consistent |
| MCP tools: 9 MCP-visible rows ↔ `docs/MCP.md` table ↔ `test_mcp` | ✓ consistent |
| `/status` fields: `logic/status_model.hpp` ↔ `docs/README.md` ↔ `www/app.js` | ✓ consistent (`odometer_km` documented in `docs/ARCHITECTURE.md`, the telemetry owner; `brownout`/`panic`/`task_wdt` are `reset_reason` values, not fields) |
| Presenter mirrors: `logic/ble_row.hpp` ↔ `www/app.js`; `logic/display_model.hpp` ↔ `tools/display_sim.py` | ✓ consistent (960 and 38 golden vectors, both parity gates green) |
| Redaction: identifier-carrying log sites ↔ `logic/redact.hpp` `kDiagRedactions` | ✓ consistent — swept every `ESP_LOG*` in `main/`; no uncovered VIN/SSID/IP/vehicle-MAC/broker/syslog-host sink. Remaining `%s` sites interpolate fixed literals (`tesla-key-esp32-setup`, `MDNS_HOSTNAME`, `kNetHostname`, Ethernet candidate names) or the deliberately-retained key fingerprint |
| NVS: `logic/nvs_contract.hpp` (19 records) ↔ `docs/README.md` ↔ `docs/SECURITY.md` ↔ host oracle | ✓ consistent (`check-nvs-contract.py` PASS with mutation canaries) |
| MQTT discovery: `logic/mqtt_discovery_registry.hpp` (55 rows) ↔ `docs/ARCHITECTURE.md` ↔ `docs/FEATURES.md` | ✓ consistent |
| Partitions: `partitions.csv` ↔ every doc quoting `0x9000` / `0xf000` / `0x20000` / `0x1f0000` / 4 MB | ✓ consistent |
| Targets/suffixes: `logic/target.hpp` ↔ `platform.hpp` ↔ `ota_update.cpp` ↔ `ci-build-all.sh` ↔ `ci-sign-artifacts.sh` ↔ `build-pages.sh` | ✓ consistent (4 targets; boot offset `0x1000` esp32 / `0x0` others) |
| Pins: ESP-IDF `v5.5.5` + digest, `tesla-ble v5.1.3`, 5 ordered patches, 4 dependency locks | ✓ consistent (`check-dependency-contract.py` PASS) |
| Version: `version.txt` = `1.4.0`, no hardcoded production version elsewhere | ✓ consistent (remaining `1.2.x`/`1.3.x` literals are all self-test fixtures) |
| Size budget: `APP_POLICY_LIMIT` `0x1e8000`, esp32c6 cliff, 4-target baseline | ✓ consistent with `.agents/rules/firmware-size-budget.md` |
| Numeric invariants (health gate, heap watchdog, link state, active window, body caps, safe-mode threshold, heap ring) | ✓ consistent with `docs/ARCHITECTURE.md` |
| Sleep/link state: `link_state()` → web hero + MQTT `sleep_state` (exhaustive, `Unknown` ⇒ omitted) | ✓ consistent |
| Kconfig ↔ docs | ✓ consistent — options the docs quote defaults for (`TESLA_ETH_WAIT_S`, `TESLA_ETH_POLL_MS`, the enable flags) match; per-pin GPIO options are described narratively, which is not drift |
| Browser-CSRF gate: `logic/http_origin.hpp` ↔ handlers ↔ `docs/SECURITY.md` | ✗ **drifted** — see the P2 finding |
| Clock authority: `main/time_sync.hpp` ↔ `paired_at` / `key_created` / MQTT `boot_time` | ✗ **drifted** — see the first P3 finding |
| Skills ↔ project (13 skills under `.agents/skills/`) | ✓ consistent except the project map gap; both sibling checklists now cover all 12 siblings incl. `$mock-test`, `$add-logic-test` carries the ownership-registration step, `$ship` assigns and validates `MERGE_SHA`, `$pr-hygiene` and `AGENTS.md` both record the Renovate merge exemption, `$device-diag` already reads `sys.stack_min_free_bytes`, `$display-preview`'s CLI modes match `tools/display_sim.py`'s `__main__` |
| Agents ↔ project (4 manifests under `.codex/agents/`) | ✗ `doc_drift_checker` — see the SKILL-DRIFT finding; `heap_safety_reviewer`, `agent_config_reviewer` and `multi_target_build_reviewer` still restate their current contracts |

## Evidence and its boundaries

Run in this container at `ca62409`:

| Gate | Result |
|---|---|
| `scripts/run-mock-tests.sh --require-all` | **PASS** for every gate except the browser gate, which reported "no Chrome/Chromium binary found" because no `chromium` is on `PATH` here |
| `test/web_ui_browser_gate.py` with `BROWSER_BIN` set to the container's Chromium | **PASS** (real DOM, console, keyboard, live regions, desktop/mobile layout) |
| `scripts/repo-lint.sh` | **PASS** (shell, Python, Node, JSON, TOML, workflows, links, ownership, diff) |
| `scripts/test-build-contracts.sh` | **PASS** (pins, targets, partitions, CI/release contracts, build/manifest contract) |
| `scripts/test-pr-gates.sh` | **PASS** (117 agent-hook cases) |
| `tools/agent-config/selftest.sh` | **PASS** (52 mutation canaries) |
| `scripts/run-sanitizer-tests.sh` | **PASS** (ASan/UBSan/LSan, 637 runtime-boundary checks, deterministic fuzz seed `6072351259557053785`) |
| ctest host suite | **PASS** (4/4), 65 Node web-UI cases, 960 BLE-row + 38 display golden vectors |

**Not run:** the pinned four-target ESP-IDF build (`scripts/idf-docker.sh` / `ci-build-verify.sh`),
disposable-key signing contract, remote CI, hardware/USB, flash, OTA, browser-against-real-firmware,
and every vehicle/live-device boundary. No claim in this document rests on any of them. The browser
gate's pass above is a host DOM check, not a check against firmware-served HTML.

## Gate readiness

**Not established.** The review is not clean (one P2), so no `$project-review` merge-gate record is
offered here, and this review does not touch any PR body or checkbox. `$skill-audit` readiness is
likewise not established: `$skill-audit ⊂ $project-review`, and two `SKILL-DRIFT` findings are open.
`$pr-hygiene` is a separate axis and is never established by this review.

## Prioritized actions

1. **Must fix — P2.** Make `query_has_exact()`'s key comparison case-insensitive (`ascii_iequal`,
   already in the same header), keep the value comparison exact, and add the uppercase/mixed-case
   `CHECK`s to `test_http_origin` alongside the existing negative cases. Note the case-insensitive
   key match in `docs/SECURITY.md:102-103`.
2. **Should fix — P3.** Gate the `kKeyCreated` stamp on `clock_is_authoritative()` like its
   `paired_at()` sibling, decide and document whether a later sync back-fills it, and correct the
   now-stale comments at `main/vehicle_pairing.cpp:467-470` and `main/main.cpp:487`.
3. **Should fix — P3.** Resolve the safe-mode entropy window: either bracket the safe-mode branch
   with `bootloader_random_enable()`/`disable()`, or drop the safe-mode fingerprint pre-warm and
   let `/status` compute it lazily under HTTP containment. Update the window comment either way.
4. **Should fix — SKILL-DRIFT.** Add the *Clock authority* row (`main/time_sync.hpp`) to the
   `$project-review` project map and the matching "new durable wall-clock stamp → gate on
   `clock_is_authoritative()`" cross-cutting link.
5. **Nice to have — SKILL-DRIFT.** Add the sleep/link-state, WiFi-watchdog and MQTT-TLS-default
   links to `.codex/agents/doc_drift_checker.toml`.
6. **Nice to have — NIT.** Clear `key_fingerprint_cache_` on the `CommitUnknown` branch that sets
   `key_reload_required_`.

Items 1–3 are firmware changes and need the pinned four-target IDF build plus a re-run of the host
suite; item 1 additionally warrants an independent re-review of the browser-mutation gate after the
edit, since a blanket case-fold applied to the value side would widen the gate rather than close it.
