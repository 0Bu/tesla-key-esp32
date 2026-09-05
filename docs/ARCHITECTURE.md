# Architecture reference

Deep internal reference for tesla-key-esp32. This is the **on-demand** companion to
[`AGENTS.md`](../AGENTS.md): AGENTS.md carries the always-needed authorization, supported-target,
build/flash-boundary, security, and memory-safety rules; the
full narrative lives here so it isn't reloaded into every session. Read this when working on
telemetry, the MQTT bridge, the web UI live feed, WiFi/LAN connectivity, sleep/link-state, pairing,
OTA, or anything that touches locks/tasks (the Concurrency contract at the end). Keep both in sync —
the canonical `$project-review` skill checks for drift between them.

> **Scope split with [`FEATURES.md`](FEATURES.md).** This file is the *Tesla-side* narrative —
> pairing, link state, telemetry, the MQTT entity list, the web UI feed. The *platform* mechanisms
> that are not specific to Tesla, BLE or evcc — crash forensics (`diag_crash.cpp`, the `coredump`
> partition, `GET /coredump`), the recovery ladder (heap watchdog → task watchdog → boot-loop safe
> mode → OTA rollback gate), the atomic CRC config blob and the WiFi credential rollback
> (`POST /set_wifi`), the `/heap` trend, the bug-report redaction (`?redact=1`) and the captive-portal
> probe handling — are cataloged in `FEATURES.md`, one entry each with *what failure it prevents*.
> Look there first for those; this file is where the vehicle-facing detail lives.

## Repository agent architecture

[`AGENTS.md`](../AGENTS.md) is the concise, runner-neutral policy loaded for normal repository
work. Reusable workflows live under [`.agents/skills/`](../.agents/skills/), Codex project
configuration and read-only specialist reviewers under [`.codex/`](../.codex/), and lifecycle
policy lives under [`tools/agent-hooks/`](../tools/agent-hooks/). CI mutation-tests this single
project-owned configuration and rejects reintroduction of retired runner-specific metadata.
`.github/workflows/pr-policy.yml` evaluates the current SHA-bound gate records from trusted
base-branch code with a read-only token and never checks out PR code; repository rules must require
its `pr-policy / current-head-records` status for that executable policy to block merges server-side.

This layer does not participate in firmware runtime behavior. It must not change the four-target
build, dependency/patch chain, partition geometry, signing boundary, OTA format, pairing/session
state, or vehicle-command behavior. Reviews and diagnosis are read-only by default; implementation
does not imply commit, push, merge, release, hardware, or vehicle authorization.

## Web UI live feed (`GET /status`)

The web UI's live data is a **browser-side interval poll**. `main/www/app.js` `boot()` calls `poll()`
once for the first paint (the hero card ships hidden and only `render()` reveals it, so without that
first call the page would sit on an empty skeleton for a whole interval) and then every **4 s** via
`setInterval`. `poll()` fetches `/status?ms=<now>` with `cache:'no-store'` — the URL is cache-busted
because a live page polls forever and one cached copy would freeze the hero on a stale state (e.g. a
transient orange "Unreachable") until a manual reload. The response is exactly what
`build_status_object()` (`http_status.cpp`) builds, and the client hands it straight to `render()`
(no envelope).

- **Refresh-now.** `poll()` is also called directly after a user action (charge/wake/gen-key) so the
  UI reflects it at once instead of waiting for the next tick. Every such call site therefore just
  means "refresh now".
- **A failed poll changes nothing on screen.** The `catch` keeps the last rendered frame and the
  optimistic local state, so a momentary blip doesn't blank the page. It does clear the `feedOk`
  flag, which parks the Bluetooth phase countdown: `app.js` ticks that number down locally once a
  second between polls, and a countdown that kept running while we've stopped hearing from the
  device would be the one element still making claims about it.
- **`render()` diffs before it writes** (`setHTML` caches the last markup on the node). Re-assigning
  `innerHTML` every 4 s would recreate the child nodes and restart their CSS animations from 0%,
  making the hero ring and the "searching" signal bars visibly jump on each poll.
- **`waitReboot()`** also does a plain `GET /status`, but that is post-OTA reboot detection on its
  own 1 s schedule — a different concern from the live feed.

**Why polling and not a push.** A WebSocket feed (`/events`) served this role for a while, and the
device paid for it: a subscriber that stopped *reading* (suspended laptop,
backgrounded tab) left its TCP send buffer full, so each async send blocked the httpd task for the
full `send_wait_timeout` (`HTTPD_DEFAULT_CONFIG`: 5 s) while the broadcast task kept producing every
2 s, each queued frame owning a heap copy of the whole `/status` JSON. That backlog exhausted the
heap (69 KB → 8 KB free, largest block 31744 → 544 B in ~7 min) and left the device **wedged, not
crashed** — no reboot, just `std::bad_alloc` spinning in the vehicle loop for ten hours, unreachable
over HTTP, MQTT and BLE. Bounding it took per-subscriber in-flight accounting and an eviction rule.
A poll needs none of that: it is request/response, the device queues nothing per client, and a
browser that stops reading costs one socket that `lru_purge_enable` reclaims. The 4 s cadence is the
price, and on a status panel it is not a visible one.

## Read-only telemetry (detail)

A rotating background poll in `loop_task_fn_` (one domain per ~30 s cycle: climate →
drive → tires → closures, full set ~120 s) refreshes per-domain caches via the
`set_*_state_callback` hooks in `vehicle_telemetry.cpp`. All polls are `NO_WAKE_SKIP`
(read-only, never wake the car) and feed the MQTT/HA bridge — evcc/pairing are unaffected.
tesla-ble invokes those hooks synchronously while `vehicle_mutex_` owns `Vehicle::loop()`, so the
hooks copy only trivially-copyable nanopb state into fixed latest-value slots under a short
`portMUX`. The same `vehicle_loop` iteration releases `vehicle_mutex_` before parsing strings and
publishing the public caches under `cache_mutex_`; no heap operation or nested cache lock runs from
the library callback.
These background polls are **paused while a serialized command/query is in flight**
(`cmd_in_flight_`), including the VCSEC health probe, so nothing is injected into the single
BLE FIFO behind another operation. Whether a connect attempt is foreground is carried separately
as `ConnectOrigin`; the arbitration flag is deliberately not reused as user-request provenance.
Every HTTP/manual entry point and every unattended auto-pair call must choose that origin
explicitly; the public methods deliberately provide no origin default.

`set_charging_amps` uses that pause as a correctness boundary: under one
`command_mutex_`/`cmd_in_flight_` transaction it sends the current action, waits for Tesla's
ACK, then sends an independent `getChargeState` request. Before the poll command can complete, the
persistent callback publishes a separate fixed `ChargingAmpsFeedback` generation plus the three
readback currents under the same short `portMUX`; it does not wait for the deferred string cache.
Success therefore requires a new fixed generation, a present field, and an exact
`charging_amps` match. The full deferred parser still clears the previous ChargeState before
decoding the new public snapshot, so an omitted field cannot inherit an old presence/value. The
ACK alone is never
reported as success. Two mismatching/missing readbacks exhaust the original command budget and
return an error, with requested/applied/request/actual current values written to the log.

The same ChargeState callback stamps `last_charge_ticks_`. `GET vehicle_data` remains
cache-only and non-blocking, but the cache is treated differently by state: idle values may be
old so read-only polling never wakes a sleeping car; during the active window (charging or a
command in the last five minutes), data older than 30 s returns HTTP 503. Thus a BLE
parser/retry storm is visible to evcc instead of being hidden behind a valid-looking HTTP 200.

**One-shot charge poll on the self-wake edge** (`logic/wake_poll.hpp`, fired in
`loop_task_fn_`). A parked, asleep car that wakes **itself** — most importantly when the charge
cable is plugged in — would otherwise never refresh its cached SOC: the active window opens only
on a recent command or cached charging, so nothing polls, and evcc keeps serving the stale
pre-plug-in reading. If that stale SOC happens to sit above `minSoc`, evcc sees no reason to start
the very charge that would open the window — a stall the stale data perpetuates. `loop_task`
closes this by watching the VCSEC sleep flag's `ASLEEP→AWAKE` edge (the same mirror the debounce
below samples) and firing **exactly one** `charge_state_poll(NO_WAKE_SKIP)` per wake episode.
`NO_WAKE_SKIP` keeps the anti-drain guarantee intact — a car already back asleep is skipped, so
the device only ever piggybacks on a wake the car performed itself; it **never** opens the active
window merely because the car is awake. The one-shot arms only after a *debounced* `ASLEEP` run
(reusing `kAsleepDebounceS`), so the ~60 s Cabin-Overheat-Protection `AWAKE↔ASLEEP` flap and an
`UNKNOWN→AWAKE` at boot cannot fire it, and it re-arms only on a fresh stable-asleep run. If the
refreshed state is `Charging`/`Starting` the existing charging arm opens the window and normal
session polling takes over; otherwise the cache is refreshed and the car re-sleeps undisturbed.
The arm/fire decision is the host-tested `logic/wake_poll.hpp`; the loop only samples the flag
mirror and fires.

Exposed under `tele` in `/status`, emitted only while the BLE link is up — the MQTT bridge
reads the caches directly, so it keeps publishing regardless (the device's web UI
renders the Overheat / Defrost chips from `tele.climate` — each shown only when a live AC draw
is available, i.e. while the car is awake and reporting; the car is never woken to populate
them — but the rest of `tele` is HA/diagnostics-only): `climate` (inside/outside/setpoint °C, on, preconditioning, plus
Cabin-Overheat-Protection `cop`/`cop_cooling`/`cop_temp`/`cop_reason` and defrost
`front_defrost`/`rear_defrost`/`defrost_mode` — separate from `is_climate_on`), `drive`
(shift, odometer_km), `tires` (fl/fr/rl/rr bar + warn), `closures` (locked,
door/frunk/trunk/window open, occupant). Numeric fields are emitted only when the car
reported them (proto3 optional) so Home Assistant shows "—"/unknown otherwise.

## OTA (self-update) — detail

Pull-based: the device fetches `manifest.json` from a fixed HTTPS URL
(`CONFIG_TESLA_OTA_MANIFEST_URL`, default GitHub Pages), compares its `version` to the
running firmware, and on confirmation downloads ITS per-target app image
(`CONFIG_TESLA_OTA_FIRMWARE_BASE_URL` + `tesla-key-esp32<suffix>.bin`, where `<suffix>` is
the chip's short tag so "esp32" appears once — `""`/`-s3`/`-c3`/`-c6` for
esp32/esp32s3/esp32c3/esp32c6, picked at compile time by `TESLA_OTA_IMG_SUFFIX` from
`CONFIG_IDF_TARGET_*`) via `esp_https_ota` into the inactive OTA slot, then reboots.
`esp_https_ota` verifies the image chip-id, so a wrong-target image is refused (never
flashed); one manifest `version` covers all targets (CI builds them from one commit).
Triggered from the web UI by tapping the firmware version in the top meta line.
Implemented in `main/ota_update.cpp`.

**Manifest intake is a bounded, exact protocol.** The HTTPS body is capped at 8192 bytes. A
non-chunked response needs a positive `Content-Length` no larger than that cap and must deliver
exactly that many bytes; a chunked response may omit a length, but it still needs the transport's
complete-data signal, must be non-empty and may not deliver a cap-plus-one byte. Truncation, a
negative/early-zero read or a lying length fails closed. The body reserves one bounded contiguous
block rather than repeatedly growing while TLS is live. Before cJSON allocation, the shared
allocation-free syntax gate requires one fully consumed JSON document, at most 16 container levels,
valid UTF-8/escapes and no decoded U+0000. `cJSON_ParseWithLengthOpts` must then consume the exact
body, the root must be an object, every root key must be unique after escape decoding, and exactly
one string-valued `version` must exist. Its version is validated before copying with the canonical
three-component grammar (zero or no leading zero, optional `[0-9A-Za-z.-]+` suffix, maximum 31
bytes); numeric cores compare as digit spans, so an oversized integer cannot overflow. The body is
released before copying the bounded version/result strings, keeping the peak at body + cJSON rather
than body + cJSON + another attacker-sized string.

**Downgrade gate (software anti-rollback).** Just after `esp_https_ota_begin` — before the
bulk download — `ota_task` reads the version from the downloaded image's own app descriptor
(`esp_https_ota_get_img_desc`) and refuses anything not strictly newer than the running
firmware. A valid RSA signature proves *authenticity* but not *freshness*, so without this a
hostile update host could serve an old, legitimately-signed image carrying a since-patched
bug. Reading the **image's** version (not the manifest's) also defeats a host that advertises
a new `version` in `manifest.json` but serves an old `.bin`. No eFuses burned.

