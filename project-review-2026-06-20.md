# Project review — tesla-key-esp32 (2026-06-20)

Holistic coherence audit (10-dimension fan-out, every finding adversarially verified)
plus a **live evcc → ESP32 → car** end-to-end test against the deployed device
(`192.168.1.194`, VIN `LRW…6735`, firmware **1.2.12**).

---

## Zusammenfassung (DE)

Das Projekt ist insgesamt **kohärent**. Der für evcc kritische Lese-Pfad ist im Live-Test
**fehlerfrei** (0 Timeouts, 0–1 ms, alle Felder vorhanden), Heap gesund, Sleep-Gating
korrekt, und die BLE-RX-Korruption, die fw 1.1.12 in eine Crash-Schleife trieb, wird von
1.2.12 jetzt **abgefangen statt abzustürzen**. Befunde: **1 echter Bug** (Geräte-Crash bei
nicht-numerischem JSON in drei Command-Handlern → Reboot-Schleife → Auto schläft nie),
**1 Inkonsistenz**, **6 Doc-Drifts**, **6 Nits**. Ein behaupteter Heap-Risiko-Befund wurde
in der Verifikation **widerlegt**. Der Write-Pfad (`set_charging_amps`) hatte einen
**zeitlich begrenzten** Timeout-Burst (12:34–12:36, ~21 min vor dem Test, nicht wiederkehrend),
verursacht durch RX-Korruption + Cold-Reconnect, die die Command-Latenz über evccs
HTTP-Timeout treibt.

## Summary (EN)

Overall coherence is **good**. The evcc-critical read path is **clean** in the live test (0
timeouts over 15 polls, 0–1 ms, every parsed field present), heap is healthy, sleep-gating
is correct, and the BLE-RX-corruption that crash-looped fw 1.1.12 is now **recovered, not
fatal**, in 1.2.12. Findings: **1 real BUG**, **1 INCONSISTENCY**, **6 DOC-DRIFT**, **6 NIT**
(14 confirmed; 1 suspected heap-risk **refuted** on verification). One write-path timeout
burst (`set_charging_amps`, 12:34–12:36, ~21 min before the test, not recurring) traces to
RX corruption + cold reconnect pushing command latency past evcc's HTTP client timeout — the
read path evcc actually loops on was unaffected.

Headline risk: the **BUG below is a remote (LAN) crash → reboot loop**, and a reboot loop is
explicitly the one thing that defeats the sleep design (each boot re-opens the polling
window). Everything else is documentation/quality drift.

---

## Findings (priority order)

### [BUG] `atof(j->valuestring)` null-deref crash on non-numeric JSON — 3 command handlers
- **Where:** [main/http_server.cpp:158](main/http_server.cpp:158) (`set_charging_amps`),
  [:167](main/http_server.cpp:167) (`set_charge_limit`),
  [:188](main/http_server.cpp:188) (`set_scheduled_charging` `start_minutes`).
- **What:** Each numeric param is parsed as
  `cJSON_IsNumber(j) ? j->valuedouble : atof(j->valuestring)`. For any present-but-non-number
  JSON value (`true`, `null`, `[]`, `{}`), cJSON leaves `valuestring == NULL` → `atof(NULL)`
  dereferences NULL in newlib `strtod` → **LoadProhibited panic → reboot**. There is no
  `cJSON_IsString` guard — unlike the VIN/broker paths ([:680](main/http_server.cpp:680),
  [:754](main/http_server.cpp:754)) which correctly guard.
- **Why it matters:** A single malformed command POST from any LAN client crashes the device.
  Per the project's own invariant a reboot re-opens the polling window, so a parked car never
  sleeps; a client repeating the bad request creates a **reboot loop** (vampire drain). The
  `handle_all` try/catch ([:883](main/http_server.cpp:883)) does **not** save it — a null
  dereference is a CPU exception, not a C++ throw.
- **Confidence:** **confirmed** — verifier traced cJSON internals against the actual build's
  source (`esp-idf/components/json/cJSON/cJSON.c`: node zero-init leaves `valuestring=NULL`;
  `parse_number` never sets it), and confirmed the catch can't contain a CPU exception.
