# Project review — tesla-key-esp32 (2026-09-07)

Read-only whole-project coherence audit at `3f550b78c230754b00227e61f563a4ad05cf82bd`
(`main`, PR #287 merged). Scope and structure follow
[`$project-review`](../../.agents/skills/project-review/SKILL.md). **No source, configuration,
skill or PR state was modified** — this document is the only artifact produced.

## Summary

The tree is in unusually good coherence: every host gate that can run in this container passes,
the target set / partition geometry / dependency pins / route / command / NVS / MQTT-registry
inventories all agree across code, config, tests and docs, and the numeric invariants quoted in
`docs/ARCHITECTURE.md` (health gate 90 s / 600 s, heap watchdog 4 KB / 5 min / 5 restarts,
link state 60 s / 120 s, active-window 5 min, ChargeState 30 s, body caps 2048 / 1024, safe-mode
threshold 4, heap ring 288 × 2 × int16 = 1152 B) match the constants they describe.

**8 findings: 1 P2, 1 P3, 3 doc-drift, 2 skill-drift, 1 tooling nit.** The headline risk is a
single class of defect the project has documented as recurring — the `?redact=1` promise. Two
identifier sinks added with their features carry no redaction rule, so a bug report a user
believes is scrubbed still names their car: an unredacted VIN on the interrupted-VIN-change
recovery path in `/diag`, and the VIN-derived Tesla advert name in `/status`. Neither is
reachable by an unauthenticated remote attacker; both defeat the redaction contract for its one
intended user. Everything else is documentation and review-map drift, none of it affecting
runtime behaviour.

## Findings

### [BUG] Unredacted VIN reaches `/diag?redact=1` on the VIN-transition recovery path

- **Where:** `main/main.cpp:550-552` (sink); `main/logic/redact.hpp:96-172`
  (`kDiagRedactions`, no matching rule); `main/diag_log.cpp:71-78` (capture hook)
- **What:** the interrupted-VIN-change rollback logs the previously configured VIN verbatim:

  ```c
  ESP_LOGW(TAG, "interrupted VIN change detected before key commit — restoring %s",
           marker.previous_vin.empty() ? "unconfigured VIN" : marker.previous_vin.c_str());
  ```

  `marker.previous_vin` is the real 17-character VIN parsed out of the `tesla_cfg/vin_txn`
  journal (`main/logic/vin_transition.hpp:117-125`). No entry in `kDiagRedactions` matches the
  phrase `"interrupted VIN change detected before key commit — restoring "`, and none of the
  existing VIN rules (`"main: VIN: "`, `"VehicleController ready for VIN "`, `" on VIN "`,
  `"/api/1/vehicles/"`) can match it.
- **Why it matters:** `diag_log_init()` runs at `main/main.cpp:274` — the first statement in
  `app_main`, long before line 550 — so the line lands in the `/diag` ring, and
  `syslog_start()` at line 387 means it is also forwarded. `GET /diag?redact=1` therefore
  answers **HTTP 200 while naming the car**, which is precisely the failure mode
  `redact.hpp` documents ("a new phrase carrying an old value is a SILENT leak… This has now
  happened twice"). This is the third instance. The VIN is the identifier that file singles out
  as the sharp one: it names a car that can be located in a driveway, and the advert name the
  device targets is derived from it.
- **Confidence:** confirmed by code path (static). Reproducing it on hardware requires a power
  loss between the `vin_txn` journal write and the key commit during `POST /set_vin`; the leak
  itself needs no reproduction — the rule table demonstrably has no matching entry.
- **Fix:** add one rule to `kDiagRedactions` in `main/logic/redact.hpp`, e.g.
  `{"interrupted VIN change detected before key commit — restoring ", ""}` (no end token — the
  value is last on the line, and the fail-closed run-to-end-of-line behaviour is what the
  neighbouring VIN rules already use), plus a `CHECK` in `test/test_logic.cpp` `test_redact`
  covering both the populated and the `"unconfigured VIN"` branch. Consider also asserting the
  `"unconfigured VIN"` literal stays outside the span, so the redacted form still distinguishes
  "there was no previous VIN" from "we scrubbed one".

### [INCONSISTENCY] `/status?redact=1` leaks the VIN-derived Tesla advert name it scrubs in `/diag`

- **Where:** `main/logic/status_model.hpp:326` (`e.str("name", d.name.c_str())` — no
  `redacted_or`), against `main/logic/status_model.hpp:325` (`addr`, redacted) and
  `main/logic/redact.hpp:127-140` (`/diag` rules `{"Tesla '", "' found: "}`)
- **What:** in the `ble.devices[]` scan listing, `addr` is redacted under `?redact=1` but the
  advertised `name` beside it is emitted verbatim. `main/ble_client.cpp:793` filters the listing
  to `adv_name_len == 18 && TeslaBLE::is_tesla_vehicle_name(adv_name)`, so every entry is a
  Tesla and every `name` is the VIN-derived `S<hash>C` advert name — for the reporter's own car
  and for every neighbouring Tesla in range.
- **Why it matters:** the two redaction surfaces disagree about the *same value class*.
  `redact.hpp` states the rule explicitly for the `/diag` side — "the advert name… is derived
  from the VIN and is therefore as identifying as the VIN — it is not a nickname" — and its
  `/status` list (`kRedactedStatusFields = 6`: `vin`, `ip`, `wifi.ssid`, `ble.addr`,
  `mqtt.broker`, `syslog.host`) never picked the field up. A pasted `?redact=1` status blob
  therefore carries a stable, scanner-matchable identifier for a specific parked car, which is
  the exact threat the redaction feature exists to remove. `docs/README.md:353-357` also promises
  only that `ble.addr` "(and every scanned neighbour's)" is scrubbed, so the doc inherits the gap.
- **Confidence:** confirmed (static; the emitter, the scan filter and the field list were all read).
- **Fix:** wrap the field — `e.str("name", redacted_or(d.name, in.redact).c_str())` — raise
  `kRedactedStatusFields` to 7 and add `ble.devices[].name` to the documented set in
  `redact.hpp`; extend `test_status_model`'s redacted golden emission in `test/test_logic.cpp`;
  update the `?redact=1` description in `docs/README.md`. Redacting the value (not omitting the
  key) keeps the presence semantics the header argues for.

### [DOC-DRIFT] `usable_battery_level` was served on five surfaces but documented on one

- **Where:** feature commit `7eb3741` ("decode and serve usable_battery_level across all
  surfaces", #276/#283). Emitted by `main/logic/status_model.hpp:352` (`vehicle.usable_soc`) and
  `:368` (`last.usable_soc`), `main/mqtt_payloads.hpp:102` (MQTT `charge` payload),
  `main/mcp_json_payloads.hpp:203` (`get_vehicle_state`), `main/logic/vehicle_data.hpp:50`
  (`/vehicle_data`), `main/www/app.js:329,395,418,434,470`.
  Documented only in `docs/README.md:270,276` (the `/vehicle_data` block).
- **What:** four documented mirrors were not updated:
  - `docs/README.md:318` — `vehicle:{soc,status,charge_limit,power,amps,actual_amps,volts,phases}`
    omits `usable_soc`.
  - `docs/README.md:328` — `last:{soc,status}` omits `usable_soc`.
  - `docs/README.md:477-479` — `tesla-key/<node>/charge {…}` omits `usable_soc`.
  - `docs/ARCHITECTURE.md:590-594` — the MQTT charge entity list omits it; `docs/MCP.md`'s
    `get_vehicle_state` field notes and example payload omit it too.
- **Why it matters:** this is the `$project-review` cross-cutting link "**New telemetry field** →
  parser **and** `/status` JSON **and** MQTT discovery **and** the web UI **and** docs" failing on
  its last leg. It also leaves an undocumented asymmetry: `usable_soc` rides the retained MQTT
  `charge` payload but has **no** row in the 55-entry `logic/mqtt_discovery_registry.hpp`, so it
  creates no HA entity. That may well be deliberate (the four `*_stack_min_free_bytes` fields are
  payload-only by design and `docs/ARCHITECTURE.md:596-599` says so explicitly) — but nothing
  records the decision for `usable_soc`, so the next reader cannot tell an intent from an omission.
- **Confidence:** confirmed (`git show --stat 7eb3741`; grep across all five surfaces).
- **Fix:** add `usable_soc` to the three `docs/README.md` field lists and the
  `docs/ARCHITECTURE.md` charge-entity list; mention it in `docs/MCP.md`'s `get_vehicle_state`
  notes. Separately, decide and **write down** whether it should gain a discovery row (→ 56 rows,
  plus the registry gate's fixtures) or is deliberately payload-only like the stack fields.

### [DOC-DRIFT] `docs/ARCHITECTURE.md` names a WiFi-watchdog constant that does not exist

- **Where:** `docs/ARCHITECTURE.md:1012` vs. `main/logic/net_link.hpp:92` and `main/net.cpp:1056`
- **What:** the doc says "after `kWdFailToReassoc` (2) consecutive failures (~60 s) it forces one
  `esp_wifi_disconnect()`". No such identifier exists anywhere in the tree; the constant is
  `tk::kWatchFailsToRecover` (value 2, so the *number* is right — only the name is stale).
- **Why it matters:** the cross-cutting rule for this subsystem
  ([`$project-review` SKILL.md:400-409](../../.agents/skills/project-review/SKILL.md)) requires the
  decision constant to be "mirrored in the **WiFi / LAN connectivity** section of
  `docs/ARCHITECTURE.md` (which quotes those numbers)". A grep for the documented name returns
  nothing, so the mirror cannot be verified from the doc side — and the skill itself already uses
  the correct name, so the two review sources disagree.
- **Confidence:** confirmed (`grep -rn "kWdFailToReassoc\|kWatchFailsToRecover"` — one hit in the
  doc, the real constant in `net_link.hpp` and `net.cpp`).
- **Fix:** rename to `kWatchFailsToRecover` in `docs/ARCHITECTURE.md:1012`.

### [DOC-DRIFT] `ver_newer()` does not exist — the OTA freshness comparator is `compare_ota_versions()`

- **Where:** `docs/ARCHITECTURE.md:517` and `.agents/skills/ota-release-verify/SKILL.md:112`, vs.
  `main/logic/ota_contract.hpp:80` (`tk::compare_ota_versions`), used at
  `main/ota_update.cpp:443,598`
- **What:** both documents attribute the version comparison to a function named `ver_newer()`.
  There is no such symbol; the comparator is `tk::compare_ota_versions()` returning
  `OtaVersionOrder`, with `tk::canonical_ota_version()` as the grammar gate. The behavioural
  claim ("parses only `x.y.z` and ignores the suffix") is still accurate —
  `ota_contract.hpp:77-79` says the same thing — so only the name is wrong.
- **Why it matters:** the PR-preview version-base argument in `ARCHITECTURE.md` (why a
  `<latest-stable>-PR-<N>` base guarantees the later main release compares strictly newer) rests
  on that function's semantics. A reader who greps for it to check the argument finds nothing,
  and `$ota-release-verify` sends its operator to the same dead name.
- **Confidence:** confirmed (repo-wide grep returns zero hits for `ver_newer`).
- **Fix:** replace with `tk::compare_ota_versions()` in both places, ideally citing
  `main/logic/ota_contract.hpp` so the grammar and the comparator are reachable from the claim.

### [SKILL-DRIFT] `$ota-release-verify` cites a stale line range for the downgrade gate

- **Where:** `.agents/skills/ota-release-verify/SKILL.md:109` — "Device-side **downgrade gate**
  (`ota_update.cpp` ~266-284)"
- **What:** the downgrade gate now lives at `main/ota_update.cpp:579-624`
  (`esp_https_ota_get_img_desc` at :584, the refusal at :598-603, the manifest/image agreement
  check at :618). Lines 266-284 are unrelated.
- **Why it matters:** `$skill-audit` classifies "a wrong number… a removed or renamed thing"
  as drift precisely because a future run follows the pointer. This is the only line-number
  citation of its kind in the tree, so it is also the only one that can rot this way.
- **Confidence:** confirmed.
- **Fix:** update to `~579-624`, or — more durable — drop the line range and cite the
  `esp_https_ota_get_img_desc` call site by name, as the same skill does elsewhere.

### [SKILL-DRIFT] `redact.hpp` rule comments point at the wrong source files

- **Where:** `main/logic/redact.hpp:122`, `:124` and `:329`
- **What:** three attributions moved with their code and were not followed:
  - `:122` "vehicle_ctrl.cpp `"Tesla MAC saved: %s"`" → actually `main/vehicle_telemetry.cpp:492`
  - `:124` "vehicle_ctrl.cpp `"could not persist Tesla MAC %s …"`" → actually
    `main/vehicle_telemetry.cpp:490`
  - `:329` "main.cpp's `"could not set DHCP hostname '%s'"`" → actually `main/net.cpp:255`
    **and** `main/net.cpp:770` (two sites now, since the Ethernet path was added)
- **Why it matters:** the rules themselves still fire — they are keyed on log phrases, not paths —
  so this is documentation, not a leak. But the file states its own reason for carrying the
  attributions: "the comment names it, because a reworded log line silently stops matching and the
  only symptom is a leak nobody sees." A reviewer auditing the table (which is exactly how the
  first finding in this report was found) is sent to files that no longer contain the line.
- **Confidence:** confirmed (`grep -rn` for each phrase across `main/`).
- **Fix:** correct the three comments; note the second DHCP-hostname site.

### [NIT] `run-sanitizer-tests.sh` picks clang++ without checking its sanitizer runtime links

- **Where:** `scripts/run-sanitizer-tests.sh:22-31`
- **What:** the compiler selection prefers `clang++` whenever it is on `PATH` and only verifies
  that the binary exists. On a host with clang installed but *without* compiler-rt (Debian/Ubuntu
  package `clang` without `libclang-rt-<N>-dev` — the configuration of this review container),
  configure dies inside CMake's compiler probe with
  `cannot find /usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.asan-x86_64.a`, exit 1 — even
  though the `g++` sitting beside it runs the entire suite green.
- **Why it matters:** the script's own header says it "deliberately fails instead of skipping when
  the compiler… is missing", and that fail-closed posture is right. But a *present-yet-unusable*
  sanitizer runtime is a different condition, and the resulting failure is an opaque CMake
  try-compile dump rather than a statement about the gate. On this host the gate is a false red:
  re-running with `CXX=g++` passes 4707 logic + 961 NVS + 637 runtime-boundary checks and the
  20 000-case fuzz corpus.
- **Confidence:** confirmed (reproduced both ways in this session — see *Evidence* below).
- **Fix:** after selecting a compiler, probe it once
  (`echo 'int main(){}' | "$CXX" -fsanitize=address,undefined,leak -x c++ - -o /dev/null`) and
  fall back to the next candidate on failure; keep the hard exit for the case where *no*
  candidate can link the sanitizer runtimes, and say that in the error message. CI is unaffected
  (its runner ships a complete clang), so this is developer-experience only.

## Coherence check

| Axis | Verdict |
|---|---|
| HTTP routes: `logic/http_route.hpp` (21 fixed + 3 vehicle) ↔ `docs/README.md` | ✓ consistent |
| Commands: `logic/command_registry.hpp` (15 REST + `get_vehicle_state`) ↔ `command_exec.cpp` ↔ `docs/README.md` ↔ `docs/MCP.md` (9 tools) | ✓ consistent |
| Argument bounds (amps 0–48, percent 50–100, start_minutes 0–1439) ↔ both surfaces ↔ docs | ✓ consistent |
| NVS: `logic/nvs_contract.hpp` 19 records ↔ `docs/README.md` table ↔ `docs/SECURITY.md` | ✓ consistent |
| MQTT: `logic/mqtt_discovery_registry.hpp` 55 rows ↔ `ARCHITECTURE.md` / `FEATURES.md` / `test/README.md` | ✓ consistent |
| `/status` field contract ↔ `docs/README.md` ↔ `main/www/app.js` | ✗ drifted (`usable_soc`) |
| Redaction: `/status` field set ↔ `/diag` phrase table ↔ actual log sites | ✗ drifted (two sinks) |
| Targets: `logic/target.hpp` ↔ `platform.hpp` ↔ `ota_update.cpp` ↔ `ci-build-all.sh` / `ci-sign-artifacts.sh` / `build-pages.sh` | ✓ consistent (all four, one list) |
| Partitions: `partitions.csv` (`nvs@0x9000/0x6000`, `otadata@0xf000/0x2000`, `coredump@0x12000/0xc000`, `ota_0@0x20000`, slots `0x1f0000`) ↔ every doc | ✓ consistent |
| Pins: ESP-IDF v5.5.5, tesla-ble v5.1.3, 5 ordered patches, 4 lockfiles ↔ docs ↔ skills | ✓ consistent |
| Runtime constants (health gate, heap watchdog, link state, active window, body caps, safe mode, heap ring) ↔ `ARCHITECTURE.md` | ✓ consistent apart from finding 4 |
| `version.txt` (1.4.0) as the sole version floor; no hardcoded version elsewhere | ✓ consistent (MCP.md's `1.4.25` is labelled illustrative) |
| `docs/MCP.md` Anthropic-API example (`claude-opus-4-8`, beta `mcp-client-2025-11-20`, `mcp_servers` + `mcp_toolset` both present) | ✓ valid — verified against current API reference, not from memory |
| Project map ↔ tree: every `main/*.cpp` / `main/*.hpp` is mapped; `main/logic/*.hpp` covered by the deliberate glob row | ✓ consistent |
| Skills ↔ project (`.agents/skills/*/SKILL.md`, `.codex/agents/*.toml`) | ✗ drifted (findings 6, and 5's skill half) |

Skills and reviewers checked individually: `$project-review`, `$skill-audit`, `$flash-esp32`,
`$ship`, `$vehicle-command-audit`, `$add-logic-test`, `$pr-hygiene`, `$feature-docs`,
`$device-diag`, `$display-preview`, `$ota-release-verify`, `$usb-recovery`, `$mock-test`;
`agent_config_reviewer`, `doc_drift_checker`, `heap_safety_reviewer`,
`multi_target_build_reviewer`. Only `$ota-release-verify` carries a contradiction (finding 6);
`$project-review`'s own "recency cross-check" (last touched at `fed9083`, nine `main/` commits
since) surfaced nothing beyond finding 3's documentation half.

## Evidence

Host/static only. Every other boundary is **not run**.

| Gate | Result |
|---|---|
| `scripts/repo-lint.sh` | PASS — 10 YAML, 6 workflows, 30 documents / 222 local targets, 58 headers, NVS + host-gate contracts |
| `scripts/run-mock-tests.sh` | PASS — incl. 960-vector `ble_row` parity, 38-case display-sim parity, C-boundary inventory (13 tasks, 21 callbacks) |
| `scripts/test-pr-gates.sh` | PASS — 110 hook self-tests |
| `tools/agent-config/selftest.sh` | PASS — 52 mutation canaries |
| `scripts/test-build-contracts.sh` | PASS — pins, targets, partitions, signing/Pages/release contracts |
| `scripts/run-sanitizer-tests.sh` | FAIL as invoked (environment — see finding 8); **PASS** with `CXX=g++`: 4707 + 961 + 637 checks, 20 000-case fuzz, ASan+UBSan+LSan |
| Web-UI browser gate | SKIPPED — no Chrome/Chromium in this container |
| Four-target pinned-IDF build | **not run** |
| Disposable-key signing contract | **not run** |
| Remote CI | **not run** |
| Hardware / USB / flash | **not run** |
| OTA | **not run** |
| Vehicle / live evcc | **not run** |
| Browser / visual | **not run** |

No compilation, host test or signed artifact in this report is evidence of hardware, flash, OTA,
signing, release or vehicle behaviour.

## Prioritized actions

1. **Must fix — close the two redaction gaps** (findings 1 and 2). Both are one-line changes plus
   test coverage, and both defeat `?redact=1` for the user who most needs it. Finding 1 is the
   third occurrence of the same pattern, so treat the mechanism as well as the instance: consider
   a gate that enumerates `ESP_LOG*` format strings carrying a `%s` fed from a VIN/SSID/MAC/host
   source and requires a matching `kDiagRedactions` phrase or an explicit allowlist entry — the
   current table can only be audited by hand, which is why the same bug keeps landing.
2. **Should fix — finish the `usable_battery_level` documentation** (finding 3), and record
   whether it is intentionally payload-only on MQTT. This is the cross-cutting link the project
   already tracks; leaving it half-done makes the next telemetry field harder to review.
3. **Should fix — the three stale symbol/line references** (findings 4, 5, 6). Cheap, and they are
   what a future automated or agent-driven review will follow.
4. **Nice to have — the `redact.hpp` attributions** (finding 7) and the sanitizer compiler probe
   (finding 8).

Gate readiness is deliberately **not** asserted: findings 1 and 2 are open, so no
`$project-review` merge record should be stamped from this run. `$pr-hygiene` is a separate axis
and is not established here.