**Rollback** is enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`); `main.cpp` defers
`esp_ota_mark_app_valid_cancel_rollback()` to a health gate (`ota_health_gate_task`) whose verdict
is the pure, host-tested `logic/health_gate.hpp`. Health is **a proven link, a non-critical largest
contiguous INTERNAL heap block and an uptime floor**, not uptime alone: the image must hold a lease
(`tk::net_is_up()`, either transport), report at least `kHeapCriticalBytes` (4 KiB) and have run
`kHealthGateBaseS` (90 s). An image that boots but then crashes/OOM-reboots under load dies while
still `PENDING_VERIFY`, so the bootloader reverts to the previous slot rather than having committed
it at startup — and so does an image that boots perfectly but never gets on the network, which a
pure 90-second timer used to commit. That second case is the one an OTA cannot repair on its own,
because the repair would have to arrive over the link the image broke; the remedy was a USB cable.
Past `kHealthGateCapS` (600 s) an image with a route and no lease is judged broken and simply left
`PENDING_VERIFY`: the next reboot from any cause rolls it back. It deliberately does **not** restart
itself — that would turn a long router outage into a silent, unrequested downgrade of a good build —
and any successfully persisted, user-requested rebooting configuration save listed below commits
it instead, via the `ota_confirm_pending_image(SuccessfulUserConfigCommit)` path described below.
A device with neither credentials nor a wire is legitimately offline (setup mode)
and counts as healthy, so an OTA installed just before the credentials were cleared is not thrown
away. A **deliberate, user-initiated reboot** inside that window is a different case only for the
successfully persisted rebooting service/network saves (`/set_mqtt`, `/set_syslog`, `/set_wifi`)
and the setup-portal save: they may confirm after the requested transaction is durably committed.
Both the timed and explicit confirmation paths first acquire the shared `HealthCommit` owner
against OTA, identity work and `FaultRestart`, then re-sample the INTERNAL largest block. A busy
owner or critical sample leaves rollback armed, closing the persisted-fault/restart race.
VIN and key mutation have a stricter identity boundary. `OtaIdentityMutationGuard` admits
`/set_vin` and `/gen_keys[?force=1]` only while the running image is already `Stable` and no
OTA/update or other identity mutation owns the gate; `PendingVerify`, unknown verification state
or an active OTA returns HTTP `503` before the VIN journal or key transaction starts. Neither
operation can therefore confirm or mutate identity on a probationary image.
Automatic VIN/key recovery, WiFi rollback and the heap watchdog are fault recovery, not health
evidence; they deliberately leave `PENDING_VERIFY` armed so the reboot rolls a regressed image
back. A brownout/power-cycle in the window likewise reverts. Confirmation is a no-op on an
already-valid image.

**Image signature.** Builds use the Secure Boot v2 RSA-3072 signature scheme *without*
hardware Secure Boot (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` + `..._RSA_SCHEME` +
`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`): every OTA image is signed and the running
app verifies the signature before installing, so a compromised update host can't push
unsigned firmware. No eFuses are burned (reversible, no brick risk, web installer still
works); trust is bootstrapped from the running app's signature block (TOFU). Compilation stays
unsigned (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`); the unprivileged build job stages only
data, then trusted `scripts/ci-sign-artifacts.sh` signs it inside the protected
`firmware-signing` Environment. No PR source executes with `OTA_SIGNING_KEY`. Classic esp32 needs chip rev
v3.0+ (`CONFIG_ESP32_REV_MIN_3` in `sdkconfig.defaults.esp32`). **Full key lifecycle +
rotation: [`SECURITY.md`](SECURITY.md).**

Partition layout (`partitions.csv`) is dual-OTA (`otadata` + `ota_0`/`ota_1`, ~2 MB each),
sized to fill 4 MB (the smallest supported flash; a larger one leaves the top
unused) so ONE table serves every target; app at `0x20000`. The `ci-build-all.sh` **app-size gate**
sits at `slot − 32 KB` (0x1e8000): each image's code rounds up to a 64 KB Secure-Boot boundary + a
4 KB signature. Firmware-size baseline schema v2 records reviewed per-target maxima for the raw
unsigned app, ELF `total_size`, `flash_code + flash_rodata`, static memory, `.bss` and IRAM, and
rejects growth in any dimension. That review baseline is deliberately separate from the unchanged
projected-signed hard gate at the slot policy: growth inside one 64 KiB signing bucket still needs
review, while signed padding/slot overflow remains an independent failure. The generated report,
rather than a copied number in this narrative, is the source for current target headroom.
The firmware is a TLS client only (OTA and MQTTS); its server is deliberately plain LAN HTTP, so
`CONFIG_MBEDTLS_TLS_CLIENT_ONLY=y` omits the unused TLS-server state machine and keeps C6 below the
next 64 KiB signing boundary without weakening client certificate verification.
**esp32s3 carries the extra on-device display code and still fits at the base `-Og`** like every
target: the Package A size levers
(#154) freed the ~64 KB the display needs, so no `-Os` (which hard-freezes under load — rejected
Package B) is required. **Migration:** a device on the old single-`factory` layout must be USB-reflashed
once via the web installer (full erase → WiFi/VIN/key reset, re-pair). After that, all
updates are OTA and preserve NVS. (Existing 8 MB-table S3 devices keep OTA-updating without
a reflash — OTA writes follow the INSTALLED table, and `ota_0` stays at `0x20000`.)

**One source tree, per-target images.** No per-board source forks — one codebase builds for
esp32 / esp32s3 / esp32c3 / esp32c6 (`scripts/ci-build-all.sh`). The build deltas are
config-only: target set per build (`idf.py set-target`), flash 4 MB, and the console is native
USB-Serial/JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`) on s3/c3/c6 — absent on the classic
esp32, where it auto-falls-back to UART0. All four targets have an explicit
`sdkconfig.defaults.<target>`: classic ESP32 carries the Secure-Boot chip-rev floor, S3 adds the
on-device ST7735/display configuration, and S3/C3/C6 select native USB-Serial/JTAG.
The web installer is a single page whose schema-v2 `manifest.json` carries exactly one build per
chipFamily plus the producing 40-hex `sourceSha`. Its local Web Serial controller uses the
repository-vendored, hash-pinned `esptool-js@0.6.1` ESM bundle, probes the connected chip, selects the
matching build and reports real cross-part progress in-page. Each build has exactly four
length/SHA-256-bound parts in semantic order: bootloader, partition table, signed app and
`ota_data_initial`. All downloads are verified before the first erase/write; the first three parts
are written before otadata is written separately and last at `0xf000`, so activation cannot precede
a complete verified flash. OTA is a single channel
where each device pulls its own
`tesla-key-esp32<suffix>.bin` (`tesla-key-esp32.bin` for the classic esp32, `-s3`/`-c3`/`-c6`
otherwise). The per-target bootloader offset (0x1000 on the classic esp32, 0x0 on s3/c3/c6) is
owned exclusively by trusted signer constants and checked against the resulting merged image and
signed factory-install manifest. The source-bound unsigned inventory binds exact paths, sizes and
digests, not flash offsets; producer `@flash_args` data is never signing or layout authority. Signed
app-only delivery leaves the installed bootloader untouched.
Before the protected job provisions the signing key, CI recomputes the canonical source fingerprint,
compares the primary build with a separate exact-source rebuild (all 53 payload files plus manifest),
and pins the exact IDF image, four dependency locks, resolved tesla-ble commit/component hash and
four patch digests. It parses the source/generated partition tables, requires every generated
`ota_data_initial.bin` to be exactly `0x2000` erased `0xff` bytes, and checks unsigned ESP image
headers/checksum/hash/chip ID/app descriptor. The signer copies those no-follow-validated files into
private staging and rehashes them before key use. It repeats the image check on the signed app and
cryptographically verifies its RSA-PSS signature against the reviewed production-authority digest
in `scripts/ota-signing-public-key.sha256`. It requires each merged image to end exactly after the
signed app, with every undeclared byte—including the NVS range—left erased.

Main classifies publication as exactly `create` or `reuse`. `create` revalidates current main, the
Release candidate and the tag's HTTP-404 absence before the key is provisioned, uploads exactly 40
assets into a draft, and binds their identity, size, digest and internal relationships. Before the
draft becomes public, all four complete merged images must byte-match the signer-owned bootloader,
partition table, erased otadata, signed app and every erased gap (including NVS) through exact EOF;
the unversioned/versioned app aliases and merged slice at `0x20000` must be identical, and every
`.elf.sha256` must name its ELF. Every signed app must pass RSA-PSS verification against the pinned
production-authority digest. In both modes CI then assembles the complete local root installer and
byte-binds all 16 manifest parts to the four merged images before either the signed Actions
artifact is uploaded or a draft/Release is created or mutated. The candidate is checked again in
the same shell step immediately
before `PATCH draft=false`; only a fresh stable `immutable: true` response becomes authority.
`reuse` accepts only that exact current 40-asset immutable Release. It opens every downloaded asset
exactly once through a directory-relative no-follow descriptor, binds metadata size/digest to that
immutable byte snapshot, and uses only the snapshot for every subsequent signature, alias,
full-layout, diagnostic and staging check. A post-validation path-swap canary proves later path
replacement cannot change staged bytes. It compares all 28 diagnostics and four
signed/merged images with the current independent build, and stages Pages without provisioning a
key, signing, re-uploading or mutating the Release. It does upload a new SHA-bound Actions recovery
artifact containing the exact twelve verified app/merged aliases, so USB recovery remains available
for a successful reuse run. Both create and reuse require the root to contain exactly four
unversioned signed apps, four versioned signed apps and four versioned merged images—no glob-matched
extra, missing, empty, symlinked or hard-linked file may cross the Actions-artifact or draft-Release
boundary. Build, signing, Pages, manifest, Release and bench consumers share the canonical
display-version grammar: no leading-zero core component and at most 31 bytes for the ESP app
descriptor. The protected `publish` job includes that locally 16/16-bound `_site/` in the exact
SHA/version-named Actions artifact together with the exact sixteen signer-owned per-target
bootloader/partition/otadata/signed-app layout inputs, and stops only after Release publication has
been refetched as immutable; it never mutates `gh-pages`.

A separate main-push-only `deploy` job has no signing Environment, OTA key or OIDC authority. It
checks out the same exact SHA without persisted credentials, downloads only the named signed
artifact, repeats the exact twelve-file root inventory, verifies the Pages manifest and all 16
parts against those local root images, then uses the sixteen layout inputs plus 28 diagnostics to
byte-bind the downloaded artifact to fresh metadata for all 40 immutable Release assets. It
revalidates branch-backed Pages and current immutable Release authority in that same final step
immediately before the branch write. After branch publication it performs
bounded cache-busted HTTPS reads of the live manifest and all 16 parts and verifies them byte-for-byte
against the immutable merged assets.
Build, independent rebuild, signing/Release mutation, branch deployment and live-channel acceptance
are distinct gates rather than one green job being treated as proof of all five.

The compiler stack gate is deliberately a **frame inventory**, not a call-graph estimate.
`scripts/check-stack-usage.py` parses every repository-owned GCC `-fstack-usage` record for each of
the four targets. Baseline schema v2 records every frame at or above 256 bytes by target,
translation unit and function identity; a new, removed or enlarged reviewed frame fails closed.
Every frame, including those below the review threshold, is still inventoried and checked against
the absolute 4096-byte limit, and any unbounded dynamic frame is rejected. GCC `.su` records do not
encode complete call depth, so this gate makes no claim about composed task-stack depth; runtime
high-water sampling remains the separate on-device signal.

The effective-build gate is based on the complete firmware graph Ninja actually compiled and
linked, not only on CMake source text. Every C/C++ translation unit in `compile_commands.json` must
belong to the repository, the pinned IDF tree, a locked managed component or the closed generated
source set; every one invokes the target compiler directly, has only trusted include roots and has
a current Ninja dependency record whose source/header inputs remain in those same roots. For main
code, the literal `SRCS` list and recursively reviewed `.def`, `.h`, `.hh`, `.hpp`, `.hxx`, `.inc`,
`.inl`, `.ipp` and `.tpp` inventory stay exact. Each final Ninja compile command equals the compile
database command plus CMake/Ninja's exact target-bound five-token depfile block immediately before
`-o`; this permits the generator's real dependency plumbing but no launcher, wrapper or hidden
compiler rewrite.

Generated firmware inputs are independently reconstructed or hash-pinned: the project ELF anchor,
three Nanopb runtime sources, the inlined/gzipped installer pages and the X.509 bundle assembly.
Their exact Ninja producer commands are pinned as well. Every linked archive is rebuilt only by the
target `ar`/`ranlib` pair and may contain only compile-database objects; the final ELF link starts
with the pinned C++ driver, uses only the two reviewed build response files, build/IDF search roots
and restricted build/IDF linker scripts, and rejects external/prebuilt objects, archives, scripts,
command chains and pre/post link seams. The final app `.bin` is likewise tied to the exact pinned
Python/esptool `elf2image` producer, chip, flash parameters, revision bounds, ELF input and
post-build digest step.

The build rejects the presence—even empty—of `CPATH`, `CPLUS_INCLUDE_PATH`, `C_INCLUDE_PATH`,
`OBJC_INCLUDE_PATH`, `DEPENDENCIES_OUTPUT`, `SUNPRO_DEPENDENCIES`, `GCC_EXEC_PREFIX`,
`COMPILER_PATH` and `LIBRARY_PATH`; `EXTRA_CFLAGS`/`EXTRA_CXXFLAGS` are overwritten with the sole
diagnostic `-fstack-usage` option rather than inheriting caller-controlled flags. Every
caller-provided `CCACHE_*` variable is rejected, including empty and future names. The pinned
official IDF image's expected `IDF_CCACHE_ENABLE=1` default is then overwritten with exactly `0`
before even the build-script self-test.
The same page can reset the selected board and stream its 115200-baud boot log after probing or
flashing. Its separate **Remove browser permission** action uses `Serial.getPorts()` and
`SerialPort.forget()` to revoke a previously granted port permission. It stays hidden when this
site has no granted port, releases a single granted port directly, and uses the browser's native
chooser only to disambiguate multiple granted ports. It refuses to interrupt a port while the
installer is busy; when the selected idle port belongs to this page, it disconnects that exact
port first and then removes the browser permission. Ports open in another tab or application stay
protected by the final open-stream guard.

**Pinned tesla-ble with an ordered build-time patch series.** esp32 / esp32s3 / esp32c3 / esp32c6 are
exactly the targets yoziru/tesla-ble declares in its `idf_component.yml` `targets:`, and the
ESP-IDF Component Manager **enforces** that list at dependency resolution, before compile. That
enforcement is treated as the definition of "supported" rather than something to work around: the
chip list cannot silently drift from what the crypto library declares, and no local checkout of a
third-party dependency has to be kept in sync. Adding a chip upstream omits (esp32c5, esp32c61)
therefore means upstreaming it there first. A local patched checkout was carried for esp32c5 for a
while and has been dropped — [`adr/0004-drop-esp32c5-target.md`](adr/0004-drop-esp32c5-target.md).

The first patch is a **correctness and anti-replay fix shared by all four targets**. Upstream
v5.1.2 calls `Peer::validate_response_counter()` and logs a duplicate CarServer response, but
then continues into state callbacks and FIFO command completion. A replay from an earlier
request can therefore refresh `last_known_charge_` or complete whichever command is currently
at the queue head. Root `CMakeLists.txt`, after dependency resolution, invokes
`scripts/apply-tesla-ble-patches.sh`; it applies every `patches/tesla-ble/*.patch` in lexical
`NNNN-description.patch` order to the materialised source before compilation. A per-materialisation
hash marker makes repeated CMake passes idempotent and lets a later patch be added, while a changed
or removed already-applied patch fails closed and requires deliberate rematerialisation. The
anti-replay patch returns immediately on
an invalid response counter, before callbacks or `mark_command_completed_`. This is tracked in
[`adr/0003-reject-replayed-tesla-responses.md`](adr/0003-reject-replayed-tesla-responses.md).

The second patch makes private-key regeneration transactional from the controller's point of
view. It verifies that an existing in-memory key can be exported before mutation, reports key
creation and NVS persistence failures, and restores the prior in-memory key if persistence of the
replacement fails. Firmware command, pairing, and polling paths remain fail-closed until the
persisted key identity is verified and the old sessions have been cleared. The host-tested
`logic/key_rotation.hpp` contract keeps `tesla_ble/key_rotate` armed across power loss and blocks
vehicle construction/signing until that cleanup reaches its durable terminal state; an NVS probe
error blocks too and cannot be mistaken for an absent marker. A VIN
transition is also journalled as `tesla_cfg/vin_txn`; the host-tested recovery decision lives in
`logic/vin_transition.hpp`, so power loss cannot silently combine a new VIN with the old key/session
state.

The third patch bounds RX-framing recovery logs without hiding the recovery itself. Warning and
error paths keep separate `steady_clock` timestamps and emit at most once per hour per severity;
repeated events increment a shared `UINT32_MAX`-saturating suppression counter that is reported and
reset only after the next emitted log. Only severe buffer corruption selects error severity. The
host semantic gate pins the helper plus all six parser/recovery callsites, so a new direct log or a
lost throttle cannot reintroduce an input-amplified log storm while the generic patch applicator
still reports green.

The fourth patch drops the five Parental Controls arms that v5.1.2 added to the
`CarServer_VehicleAction` oneof, together with their nanopb message descriptors. The firmware never
builds or sends those actions, but a referenced oneof arm keeps its descriptor tables out of reach
of `--gc-sections`, so they cost flash in every image. Removing them returns `car_server.pb.c` to
byte-identical descriptor size with v5.1.1. This is a size patch, not a correctness one: esp32c6
sits closest to the app-size policy ceiling, and image sizes quantize to 64 KiB, so a few hundred
bytes there decide whether the signed image still fits the `0x1f0000` OTA slot.

