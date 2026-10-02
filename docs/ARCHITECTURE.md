# Architecture reference

Deep internal reference for tesla-key-esp32 — the **on-demand** companion to
[`AGENTS.md`](../AGENTS.md), which carries the always-needed authorization, target, build,
security and memory-safety rules. Read this when touching telemetry, MQTT/Syslog, the web UI feed,
WiFi/Ethernet, sleep/link state, pairing, OTA, HTTP/MCP intake, or anything involving locks and
tasks (the [Concurrency contract](#concurrency-normative-contract) at the end). Keep both in sync;
the canonical `$project-review` skill checks for drift.

| Topic | Owner |
|---|---|
| Vehicle-facing behavior, runtime internals (this file) | `ARCHITECTURE.md` |
| Platform mechanisms and *what failure each prevents* (crash forensics, recovery ladder, config blob, redaction, CI gates) | [`FEATURES.md`](FEATURES.md) |
| Threat model, signing, release pipeline, CI trust boundaries | [`SECURITY.md`](SECURITY.md) |
| HTTP API, MQTT topics, NVS retention, build/flash/provisioning | [`README.md`](README.md) |
| MCP wire contract and client setup | [`MCP.md`](MCP.md) |

## Repository agent architecture

[`AGENTS.md`](../AGENTS.md) is the concise, runner-neutral policy. Reusable workflows live in
[`.agents/skills/`](../.agents/skills/), hooks and specialist reviewers in
[`.agents/`](../.agents/), lifecycle policy in [`tools/agent-hooks/`](../tools/agent-hooks/). CI
mutation-tests this configuration and rejects retired runner-specific metadata.
`.github/workflows/pr-policy.yml` evaluates SHA-bound gate records from trusted base-branch code
with a read-only token and never checks out PR code; it blocks merges server-side only once
repository rules require `pr-policy / current-head-records`. This layer never affects firmware
behavior: not the four-target build, dependency/patch chain, partitions, signing, OTA format,
pairing/session state or vehicle commands.

## Web UI

**Layout.** The page (`main/www/`, inlined into one gzipped document at build time) has two blocks:
the **Car** hero card (battery gauge, status, detail chips, gauge tap action) and one settings card
(`#paneSettings`) with rows **Vehicle** (VIN), **Security key**, **Network** (Wi-Fi/Ethernet,
Bluetooth, MQTT, Syslog) and **Firmware**. From 860 px the hero is sticky in the left column and
settings fill the right; below that both stack. The Firmware row shows version and channel: the
version (`#verLink`) opens the channel dialog (`#chanModal`), the download icon (`#fwCheckBtn`) runs
an on-demand check and opens the update dialog (`#otaModal`) only if a newer version exists. While a
check, channel change or update runs (`isOtaBusy()`), all settings controls are disabled and no
second dialog opens; focus returns to the initiating control afterwards. Text entry and
confirmations (`askText()`, `askConfirm()`) are Promise-based modal sheets with input validation
and fail-closed save handling.

**Live feed (`GET /status`).** The feed is a **browser-side poll**: `boot()` in `main/www/app.js`
calls `poll()` once for first paint (the hero ships hidden and only `render()` reveals it) and then
every **4 s**. `poll()` fetches `/status?ms=<now>` with `cache:'no-store'` — cache-busted because a
cached copy would freeze the hero on a stale state — and passes the response of
`build_status_object()` (`http_status.cpp`) straight to `render()`.

- `poll()` is also called after a user action (charge/wake/gen-key) so the UI reflects it at once.
- A **failed poll changes nothing on screen**: the last frame and optimistic state stay, but the
  `feedOk` flag clears and parks the Bluetooth phase countdown — the one element that would still
  be making claims about a device we have stopped hearing from.
- `render()` **diffs before writing** (`setHTML` caches the last markup per node); reassigning
  `innerHTML` every 4 s would restart CSS animations and make the hero ring visibly jump.
- `waitReboot()` (post-OTA detection) is a separate 1 s `GET /status` loop.

*Why polling, not push.* An earlier WebSocket feed let a subscriber that stopped reading (suspended
laptop, background tab) fill its TCP send buffer, so each async send blocked the httpd task for the
5 s `send_wait_timeout` while a broadcast task queued a heap copy of `/status` every 2 s. The
backlog exhausted the heap (largest block 31744 → 544 B) and left the device wedged for hours.
A poll is request/response: nothing is queued per client, and a browser that stops reading costs one
socket that `lru_purge_enable` reclaims.

## Read-only telemetry and command correctness

A rotating background poll in `loop_task_fn_` (one domain per ~30 s: climate → drive → tires →
closures, full set ~120 s) refreshes per-domain caches through the native `on_*_state_` handlers in
`vehicle_telemetry.cpp`. All polls are `NO_WAKE_SKIP` (never wake the car) and feed MQTT/HA; evcc
and pairing are unaffected. tesla-ble callbacks run synchronously under `vehicle_mutex_`, so hooks
copy only trivially-copyable nanopb state into fixed latest-value slots under a short `portMUX`;
`vehicle_loop` parses strings and publishes the public caches under `cache_mutex_` *after*
releasing `vehicle_mutex_`. No heap operation or nested cache lock runs in a library callback.

Background polls **pause while a serialized command/query is in flight** (`cmd_in_flight_`),
including the VCSEC health probe, so nothing is injected into the single BLE FIFO behind another
operation. Whether a connect attempt is foreground is carried separately as `ConnectOrigin`; every
HTTP/manual entry point and unattended auto-pair call must choose it explicitly (the public methods
have no default).

**`set_charging_amps` is one transaction** under `command_mutex_`/`cmd_in_flight_`: send the action,
wait for Tesla's ACK, then send an independent `getChargeState`. The persistent callback publishes a
fixed `ChargingAmpsFeedback` generation plus the readback currents under the same `portMUX` before
the poll completes, so success requires a new generation, a present field and an exact
`charging_amps` match; the deferred parser clears the previous ChargeState before decoding so an
omitted field cannot inherit an old value. An ACK alone is never success. Two mismatching/missing
readbacks exhaust the command budget and return an error (requested/applied/request/actual currents
are logged).

**CarServer response validation and idempotency.**
1. *Request-UUID matching* (`logic/ble_dispatcher.hpp`, `vehicle_telemetry.cpp`): a CarServer
   response with a non-empty `request_uuid` is checked against the outstanding request for that
   domain. A mismatched, late or foreign response is dropped before callbacks, ending in an
   explicit timeout rather than completing whatever sits at the FIFO head.
2. *Idempotent setpoints* (`logic/command_result.hpp`, `command_runner.hpp`): a setpoint the car
   already holds returns `actionStatus.result != OK` with reason `already_set`;
   `tk::is_nominal_already_set()` classifies that as success, so web UI, MQTT and MCP setpoint
   writes are idempotent.

**Cache freshness.** `GET vehicle_data` stays cache-only and non-blocking. The same ChargeState
callback stamps `last_charge_ticks_`. Idle values may be old so read-only polling never wakes a
sleeping car; during the active window (charging, or a command in the last five minutes) data older
than 30 s returns HTTP 503, so a BLE parser/retry storm is visible to evcc instead of hiding behind
a valid-looking 200.

**One-shot charge poll on wake and stale-cache bootstrap** (`logic/wake_poll.hpp`, fired from
`loop_task_fn_`). A parked car that wakes *itself* — typically when the charge cable is plugged in —
would never refresh its cached SOC: the active window opens only on a recent command or cached
charging, so evcc keeps serving the stale pre-plug reading, and if that SOC sits above `minSoc` it
sees no reason to start the charge that would open the window. `WakePollState` therefore fires
**exactly one** `charge_state_poll(NO_WAKE_SKIP)` per wake episode:

- *Edge arm:* the VCSEC sleep flag's `ASLEEP→AWAKE` edge, armed only after a *debounced* `ASLEEP`
  run (`kAsleepDebounceS`), so the ~60 s Cabin-Overheat-Protection flap cannot fire spurious polls.
- *Stale-cache bootstrap arm* (`paired && ble_connected && !cache_fresh`, freshness keyed on age
  `< kChargeCacheFreshS`, not validity): covers the cases with no edge — a device reboot (cache
  starts invalid) and return from a drive (the pre-drive wake consumed the arm; reconnect reads
  `UNKNOWN→AWAKE`, which is no edge), which once left an hours-stale 83 % on the wire against an
  actual 18 %.
- *Anti-drain:* `NO_WAKE_SKIP` skips a car already back asleep; the device only piggybacks on a wake
  the car performed itself and never opens the active window merely because the car is awake.
- *Retry:* the latch tracks success, not dispatch. A timed-out/failed poll (issue #301) retries with
  exponential backoff (30 s, capped at 300 s).
- *Quiescence:* once a fresh cache is held in an awake episode (issue #308), `episode_fresh` keeps
  the system quiet even after the cache ages past `kChargeCacheFreshS`, so an idle parked car is not
  polled every 60 s and can sleep. It resets on stable sleep or BLE disconnect.

The decision is pure and host-tested; the loop only samples the flag mirror, cache age and
connection state.

**Exposed fields.** `tele` in `/status` is emitted only while the BLE link is up (MQTT reads the
caches directly and keeps publishing):

| Domain | Fields |
|---|---|
| `climate` | inside/outside/setpoint °C, `on`, `preconditioning`; Cabin-Overheat-Protection `cop`/`cop_cooling`/`cop_temp`/`cop_reason`; defrost `front_defrost`/`rear_defrost`/`defrost_mode` (separate from `is_climate_on`) |
| `drive` | `shift`, `odometer_km` |
| `tires` | `fl`/`fr`/`rl`/`rr` (bar), `warn` |
| `closures` | `locked`, `door`/`frunk`/`trunk`/`window` open, `user` (occupant) |

The web UI renders Overheat/Defrost chips from `tele.climate` (only with a live AC draw; the car is
never woken to populate them); the rest of `tele` is for HA and diagnostics. Numeric fields appear
only when the car reported them (proto3 optional), so consumers show "unknown", not a phantom 0.

## OTA self-update

Pull-based: the device fetches `manifest.json` from a fixed HTTPS URL
(`CONFIG_TESLA_OTA_MANIFEST_URL`, default GitHub Pages), compares its `version` with the running
firmware and, on confirmation, downloads *its* per-target image
(`CONFIG_TESLA_OTA_FIRMWARE_BASE_URL` + `tesla-key-esp32<suffix>.bin`; suffix `""`/`-s3`/`-c3`/`-c6`
for esp32/esp32s3/esp32c3/esp32c6, chosen at compile time by `TESLA_OTA_IMG_SUFFIX`) via
`esp_https_ota` into the inactive slot, then reboots. `esp_https_ota` verifies the chip id, so a
wrong-target image is refused; one manifest `version` covers all targets. Triggered from the
Firmware row (or the Safe Mode banner); implemented in `main/ota_update.cpp`.

**Manifest intake is a bounded, exact protocol.** The HTTPS body is capped at 8192 bytes. A
non-chunked response needs a positive `Content-Length` within the cap and must deliver exactly that
many bytes; a chunked one may omit it but needs the transport's complete-data signal and must be
non-empty and within the cap. Truncation, a negative/early-zero read or a lying length fails closed.
One bounded contiguous block is reserved up front rather than growing while TLS is live. Before
cJSON allocation, the shared allocation-free syntax gate requires one fully consumed JSON document
(≤ 16 levels, valid UTF-8/escapes, no decoded U+0000); `cJSON_ParseWithLengthOpts` must consume the
exact body; the root must be an object with unique keys and exactly one string `version`. The
version is validated before copying against the canonical grammar (no leading-zero core, optional
`[0-9A-Za-z.-]+` suffix, ≤ 31 bytes); numeric cores compare as digit spans so a huge integer cannot
overflow. The body is released before the bounded copies, keeping the peak at body + cJSON.

**Downgrade gate.** Right after `esp_https_ota_begin` — before the bulk download — `ota_task` reads
the version from the image's own app descriptor (`esp_https_ota_get_img_desc`) and refuses anything
not strictly newer than the running firmware. A valid signature proves authenticity, not freshness,
and reading the *image's* version also defeats a host that advertises a new manifest version but
serves an old binary. No eFuses are burned.

**Rollback and health gate.** Rollback is enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`);
`main.cpp` defers `esp_ota_mark_app_valid_cancel_rollback()` to `ota_health_gate_task`, whose
verdict is the pure, host-tested `logic/health_gate.hpp`. Health is **a proven lease
(`tk::net_is_up()`, either transport), an INTERNAL largest block ≥ `kHeapCriticalBytes` (4 KiB) and
`kHealthGateBaseS` (90 s) uptime** — not uptime alone. An image that crashes/OOM-reboots under load
dies while `PENDING_VERIFY` and the bootloader reverts; so does one that boots fine but never gets
on the network, the case a later OTA could not repair because the fix would have to arrive over the
broken link. Past `kHealthGateCapS` (600 s) with a route but no lease the image is judged broken
and left `PENDING_VERIFY` for the next reboot to roll back; it deliberately does **not** restart
itself, which would turn a long router outage into a silent downgrade. The decision function
treats a device with neither credentials nor a wire as legitimately offline (`link_expected` false),
but the gate is only armed once `app_main` reaches its essential services: the setup portal never
returns, so an image that boots into setup mode is neither confirmed nor judged, and stays
`PENDING_VERIFY` for the next reboot (see *Early confirmation* below).

- *Early confirmation:* only a successfully persisted, user-requested rebooting save (`/set_mqtt`,
  `/set_syslog`, `/set_wifi`) may confirm inside the window, through
  `ota_confirm_pending_image(SuccessfulUserConfigCommit)`. The setup-portal save calls the same
  function, but every portal entry point runs before runtime admission and never reaches Ready, so
  the confirmation is always refused there and the reboot keeps rollback armed: a saved setup form
  is not evidence that a pending image is accepted.
- *Shared owner:* timed and explicit confirmation both acquire the `HealthCommit` owner against OTA,
  identity work and `FaultRestart`, then re-sample the INTERNAL largest block; a busy owner or
  critical sample leaves rollback armed.
- *Identity boundary:* `OtaIdentityMutationGuard` admits `/set_vin` and `/gen_keys[?force=1]` only
  while the image is already `Stable` and nothing else owns the gate; `PendingVerify`, unknown state
  or active OTA returns `503` before the VIN journal or key transaction starts.
- *Not health evidence:* automatic VIN/key recovery, WiFi rollback, the heap watchdog and
  brownouts leave `PENDING_VERIFY` armed so the reboot rolls a regressed image back. Confirming an
  already-valid image is a no-op.

**Signature.** Every OTA image carries an RSA-3072 Secure Boot v2 signature, verified by the running
app without hardware Secure Boot (TOFU trust anchor, no eFuses, `CONFIG_ESP32_REV_MIN_3` on the
classic esp32). Key lifecycle, rotation and the CI signing boundary: [`SECURITY.md`](SECURITY.md).

**Channels and PR previews.** Devices follow `Release` (default), `Dev` or a targeted PR preview:

- *Release* reads the root manifest and accepts only official candidates (no prerelease suffix),
  newer than the running version. Switching from `Dev` to `Release` may downgrade so a device can
  always return to production firmware.
- *Dev* reads `dev/manifest.json` and accepts `x.y.z-dev.N`; on the same core version the dev number
  must increase, and dev→dev downgrades are rejected.
- *PR previews* (`/ota/check?pr=<N>`, `/ota/update?pr=<N>`, or `?pr=<N>` on the web UI) read
  `PR/<N>/manifest.json`. A preview's version is `<latest-stable-release>-PR-<N>`, based on the
  newest complete immutable non-prerelease Release (never `next` or an RC): `tk::compare_ota_versions()`
  (`logic/ota_contract.hpp`) parses only `x.y.z` and ignores the suffix, so a later release compares
  strictly newer and the device OTAs forward, whereas a `next` base would collide with the number the
  merge cuts and stall OTA. The next unparameterized check returns to the selected channel once the
  candidate's core version is ≥ the running core.
- The channel is set from the web UI or `POST /set_ota {"channel":"dev"|"release"}` and reported as
  `/status` `ota.channel`. It is packed into reserved flag bits 2–3 of the v1 `ConfigBlob`
  (`has_ota`, `ota_channel`); writes always emit a v1 blob so older builds (e.g. v1.5.7) decode it
  without an unknown-version wipe to the setup AP. Unrelated saves preserve `has_ota=false`, so a dev
  build without an explicit choice still defaults to Dev.
- `GET /ota/changelog` returns `text/plain` notes (≤ 1024 B, `no-store`; `204` if none) for the
  offered update: `ota_check` fetches the sibling `changelog.json`, and
  `tk::ota_changelog_select_range()` keeps the range between running and offered version.

**PR preview installer.** A maintainer can opt a reviewed same-repository PR into a **signed** build
with the `signed-preview` label, so it can be browser-flashed and tried *before* merge; unlabelled and
fork PRs stay unsigned compile checks and publish no preview (an unsigned image crash-loops at boot).
The result is a full self-contained site at `https://0bu.github.io/tesla-key-esp32/PR/<N>/` on the
`gh-pages` branch — it detects that it lives under `/PR/<N>/`, shows a preview banner and flashes
that PR's own firmware. The **root** page has **no version picker**: it flashes the latest official
Release, and a PR's firmware is reached only through its own URL (or the link on the PR). Browser
flashing needs same-origin binaries because GitHub Release assets carry no CORS headers, and root,
`dev/` and durable `PR/<N>/` subpaths must coexist, which is why one branch-backed authority is used;
`scripts/publish-pages-branch.sh` syncs each part (a root sync preserves `dev/` and `PR/`). The
signing flow, Pages layout and cleanup are in
[`SECURITY.md`](SECURITY.md#release-pipeline-and-trust-boundaries).

**Partition layout, size gate and targets.** `partitions.csv` is dual-OTA (`otadata` +
`ota_0`/`ota_1`, `0x1f0000` each), sized to fill 4 MB so one table serves every target; the app is
at `0x20000`. The `ci-build-all.sh` **app-size gate** sits at `slot − 32 KB` (0x1e8000): code rounds
up to a 64 KB Secure-Boot boundary plus a 4 KB signature. Per-target baselines (schema v2) cap the
raw app, ELF total, flash code + rodata, static memory, `.bss` and IRAM; that review baseline is
separate from the projected-signed hard gate, and the generated size report — not a number in this
prose — is the source for current headroom. In particular, the ESP32-C6 target operates near the 64 KB
quantization threshold (`C6_64K_CLIFF` at 1,966,080 bytes / `0x1E0000` in `scripts/check-firmware-size.sh`):
exceeding this cliff by even a single byte triggers a 64 KiB quantization step that immediately violates
both the `0x1E8000` policy limit and partition boundary, requiring strict review of C6 size deltas. Design levers behind the fit:

- The firmware is a TLS **client** only (OTA, MQTTS); its server is plain LAN HTTP, so
  `CONFIG_MBEDTLS_TLS_CLIENT_ONLY=y` drops the unused TLS-server state machine and keeps C6 below the
  next 64 KiB signing boundary without weakening certificate verification.
- Every target builds at `-Og` (`-Os` hard-freezes under load and was rejected), with one
  component-private exception: Mbed TLS / TF-PSA-Crypto at `-Os`
  (`CONFIG_MBEDTLS_COMPILER_OPTIMIZATION_SIZE`), because at `-Og` esp32c6 reaches a projected signed
  `0x1f1000`, past the slot. `sdkconfig.defaults` pins it and `scripts/check-build-semantics.py`
  enforces it.
- One source tree builds esp32/esp32s3/esp32c3/esp32c6 with config-only deltas
  (`sdkconfig.defaults.<target>`): the classic ESP32 carries the chip-rev floor, S3 the display,
  S3/C3/C6 native USB-Serial/JTAG console (the classic esp32 falls back to UART0).
- **Migration:** a device on the old single-`factory` layout needs one USB reflash via the web
  installer (full erase, re-pair); afterwards all updates are OTA and preserve NVS. Existing 8 MB-table
  S3 devices keep OTA-updating, since OTA follows the *installed* table and `ota_0` stays at `0x20000`.

The Web Serial installer's data contract and the full release/Pages pipeline are in
[`SECURITY.md`](SECURITY.md#ota-self-update); the per-mechanism build gates (reproducibility,
effective-build closure, stack-frame inventory, environment rejection) are catalogued in
[`FEATURES.md` §6](FEATURES.md#6-build-test-and-ci).

### Flashing & NVS safety

Flash writes and NVS operations operate under strict boundary controls to protect pairing keys, WiFi credentials, and device state:

- **Normal updates:** OTA (`POST /ota/update`) writes the inactive application slot selected from the installed partition table and updates `otadata`. A verified USB application update writes the signed app to `ota_0` at `0x20000`, then erases `otadata` to activate it. Both preserve the installed bootloader, partition table and NVS at `0x9000` (`0x6000` bytes), including pairing keys, VIN and network settings. Neither path performs a partition migration.
- **Whole-chip erase recovery (`erase_flash`):** Running `esptool erase_flash` completely clears flash memory, destroying the bootloader, partition table, active firmware, otadata, and NVS. Restoring an operable device requires an **initial full flash** containing all four components from an official signed Release:
  1. `bootloader.bin` at offset `0x1000` (ESP32) or `0x0` (ESP32-S3, ESP32-C3, ESP32-C6)
  2. `partition-table.bin` at offset `0x8000`
  3. `ota_data_initial.bin` at offset `0xF000`
  4. `tesla-key-esp32.bin` (signed app binary) at offset `0x20000` (`ota_0`)
  The official browser-based Web Serial installer obtains these four parts from the release-bound Pages manifest and validates provenance, target family and bytes before writing. GitHub Releases publish signed app and merged-image assets, not separate bootloader, partition-table and initial-otadata files. Manual full recovery therefore needs a separately authorized complete write set and a verified source for every part; do not substitute an app-only recovery or unsigned local build. A merged image is an initial-install artifact that can overwrite the NVS range and requires explicit authorization for that loss. After an intentional whole-chip erase, the device starts unconfigured and requires network setup and fresh vehicle-key enrollment. See [SECURITY.md](SECURITY.md#ota-self-update) for the installer and artifact trust contract.

## Pinned tesla-ble and native orchestration

The supported chips are exactly what `yoziru/tesla-ble` declares in its `idf_component.yml`
`targets:`, and the Component Manager **enforces** that list at dependency resolution. That is the
definition of "supported": no local checkout of a third-party dependency has to be kept in sync, and
adding a chip it omits (esp32c5, esp32c61) means upstreaming it first
([ADR-0004](adr/0004-drop-esp32c5-target.md)). The root `CMakeLists.txt` applies every
`patches/tesla-ble/NNNN-*.patch` in lexical order through `scripts/apply-tesla-ble-patches.sh`;
a per-materialisation hash marker makes repeated CMake passes idempotent and lets a later patch
be added, while a changed or removed applied patch fails closed. All four images use the same
tesla-ble revision (v5.2.0) and patch series.

Since [ADR-0005](adr/0005-tesla-ble-seam.md) the firmware bypasses the monolithic
`TeslaBLE::Vehicle` and uses `TeslaBLE::Client` directly, with hardware-free orchestration in
`main/logic/`. That retired patches 0001–0003 (and superseded
[ADR-0003](adr/0003-reject-replayed-tesla-responses.md)):

- **RX/TX framing** (`rx_framing.hpp`): strict 2-byte big-endian length prefix as in
  `teslamotors/vehicle-command` `ble.go`, bounded buffer, inter-chunk timeout. TX writes the
  builder output unchanged (`tk::build_ble_tx_frame`) and refuses a malformed frame
  (`tk::is_well_formed_ble_frame`).
- **Dispatcher and session state** (`ble_dispatcher.hpp`, `session_state.hpp`): separate VCSEC and
  Infotainment trackers mirroring the library `Peer`'s counter validation and epoch, strict
  request-UUID matching.
- **Key regeneration** (`key_rotation.hpp` `tk::regenerate_private_key()`, called from
  `vehicle_pairing.cpp`): transactional generation with a 2048 B PEM export and rollback on NVS
  failure. `tesla_ble/key_rotate` stays armed across power loss and blocks construction/signing until
  cleanup is durable (an NVS probe error blocks too). An interrupted rotation also retires
  `key_created` before clearing the journal: its timestamp cannot be proven to belong to the
  current key, even if the new date may already have been stamped. Both erase sequences are host-tested
  against the real NVS adapter (`tk::run_key_rotation_boot_cleanup`, `tk::retire_key_rotation_journal`
  in `logic/key_rotation.hpp`). `tesla_cfg/vin_txn` journals VIN transitions,
  with the recovery decision in `logic/vin_transition.hpp`, so power loss cannot combine a new VIN
  with the old key/session.
- **Command FIFO and runner** (`command_runner.hpp`): bounded (8 slots) arbitration of VCSEC auth →
  wake → infotainment auth → payload, with exponential backoff and nominal `already_set` handling.

The remaining series is three patches, hash-checked and fail-closed:

| Patch | Purpose |
|---|---|
| `0004-drop-unused-parental-controls-actions` | Drops the five Parental Controls arms v5.1.2 added to the `CarServer_VehicleAction` oneof, plus their nanopb descriptors. A referenced oneof arm keeps its descriptors out of `--gc-sections`, so they cost flash in every image; removal returns `car_server.pb.c` to the v5.1.1 descriptor size. A size patch, not a correctness one: esp32c6 sits closest to the ceiling and images quantize to 64 KiB. |
| `0005-align-session-counter-replay-with-signer-go` | In `src/peer.cpp`, on a lower vehicle-reported counter keep `max(local, reported)` and still apply epoch/time (as `signer.go` `UpdateSessionInfo`), instead of hard-rejecting or forcing the lower counter, which would break anti-replay monotonicity. Removing that call site lets `--gc-sections` drop the dead `force_update_session`. |
| `0006-port-crypto-bindings-to-psa` | Moves the crypto bindings from the Mbed TLS 3 API (removed in ESP-IDF 6's Mbed TLS 4) to PSA: P-256 keys as PSA keys, `psa_raw_key_agreement`, `psa_aead_*`, `psa_hash_compute`, `psa_mac_compute`, `psa_generate_random`; `mbedtls_pk` still parses/writes the NVS PEM. Wire bytes, session derivation and key format are unchanged, proven by the V1 vectors in `test/test_tesla_ble_harness.cpp` ([ADR-0002](adr/0002-idf6-mbedtls4-crypto-seam.md)). One behavior changes: the AES-GCM tag of a vehicle response is now *verified* (upstream computed it and never compared), and response metadata carries the counter the response sends, as `Signer.Decrypt` does. A response failing authentication is dropped (ADR-0005 §2). |

## On-device display and status LED

**ST7735 display (LilyGO T-Dongle-S3).** `main/display.cpp` drives the dongle's 0.96" panel as a
status view (WiFi/BLE header + SoC battery, or a search/"Pairing…" animation); cache-only, it never
wakes the car. Each BOOT-button tap rotates 90° through landscape (160×80) → portrait (80×160, a
vertical battery filling bottom→top) → their 180° flips, using MADCTL `{0xC8,0xA8,0x08,0x68}` over
the same framebuffer with col/row offsets swapping 1/26↔26/1; the index persists in NVS
`tesla_cfg/disp_rot` (migrated from the `disp_flip` bool). The landscape MADCTLs (0xA8/0x68) and
(1,26) offsets are hardware-verified; the portrait ones follow the standard ST7735 rotation set and
want an on-device confirm (a wrong 90° direction is a one-line `+1`→`+3` flip in `rotate_90()`).
The backlight is active-low.

*What* to show — the priority ladder (WiFi-search > pairing > BLE-search > battery), SoC gradient,
RSSI→bars mapping and SSID-scroll offset — is decided by the pure, host-tested presenter
`main/logic/display_model.hpp` (its `Orient` axis picks per-layout geometry) from the IDF-free
`UiSnapshot` (`logic/ui_state.hpp`, assembled once under the cache lock by
`VehicleController::ui_snapshot()`); `display.cpp` only draws the resulting `Model`
(`draw_landscape`/`draw_portrait`). The layout mirrors `tools/display_sim.py`, the pixel-exact
offline renderer, and `scripts/check-display-sim-parity.sh` (run by `run-mock-tests.sh`) diffs the
sim's `decide()` against golden vectors the C++ presenter emits, so drift fails the `logic-test` gate.

Hardware: framebuffer in ~25 KB internal SRAM (no PSRAM), SPI 40 MHz, BOOT on GPIO0, compiled via
`sdkconfig.defaults.esp32s3` and a no-op stub on other targets. Because the one esp32s3 image also
runs on generic ESP32-S3 boards, `display_start()` first **auto-detects the T-Dongle-S3** by its
TF-card socket's external pull-ups (≥ 4 of 6 GPIOs HIGH); otherwise it is a complete no-op (no
framebuffer, no GPIO driven).

**Status LED (APA102 on the T-Dongle underside).** `main/led_status.cpp` is an independent
indicator: WiFi/BLE search (breathing), pairing (pulse), charging (green swell), dimmed SoC colour
when parked, blue for OTA, amber/red for warnings/errors. Its priority ladder is the host-tested
`logic/led_status.hpp`, reading the **same `UiSnapshot`** as the panel (so panel, LED, web hero and
MQTT never disagree) plus a small `LedAlerts` that holds a transient fault visible 10–15 s. The SoC
colour comes from the shared `logic/soc_gradient.hpp`. Cache-only, needs no MQTT or panel, ~12-byte
bit-banged frame, no heap. Opt-in: a stub unless `CONFIG_TESLA_LED_ENABLED` (default off; a
T-Dongle-S3 wires DI=40/CI=39).

## Home Assistant MQTT bridge

`main/mqtt_ha.cpp` publishes all cached telemetry and device status to an MQTT broker using Home
Assistant's MQTT Discovery, so every entity appears under one device. **Read-only by design** — no
command topics are subscribed, so HA can neither control nor wake the car — and independent of
evcc, BLE and pairing. Config, topics and payload fields:
[`README.md`](README.md#home-assistant-mqtt).

- **Config.** The broker URI comes from NVS `mqtt_uri` (web UI stores `host:port`), overriding
  `CONFIG_TESLA_MQTT_BROKER_URI`; empty disables the bridge. `/set_mqtt` reboots to re-init.
- **Transport / TLS.** A schemeless entry defaults to plaintext `mqtt://` **unless credentials are
  present** (a configured username or `user:pass@host`), then to **`mqtts://`**, so the password is
  not sent in the clear to an off-LAN broker. `mqtts://` verifies the broker certificate against the
  bundled CA roots (as OTA does); an untrusted certificate fails the handshake and the bridge stays
  disconnected — **no silent fallback to plaintext**. The reason surfaces in `/status` `mqtt.error`
  (and `mqtt.tls`). An explicit scheme is always honored.
- **Node id** `teslakey_<vin>` from the lowercase validated VIN: topics, entity `unique_id`s and the
  HA device identifier survive replacing the ESP32 board; changing the vehicle intentionally creates
  a new HA device. The physical eFuse MAC stays visible as `sys.board_mac` for triage.
- **Registry.** `logic/mqtt_discovery_registry.hpp` holds exactly **55** entity rows and is the
  only source for component, object id, state domain, JSON field/type, template inversion and HA
  metadata. Domains map to `<base>/<node>/{charge,climate,drive,tires,closures,vehicle,device}`
  (retained JSON), availability/LWT is `<base>/<node>/availability`, discovery configs are
  `<prefix>/<sensor|binary_sensor>/<node>/<object>/config` (retained); production derives topic,
  `unique_id`, state topic and template from the row.
- **Presence and units.** Optional car-sourced numbers/booleans are emitted only when reported, so
  an unseen value reads "unknown", not 0/OFF; every binary template has the same presence guard
  and only `locked` inverts ON/OFF for HA's `lock` class. `safe_mode`/`crash_dump` are real JSON
  booleans (a non-empty `"OFF"` string would evaluate truthy). The warn and
  door/frunk/trunk/window aggregates fold per-wheel/per-opening booleans with present-AND-true
  semantics. Tesla reports range/rate/odometer imperial and the bridge converts to km, km/h; only the
  Tesla-compatible `/api` path keeps miles (evcc). Task-stack minima and `usable_soc` ride in the
  retained payloads without discovery rows, so they create no entities or duplicate battery sensors.
  *last boot* is an ISO-8601 `timestamp` (HA renders "x minutes ago"), latched once the wall clock
  is authoritative (NTP sync or explicit browser `/set_time` via `clock_is_authoritative()`; an
  authoritative NTP sync takes precedence and upgrades an earlier browser-set latch).
- **Publishing.** The `mqtt_pub` task reads the thread-safe caches; on every (re)connect it resends
  discovery, `online` and a snapshot, then republishes every interval. The source polls' active-window
  gating still lets the car sleep, so MQTT keeps serving the last-known retained values while a
  domain's cache is valid. When a domain's cache is not valid (pairing reset, invalidation, nothing
  heard since boot) the task overwrites its retained topic once with the empty object `{}` and then
  stays quiet (`logic/mqtt_state_lifecycle.hpp`), so stale readings cannot resurface in HA after a
  reconnect. Every discovery `value_template` renders an absent or null field as the literal `None`
  (`logic/ha_templates.hpp`): HA ignores an empty rendering and keeps the previous state, and only
  `None` moves an entity to unknown, so `{}` drives the whole domain to unknown. Every
  payload is built by the `mqtt_payloads.hpp` emitters that the pinned-cJSON host matrix executes and
  is completed through the sticky owner before the publish seam runs, so a build/print failure
  publishes nothing and cannot replace a good retained document with a partial one. Discovery →
  availability → state short-circuits, and any build, print or broker failure re-arms the whole
  sequence. The 60 s task watchdog is fed at the loop boundary and after each completed publish, so
  a progressing discovery burst cannot trip it while a publish that never returns still does.

## Syslog forwarder

`main/syslog.cpp` forwards the in-RAM diag log (`GET /diag`, `main/diag_log.cpp`) to a UDP Syslog
collector, best-effort, framed as RFC 5424 (`<PRI>1 - tesla-key-esp32 - - - - <message>`). The one
capture point is `diag_log.cpp`'s `esp_log_set_vprintf` hook, which already mirrors every `ESP_LOG*`
line (this firmware, ESP-IDF and NimBLE, the latter pre-throttled to `WARN`) into the ring and now
also queues it for Syslog. Enabling and operator behavior: [`README.md`](README.md#syslog).

`diag_log.cpp` copies each bounded line into the ring and snapshots the sink while holding the ring
mutex, then releases it before invoking the sink; the sink may allocate, throw or do network work,
and keeping it outside the lock stops an OOM/unwind path or slow forwarder from wedging `/diag` and
every logging producer. Chunked `/diag` readers bind their snapshot to an append count and clear
epoch and stop before mixing two generations — essential for `?redact=1`, where a new prefix joined
to an old sensitive tail could evade line-marker redaction. A wrapped ring's partial first line is
discarded, and a logical line over the 288-byte frame is emitted only as `<redacted>`, so a marker
and its value can never be split across two independently redacted fragments.

- **Severity** (`tk::syslog_pri_for_line`): facility `user` (1); severity from the line's esp_log
  level (`E`/`W`/`I`/`D`|`V` → 3/4/6/7), skipping a leading ANSI colour escape; a line without a
  recognised `"<L> ("` prefix stays `info` (tesla-ble and NimBLE lines have none, and inventing a
  severity would be a false alarm). It was once a hardcoded `<14>`, so a week of logs arrived
  uniformly as `info` — including 61,417 `ESP_LOGE` and 81,708 `ESP_LOGW` lines — and
  `severity:error` matched nothing.
- **Config.** One NVS string `syslog_uri` (`tesla_cfg`), bare `host:port` (a bare host defaults to
  514; `""` disables), falling back to `CONFIG_TESLA_SYSLOG_SERVER`. Set via the web UI
  (`POST /set_syslog`). Resolved **once** at `syslog_start()` (early in `app_main`, before WiFi);
  like MQTT, a change persists then reboots.
- **Delivery.** A background task drains a fixed 24-deep queue of 256-byte messages (small on
  purpose: one contiguous allocation on a device limited by largest free block) and, once the
  network is up, resolves the target with `getaddrinfo()`. Re-resolve + re-probe is throttled to
  ~10 s (`have_checked`, not `!resolved`), so a failing DNS never becomes a per-loop storm. Delivery
  gates on **DNS resolution only**.
- **Startup.** `logic/syslog_start_gate.hpp` handles FreeRTOS scheduling the consumer before
  `xTaskCreate()` returns: the task waits while its queue is unpublished, only a successful create
  commits the gate and publishes the process-lifetime queue (release/acquire), and a create failure
  cancels the waiter before deleting the unpublished queue. Once published the log hook may hold the
  queue at any time, so it is never deleted — optional forwarding degrades instead of creating a
  hook-versus-delete use-after-free.
- **Reachability probe (advisory, never a gate).** ARP for an on-subnet host (works when the
  collector firewalls ICMP), else two ICMP echoes (800 ms each); shown as `/status.syslog.reachable`.
- **Send failures** (`logic/syslog_policy.hpp`): `sendto()`/`socket()` errnos are HARD
  (`ENETUNREACH`, `EHOSTUNREACH`, `ENETDOWN`, `EHOSTDOWN`, `EADDRNOTAVAIL` → re-resolve + re-probe
  now) or TRANSIENT (all else incl. `ENOMEM`/`ENOBUFS`/`EAGAIN` → hold, ordinary cadence). Clearing
  the throttle on every failure would turn a chatty BLE poll into a `getaddrinfo()`+ping storm that
  runs hardest when the link is worst; the handler also logs only the failing/recovering
  *transition*, never per line. This mirrors an equivalent module in the sibling
  `daikin-altherma-esp32` project, where that exact storm was diagnosed on a live board.
- **Loop guard.** `syslog_send()` drops any line containing the `"syslog:"` tag its own
  `ESP_LOGx` calls render, so its "send failed" diagnostics cannot feed the storm above.
- **Status.** `syslog_status()` → `/status.syslog` (`configured`/`resolved`/`reachable`/`host`/
  `port`/`error`), read by the web UI like the MQTT row.

## Heap-exhaustion watchdog (last-resort escalation)

Every OOM guard turns "out of memory" into **recover and continue** — `handle_all` answers 503, BLE
parse guards reset the link, an MQTT publish is skipped — right for a *transient* shortage. The
watchdog answers the next question: *what if it never recovers?* A wedge is the worst failure
shape: a crash reboots in seconds, a hang looks powered-off, never heals and reports nothing. The
motivating incident was a non-reading WebSocket subscriber that left the device at `free=14820`,
`largest_block=768` (healthy: 31744) for ten hours, with `vehicle_->loop()` throwing `bad_alloc` ~20×/s
until the WiFi watchdog itself could no longer allocate its task (that feed has since been replaced
by polling).

The escalation is the pure, host-tested `main/logic/heap_watchdog.hpp`, sampled by `loop_task_fn_`
at the existing 30 s heap-log site:

- **Trigger:** `largest_block` below `kHeapCriticalBytes` (4 KB) *continuously* for
  `kHeapCriticalHoldMs` (5 min). Healthy is 31744 B and the wedge sat at 480–1536 B.
- **`largest_block`, never `free`** — the wedge held a plausible ~16 KB free the whole time — and
  **`MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL`**, since `heap_caps_*` reports the max across every
  heap carrying a cap and a board that registers PSRAM into `8BIT` (`CONFIG_SPIRAM_USE_MALLOC`) would read ~7.8 MB in the same
  wedge and never trigger. The `HEAP` log line carries `internal_largest=` next to the historical
  `8BIT` figures (identical on the four PSRAM-less targets).
- **An unbroken run, never a single `bad_alloc`.** One recovered sample resets the clock — this keeps
  the watchdog from becoming a reboot loop, which matters because each boot re-opens the active
  polling window and keeps a parked car awake.
- **Excused during OTA.** `esp_https_ota` makes the firmware's largest allocations, and a restart
  mid-install is the one reboot that could leave a half-written slot. An OTA **clears** the run
  (skipping would let a pre-download run resume its clock and fire mid-install). It queries
  `ota_is_busy()` (one atomic), never `ota_get_status()` (copies `std::string`s, can throw).
- **On fire:** persist `reboot_why=heap:<n>` to NVS `tesla_cfg`; **only a successful durable write
  authorizes the reboot**, so an unrecorded restart can never erase the evidence/cap. Then
  `ESP_LOGE`, a **300 ms `vTaskDelay`**, `esp_restart()`. The delay is not cosmetic: `syslog_send()`
  only queues, its task runs at priority 3 against `loop_task`'s 5, and without a yield the final
  message dies in the queue. It deliberately does **not** confirm a `PENDING_VERIFY` image, so a
  heap regression rolls the new image back.
- **Bounded:** after `kHeapMaxConsecutiveRestarts` (5) consecutive restarts it stops restarting and
  says so once — five cycles prove a restart is not fixing it. A boot that followed a watchdog
  restart **does not seed the active polling window** (`vehicle_ctrl.cpp` `init()`), which is what
  would make a restart loop expensive for a parked car. Only an exact NVS `NOT_FOUND` is an ordinary
  boot; a breadcrumb is consumed with an erase, and a read/erase failure or malformed value closes
  the ladder at the cap and suppresses the post-restart window instead of being read as zero.
- **Afterwards:** `main.cpp` takes the breadcrumb once at boot and `/status` reports it as
  **`last_reboot`** (absent on ordinary boots). `esp_reset_reason()` cannot tell `esp_restart()`
  from a power cycle (both SW/POWERON), so without it a device that self-heals at 04:00 leaves no
  trace.

The critical-path `"heap:<n>"` formatter uses a fixed buffer. The NVS write can still fail or
allocate, and the boot-time read builds a `std::string`; errors and exceptions there are contained
and a persistence failure blocks `esp_restart()` — an uncontained throw or unrecorded reboot in
this code would turn a wedge into an opaque reboot loop.

### Why a restart, and not in-place recovery

Rebooting is the crude answer, adopted only after the alternatives were researched: ESP-IDF offers no
reliable way to reclaim a wedged heap from inside the running image, and the teardown paths one would
call are among the least reliable code in the SDK.

- **Subsystem teardown/reinit** (`httpd_stop`/`httpd_start`, `esp_mqtt_client_destroy`,
  `esp_wifi_deinit`, `nimble_port_deinit`) has the worst record. `nimble_port_deinit` leaked twice in a
  row ([esp-idf#8136](https://github.com/espressif/esp-idf/issues/8136)); `esp_wifi_deinit` leaked per
  cycle on IDF 5.0/5.1 ([#12014](https://github.com/espressif/esp-idf/issues/12014)) and was
  crash-prone in the 3.x era ([#2050](https://github.com/espressif/esp-idf/issues/2050)); `httpd_stop()`
  leaked ~16.7 KB per cycle on 5.3 because it never released the with-caps task stacks, and the first
  upstream fix crashed on an assert ([#14266](https://github.com/espressif/esp-idf/issues/14266)).
  Whether deinit returns its memory depends on the exact IDF revision. Worse, deinit paths
  **allocate** while allocation is failing, so a throw unwinds into the net-less loop task and
  `abort()`s — an uncontrolled restart *without* the breadcrumb. The crash-only literature says the
  same: a restart is trustworthy only when implemented outside the failing component, and rarely
  exercised cleanup paths are unreliable *because* they are rarely exercised
  ([Candea & Fox, HotOS-IX](https://www.usenix.org/legacy/events/hotos03/tech/full_papers/candea/candea.pdf)).
- **A ballast block** freed under pressure is a real pattern (libstdc++ ships an emergency pool so
  `bad_alloc` can still be thrown), but it costs permanent internal DRAM out of a ~31 KB largest
  block, and this restart path needs no headroom.
- **`heap_caps_register_failed_alloc_callback()`** runs synchronously inside the allocator in an
  arbitrary task/ISR context, cannot satisfy the allocation and has no documented safety rules — a
  sensor, not an actor, already covered by the 30 s sample. Espressif's own built-in escalation
  (`CONFIG_HEAP_ABORT_WHEN_ALLOCATION_FAILS`) is a bare `esp_system_abort()`.
- **Defragmentation** does not exist (part of the TLSF allocator lives in ROM), and split-heap was
  judged of marginal value once WiFi and BLE coexist
  ([micropython#8940](https://github.com/micropython/micropython/issues/8940)). This project already
  contains what it can (static `/diag` ring, gzipped web UI, once-at-boot display framebuffer), but
  that is prevention, not a cure for the morning after.
- **Shipping firmware** restarts in bounded fashion instead of healing a live heap: ESPHome's
  [safe mode](https://esphome.io/components/safe_mode/) counts boot failures and escalates, and
  [Tasmota](https://tasmota.github.io/docs/Device-Recovery/) counts restarts up to a settings wipe.

So the reboot is the *last* rung, made defensible by the surrounding hygiene: a **long unbroken
trigger**, a **restart cap** (Candea & Fox prescribe a maximum retry limit to prevent reboot cycles)
and a **breadcrumb persisted before the reset**, which Memfault's watchdog guidance names as what
separates a diagnosable reset from a mystery. A *load-shedding rung* (stopping MQTT, closing idle
sockets before restarting) is the strongest candidate for a next step, but it only helps for leaks in
structures the firmware owns and is deferred until a second incident shows the pattern.

**Reading it in syslog.** Syslog is the only post-mortem source that survives the restart (the
`/diag` ring is RAM), so the escalation narrates itself:

| Line | Meaning |
|------|---------|
| `HEAP free=… largest_block=… min_free=… internal_largest=…` | ordinary 30 s trend line |
| `HEAP CRITICAL: … watchdog ARMED, restarting in 300 s unless it recovers` | countdown opened |
| `HEAP CRITICAL for <n> s … restarting in <m> s unless it recovers` | one line per sample — proof the shortage was sustained |
| `HEAP recovered after <n> s critical … watchdog disarmed` | run ended on its own |
| `HEAP critical run (<n> s) cleared: an OTA is in flight …` | run excused, not healed |
| `HEAP EXHAUSTED for <n> s … RESTARTING DELIBERATELY (watchdog restart <k>/5, reboot_why=heap:<k>; …)` | the restart, with its cause |
| `HEAP EXHAUSTED … but <n> consecutive watchdog restarts have not fixed it — NOT restarting again` | cap held; device stays up degraded |
| `BOOT this boot was caused by the firmware itself: reason=heap:<k> …` | logged on the *next* boot |

Elapsed times are the **measured** age of the run (sampled every 30 s), so a fired run reads
slightly over 300 s. **Keep any line on this path under ~230 characters:** the capture hook formats
into a 256-byte *stack* buffer (it must not allocate on the heap it reports about), and longer lines
reach `/diag` and Syslog cut mid-sentence. Both `BOOT` lines are emitted **after** `syslog_start()`
on purpose — before it `syslog_send()` is a no-op and anything logged only reaches the serial
console and the `/diag` ring, which the restart erases. `BOOT reset_reason=…` is *sampled* at the top
of `app_main` (before NVS, WiFi and `start()`s allocate) so its heap figures describe the boot we
came up in. Leaving that line above `syslog_start()` once meant a week with 56 boots and zero
received `BOOT reset_reason=` lines — a 2 h outage with no way to tell a panic from a brownout from a
deliberate restart.

## WiFi / LAN connectivity

Everything is in `main/net.cpp` behind the transport seam `main/net.hpp`. The HTTP server, MQTT,
Syslog, mDNS, SNTP, OTA, display and LED ask `tk::net_is_up()` / `tk::net_kind()` /
`tk::net_active_netif()` and never touch `esp_wifi`. (Before the seam a `wifi_is_connected()`
predicate was hand-declared `extern` in five files and `"WIFI_STA_DEF"` hardcoded in three — each
correct only while WiFi was the sole transport.) Transport identity (`tk::NetLink::{None,Wifi,Eth}`)
and the watchdog's decision logic are the host-tested `logic/net_link.hpp`.

### Which transport comes up

Boot order is **wire first, radio second**, because of the radio: WiFi and BLE share ONE antenna
path on every supported chip, so a *running* WiFi stack means time-division coexistence with NimBLE
(forcing `WIFI_PS_MIN_MODEM` and slowing every GATT round-trip). Coming up on Ethernet avoids
*starting* WiFi: no coexistence arbitration, and the ~57 KB of largest block the stack holds stays
free.

1. `tk::net_eth_probe()` runs very early — **before** the setup-portal decision — over an ordered
   candidate table of hardware-verified pinouts (M5Stack ATOMIC PoE Base, Waveshare ESP32-S3-ETH,
   or Kconfig overrides), reading the W5500's `VERSIONR` (always 0x04; a floating MISO reads
   0x00/0xFF, so there is no realistic false positive). Custom GPIOs keep their full Kconfig integer
   width through validation; nonexistent, reserved, strapping, flash/PSRAM and USB pins are rejected
   before the SPI driver can drive them. A failing candidate tears down its SPI bus before the next;
   with no answer at all the bus is freed and GPIOs are left as found.
2. A wired board with **no stored SSID does not enter the setup portal**: DHCP already gives it an
   address, and a captive AP would strand a reachable device. The VIN is set over the LAN.
3. `tk::net_start_eth()` waits for a lease, and the deadline means two things. With **no PHY link**
   it falls back to WiFi after `kEthLinkGraceMs` (4 s). With **link up but no lease** (a slow DHCP
   server) it waits in `CONFIG_TESLA_ETH_WAIT_S` intervals (20 s), up to `kEthLeaseLinkedCapFactor`
   (3) × that — falling back there would start WiFi for the whole boot on a board that is in fact
   wired. The driver keeps running either way, so a later cable takes over. **On the wired path
   `esp_wifi_init()` is never called** (`main.cpp` short-circuits `!on_wire && !net_start_wifi(...)`).
4. On the wired path the WiFi credential-rollback backup is **not** consumed: Ethernet proves nothing
   about credentials on trial, and spending them would discard the only way back to a working
   network when the cable is unplugged.

Every resource acquired after the positive probe belongs to one startup record until activation
commits; a failure unwinds in reverse dependency order (stop driver; unregister handlers; retract
globals; delete glue and netif; uninstall driver; delete PHY, MAC, event group; release SPI). If
driver uninstall fails its PHY/MAC/SPI tail is kept alive and boot fails closed — deleting objects
the driver can still reach would become dangling callbacks. A no-link/DHCP boot fallback differs: the
activated stack stays process-lifetime so a later cable takes over.

**Polling mode is deliberate.** The common transport models only SCLK/CS/MISO/MOSI, and the ATOMIC
PoE Base routes just those plus power (no INT, no RST). The driver therefore polls at
`CONFIG_TESLA_ETH_POLL_MS` (10 ms) and resets the PHY over SPI (the `MR` register); Waveshare
exposes INT/RST, but this shared path leaves them untouched, and any new candidate must audit every
SPI and auxiliary pin against earlier ones. Polling is a first-class driver mode: the
`espressif/w5500` MAC constructor (`esp_eth_mac_new_w5500`) accepts exactly one of an interrupt GPIO
or a poll period. The poll period bounds RX *latency*, not throughput (each poll drains the W5500's
16 KB buffer). ESP-IDF 6 moved the SPI Ethernet drivers to
`esp-eth-drivers`; `espressif/w5500` is commit-pinned in `main/idf_component.yml` and resolved for
esp32s3 only, so `CONFIG_TESLA_ETH_ENABLED` exists only there.

**The wire must win the default route — lwIP does not do it alone.** ESP-IDF defaults
`WIFI_STA_DEF` to `route_prio` 100 and `ETH_DEF` to 50, the opposite of this feature, so `net.cpp`
creates the Ethernet netif with `kEthRoutePrio` (128). Scope, verified in lwIP's `ip4_route()`:
*off-link* destinations (NTP, OTA, an off-subnet broker/collector) go to `netif_default`, which
`route_prio` selects; *on-link* destinations ignore priority and leave over the **first-registered**
netif whose subnet matches, i.e. the WiFi station when both share a `/24` (measured: on-link syslog
kept the WiFi source address while `/status.ip` showed Ethernet). That asymmetry is **accepted**:
overriding per-packet routing would only improve the runtime hot-plug case, which never delivers
this transport's benefit anyway because WiFi is already running. The benefit lives in the
boot-with-cable path, where WiFi never starts and no second netif exists.

**State is per transport and lease.** The watchdog retains one ICMP baseline per `NetLink`, bound
to the sampled lease generation and gateway IPv4 address. Every link-down and new-IP event advances
the generation, so even a different router using the same IPv4 address starts without a baseline.
The failure streak is reset when this identity changes, and an in-flight probe is discarded if its
lease or gateway changes before the verdict. The lease the watchdog ends itself counts too:
`net_recover()` re-associates (link-down, then link-up), so after a recovery the new lease must
answer ICMP once before another recovery is allowed. That is the price of never inheriting proof
across leases: a ghost association that re-forms before its first reply is not recovered a second
time (the watchdog keeps logging that the gateway never answered). Both
transports can hold a lease at once (WiFi fallback plus a later cable), so `tk::net_kind()` is
*derived* from two lease flags by the host-tested `tk::net_link_active()` (Ethernet outranks WiFi:
it costs the BLE radio nothing and is what lwIP puts first) rather than written by the last event.
Unplugging the cable must fall back to WiFi, not "no network", or Syslog stops, the display shows
"searching" and MQTT drops RSSI despite a healthy WiFi lease. WiFi-only readings
(`net_wifi_signal`, `net_wifi_standard`) gate on the WiFi *lease*, the window in which
`esp_wifi_sta_get_ap_info()` is safe. The W5500 has no MAC (no EEPROM), so one comes from
`ESP_MAC_ETH` (eFuse-derived, distinct from the WiFi STA MAC). The MQTT/HA node id derives from the
VIN, so switching transport or replacing the board cannot rename entities.

### Keeping the link up

- **Event-driven reconnect.** `wifi_event_handler` reconnects on every `WIFI_EVENT_STA_DISCONNECTED`.
  Boot keeps the original budget: if the device has **never** held an IP (`s_ever_up == false`) and
  the `MAX_RETRY` (10) fast attempts are spent, it sets `WIFI_FAIL_BIT` so `net_start_wifi()` times
  out into the **setup portal** (credentials presumed wrong). Once online at least once, later
  drops reconnect **forever** — the credentials are known-good and giving up would strand the device
  (the old code gave up after 10 in all cases, leaving a board reachable over BLE but off the LAN
  after a 3.5 h router outage).
- **Connectivity watchdog** (`net_watchdog_task`, ~30 s; verdict from `tk::watch_step()` in
  `logic/net_link.hpp`, so counting and the never-answered baseline rule are host-tested). It catches
  the **missed-deauth "ghost" association**: the stack holds an IP and emits TCP that times out but
  the AP forwards nothing and no disconnect event ever fires. It ICMP-echoes the **default gateway**
  only while the link believes it is up; after `tk::kWatchFailsToRecover` (2) consecutive failures
  (~60 s) it forces **one** `esp_wifi_disconnect()` and the endless-retry handler reconnects (the
  watchdog never calls `esp_wifi_connect()` itself, avoiding a cross-task double-connect). On a
  wired link the same verdict restarts the Ethernet MAC (`esp_eth_stop`/`esp_eth_start`), which
  re-runs auto-negotiation and DHCP — the wired ghost is a switch port reporting link while
  forwarding nothing. It never harms a healthy link: it acts **only** while the link believes it is
  up (a known-down link is the handler's job, and forcing a disconnect would churn the shared
  radio) and **only** if the gateway has answered ICMP **at least once** — a gateway that never
  replies is "ICMP not a usable signal", never "link dead", so it cannot cause a perpetual ~60 s
  re-association loop. The probe **fails open only on its own setup failure** (not initialised,
  unparseable gateway, `esp_ping` create error → "reachable"), whereas a missing lease/gateway or a
  gateway that does not answer counts as unreachable. It **never reboots**: a reboot during an AP
  outage would hit the 30 s boot timeout and drop into the setup portal, abandoning good credentials.
- **ICMP probe lifetime.** The probe's control block, callback arguments and semaphore are file-scope
  persistent, shared through `ping_probe_run` by the watchdog and the Syslog reachability check.
  Each session owns a monotonically increasing generation; on timeout the caller requests
  `esp_ping_stop` and waits a second bounded interval, and if the matching `on_ping_end` still has not
  arrived the handle stays quarantined as `PendingEnd` — no delete, new session or generation
  reuse until that exact generation ended. Stale/out-of-order callbacks cannot complete a
  replacement waiter, and create/start failures abandon a generation only while no callback-capable
  worker started. This prevents both a former stack use-after-free and a delete/reuse race with the
  asynchronous worker.

## Sleep / link-state (the single source of truth)

`VehicleController::link_state()` is the *single* source of truth shared by the web UI, `/status`
and MQTT (`sleep_status`), so they never drift. Four published values:

| Value | Meaning |
|---|---|
| `AWAKE` | Fresh live infotainment telemetry, < `kAwakeMaxAgeS` = 60 s. |
| `ASLEEP` | No live data **and proven, debounced** sleep: the car's own VCSEC sleep flag (`vcsec_sleep_state_`, sampled in `loop_task`) held uninterrupted `ASLEEP` ≥ `kAsleepDebounceS` ≈ 120 s while still reachable within `kReachableMaxAgeS`. `AWAKE` and `UNKNOWN` break the run, so a Cabin-Overheat-Protection flap or an unknown interval cannot accumulate into sleep proof. The clock is fed the reported `SleepState` itself (`tk::next_asleep_since`, `tk::vcsec_asleep_proven`, host-tested), so no call site can skip `UNKNOWN`. |
| `IDLE` | Reachable over BLE within `kReachableMaxAgeS` = 150 s but **not provably asleep** — infotainment polling was paused to let the car sleep and VCSEC has not confirmed; we never claim sleep. |
| `UNREACHABLE` | No signed BLE round-trip for ≥ 150 s (two ~30 s health-probe cycles plus headroom), or the car answers nothing: driven off, out of range or in deep sleep. |

Nothing heard since boot/re-pair ⇒ the value is omitted (HA shows "unknown"; strictly, state topics
are retained, so until the first post-reboot publish HA may still show the pre-reboot value).
**Asymmetry:** a debounced VCSEC `ASLEEP` counts as positive proof of sleep, but a VCSEC `AWAKE`
never claims `AWAKE` (a parked car reports VCSEC `AWAKE` while its infotainment sleeps — the old
`wake_up()` trap); `AWAKE` requires live infotainment telemetry, so a wrong VCSEC `AWAKE` can only
leave us in `IDLE`. The raw flag is surfaced as `vcsec_sleep` for diagnostics. Reachability is a
`last_reachable_ticks_` clock stamped on every successful signed round-trip, including the idle
health poll.

The web UI mirrors this exactly. The **"Vehicle asleep"** hero (with wake button) appears **only**
for a proven `ASLEEP`; `IDLE` shows a neutral **"Parked"** card (last-known SOC, idle time, same wake
button) with no sleep claim; `UNREACHABLE` *and* the unknown state keep the card but state the gap —
**"Vehicle unreachable"** or **"Checking status…"** over an empty gauge, with retained battery/idle
time only as labelled last-known chips and no action. In that state the BLE row drops its green,
ripples an amber wave across the signal bars and shows an amber dot and status line, flagging
"connected but stateless". A momentary "Disconnected" BLE row is normal (the link is dropped between
polls by design) and never drives the hero — only `link` does.

**BLE phase countdown ("7s left" / "retry in 22s").** `/status.ble` carries `phase` + `phase_s`
(both or neither), decided by the host-tested `main/logic/ble_phase.hpp` from two independently
armed deadlines:

- `connecting` — an attempt is running and gives up in `phase_s`; armed by `ensure_connected_`
  (`vehicle_commands.cpp`), the one place any attempt is started and bounded.
- `waiting` — the next attempt starts in `phase_s`; armed by `idle_until_next_health_poll_`
  (`vehicle_pairing.cpp`), which owns both the wait and its countdown from one constant, so the row
  cannot promise a retry at a time the loop does not retry.

They **overlap** routinely (a command or `loop_task`'s warm-up connect starts an attempt mid-wait):
`connecting` outranks `waiting`, and since neither clears the other the idle wait's countdown
reappears when the attempt ends. `phase_s` rounds **up** and `0` means "right now", never "no
countdown" — gating on `> 0` once dropped the last second and flashed a bare "Disconnected" between
cycles. `app.js` ticks the number down locally each second between the 4 s polls, resyncing only on
a phase change or a ≥ 2 s disagreement, and paints a dedicated `.cd` node with `textContent`
(rewriting through `setHTML` would restart the bar fill animation each tick). Each row's countdown
node names the one phase it renders, so "Searching…" is never suffixed with a retry countdown.

**The row state is decided by a presenter, not inline in the UI.** `main/logic/ble_row.hpp`
(`tk::ble::decide`) maps raw `/status` fields — deriving "has VIN" and "link known" itself, so no
untested adapter sits between JSON and verdict — to one of six row states plus its countdown;
`main/www/app.js` only renders it, and `scripts/check-ble-row-parity.sh` re-decides an exhaustive
input sweep with the JavaScript that actually ships (as `display_model.hpp` does with
`tools/display_sim.py`). **The label follows the phase, not `ble.scanning`**: `scanning` is not a field
of `RowInputs`, so a label driven off it is unrepresentable. The background warm-up scan has no
deadline and runs straight through the idle wait, so keying off it once flipped the row to
"Searching…" mid "retry in …". Only `phase === "connecting"` says "Searching…"; the one exception
with no countdown is the no-VIN listing-only scan, which has no schedule to count. The disconnected
row draws **outlined, unfilled** bars (an empty gauge), the searching row uses the same amber
(`--warn-base`) as "link up, nothing known", and Wi-Fi's own search stays green.

**Connection-failure detection ("Connection failed" hero).** When the target's advert is heard but the
link will not come up after repeated tries, `/status.ble` carries `connect_fail` (consecutive recent
failures; only while failing) and `car_connectable` (the advert's connectable flag).
`car_connectable=false` means the car advertises **non-connectable**, i.e. it is at its ~3-device
BLE limit — mirroring vehicle-command's `ErrMaxConnectionsExceeded` (the timeout itself carries no
reason). The hero says "too many Bluetooth devices connected" in that case, else "move closer /
disconnect other devices", in the setup flow *and* the paired state. Signal windows are 90 s to stay
stable across the ~30–40 s health-probe cadence. `ble.devices[]` also carries per-device
`connectable`, and the no-VIN screen lists nearby Teslas (bars · dBm · MAC) from the periodic
listing-only scan. Hero glyphs (muted grey): key/pencil "Set up needed", link "Pairing", Bluetooth
"Connection failed", bolt "Vehicle unreachable".

**The same verdict decides what a failed connect LOGS** (`logic/connect_outcome.hpp`).
`ensure_connected_()` once ended every failed attempt with `connection timeout after 10000ms` — wrong
twice: it claimed a timeout when the scan never matched the car (no connect attempted), and, since
the background health poll retries roughly every 40 s forever, a car parked elsewhere produced 7117
ERROR lines in a week for an expected, self-resolving state.

| `target_connectable()` | Cause | Background level |
|---|---|---|
| `-1` (no matching advert) | `OutOfRange` — car away/asleep | **warn** — the expected resting state |
| `0` (advert non-connectable) | `AtBleLimit` — at its ~3-device limit | **error** |
| `1` (connectable, connect failed) | `ConnectFailed` | **error** — the two-boards-on-one-car signature |

Rate limit: the first occurrence of a cause is logged, then once per monotonic hour
(`kConnectFailRepeatMs`) until the cause **changes** or a connect succeeds. It is time-based because
the unpaired auto-enrolment path can issue ten probes in a burst, and a change of cause is never
suppressed ("car came back but now the connect fails" is the transition worth seeing).
**Foreground attempts** (`ConnectOrigin::Foreground` — an evcc/MCP/user request is blocked on it) are
always ERROR and never suppressed. Classification needs both the target-name report and the primary
advert's connectability bit *observed since the attempt began*, so a fresh SCAN_RSP can never lend an
older/default bit a fresh timestamp (the separate 90 s `target_connectable()` history stays stable
for the UI but cannot make a stale sighting a current `ConnectFailed`/`AtBleLimit`). The nameless
primary usually precedes the named SCAN_RSP; a fixed allocation-free cache correlates them by
address, including a new/rotated address. Raw scan/GAP/GATT events are DEBUG-only, and the production
build's compile-time max level is INFO, so the classified command-layer line is the single production
WARN/ERROR signal and suppressed attempts print nothing (the verdict stays readable in `/status.ble`).
Command-completion timeouts use a separate typed policy: the automatic Whitelist Add Key is expected
not to complete and recovers its FIFO at DEBUG; the background GET_STATUS health probe warns first
and then hourly while unanswered; a timed-out HTTP/evcc/user request warns every time; any valid
signed status response closes a prior health-timeout run. The unpaired supervisor's three setup
reminders log at INFO on entry and hourly, DEBUG in between; a background enrolment losing the
command-mutex race is DEBUG, a blocked foreground pairing request stays WARN.

## Pairing lifecycle / invalidation

The web UI keys controls and SOC off `paired` (= `has_session()`, the stored VCSEC session in NVS).
Three events invalidate a pairing and force a clean re-pair
(`clear_session_and_cache_()` in `vehicle_pairing.cpp`):

1. **Key deleted on the car** — detected three ways, each setting `pairing_lost_`:
   (a) the *primary* detector, the `set_message_callback` observer in `vehicle_ctrl.cpp`, matches a
   signed-message fault (`UNKNOWN_KEY_ID`/`INACTIVE_KEY`/`INVALID_KEY_HANDLE`) — the path that fires
   on an already-established (cached) session, e.g. the background charge poll; (b) any reply
   containing `"whitelist"` (`KEY_NOT_ON_WHITELIST`, only during a session-info handshake) in
   `make_result_cb_`; (c) a two-strike `"authentication failed"` honoured **only** for the periodic
   signed VCSEC `health_probe_` (~30 s), so deletion is caught without evcc traffic while a
   role-denied user command can never trip it. On detection the key is regenerated (the old one is
   useless), session and cache cleared, and pairing restarts.
2. **Key regenerated** (`/gen_keys?force=1`): `generate_key()` also clears session + cache and
   drops the BLE link.
3. **VIN changed** (`/set_vin`): `reset_for_new_vehicle()` regenerates the key, clears session +
   cache, forgets the stored `ble_mac` and reboots. Re-saving the same VIN is a no-op.

Afterwards `has_session()` is false and the UI shows "not paired" and hides controls/SOC. Because the
50 ms vehicle loop samples `has_session()`, the storage adapter caches the first successful
`session_vcsec` existence probe and updates it on every successful save/remove; read errors stay
uncached and retry, so a transient fault never looks durably absent.

The main-task BLE-MAC string is startup-only input. When it is empty the NimBLE host posts a fixed
LinkUp record only; `vehicle_loop` marks the link connected on GATT discovery, acknowledges the exact
connection generation as command-ready, materializes the peer address after unlock and makes one
best-effort NVS persistence attempt outside every shared lock. It never mutates the startup
`std::string` across tasks.

**Session persistence and clock restore ordering.** `load_nvs_sessions_()` inherits upstream's age
check `(unix_now - session.clock_time)` (ADR-0005 §2). `session.clock_time` is vehicle epoch/uptime
(hundreds of thousands of seconds), not Unix time, so once the wall clock is real (~1.77 billion s)
the age far exceeds 3600 s and stored sessions are rejected; a 1970 clock gives a negative age and
would *keep* them. `main.cpp` therefore calls `restore_clock_from_nvs()` (the `last_time` cache
written on each NTP sync; it needs no network) **before** `VehicleController::init()`, so stale
sessions from an uninitialised clock are rejected fail-closed. True persistent session reuse across
reboots would need upstream alignment to track vehicle epoch separately from Unix time.

**Clock authority and durable timestamps.** A restored clock from the NVS `last_time` cache is
sufficient to reject stale sessions and validate OTA TLS certificates, but is not authoritative
because it reflects the time of the previous sync. Authoritative clock sources are SNTP
(`on_time_sync`) and explicit browser synchronization via `POST /set_time` (`apply_browser_clock`),
tracked by `clock_is_authoritative()`. Every durable wall-clock timestamp (`key_created`,
`paired_at`, and the MQTT `boot_time` latch in `mqtt_ha.cpp`) requires an authoritative clock;
an authoritative NTP sync takes precedence and upgrades any earlier browser-synchronized
`boot_time` latch.

**A configured VIN gates pairing entirely.** The device finds the car by its VIN-derived BLE name
(`S<hex>C`), so `auto_pair_task` first checks `has_plausible_vin()` (the same 17-char validator as the
web UI and `POST /set_vin`). With no VIN it logs once (`auto-pair: no VIN configured — pairing
disabled`) and idles: no connect attempts, but a periodic **listing-only** scan
(`set_target_vin("")`) still shows nearby Teslas by signal without a manual `/scan`. This is the
*design* that stops the device whitelisting its key onto an arbitrary nearby Tesla, and it no longer
relies on the `"UNKNOWN"` placeholder hashing to a name that happens not to collide (the placeholder
is kept out of matching). The UI shows "Add the vehicle VIN in Setup to begin."

## HTTP request-body and allocator-failure contract

Every normal REST and MCP body enters through `read_body_result()`, whose typed result keeps empty
body, body over the 2 KiB cap, allocation failure and receive failure distinct.

| Case | REST | MCP |
|---|---|---|
| Empty / receive failure / malformed | `400` (persisted-config routes require a body; REST commands accept empty only where the registry declares no argument or the legacy optional boolean) | HTTP 200, `-32700` |
| Oversize (> 2 KiB) | `413` | HTTP `413`, `-32600` |
| Allocation failure (incl. valid JSON whose cJSON tree is null) | `503` | HTTP `503`, `-32603` |
| Nesting > 16 arrays/objects | `400` | HTTP 200, `-32600` (request-complexity limit, not a parse error) |
| Malformed raw UTF-8 in a string (bad/truncated/overlong, UTF-16 surrogate, > U+10FFFF) | `400` | `-32700` |
| Escaped U+0000 (cJSON's NUL-terminated API cannot keep it for exact ID correlation) | `400` | `-32600` |

No rejected request reaches command dispatch or a persistent config mutation.
`logic/json_syntax.hpp` classifies bounded JSON *without allocating* (nesting limit, shortest-form
UTF-8, capture of the raw numeric-ID token); because `cJSON_Parse()` returns null for both malformed
input and allocator failure, syntactically valid supported input followed by a null tree is treated as
OOM/`503`. Body and parse-tree owners are released after copying the bounded inputs and before any
blocking BLE, probe, NVS or restart path. `logic/config_request.hpp` owns the MQTT/Syslog mutation
order (load, probe, save, respond, restart — tested with spies): a failure before the durable save
performs none of the later steps, and an empty MQTT/Syslog value is an intentional disable, not a
missing body.

The captive provisioning server is a separate transport boundary: its `POST /save` reassembles at most
1024 body bytes in a fixed buffer, and an empty or larger declared body (or a receive failure) is
`400` before parsing or persistence. The 2 KiB policy must not be generalized to it.

Responses use `JsonBuilder`, whose failure bit is sticky: a failed Create/Add keeps ownership until
all stack emitters unwind, then discards the whole tree instead of leaking a partial 200. REST and MCP
share the `json_http_reply` seam, which applies no success status and sends nothing until printing
completed, and sets 503 on print OOM before one fixed fallback send. `test/run-cjson-oom-tests.sh`
builds the exact cJSON of the pinned `espressif/cjson` registry release (re-hashed against the
lockfile) and fails every allocation in the production status emitter, representative REST/MCP
envelopes, the shared reply seam and the parser; the MQTT companion does the same for retained
payloads. These prove ownership and response policy behind deterministic seams — only the pinned IDF
build compiles the real HTTP/NVS/FreeRTOS integration, so host success is never on-device evidence.

**Active-OTA conflict guard (HTTP 409).** While an update download/flash task runs
(`ota_is_updating()`), the mutating routes `POST /send_key`, `/set_time`, `/set_mqtt`, `/set_syslog`,
`/set_wifi` and `/set_ota` return `409 Conflict` before reading the body, keeping NVS writes and
reboots away from the flashing process. The reply is the standard command response
`{"response":{"result":false,"command":"<name>","vin":"","reason":"an OTA update is in progress"}}`
(flat `{"ok":false,"result":false,"reason":…}` for `/send_key`). The test is a best-effort snapshot at
handler entry, not an atomic exclusion. A running update *check* does not trip it, but holds the
same OTA gate: `/set_mqtt`, `/set_syslog` and `/set_wifi` then save, log `reboot postponed` and still
answer with their normal success text (they restart only if `ota_config_restart_begin()` can take the
gate), while `POST /gen_keys` and `POST /set_vin` answer `503` for the whole check
(`OtaIdentityMutationGuard`). `POST /crash/dismiss` and `GET /coredump?clear=1` are not covered.

## MCP endpoint (/mcp)

`main/mcp_server.cpp` exposes the device to MCP clients over the existing `esp_http_server` on port
80. This section is the firmware-internal design; wire examples, client configs and troubleshooting
are in [`MCP.md`](MCP.md).

**Transport — Streamable HTTP, stateless.** `POST /mcp` carries one JSON-RPC 2.0 message, answered as
`application/json`. There is no SSE and no server-initiated request (`GET /mcp` → `405`,
`Allow: POST`): a long-lived stream would pin one of the few httpd sockets and the device has no
push use case. There is no `Mcp-Session-Id`, and `MCP-Protocol-Version` is ignored. Every message must
carry `jsonrpc` as the exact string `"2.0"`, and the common envelope rejects duplicate object keys
recursively before notification classification or dispatch (a unique valid `id` can still correlate
`-32600`; a duplicate id returns null). Notifications (method, no `id`) get `202` with no body; a
method-less, id-less `{}` is *not* a notification and gets `-32600`. Batches are rejected (`-32600`;
removed in `2025-06-18`), which also bounds heap cost.

**Version negotiation** (`tk::mcp_negotiate_version`, `logic/mcp.hpp`): supported `2025-06-18` and
`2025-03-26`; anything else is answered with the latest. Methods: `initialize` (capabilities `tools`
only), `ping`, `tools/list`, `tools/call`; everything else `-32601`.

**One spec table drives both surfaces.** `logic/command_registry.hpp` (`kCommands`, `CmdArg`,
`kCmdMaxArgs`) carries each command's REST name, MCP tool name/description and each argument's
per-surface keys with ONE shared `{lo,hi}` bounds pair. The `tools/list` schema, the MCP executor's
validation and the REST `/command` validation (`http_api.cpp`) are all generated from it, so drift
between schema and enforcement, or between `/api` and `/mcp`, is impossible by construction. The
surfaces stay deliberately different:

- **MCP is strict:** an absent required argument or a present-but-unparseable one is `-32602`
  (silently defaulting `set_scheduled_charging`'s `enable` would *disable* the schedule and report
  success); unambiguous encodings are coerced (numeric strings, 0/1 for bools); integers must be
  integral and in bounds before the int cast (UB guard).
- **REST keeps TeslaBleHttpProxy defaults** only for an absent *optional* field (`api_default`); a
  supplied fractional/out-of-range value is `400`. The one scalar-body exception is evcc's
  `charge_start` = JSON `true` / `charge_stop` = `false`; other non-object bodies are `400`. The
  safety exception is `set_charging_amps` (`api_required`): missing/malformed/fractional input is
  `400`, never a silent 0 A. All command failures keep the compatible JSON result/reason but return
  `502` rather than 200.

Both surfaces execute through the single kind→controller dispatch in `command_exec.cpp`. The registry,
routing, versions, validation and shared outcome text (`logic/command_result.hpp`, also the REST
`reason`) are IDF-free and host-tested (`test/test_logic.cpp`, `test_mcp`, including a pin on the
`tools/list` row order). The tool set — the run-on-key charging commands plus cache-only
`get_vehicle_state`, with role-refused commands absent (`mcp_name == nullptr`) — is tabulated in
[`MCP.md`](MCP.md#tools).

**Heap safety.** `tools/list` is the largest response (~1.5 KB) and `cJSON_PrintUnformatted` builds it
as one contiguous block, so descriptions stay terse and the tool set small; static registry strings
use `cJSON_CreateStringReference` (no per-request strdup of `.rodata`). The real `tools/list` and
cache-state producers live in `mcp_json_payloads.hpp`, so the cJSON allocation matrix exercises their
true growth. JSON-RPC numeric ids are checked from the raw token before cJSON rounding: canonical
decimal safe integers in `[-9007199254740991, 9007199254740991]` (no fraction, exponent or negative
zero) whose materialized value must match; the reply uses an internally generated exact decimal token.
String ids are copied into fixed 64-byte storage; other types, longer strings and ambiguous
duplicates are rejected with a null id. The sticky owner used by REST prevents a partial envelope;
NULL printing maps to `503`. Once method input is reduced to static pointers, booleans, enums and
fixed argument arrays, the request tree is released before response construction as well as before a
blocking vehicle call (a real-cJSON maximum-size canary proves padding- and id-dependent allocations
are zero before the largest response is built). Both handlers run inside `http_server.cpp`'s
`handle_all` try/catch.

**Security posture** is identical to the rest of the HTTP API: no auth, no TLS, trusted LAN only
([`SECURITY.md`](SECURITY.md#http-api-exposure)); it grants nothing the open REST API does not, and the
enrolled key stays Charging Manager only. Client setup: [`MCP.md`](MCP.md#client-integration).

## Concurrency (normative contract)

This section is the **rule**, not a description: new code either fits it or changes it here first,
in the same PR. Deadlock is the device's worst failure mode — frozen but not rebooting, evcc blind,
and the polling window stuck open so a parked car never sleeps.

### Lock hierarchy (`VehicleController`)

Four FreeRTOS mutexes plus one fixed-data critical-section mux, created in
`VehicleController::init` / the object definition. The one shared, exception-safe RAII guard is
`tk::SemGuard` in [`main/rtos_guard.hpp`](../main/rtos_guard.hpp) (blocking or finite/zero-wait,
exposes `acquired()`); `vehicle_ctrl_internal.hpp` keeps the aliases `tk::MutexGuard = tk::SemGuard`
and `tk::InFlightGuard`. Every take/give around code that can throw (a tesla-ble builder/parser →
`std::bad_alloc`, a `std::string` copy) goes through the guard so the lock is released on unwind
(issue #204).

| Primitive | Kind | Protects |
|---|---|---|
| `command_mutex_` | mutex, RAII | one whole command/query transaction and command FIFO generation; for `set_charging_amps`, the action and the verifying ChargeState poll are one transaction |
| `vehicle_mutex_` | mutex, RAII (`SemGuard`) | **every** call into `client_` and command-runner state (payload build/dispatch, `drive_command_runner_`, `process_rx_frame_`) |
| `cache_mutex_` | mutex, RAII, leaf | the `last_known_*` caches (`std::string` members ⇒ an unlocked copy is torn-read UB) |
| `result_mutex_` | mutex, RAII, leaf | the externally visible `last_error_` snapshot HTTP/MCP read after a foreground command |
| `CommandCompletion::sem` | per-request binary semaphore, shared ownership | signals one request-local fixed completion record; a timed-out callback cannot address stack storage or a later request's semaphore |
| `telemetry_pending_mux_` | `portMUX`, innermost, POD only | the fixed latest-value nanopb mailboxes, pending mask and charging-amps feedback generation written synchronously by tesla-ble callbacks |

**Normative order:** `command_mutex_` → `vehicle_mutex_`. `cache_mutex_` and `result_mutex_` are
independent leaf locks, never held while another semaphore is taken. `telemetry_pending_mux_` is
innermost: only bounded POD copies, and code inside never takes a semaphore, allocates, logs, parses
or calls out. Corollaries, each load-bearing:

- `vehicle_mutex_` is held only for the library call itself — **never across a request-local
  `CommandCompletion::sem` wait** (the RX path needs it to deliver the result; holding it would
  deadlock every command into its timeout). The waiter and the queued callback each retain the
  completion, and generation invalidation stops a late callback completing a later request.
- `cache_mutex_` is a **leaf**: only plain struct copy/assignment, never while calling out (library,
  BLE, NVS, logging) or taking another lock. The synchronous tesla-ble state callbacks no longer take
  it; `vehicle_loop` parses/publishes after releasing `vehicle_mutex_`.
- `clear_session_and_cache_()` takes `vehicle_mutex_` internally, so it must **not** be entered
  holding it (non-recursive ⇒ self-deadlock).
- The command-result callback writes only its request-local `CommandCompletion` fields before giving
  its semaphore. After the wait, the command task may allocate/log and publishes `last_error_` under
  `result_mutex_`; callbacks never touch that string.
- `cmd_in_flight_` (atomic, `tk::InFlightGuard`) is set only under `command_mutex_`; `loop_task`
  reads it to pause background polls — a flag that orders nothing.

### Task inventory

Application-task priorities are declared **only** in [`main/task_config.hpp`](../main/task_config.hpp)
(`tk::kPrio*`) so relative order is reviewable in one place; stack sizes stay at the `xTaskCreate`
sites with their sizing rationale.

| Task | Priority | Stack | Created in | Purpose |
|---|---|---|---|---|
| `vehicle_loop` | `kPrioVehicleLoop` = 5 | 8192 | `vehicle_ctrl.cpp` (fn: `vehicle_telemetry.cpp`) | drain fixed NimBLE Link/RX events, drive `drive_command_runner_()`, parse deferred telemetry after unlock, rotating NO_WAKE poll, sleep gating, BLE-fault link reset |
| `captive_dns` | `kPrioCaptiveDns` = 5 | 4096 | `provisioning.cpp` | captive-portal DNS (setup-AP mode only; vehicle stack not running) |
| `ota` | `kPrioOta` = 5 | 8192 | `ota_update.cpp` | OTA download + flash (transient) |
| `ota_chk` | `kPrioOtaCheck` = 5 | 8192 | `ota_update.cpp` | OTA manifest check (transient) |
| `auto_pair` | `kPrioAutoPair` = 4 | 8192 | `vehicle_ctrl.cpp` (fn: `vehicle_pairing.cpp`) | pairing supervisor: enrol / re-pair / health probe |
| `net_wd` | `kPrioWifiWatchdog` = 4 | 3072 | `net.cpp` | ghost-link watchdog (forces the active transport to re-establish, never reboots) |
| `mqtt_pub` | `kPrioMqttPub` = 4 | 6144 | `mqtt_ha.cpp` | MQTT/HA publisher (reads the caches) |
| `display` | `kPrioDisplay` = 3 | 6144 | `display.cpp` | ST7735 renderer (`CONFIG_TESLA_DISPLAY_ENABLED` builds) |
| `ota_gate` | `kPrioOtaGate` = 3 | 3072 | `main.cpp` | one-shot OTA rollback health gate (polls every 5 s; commits on proven link + INTERNAL largest block ≥ 4 KiB past 90 s under the shared owner, gives up at 600 s) |
| `safe_gate` | `kPrioOtaGate` = 3 | 2560 | `safe_mode.cpp` | one-shot healthy-window timer: after 30 s under the full normal workload it durably clears the crash-boot counter; latched safe mode never creates it |
| `syslog_task` | `kPrioSyslog` = 3 | 6144 | `syslog.cpp` | best-effort UDP Syslog forwarder (opt-in; degraded-not-fatal on a failed start) |
| `led` | `kPrioLed` = 2 | 3072 | `led_status.cpp` | APA102 status LED (`CONFIG_TESLA_LED_ENABLED` builds) |

`vehicle_loop` and `auto_pair` are created as one lifecycle unit behind `logic/task_start_gate.hpp`.
Both entry functions may be scheduled immediately, but while the gate is `Creating` their only
permitted operation is a one-tick wait — before TWDT registration, mutexes, BLE or vehicle access.
Two successful creates move the pair to `Running`; if the second create fails, `Cancelled` makes the
first task acknowledge and self-delete, and the creator waits for that acknowledgement instead of
externally deleting a task that may be running on the other core. Released tasks still wait on the
global runtime-admission gate (see [Startup failure policy](#startup-failure-policy-normative)). The
loop task's TWDT subscription is RAII-owned and removed on every unwind before self-delete.

Not in the table (ESP-IDF-owned priorities from IDF Kconfig): the **NimBLE host task** — the two
project adapters that cross into `VehicleController` (`ble_link_event_cb_`, `ble_rx_event_cb_`) only
copy bounded bytes/POD into a statically backed deferred queue and never call `Vehicle`, NVS, logging
or an allocating parser. The remaining GAP/GATT lifecycle callbacks still do bounded parsing, DEBUG
diagnostics and synchronous NimBLE submissions, but every lifecycle mutex attempt is zero-wait and
every failure drops/retries fail-closed instead of blocking the host. `vehicle_loop` owns
`apply_ble_link_state_`, `process_ble_host_events_`, final ready publication and deferred telemetry
parsing. The **esp_http_server task** runs every HTTP/MCP handler (the `command_mutex_` cycles and
cache copies), alongside the usual esp_timer / WiFi / LwIP system tasks.

### Live stack-headroom evidence

`main/stack_watch.cpp` samples `uxTaskGetStackHighWaterMark(nullptr)` from the owning task only and
keeps the lowest free value (ESP-IDF bytes) for this boot, at four allocation-rich points: `httpd`
(once per request exit, including exception/OOM fallbacks), `vehicle` (each 50 ms loop boundary),
`auto_pair` (each supervisor round) and `mqtt` (each 500 ms publisher boundary). The FreeRTOS value is
already retrospective, so a sample after a deep call still contains its minimum.
`/status.sys.stack_min_free_bytes` and the MQTT device payload expose the same cached values; a task
not started yet (or absent in safe mode) is omitted, and a genuine zero stays visible. These are
measurements, not universal alarm thresholds — sizes and call paths differ per target. The manual
bench-report gate uses one-eighth-of-stack policy floors (`httpd` and `vehicle` ≥ 1024 B, `mqtt` ≥
768 B, optional `auto_pair` ≥ 1024 B when present; derived from the 8192/8192/6144/8192-byte stacks),
described with the bench workflow in [`FEATURES.md` §6](FEATURES.md#6-build-test-and-ci).

### Atomics doctrine

A member is a `std::atomic` **only** when it is a single scalar — flag, counter or tick stamp —
crossing tasks with no multi-field consistency requirement (`pairing_lost_`, `cmd_in_flight_`,
`cmd_fail_streak_`, `last_contact_ticks_`, …). Anything read or written as a *group* — above all
structs holding `std::string` — goes under a mutex. The test: if two fields must be observed
consistently together, that is a mutex, not two atomics. Cross-task **file-scope** scalars follow the
same rule: WiFi connected / ever-connected / gateway-reachable / NTP-synced (`main.cpp`), diag
verbosity (`diag_log.cpp`), BLE `want_connect_`/`connecting_`/`scanning_`/`host_synced_` and the
connect-fail counter/stamp (`ble_client.hpp`), and MQTT `configured`/`tls`/`connected`
(`mqtt_ha.cpp`) are all `std::atomic` (seq_cst); `volatile` is not a happens-before edge.

### Exception containment (normative)

C++ exceptions are enabled and the heap is tight, so `std::bad_alloc` and library throws are
**reachable**. An exception escaping into an ESP-IDF / FreeRTOS / NimBLE / esp-mqtt /
esp_http_server / SNTP **C frame** unwinds through non-exception-aware code → `std::terminate()` →
`abort()` → reboot (and a reboot loop re-opens the poll window). The rule, by execution model:

- **Long-running tasks** (`vehicle_loop`, `auto_pair`, `mqtt_pub`, `syslog_task`, `display`, `led`)
  wrap their **iteration** in `try { … } catch (std::exception&) catch (…)`, log the component, keep
  invariants (RAII locks release on unwind), delay briefly so no tight error loop forms, and continue.
- **One-shot jobs** (`ota_chk`, `ota`) turn a throw into a terminal **error state** visible in
  `/ota/status` — never a reboot.
- **C callbacks** (NimBLE GAP/GATT + RX, MQTT, SNTP) catch locally or pass a mechanical
  fixed-buffer/POD/atomic audit, and return a valid API result. Persistent tesla-ble state callbacks
  are thin adapters to fixed latest-value mailboxes; the dynamic status and command-result callbacks
  publish POD/fixed text only. The command-result callback **always** gives its request-local
  semaphore after its catch-all, releasing the waiter without logging or allocating under
  `vehicle_mutex_`. NimBLE-host and `esp_timer` callbacks use only zero-wait mutex attempts —
  catch-all containment is not protection against a blocked callback.
- **Critical boot** (`app_main`) has a top-level boundary: anything escaping it is logged and enters
  the fatal-startup policy instead of a bare `abort()`.
- Every boundary has a terminal `catch (...)` because third-party code may throw non-standard types;
  task and one-shot paths may first log `catch (std::exception&)`, and C ABI callbacks may use a bare
  catch-all only when their sole safe response is a fixed valid API result.

`test/test_runtime_boundary_contract.py` enforces this mechanically: it derives the shipped C++
inventory from the literal `main/CMakeLists.txt` `SRCS` block, then every FreeRTOS task and reviewed C
callback from actual registration calls and callback-bearing structs (including
`esp_log_set_vprintf`); inline/runtime-selected callbacks and non-literal registration are rejected,
and each boundary needs a direct/delegated catch-all or a mechanical fixed-buffer/C/atomic audit.
Mutation canaries and the runtime-boundary binary are described in
[`test/README.md`](../test/README.md#beyond-the-pure-logic-binary). This is structural/direct host
evidence; the four-target IDF build remains the integration gate for the actual C frames.

### Startup failure policy (normative)

Component start/init functions **report success/failure**, and `app_main` classifies them and never
runs a partial system that still announces itself as "running" (issue #204).
`logic/runtime_admission.hpp` is the fail-closed cross-task latch: boot starts `Booting`; only after
every essential service and both vehicle-task allocations succeed may `app_main` make the one-way
transition to `Ready`; safe mode goes to `SafeMode`, a fatal startup path stores `Fatal`, and neither
terminal state can be promoted. The two vehicle tasks wait without touching the car until `Ready`;
vehicle-active HTTP/MCP routes return 503 and the shared command/telemetry entry points refuse work
unless the gate is ready. OTA health also treats `Booting`, `SafeMode` and `Fatal` as non-health
evidence, so a partial boot cannot spend rollback because its timer survived.

NimBLE needs an extra acknowledgement: the pinned ESP-IDF wrapper starts its hidden host task through
a void function that cannot report task-create failure. `ble_client.start` therefore does not admit the
service until the real host sync callback wins a bounded `logic/nimble_start_gate.hpp` transition;
timeout is terminal, so a late callback cannot resurrect boot. OTA health snapshots that sync plus a
saturating per-boot reset counter and refuses confirmation if the host is no longer synced or any
reset occurred after admission.

- **Essential** — `config`/`tesla_ble` NVS, `VehicleController::init` (sync primitives + tasks), the
  WiFi event group/station netif/watchdog semaphore+task, NimBLE and its mutex/timer
  (`ble_client.start`), the primary HTTP server (`http_server_start`, which unwinds a partial handler
  registration and stops the server) and the OTA health-gate task. Failure calls `boot_fatal()`: a
  still-`PENDING_VERIFY` image is marked invalid and rebooted into the previous slot immediately; an
  already-valid image **halts** and preserves diagnostics until an external reset, because a permanent
  startup loop would keep reopening the vehicle polling window without repairing a hard
  allocation/init failure.
- **Optional** — the MQTT bridge (`mqtt_ha_start`), Syslog (`syslog_start`) and the on-device
  display/LED. Allocation exceptions are contained at their public start boundary, partial
  client/task resources unwind, the failure is **logged** and the feature degrades to disabled (its
  status must not claim it is operational). The primary BLE/HTTP proxy runs regardless.

### Deferred: owned BLE-ops queue

The structural alternative to the flags above is one owner task serializing *all* tesla-ble access by
message passing, replacing the `command_mutex_`/`vehicle_mutex_`/`cmd_in_flight_` coordination and
giving commands true queue priority over background polls. **Deliberately deferred** (architecture
review 2026-07, P7): the current compensations (`cmd_in_flight_` poll pause, `cmd_fail_streak_`
link-drop backstop) are live-tested and stable, the command surface is not growing, and the rework
would touch the most incident-prone code for a structural, not behavioural, win while costing static
task and queue memory on the tightest targets. **Revisit** when (a) a new command class lands (e.g.
upstream registers `scheduledDepartureAction`) or (b) another queue-position incident occurs despite
`cmd_in_flight_`.
