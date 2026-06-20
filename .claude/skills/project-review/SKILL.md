---
name: project-review
description: Comprehensive, whole-project coherence review of the tesla-key-esp32 firmware — finds bugs, inconsistencies, doc/code drift, config/build mismatches, and cross-cutting incoherence so docs, code, config and the build all agree and the project is correct. Use whenever the user asks to "review the project", "project review", audit the repo, "check docs/code for errors/inconsistencies", check that everything is consistent/coherent, or after a batch of changes to make sure nothing drifted. Reach for this even when they don't say "review" — any "is the whole thing still right/coherent?" request fits.
---

# project-review — holistic coherence audit of tesla-key-esp32

This project is an **ESP-IDF 5.x C++ firmware** for the ESP32-S3 that acts as a
**BLE↔HTTP proxy for a Tesla vehicle**, API-compatible with TeslaBleHttpProxy so it
works as an **evcc** BLE vehicle. It is small but dense with **non-local invariants**:
a one-line change in code often has to be mirrored in three docs, a Kconfig option, the
partition table, and the web UI — and several rules only bite at *runtime* (BLE wake
semantics, heap fragmentation, OTA rollback) where a static read won't catch them.

The goal of a review here is not just "find bugs in this file" but **coherence**: does
the documentation describe the code that exists, does a feature appear everywhere it
should, do the config/build/version all agree, and do the runtime invariants still hold.

## How to run a review

Work in this order — it's what makes the review catch *drift* rather than just style:

1. **Build the intended model from the docs first.** Read [`.claude/CLAUDE.md`](../../../.claude/CLAUDE.md)
   (the architecture + API + invariants overview), [`README.md`](../../../README.md),
   [`docs/README.md`](../../../docs/README.md), [`docs/SECURITY.md`](../../../docs/SECURITY.md).
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
6. **Write the report** in the structure at the end.

Use parallel reads/`Explore` to cover the tree quickly, but reason about the cross-cutting
links yourself — that's where the value is.

## Project map (what to read)

| Area | Files | Responsibility |
|---|---|---|
| Boot / wiring | `main/main.cpp` | WiFi, NVS, SNTP, mDNS, starts every component; boot heap log; OTA mark-valid |
| BLE GATT client | `main/ble_client.{cpp,hpp}` | NimBLE central; scans Tesla UUID; RX notify → `on_rx_data` (runs on the **NimBLE host task**) |
| Vehicle control | `main/vehicle_ctrl.{cpp,hpp}` | `TeslaBLE::Vehicle` wrapper; command API; **loop_task** (active-window polling + sleep gating); caches |
| HTTP API | `main/http_server.{cpp,hpp}` | `esp_http_server` on :80; single catch-all `handle_all` dispatch (wrapped in try/catch) |
| HA bridge | `main/mqtt_ha.{cpp,hpp}` | read-only MQTT discovery publish; its own tasks |
| Storage | `main/nvs_storage.{cpp,hpp}` | NVS adapter; **maps library keys ≤15 chars** |
| Diag log | `main/diag_log.{cpp,hpp}` | in-RAM console ring (`GET /diag`); **static `.bss` buffer** (heap budget!) |
| OTA | `main/ota_update.{cpp,hpp}` | pull-based self-update; dual-slot |
| Provisioning | `main/provisioning.{cpp,hpp}` | captive setup portal when no WiFi |
| Web UI | `main/www/index.html` | compiled into the app binary; polls `/status` every 4 s |
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
- Dual-OTA layout (`partitions.csv`): `nvs@0x9000`, app at **`0x20000`**, two 3 MB slots,
  **8 MB** flash. NVS is never in the flashed set, so pairing/key/VIN survive OTA.
- Rollback is enabled — `main.cpp` must call `esp_ota_mark_app_valid_cancel_rollback()` after
  a healthy boot, **else a crash within the verify window rolls back to the old slot.**
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

## Cross-cutting consistency (add X → also update Y)

The most common inconsistency is a feature that exists in code but not in all the places
that describe it. When reviewing a change (or the repo as a whole), check these links:

- **New/changed HTTP endpoint** → `handle_all` dispatch **and** the API list in
  `.claude/CLAUDE.md` **and** `docs/README.md` **and** the web UI if user-facing.
- **New command** → `handle_command` switch **and** the command list in `.claude/CLAUDE.md`
  **and** docs. (Note: vehicle-control *buttons* were deliberately removed from the web UI.)
- **New NVS key** → ≤15 chars **and** the namespace table in `.claude/CLAUDE.md`.
- **New Kconfig option** → `main/Kconfig.projbuild` **and** any doc that references defaults
  **and** `sdkconfig.defaults` if a non-default value is required.
- **Partition / offset / flash-size change** → `partitions.csv` **and** every doc that states
  the offset (`0x20000`), size (`8 MB`), or dual-OTA layout **and** the migration note.
- **Version change** → `version.txt` only; hunt for any other hardcoded version.
- **New telemetry field** → parser (with presence flag) **and** `/status` JSON **and** MQTT
  discovery **and** the web UI **and** docs.
- **tesla-ble library bump** → `main/idf_component.yml` pin; never patch
  `managed_components/`.

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
- A `git`/build check beats a guess: `idf.py build` for compile/warnings, `grep` for the
  other half of a cross-cutting link.

## Report structure

Produce a single report in this shape:

```
# Project review — tesla-key-esp32 (<date>)

## Summary
<2–4 sentences: overall coherence, how many findings by severity, headline risks.>

## Findings
For each, in priority order:
### [SEV] <short title>   (SEV = BUG | INCONSISTENCY | DOC-DRIFT | RISK | NIT)
- **Where:** `path:line` (and the other side of the link, if cross-cutting)
- **What:** what is wrong / what disagrees with what
- **Why it matters:** concrete consequence
- **Confidence:** confirmed | suspected (+ how to verify if suspected)
- **Fix:** the specific change(s), in every place that must move together

## Coherence check
<Doc↔code, config↔code, version, and each cross-cutting link: ✓ consistent / ✗ drifted.>

## Prioritized actions
1. <must-fix> … 2. <should-fix> … 3. <nice-to-have> …
```

Order findings by impact (a wake/heap/OTA correctness bug outranks a doc nit). Keep each
finding tight and actionable — the point is that someone can fix the whole project from this
report without re-deriving the context.