All four images use the same tesla-ble revision and ordered patch-series behavior. The wider
tesla-ble dependency strategy (IDF-6 / Mbed TLS 4 crypto seam, issue #61) is
[`adr/0002-idf6-mbedtls4-crypto-seam.md`](adr/0002-idf6-mbedtls4-crypto-seam.md).

**On-device ST7735 display (LilyGO T-Dongle-S3).** The dongle carries a
0.96" ST7735 LCD and it IS driven — see `main/display.cpp` (a status panel: WiFi/BLE header + a
SoC battery, or a WiFi/BLE-search / "Pairing…" animation; cache-only, never wakes the car). The
panel is drivable **LANDSCAPE (160x80, horizontal battery)** or **PORTRAIT (80x160, two-row header
over a vertical battery filling bottom→top)** — each BOOT-button tap rotates 90° through the four
orientations (landscape/portrait ± their 180° flips, MADCTL `{0xC8,0xA8,0x08,0x68}` over the SAME
framebuffer with the col/row offsets swapping 1/26↔26/1); the index persists in NVS
`tesla_cfg/disp_rot`. **What to show** — the priority ladder (WiFi-search > pairing > BLE-search >
battery), the SoC gradient, the RSSI→bars mapping and the SSID-scroll offset — is decided by a
pure, host-tested presenter (`main/logic/display_model.hpp`, whose `Orient` axis picks the
per-layout SSID geometry) reading a shared, IDF-free `UiSnapshot` (`main/logic/ui_state.hpp`,
assembled once under the cache lock via `VehicleController::ui_snapshot()`); `display.cpp` is a
thin renderer (`draw_landscape` / `draw_portrait`) that only DRAWS the resulting `Model` — so those
decisions are unit-tested in `test/` without a board (`logic-test` job), the layout constants have
one home, and the status LED consumes the same snapshot.
The rendering (the layout mirrors
`tools/display_sim.py`, the pixel-exact offline renderer and font source of truth — and that
mirror is no longer by hand: `scripts/check-display-sim-parity.sh`, run by
`scripts/run-mock-tests.sh`, diffs the sim's `decide()` against golden vectors the C++ presenter
emits, so a drift between firmware and sim fails the `logic-test` gate). The hardware wiring comes
from Kconfig/`sdkconfig.defaults.*`:

- **T-Dongle-S3** (ESP32-S3): framebuffer in ~25 KB internal SRAM (no PSRAM enabled); SPI 40 MHz;
  BOOT button on GPIO0. Compiled for esp32s3 via `sdkconfig.defaults.esp32s3`. Because the single
  esp32s3 image also runs on a **generic ESP32-S3** (no panel), `display_start()` first
  **auto-detects the T-Dongle-S3** by its TF-card socket's external SD pull-ups (a generic S3
  leaves those GPIOs floating) — a majority ≥4/6 HIGH means the dongle; otherwise the display is a
  complete no-op (no framebuffer, no GPIO driven), so a generic S3 boots exactly as before.

On both boards the backlight is active-low, and each tap on the BOOT button rotates the panel 90°,
cycling landscape → portrait → landscape-180° → portrait-180° → … (rotation index in NVS
`tesla_cfg/disp_rot`, migrated from the pre-rotation `disp_flip` bool). The two landscape MADCTLs
(0xA8/0x68) and the (1,26) offsets are HW-verified; the portrait MADCTLs (0xC8/0x08) and (26,1)
offsets follow the standard ST7735 rotation set and want a quick on-device confirm (the 90°
direction is a one-line `+1`→`+3` flip in `rotate_90()` if it turns the wrong way). Compiled to a
no-op stub on the other targets (`#else` in `display.cpp`), so one source tree still serves every board.

