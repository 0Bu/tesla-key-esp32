---
name: project-review
description: "Read-only whole-project coherence review of tesla-key-esp32 for bugs, security risks, doc/code drift, config/build mismatches, and cross-cutting inconsistencies. A review request authorizes inspection and reporting only: do not edit files, stamp or edit PRs, commit, push, merge, release, flash, OTA, contact a live vehicle/device, or fix findings unless the user separately authorizes implementation."
---

> **Canonical runner-neutral skill.** Read [`AGENTS.md`](../../../AGENTS.md) before acting.
> Project skills are canonical under [`.agents/skills/`](../), and lifecycle/PR policy is
> enforced by the runner-neutral core under [`tools/agent-hooks/`](../../../tools/agent-hooks/).
> This skill does not grant permissions beyond the user's explicit request.
> Invoke this workflow canonically as `$project-review`.

# project-review — holistic coherence audit of tesla-key-esp32

This project is an **ESP-IDF 5.x C++ firmware** for the **ESP32 family** — one source tree
builds for esp32 / esp32s3 / esp32c3 / esp32c6 — exactly the four targets yoziru/tesla-ble
supports, which the ESP-IDF Component Manager enforces at dependency resolution. All four receive
the complete ordered repository patch series in `patches/tesla-ble/` via root CMake: replay
rejection, transactional key regeneration/persistence, and bounded RX-framing recovery logs. The
firmware acts as a **BLE↔HTTP proxy for a Tesla
vehicle**, API-compatible with TeslaBleHttpProxy,
so it works as an **evcc** BLE vehicle. It is small but dense with **non-local invariants**:
a one-line change in code often has to be mirrored in three docs, a Kconfig option, the
partition table, and the web UI — and several rules only bite at *runtime* (BLE wake
semantics, heap fragmentation, OTA rollback) where a static read won't catch them.

The goal of a review here is not just "find bugs in this file" but **coherence**: does
the documentation describe the code that exists, does a feature appear everywhere it
should, do the config/build/version all agree, and do the runtime invariants still hold.

> **Read-only boundary.** Review, audit, triage, and gate-readiness requests authorize inspection
> and a findings report only. Do not edit tracked files, PR bodies or checkboxes; do not commit,
> push, merge, release, sign, flash, OTA, or contact a live device/vehicle. If the user later asks
> for fixes, treat that as a separate implementation scope and preserve all publication/hardware
> boundaries in [`AGENTS.md`](../../../AGENTS.md).

## How to run a review

Work in this order — it's what makes the review catch *drift* rather than just style:

1. **Build the intended model from the docs first.**
   [`AGENTS.md`](../../../AGENTS.md) owns runner policy, authorization, safety, evidence, build, and review contracts.
   [`docs/README.md`](../../../docs/README.md) owns hardware, HTTP API, and commands.
   [`docs/ARCHITECTURE.md`](../../../docs/ARCHITECTURE.md) owns telemetry, MQTT, sleep/link state, pairing, and OTA.
   [`docs/MCP.md`](../../../docs/MCP.md) owns MCP tools.
   [`docs/SECURITY.md`](../../../docs/SECURITY.md) owns NVS, signing, and exposure.
   [`README.md`](../../../README.md) owns the user journey. The policy and deep references must
   agree with the code without copying those long technical catalogs back into `AGENTS.md`.
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
   (this one **and** every sibling under `.agents/skills/`) **and** the review subagents under
   `.codex/agents/` are part of what drifts — confirm each still maps the project that exists
   before you trust it, and report any gap as a `SKILL-DRIFT` finding. Do not correct it during a
   review-only pass.
7. **Write the report** in the structure at the end.
8. **Report gate readiness without mutating the PR.** If and only if the review is clean, provide
   the exact records that a separately authorized PR-body update would need:

   ```
   - [x] `$project-review` clean — merge gate @ <full-40-hex-sha>
   - [x] `$skill-audit` clean — PR create/push gate @ <full-40-hex-sha>
   ```

   [`tools/agent-hooks/require-pr-gates.sh`](../../../tools/agent-hooks/require-pr-gates.sh) validates
   top-level records against the exact lowercase 40-hex PR head; prefixes are rejected. The review
   itself never ticks/stamps them. A clean
   `$pr-hygiene` record must come from a separate run — its personal-data/language screen is a
   different axis from this review's coherence scope, so it is never established here. The only
   canonical merge is `gh --repo github.com/0Bu/tesla-key-esp32 pr merge <numeric-pr> --match-head-commit <full-40-hex-head-sha> --squash`;
   structured/MCP merges and all alternative modes fail closed.

Use parallel repository reads to cover the tree quickly, but reason about the cross-cutting
links yourself — that's where the value is.

## Project map (what to read)

