---
name: project-review
description: Comprehensive, whole-project coherence review of the tesla-key-esp32 firmware — finds bugs, inconsistencies, doc/code drift, config/build mismatches, and cross-cutting incoherence so docs, code, config and the build all agree and the project is correct. Use whenever the user asks to "review the project", "project review", audit the repo, "check docs/code for errors/inconsistencies", check that everything is consistent/coherent, or after a batch of changes to make sure nothing drifted. Reach for this even when they don't say "review" — any "is the whole thing still right/coherent?" request fits.
---

# project-review — holistic coherence audit of tesla-key-esp32

This project is an **ESP-IDF 5.x C++ firmware** for the **ESP32 family** — one source tree
builds for esp32 / esp32s3 / esp32c3 / esp32c6 / esp32c5 (the four targets yoziru/tesla-ble
supports, plus esp32c5 via a local build-time patch of tesla-ble — `prepare-tesla-ble-c5.sh` +
the conditional `main/idf_component.yml`) — that acts as a **BLE↔HTTP proxy for a Tesla
vehicle**, API-compatible with TeslaBleHttpProxy
so it works as an **evcc** BLE vehicle. It is small but dense with **non-local invariants**:
a one-line change in code often has to be mirrored in three docs, a Kconfig option, the
partition table, and the web UI — and several rules only bite at *runtime* (BLE wake
semantics, heap fragmentation, OTA rollback) where a static read won't catch them.

The goal of a review here is not just "find bugs in this file" but **coherence**: does
the documentation describe the code that exists, does a feature appear everywhere it
should, do the config/build/version all agree, and do the runtime invariants still hold.

## How to run a review

Work in this order — it's what makes the review catch *drift* rather than just style:

1. **Build the intended model from the docs first.** Read [`.claude/CLAUDE.md`](../../../.claude/CLAUDE.md)
   (the always-needed essentials: API, command list, NVS table, partition offsets, link-state
   summary, invariants) **and** [`docs/ARCHITECTURE.md`](../../../docs/ARCHITECTURE.md) (the deep
   reference CLAUDE.md points to — telemetry fields, MQTT entities, full sleep/link-state +
   connection-failure semantics, pairing, OTA detail), [`README.md`](../../../README.md),
   [`docs/README.md`](../../../docs/README.md), [`docs/SECURITY.md`](../../../docs/SECURITY.md).
   CLAUDE.md and `docs/ARCHITECTURE.md` must agree with each other and with the code — drift
   between the slim summary and the deep reference is itself a finding.
   Note every concrete claim: endpoints, commands, NVS keys, partition offsets, flash
   size, version, defaults. These are your assertions to check.
2. **Read the code and find where reality diverges.** Walk the components below. For each
   doc claim, confirm the code matches. For each code feature, confirm the docs cover it.
   Drift goes **both ways** — an undocumented endpoint is as much a finding as a documented
   one that no longer exists.
3. **Check config & build agree** with both: `sdkconfig.defaults`, `partitions.csv`,
   `main/idf_component.yml` (library pin), `main/Kconfig.projbuild`, `version.txt`,
   `.github/workflows/build.yml`.
4. **Re-derive the runtime invariants** (wake/sleep, heap, OTA, concurrency) from the code
   — these are listed below and are the easiest things to silently break.
5. **Verify before you assert** (see *Verification discipline*). Separate confirmed bugs
   from hypotheses. Do not over-claim.
6. **Audit the review tooling against the project** (see *Reviewing the skills*). The skills
   (this one **and** every sibling under `.claude/skills/`) **and** the review subagents under
   `.claude/agents/` are part of what drifts — confirm each still maps the project that exists
   before you trust it, and report any gap as a `SKILL-DRIFT` finding (correcting it in the same
   pass).
7. **Write the report** in the structure at the end.
8. **Record the pass in the PR so the gates clear (no file marker).** A clean review (no
   *blocking* findings) must **tick the PR's `/project-review` checkbox and stamp it with the
   reviewed commit** — the `require-project-review.sh` PreToolUse hook refuses every PR merge
   (`gh pr merge` **and** `mcp__github__merge_pull_request`) until that box is ticked **and** its
   stamped sha still matches the PR head:

   ```
   - [x] `/project-review` clean — merge gate @ <short-sha>    # <short-sha> = git rev-parse --short=12 HEAD
   ```

   A full review also audits the skills (step 6), and `skill-audit ⊂ project-review`, so **also**
   tick the sibling `/skill-audit` box in the same body to clear `require-skill-audit.sh` (which
   blocks opening a PR and every push to it). Edit the PR body with
   `gh pr edit <pr> --body-file <file>` (or the GitHub MCP update tool in web/remote). Any later
   commit changes the sha and re-stales both boxes, forcing a fresh review before the next merge.
   Don't tick either if findings block the merge — fix first.