**On-device status LED (T-Dongle underside APA102).** A second, independent indicator: the single
addressable RGB pixel on the dongle underside (`main/led_status.cpp`), driven as a colour +
animation status light — WiFi/BLE search (breathing), pairing (pulse), charging (green swell),
a dimmed SoC colour when parked, blue for an OTA in flight, amber/red for warnings/errors. Its
priority ladder lives in the pure, host-tested `logic/led_status.hpp` and reads the **same shared
`UiSnapshot`** the ST7735 presenter consumes (so the panel, the LED, the web-UI hero and MQTT never
disagree about the car's state), plus a tiny LED-only `LedAlerts` for the latched error/warn/OTA
tiers (those hold a transient fault visible for 10–15 s). The SoC colour comes from the shared
`logic/soc_gradient.hpp` ramp — the same table the battery fill uses. Cache-only (never wakes the
car), needs no MQTT and no panel (works on a display-less board), an ~12-byte bit-banged APA102
frame with no heap. Opt-in: a no-op stub unless `CONFIG_TESLA_LED_ENABLED` (default off — not every
board carries this LED); pins/brightness from Kconfig (a T-Dongle-S3 wires it DI=40/CI=39).

**PR preview installer.** A maintainer can opt a reviewed same-repo PR into a **signed** build
with the `signed-preview` label, so it can be browser-flashed and tried *before* merge. The normal
PR build is unsigned and unprivileged. Only after it succeeds does the default-branch
`signed-pr-preview.yml` workflow validate the current head and independently rebuild that exact SHA
on a separate runner defined by the trusted workflow. That job can execute PR source, but has no
secrets, write/identity-token scope, Environment, restored cache or access to the primary artifact.
Only after the two exact 53-file inventories plus manifests match does the signer enter the protected
`firmware-signing` Environment, revalidate state/head/label after the approval wait, and recompute the
latest complete immutable stable version base. The protected job never checks out the PR; the
default-owned DAG binds both secret-free producers to the exact head, and the signer treats their
byte-identical, source-SHA/version-bound inventories only as bounded data. It never executes an
artifact file and does not provision the key until both rebuilds and the exact `<base>-PR-<N>`
identity match. It also
refetches the current default branch immediately before key provisioning, artifact upload and Pages
publication and requires all three checks to equal the trusted workflow's exact `github.sha`; a main
advance retires the stale queued policy run. CI then writes
all four signed apps through the same RSA-PSS and production-authority digest gate as main before it
writes
the PR's **full self-contained site**
(`build-pages.sh` → the installer page + a per-PR `manifest.json` + same-origin bins) to
`PR/<N>/` on the **`gh-pages` branch**, so `https://0bu.github.io/tesla-key-esp32/PR/<N>/` is a
directly browsable installer for that PR — it detects it is under `/PR/<N>/`, shows a preview
banner, and flashes that PR's own firmware. The **root** page has **no version picker**: it
always flashes **main**. A PR's firmware is reached only by its own `PR/<N>/` page — open the URL
directly, or follow the link posted on the PR. The serving topology has exactly one authority:
branch-backed legacy Pages from **`gh-pages:/`**. Pages Actions artifacts and the
`actions/upload-pages-artifact` / `actions/deploy-pages` path are not part of this repository's
publication model. The browser flasher fetches every part in-page and GitHub Release assets carry
no CORS headers, so the bins must be same-origin while root and durable PR subpaths must coexist.
Main owns the gh-pages **root**;
each PR owns `PR/<N>/`; both are synced by `scripts/publish-pages-branch.sh` (root sync
preserves the `PR/` tree). Constraints:

- **Signed-only and opt-in.** Unlabelled and fork PRs remain unsigned compile checks and publish
  **no** preview (an unsigned image crash-loops at boot — see [`SECURITY.md`](SECURITY.md)).
- **Versioning `<latest-stable-release>-PR-<N>`** (e.g. `1.4.30-PR-157`), stamped from the newest
  complete immutable non-prerelease GitHub Release (its stable tag plus all four digest-bound merged
  assets), never a raw newer RC tag. The protected signer derives this base again after the approval
  wait and requires exact equality, so a stale but regex-valid base cannot be signed. `ver_newer()`
  parses only `x.y.z` and ignores the suffix, so basing on
  the *latest stable release* (not `next` or a prerelease core) guarantees a later main release compares strictly-newer → the
  PR-flashed device OTA-updates forward to main; a `next` base would collide with the number
  the merge cuts and stall OTA.
- **OTA stays on main.** `CONFIG_TESLA_OTA_MANIFEST_URL` is compile-time and unchanged in PR
  builds, so a PR-flashed device checks OTA against the **main** manifest, never its own
  preview. The real-key signature anchors trust so the main release is accepted.
- **Cleanup and reconciliation.** Signing and deletion share the same per-PR concurrency group. A
  close, force-push or `signed-preview` label removal deletes the old tree and cancels an in-flight
  publisher. The event cleanup is a trusted-base `pull_request_target` workflow and checks out the
  exact base SHA before running either the Pages-authority check or deletion script; it never
  executes PR workflow/code with the branch-write token. A daily/manual matrix reconciliation revalidates each surviving directory under that
  same lock and removes it unless the PR is open, same-repository, labelled and its current head
  equals the schema-v2 manifest `sourceSha`.

GitHub Pages is configured to **Deploy from branch `gh-pages` at `/`**; that branch is the serving
authority for both the root installer and `PR/<N>/`. `scripts/check-pages-source.py` validates the
Pages API's `legacy` mode, exact branch/path and credential-free HTTPS URL. Main and signed-preview
signers run it before signing; the separate main deploy job and signed preview check it again
immediately before their branch mutation, while preview cleanup does so before deletion. Live
acceptance refetches the same API state and derives the URL from it.
Release reconciliation then requires the served root manifest identity to match the branch, tag and
latest stable Release, so a branch update that has not reached the public site never counts as a
completed publication.

## Home Assistant MQTT bridge

`main/mqtt_ha.cpp` publishes **all** cached telemetry + device status to an MQTT broker
using HA's MQTT-Discovery convention, so every entity auto-appears in Home Assistant
grouped under one device. **Read-only by design** — no command topics are subscribed
(the car is never controlled or woken from HA). Independent of evcc/BLE/pairing.

- **Config:** broker URI from NVS `mqtt_uri` (web UI: Connections → MQTT, stores `host:port`)
  overriding `CONFIG_TESLA_MQTT_BROKER_URI`; empty = disabled (bridge is a no-op).
  Optional `CONFIG_TESLA_MQTT_USERNAME`/`PASSWORD`, `CONFIG_TESLA_MQTT_DISCOVERY_PREFIX`
  (default `homeassistant`), `CONFIG_TESLA_MQTT_BASE_TOPIC` (default `tesla-key`),
  `CONFIG_TESLA_MQTT_PUBLISH_INTERVAL_S` (default 15). `/set_mqtt` reboots to re-init.
- **Transport / TLS:** a schemeless entry defaults to plaintext `mqtt://` **unless credentials
  are present** (a configured username, or `user:pass@host` userinfo), in which case it defaults
  to **`mqtts://`** so the password (sent in the clear in the MQTT CONNECT on plain mqtt) is not
  exposed to a sniffer on a broker that lives off the trusted LAN. For `mqtts://` the broker
  certificate is verified against the bundled CA roots (same trust store as OTA); an untrusted
  cert fails the handshake and the bridge stays disconnected — there is **no silent fallback to
  plaintext**. The failure reason surfaces in `/status` (`mqtt.error`) and the web UI. An explicit
  scheme (`mqtt://`/`mqtts://`) is always honored. `/status` also exposes `mqtt.tls`.
- **Node id:** `teslakey_<vin>` from the lowercase, validated VIN. Discovery topics, state
  topics, entity `unique_id`s and the HA device identifier therefore survive replacement of the
  ESP32 board; changing the configured vehicle intentionally creates a different HA device. The
  physical WiFi-STA eFuse MAC remains independently visible as `sys.board_mac` and in the boot
  diagnostics so replacement boards can still be distinguished during triage.
- **Topics and discovery registry:** the IDF-free production registry in
  `logic/mqtt_discovery_registry.hpp` contains exactly 55 entity rows and is the only source for
  component, object ID, state domain, JSON field/type, value-template inversion and HA metadata.
  It maps domains to `<base>/<node>/{charge,climate,drive,tires,closures,vehicle,device}` (retained
  JSON), availability/LWT is `<base>/<node>/availability` (`online`/`offline`), and discovery
  configs are `<prefix>/<sensor|binary_sensor>/<node>/<object>/config` (retained). Production
  derives each config topic, `unique_id`, state topic and value template from the row rather than
  reconstructing that mapping in the IDF shell.
- **Entities:** charge (soc, charge_limit, power, amps, range **km**, rate **km/h**,
  charging_state, plus extended read-only enrichment: actual_current/current_request **A**
  (delivered vs requested), volts **V** at the charger, charger phases, energy_added **kWh**
  session, minutes_to_full,
  charge limit_reason — HA bridge only, never on the `/api` evcc path), climate
  (inside/outside/setpoint °C, on, preconditioning, plus Cabin-Overheat-Protection
  cop/cop_cooling/cop_temp/cop_reason and defrost front_defrost/rear_defrost/defrost_mode),
  drive (shift,
  odometer km), tires (fl/fr/rl/rr bar + warn), closures (locked/door/frunk/trunk/window/
  occupant), sleep_state, and device diagnostics (optional wifi/ble RSSI, ble_link, paired,
  optional **last boot** timestamp, free_heap, largest_block, min_free_heap, firmware,
  reset reason slug/code, crash-dump and safe-mode flags, and WiFi/MQTT reconnect counters).
  The same retained Device JSON also carries optional per-task minimum-free-stack bytes for HTTP,
  vehicle, auto-pair and MQTT as raw MQTT diagnostics; those four payload-only fields deliberately
  have no HA discovery rows and therefore do not create entities. Optional
  car-sourced numeric and boolean fields are emitted only when the car reported them (proto3
  optional), so an unseen value reads "unknown" in HA rather than a phantom 0/OFF. Every binary
  discovery template has the same presence guard; `locked` alone inverts ON/OFF for HA's `lock`
  class. Device `safe_mode` and `crash_dump` are real JSON booleans, not truthy `"ON"`/`"OFF"`
  strings. The aggregates warn and door/frunk/trunk/window fold several per-wheel/
  per-opening booleans with present-AND-true semantics (an unreported part counts as
  no-warning/closed by design). **Units:** Tesla reports range/rate/odometer imperial; the MQTT bridge
  converts to metric (km, km/h) — only the Tesla-compatible `/api` path keeps miles (evcc).
- **Publishing:** a dedicated `mqtt_pub` task reads the thread-safe caches; on every
  (re)connect it (re)sends discovery + `online` + an immediate snapshot, then republishes
  state every interval. The same active-window gating that lets the car sleep applies to
  the *source* polls, so MQTT keeps serving the last-known (retained) values while asleep.
  Every retained JSON payload is built by the same `mqtt_payloads.hpp` production emitters the
  pinned-cJSON host matrix executes, and is completed through the sticky owner before the
  publish seam is called; a build/print failure therefore publishes nothing and cannot replace a
  good retained document with a partial one. Discovery → availability → state is short-circuited,
  and any build, print or broker-publish failure rearms the complete discovery sequence rather than
  resuming halfway through it. The host gate materializes all 55 registry rows and requires every
  referenced `value_json` field and type in the corresponding full domain emitter. Static policy
  derives every `build_*_payload` definition, production call and named OOM/success case as equal
  sets, so adding a ninth factory without a gate case fails closed.
  The 60 s task watchdog is fed at the loop boundary and after every completed publish: a
  discovery burst that keeps making progress cannot false-trip it, while a single publish that
  never returns still does.

## Syslog forwarder

`main/syslog.cpp` forwards the same in-RAM diag log served by `GET /diag` (`main/diag_log.cpp`)
to a UDP Syslog collector, best-effort, framed as RFC 5424 (`<PRI>1 - tesla-key-esp32 - - - -
<message>`). The capture point is `diag_log.cpp`'s `esp_log_set_vprintf` hook, which already
mirrors every `ESP_LOG*` line (from this firmware **and** ESP-IDF/NimBLE internals — NimBLE is
pre-throttled to `WARN` there) into the `/diag` ring; the same call also queues the line for
Syslog, so there is exactly one capture point to keep in sync, not two.

`diag_log.cpp` copies each bounded line into the ring and snapshots the optional sink while holding
the ring mutex, then releases the mutex before invoking the sink. The sink may allocate, throw or
perform queue/network work; keeping it outside the lock prevents an OOM/unwind path or a slow
forwarder from wedging `/diag` and every producer that logs. Chunked readers also bind their
logical snapshot to an append count and clear epoch: writers may replace bytes already sent, but if
they reach an unread byte the response stops before mixing two generations. That fail-closed rule
is especially important for `?redact=1`, where a new prefix joined to an old sensitive tail could
otherwise evade line-marker redaction. A wrapped ring's initial partial line is discarded before
redaction, and a logical line longer than the bounded 288-byte frame is emitted only as the static
`<redacted>` token; the marker and its sensitive value can therefore never be split across two
independently redacted fragments.

- **Severity** (`tk::syslog_pri_for_line`, host-tested): facility is always `user` (1); the
  severity comes from the line's own esp_log level, which the hook sees because it captures the
  *formatted* line — `E`/`W`/`I`/`D`|`V` → 3/4/6/7. A leading ANSI colour escape
  (`CONFIG_LOG_COLORS=y`, the IDF default) is skipped first, and a line without a recognised
  `"<L> ("` prefix stays `info` — plenty of forwarded lines come from tesla-ble and NimBLE with
  no esp_log prefix at all, and inventing a severity for those would be a false alarm.
  *This used to be a hardcoded `<14>`.* The cost was measured: a week of device logs
  (17.–24.07.2026, 211366 lines) arrived at the collector as uniformly `info`, including 61417
  `ESP_LOGE` and 81708 `ESP_LOGW` lines, so `severity:error` matched nothing and finding a fault
  meant substring-matching `_msg:"E ("`.

- **Config:** one NVS string, `syslog_uri` (`tesla_cfg` namespace) — a bare `"host:port"`, no
  scheme (a bare host defaults to port 514); `""` disables forwarding. Falls back to
  `CONFIG_TESLA_SYSLOG_SERVER` (Kconfig, default empty). Set from the web UI (Connections →
  Syslog card, pencil icon → `POST /set_syslog`, `{"server":"host:port"}`) or NVS/Kconfig
  directly. Resolved **once**, at `syslog_start()` (called early in `app_main`, before WiFi) —
  like the MQTT bridge, a config change persists then reboots to apply, so there is nothing to
  re-read at runtime.
- **Delivery:** a background task queues lines (fixed 24-deep queue of 256-byte messages —
  small on purpose, since the queue is one contiguous heap allocation and this device's binding
  memory limit is the largest *contiguous* free block) and, once WiFi is up, resolves the
  target via `getaddrinfo()` and forwards. Re-resolve + re-probe is throttled to a ~10s cadence
  (`have_checked`, not `!resolved` — a persistently failing DNS/host must not re-run
  `getaddrinfo()`+ping every loop). **Delivery gates on DNS resolution only** (`resolved`) —
  never on the reachability probe below — since Syslog is inherently best-effort UDP.
  Startup uses `logic/syslog_start_gate.hpp` because FreeRTOS may schedule the consumer before
  `xTaskCreate()` returns. The task waits while its queue remains startup-local and unpublished;
  only a successful create commits the gate and publishes the process-lifetime queue with
  release/acquire ordering. A create failure cancels the waiter before deleting the unpublished
  queue. Once published, the global log hook may have snapshotted it at any time, so normal boot
  never deletes it — optional forwarding degrades without creating a hook-versus-delete UAF.
- **Reachability probe (advisory only, never a delivery gate):** ARP for an on-subnet host (L2,
  works even when the collector firewalls ICMP), else a 2-echo ICMP ping (800 ms timeout each).
  Surfaced in `/status.syslog.reachable` purely as a UI hint ("Enabled · not answering ping").
- **Send-failure handling** (`logic/syslog_policy.hpp`, host-tested): an errno from
  `sendto()`/`socket()` is classified HARD (routing/host-down errors — `ENETUNREACH`,
  `EHOSTUNREACH`, `ENETDOWN`, `EHOSTDOWN`, `EADDRNOTAVAIL` — re-resolve + re-probe immediately)
  or TRANSIENT (everything else, incl. `ENOMEM`/`ENOBUFS`/`EAGAIN` — hold the destination, let
  the ordinary cadence re-check). Getting this wrong the other way — clearing the throttle on
  every failure — turns a chatty diag stream (a busy BLE poll can log several lines a second)
  into a `getaddrinfo()`+ping storm that runs hardest exactly when the link is worst; the
  handler also logs the failing/recovering *transition* only, never per-line, for the same
  reason. Mirrors an equivalent module in the sibling `daikin-altherma-esp32` project, where
  this exact storm was diagnosed on a live board.
- **Loop guard:** `syslog_send()` drops any captured line containing the `"syslog:"` tag this
  module's own `ESP_LOGx(TAG, ...)` calls render (`TAG = "syslog"`) — otherwise this module's
  own "send failed" diagnostics would themselves be queued for (failing) delivery, feeding the
  exact storm the paragraph above avoids.
- **Status:** `syslog_status()` → `/status.syslog` (`configured`/`resolved`/`reachable`/`host`/
  `port`/`error`), read by the web UI's Connections card exactly like the MQTT row.

## Heap-exhaustion watchdog (the last-resort escalation)

Every OOM guard in this firmware turns "out of memory" into **recover and continue** — the
`handle_all` try/catch answers 503, the BLE parse guards reset the link, an MQTT publish is skipped.
That is correct for a **transient** shortage and must stay. What was missing is the
next question: *what if it never recovers?*

On **2026-07-18** a non-reading subscriber of the then-current WebSocket status push exhausted the
heap (that feed has since been replaced by the web UI polling `/status`, which cannot queue a
backlog on the device at all). The device then sat at `free=14820`, `largest_block=768` — against a
healthy 31744 — for **ten hours**. It never crashed and never rebooted: `vehicle_->loop()` threw
`std::bad_alloc`, the handler reset the BLE link, the next 50 ms iteration threw again, ~20×/s all
night. HTTP could not serve, MQTT could not reconnect, and the WiFi watchdog was itself dead
(`ping_sock: create ping task failed` — it could no longer allocate its own task). **A hang is the
worst failure shape available**: a crash reboots in seconds, a wedge looks like a powered-off
device, heals never, and reports nothing.

The escalation lives in the pure, host-tested `main/logic/heap_watchdog.hpp`, sampled by
`loop_task_fn_` at the existing 30 s heap-log site:

- **Trigger:** `largest_block` below **`kHeapCriticalBytes` (4 KB)** *continuously* for
  **`kHeapCriticalHoldMs` (5 min)**. Healthy steady state is 31744 B and the wedge sat at
  480–1536 B, so the threshold separates them without a tight estimate of either.
- **`largest_block`, never `free`.** The binding limit on this chip is the largest contiguous
  block. During the incident `free` held a plausible-looking ~16 KB the whole time — a total-free
  test would never have fired.
- **`MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL`, never plain `8BIT`.** `heap_caps_*` reports the max
  across every heap carrying the cap, and a board that **registers PSRAM** into `8BIT`
  (`CONFIG_SPIRAM_USE_MALLOC`) skews it. Such a board in the exact wedge — internal DRAM at 768 B — would read
  ~7.8 MB and never trigger, making the watchdog a silent no-op on the one target with the extra
  RAM. The thresholds are internal-DRAM numbers and only mean anything against an internal-DRAM
  sample. The `HEAP` log line carries `internal_largest=` alongside the historical `8BIT` figures
  (identical on the four PSRAM-less targets, so old captures stay comparable).
- **An unbroken run, never a single `bad_alloc`.** One failed allocation is exactly the transient
  the other guards absorb. A single recovered sample resets the clock. This is what keeps the
  watchdog from becoming a reboot loop — which matters because each boot re-opens the active
  polling window, so a rebooting device keeps a parked car awake. (That objection is weaker than
  it looks: the wedged state defeats car sleep too, and harder — it reset the BLE link at 20 Hz
  for ten hours.)
- **Excused during an OTA.** `esp_https_ota` holds the largest allocations this firmware ever
  makes, so a low reading there proves nothing, and a restart mid-install is the one reboot that
  could leave a half-written slot. An OTA **clears** the run rather than skipping the sample —
  skipping would let a run that began *before* the download resume its clock and fire during it.
  Queried via `ota_is_busy()` (one atomic), never `ota_get_status()` (copies `std::string`s and
  can throw on the very heap this is deciding about).
- **On fire:** persist `reboot_why=heap:<n>` to NVS `tesla_cfg`; **only a successful durable write
  authorizes the reboot**. A normal write failure or exception leaves the device up degraded at the
  current ladder step, so an unrecorded restart can never erase the evidence/cap. After persistence,
  `ESP_LOGE`, a **300 ms `vTaskDelay`**, then `esp_restart()`. This is automatic fault recovery and
  therefore deliberately does **not** confirm a `PENDING_VERIFY` OTA image: a new image whose heap regression
  fires the watchdog must roll back on the next boot. The delay is not
  cosmetic: `syslog_send()` only *queues*, and its task runs at priority 3 against `loop_task`'s
  5, so without a yield the final message dies in the queue on a single-core target — and the
  `/diag` ring does not survive the reboot either. Skipping it would throw away the one line that
  explains the restart.
- **Bounded:** after **`kHeapMaxConsecutiveRestarts` (5)** consecutive watchdog restarts it stops
  restarting and says so once. Five cycles prove a restart is not fixing this one, and continuing
  would cycle the radios every ~10 min indefinitely. Relatedly, a boot that followed a watchdog
  restart **does not seed the active polling window** (`vehicle_ctrl.cpp` `init()`) — that seeding
  is what would make a restart loop expensive for a parked car, and a self-healed device has no
  user waiting on a warm cache. Only an exact NVS `NOT_FOUND` is an ordinary boot. A breadcrumb is
  consumed with an erase; a read/erase failure or a malformed/out-of-range value closes the ladder
  at the cap and suppresses the post-restart activity window instead of being mistaken for zero.
  A genuine power cycle, crash or OTA normally leaves no breadcrumb, so the counter still bounds
  only deliberate watchdog restarts when storage is healthy.
- **Afterwards:** `main.cpp` takes the NVS breadcrumb once at boot (read + clear) and holds it for
  the process; `/status` reports it as **`last_reboot`** (omitted on every ordinary boot). This
  exists because `esp_reset_reason()` cannot tell a deliberate `esp_restart()` from a user power
  cycle — both read SW/POWERON — so without it a device that self-heals unattended at 04:00 leaves
  no trace, and the next investigation starts from scratch.

The critical-path `"heap:<n>"` formatter uses a fixed buffer and does not allocate. The subsequent
NVS write can still fail or allocate internally, and the boot-time NVS read materializes a
`std::string`; both normal errors and exceptions are contained. Persistence failure blocks
`esp_restart()`. That matters because this code runs precisely when allocation is failing: an
uncontained throw or an unrecorded reboot here would turn a wedge into an opaque reboot loop.

### Why a restart, and not in-place recovery

Rebooting is the crude answer, so it was only adopted after the alternatives were researched and
found worse. The short version: **ESP-IDF offers no reliable way to reclaim a wedged heap from
inside the running image**, and the teardown paths one would have to call are themselves among the
least reliable code in the SDK. Evidence for each candidate:

- **Subsystem teardown/reinit** (`httpd_stop`/`httpd_start`, `esp_mqtt_client_destroy`,
  `esp_wifi_deinit`, `nimble_port_deinit`) — the obvious "smarter" fix, with the worst track
  record. `nimble_port_deinit` leaked *twice in a row*: Espressif fixed one leak, and a second,
  independent one survived it ([esp-idf#8136](https://github.com/espressif/esp-idf/issues/8136)).
  `esp_wifi_deinit` leaked per cycle on IDF 5.0/5.1 (a regression absent in 4.4, fixed in 5.2 —
  [#12014](https://github.com/espressif/esp-idf/issues/12014)), was reproducibly crash-prone in the
  3.x era ([#2050](https://github.com/espressif/esp-idf/issues/2050)), and
  [esp32.com t=13189](https://esp32.com/viewtopic.php?t=13189) documents a supplicant callback no
  deinit path ever frees. `esp_http_server` leaked **~16.7 KB per stop/start cycle** on IDF 5.3
  because `httpd_stop()` never released the with-caps task stacks — and the *first upstream fix for
  that* crashed on an assert ([#14266](https://github.com/espressif/esp-idf/issues/14266)). The
  pattern is not "one bad release": whether deinit returns its memory depends on the exact IDF
  revision and is patched reactively. On top of that, deinit paths **allocate**, and this code runs
  when allocation is already failing — a throw there unwinds into the net-less loop task and
  `abort()`s, trading our controlled restart for an uncontrolled one *without* the breadcrumb. The
  crash-only literature names this directly: a restart is trustworthy only when it is implemented
  outside the failing component and does not run that component's own code, and rarely-exercised
  cleanup paths are unreliable *because* they are rarely exercised
  ([Candea & Fox, HotOS-IX](https://www.usenix.org/legacy/events/hotos03/tech/full_papers/candea/candea.pdf)).
- **A ballast / rainy-day block** freed under pressure is a real pattern — libstdc++ ships one (a
  preallocated emergency pool so `std::bad_alloc` can still be *thrown* once malloc fails, which is
  also the most likely explanation for how the wedged device kept throwing for ten hours instead of
  crashing). Rejected here because it costs permanent internal DRAM out of a ~31 KB steady-state
  largest block, and this restart path needs no headroom: it is allocation-free by construction.
- **`heap_caps_register_failed_alloc_callback()`** is official, but it runs *synchronously inside
  the allocator*, in the context of whatever task or ISR hit the failure; it takes the size, the
  caps and the function name, returns nothing, and cannot satisfy or retry the allocation. ESP-IDF
  documents no constraints on what is safe inside it. That makes it a sensor, not an actor — and
  the 30 s sample site already covers the sensing. Espressif's own built-in escalation for exactly
  this condition is `CONFIG_HEAP_ABORT_WHEN_ALLOCATION_FAILS` → `esp_system_abort()`, i.e. a reset
  with *less* ceremony than ours.
- **Defragmentation** does not exist: the heap docs treat fragmentation-induced failure as expected
  behaviour and offer no compaction, and part of the TLSF allocator lives in chip ROM, so even
  allocator-level improvements cannot reach released branches. Espressif's only in-place lever is
  carving dedicated regions for critical objects (`heap_caps_add_region_with_caps`), and a
  MicroPython maintainer judged split-heap of marginal value on ESP32 once WiFi and BLE must
  coexist ([micropython#8940](https://github.com/micropython/micropython/issues/8940)). This
  project already does the containment half — the static `/diag` ring, the gzipped web UI, the
  once-at-boot display framebuffer — but that is *prevention*, and prevention has no answer for
  the morning after.
- **What shipping ESP32 firmware actually does** is bounded restarting, not in-place repair.
  ESPHome's [safe mode](https://esphome.io/components/safe_mode/) is a restart ladder: a persisted
  boot-failure counter (default 10), a boot that only counts as "good" after a healthy-uptime
  window, a degraded mode keeping just logging/network/OTA — and it reboots *again* after five
  minutes. [Tasmota](https://tasmota.github.io/docs/Device-Recovery/) counts restarts and escalates
  to a settings wipe and finally a reflash. Neither attempts to heal a live heap.

So the reboot is the *last* rung, and the parts that make it defensible are the hygiene around it,
all of which match the same literature: a **long unbroken trigger** so transients never reach it, a
**restart cap** (Candea & Fox prescribe a maximum retry limit precisely to prevent reboot cycles),
and a **breadcrumb persisted before the reset**, which Memfault's watchdog guidance names as the
thing that separates a diagnosable reset from a mystery.

**What is deliberately not here:** a *load-shedding rung* before the restart — dropping the
firmware's own optional consumers (stopping MQTT, closing idle sockets) to release the heap this
firmware itself owns, and only restarting if `largest_block` does not recover. That would have
healed the 2026-07-18 incident in place, since the leak was in structures we owned. It is the
strongest candidate for a next rung, but it only helps for leaks in structures *we* know about, and
it stays out of this change until a second incident shows the pattern is worth the complexity.

### Reading it in syslog

Syslog is the only post-mortem source that outlives the restart — the `/diag` ring is RAM and
`esp_reset_reason()` cannot tell a deliberate `esp_restart()` from a power cycle. So the escalation
narrates itself; a reader should be able to reconstruct the whole decision from these lines alone:

| Line | Meaning |
|------|---------|
| `HEAP free=… largest_block=… min_free=… internal_largest=…` | the ordinary 30 s trend line, always present |
| `HEAP CRITICAL: … watchdog ARMED, restarting in 300 s unless it recovers` | the countdown just opened |
| `HEAP CRITICAL for <n> s … restarting in <m> s unless it recovers` | still critical, one line per 30 s sample — this is the proof the shortage was *sustained*, not a spike |
| `HEAP recovered after <n> s critical … watchdog disarmed` | the run ended on its own; no restart |
| `HEAP critical run (<n> s) cleared: an OTA is in flight …` | the run was excused, not healed |
| `HEAP EXHAUSTED for <n> s … RESTARTING DELIBERATELY (watchdog restart <k>/5, reboot_why=heap:<k>; …)` | the restart, with the state that caused it |
| `HEAP EXHAUSTED … but <n> consecutive watchdog restarts have not fixed it — NOT restarting again` | the cap held; the device stays up degraded |
| `BOOT this boot was caused by the firmware itself: reason=heap:<k> …` | logged on the *next* boot, closing the loop |

The elapsed times are the **measured** age of the critical run, not the configured hold, so a fired
run legitimately reads somewhat over 300 s (it is sampled on a 30 s cadence).

**Keep any line on this path under ~230 characters.** `diag_log.cpp`'s capture hook formats into a
256-byte *stack* buffer (deliberately — it must not allocate on the heap it is reporting about),
and anything longer reaches both `/diag` and syslog **cut off mid-sentence**. The restart line is
the one that must never be truncated, so it carries the state and a pointer here, not the
reasoning itself.

**Both `BOOT` lines are emitted *after* `syslog_start()` on purpose** — `syslog_send()` is a no-op
before that, because the queue it writes to does not exist yet. Anything logged earlier in
`app_main` reaches the serial console and the `/diag` RAM ring and *nothing else*, which for a
post-mortem means nothing at all: the restart erases the ring. That applies to the `reason=heap:<k>`
breadcrumb (which is *read* earlier, before anything else can reboot, but announced here) and to
`BOOT reset_reason=…` — whose values are likewise **sampled at the top of `app_main`**, before NVS
init, WiFi and the component `start()`s have allocated, so the heap figures describe the boot we
came up in rather than the boot we already made.

That second line had been left above `syslog_start()`, and the effect is worth recording because it
is the failure mode this whole section exists to prevent: over 17.–24.07.2026 the device booted 56
times and the collector received **zero** `BOOT reset_reason=` lines. When it went silent for 1 h
54 min on 20.07. and came back up, there was no way to tell a panic from a brownout from a deliberate
restart — the one line that answers it had been written to a ring that the reboot then wiped.

## WiFi / LAN connectivity (reconnect + watchdog)

**Where this lives.** All of it is `main/net.cpp`, behind the transport seam `main/net.hpp`.
Everything above that seam — the HTTP server, MQTT, syslog, mDNS, SNTP, OTA, the display, the
LED — asks `tk::net_is_up()` / `tk::net_kind()` / `tk::net_active_netif()` and never touches
`esp_wifi` itself. Before the seam existed there was one predicate, `wifi_is_connected()`,
hand-declared as an `extern` in five translation units, and `esp_netif_get_handle_from_ifkey(
"WIFI_STA_DEF")` hardcoded in three more; each was correct while WiFi was the only transport
and silently wrong the moment it was not. The transport identity itself
(`tk::NetLink::{None,Wifi,Eth}`) and the watchdog's decision logic are the host-tested
`main/logic/net_link.hpp`.

### Which transport comes up

Boot order is **wire first, radio second** — and the reason is the radio, not the bandwidth.
WiFi and BLE share ONE antenna path on every chip this firmware targets, so a *running* WiFi
stack means time-division coexistence with NimBLE for as long as the device is powered. That is
what forces `WIFI_PS_MIN_MODEM` (see below) and what makes every GATT round-trip to the car
slower than it needs to be. Coming up on Ethernet does not merely avoid *using* WiFi, it avoids
**starting** it: no coexistence arbitration at all, and the ~57 KB of largest-block the stack
holds stays free on a device whose binding limit is the largest *contiguous* block.

1. `tk::net_eth_probe()` runs very early — **before** the setup-portal decision — and reads the
   W5500's `VERSIONR` (0x0039, always 0x04). A floating MISO reads 0x00/0xFF, so there is no
   realistic false positive. On no answer the SPI bus is freed again and those GPIOs are left as
   they were found.
2. A wired board with **no stored SSID does not enter the setup portal**. DHCP gives it an
   address with nothing configured, so a captive AP would strand a perfectly reachable device —
   a regression created purely by adding a transport. The VIN is then set over the LAN.
3. `tk::net_start_eth()` waits for a lease, and the deadline means **two different things**:
   with the PHY reporting **no link** (no cable, dead switch port) it falls back to WiFi after
   `CONFIG_TESLA_ETH_WAIT_S` (20 s) — nothing is coming. With the **link up but no lease yet** (a
   slow or busy DHCP server) it keeps waiting, up to `kEthLeaseLinkedCapFactor` × that. Falling
   back there would run the WiFi stack for the rest of the boot — paying the BLE coexistence cost
   and ~57 KB of largest-block — on a board that is in fact wired. The Ethernet driver keeps
   running either way, so a cable plugged in later still takes over.
   **On the wired path `esp_wifi_init()` is never called at all:** `main.cpp` short-circuits
   (`!on_wire && !net_start_wifi(...)`), which is what turns "prefer the wire" into "no radio
   coexistence" rather than merely "prefer the wire for routing".
4. On the wired path the WiFi credential-rollback backup is **not** consumed: coming up on
   Ethernet proves nothing about credentials on trial, and spending them there would discard the
   only way back to a working network the moment the cable is unplugged.

Every resource acquired after the positive probe is owned by one startup record until activation
commits. A failure unwinds in dependency-reverse order: stop the driver; unregister IP/Ethernet
handlers; retract the coherent globals; delete glue and netif; uninstall the driver; delete PHY,
MAC and event group; finally release SPI. If driver uninstall fails, its PHY/MAC/SPI tail is kept
alive and boot fails closed — deleting objects still reachable by the driver would turn a bounded
startup fault into dangling callbacks or a later UAF. A no-link/DHCP boot fallback is different:
the successfully activated Ethernet stack remains process-lifetime state so a later cable can
take over without rebooting.

**Polling mode is deliberate, not a workaround.** The M5Stack ATOMIC PoE Base routes only
SCLK/CS/MISO/MOSI + power — there is no INT line and no RST line to wire — so the driver polls at
`CONFIG_TESLA_ETH_POLL_MS` (10 ms) and the PHY is reset over SPI (the W5500's `MR` register)
instead of by a strobe. ESP-IDF ships a CI configuration for exactly this shape
(`components/esp_eth/test_apps/sdkconfig.ci.poll_w5500`, also 10 ms at 20 MHz). The poll period
bounds RX *latency*, not throughput: each poll drains everything queued in the W5500's 16 KB
buffer. Note that ESP-IDF **6.0 moves the SPI Ethernet drivers out of the core** into the
`esp-eth-drivers` component — one `idf_component.yml` line whenever the IDF-6 work
([ADR-0002](adr/0002-idf6-mbedtls4-crypto-seam.md)) happens.

**The wire is made to win the default route — lwIP does not do it on its own.** ESP-IDF ships
`WIFI_STA_DEF` at `route_prio` **100** and `ETH_DEF` at **50** (`esp_netif_defaults.h`), i.e. the
opposite of what this feature is for, so `net.cpp` creates the Ethernet netif with a raised
`route_prio` (`kEthRoutePrio`).

Be precise about the scope, because lwIP's `ip4_route()` has two branches and only one of them
consults priority (verified in `components/lwip/.../ip4.c`, not assumed):

- **Off-link** destinations — anything via the gateway: NTP, OTA, an MQTT broker or syslog
  collector outside the subnet — go to `netif_default`, which is precisely what `route_prio`
  selects. Without the raise, the WiFi station would win them.
- **On-link** destinations do not consult it at all: `ip4_route()` walks `netif_list` and returns
  the **first** netif that is up and whose subnet matches. With both interfaces on the same `/24`
  — the normal home case — same-subnet traffic leaves over whichever netif was registered first,
  i.e. the WiFi station. Measured: with both up, syslog to an on-link collector kept the WiFi
  source address while `/status.ip` reported the Ethernet one.

That asymmetry is **accepted, not fought**. Forcing per-packet source selection across two netifs
on one subnet means overriding the stack's routing, and the only state it would improve is the
runtime hot-plug — which never delivers this transport's actual benefit anyway, because WiFi is
already running (coexistence paid, heap spent). The benefit lives in the boot-with-cable path,
where WiFi is never started and there is no second netif to disagree with.

**The watchdog's ICMP baseline is PER TRANSPORT.** `s_gw_ever_reachable` is indexed by
`NetLink`, not global. A single flag let a freshly plugged-in Ethernet segment inherit "this
gateway has answered before" from the *WiFi* gateway — precisely the false evidence the baseline
rule exists to refuse, evaporating at the moment it is needed. Same class of mistake as a
per-transport lease held in one global: state that belongs to a transport must be kept per
transport.

**Both transports can hold a lease at once.** A board whose W5500 found no lease at boot falls
back to WiFi with the Ethernet driver still running, so a cable plugged in later brings the wire
up *alongside* the radio. `tk::net_kind()` is therefore DERIVED from two per-transport lease
flags via the host-tested `tk::net_link_active()` (Ethernet outranks WiFi — it is the transport
that costs the BLE radio nothing, and it is what lwIP puts first), not written by whichever event
fired last. That matters on the way back down: unplugging the cable must fall back to WiFi, not
to "no network", or syslog stops, the display shows "searching" and MQTT drops the RSSI while a
perfectly healthy WiFi lease is still in hand. For the same reason the WiFi-only readings
(`net_wifi_signal`, `net_wifi_standard`) gate on the WiFi *lease*, not on which transport is
active — the lease flag is exactly the window in which `esp_wifi_sta_get_ap_info()` is safe.

**The W5500 has no MAC of its own** (no EEPROM), so one is supplied from `ESP_MAC_ETH` — the
chip's eFuse-derived Ethernet address, distinct from the WiFi STA MAC so the two can never
collide on one LAN. The MQTT/HA node id is transport- and board-independent: it is derived from
the validated VIN, because it is baked into every Home Assistant entity id. Switching between
WiFi and Ethernet — or replacing the entire ESP32 board — therefore cannot rename the vehicle's
entities.

The STA→LAN link (distinct from the car BLE link-state below) is kept up by two layers:

- **Event-driven reconnect.** `wifi_event_handler` reconnects on every
  `WIFI_EVENT_STA_DISCONNECTED`. The boot-time path keeps the original budget: if the device
  has **never** held an IP (`s_ever_up == false`) and the `MAX_RETRY` (10) fast
  attempts are spent, it sets `WIFI_FAIL_BIT` so `net_start_wifi()` times out and falls back to
  the **setup portal** (the credentials are presumed wrong). But once the device has been
  online at least once, a later drop reconnects **forever** — the credentials are known-good,
  so surrendering would only strand the device. (The old code gave up after 10 retries in
  *all* cases, which is exactly how a 3.5 h router outage left the board reachable-over-BLE
  but off the LAN, recoverable only by a manual USB reset.)

- **Connectivity watchdog** (`net_watchdog_task`, ~30 s cadence; the verdict comes from
  `tk::watch_step()` in `logic/net_link.hpp`, so its counting and — more importantly — its
  never-answered-ICMP baseline rule are covered by `test/test_logic.cpp` rather than living as
  an `if` inside a task loop). The event path cannot catch
  a **missed-deauth "ghost" association**: the stack still believes it is connected (holds the
  IP, keeps emitting TCP that times out — e.g. MQTT `esp-tls select() timeout`) but the AP
  forwards nothing and **no disconnect event ever fires**, so the handler never runs. The
  watchdog ICMP-echoes the **default gateway** only while the link believes it is up; after
  `kWdFailToReassoc` (2) consecutive failures (~60 s) it forces **one** `esp_wifi_disconnect()`
  — the endless-retry handler then reconnects with the known-good credentials (so the watchdog
  never calls `esp_wifi_connect()` itself, avoiding a cross-task double-connect). On a wired
  link the same verdict restarts the Ethernet MAC (`esp_eth_stop`/`esp_eth_start`), which
  re-runs auto-negotiation and re-requests DHCP — the wired ghost is a switch port that still
  reports link while forwarding nothing, and it fires no `ETHERNET_EVENT_DISCONNECTED` either. A
  stack that
  *gave up* needs no help here — that path is owned by the handler. Two guards keep it from
  ever harming a healthy link: it acts **only** when the link still believes it is up (a known-
  down link is the handler's job, and forcing a disconnect there would only churn the shared
  WiFi/BLE radio), and **only** if the gateway has answered ICMP **at least once**
  (`s_gw_ever_reachable`) — a gateway that never replies (a router/firewall dropping LAN ICMP)
  is treated as "ICMP not a usable signal here", never as "link dead", so it cannot trigger a
  perpetual ~60 s re-association loop. The probe **fails open only on its own setup failure**
  (watchdog not yet initialised, unparseable gateway, or `esp_ping` session-create error →
  "reachable"); a *missing* DHCP lease/gateway, or a gateway that is up but does not answer
  echo, is treated as unreachable — which is why the baseline guard matters. It
  **never reboots** — a reboot during an AP outage would hit the 30 s boot timeout and drop
  into the setup portal, abandoning good credentials. (Implementation note: the ICMP probe's
  control block, callback arguments and semaphore are file-scope persistent and shared through
  `ping_probe_run` by the network watchdog and Syslog reachability check. Each session owns an
  exact monotonically increasing generation. On timeout the caller requests `esp_ping_stop` and
  waits a second bounded interval; if the matching `on_ping_end` still has not arrived, the handle
  remains quarantined as `PendingEnd` and no delete, new session or generation reuse is allowed.
  A later call may clean up only after that exact generation ended; stale/out-of-order callbacks
  cannot complete a replacement waiter. Create/start failures abandon a generation only while no
  callback-capable worker was started. This prevents both the former stack use-after-free and a
  delete/reuse race with esp_ping's asynchronous worker.)

## Sleep / link-state (the single source of truth)

**sleep_state** comes from `VehicleController::link_state()` — the *single* source of truth
shared with the web UI so the two never drift. Four published values:
`AWAKE` (fresh live infotainment telemetry, < 60 s), `ASLEEP` (no live data AND **proven,
debounced** sleep — the car's own VCSEC sleep flag, read from the library's
`Vehicle::sleep_state()` and sampled in `loop_task`, has held `ASLEEP` for ≥ `kAsleepDebounceS`
≈ 120 s while still reachable, so a Cabin-Overheat-Protection `AWAKE↔ASLEEP` flap (~60 s)
can't trip it), `IDLE` (reachable over BLE but **not provably asleep** — we stopped polling
the infotainment domain to let the car sleep and the VCSEC flag hasn't confirmed; we honestly
don't know, so we never claim sleep), and `UNREACHABLE` (the car answers *nothing* over BLE ⇒
driven off / out of range / deep sleep). Nothing heard since boot/re-pair ⇒ omitted so HA
shows "unknown" (strictly: the state topics are retained, so until the first post-reboot
publish replaces them HA may still show the pre-reboot value; a fresh install shows
"unknown" immediately). **Asymmetry (important):** `link_state()` trusts the VCSEC flag's *debounced
`ASLEEP`* as positive proof of sleep, but **never** trusts its `AWAKE` reading to claim
`AWAKE` (a parked car reports VCSEC `AWAKE` while its infotainment sleeps — the old
`wake_up()` trap); `AWAKE` still requires live infotainment telemetry, so a wrong VCSEC
`AWAKE` can only leave us in `IDLE`, never falsely `AWAKE`. The raw (un-debounced) flag is
also surfaced as `vcsec_sleep` in `/status` for diagnostics.

The web UI mirrors this exactly: it shows the "Vehicle asleep" hero (with the wake button)
**only** when `ASLEEP` is a proven fact; for `IDLE` it shows a neutral **"Parked"**
card (last-known SOC + idle time + the same wake button) that makes no sleep claim; and for both
`UNREACHABLE` *and* the unknown state (nothing heard since boot — the on-demand BLE link hasn't
reached the car yet) it **hides the hero card entirely**. Both states know nothing current about the
car, and a hero filled with a retained battery percentage and an idle timer reads as live status;
withholding the card is the honest form, and the state is still signalled — never as a sleep claim —
on the BLE row. In that same
unknown/unreachable state the BLE connection row drops its green and animates an orange
ping-pong across the signal bars (a darker-orange crest bouncing edge→edge over a light-orange
base) with an orange MAC, flagging "connected but stateless" at a glance. The momentary BLE row
reading "Disconnected" is normal (the link is dropped between polls by design) and is not used
to drive the hero — only `link` is.

**BLE phase countdown (the Bluetooth row's "(7s left)" / "(retry in 22s)").** The row names
which phase the radio is in and counts it down, so an idle-looking device visibly explains
itself instead of sitting on a static label. `/status.ble` carries `phase` + `phase_s` — always both or
neither — decided by the pure, host-tested `main/logic/ble_phase.hpp` from two
independently-armed deadlines:

- **`connecting`** — a scan/connect attempt is running and gives up in `phase_s`. Armed by
  `ensure_connected_` (`vehicle_commands.cpp`), the ONE place any attempt is started and
  bounded, so every command, telemetry poll and health probe gets the countdown for free.
- **`waiting`** — no attempt is running; the next one starts in `phase_s`. Armed by
  `idle_until_next_health_poll_` (`vehicle_pairing.cpp`), which owns both the wait and the
  countdown for it — they come from one constant, so the row cannot promise a retry at a time
  the loop doesn't retry.

The two phases **overlap** routinely: a command, or `loop_task`'s warm-up connect, starts an
attempt in the middle of auto-pair's idle wait. `connecting` therefore outranks `waiting` (the
attempt is the more specific truth), and because neither deadline clears the other, the idle
wait's countdown reappears when the attempt ends instead of the row going bare. In the web UI
each row's countdown node declares the one phase it will render, so "Searching…" can never be
suffixed with the *retry* countdown.

**The row's state is decided by a presenter, not inline in the UI.** `main/logic/ble_row.hpp`
(`tk::ble::decide`) maps the raw `/status` fields — deriving "is there a VIN" and "is the link known"
itself, so no untested adapter sits between the JSON and the verdict — to one of six row states plus the countdown
that belongs beside it, and `main/www/app.js` only renders that decision. The two are kept
identical by `scripts/check-ble-row-parity.sh`, which dumps the C++ decision over an exhaustive
input sweep and re-decides it with the JavaScript that actually ships — the same arrangement
`display_model.hpp` has with `tools/display_sim.py`. This row was the last UI surface still
deciding "what to show" in untestable browser code, and it is where three rounds of user-visible
bugs landed.

**The label follows the phase, not `ble.scanning`** — enforced structurally: `scanning` is not a
field of `RowInputs` at all, so a label driven off it is unrepresentable rather than merely
tested-for. The radio also runs a background warm-up
scan (`loop_task`) that has no deadline of its own and lasts straight through the idle wait, so
keying the label off the raw scanning flag flipped the row to "Searching…" in the middle of a
"retry in …" countdown — label and number describing different phases, which read as the row
jumping around. `phase === "connecting"` is now the ONE thing that says "Searching…"; everything
else is "Disconnected". Each row's countdown node names the single phase it will render, so a
mismatch shows nothing rather than a foreign number.

The time sits at the row's **right edge** (`margin-left:auto`), in the column the tile rows put
their edit pencil in, and stays muted in every phase so it reads as one steady right-hand column
instead of recolouring with the label. The disconnected row draws **outlined, unfilled** signal
bars — an empty gauge rather than a dimmed reading; the searching row uses the same amber
(`--warn-base`) the "link up, nothing known yet" bars already use, so the BLE row
has one amber "in-between" language. Wi-Fi's own search stays green.

`phase_s` rounds **up** and `0` is a real value meaning "right now" — never "no countdown".
Gating on `> 0` (or truncating) is what made the first cut of this drop its last second and
flash a bare "Disconnected" between every cycle. `app.js` ticks the number down locally once a
second between the 4 s `/status` polls, resyncing to the device only on a phase change or a
≥2 s disagreement so the two clocks can't jitter the number back upward; it paints into a
dedicated node with `textContent`, because rewriting the row through `setHTML` every second
would re-create the bar `<rect>`s and restart their CSS fill animation on every tick.

Not every scan is a phase. `loop_task`'s warm-up connect calls `BleClient::connect()` directly
rather than going through `ensure_connected_`, so it arms nothing and just leaves the radio
scanning, on and off, straight through the idle wait — under the label-follows-phase rule that
correctly reads as "Disconnected · retry in …", because the link *is* down and the next real
attempt *is* scheduled. The one row that shows "Searching…" with no countdown is the no-VIN
listing-only scan, which has no pairing schedule to count down at all.

**Connection-failure detection (web-UI hero "Connection failed").** When the target car's
advert is heard but the BLE link won't come up after repeated tries, `/status.ble` carries
`connect_fail` (consecutive recent failures; only while actively failing) and `car_connectable`
(the target advert's connectable flag). `car_connectable=false` ⇒ the car advertises
**non-connectable** ⇒ it is at its ~3-device BLE-connection limit — mirroring tesla-ble's
upstream `vehicle-command`, whose BLE transport raises `ErrMaxConnectionsExceeded` off the same
`Connectable` flag (the *connect timeout itself* carries no reason). The web UI shows a
"Connection failed" hero (orange Bluetooth glyph) — "too many Bluetooth devices connected" when
`car_connectable=false`, else "move closer / disconnect other devices" — in **both** the setup
flow *and* the paired state (a paired device that can't get a slot says so instead of hiding the
hero). The signal windows are 90 s so they stay stable across a paired device's ~30-40 s
health-probe cadence. `/status.ble.devices[]` also carries per-device `connectable`, and the
no-VIN screen lists nearby Teslas (bars · dBm · MAC, sorted by signal) from the periodic
listing-only scan. Hero glyphs: grey Bluetooth = "Set up needed", grey NFC-card = "Pairing",
orange Bluetooth = "Connection failed".

**The same three-way verdict decides what a failed connect LOGS** (`logic/connect_outcome.hpp`,
host-tested). `ensure_connected_()` used to end every unsuccessful attempt with one line —
`E vehicle_ctrl: connection timeout after 10000ms` — and that was wrong twice over. It asserted a
timed-out connect even when the scan never matched the car, i.e. when no connect was attempted at
all; and, since the background health poll retries roughly every 40 s forever with no backoff, a car
parked elsewhere emitted **7117 ERROR lines in a week** (17.–24.07.2026) describing a condition that
was expected, unchanged and self-resolving. So:

| `target_connectable()` | Cause | Background level |
|---|---|---|
| `-1` (no matching advert) | `OutOfRange` — car away/asleep | **warn** — the expected resting state |
| `0` (advert non-connectable) | `AtBleLimit` — at its ~3-device limit | **error** |
| `1` (connectable, connect failed) | `ConnectFailed` | **error** — the two-boards-on-one-car signature |

Rate limit: first occurrence of a cause is logged, then once per monotonic hour
(`kConnectFailRepeatMs`) until the cause **changes** or a connect succeeds. This is deliberately
time-based because the unpaired auto-enrolment path can issue ten probes in a burst. A change of cause is
never suppressed — "the car came back but now the connect fails" is precisely the transition worth
seeing, and folding it into a running streak would hide it for up to an hour. **Foreground attempts
(`ConnectOrigin::Foreground` — an evcc/MCP/user request is blocked on this one) are always ERROR and never
suppressed**, whatever the cause: a request that returned nothing must leave a line behind.
Classification requires both the target-name report and the primary advert carrying its
connectability bit to have been observed since that connect attempt began. A fresh SCAN_RSP can
therefore never lend an older/default connectability bit a fresh timestamp. The separate 90 s
`target_connectable()` history remains intentionally stable for the UI, but cannot turn a stale
previous sighting into a current `ConnectFailed`/`AtBleLimit` log. Raw scan, GAP-connect and GATT-
readiness events are DEBUG-only; the classified command-layer line is the single production
WARN/ERROR signal, so lower callbacks cannot bypass the same volume limit.
The nameless primary usually arrives before the named SCAN_RSP; a fixed allocation-free host-task
cache correlates them by address, including the first report for a new/rotated address.
Suppressed attempts emit no line in the production build, whose compile-time maximum log level is
INFO. Only a diagnostic build compiled with maximum DEBUG can expose the individual attempt lines;
the BLE scan verdict remains readable in `/status.ble` regardless.
Command-completion timeouts use a separate typed policy because provenance is not timeout
semantics: the automatic Whitelist Add Key is expected not to complete and recovers its FIFO at
DEBUG; the authorised background GET_STATUS health probe warns first and then hourly while it
remains unanswered; a timed-out HTTP/evcc/user request warns every time.
The direct HTTP/auto-pair vehicle-status callback follows that same rule (foreground warning,
expected-silent enrolment probe), and any valid signed status response closes a prior health-
timeout run just like a generic command completion.
The unpaired supervisor's three setup reminders follow the same monotonic volume contract: INFO
when enrolment is entered and once per hour while unchanged, DEBUG on the intervening ~38 s rounds.
A background enrolment that merely loses the command mutex race is DEBUG; the same timeout for a
blocked foreground pairing request remains WARN.

Reachability is tracked by a
`last_reachable_ticks_` clock stamped on every successful signed round-trip, incl. the idle
health poll. **last boot** is published as an ISO-8601 timestamp (device_class
`timestamp`) so HA shows an auto-scaling relative "x minutes/days ago" instead of a raw
seconds counter; only emitted once the wall clock is NTP-synced.

## Pairing lifecycle / invalidation

The web UI keys everything (control buttons, SOC) off `paired` (= `has_session()`, the
stored VCSEC session in NVS). Three events invalidate a pairing and force a clean re-pair
so no stale data is shown (`clear_session_and_cache_()` in `vehicle_pairing.cpp`):

1. **Key deleted on the car side** — auto-detected three ways: (a) the **primary** detector,
   the `set_message_callback` observer in `vehicle_ctrl.cpp`, matches a signed-message fault
   (`UNKNOWN_KEY_ID`/`INACTIVE_KEY`/`INVALID_KEY_HANDLE`) — the path that actually fires on an
   already-established (cached) session, e.g. the background charge poll; (b) any reply whose
   message contains `"whitelist"` (`KEY_NOT_ON_WHITELIST`, only during a session-info handshake)
   in `make_result_cb_`; (c) a two-strike `"authentication failed"` honoured **only** for the
   periodic signed VCSEC `health_probe_` (~30 s), so a deletion is caught even with no evcc
   traffic while a role-denied user command can never trip it. Each sets `pairing_lost_`; on
   detection the key is regenerated (the old one is useless), session + cache cleared, and
   pairing restarts.
2. **Key regenerated** (`/gen_keys?force=1`) — `generate_key()` now also clears the
   session + cache and drops the BLE link.
3. **VIN changed** (`/set_vin`) — `reset_for_new_vehicle()` regenerates the key, clears
   session + cache, and forgets the stored `ble_mac` (old car), then reboots. Re-saving
   the same VIN is a no-op for the pairing.

After any of these `has_session()` is false → UI shows "not paired", hides controls/SOC.

`has_session()` is also sampled by the 50 ms vehicle loop. The storage adapter therefore caches
the first successful `session_vcsec` existence probe and updates that atomic cache after every
successful session save/remove; NVS read errors remain uncached and are retried. This avoids a
continuous NVS length probe without making a transient storage fault look durably absent.

The main-task BLE-MAC string is startup-only input. When it is empty, the NimBLE host posts a fixed
LinkUp record only. `vehicle_loop` first applies `Vehicle::set_connected(true)`, then acknowledges
the exact connection generation as command-ready, materializes the fixed peer address after
unlock, and performs the one best-effort NVS persistence attempt outside every shared lock. It
never mutates the startup `std::string` across tasks.

**Session reuse across a reboot needs the wall clock restored first.** The `sess_vcsec`/`sess_info`
blobs in NVS exist so a restart does not cost a fresh handshake, but tesla-ble only accepts a
persisted session younger than an hour, and it measures that as

```c
uint32_t session_age = (uint32_t) time(nullptr) - session.clock_time;   // vehicle.cpp
```

At 1970 that subtraction **underflows** — the age comes out as the raw stored epoch (~1.78e9), which
is comfortably over the 3600 s limit, so *every* persisted session is rejected however fresh it is.
`main.cpp` therefore calls `restore_clock_from_nvs()` (the `last_time` cache written on each NTP
sync) **before** `VehicleController::init()`, not next to the SNTP setup after WiFi where it used to
sit — the restore itself needs no network, so nothing kept it down there. Measured before the fix:
49 boots in the 17.–24.07.2026 syslog, 49 rejections of both domains, the last of them discarding a
VCSEC session that was 43 minutes old. NTP refines the restored clock seconds later; the ordering is
what matters, not the precision.

**A configured VIN gates pairing entirely.** The device targets the car by its VIN-derived
BLE name (`S<hex>C`), so `auto_pair_task` first checks `has_plausible_vin()` (17-char VIN;
`VehicleController::vin_is_plausible`, the same validator the web UI / `POST /set_vin` use).
With no VIN it logs once (`auto-pair: no VIN configured — pairing disabled`) and idles — it
does **not** spin connect attempts, but it *does* run a periodic listing-only discovery scan so
the web UI shows nearby Teslas (sorted by signal) live without a manual `/scan`. `set_target_vin`
is given an empty target so the scanner lists nearby Teslas but never connects or enrols on one.
This is the *design*
that stops the device whitelisting its Charging-Manager key onto an arbitrary nearby Tesla — it
no longer depends on the `"UNKNOWN"` placeholder hashing to a name that happens never to
collide (the placeholder is kept out of the matching path). The web UI already shows "Add the
vehicle VIN below to begin." when no VIN is set, so it never implies pairing without one.

## HTTP request-body and allocator-failure contract

Every normal REST and MCP body enters through `read_body_result()`. Its typed result preserves the
difference between an empty body, a body over the 2 KiB cap, allocation failure and a receive
failure; handlers map those cases deliberately instead of folding them all into malformed JSON.
Persisted configuration routes require a body and map empty/receive/malformed input to HTTP `400`;
REST vehicle commands deliberately accept empty input only where their shared command registry
declares no argument or the legacy optional boolean shape. All REST routes map oversize to `413`
and allocation failure to `503`. MCP keeps JSON-RPC error transport semantics: empty/receive/malformed input is an HTTP-200
`-32700` envelope, oversize is HTTP `413` with `-32600`, and allocation failure is HTTP `503` with
`-32603`. JSON nested beyond 16 arrays/objects is a deliberate request-complexity limit: REST maps
it to `400`, while MCP returns HTTP `200` with Invalid Request `-32600`; it is not a parse error.
Malformed raw UTF-8 inside a JSON string — a bad/truncated/overlong sequence, a UTF-16-surrogate
encoding or a scalar above U+10FFFF — remains a syntax error (REST `400`, MCP `-32700`).
Escaped U+0000 is also rejected before cJSON (REST `400`, MCP `-32600`) because cJSON's
NUL-terminated string API cannot preserve it for exact request-ID correlation.
No rejected request reaches command dispatch or a persistent config mutation.

`logic/json_syntax.hpp` first classifies bounded JSON without allocating, including the explicit
16-container nesting limit, shortest-form raw UTF-8 scalar validation and capture of the original
top-level numeric-ID token. That matters because
`cJSON_Parse()` receives the bounded, explicitly NUL-terminated body and returns null for both
malformed input and allocator failure: syntactically
valid, supported input followed by a null cJSON tree is therefore treated as OOM/`503`, while
malformed, over-nested and NUL-containing JSON retain their distinct mappings above. Body and
parse-tree owners are released after copying the bounded inputs and before any
blocking BLE, probe, NVS or restart path. `logic/config_request.hpp` owns the MQTT/Syslog mutation
order and is tested with load, probe, save, response and restart spies; a failure before the durable
save performs none of the later actions. Empty MQTT/Syslog values remain intentional disable
requests, not missing bodies.

The captive provisioning server is a separate transport boundary. Its `POST /save` does not use
the normal API server's allocated 2 KiB intake: it reassembles at most 1024 body bytes in its fixed
buffer path. An empty body or a declared body above 1024 bytes returns HTTP `400` before form
parsing or persistence; a receive failure is also `400`. The 2 KiB REST/MCP policy must therefore
not be generalized to this setup-only endpoint.

Responses use `JsonBuilder`, whose failure bit is sticky: a failed Create/Add retains ownership
until all stack emitters have unwound, then discards the whole tree instead of leaking a partial
HTTP-200/MCP result. REST and MCP serialize through the same `json_http_reply` production seam;
it does not apply a success status or send until printing completed, and print OOM sets 503 before
the one fixed fallback send. `test/run-cjson-oom-tests.sh` compiles the exact cJSON source from
pinned ESP-IDF v5.5.5 and fails every allocation in the production status emitter, representative
REST/MCP envelopes, their shared reply seam and the parser;
the MQTT companion does the same for retained discovery/state payloads and broker failures. The
tests prove ownership and response policy behind deterministic transport/publish seams. Only the
pinned IDF build compiles the real HTTP/NVS/FreeRTOS integration, so host success is not reported as
on-device runtime evidence.

## MCP endpoint (/mcp)

`main/mcp_server.cpp` exposes the device to MCP (Model Context Protocol) clients — Claude
Desktop/Code, VS Code, or any agent framework — over the existing `esp_http_server` on
port 80, so an LLM agent can read state and drive charging without an extra proxy process.
This section covers the firmware-internal design; the **user-facing integration guide**
(wire examples, client configs, troubleshooting) is [`MCP.md`](MCP.md).

**Transport — Streamable HTTP, stateless profile.** `POST /mcp` carries exactly one
JSON-RPC 2.0 message and is answered with `application/json`:

- No SSE stream and no server-initiated requests — `GET /mcp` returns `405` with
  `Allow: POST`. A long-lived stream would pin one of the few httpd sockets and the
  device has no server-push use case.
- No `Mcp-Session-Id` — every request is self-contained; the `MCP-Protocol-Version`
  header is ignored (nothing version-dependent happens after `initialize`).
- Every request and notification must carry `jsonrpc` as the exact string `"2.0"`. The common
  envelope also rejects duplicate object keys recursively before notification classification or
  dispatch; a unique valid `id` can still correlate `-32600`, while a duplicate id returns null.
- Notifications (`notifications/*`: a message with a method and no `id`) are acknowledged with
  `202 Accepted` and no body, per the transport spec. The server never initiates requests, so a
  client response has no valid role here. A method-less, id-less message (`{}`) is NOT a
  notification — it gets `-32600` so a
  broken client isn't left waiting for a reply that never comes.
- JSON-RPC **batches are rejected** (`-32600`) — protocol `2025-06-18` removed them, and
  the single-message parse keeps the heap cost bounded (2 KB body cap, same as the REST
  endpoints).

**Version negotiation** (`tk::mcp_negotiate_version`, `main/logic/mcp.hpp`): supported
revisions are `2025-06-18` and `2025-03-26`; a request for anything else is answered with
our latest supported revision, per the MCP lifecycle spec (the client disconnects if it
can't proceed). **Methods:** `initialize` (capabilities: `tools` only), `ping`,
`tools/list`, `tools/call`; everything else → `-32601`.

**One spec table drives everything — including the REST surface.** The command registry in
`logic/command_registry.hpp` (`kCommands`, `CmdArg`, `kCmdMaxArgs`) carries each command's
REST name, MCP tool name + description, AND each argument's per-surface keys with ONE
shared `{lo,hi}` bounds pair. The advertised `tools/list` JSON schema, the MCP executor's
validation, and the REST `/command` validation (`http_api.cpp`) are all generated from that
table, so schema-vs-enforcement drift — and any `/api`-vs-`/mcp` disagreement about names
or ranges — is impossible by construction. Surface semantics stay deliberately different:
MCP is strict — an absent required argument OR a present-but-unparseable one is a `-32602`
protocol error (silently defaulting `set_scheduled_charging`'s `enable` would *disable*
the schedule and report success); loose-but-unambiguous encodings are coerced (numeric
strings for ints, 0/1 for bools); supplied integers must be integral and inside the spec bounds
before the int cast (UB guard). REST retains TeslaBleHttpProxy compatibility defaults only for an
absent optional field (`api_default`); a supplied fractional/out-of-range value is HTTP 400. The
registry also owns the one scalar-body
compatibility exception: evcc serializes `charge_start` as JSON `true` and `charge_stop`
as JSON `false`; only those matching command/value pairs are accepted, while other
non-object bodies remain HTTP 400. The explicit safety exception is
`set_charging_amps`: its registry row sets `api_required`, so missing/malformed input is HTTP
400 rather than silently becoming 0 A; a fractional value is also rejected because the Tesla
field is an integer amp limit. All REST command failures retain their compatible JSON result/reason
body but return HTTP 502 instead of a misleading HTTP 200. Both surfaces execute through the single kind→controller
dispatch in `command_exec.cpp`. The registry, method routing, version table, validation and the
shared command-outcome text (`logic/command_result.hpp`, also used by the REST
`/command` reason so the two paths can never diverge) are IDF-free and covered by the
host mock build (`test/test_logic.cpp`, `test_mcp` — including a pin on the `tools/list`
row order, which is the registry's table order). The tool set itself — exactly the
run-on-key charging commands plus cache-only `get_vehicle_state`, role-refused commands
deliberately absent (`mcp_name == nullptr`) — is documented with the full per-tool table
in [`MCP.md`](MCP.md#tools).

**Heap safety:** `tools/list` is the endpoint's largest response (~1.5 KB serialized) and
`cJSON_PrintUnformatted` builds it in one contiguous block — the crash-risk currency on
this heap — so tool descriptions stay terse and the tool set small; the static registry
strings are attached via `cJSON_CreateStringReference` (no per-request strdup of
`.rodata`). The real `tools/list` and cache-state/double-print producers live in
`mcp_json_payloads.hpp`, so the pinned-cJSON matrix executes their growth allocations rather than a
smaller fixture. JSON-RPC numeric ids are checked from the raw token before cJSON rounding and must
be canonical decimal safe integers in `[-9007199254740991, 9007199254740991]` (no fraction,
exponent or negative zero); the materialized value must match and the response uses an internally
generated exact decimal token. Strings are copied into fixed maximum-64-byte storage. Other types,
longer strings and ambiguous duplicate ids are rejected with a null response id.
The same sticky owner used by REST prevents any failed envelope/object adoption from
escaping as a partial result; NULL printing maps to 503. Once method-specific input has been
reduced to static pointers, booleans, enums and fixed argument arrays, the request tree is released
before response construction as well as before a blocking vehicle call. A real-cJSON maximum
2-KiB `tools/list` input canary measures live bytes and proves that padding- and id-dependent
allocations are zero before the
largest response is built. Both handlers are dispatched inside `http_server.cpp`'s `handle_all`
try/catch.

**Security posture:** identical to the rest of the HTTP API — no auth, no TLS, trusted
LAN only (see [`SECURITY.md`](SECURITY.md)). The endpoint grants nothing the open REST
API doesn't already expose; the enrolled key stays Charging Manager only. Client
configuration lives in [`MCP.md`](MCP.md#client-integration).

## Concurrency (normative contract)

This section is the **rule**, not a description: new code either fits it or changes it here
first, in the same PR. Deadlock is this device's worst failure mode — frozen but not
rebooting (no panic, so no reboot), evcc blind, and the polling window stuck open so a
parked car never sleeps.

### Lock hierarchy (`VehicleController`)

The controller uses four FreeRTOS mutexes plus one fixed-data critical-section mux, created or
initialized in `VehicleController::init` / the object definition. The ONE shared,
exception-safe RAII guard is `tk::SemGuard` in [`main/rtos_guard.hpp`](../main/rtos_guard.hpp)
(blocking or finite/zero-wait, exposes `acquired()`); `vehicle_ctrl_internal.hpp` keeps the
historical alias `tk::MutexGuard = tk::SemGuard` plus `tk::InFlightGuard`. Every take/give around
code that can throw (a tesla-ble builder/parser → `std::bad_alloc`, a `std::string` copy) goes
through the guard so the lock is released during stack unwinding, never left held (issue #204):

| Primitive | Kind | Protects |
|---|---|---|
| `command_mutex_` | mutex, RAII | one whole command/query transaction and tesla-ble FIFO generation; for `set_charging_amps`, the action and verifying ChargeState poll are one transaction |
| `vehicle_mutex_` | mutex, RAII (`SemGuard`) | **every** call into the tesla-ble `vehicle_` object (send, `loop()`, `on_rx_data`, `set_connected`) |
| `cache_mutex_` | mutex, RAII, leaf | the `last_known_*` caches (`std::string` members ⇒ an unlocked copy is torn-read UB) |
| `result_mutex_` | mutex, RAII, leaf | the externally visible `last_error_` snapshot read by HTTP/MCP after a foreground command returns |
| `CommandCompletion::sem` | per-request binary semaphore, shared ownership | signals one request-local fixed completion record; a timed-out callback cannot address stack storage or a later request's semaphore |
| `telemetry_pending_mux_` | `portMUX`, innermost, POD only | the fixed latest-value nanopb mailboxes, pending mask and charging-amps feedback generation written synchronously by tesla-ble callbacks |

**Normative order:** semaphore nesting is `command_mutex_` → `vehicle_mutex_`. `cache_mutex_` and
`result_mutex_` are independent leaf locks and are never held while another semaphore is acquired.
`telemetry_pending_mux_` is innermost: only bounded POD copies happen inside it, and code inside the
critical section never takes a semaphore, allocates, logs, parses or calls out. Corollaries, each
load-bearing today:

- `vehicle_mutex_` is held only for the library call itself — **never across a request-local
  `CommandCompletion::sem` wait** (the RX path needs it to deliver the result; holding it would
  deadlock every command into its timeout). Each completion is retained by the waiter and queued
  callback; generation invalidation prevents a late callback from completing a later request.
- `cache_mutex_` is a **leaf**: held only for a plain struct copy/assignment, never while
  calling out (library, BLE, NVS, logging) and never while taking another lock. The synchronous
  tesla-ble state callbacks no longer take it: they publish fixed nanopb latest values, and
  `vehicle_loop` parses/publishes them only after releasing `vehicle_mutex_`.
- `clear_session_and_cache_()` takes `vehicle_mutex_` internally, so it must **not** be entered
  while holding it (non-recursive mutex ⇒ self-deadlock; see the comment at its definition).
- The callback writes only its request-local fixed `CommandCompletion` fields before giving that
  completion's semaphore. After the wait, the command task may allocate/log and publishes the
  user-facing `last_error_` under `result_mutex_`; callbacks never touch that string.
- `cmd_in_flight_` (atomic, `tk::InFlightGuard`) is set only under `command_mutex_`; `loop_task`
  reads it to pause background polls — a flag, not a lock; it orders nothing.

### Task inventory

Application-task priorities are declared **only** in [`main/task_config.hpp`](../main/task_config.hpp)
(`tk::kPrio*`) so relative order is reviewable in one place; stack sizes stay at the
`xTaskCreate` sites with their sizing rationale. Current inventory:

`vehicle_loop` and `auto_pair` are created as one lifecycle unit behind
`logic/task_start_gate.hpp`. Both entry functions may be scheduled immediately, but while the gate
is `Creating` their only permitted operation is a one-tick wait—before TWDT registration, mutexes,
BLE or vehicle access. Two successful creates move the pair to `Running`; if the second create
fails, `Cancelled` makes the first task acknowledge and self-delete, and the creator waits for that
acknowledgement instead of externally deleting a task that could be running on another core. After
the pair is released it still waits on the global runtime-admission gate below. The loop task's TWDT
subscription is RAII-owned and is removed on every unwind before the task can self-delete.

| Task | Priority | Stack | Created in | Purpose |
|---|---|---|---|---|
| `vehicle_loop` | `kPrioVehicleLoop` = 5 | 8192 | `vehicle_ctrl.cpp` (fn: `vehicle_telemetry.cpp`) | drain fixed NimBLE Link/RX events, pump `vehicle_->loop()`, parse deferred telemetry after unlock, rotating NO_WAKE poll, sleep gating, BLE-fault link reset |
| `captive_dns` | `kPrioCaptiveDns` = 5 | 4096 | `provisioning.cpp` | captive-portal DNS (setup-AP mode only; vehicle stack not running) |
| `ota` | `kPrioOta` = 5 | 8192 | `ota_update.cpp` | OTA download + flash (transient) |
| `ota_chk` | `kPrioOtaCheck` = 5 | 8192 | `ota_update.cpp` | OTA manifest check (transient) |
| `auto_pair` | `kPrioAutoPair` = 4 | 8192 | `vehicle_ctrl.cpp` (fn: `vehicle_pairing.cpp`) | pairing supervisor: enrol / re-pair / health probe |
| `net_wd` | `kPrioWifiWatchdog` = 4 | 3072 | `net.cpp` | ghost-link watchdog (force the active transport to re-establish, never reboot) |
| `mqtt_pub` | `kPrioMqttPub` = 4 | 6144 | `mqtt_ha.cpp` | MQTT/HA publisher (reads the caches) |
| `display` | `kPrioDisplay` = 3 | 6144 | `display.cpp` | ST7735 renderer (`CONFIG_TESLA_DISPLAY_ENABLED` builds) |
| `ota_gate` | `kPrioOtaGate` = 3 | 3072 | `main.cpp` | one-shot OTA rollback health gate (polls every 5 s; commits on proven link + INTERNAL largest block ≥4 KiB past 90 s under the shared owner, gives up at 600 s) |
| `safe_gate` | `kPrioOtaGate` = 3 | 2560 | `safe_mode.cpp` | one-shot healthy-window timer; after 30 s under the full normal workload, durably clears the crash-boot counter, while latched safe mode never creates it |
| `syslog_task` | `kPrioSyslog` = 3 | 6144 | `syslog.cpp` | best-effort UDP Syslog forwarder (opt-in; degraded-not-fatal on a failed start) |
| `led` | `kPrioLed` = 2 | 3072 | `led_status.cpp` | APA102 status LED (`CONFIG_TESLA_LED_ENABLED` builds) |

Not in the table (ESP-IDF-owned, priorities from IDF Kconfig, not `task_config.hpp`): the
**NimBLE host task** — the two project adapters that cross into `VehicleController`
(`ble_link_event_cb_`, `ble_rx_event_cb_`) only copy bounded bytes/POD into the statically backed
deferred queue; they never call `Vehicle`, NVS, logging or an allocating parser. The surrounding
GAP/GATT lifecycle callbacks still perform bounded parsing, DEBUG diagnostics and synchronous
NimBLE submissions, but every lifecycle mutex attempt is zero-wait and every failure drops/retries
fail-closed instead of blocking the host. `vehicle_loop` owns `Vehicle::set_connected`,
`on_rx_data`, final ready publication and deferred telemetry parsing. The **esp_http_server task** runs every
HTTP/MCP handler, i.e. the `command_mutex_` cycles and cache copies; plus the usual esp_timer /
WiFi / LwIP system tasks.

### Live stack-headroom evidence

`main/stack_watch.cpp` makes the task inventory's second memory budget observable before a crash.
It samples `uxTaskGetStackHighWaterMark(nullptr)` from the owning task only and retains the lowest
free value, in ESP-IDF bytes, for the current boot. Four allocation-rich paths are watched:

- `httpd`: once on every request exit, including exception/OOM fallbacks;
- `vehicle`: at every 50 ms loop boundary;
- `auto_pair`: at every pairing-supervisor round; and
- `mqtt`: at every 500 ms publisher loop boundary.

The FreeRTOS value is already retrospective, so the sample after a deep call still contains that
call's minimum; it need not run at the exact deepest instruction. `/status.sys.stack_min_free_bytes`
and the MQTT device payload expose the same cached values. The reporting period is **this boot**;
the first request cannot report its own not-yet-completed path, and a task not started yet (or
deliberately absent in safe mode) is omitted. Presence is stored separately from the measurement,
so a genuine zero-byte high-water mark remains visible as the critical value it is. These are
measurements, not universal alarm thresholds: stack sizes and call paths differ across the four
supported targets. The manual bench-report gate nevertheless needs more than a one-byte
near-overflow to call a run acceptable. It therefore reserves one eighth of each configured watched
task stack: `httpd` and `vehicle` must each retain at least 1024 B and `mqtt` at least 768 B;
all three must be present for every normal/final profile. `auto_pair` is optional because a valid
already-paired run need not sample it, but when reported it must retain at least 1024 B. These are
reviewed acceptance-policy margins derived from the configured 8192/8192/6144/8192-byte task
stacks, not claims of hardware-derived universal alert floors. Recovery still omits vehicle/MQTT
during its latched safe-mode phase, then its required separate non-fault reboot supplies the final
normal-boot snapshot to which the three-task rule applies. Its default-branch workflow materializes a closed
`report-json` dispatch input, checks schema/plausibility and equality to the separately entered
source hash, firmware hash, profile and target, fingerprints the validated JSON, then uploads that
exact file without an intervening step. The report SHA-256 and resulting Actions artifact
ID/archive digest identify and protect the report only: stack readings, NVS
preservation, firmware signature verification, source/artifact identity and physical execution are
operator declarations, not independently measured or cryptographically proven by the ingest.
Schema v2 records `initialBootFailCount` and `finalBootFailCount`. The recovery profile must begin
and end at zero, perform at least four fault resets to reach the safe-mode latch, then a separate
non-fault reboot to clear it—at least five planned reboots in total.

### Atomics doctrine

A member is a `std::atomic` **only** when it is a single scalar — flag, counter, or tick
stamp — crossing tasks with no multi-field consistency requirement (`pairing_lost_`,
`cmd_in_flight_`, `cmd_fail_streak_`, `last_contact_ticks_`, …). Anything read or written as
a *group* — above all structs holding `std::string` — goes under a mutex (`cache_mutex_` for
the caches). The test: if two fields must be observed consistently together, that is a mutex,
not two atomics.

Cross-task **file-scope** scalars follow the same rule outside `VehicleController`: WiFi
connected / ever-connected / gateway-reachable / NTP-synced (`main.cpp`), diag verbosity
(`diag_log.cpp`), BLE `want_connect_`/`connecting_`/`scanning_`/`host_synced_` + the connect-fail
counter/stamp (`ble_client.hpp`), and MQTT `configured`/`tls`/`connected` (`mqtt_ha.cpp`) are all
`std::atomic` (simple seq_cst) — `volatile` blocks some optimizations but is **not** a
happens-before edge under the C++ memory model.

### Exception containment (normative)

C++ exceptions are enabled and the heap is tight, so `std::bad_alloc` (and library throws) are
**reachable**, not theoretical. An exception that escapes into an ESP-IDF / FreeRTOS / NimBLE /
esp-mqtt / esp_http_server / SNTP **C frame** unwinds through non-exception-aware code →
`std::terminate()` → `abort()` → reboot (and a reboot loop re-opens the poll window, so a parked
car never sleeps). The rule, by execution model:

- **Long-running FreeRTOS tasks** (`vehicle_loop`, `auto_pair`, `mqtt_pub`,
  `syslog_task`, `display`, `led`) wrap their **iteration** in `try { … } catch (std::exception&)
  catch (…)`, log the component, preserve invariants (RAII locks release on unwind), delay briefly
  so no tight error loop forms, and continue with the next iteration.
- **One-shot jobs** (`ota_chk`, `ota`) convert a throw into a terminal **error state** visible in
  `/ota/status` — never a reboot.
- **C callbacks** (NimBLE GAP/GATT + RX, MQTT, SNTP) catch locally or pass a mechanical
  fixed-buffer/POD/atomic audit and return a valid API result. Persistent tesla-ble state callbacks
  are thin adapters to fixed latest-value mailboxes; the dynamic status and command-result
  callbacks publish POD/fixed text only. The command-result callback additionally **always** gives
  its request-local `CommandCompletion::sem` after its catch-all, so the foreground waiter is
  released without logging or allocating while `vehicle_mutex_` is held. NimBLE-host and
  `esp_timer` lifecycle callbacks use only zero-wait mutex attempts; catch-all containment is not
  treated as protection against a blocked callback.
- **Critical boot** (`app_main`) has a top-level boundary; anything that escapes it is logged and
  enters the same fatal-startup policy as an explicitly failed essential component instead of
  reaching a bare `abort()`.
- Every boundary has a terminal catch-all (`catch (...)`) because third-party code is not guaranteed
  to throw only standard exception types. Task and one-shot paths may first log
  `catch (std::exception&)`; C ABI callbacks may intentionally use catch-all only when their only
  safe response is a fixed valid API result.

`test/test_runtime_boundary_contract.py` first derives the shipped C++ inventory from the literal
`main/CMakeLists.txt` `SRCS` block, including nested `.cpp`/`.cc`/`.cxx` paths, then derives every
FreeRTOS task and reviewed C callback from the actual registration calls and callback-bearing
structs, including address-of spelling and the `esp_log_set_vprintf` hook. Inline/runtime-selected
callbacks and non-literal source registration are rejected; each boundary either has
a direct/delegated catch-all or passes a mechanical fixed-buffer/C/atomic call audit. Mutation
canaries add a registration, remove a catch/lifetime release, introduce a dynamic callback or a
throwing call, restore NimBLE→Vehicle re-entry, parse/log/allocate inside a tesla-ble callback,
publish BLE readiness before the deferred Vehicle acknowledgement, reuse stale charging-current
feedback, materialize OTA strings under the status lock, race crash dismissal with its immutable
string/vector snapshot, register a nested `.cc` source, bypass sticky cJSON construction, remove the real
`/status` emitter or any production MQTT builder/sequencer seam, bypass persist-before-restart,
move either vehicle task across its dual/global admission barriers, restore external task deletion,
remove TWDT unwind, bypass either production ping user, or reorder/bypass bounded OTA body/JSON/
version validation.
All shipped sources plus headers are scanned for raw cJSON Create/Add bypasses outside the single
`JsonBuilder` implementation. The companion
runtime binary exercises diagnostic generation changes after unlock, every partial MQTT-probe
ownership stage, safe-mode NVS failures/exceptions, and heap-breadcrumb save/read/erase/malformed
failures. It also compiles the production ping lifecycle against deterministic esp_ping/FreeRTOS
stubs for create/start failure, reply/no-reply and timeout→stop→late-end quarantine with stale
callback rejection. This is structural/direct host evidence; the four-target IDF build remains the
integration gate for the actual C frames.

### Startup failure policy (normative)

Component start/init functions **report success/failure**; `app_main` classifies them and never
runs a partial system that still announces itself as "running" (issue #204):

`logic/runtime_admission.hpp` is the fail-closed cross-task latch for that rule. Boot starts in
`Booting`; only after every essential service and both vehicle task allocations succeed may
`app_main` make the one-way transition to `Ready`. Safe mode transitions to `SafeMode`, while any
fatal startup path stores `Fatal`; neither terminal state can later be promoted. The two vehicle
task entries wait without touching the car until `Ready`; vehicle-active HTTP/MCP routes return
503 and the shared command/telemetry entry points refuse work unless the same gate is ready. The OTA
health task also treats `Booting`, `SafeMode` and `Fatal` as non-health evidence, so a partial boot
cannot spend rollback merely because its timer survived.

NimBLE has an additional acknowledgement boundary because the pinned ESP-IDF wrapper starts its
hidden host task through a void function that cannot report task-create failure. `ble_client.start`
does not admit the essential service until the real host sync callback wins a bounded
`logic/nimble_start_gate.hpp` transition. Timeout is terminal, so a late callback cannot resurrect
boot. OTA health snapshots that positive sync and a saturating per-boot reset counter, then refuses
confirmation if the host is no longer synced or any reset occurred after admission.

- **Essential** — `config`/`tesla_ble` NVS, `VehicleController::init` (its sync primitives + tasks),
  the WiFi event group/station netif/watchdog semaphore+task, NimBLE and its BLE mutex/timer
  resources (`ble_client.start`), the primary HTTP server (`http_server_start`, which unwinds a
  partial handler registration and stops the server), and the OTA health-gate task. A failure calls
  `boot_fatal()` and follows a state-aware policy: a still-`PENDING_VERIFY` image is explicitly
  marked invalid and rebooted into the previous slot immediately; an already-valid image **halts**
  and preserves diagnostics until an external reset. The latter deliberately avoids a permanent
  startup reboot loop, which would repeatedly reopen the vehicle polling window without repairing
  a hard allocation/init failure.
- **Optional** — the MQTT bridge (`mqtt_ha_start`), Syslog forwarding (`syslog_start`), and the
  on-device display/LED when enabled. MQTT and Syslog contain allocation exceptions at their public
  start boundary, unwind partially-created client/task-support resources, are **logged**, and
  degrade to disabled; their public status must not claim they are operational. Boot continues —
  the primary BLE/HTTP proxy runs regardless.

### Deferred: owned BLE-ops queue

The structural alternative to the flags above: one owner task serializing *all* tesla-ble
access via message passing, replacing the `command_mutex_`/`vehicle_mutex_`/`cmd_in_flight_`
coordination by construction and giving commands true queue priority over background polls.
**Deliberately deferred** (architecture review 2026-07, P7): the current compensations
(`cmd_in_flight_` poll pause, `cmd_fail_streak_` link-drop backstop) are live-tested and
stable, the command surface is not growing, and the rework would touch the most
incident-prone code for a structural — not behavioural — win, while costing static
task+queue memory on the tightest targets. **Revisit triggers:** (a) a new command class
lands (e.g. upstream tesla-ble registers `scheduledDepartureAction`), or (b) another
queue-position incident occurs despite `cmd_in_flight_`.