| Area | Files | Responsibility |
|---|---|---|
| Boot / wiring | `main/main.cpp` (+ `boot_fatal.hpp`) | NVS, config/VIN resolve, clock restore, BLE + network bring-up ORDER, SNTP, mDNS, starts every component; boot heap log; OTA mark-valid |
| Board identity | `main/board.{cpp,hpp}` | Runtime board detection for the ONE image per chip. The esp32s3 image serves THREE boards (T-Dongle-S3 / bare ESP32-S3 / AtomS3 Lite + ATOMIC PoE Base). ONE cached detector because display and Ethernet OVERLAP ON A PIN — the panel's SPI clock is GPIO5, the PoE base's SCLK |
| Network transport | `main/net.{cpp,hpp}` + `main/ping_probe.hpp` + `logic/net_link.hpp` | The ONE transport seam: everything above it asks `tk::net_is_up()` / `net_kind()` / `net_active_netif()` and never touches `esp_wifi`. Owns the WiFi station, the endless-reconnect handler, the credential-rollback boot window (`logic/wifi_rollback.hpp`) and the generation-owned gateway-ICMP ghost-link watchdog (`net_wd`); the transport identity `NetLink::{None,Wifi,Eth}` and the watchdog's decision (incl. the never-answered-ICMP baseline rule) are host-tested in `logic/net_link.hpp`. Also the OPTIONAL W5500 SPI Ethernet backend (`CONFIG_TESLA_ETH_ENABLED`, esp32s3 only): VERSIONR probe, POLLING mode (the PoE base routes no INT/RST), a TWO-PHASE bring-up (a short link grace answers "is a cable attached", then the generous lease deadline once the PHY reports link — one timer for both delayed the setup AP on a credential-less board), a raised `route_prio` (ESP-IDF defaults ETH *below* the WiFi station; it governs `netif_default`/off-link traffic only — `ip4_route()` matches on-link destinations by `netif_list` order), and — on a lease — WiFi is never started at all |
| Target identity | `main/platform.hpp` | `TK_PLATFORM` string per `CONFIG_IDF_TARGET_*`; must agree with `/api/proxy/1/version`, the HA device model, and esp-web-tools `chipFamily` |
| Task priorities | `main/task_config.hpp` | `tk::kPrio*` — the ONE named-constant table every `xTaskCreate` site takes its priority from; must agree with the task inventory in `docs/ARCHITECTURE.md` ("Concurrency") |
| Task stack headroom | `main/stack_watch.{cpp,hpp}` + `logic/status_model.hpp` + `main/http_status.cpp` + `main/mqtt_ha.cpp` | Allocation-free, per-owning-task historical minima for httpd, vehicle, auto-pair and MQTT. Unsampled tasks are absent, while a genuine zero-byte measurement remains reportable; `/status` and MQTT are cache-only readers. |
| RTOS RAII guards | `main/rtos_guard.hpp` | `tk::SemGuard` / `MutexGuard` / `InFlightGuard` — the take/give and flag pairs that must survive a throw. Every `command_mutex_` / `scan_mutex_` acquisition goes through these, so the lock hierarchy in `docs/ARCHITECTURE.md` ("Concurrency") is only true while they are used; deliberately in `tk::` to avoid an ODR clash with a stock `MutexGuard` |
| BLE GATT client | `main/ble_client.{cpp,hpp}` | NimBLE central; discovers vehicles by the Tesla name in SCAN_RSP (the service UUID is absent from adverts), then discovers the Tesla service/write/notify UUIDs after connect; RX notify → `on_rx_data` (runs on the **NimBLE host task**) |
| Vehicle control | `main/vehicle_ctrl.{cpp,hpp}` + `main/runtime_admission.{cpp,hpp}` + `vehicle_commands.cpp` + `vehicle_telemetry.cpp` + `vehicle_pairing.cpp` (+ `vehicle_ctrl_internal.hpp`, `reboot_reason.hpp`) | one `VehicleController`, split by concern: core wiring/`link_state()` glue; global runtime admission; command API; **loop_task** (active-window polling + sleep gating) + caches; pairing lifecycle/keys; allocation-free, fail-closed heap-watchdog breadcrumb restore |
| HTTP API | `main/http_server.{cpp,hpp}` + `http_api.cpp` + `http_status.cpp` + `http_ota.cpp` + `http_config.cpp` + `http_common.cpp` + `mcp_server.cpp` + `command_exec.cpp` (+ `http_handlers.hpp`, `json_builder.hpp`, `json_http_reply.hpp`, `mcp_json_payloads.hpp`, `status_json_emitter.hpp`, `mqtt_probe_owner.hpp`, `logic/http_route.hpp`) | `esp_http_server` on :80; single catch-all `handle_all` dispatch (wrapped in try/catch) in `http_server.cpp` — EVERY request is classified by the exact method/path table in `logic/http_route.hpp` and then enters the mapped handler through the `GuardedReq` containment signature; handlers split by route group; typed body/JSON results preserve 400/413/503 and forbid dispatch/persistence after rejection; request owners are released before blocking work and sticky cJSON ownership prevents partial 200/MCP responses under allocation failure; the MQTT configuration probe owns partial callback resources through its non-throwing RAII seam; `mcp_server.cpp` serves `/mcp` (stateless JSON-RPC 2.0 MCP server for AI agents — guide in `docs/MCP.md`); both command surfaces resolve names/args via `logic/command_registry.hpp` and execute through `command_exec.cpp`; `/status` shaping decided in `logic/status_model.hpp` (`build_status_object()` gathers + serializes only) |