Use parallel reads/`Explore` to cover the tree quickly, but reason about the cross-cutting
links yourself — that's where the value is.

## Project map (what to read)

| Area | Files | Responsibility |
|---|---|---|
| Boot / wiring | `main/main.cpp` | WiFi, NVS, SNTP, mDNS, starts every component; boot heap log; OTA mark-valid |
| Target identity | `main/platform.hpp` | `TK_PLATFORM` string per `CONFIG_IDF_TARGET_*`; must agree with `/api/proxy/1/version`, the HA device model, and esp-web-tools `chipFamily` |
| BLE GATT client | `main/ble_client.{cpp,hpp}` | NimBLE central; scans Tesla UUID; RX notify → `on_rx_data` (runs on the **NimBLE host task**) |
| Vehicle control | `main/vehicle_ctrl.{cpp,hpp}` + `vehicle_commands.cpp` + `vehicle_telemetry.cpp` + `vehicle_pairing.cpp` (+ `vehicle_ctrl_internal.hpp`) | one `VehicleController`, split by concern: core wiring/`link_state()` glue; command API; **loop_task** (active-window polling + sleep gating) + caches; pairing lifecycle/keys |
| HTTP API | `main/http_server.{cpp,hpp}` + `http_api.cpp` + `http_status.cpp` + `http_ota.cpp` + `http_config.cpp` + `http_common.cpp` + `mcp_server.cpp` + `command_exec.cpp` (+ `http_handlers.hpp`) | `esp_http_server` on :80; single catch-all `handle_all` dispatch (wrapped in try/catch) in `http_server.cpp`; handlers split by route group; `mcp_server.cpp` serves `/mcp` (stateless JSON-RPC 2.0 MCP server for AI agents — guide in `docs/MCP.md`); both command surfaces resolve names/args via `logic/command_registry.hpp` and execute through `command_exec.cpp`; `/status` shaping decided in `logic/status_model.hpp` |
| HA bridge | `main/mqtt_ha.{cpp,hpp}` | read-only MQTT discovery publish; its own tasks |
| Storage | `main/nvs_storage.{cpp,hpp}` | NVS adapter; **maps library keys ≤15 chars** |
| Diag log | `main/diag_log.{cpp,hpp}` | in-RAM console ring (`GET /diag`); **static `.bss` buffer** (heap budget!) |
| OTA | `main/ota_update.{cpp,hpp}` | pull-based self-update; dual-slot |
| Provisioning | `main/provisioning.{cpp,hpp}` | captive setup portal when no WiFi |
| Web UI | `main/www/` (`index.html` markup + `style.css` + `app.js`, spliced by `inline_assets.cmake`) | compiled into the app binary as ONE self-contained page; polls `/status` every 4 s |
| On-device indicators | `main/display.{cpp,hpp}` + `main/display_font.h` (generated by `tools/display_sim.py`) + `main/led_status.{cpp,hpp}` | ST7735 status panel (T-Dongle-C5/S3, `CONFIG_TESLA_DISPLAY_ENABLED`) + underside APA102 status LED (`CONFIG_TESLA_LED_ENABLED`, default off). Both are **thin renderers** — cache-only (never wake the car), no MQTT; the "what to show" decisions live in the host-tested `logic/display_model.hpp` / `logic/led_status.hpp`, both reading the shared `logic/ui_state.hpp` snapshot + `logic/soc_gradient.hpp` ramp. No-op stubs on boards without the hardware |
| Pure logic (host-tested) | `main/logic/*.hpp` (`vin`, `units`, `link_state`, `target`, `mcp`, `command_registry`, `status_model`, `vehicle_data`, `command_result`, `ui_state`, `display_model`, `soc_gradient`, `led_status`) + `test/test_logic.cpp` | **IDF-free** logic the device and the host test share: VIN validation, imperial→metric units, the `link_state()` four-state machine + its `/status`/MQTT strings, per-target platform/OTA-suffix map, the ONE command registry both `/api` and `/mcp` dispatch from (names/kinds/shared arg bounds + `tools/list` row order) with the MCP JSON-RPC routing/version negotiation, the golden-pinned `/status` field contract (`status_model` over the `vehicle_data` result structs), the shared command result-string mapping, and the on-device display presenter + status-LED ladder (both off the shared `ui_state` snapshot + `soc_gradient` ramp). Host-built + run by `scripts/run-mock-tests.sh` (CI `logic-test` gate) — the real local verification loop; the same script also parity-checks `tools/display_sim.py` against the C++ display presenter (`scripts/check-display-sim-parity.sh`) |
| Library | `managed_components/yoziru__tesla-ble/` | **fetched, regenerated — NEVER edit** (pin in `main/idf_component.yml`) |