- **Note:** evcc itself sends valid numbers, so this is **not** what evcc triggers in normal
  operation — but it is a real LAN-reachable crash vector.
- **Fix (apply to all three sites):**
  ```cpp
  int v = DEFAULT;
  if (cJSON_IsNumber(j))                       v = (int)j->valuedouble;
  else if (cJSON_IsString(j) && j->valuestring) v = atoi(j->valuestring);
  ```

### [INCONSISTENCY] Command tables list `door_lock`/`door_unlock` with no "rejected for this role" caveat
- **Where:** [.claude/CLAUDE.md:47](.claude/CLAUDE.md:47) and
  [docs/README.md:114](docs/README.md:114) (and prose [README.md:58](README.md:58)) vs the
  implementation [main/http_server.cpp:148](main/http_server.cpp:148),
  [main/vehicle_ctrl.cpp:761](main/vehicle_ctrl.cpp:761).
- **What:** `door_lock`/`door_unlock` are fully implemented and listed alongside working
  commands, while three docs assert the Charging-Manager key "cannot unlock doors".
- **Why it matters / refinement:** The security claim itself is **coherent** — verifier
  confirmed `docs/SECURITY.md:46-47` grounds it in the role restriction
  ([vehicle_ctrl.cpp:1060](main/vehicle_ctrl.cpp:1060), enrols only
  `ROLE_CHARGING_MANAGER`), so the car rejects the command server-side. The real (milder) gap
  is that the **command-list tables present door lock/unlock as ordinary working commands**
  with no note they're expected to be rejected for the enrolled role.