| HA bridge | `main/mqtt_ha.{cpp,hpp}` + `main/mqtt_json_publish.hpp` + `main/mqtt_payloads.hpp` + `main/mqtt_publish_sequence.hpp` | read-only MQTT discovery publish; its own tasks. Retained JSON is fully built before publish; build/print/broker failure publishes nothing, short-circuits discovery → availability → state, and rearms the full discovery sequence. |
| Storage | `main/nvs_storage.{cpp,hpp}` + `logic/nvs_contract.hpp` | Exact 19-record namespace/logical-key/stored-key/API/owner/retention registry. The adapter maps only the two declared long tesla-ble session names and rejects unknown namespaces, keys, wrong APIs and >15-byte/colliding additions before an NVS call; the direct display U8 migration seam is separately inventoried. |
| Diag log | `main/diag_log.{cpp,hpp}` | in-RAM console ring (`GET /diag`); **static `.bss` buffer** (heap budget!) |
| Syslog | `main/syslog.{cpp,hpp}` | UDP RFC 5424 forwarder for the captured diag lines; server from NVS `syslog_uri` / `CONFIG_TESLA_SYSLOG_SERVER` (`POST /set_syslog`, empty = disabled); hard/transient send-failure split **and** the per-line RFC 5424 PRI (facility `user`, severity from each line's own esp_log level) in the host-tested `logic/syslog_policy.hpp` |
| Crash forensics | `main/diag_crash.{cpp,hpp}` + `logic/crashinfo.hpp` + `logic/reset_reason.hpp` + `logic/bootlog.hpp` | ONE-SHOT boot capture of why the last run ended: reset reason **always** (needs no partition, so already-deployed devices keep reporting it), plus the core-dump SUMMARY where the `coredump` partition exists. Parsed once at boot, never on a request path; an ORPHAN dump (app-elf-sha ≠ running build) is erased, and declared foreign only on PROOF. Feeds `/status.last_crash`, MQTT, and the once-per-boot syslog replay (`/diag` is RAM and does not survive the reboot it would explain). BACKTRACE is **Xtensa-only** — on RISC-V (c3/c6, half the fleet) the decode is left to `GET /coredump` offline |
| Boot-loop safe mode | `main/safe_mode.{cpp,hpp}` + `logic/boot_guard.hpp` | counts CRASH-ONLY boots in NVS (`boot_fails`); past the threshold it **latches** → WiFi + web UI + OTA only, BLE/vehicle/MQTT skipped, so a board crashing on the vehicle path stays fixable in a browser. NVS read/write failure or exception enters safe mode; a healthy clear is reported only after a successful write. Complements the heap watchdog, whose cap counts only restarts WE chose — a PANIC loop was uncounted before this. The healthy timer is deliberately NOT armed while latched. Drives `/status.sys.safe_mode` |
| Heap trend | `main/heap_trend.{cpp,hpp}` + `logic/heap_history.hpp` | the board's own 24-hour free/largest-block ring (`GET /heap`), fed from the SAME two samples `loop_task` hands the heap watchdog, so the chart a human reads and the threshold the firmware acts on cannot disagree. Fixed static storage (~1.2 KB), **never heap** — a diagnostic must not compete for the block it exists to measure. It lives in **`.noinit`**, so it survives every reset that kept power: the heap watchdog's answer to exhaustion IS a restart, and a `.bss` ring was erased by the one event it exists to explain. The retained image must pass a CRC-32 **and** a derived layout fingerprint (`HeapPersist` in `logic/heap_history.hpp`) or the trend starts empty, and a carry offset keeps ONE bucket clock across the reboot — `GET /heap`'s `b_boot` names the bucket this boot began in |
| Config blob | `main/config_blob.{cpp,hpp}` + `logic/config_store.hpp` + `logic/wifi_rollback.hpp` | the ONE atomic credential/service entry in NVS: WiFi creds + one-shot rollback backup + VIN + `mqtt_uri` + `syslog_uri` as a single CRC-checked `nvs_set_blob`, all-or-nothing across a write failure AND a power cut. READS the legacy per-key layout as fallback (absent/failed CRC) and mirrors back on save, so neither an OTA nor a downgrade strands a deployed device's config. Deliberately EXCLUDES separately owned cache/time/display/reboot records (`ble_mac`, `last_time`, `reboot_why`, `disp_rot`) and different-lifetime journal/safety state (`vin_txn`, `boot_fails`); a config snapshot must neither revert another writer nor erase recovery state. |
| OTA | `main/ota_update.{cpp,hpp}` + `main/ota_manifest.hpp` | pull-based self-update; dual-slot; bounded manifest parsing rejects ambiguous target entries before download |
| Provisioning | `main/provisioning.{cpp,hpp}` + `logic/captive.hpp` + `logic/http_body.hpp` | captive setup portal when no WiFi; its `POST /save` is a separate fixed 1024-byte receive path (empty/oversized → 400), not the normal API/MCP 2 KiB body policy |
| Web UI | `main/www/` (`index.html` markup + `style.css` + `app.js`, spliced by `inline_assets.cmake`) | compiled into the app binary as ONE self-contained page; live-updates by polling `GET /status` every 4 s (cache-busted + `no-store`; a failed poll keeps the last frame and parks the BLE countdown) |
| On-device indicators | `main/display.{cpp,hpp}` + `main/display_font.h` (generated by `tools/display_sim.py`) + `main/led_status.{cpp,hpp}` | ST7735 status panel (T-Dongle-S3, `CONFIG_TESLA_DISPLAY_ENABLED`) + underside APA102 status LED (`CONFIG_TESLA_LED_ENABLED`, default off). Both are **thin renderers** — cache-only (never wake the car), no MQTT; the "what to show" decisions live in the host-tested `logic/display_model.hpp` / `logic/led_status.hpp`, both reading the shared `logic/ui_state.hpp` snapshot + `logic/soc_gradient.hpp` ramp. No-op stubs on boards without the hardware |
| Pure logic (host-tested) | `main/logic/*.hpp` (exhaustively owned by `test/logic_test_ownership.json`, including `ble_chunk`, `http_origin`, `http_route`, `nvs_contract` and `mqtt_discovery_registry`) + its named host-test entry in `test/test_logic.cpp`, `test/test_nvs_storage.cpp` or `test/test_mqtt_json_publish.cpp` | **IDF-free** logic the device and host tests share (exception: `ble_row` is a browser-consumed specification held by the parity harness). It covers negotiated BLE payloads, browser-origin rejection, command/MCP routing, transactional recovery, status/UI decisions, exact HTTP routes, all declared NVS records and all MQTT discovery entities. The ownership gate requires every header's concrete test function to have exactly one definition and invocation; decoy strings/comments and uninvoked definitions fail, while production-shared registries have mutation canaries. |

| Library | `managed_components/yoziru__tesla-ble/` | **fetched, regenerated — NEVER edit or commit** (pin in `main/idf_component.yml`); root CMake applies committed `patches/tesla-ble/` through `scripts/apply-tesla-ble-patches.sh` |

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
  boot **except after a heap-watchdog restart** (`boot_heap_restarts() != 0`) — re-seeding on
  every boot is exactly what would make a restart loop keep a parked car awake. **Never** gate on "car observed awake" — that is self-perpetuating (our polling keeps
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
  (HA shows "unknown"); the web UI **hides the hero card** for both `unreachable` and the
  cold-start `unknown` — rather than fill it with stale battery/idle chips — and signals the state
  on the BLE row instead (orange ping-pong bars + orange MAC). Never a sleep claim.
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
- The compile-time stack budget is per-frame evidence, not a call graph. Re-derive
  `scripts/check-stack-usage.py` against schema-v2 `firmware-stack-baseline.json`: every target's
  every compiler `.su` frame is inventoried; every target/file/function frame ≥256 bytes is
  review-baselined; >4096 bytes and any unbounded dynamic frame fail. Do not infer or report
  composed call depth from this gate; on-device high-water telemetry is separate evidence.
- The `main` production translation-unit surface is exactly the one literal
  `idf_component_register(SRCS ...)` inventory. `target_sources`, included/subdirectory CMake,
  property attachment, paths escaping `main/`, or any change to the exact private four-option block
  are stop findings. Independently inspect the full compile database: every repository-owned C/C++
  source must be either the exact literal `__idf_main` inventory, an explicitly reviewed generated
  source, or part of a lock-verified managed component. Extra local IDF components, local include
  roots outside those owners, untrusted external source/include roots outside the pinned IDF tree
  and exact current-build generated-source allowlist, forced include/macro files, compiler
  plugins/specs, preprocessor pass-through or prefix/sysroot forms, a compiler wrapper/environment,
  macro/`include_next` source directives, a main source outside `__idf_main`, missing
  `-fstack-usage`, or anything other than exactly `-Og` is a gate bypass. External reproducibility
  build directories must still pass the same closed generated-source classification.
- **Those guards all mean "recover and continue", which is right for a TRANSIENT shortage and
  wedges the device on a permanent one** (2026-07-18: `bad_alloc` out of `loop()` ~20×/s for ten
  hours, nothing serving, no reboot — a wedge is worse than a crash, because a crash restarts).
  The one escalation is `logic/heap_watchdog.hpp`, sampled in `vehicle_telemetry.cpp`'s
  `loop_task_fn_`: INTERNAL `largest_block` (`8BIT|INTERNAL` — plain `8BIT` would include any
  PSRAM and make it a silent no-op there) under 4 KB for 5 **unbroken** minutes,
  OTA-excused, capped at 5 consecutive restarts, breadcrumb `reboot_why=heap:<n>` → `/status`
  `last_reboot`. NVS persistence success is the reboot authority; save failure stays degraded, and
  read/erase failure or a malformed breadcrumb fails closed at the cap without reopening the
  vehicle window. Every exception/error is contained because this runs while allocation is failing,
  and every log line on it must render under ~230 chars — `diag_log.cpp` formats into a 256-byte
  stack buffer and truncates silently past it. **This is deliberate; do not review it as a reboot
  risk.** `.codex/agents/heap_safety_reviewer.toml` restates this bullet and must move with it.

### NVS / config
- Namespaces: exactly `tesla_cfg` (runtime cfg) and `tesla_ble` (key + sessions). The exact
  namespace/logical-key/stored-key/API/owner/retention table is `logic/nvs_contract.hpp` and its
  operator mirror is `docs/README.md`. Stored names are **≤15 bytes**; there is no truncation
  fallback. Unknown, moved, colliding, wrong-API and 16-byte keys fail before NVS access. An
  **empty** config value disables the feature it gates (e.g. `mqtt_uri`).

### OTA / versioning
- Dual-OTA layout (`partitions.csv`): `nvs@0x9000`, app at **`0x20000`**, two ~2 MB slots
  (`0x1f0000`), **4 MB** flash (smallest supported part; a larger flash leaves the top
  unused). NVS is never in the flashed set, so pairing/key/VIN survive OTA. One source tree
  builds for esp32 / esp32s3 / esp32c3 / esp32c6 (the tesla-ble targets); each device pulls its
  own `tesla-key-esp32<suffix>.bin`
  (`""`/`-s3`/`-c3`/`-c6`, so "esp32" appears once —
  must match across `ota_update.cpp`, `logic/target.hpp`, `ci-sign-artifacts.sh`,
  `build-pages.sh`) and the web
  installer auto-selects by chipFamily.
- Rollback is enabled and **deliberately deferred**: `main.cpp` does NOT mark the image valid
  at startup — `ota_health_gate_task` calls `esp_ota_mark_app_valid_cancel_rollback()` only
  on the verdict of the host-tested `logic/health_gate.hpp`, which is a PROVEN LINK
  (`tk::net_is_up()`, either transport), INTERNAL largest block ≥ 4 KiB **plus** an uptime floor
  of `kHealthGateBaseS` = 90 s — not uptime alone. A crash inside that window reboots
  still-`PENDING_VERIFY` and the bootloader
  reverts; so does an image that boots fine and never gets online, which a pure timer used to
  commit and which no later OTA could repair. Past `kHealthGateCapS` = 600 s an unhealthy image
  is LEFT pending (it must NOT self-restart — that would silently downgrade a good build over a
  router outage). Only successfully persisted rebooting `/set_mqtt`, `/set_syslog`, `/set_wifi`
  and setup-portal saves may confirm early. Both mark-valid paths must acquire the shared
  `HealthCommit` owner against OTA/identity/`FaultRestart` and re-check INTERNAL largest block after
  admission; any failure leaves rollback armed. `/set_vin` and `/gen_keys[?force=1]` are admitted by
  `OtaIdentityMutationGuard` only in Stable state with the OTA/identity gate idle;
  PendingVerify/unknown/active-OTA returns 503 before mutation. VIN/recovery reboots are not health
  evidence and must leave rollback armed. A credential-less, wireless device is in setup mode and
  counts as healthy.
  Re-introducing a mark-valid at startup, or a bare timer, is the regression to flag.
- **OTA images are signed** (Secure Boot v2 RSA-3072 scheme *without* hardware Secure Boot, no
  eFuses): `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` + `..._RSA_SCHEME` +
  `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` in `sdkconfig.defaults`; the build stays
  unsigned (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`); `scripts/ci-build-all.sh` is
  unprivileged and `scripts/ci-sign-artifacts.sh` receives `OTA_SIGNING_KEY` only in the protected
  main publish job. A same-repository PR remains unsigned unless a maintainer adds
  `signed-preview`; the default-branch `workflow_run` then validates head/repo/state/label, waits
  for Environment approval, signs the artifact strictly as data and revalidates immediately before
  publishing `gh-pages/PR/<N>/`. Fork PRs are never eligible. Signer and cleanup share a per-PR lock,
  with event-driven plus scheduled reconciliation. Trust is TOFU from the running app's signature
  block. Production create/reuse and protected signed-preview paths must additionally verify every
  RSA-PSS signature against `scripts/ota-signing-public-key.sha256` and accept exactly one
  first-position block before artifact or Pages publication. Changing the
  key or pin is a separately reviewed USB fleet migration: software-only TOFU cannot rotate keys
  over OTA. The `flash-esp32`, `ship` and both `usb-recovery` artifact-selection paths must run the
  same app-only signature/pin validator before a physical write; metadata/size checks alone are a
  stop finding. The signed-preview producer and `flash-esp32` consumer must agree on the exact
  `tesla-key-esp32-pr<PR>-<full-head-SHA>` artifact name; the consumer obtains the display version
  only from the checked `build-metadata.txt`. Classic esp32 needs
  `CONFIG_ESP32_REV_MIN_3` (`sdkconfig.defaults.esp32`).
- **Pages has one serving authority:** GitHub Pages API must report branch-backed `legacy` mode,
  source `gh-pages:/`, for both root and `PR/<N>/`. `scripts/check-pages-source.py` is run before
  protected signing, immediately before branch publication/deletion, and when deriving the URL for
  live acceptance. `actions/upload-pages-artifact` / `actions/deploy-pages` or another branch/path
  is a stop finding, not an alternative deployment mode.
- **Downgrade gate (software anti-rollback):** before the bulk download, `ota_task` reads the
  downloaded image's own app-descriptor version (`esp_https_ota_get_img_desc`) and refuses
  anything not strictly newer than the running firmware — a signature proves authenticity, not
  freshness. Weakening it re-opens the old-but-validly-signed-image attack.
- `version.txt` is the committed **version floor**; CI (`scripts/select-release-version.sh` →
  `scripts/next-version.sh`, see `.github/workflows/build.yml`) computes the actual stable release
  as the maximum of that floor, stable-tag patch increments and prerelease-core promotions, but
  publishes only stable `X.Y.Z` identities and idempotently reuses one stable Release tag already
  pointing at the exact current-main source SHA after a partial-publish retry only while it remains
  the newest valid tag; stale runs block. A retry with an already immutable Release must download
  and bind its exact 40 assets, 28 diagnostics, signed-app aliases/merged slices and ELF checksums,
  then reconcile Pages without key provisioning, signing, Release upload or Release mutation.
  A successful reuse run still uploads one new SHA-bound Actions recovery artifact containing the
  twelve production-pin-verified app/merged aliases; absence or ambiguity is a stop finding.
  `scripts/release-relevance.sh` compares current main cumulatively with the last root-Pages
  snapshot whose branch manifest, actually served live manifest, source SHA, latest GitHub Release
  API object with `immutable: true`, tag and four digest-bound merged assets all agree; rename folding is disabled so deleting or
  moving a firmware path stays relevant. Thus a docs-only successor reconciles firmware from a
  stale overlapping main run, and a same-SHA rerun retries a failed branch publication, instead of
  losing that release. An absent, unreadable, mutable, missing-`immutable`, or partially published baseline yields
  release-relevant, never a silent skip. CI refetches current main/release-candidate state before
  signing, immediately before signed-artifact upload, in the same shell step before draft publish,
  and before Release/Pages mutation;
  PR preview display versions use `--latest-published-stable` (complete immutable stable Release assets),
  never a newer raw prerelease tag whose numeric core could collide with the subsequent stable OTA;
  Pages also requires the matching latest GitHub Release object to report `immutable: true` and four digest-bound merged
  assets, not merely a checkout-local tag. CI
  **stamps the selected version into the build**, so the reported version,
  firmware filename, Release tag and OTA manifest agree.
  Locally `version.txt` → `PROJECT_VER` directly. Either way a **hardcoded version anywhere
  else is drift**. Old single-`factory` devices need a one-time USB reflash (migration).

### evcc / HTTP contract
- Response shape must match TeslaBleHttpProxy: `.response.response.charge_state.*`, the field
  is **`charge_amps`** (not `charging_amps`), and `charge_state` is **always fully populated**
  (a missing numeric field makes evcc parse `<nil>` and fail). `vehicle_data` is served from
  **cache** and never blocks. Idle cache may be old so reads do not wake the car; while charging
  or for five minutes after a command it must be ≤30 s old, otherwise HTTP 503 exposes a broken
  feedback path.
- `set_charging_amps` requires an integer body, serializes action ACK + explicit ChargeState
  readback, and succeeds only on a fresh exact amp match. Missing/mismatching readback and Tesla
  rejection are HTTP 502. Replayed CarServer counters must return before callbacks/FIFO completion
  via the pinned patch in `patches/tesla-ble/`.
- `charge_start` accepts the JSON scalar `true` and `charge_stop` accepts `false` because evcc's
  generic boolean setter emits those bodies. The matching command/value pairs are the only
  scalar-body exception; mismatched booleans and other non-object bodies are HTTP 400.
- **No HTTP auth / TLS by design** (evcc can't send credentials) — trusted LAN only. Mutating
  browser requests add a narrower device-Host + same-Origin/`Sec-Fetch-Site` gate (including
  state-changing GET forms) while headerless evcc/curl remains allowed; do not mistake that CSRF
  mitigation for LAN-client authentication. Document any deviation in `docs/SECURITY.md`.

### Pairing
- Keys are enrolled **Charging Manager only**; owner role is intentionally removed
  (`?role=owner` → 403). The on-screen "Add key" dialog on the car appears **only while a
  Tesla NFC keycard is on the console reader**. Three events invalidate a pairing and must
  clear session + cache: key deleted on the car (`whitelist`), `gen_keys?force=1`, VIN change.

### Telemetry / MQTT
- proto3-optional fields are emitted **only when the car reported them** (presence flags) so
  the UI/HA show "—"/unknown, never a phantom 0. The MQTT bridge is **read-only** (no command
  topics subscribed — the car is never controlled or woken from HA).
- Retained discovery/state JSON is all-or-nothing: sticky cJSON construction publishes nothing on
  build/print OOM or a broker error, and any failure rearms Discovery → availability → state. The
  exact pinned-cJSON allocation matrix and publish spy must stay wired into the sanitized IDF gate.
- `sleep_state` (MQTT) and the web-UI hero **both** derive from `link_state()` — see *Link
  state* above. The MQTT switch over the four values must stay **exhaustive** (no catch-all
  `else` defaulting to "asleep") and the web UI must handle every state, including the omitted
  "unknown" — the historic bug was `unknown` falling through to a false "asleep".

## Cross-cutting consistency (add X → also update Y)

The most common inconsistency is a feature that exists in code but not in all the places
that describe it. When reviewing a change (or the repo as a whole), check these links:

- **New/changed HTTP endpoint** → `handle_all` dispatch **and** the API list in
  `docs/README.md` **and** the web UI if user-facing.
- **New command** → a `kCommands` registry row (`main/logic/command_registry.hpp`) **and**
  the kind dispatch in `main/command_exec.cpp` **and** the command list in `docs/README.md`.
  The web UI currently exposes the supported charge toggle and explicit wake quick actions; keep
  those buttons, the registry and the handler semantics aligned.
- **New MCP tool** → the same `kCommands` row gains `mcp_name`/`mcp_desc` (the ONE source
  the advertised schema, the MCP executor validation AND the REST validation are generated
  from; `tools/list` order = table order) **and** a `CHECK` in `test/test_logic.cpp`
  (`test_mcp`) **and** the tool table in `docs/MCP.md`. (Tools mirror the run-on-key charging command set + read-only
  `get_vehicle_state` — never expose a role-refused command: `mcp_name == nullptr`.)
- **New/changed `/status` field** → `logic/status_model.hpp` **and** its golden emissions
  in `test/test_logic.cpp` (`test_status_model`) **and** the web UI consumer (`www/app.js`).
- **Changed presenter decision with a MIRRORED consumer** → the spec header
  (`main/logic/ble_row.hpp`, resp. `logic/display_model.hpp`) **and** its mirror (the `BLE_ROW`
  region of `main/www/app.js`, resp. `tools/display_sim.py`) **and** the `CHECK`s in
  `test/test_logic.cpp` **and** the exhaustive sweep in `test/<name>_golden_dump.cpp`. The two
  sides are held together by `scripts/check-ble-row-parity.sh` / `check-display-sim-parity.sh`
  from `run-mock-tests.sh` — editing one side alone fails CI, which is the point.
- **New log line carrying a PRIVATE IDENTIFIER** (VIN, SSID, device IP, a vehicle/nearby-device
  BLE MAC, the MQTT broker or
  the syslog host) → a matching rule in `main/logic/redact.hpp`'s `kDiagRedactions` **and** a
  `CHECK` in `test/test_logic.cpp` (`test_redact`). The table is keyed on log PHRASES, so a new
  phrase carrying an old value is a SILENT leak — `/diag?redact=1` keeps answering 200 and the
  reader believes the log is scrubbed. This has now happened twice: the `http_server.cpp` request
  log (the VIN on every evcc poll — the most frequent line in the ring) and the failure branch of
  the `ble_mac` write, whose success branch was covered and whose failure branch was not. Both
  landed with the feature that introduced them and neither carried a rule.
  The physical controller's board MAC is intentionally diagnostic and remains visible.
- **New NVS key** → register logical/stored key, namespace, storage API, owner, retention and
  secrecy in `logic/nvs_contract.hpp`; update its exact host oracle and `docs/README.md`, and,
  when secret or security-relevant, `docs/SECURITY.md`.
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
  **and** update the full semantics in `docs/ARCHITECTURE.md` plus any enumerated status/MQTT
  values in `docs/README.md`.
- **New chip / target** → only for a chip tesla-ble already lists (the Component Manager refuses
  any other, and a locally patched checkout of the crypto library was tried for esp32c5 and
  dropped — `docs/adr/0004-drop-esp32c5-target.md`): `main/idf_component.yml` git dep
  **and** `logic/target.hpp` (enum + `platform_name`
  + `image_suffix`) **and** `platform.hpp` (`TK_TARGET`) **and** the OTA `<suffix>` map
  (`ota_update.cpp`) **and** `ci-build-all.sh` (`TARGETS`, unsigned build + projected-signed size)
  **and** `ci-sign-artifacts.sh` (`TARGETS`/`image_suffix`/`boot_offset`, real signature + merged
  image) **and** `build-pages.sh` (`TARGETS`/`image_suffix`/`chip_family`) **and**
  `test/test_logic.cpp` CHECKs **and** every doc that lists the supported targets. (esp32c6 binds
  the size policy: `ci-build-all.sh` gates the deterministic projected signed size and the trusted
  signer independently gates the actual signed bytes after 64-KiB padding + 4-KiB signature.)
- **New BOARD variant on an existing chip** → the runtime detector in `main/board.{cpp,hpp}`
  (ONE cached probe — never a second copy) **and** the per-target `sdkconfig.defaults.<target>`
  **and** `main/Kconfig.projbuild` if it adds an option **and** every doc that lists which boards
  an image serves (`docs/README.md` Hardware, `docs/FEATURES.md`). The trap
  is SHARED GPIOs: the esp32s3 image serves a T-Dongle-S3 (ST7735 on MOSI3/SCK5/CS4) and an
  AtomS3 Lite + ATOMIC PoE Base (W5500 on SCLK5/CS6/MISO7/MOSI8) — **GPIO5 is both** — so a new
  peripheral probe must be gated on the detector before it drives a pin.
- **WiFi/LAN reconnect or watchdog change** → the STA→LAN reconnect policy lives ONLY in
  `main/net.cpp` (`MAX_RETRY`, `s_ever_up`, `kWdPeriodS`/`kWdPingCount`, the
  `s_gw_ever_reachable` baseline latch) with the watchdog's DECISION — the consecutive-failure
  count `kWatchFailsToRecover` and the never-answered-ICMP guard — in the host-tested
  `main/logic/net_link.hpp` (`watch_step()`, cases in `test/test_logic.cpp`) **and** mirrored in
  the **"WiFi / LAN connectivity"** section of
  `docs/ARCHITECTURE.md` (which quotes those numbers). This is the STA→LAN link, **distinct**
  from the car-BLE `link_state()`.
  Invariant: the watchdog must **never reboot** (a reboot mid-outage hits `wifi_connect()`'s
  boot timeout → setup portal, abandoning good credentials).
- **MQTT transport / TLS-default change** → the scheme-defaulting rule lives in
  `mqtt_ha.cpp` (`mqtt_ha_start`: schemeless broker ⇒ `mqtt://`, but ⇒ `mqtts://` when
  credentials are present, CA-bundle-verified, **no plaintext fallback**) **and** surfaces in
  `/status` (`mqtt.tls`/`mqtt.error`, `http_status.cpp`) **and** the web UI's "· secured" MQTT
  row **and** the MQTT sections of `docs/README.md`, `docs/ARCHITECTURE.md` and, for transport
  trust claims, `docs/SECURITY.md`.
- **tesla-ble library bump** → `main/idf_component.yml` pin **and** explicitly rebase/remove
  every committed `patches/tesla-ble/` change against the new source. Never hand-edit or commit
  `managed_components/`; the configure-time patch script owns generated checkout changes.

## Reviewing the skills (meta-coherence)

The skills under `.agents/skills/` **and the review subagents under `.codex/agents/`** are
themselves documents that drift — each lags the code by exactly the changes landed since it was
last touched. A review is **not complete** until you have checked that **every** skill and
**every** agent still describes the project that exists; otherwise future runs inherit a stale
map. Treat a gap as a real finding (`SKILL-DRIFT`), reported alongside code/doc findings, and
propose the specific edits without applying them during a review-only pass.

The agents matter here for a second reason: two of them **duplicate content this skill owns**,
so they are cross-cutting sinks like any other. `doc_drift_checker` restates this skill's
*cross-cutting "add X → also update Y" list* and `heap_safety_reviewer` restates the *heap /
contiguous-block invariant* — touch either here and the matching agent must move too, or the
project ends up with two review maps that disagree. That drift is exactly what a coherence
review exists to catch, so hold the agents to it.

### Termination — one read-only pass

A review is read → check → report → stop. Drift is measured against code/config/scripts, never
against prose preference. Report only contradictions grounded in a named project fact. Do not
edit and re-audit in the same review-only run; a separately authorized implementation can address
accepted findings, and a later independent review verifies those changes.

### This skill (`$project-review`)

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
  switch against `docs/README.md` plus the MCP tool table in `docs/MCP.md`.
- **No stale specifics in the skill text** — hardcoded chip names (e.g. a lone "ESP32-S3" where
  it is now multi-target), file paths, partition offsets, sizes, or version assumptions.
- **Recency cross-check.** `git log --oneline -10 -- main/` vs. the skill's last change
  (`git log -1 -- .agents/skills/project-review/SKILL.md`): every commit in between is a
  candidate for an invariant or cross-cutting link the skill hasn't absorbed yet.

### The other skills (audit and report each)

The same drift hits the sibling skills. **Discover them, don't hardcode the list:**
`ls .agents/skills/*/SKILL.md` (skills) **and** `ls .codex/agents/*.toml` (subagents). For each,
the test is the same — does its `description` + steps + concrete numbers (offsets, counts, flags,
paths, target set) still match the script, code, and config it drives? Report a stale one as
`SKILL-DRIFT` with a proposed change; do not edit it during the review. The current siblings and
what each must stay true to:

- **`$flash-esp32`** wraps the local compile + explicitly signed USB-flash path and the
  provenance-checked signed-PR-preview download. Re-verify against `scripts/idf-docker.sh`
  (Docker-pinned from `esp-idf-toolchain.txt`, no local IDF), `.github/workflows/build.yml`
  (ordinary PR artifact is `firmware-unsigned`), `.github/workflows/signed-pr-preview.yml`,
  `partitions.csv` (offsets `nvs@0x9000`, `otadata@0xf000`, app `@0x20000`), the target set and
  app-only activation sequence. Every flash command must use a verified signed app, write/erase
  otadata last and preserve bootloader/partition/NVS; absence/ambiguity/SHA mismatch must stop
  before USB write. Fresh-board bootloader offsets belong to the Pages installer contract.
- **`$ship`** takes a merged PR to the board: squash-merge → `gh run watch` on the post-merge
  `build` run → download the signed artifact (`tesla-key-esp32-<version>-<full-source-SHA>`; per-target
  `tesla-key-esp32<sfx>.bin`, never `*-merged.bin`) → USB app-slot flash (`0x20000` + otadata
  erase `0xf000/0x2000`, NVS preserved) or device OTA → verify via `/status` +
  `/api/proxy/1/version`. The OTA branch must bind `update_available` to the exact artifact
  version before POST, bound download/reboot retries, and keep exact version/platform under
  observation for at least 100 s from the first post-OTA live baseline, with monotonic uptime and
  wall-clock deltas both reaching that floor and staying within the small documented tolerance.
  Absolute device uptime does not prove the 90-s health-gate probation has elapsed, and a hidden
  reboot or unconfirmed mark-valid result must fail closed. USB gets only a short bounded boot/WiFi
  reachability retry and must not inherit the probation wait.
  Re-verify against `.github/workflows/build.yml` (artifact naming, the
  firmware-change-gated release), `scripts/ci-build-all.sh` (unsigned four-target producer),
  `scripts/ci-sign-artifacts.sh` (suffix map, signing, merged copies), `partitions.csv` offsets,
  and the `/ota/*` endpoints. Complementary to `$flash-esp32`
  (local-tree build+flash, no merge); it defers the merge gate to `require-pr-gates.sh`, including
  current `$project-review` and independent `$pr-hygiene` records plus `$feature-docs` when the
  diff is feature-relevant.
- **`$vehicle-command-audit`** compares the firmware against upstream `teslamotors/vehicle-command`,
  gated by what `yoziru/tesla-ble` (pin in `main/idf_component.yml`) can actually do. Re-verify the
  tesla-ble **pin** in its source map (`v5.1.2`) still matches `idf_component.yml`, that its upstream
  file paths still resolve (e.g. `pkg/vehicle/charge.go`), and that its "worked findings" table is
  not asserting drift already fixed in the tree. It is the *upstream-conformance* counterpart to this
  skill — keep the two complementary, not overlapping.
- **`$add-logic-test`** scaffolds a new pure-logic unit in `main/logic/` + its `CHECK` cases in
  `test/test_logic.cpp`. Re-verify its claims against `scripts/run-mock-tests.sh`, the CI
  `logic-test` job (`.github/workflows/build.yml`), the `stop-logic-tests` handler in
  `tools/agent-hooks/agent_hook.py` wired by `.codex/hooks.json`, the
  `CHECK`/`CHECK_STR`/`CHECK_NEAR` macro set in
  `test/test_logic.cpp`, and the `static_assert` lock pattern (`main/ota_update.cpp` /
  `main/logic/target.hpp`).
- **`$pr-hygiene`** screens the PR title/body, commit messages and touched documentation for
  personal/private information (LAN IPs, MAC addresses, VINs, WiFi network names, hostnames,
  emails) and for content not written in English. Re-verify it against
  `tools/agent-hooks/require-pr-gates.sh`: it is the **fourth** PR gate and the strictest — it
  fires at PR creation, every push, **and** merge, unlike `$skill-audit` (create/push only) or
  `$project-review`/`$feature-docs` (merge only) — and, unlike `$skill-audit ⊂ $project-review`,
  neither this review nor `$skill-audit` establishes its readiness on their own; confidentiality
  and language are a separate axis from coherence.
- **`$feature-docs`** keeps `docs/FEATURES.md` in sync when a platform feature lands or changes.
  Re-verify its conditional merge gate against `tools/agent-hooks/require-pr-gates.sh`, especially
  that the policy-path set covers `AGENTS.md`, `.agents/`, `.codex/`,
  `.github/PULL_REQUEST_TEMPLATE.md`, `tools/agent-hooks/`, and `tools/agent-config/`, and that the
  firmware/release set still covers `main/`, `test/`, `sdkconfig.defaults*`,
  `partitions.csv`, the shipped Pages runtime (`docs/index.html`, `installer-bootstrap.mjs`,
  `serial-port-release.mjs`, `web-installer.mjs`, `docs/vendor/`), and
  `.github/workflows/{build,signed-pr-preview,pr-preview-cleanup,pr-policy,bench-acceptance}.yml`, plus the cumulative
  Release/Pages classifier `scripts/release-relevance.sh`.
  Confirm all four gates fail closed when `pr-gate-lib.sh` is missing or incomplete. Keep its
  gate mechanics aligned with the
  three unconditional PR gates and with `$skill-audit`'s corresponding sibling entry. Bash matching
  is centralized in `gate_bash_actions`: wrappers/path-qualified commands and compound actions
  must be recognised, while every create/push/merge must be standalone so no earlier segment can
  mutate the audited HEAD/config/PR. Multiple/ambiguous actions fail closed.
- **`$skill-audit`** is the dedicated, PR-gated skill that runs *this very audit* (every skill +
  agent vs. the project) on its own, gated by `require-pr-gates.sh` (blocks opening a PR and
  every push to it, not the merge). It is the **authority for
  the per-sibling drift checklist** — this section and `$skill-audit`'s *Per-target checklist*
  describe the same siblings and must agree; a divergence between them is itself `SKILL-DRIFT`.
  `$skill-audit ⊂ $project-review`: a clean full review can establish readiness for both PR
  records without editing the PR (step 8). Re-verify its own numbers (hook `require-pr-gates.sh`, the PR-checkbox
  gate mechanism — no file marker, command count `15`, tesla-ble pin), including that
  `gate_push_head_sha` permits only current-project/current-HEAD/current-branch pushes to the one
  verified `origin` and rejects Git-global or environment repo/config context (`-C`, `-c`,
  `--git-dir`, `GIT_DIR`, `GIT_CONFIG_*`, env cwd, etc.), path-qualified executables and
  foreign/multiple refspecs before trusting the stamp. Confirm PR-create checks only the one exact
  body/body-file (never title/other args), and that the sibling/agent list still matches the tree.
- **`$device-diag`** is the read-only, cache-only live-board triage lens (`/status` + `/diag` →
  symptom→cause table); it diagnoses and hands off, never flashes or commands the car. Re-verify the
  `/status` keys it reads against the field contract in `main/logic/status_model.hpp` (the key
  literals live in its `emit()`, not in `http_status.cpp`, which only gathers + serves), the lowercase
  `link_state_web_str` strings (`main/logic/link_state.hpp`), the `/diag` params, and its
  error-signature sites (the classified production `BLE connect gave up` line in
  `vehicle_commands.cpp`, raw maximum-DEBUG `connect error` detail in `ble_client.cpp`, and
  `BOOT`/`HEAP` in `main.cpp`). Keep it
  complementary to the global `$tesla-key-e2e-evcc` skill (which drives the command path) and the
  flash/recovery skills.
- **`$display-preview`** renders `tools/display_sim.py` to PNGs for a human eyeball pass. Re-verify its
  CLI modes + default outputs against the script's `__main__`, the presenter/renderer it targets
  (`main/logic/display_model.hpp`, `main/display.cpp`), and the parity gate it defers to
  (`scripts/check-display-sim-parity.sh`) — one of the two parity gates `run-mock-tests.sh` runs, the
  sibling being `scripts/check-ble-row-parity.sh` for the web UI's BLE row. It is the visual
  complement to that automated gate, not a replacement.
- **`$ota-release-verify`** verifies the already-published OTA channel is byte-for-byte coherent
  with the latest GitHub Release: manifest `sourceSha` equals the dereferenced Release-tag commit,
  all 16 parts match manifest length/SHA-256 and their byte ranges in the four exact Release merged
  assets, and all four apps report the exact Release version and chip family. Re-verify the
  legacy `gh-pages:/` serving authority (`scripts/check-pages-source.py`), the
  manifest/firmware-base URLs (`main/Kconfig.projbuild`), the 4-chipFamily set + per-part offsets
  (`scripts/build-pages.sh`/`scripts/check-release-pages-bytes.py`), the suffix map
  (`ota_update.cpp`/`logic/target.hpp`/`ci-sign-artifacts.sh`/`build-pages.sh`), and the `/ota/*` +
  `/api/proxy/1/version` endpoints. Confirm `workflow_dispatch` remains build/test-only and cannot
  sign, release or republish Pages, old runs fail once main or the newest valid tag advances, and
  eligibility is rechecked immediately before external mutations. Complementary to `$ship`
  (cut/flash a release), not overlapping.
- **`$usb-recovery`** is the no-build emergency reflash and requires explicit user authorization:
  USB-write only an exact signed Release asset byte-matched to its source-SHA-bound main artifact,
  or one exact signed main artifact; Pages is never a CLI fallback. Then write the app to
  `0x20000` + erase `otadata`, NVS preserved. Re-verify its
  partition map against `partitions.csv` (app `@0x20000`, `otadata@0xf000/0x2000`, `nvs@0x9000`
  untouched), the signed-image requirement, the `-merged.bin` warning, and the no-auto-reset
  gotchas. Post-reset verification uses a short bounded reachability retry and requires exact
  version/platform plus `paired:true`. It is the
  recovery counterpart to `$flash-esp32`/`$ship`, not a build path.
The review subagents under `.codex/agents/` — audit these the same way (they are the targeted
lenses this skill delegates to; keep them complementary, not contradictory):

- **`doc_drift_checker`** is the fast targeted-diff lens for the *cross-cutting* links. Its
  "add X → also update Y" enumeration must stay a subset of — and agree with — the *Cross-cutting
  consistency* section above; a link added here that it lacks (or a stale one it still lists) is
  `SKILL-DRIFT`.
- **`heap_safety_reviewer`** is the allocation/throw lens. Re-verify its numbers and rules
  against the *Memory / heap* invariant above (largest **contiguous** block is the binding limit,
  `handle_all` try/catch → 503, streamed `/diag`) and against `main.cpp`'s heap-attribution log —
  the two heap maps must not diverge.
- **`agent_config_reviewer`** audits runner-neutral agent configuration, skills, hooks, and
  subagents—not firmware logic. Confirm its boundary still points firmware-correctness work back
  at this skill and its inventory matches `AGENTS.md`, `.agents/`, `.codex/`, and
  `tools/agent-hooks/`.
- **`multi_target_build_reviewer`** is the per-target build/config divergence lens (the four
  targets built from one tree). Re-verify its facts against the *Cross-cutting consistency*
  section and the build wiring: the target set (esp32/s3/c3/c6), per-target bootloader offsets
  (`0x1000`/`0x0`), the image-suffix map (`ci-sign-artifacts.sh`/`build-pages.sh`/
  `ota_update.cpp` `TESLA_OTA_IMG_SUFFIX`), the app-size gate (`slot − 32 KB`), the single git
  `main/idf_component.yml` dependency, plus the
  all-target source-patch path (`patches/tesla-ble/` + apply script + root CMake). Keep it
  complementary to this skill, not a firmware-logic reviewer.
- **Any skill or agent added since this was written** must be audited too — and added to this list.

A skill or agent that drives a script is only as current as the script: when the script changes,
re-read the doc that documents it. Project hooks in `.codex/hooks.json` must delegate to the
neutral implementation under `tools/agent-hooks/` (`agent_hook.py`, `require-pr-gates.sh`, and
their shared helpers). A hook whose behaviour a skill/agent describes must match that core.

## Verification discipline (avoid confident-but-wrong findings)

Firmware bugs are easy to mis-diagnose. Hold findings to evidence:

- Separate **confirmed** ("I reproduced it / the code path provably does this") from
  **suspected** ("looks wrong, needs checking"). Label them differently in the report.
- For **runtime** claims (crash, wake, race, OOM, leak) cite the distinguishing signal, don't
  guess: a reset is `PANIC` vs `BROWNOUT` vs `*_WDT` (`esp_reset_reason`); a crash backtrace
  only means something decoded against the **matching** `ELF file SHA256`; do not flash during a
  review to obtain one—use a provenance-matched artifact or report the decode as unverified. OOM shows up as a
  shrinking **largest free block**, not total free; a leak is a monotonic decline, fragmentation
  is a stable-but-low largest block.
- Don't propose a hand-edit to `managed_components/`. Recommend a wrapper guard or pin bump; when
  a confirmed defect is inside library dispatch and no upstream release contains the fix, propose
  a minimal repository patch under `patches/tesla-ble/`, to be implemented only with separate authorization.
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
SKILL.md under .agents/skills/ still cover the tree?): ✓ consistent / ✗ drifted.>

## Prioritized actions
1. <must-fix> … 2. <should-fix> … 3. <nice-to-have> …
```

Order findings by impact (a wake/heap/OTA correctness bug outranks a doc nit). Keep each
finding tight and actionable — the point is that someone can fix the whole project from this
report without re-deriving the context.