## Project invariants (the high-value checks)

These are the things that are easy to get subtly wrong and that a generic review misses.
Treat a violation of any of these as a real finding.

### BLE wake / sleep semantics
- **VCSEC**-domain commands hit the always-on body controller and **never wake the main
  computer (MCU)** nor honor wake policy. **INFOTAINMENT**-domain commands honor `WakePolicy`.
- `NO_WAKE_SKIP` skips **only when the device already knows the car is asleep**
  (`sleep_state_ == ASLEEP`). After a boot/disconnect `sleep_state_` resets to **UNKNOWN**,
  so a poll then **proceeds** and opens an infotainment session (which rouses the MCU). Only
  `WAKE_IF_NEEDED` sends an explicit wake (`RKE_ACTION_WAKE_VEHICLE`).
- **Active-window sleep gating** (`loop_task_fn_`): background infotainment polls run only
  while `window = recent command (last 5 min) OR charging`. `init()` seeds the window at
  boot. **Never** gate on "car observed awake" — that is self-perpetuating (our polling keeps
  the MCU awake → window never closes → the car can never sleep). A parked, idle car must be
  left to reach sleep. Anything that re-opens the window on a loop (e.g. a reboot loop) is a
  bug because it defeats this.

### Link state (single source of truth)
- `VehicleController::link_state()` is the **single source of truth**, shared by the web UI
  and the MQTT bridge so the two can never disagree. **Four states:** **AWAKE** (fresh live
  infotainment telemetry < 60 s), **ASLEEP** (no live data **and** the car's VCSEC sleep flag
  has held ASLEEP for ≥ ~120 s — *debounced*, sampled in `loop_task`, so a Cabin-Overheat
  `AWAKE↔ASLEEP` flap (~60 s) can't trip it), **IDLE** (reachable over BLE but **not provably
  asleep** — we stopped polling infotainment to let it sleep and VCSEC hasn't confirmed →
  web UI shows the neutral **"Parked"** card, which makes **no** sleep claim), **UNREACHABLE**
  (answers nothing over BLE). Nothing heard since boot/re-pair ⇒ MQTT sleep_state **omitted**
  (HA shows "unknown"); the web UI shows a grey **"Unreachable"** hero (or **"Connecting…"** for
  the unknown cold-start state) with the orange ping-pong BLE bars — not hidden, never a sleep claim.
- **Asymmetry — do not break it:** trust the *debounced ASLEEP* VCSEC flag as proof of sleep,
  but **never** trust VCSEC `AWAKE` to claim AWAKE. A parked car reports VCSEC `AWAKE` while
  its infotainment sleeps (the old `wake_up()` trap); AWAKE always requires live infotainment
  telemetry, so a wrong VCSEC `AWAKE` can only land in IDLE, never falsely AWAKE. The
  momentary BLE "Disconnected" row is normal (link dropped between polls by design) and must
  **not** drive the hero — only `link` does.

### Memory / heap (this device is RAM-constrained)
- The binding constraint is the **largest *contiguous* free block**, not total free heap.
  Steady-state it is only tens of KB (NimBLE + WiFi + MQTT dominate; see the boot
  heap-attribution log in `main.cpp`). Any single allocation larger than that throws.
- **C++ exceptions are enabled** (`CONFIG_COMPILER_CXX_EXCEPTIONS=y`), but an **uncaught**
  throw that unwinds into C frames (NimBLE host task, the C httpd loop) → `std::terminate` →
  `abort()` → reboot. So: HTTP handlers run under the `handle_all` try/catch (→ 503 on OOM);
  library calls that parse BLE RX (`on_rx_data`, `loop()`) are wrapped; **flag any new large
  allocation** (`std::string` of a whole buffer, TLS for OTA, big JSON) that isn't guarded or
  could exceed the largest block. `/diag` must **stream** (`httpd_resp_send_chunk`), never
  build the whole log into one `std::string`.