- **Confidence:** confirmed (with the framing correction above).
- **Fix:** Annotate the two command tables (e.g. "sent, but rejected by the car for the
  Charging-Manager role"); leave the security docs as-is (they're correct).

### [DOC-DRIFT] `get_charge_state` comment claims the active window opens on "car observed awake" — contradicts the critical sleep invariant
- **Where:** [main/vehicle_ctrl.cpp:830](main/vehicle_ctrl.cpp:830) (stale comment) vs the
  real gate [main/vehicle_ctrl.cpp:515](main/vehicle_ctrl.cpp:515) (`window = recent_cmd ||
  charging`) and the authoritative descriptions at
  [vehicle_ctrl.cpp:499](main/vehicle_ctrl.cpp:499) /
  [vehicle_ctrl.hpp:293](main/vehicle_ctrl.hpp:293).
- **What:** The comment says freshness holds "while the active window is open (a recent
  command, charging, or **the car observed awake**)". The code has **no awake term** —
  `sleep_state_` is never read in the window decision, and two other comments say the
  **opposite** ("we deliberately do NOT open the window merely because the car is awake…
  self-perpetuating").
- **Why it matters:** This is the single most safety-critical wake/sleep invariant. The stale
  comment documents exactly the broken behavior the code was written to avoid; a maintainer
  could "restore" an awake-based window to match the comment and silently reintroduce the
  never-sleeps / vampire-drain bug. **The code is correct; the comment is wrong and dangerous.**
- **Confidence:** confirmed. Comment-only fix, no code change.
- **Fix:** Drop "or the car observed awake" / "or the car waking" from the comment at
  [vehicle_ctrl.cpp:829-833](main/vehicle_ctrl.cpp:829); align with the correct comments.

### [DOC-DRIFT] `tele` telemetry is documented as "feed the web UI only" but the web UI never consumes it
- **Where:** [.claude/CLAUDE.md](.claude/CLAUDE.md) "Read-only telemetry" ("feed the web UI
  only", "the UI renders —") vs [main/www/index.html](main/www/index.html) `render()` (no
  `s.tele` access anywhere) — producer at
  [main/http_server.cpp:517](main/http_server.cpp:517).
- **What:** `/status` builds a full `tele:{climate,drive,tires,closures}` object, and CLAUDE.md
  says the rotating-poll caches "feed the web UI only" with "—" rendering in the UI. But the
  web UI (charge/SOC-only) **never references `s.tele`** (confirmed by grep + `git log -S`).
  The actual sole consumer is the MQTT/HA bridge ([mqtt_ha.cpp](main/mqtt_ha.cpp)).
- **Why it matters:** The doc names a UI consumer that doesn't exist; the climate/tires/
  closures/odometer data is only visible via Home Assistant, not the device's own page.
  (`docs/README.md:161` is fine — it only documents the `/status` field, makes no UI claim.)
- **Confidence:** confirmed.
- **Fix:** Reword CLAUDE.md "Read-only telemetry" to say the caches feed the **MQTT/HA bridge**
  (and are exposed under `tele` in `/status` for that + diagnostics), with the "—"/unknown
  rendering living in Home Assistant.

### [DOC-DRIFT] `GET /ota/status` field list in CLAUDE.md is stale (missing `update_available`, `current`)
- **Where:** [.claude/CLAUDE.md:81](.claude/CLAUDE.md:81) (`{state,progress,message,available}`)
  vs handler [main/http_server.cpp:392-402](main/http_server.cpp:392) (6 fields) — the UI
  branches on `o.update_available` and shows `o.current`
  ([index.html:621](main/www/index.html:621),[:623](main/www/index.html:623)).
  `docs/README.md:172-173` is already correct.
- **Why it matters:** The architecture doc disagrees with both code and user-facing docs on a
  live endpoint whose two missing fields drive the OTA update flow.
- **Confidence:** confirmed.
- **Fix:** Update CLAUDE.md:81 to `{state,progress,message,available,update_available,current}`.

### [DOC-DRIFT] `/status` schema in docs omits the asleep-card fields `last` and `last_seen_s`
- **Where:** [docs/README.md:154-161](docs/README.md:154) vs
  [main/http_server.cpp:578-589](main/http_server.cpp:578) (always emits top-level `last:{soc,
  status}` and `last_seen_s`) — consumed by the "Vehicle asleep" card
  ([index.html:429-432](main/www/index.html:429)).
- **Confidence:** confirmed.
- **Fix:** Add `last:{soc,status}` and `last_seen_s` to the `/status` field list in docs.

### [DOC-DRIFT] Build guide pins ESP-IDF v5.3.2 while CI builds (and ships) with v5.5.4
- **Where:** [docs/README.md:35](docs/README.md:35) (`git checkout v5.3.2`) vs
  [.github/workflows/build.yml:75](.github/workflows/build.yml:75) (`esp_idf_version:
  v5.5.4`). The `>=5.0.1` floor is consistent everywhere; only the pinned local version drifts.
- **Why it matters:** A contributor reproducing a CI build/warning on v5.3.2 may not match CI
  GCC behavior (the `-Wno-error=format` workaround in `CMakeLists.txt` is a toolchain
  artifact). The doc silently ages on every CI bump.
- **Confidence:** confirmed.
- **Fix:** Bump docs to v5.5.4, or note v5.3.2 is a known-good floor and CI uses v5.5.4.

### [DOC-DRIFT] `GET /api/proxy/1/version` returns an undocumented `platform` field
- **Where:** [docs/README.md:174](docs/README.md:174) / [.claude/CLAUDE.md:78](.claude/CLAUDE.md:78)
  vs [main/http_server.cpp:331-332](main/http_server.cpp:331) (`{version, platform:"ESP32-S3"}`).
- **Confidence:** confirmed (trivial impact). **Fix:** document `platform` or drop it.

### [NIT] `/diag?verbose=0` is undocumented
- **Where:** [docs/README.md:163](docs/README.md:163) / [.claude/CLAUDE.md:72](.claude/CLAUDE.md:72)
  vs [main/http_server.cpp:598](main/http_server.cpp:598). Handler honors `verbose=0` (the only
  runtime way to disable verbose RX logging short of reboot). **Fix:** document
  `/diag[?verbose=0|1][?clear=1]`.

### [NIT] Dead `diag_log_dump()` still ships the whole-buffer `std::string` allocator
- **Where:** [main/diag_log.cpp:57-71](main/diag_log.cpp:57) (zero callers; live path uses the
  streaming `diag_log_dump_chunks` at [http_server.cpp:608](main/http_server.cpp:608)).
- **Why it matters:** Latent footgun — it's the exact bad_alloc-on-fragmented-heap anti-pattern
  the streaming rewrite removed; a future caller reaching for the obvious-looking function
  reintroduces the crash. **Fix:** delete it (or add a "do not use over HTTP" warning).

### [NIT] Falling-edge log string lists "awake" as a window condition that isn't checked
- **Where:** [main/vehicle_ctrl.cpp:520](main/vehicle_ctrl.cpp:520) ("idle: no
  command/charging/awake…"). Same false mental model as the `get_charge_state` comment above.
  **Fix:** "idle: no command and not charging — dropping BLE link…".

### [NIT] `mqtt_user`/`mqtt_pass` NVS overrides are read but never written, and undocumented
- **Where:** [main/mqtt_ha.cpp:357-358](main/mqtt_ha.cpp:357). `/set_mqtt` only persists
  `mqtt_uri`; nothing writes user/pass; docs describe MQTT creds as compile-time only.
  **Fix:** add a writer + document, or drop the two `load_str` overrides.

### [NIT] CI firmware-change regex lists gitignored files that can never match
- **Where:** [.github/workflows/build.yml:44](.github/workflows/build.yml:44) includes
  `dependencies\.lock$` and `sdkconfig$`, both gitignored → dead alternatives (real triggers
  covered by `^main/` and `sdkconfig\.defaults$`). **Fix:** drop the two dead patterns.

### [NIT] CLAUDE.md NVS-namespace table omits stored keys
- **Where:** [.claude/CLAUDE.md](.claude/CLAUDE.md) NVS table. `tesla_cfg` also holds
  `last_time`, `mqtt_uri`/`mqtt_user`/`mqtt_pass`; `tesla_ble` also holds `key_created`,
  `paired_at`. **Fix:** add the keys, or mark the lists illustrative.

### Refuted (verified NOT a bug)
- **"BLE build/send paths in background tasks lack try/catch (unlike RX)"** — placement facts
  were right, but the **risk premise is wrong**: the build/send path encodes into a
  fixed-size buffer via nanopb/mbedTLS (pure C, return-code errors, no large contiguous
  allocation, no attacker-controlled length prefix), and the throw-prone **inbound** parsing
  is already guarded in `on_rx_data`/`loop()`. The guard asymmetry is intentional and
  correctly scoped.

---

## Live e2e test (evcc pod → ESP32 → car)

Read-only path run from inside the evcc pod (the real network path). **11 PASS / 1 FAIL.**

| Check | Result |
|---|---|
| `GET /status` — paired, BLE connected, fw **1.2.12** | ✅ PASS |
| `vehicle_data?endpoints=charge_state` ×15 (evcc's poll) | ✅ **0 timeouts, avg 0 ms / max 1 ms** |
| All 7 evcc-parsed fields present (`charging_state`, `battery_level`, `charge_limit_soc`, `charger_power`, `charge_rate`, `charge_amps`, `battery_range`) | ✅ PASS |
| `body_controller_state` live BLE read (LOCKED / AWAKE / NOT_PRESENT) | ✅ PASS |
| Commands (write path) | ⏭ skipped (read-only run; not opted in) |
| evcc logs, last 30 min | ❌ **5× `set_charging_amps` timeout, 12:34–12:36** |

**Device-side runtime observations (`/diag`):**
- **Heap healthy:** `free=102872 largest_block=55296 min_free=48876` — largest contiguous
  block ~54 KB, min-ever ~48 KB. No OOM pressure.
- **Not reboot-looping:** uptime climbed 1 429 s → 1 503 s across two reads (monotonic). A
  single boot occurred ~12:33 (cause rotated out of the 16 KB ring — undetermined; could be
  an OTA to 1.2.12 or a one-off).
- **Sleep-gating works:** `idle: no command/charging/awake — dropping BLE link so the car can
  sleep` fired **while the car was awake** → confirms the gate does NOT depend on observed-awake.
- **RX-corruption is now non-fatal:** the `Invalid message length … / buffer recovery failed
  / clearing all data` burst — the same symptom that crash-looped fw 1.1.12 — is recovered;
  the device keeps running and polls complete. **The RX hardening works.**

**[RISK] Write-path command latency under RX corruption (root cause of the FAIL)**
- Polls complete but **slowly** while the car is awake: Climate 6602 ms, Drive 5261 ms, Charge
  3320 ms, Tire 2922 ms — inflated by the frequent buffer-recovery. Add cold scan+connect
  (~3–5 s) after the idle link-drop and a write command (`set_charging_amps`) exceeds evcc's
  HTTP client timeout → the 12:34–12:36 burst.
- **Reads are unaffected** (served from `last_known_charge_` cache, 0–1 ms), which is why
  evcc's continuous poll loop never timed out.
- One `command failed: Connection lost` right after an idle link-drop — a command arriving as
  the link closes isn't always retried/reconnected gracefully.
- `Counter 1 is a replay / Duplicate response counter detected` always fires *after* a poll
  already completed successfully → it's filtering a duplicated BLE notification (correct
  anti-replay), **not** a failure cause.
- This is a **runtime robustness** issue, not a regression in the firmware contract; the
  errors were a bounded, non-recurring burst. Worth tracking: why is BLE RX so frequently
  corrupted while the car is awake (link quality `rssi=-83`, or a framing/MTU reassembly
  issue)?

---

## Coherence check

| Link | Status |
|---|---|
| HTTP endpoints: docs ↔ `handle_all` dispatch ↔ web UI | ✗ drift (`/ota/status`, `/status` `last`, `platform`, `verbose=0`) |
| Commands: docs ↔ `handle_command` switch ↔ impl | ✗ drift (door lock/unlock caveat) |
| Telemetry field: parser ↔ `/status` ↔ MQTT ↔ UI ↔ docs | ✗ drift ("feed the web UI only" is false; UI is charge-only) |
| evcc/HTTP response contract (`.response.response.charge_state.*`, `charge_amps`, fully populated, cached) | ✓ consistent (live-verified: all fields present, cache-served) |
| Config/build: `version.txt` ↔ partitions ↔ Kconfig ↔ sdkconfig ↔ lib pin ↔ CI | ✓ mostly (✗ IDF version doc, dead CI regex) |
| Version single-source (`version.txt`, no hardcoded versions) | ✓ consistent (device 1.2.12 = CI-stamped; worktree floor 1.2.0 — expected) |
| Partition/OTA layout (8 MB, `0x20000`, dual 3 MB, NVS preserved) | ✓ consistent (code/docs/CSV agree) |
| OTA rollback mark-valid after healthy boot | ✓ present |
| Wake/sleep invariants (VCSEC vs INFOTAINMENT, `NO_WAKE_SKIP`, active-window not awake-gated) | ✓ code correct (✗ one comment + one log string misdescribe it) |
| Heap invariants (handlers guarded, `/diag` streams, RX guarded) | ✓ consistent (NIT: dead non-streaming dumper still present) |
| NVS keys ≤15 chars, namespaces, empty-disables | ✓ consistent (NIT: table incomplete; `mqtt_user/pass` dead) |
| Pairing invalidation (3 events clear session+cache) | ✓ consistent |

## Prioritized actions

1. **Must-fix:** the `atof(NULL)` crash — guard all three numeric command params with
   `cJSON_IsString` (LAN-reachable crash → reboot loop → defeats sleep). Small, contained.
2. **Should-fix (correctness-adjacent docs):** the `get_charge_state` "observed awake" comment
   + the `idle … awake` log string (they document the broken behavior the code avoids); the
   `tele` "feed the web UI only" wording.
3. **Should-fix (doc drift):** CLAUDE.md `/ota/status` fields, `/status` `last`/`last_seen_s`,
   ESP-IDF version pin; annotate door lock/unlock command tables.
4. **Nice-to-have (nits):** delete dead `diag_log_dump()`; document `/diag?verbose=0`; resolve
   `mqtt_user/pass` (writer+doc or remove); prune dead CI regex patterns; complete the NVS table.
5. **Track (runtime):** investigate the BLE RX corruption that inflates write-command latency
   (link quality / framing); consider whether a longer evcc client timeout or a faster
   cold-connect path is warranted.