- Static buffers (e.g. `diag_log`'s ring) come straight off the heap budget — sizing them up
  shrinks the largest free block.

### NVS / config
- Namespaces: `tesla_cfg` (runtime cfg) and `tesla_ble` (key + sessions). NVS keys are
  **≤15 chars** — the storage adapter maps longer library keys; a new key over 15 chars is a
  bug. An **empty** config value disables the feature it gates (e.g. `mqtt_uri`).

### OTA / versioning
- Dual-OTA layout (`partitions.csv`): `nvs@0x9000`, app at **`0x20000`**, two ~2 MB slots
  (`0x1f0000`), **4 MB** flash (smallest supported part; a larger flash leaves the top
  unused). NVS is never in the flashed set, so pairing/key/VIN survive OTA. One source tree
  builds for esp32 / esp32s3 / esp32c3 / esp32c6 (the tesla-ble targets) + esp32c5 (local
  build-time patch); each device pulls its own `tesla-key-esp32<suffix>.bin`
  (`""`/`-s3`/`-c3`/`-c6`/`-c5`, so "esp32" appears once —
  must match across `ota_update.cpp`, `ci-build-all.sh`, `build-pages.sh`) and the web
  installer auto-selects by chipFamily.
- Rollback is enabled and **deliberately deferred**: `main.cpp` does NOT mark the image valid
  at startup — `ota_health_gate_task` calls `esp_ota_mark_app_valid_cancel_rollback()` only
  after the new image has run healthily for `kOtaHealthGateS` ≈ 90 s. A crash inside that
  window reboots still-`PENDING_VERIFY` and the bootloader reverts — that rollback IS the
  designed safety net (re-introducing a mark-valid at startup would be the regression to flag).
- **OTA images are signed** (Secure Boot v2 RSA-3072 scheme *without* hardware Secure Boot, no
  eFuses): `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` + `..._RSA_SCHEME` +
  `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` in `sdkconfig.defaults`; the build stays
  unsigned (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`) and `scripts/ci-build-all.sh` signs
  each image with the `OTA_SIGNING_KEY` secret on a main push / manual `workflow_dispatch` on
  main **and on a same-repo `pull_request`** — the latter so a PR publishes a signed, boot-able
  per-PR preview installer (`gh-pages` `PR/<N>/`, version `<latest-tag>-PR-<N>`; OTA URL
  unchanged → device still updates from main). Fork PRs get no secret → build unsigned → no
  preview. Trust is TOFU from the running app's signature block. Classic esp32 needs
  `CONFIG_ESP32_REV_MIN_3` (`sdkconfig.defaults.esp32`).
- **Downgrade gate (software anti-rollback):** before the bulk download, `ota_task` reads the
  downloaded image's own app-descriptor version (`esp_https_ota_get_img_desc`) and refuses
  anything not strictly newer than the running firmware — a signature proves authenticity, not
  freshness. Weakening it re-opens the old-but-validly-signed-image attack.
- `version.txt` is the committed **version floor**; CI (`scripts/next-version.sh`, see
  `.github/workflows/build.yml`) auto-computes the actual release version and **stamps it into
  the build**, so the reported version, firmware filename, and OTA manifest agree for a release.
  Locally `version.txt` → `PROJECT_VER` directly. Either way a **hardcoded version anywhere
  else is drift**. Old single-`factory` devices need a one-time USB reflash (migration).

### evcc / HTTP contract
- Response shape must match TeslaBleHttpProxy: `.response.response.charge_state.*`, the field
  is **`charge_amps`** (not `charging_amps`), and `charge_state` is **always fully populated**
  (a missing numeric field makes evcc parse `<nil>` and fail). `vehicle_data` is served from
  **cache** and never blocks (avoids gateway 502s).
- **No HTTP auth / TLS by design** (evcc can't send credentials) — trusted LAN only; document
  any deviation in `docs/SECURITY.md`.

### Pairing
- Keys are enrolled **Charging Manager only**; owner role is intentionally removed
  (`?role=owner` → 403). The on-screen "Add key" dialog on the car appears **only while a
  Tesla NFC keycard is on the console reader**. Three events invalidate a pairing and must
  clear session + cache: key deleted on the car (`whitelist`), `gen_keys?force=1`, VIN change.

### Telemetry / MQTT
- proto3-optional fields are emitted **only when the car reported them** (presence flags) so
  the UI/HA show "—"/unknown, never a phantom 0. The MQTT bridge is **read-only** (no command
  topics subscribed — the car is never controlled or woken from HA).
- `sleep_state` (MQTT) and the web-UI hero **both** derive from `link_state()` — see *Link
  state* above. The MQTT switch over the four values must stay **exhaustive** (no catch-all
  `else` defaulting to "asleep") and the web UI must handle every state, including the omitted
  "unknown" — the historic bug was `unknown` falling through to a false "asleep".

## Cross-cutting consistency (add X → also update Y)

The most common inconsistency is a feature that exists in code but not in all the places
that describe it. When reviewing a change (or the repo as a whole), check these links:

- **New/changed HTTP endpoint** → `handle_all` dispatch **and** the API list in
  `.claude/CLAUDE.md` **and** `docs/README.md` **and** the web UI if user-facing.
- **New command** → a `kCommands` registry row (`main/logic/command_registry.hpp`) **and**
  the kind dispatch in `main/command_exec.cpp` **and** the command list in `.claude/CLAUDE.md`
  **and** docs. (Note: vehicle-control *buttons* were deliberately removed from the web UI.)
- **New MCP tool** → the same `kCommands` row gains `mcp_name`/`mcp_desc` (the ONE source
  the advertised schema, the MCP executor validation AND the REST clamp are generated
  from; `tools/list` order = table order) **and** a `CHECK` in `test/test_logic.cpp`
  (`test_mcp`) **and** the tool table in `docs/MCP.md` **and** the `/mcp` entry in
  `.claude/CLAUDE.md`. (Tools mirror the run-on-key charging command set + read-only
  `get_vehicle_state` — never expose a role-refused command: `mcp_name == nullptr`.)
- **New/changed `/status` field** → `logic/status_model.hpp` **and** its golden emissions
  in `test/test_logic.cpp` (`test_status_model`) **and** the web UI consumer (`www/app.js`).
- **New NVS key** → ≤15 chars **and** the namespace table in `.claude/CLAUDE.md`.
- **New Kconfig option** → `main/Kconfig.projbuild` **and** any doc that references defaults
  **and** `sdkconfig.defaults` if a non-default value is required.
- **Partition / offset / flash-size change** → `partitions.csv` **and** every doc that states
  the offset (`0x20000`), flash size (`4 MB`), slot size (`~2 MB` / `0x1f0000`), or dual-OTA
  layout **and** the migration note.
- **Version change** → `version.txt` only; hunt for any other hardcoded version.
- **New telemetry field** → parser (with presence flag) **and** `/status` JSON **and** MQTT
  discovery **and** the web UI **and** docs (the field list in `docs/ARCHITECTURE.md`).
- **Sleep / link-state change** → `link_state()` is the single source of truth feeding **both**
  the web-UI hero (`main/www/app.js`) **and** MQTT `sleep_state` (`mqtt_ha.cpp`). Touch one
  sink → keep the other in sync (exhaustive MQTT switch, every web-UI state incl. unknown)
  **and** update the four-state summary in `.claude/CLAUDE.md` **and** the full semantics in
  `docs/ARCHITECTURE.md`.
- **New chip / target** → for a chip tesla-ble already lists, `main/idf_component.yml` git dep;
  for one it omits (like esp32c5), the conditional `main/idf_component.yml` + a local patched
  checkout (`prepare-tesla-ble-c5.sh` pattern) **and** `logic/target.hpp` (enum + `platform_name`
  + `image_suffix`) **and** `platform.hpp` (`TK_TARGET`) **and** the OTA `<suffix>` map
  (`ota_update.cpp`) **and** `ci-build-all.sh` + `build-pages.sh` (`TARGETS`/`image_suffix`/
  `chip_family`) **and** `test/test_logic.cpp` CHECKs **and** every doc that lists the supported
  targets. (Watch the `ci-build-all.sh` app-size gate — esp32c5 already binds it.)
- **WiFi/LAN reconnect or watchdog change** → the STA→LAN reconnect policy + connectivity-
  watchdog constants live ONLY in `main/main.cpp` (`MAX_RETRY`, `s_wifi_ever_connected`,
  `kWdPeriodS`/`kWdFailToReassoc`/`kWdPingCount`, the ghost-only + `s_gw_ever_reachable`
  guards) **and** are mirrored in the **"WiFi / LAN connectivity"** section of
  `docs/ARCHITECTURE.md` (which quotes those numbers) **and** the deep-reference topic index in
  `.claude/CLAUDE.md`. This is the STA→LAN link, **distinct** from the car-BLE `link_state()`.
  Invariant: the watchdog must **never reboot** (a reboot mid-outage hits `wifi_connect()`'s
  boot timeout → setup portal, abandoning good credentials).
- **MQTT transport / TLS-default change** → the scheme-defaulting rule lives in
  `mqtt_ha.cpp` (`mqtt_ha_start`: schemeless broker ⇒ `mqtt://`, but ⇒ `mqtts://` when
  credentials are present, CA-bundle-verified, **no plaintext fallback**) **and** surfaces in
  `/status` (`mqtt.tls`/`mqtt.error`, `http_status.cpp`) **and** the web UI's "· secured" MQTT
  row **and** the MQTT sections of `.claude/CLAUDE.md` + `docs/ARCHITECTURE.md`.
- **tesla-ble library bump** → `main/idf_component.yml` pin; never patch
  `managed_components/`.

## Reviewing the skills (meta-coherence)

The skills under `.claude/skills/` **and the review subagents under `.claude/agents/`** are
themselves documents that drift — each lags the code by exactly the changes landed since it was
last touched. A review is **not complete** until you have checked that **every** skill and
**every** agent still describes the project that exists; otherwise future runs inherit a stale
map. Treat a gap as a real finding (`SKILL-DRIFT`), reported alongside the code/doc ones, and
propose the specific edits in the same report (correct the map in the pass that found the drift).

The agents matter here for a second reason: two of them **duplicate content this skill owns**,
so they are cross-cutting sinks like any other. `doc-drift-checker` restates this skill's
*cross-cutting "add X → also update Y" list* and `heap-safety-reviewer` restates the *heap /
contiguous-block invariant* — touch either here and the matching agent must move too, or the
project ends up with two review maps that disagree. That drift is exactly what a coherence
review exists to catch, so hold the agents to it.

### Termination — the audit converges, it does not loop

This self-audit cannot become an endless review → edit → review cycle, by design. Hold to all
four rules — together they guarantee a fixpoint:

- **A review is a single pass.** It is a one-shot instruction set — read → check → report (→
  optionally apply the proposed edits) → **stop**. Nothing here re-invokes the review, and no
  hook watches `SKILL.md` to re-trigger one. Editing a skill never starts another run; only the
  user does. So there is no mechanical loop to break.
- **Drift is measured against the *code*, never against the skill's own wording.** A
  `SKILL-DRIFT` finding exists only where a skill **contradicts** a fact in the code / config /
  script (a wrong number, a removed feature, a renamed path). The fix *removes* that
  contradiction without creating a new one, so a re-run converges: skill == code → zero
  findings → zero edits. The stable state is "skill matches code", and every legitimate edit
  moves toward it.
- **Only correct contradictions — never reword, restyle, or "improve".** Prose preferences have
  no ground truth, so polishing them never converges. The test for a legitimate edit: *name the
  code fact it now matches.* If you can't — or if the edit wouldn't survive the next review
  unchanged — it is churn; don't make it.
- **Don't re-audit within a pass.** Once you've corrected a skill in this run, you're done with
  it — you do not re-open it to check your own edit. The next *user-initiated* review verifies
  it against the code, like any other change.

### This skill (`project-review`)

Run these checks against the current tree:

- **Project map covers every source file.** `ls main/*.{cpp,hpp} main/logic/*.hpp` and confirm
  each lands in the *Project map* table (or is deliberately out of scope). A `main/*.cpp` (or a
  `main/logic/*.hpp`) the map never mentions is the signal that a whole subsystem appeared
  without the skill noticing.
- **Invariants match the code's *current* model**, not a superseded one. For each subsystem
  with an invariant (wake/sleep, **link state**, heap, OTA, NVS, evcc, pairing, telemetry),
  re-read the code and confirm the invariant still states what the code does. Sleep/link state
  is the historically fast-moving one — re-derive it every time.
- **Cross-cutting list is complete.** Every "add X → also update Y" link should map a real
  multi-place feature; a feature that spans code + docs + config + UI but isn't listed is
  exactly the drift a coherence review is meant to catch.
- **API / command lists are current.** Diff the routes in `http_server.cpp` (dispatch; handlers in `http_api/status/ota/config.cpp` + `mcp_server.cpp`) and the command
  switch against the references the skill and `.claude/CLAUDE.md` lean on.
- **No stale specifics in the skill text** — hardcoded chip names (e.g. a lone "ESP32-S3" where
  it is now multi-target), file paths, partition offsets, sizes, or version assumptions.
- **Recency cross-check.** `git log --oneline -10 -- main/` vs. the skill's last change
  (`git log -1 -- .claude/skills/project-review/SKILL.md`): every commit in between is a
  candidate for an invariant or cross-cutting link the skill hasn't absorbed yet.

### The other skills (audit and correct each)

The same drift hits the sibling skills. **Discover them, don't hardcode the list:**
`ls .claude/skills/*/SKILL.md` (skills) **and** `ls .claude/agents/*.md` (subagents). For each,
the test is the same — does its `description` + steps + concrete numbers (offsets, counts, flags,
paths, target set) still match the script, code, and config it drives? Correct a stale one in
place (same kind of fix as any doc) and report it as `SKILL-DRIFT`. The current siblings and
what each must stay true to:

- **`flash-esp32`** wraps the build + USB-flash path. Re-verify against `scripts/idf-docker.sh`
  (Docker-pinned, no local IDF), `partitions.csv` (offsets `nvs@0x9000`, `otadata@0xf000`,
  app `@0x20000`), the target set (esp32/s3/c3/c6/c5) and per-target bootloader offset, and that
  `@flash_args` preserves NVS (never writes `nvs@0x9000`).
- **`ship`** takes a merged PR to the board: squash-merge → `gh run watch` on the post-merge
  `build` run → download the signed artifact (`tesla-key-esp32-<version>`; per-target
  `tesla-key-esp32<sfx>.bin`, never `*-merged.bin`) → USB app-slot flash (`0x20000` + otadata
  erase `0xf000/0x2000`, NVS preserved) or device OTA → verify via `/status` +
  `/api/proxy/1/version`. Re-verify against `.github/workflows/build.yml` (artifact naming, the
  firmware-change-gated release), `scripts/ci-build-all.sh` (suffix map, `sign_image`, merged
  copies), `partitions.csv` offsets, and the `/ota/*` endpoints. Complementary to `flash-esp32`
  (local-tree build+flash, no merge); it defers the merge gate to `require-project-review.sh`.
- **`e2e-evcc`** wraps `scripts/e2e_evcc.sh`. Re-verify the command count (must equal the
  `handle_command` switch — currently **15**), the version-coherence claim (`/status` = `X`,
  `/api/proxy/1/version` = `X-esp32` via `fw_version()`), the `vehicle_data` fields it asserts,
  the out-of-scope endpoint list, and the env-var gates (`RUN_COMMANDS` / `ALLOW_CHARGE_TOGGLE`
  / `RUN_ALL_COMMANDS`).
- **`vehicle-command-audit`** compares the firmware against upstream `teslamotors/vehicle-command`,
  gated by what `yoziru/tesla-ble` (pin in `main/idf_component.yml`) can actually do. Re-verify the
  tesla-ble **pin** in its source map (`v5.1.1`) still matches `idf_component.yml`, that its upstream
  file paths still resolve (e.g. `pkg/vehicle/charge.go`), and that its "worked findings" table is
  not asserting drift already fixed in the tree. It is the *upstream-conformance* counterpart to this
  skill — keep the two complementary, not overlapping.
- **`add-logic-test`** scaffolds a new pure-logic unit in `main/logic/` + its `CHECK` cases in
  `test/test_logic.cpp`. Re-verify its claims against `scripts/run-mock-tests.sh`, the CI
  `logic-test` job (`.github/workflows/build.yml`), the `run-logic-tests.sh` **Stop hook**
  (`.claude/settings.json`), the `CHECK`/`CHECK_STR`/`CHECK_NEAR` macro set in
  `test/test_logic.cpp`, and the `static_assert` lock pattern (`main/ota_update.cpp` /
  `main/logic/target.hpp`).
- **`skill-audit`** is the dedicated, PR-gated skill that runs *this very audit* (every skill +
  agent vs. the project) on its own, gated by `require-skill-audit.sh` (blocks opening a PR and
  every push to it, not the merge). It is the **authority for
  the per-sibling drift checklist** — this section and `skill-audit`'s *Per-target checklist*
  describe the same siblings and must agree; a divergence between them is itself `SKILL-DRIFT`.
  `skill-audit ⊂ project-review`: running a full review here covers its scope (tick both PR
  checkboxes, step 8). Re-verify its own numbers (hook `require-skill-audit.sh`, the PR-checkbox
  gate mechanism — no file marker, command count `15`, tesla-ble pin) and that the sibling/agent
  list it carries still matches the tree.
The review subagents under `.claude/agents/` — audit these the same way (they are the targeted
lenses this skill delegates to; keep them complementary, not contradictory):

- **`doc-drift-checker`** is the fast targeted-diff lens for the *cross-cutting* links. Its
  "add X → also update Y" enumeration must stay a subset of — and agree with — the *Cross-cutting
  consistency* section above; a link added here that it lacks (or a stale one it still lists) is
  `SKILL-DRIFT`.
- **`heap-safety-reviewer`** is the allocation/throw lens. Re-verify its numbers and rules
  against the *Memory / heap* invariant above (largest **contiguous** block is the binding limit,
  `handle_all` try/catch → 503, streamed `/diag`) and against `main.cpp`'s heap-attribution log —
  the two heap maps must not diverge.
- **`claude-code-optimizer`** audits the `.claude/` setup (CLAUDE.md, `settings.json`
  permissions + hooks, skills, subagents), **not** firmware logic. Confirm its boundary still
  points firmware-correctness work back at this skill, and that the hook/skill/agent inventory it
  reasons over matches what actually lives under `.claude/` (`ls .claude/hooks/ .claude/agents/`).
- **`multi-target-build-reviewer`** is the per-target build/config divergence lens (the five
  targets built from one tree). Re-verify its facts against the *Cross-cutting consistency*
  section and the build wiring: the target set (esp32/s3/c3/c6/c5), per-target bootloader offsets
  (`0x1000`/`0x2000`/`0x0`), the image-suffix map (`ci-build-all.sh`/`build-pages.sh`/
  `ota_update.cpp` `TESLA_OTA_IMG_SUFFIX`), the app-size gate (`slot − 32 KB`), and the esp32c5
  local tesla-ble patch routing (`prepare-tesla-ble-c5.sh` + `main/idf_component.yml`). Keep it
  complementary to this skill, not a firmware-logic reviewer.
- **Any skill or agent added since this was written** must be audited too — and added to this list.

A skill or agent that drives a script is only as current as the script: when the script changes,
re-read the doc that documents it. The hooks under `.claude/hooks/` (`require-project-review.sh`
merge gate **and** `require-skill-audit.sh` PR-create/push gate, `run-logic-tests.sh` Stop hook,
`report-capabilities.sh`/`build-efficiency-check.sh` SessionStart, `clang-format-edit.sh`
PostToolUse) are wired in `.claude/settings.json`; a hook whose behaviour a skill/agent describes
must match the script that actually runs.

## Verification discipline (avoid confident-but-wrong findings)

Firmware bugs are easy to mis-diagnose. Hold findings to evidence:

- Separate **confirmed** ("I reproduced it / the code path provably does this") from
  **suspected** ("looks wrong, needs checking"). Label them differently in the report.
- For **runtime** claims (crash, wake, race, OOM, leak) cite the distinguishing signal, don't
  guess: a reset is `PANIC` vs `BROWNOUT` vs `*_WDT` (`esp_reset_reason`); a crash backtrace
  only means something decoded against the **matching** `ELF file SHA256` (flash your own
  build to guarantee a match — OTA/CI builds aren't locally reproducible); OOM shows up as a
  shrinking **largest free block**, not total free; a leak is a monotonic decline, fragmentation
  is a stable-but-low largest block.
- Don't propose editing `managed_components/`. A real library bug is fixed in our wrapper
  (catch/guard at the call boundary) or by bumping the pin.
- A `git`/build check beats a guess: `idf.py build` for compile/warnings, the host **mock
  tests** (`scripts/run-mock-tests.sh` — seconds, no Docker/board) to actually *run* a
  pure-logic change (VIN/units/`link_state`/target — the same `main/logic/` headers the
  firmware uses; CI gates the build on the `logic-test` job), `grep` for the other half of a
  cross-cutting link.

## Report structure

Produce a single report in this shape:

```
# Project review — tesla-key-esp32 (<date>)

## Summary
<2–4 sentences: overall coherence, how many findings by severity, headline risks.>

## Findings
For each, in priority order:
### [SEV] <short title>   (SEV = BUG | INCONSISTENCY | DOC-DRIFT | SKILL-DRIFT | RISK | NIT)
- **Where:** `path:line` (and the other side of the link, if cross-cutting)
- **What:** what is wrong / what disagrees with what
- **Why it matters:** concrete consequence
- **Confidence:** confirmed | suspected (+ how to verify if suspected)
- **Fix:** the specific change(s), in every place that must move together

## Coherence check
<Doc↔code, config↔code, version, each cross-cutting link, and skills↔project (does every
SKILL.md under .claude/skills/ still cover the tree?): ✓ consistent / ✗ drifted.>

## Prioritized actions
1. <must-fix> … 2. <should-fix> … 3. <nice-to-have> …
```

Order findings by impact (a wake/heap/OTA correctness bug outranks a doc nit). Keep each
finding tight and actionable — the point is that someone can fix the whole project from this
report without re-deriving the context.
