# Technical feature catalog

What this firmware implements at the *platform* level — the ESP-IDF capabilities, security
mechanisms and diagnostic surfaces that are not specific to Tesla, BLE or evcc. It is the answer to
"does this device do X, and where does X live?", asked by someone who has not read the whole tree.

The Tesla-specific narrative (pairing, link state, telemetry, the MQTT entity list) lives in
[`ARCHITECTURE.md`](ARCHITECTURE.md); the always-needed essentials are in
[`../.claude/CLAUDE.md`](../.claude/CLAUDE.md). **Keep this file current when a technical feature
lands or changes** — the `feature-docs` skill does that, and `.claude/hooks/require-feature-docs.sh`
gates a PR whose diff reaches one of the areas below.

Each entry says what the feature IS, where it lives, and — the part that is easy to lose — *what
failure it exists to prevent*. A feature without that last part is one nobody can safely delete.

---

## 1. Boot, crash forensics and recovery

| Feature | Where | Prevents |
|---|---|---|
| Reset-reason capture | `main/diag_crash.cpp`, `main/logic/reset_reason.hpp` | A reboot nobody can attribute. The reason is sampled once at boot and reported on `/status.sys.reset_reason`, over MQTT and in the syslog boot line. |
| Core dump to flash | `sdkconfig.defaults` (`ESP_COREDUMP_ENABLE_TO_FLASH`), `partitions.csv` (`coredump`, 48 KB at `0x12000`), `main/diag_crash.cpp` | A panic leaving no artifact. The crashed task, PC, backtrace and app-ELF hash are parsed ONCE at boot; the raw image streams from `GET /coredump` for offline symbolisation. |
| **Backtrace is Xtensa-only** | `main/diag_crash.cpp` (`CONFIG_IDF_TARGET_ARCH_XTENSA`) | Publishing raw stack words as if they were return addresses. IDF declares `esp_core_dump_bt_info_t` per ARCHITECTURE: Xtensa carries an unwound PC array, RISC-V a raw stack dump (no windowed registers, so IDF cannot unwind on-device). On esp32/esp32s3 `/status.last_crash` carries `backtrace[]`; on esp32c3/c6 it does not — reason, task, PC and the ELF hash still do, and the full stack is in the downloadable dump, where an offline decoder unwinds it properly. |
| Orphan-dump erase | `logic/crashinfo.hpp` `coredump_is_foreign()` | Advertising a download the decoder will reject: the coredump partition survives an OTA, so a dump can belong to a previous build. Declared foreign only on PROOF, since the erase destroys the one artifact a panic left. |
| Crash report dismissal | `POST /crash/dismiss`, `logic/crashinfo.hpp` `crash_erase_permits_dismiss()` | A crash banner that comes back on every page reload. Erase first, mark second — a dismissal must never outlive a failed erase. The one erase result that does NOT block it is `ESP_ERR_NOT_FOUND`: the device has no `coredump` partition, which is the state of every OTA-upgraded board, and there is simply nothing to destroy. Treating that as a failure answered 500 to every dismissal on those devices, so a fault-reset report could never be acknowledged — the exact failure this row exists to prevent. Every other error still blocks, because then a dump may still be downloadable. |
| Boot-loop safe mode | `main/safe_mode.cpp`, `logic/boot_guard.hpp` | A device that crashes its way out of reach. After 4 consecutive crash boots it starts WiFi + web UI + OTA only, skipping BLE/vehicle and MQTT, so recovery is a browser away instead of a USB cable. Also stops each boot re-opening the car's polling window, which is what drains a parked battery. It **latches**: the healthy-boot timer that clears the counter is not armed while safe mode is active, because surviving 30 s with the crashing subsystems switched off is evidence about the recovery surface, not about the fault — arming it there would give a 4-crashes-then-one-quiet-boot cycle rather than a latch. Any non-fault reset (an OTA install, a `/set_*` save, a power-cycle) zeroes the counter, so it ends when someone acts on it. |
| Heap watchdog | `logic/heap_watchdog.hpp`, sampled in `vehicle_telemetry.cpp` | The wedge that is worse than a crash: internal `largest_block` under 4 KB for 5 unbroken minutes ⇒ deliberate restart with a `reboot_why=heap:<n>` breadcrumb, capped at 5. |
| Task watchdog (TWDT) | `sdkconfig.defaults` (60 s, `PANIC=y`), subscribed by `vehicle_loop` and `mqtt_pub` | The half the heap watchdog cannot see: a task blocked forever on a semaphore, the BLE stack or a socket, with the heap looking perfectly healthy. |
| Stack-overflow watchpoint | `sdkconfig.defaults` (`FREERTOS_WATCHPOINT_END_OF_STACK`) | A sparsely-writing frame stepping over the canary and corrupting a neighbour, so the crash surfaces later somewhere innocent. The watchpoint panics at the offending instruction. |
| OTA rollback health gate | `logic/health_gate.hpp`, `main.cpp` `ota_health_gate_task` | Committing an image on the wrong evidence. Health is a PROVEN LINK plus a 90 s uptime floor, not uptime alone — so both the image that dies under load AND the image that boots perfectly and never gets online are reverted. The second is the one an OTA cannot repair on its own (the repair would have to arrive over the link it broke), it survives any pure timer without difficulty, and the previous 90 s sleep therefore sealed it in as valid. Past a 600 s cap an unhealthy image is LEFT pending — no self-restart, because that would turn a long router outage into a silent downgrade of a good build — so the next reboot rolls it back, while any deliberate `/set_*` save commits it. A device with no credentials and no wire is in setup mode, which is legitimately offline and counts as healthy. |
| Essential-startup failure | `main/boot_fatal.hpp`, defined in `main.cpp` | A half-initialised firmware pretending to run. When something essential fails to come up (NVS, the vehicle controller, the LAN watchdog, a transport's event group) a PENDING OTA image actively rolls back — merely parking the task would wedge the device on an unverified slot until someone resets it — while an already-valid image HALTS instead of rebooting, because a reboot loop re-opens the car's polling window on every boot and erases the in-memory diagnostics that would explain it. |

## 2. Configuration and storage

| Feature | Where | Prevents |
|---|---|---|
| Atomic CRC config blob | `logic/config_store.hpp`, `main/config_blob.cpp` | A torn credential state. WiFi credentials + the rollback backup + VIN + MQTT + syslog are ONE CRC-checked `nvs_set_blob` — all-or-nothing across both a write failure and a power cut. |
| Legacy per-key fallback | `main/config_blob.cpp` `cfg_load()` | Stranding every deployed device's WiFi and VIN on the first OTA that introduces the blob. The legacy keys are also mirrored on save, so a downgrade still finds its configuration. |
| `[[nodiscard]]` NVS writes | `main/nvs_storage.hpp` + `-Werror=unused-result` | A silently dropped write of something the device cannot rebuild — the private key, the VIN, the safe-mode counter. |
| WiFi credential rollback | `logic/wifi_rollback.hpp`, `main.cpp`, `POST /set_wifi` | An unrecoverable typo. A bad credential change self-heals back to the last working network; only an AP that SUSTAINS an auth refusal spends the new credentials, while an absent SSID gets a 180 s grace. |
| Config survives OTA | `partitions.csv` (`nvs` at `0x9000`) | Losing pairing/keys on upgrade. Keep the offset and size stable across versions. |
| MQTT broker test-before-persist | `logic/mqtt_uri.hpp`, `main/http_config.cpp` (`POST /set_mqtt`) | Learning a broker is wrong only after saving it and rebooting. A CHANGED, non-empty broker is CONNECTED to first, and only a broker that accepts the CONNECT is written — 400 separates "your credentials are refused" from 502 "nothing answered", because collapsing them makes an outage look like a typo. It dials the URI `mqtt_effective_uri()` derives, which is the one the bridge dials at boot: a probe against a separately-derived URI would be a green check for a connection that never happens, and the rule it shares is not one to retype — it silently decides whether the password crosses the network in the clear. The probe also REFUSES ITSELF when the largest contiguous INTERNAL block cannot afford a second mbedTLS session (~48 KB for TLS): a 503 costs the user a retry, attempting it costs a `bad_alloc` on the httpd task. |

## 3. Security

| Feature | Where | Prevents |
|---|---|---|
| Signed OTA (Secure Boot v2 RSA-3072, no hardware Secure Boot) | `sdkconfig.defaults`, `scripts/ci-build-all.sh` | A compromised update host pushing unsigned firmware. Reversible, no eFuses burned. Details: [`SECURITY.md`](SECURITY.md). |
| Downgrade gate | `main/ota_update.cpp` | A signed but OLD image carrying a since-patched flaw. A signature proves authenticity, not freshness. |
| Manifest ↔ image version match | `main/ota_update.cpp` | The manifest and the `.bin` being separately-controlled artifacts: a host advertising 1.9.0 while serving 1.8.0 would install a build nobody published. Both must name the same version, and the manifest is re-fetched on the update path so a direct caller is gated too. |
| Bug-report redaction | `logic/redact.hpp`, `GET /status?redact=1`, `GET /diag?redact=1` | A diagnostic snapshot carrying the reporter's VIN, SSID, IP, BLE MACs, broker or syslog host into a public issue. Fails closed; substitutes the value and keeps the key. `/status` redacts by FIELD, `/diag` by LINE — and the line table has to cover every log statement that interpolates one of those values, including `http_server.cpp`'s request log, whose URI carries the VIN on every evcc poll and is therefore the most frequent VIN sink in the ring (measured: 31 of 286 lines on a live board eleven minutes after boot). A missed sink makes the whole promise false, so a new VIN/SSID/host-bearing log line needs a rule in the same commit. |
| Restricted setup-AP surface | `main/provisioning.cpp` | The open setup AP exposing `/gen_keys`, `/diag`, `/status` or the command API. Only the provisioning routes are registered in AP mode. |
| Charging-Manager-only key role | `vehicle_pairing.cpp` | Enrolling a key with door/body privilege the integration does not need. |

No HTTP auth or TLS on the device API, by design — evcc cannot send credentials, so the device is
trusted-LAN only. See [`SECURITY.md`](SECURITY.md).

## 4. Network

| Feature | Where | Prevents |
|---|---|---|
| Captive portal | `main/provisioning.cpp`, `logic/captive.hpp` | A setup portal that does not pop. A DNS catch-all, a **302 redirect** on the three OS probe paths (a 200 + page is a heuristic Android may leave undecided), and the RFC 8910 DHCP option 114 — three independent ways for a client to find it. |
| SPI Ethernet (W5500) | `net.cpp` (`CONFIG_TESLA_ETH_ENABLED`), `main/Kconfig.projbuild`, `sdkconfig.defaults.esp32s3` | Having to run the BLE proxy on a radio that must share ONE antenna path with BLE. On a wire the WiFi stack is never started — no coexistence arbitration, no `WIFI_PS_MIN_MODEM` tax on every GATT round-trip, and its heap stays free. Polling mode (no INT line on the ATOMIC PoE Base), a positive VERSIONR probe, and a bounded lease wait that falls back to WiFi rather than stranding an unplugged board. |
| One image, three boards | `main/board.{cpp,hpp}` | Two independent copies of "which board is this" answering differently on a SHARED pin: the T-Dongle-S3's panel clock is GPIO5, which is the PoE base's SPI SCLK. One cached detector, and the Ethernet probe is refused on a detected T-Dongle. |
| Transport seam | `main/net.hpp` + `main/net.cpp`, `logic/net_link.hpp` | A network layer that can only ever be WiFi. ONE contract (`net_is_up` / `net_kind` / `net_active_netif`) replaces a predicate hand-`extern`ed in five modules and a `"WIFI_STA_DEF"` ifkey hardcoded in three — each correct only while a radio was the sole transport. |
| Gateway ICMP watchdog | `net.cpp` `net_watchdog_task`, `logic/net_link.hpp` `watch_step()` | The "ghost association" that fires no disconnect event: the stack believes it is connected, the AP forwards nothing, and no reconnect handler ever runs. The decision — including the baseline rule that a gateway which has NEVER answered ICMP must not trigger recovery — is host-tested, not an `if` inside a task loop. |
| Endless runtime reconnect | `net.cpp` `wifi_event_handler` | Surrendering to the setup portal on a transient outage once the credentials are known-good. |
| mDNS + DHCP hostname | `main.cpp` | Having to find the IP. Both are set to the same name, so router DNS agrees with `tesla-key-esp32.local`. |
| SNTP + browser-clock fallback | `main.cpp`, `POST /set_time` | A 1970 clock, which makes tesla-ble reject every persisted session and breaks OTA TLS date validation. |
| UDP syslog (RFC 5424) | `main/syslog.cpp`, `logic/syslog_policy.hpp` | Losing the log that explains a reboot — `/diag` is RAM and does not survive one. Errno-classified send failures, PRI from each line's own log level. |
| Syslog boot replay | `logic/bootlog.hpp`, `main/syslog.cpp` | The crash record never reaching the collector: it is captured before WiFi exists, so it is replayed once when a destination first resolves, together with a build-identity line. |

## 5. Diagnostics and observability

| Feature | Where | Prevents |
|---|---|---|
| `/status.sys` block | `logic/status_model.hpp` | A primary API that cannot report the number causing its own reboots. Free heap, min-free, largest contiguous block, uptime, WiFi reconnects, reset reason, safe-mode flag — always present. |
| `/status.last_crash` | `logic/status_model.hpp`, `main/diag_crash.cpp` | Having to be attached over serial to learn a device crashed. Emitted only when the boot is notable, so its presence is the signal. |
| 24-hour heap trend (`GET /heap`) | `logic/heap_history.hpp`, `main/heap_trend.cpp` | A spot value that cannot show DRIFT. A leak is a slope; fragmentation is the two lines separating. Fixed ring (~1.2 KB), never heap — a diagnostic must not compete for the resource it measures. |
| **The trend survives the restart** | `main/heap_trend.cpp` (`__NOINIT_ATTR`), `logic/heap_history.hpp` `HeapPersist` | The instrument being erased by the event it exists to explain. The heap watchdog's answer to exhaustion IS a restart, and a `.bss` ring came back empty from it — the reader got the `heap:<n>` breadcrumb saying a slope existed, and no slope. Same for a panic, the task watchdog and an OTA reboot. `.noinit` is DRAM the startup code neither loads nor zeroes, so it survives every reset that KEPT POWER, at zero flash writes (NVS persistence stays rejected: ~100k writes a year for an artifact whose value is the last day) and zero extra RAM. A power cut still clears it, so the retained bytes must PROVE themselves: a CRC-32 (the same routine the config blob uses) rejects SRAM noise, and a derived layout fingerprint rejects an image an OTA reshaped while leaving the bytes valid — either failure starts an empty trend rather than drawing a chart nobody can vouch for. A carry offset keeps ONE bucket clock across the reboot, which therefore lands on a bucket boundary; `/heap`'s `b_boot` names it, because a retained trend that read as one unbroken run would be a chart that lies about a restart. |
| In-RAM diag ring (`GET /diag`) | `main/diag_log.cpp` | Needing a serial cable to read the log. Streamed in chunks, never assembled into one allocation. |
| MQTT device diagnostics | `main/mqtt_ha.cpp` | Reboots and link churn being invisible from Home Assistant. Reset reason (slug AND number, since metrics stores drop strings), safe mode, crash-dump flag, both heap figures, WiFi/MQTT reconnect counters. |
| Golden-pinned `/status` contract | `logic/status_model.hpp` + `test/test_logic.cpp` | The field contract drifting silently under the web UI. |

## 6. Build, test and CI

| Feature | Where | Prevents |
|---|---|---|
| Host mock build | `scripts/run-mock-tests.sh`, `test/` | Reasoning about logic instead of running it. Compiles `main/logic/` with the plain system toolchain — no ESP-IDF, no Docker, no board — so a cloud session has a real verification loop. |
| Warning contract on `main/` | `main/CMakeLists.txt` | An unpinned guarantee: `-Werror=return-type` and `-Werror=unused-result` hold `main/*.cpp` (where every shipped crash happened) to a contract that does not depend on IDF's own defaults. |
| Presenter parity checks | `scripts/check-display-sim-parity.sh`, `scripts/check-ble-row-parity.sh` | The Python display sim and the browser's JS drifting from the C++ presenters they mirror. |
| Four targets, one tree | `sdkconfig.defaults.*`, `scripts/ci-build-all.sh` | Per-chip divergence. CI builds esp32 / s3 / c3 / c6 and size-gates each signed image. The set is exactly what `yoziru/tesla-ble` declares; the Component Manager enforces it. |

### Why there is no static analyser (measured, not assumed)

Read this before proposing one. Measured on this tree with clang-tidy 18:

- **Blanket config**: 3355 findings, dominated by noise — 769 `llvmlibc-callee-namespace`, 599
  `altera-unroll-loops`, and 598 `pro-type-vararg` + 593 `avoid-do-while` which are almost entirely
  this project's own `CHECK` macro.
- **Curated to bug-finding checks** (`bugprone-*`, `clang-analyzer-*`, `cert-*`, `performance-*`):
  **3 findings, ZERO of them in `main/logic/`**. Two are `cert-err33-c` on `snprintf` in the test
  file's golden emitter (the return is the length it *would* have written, into a fixed buffer);
  one is `bugprone-easily-swappable-parameters` on a test lambda.
- **`-Wconversion`**: 3 hits, all `int`→`float` in the two colour-gradient `lerp8` helpers, exact
  over the value range they are called with. **`-Wshadow`, `-Wsign-conversion`**: zero.

The yield is this low for a structural reason visible in the code: the bug classes are typed out
rather than linted out, and the defects this project actually ships fixes for — a wrong link-state
transition, a stale cache served as fresh, a heap budget — are not in a linter's language. So there
is deliberately **no `.clang-tidy` file**: an inert config reads like a guarantee while doing
nothing.

What the survey *did* find was the opposite gap. `main/logic/` was never the exposed half — it has
`-Wall -Wextra -Werror` and 3400+ host-tested checks. `main/*.cpp` was, and it carried no warning
policy of its own. That is now pinned in `main/CMakeLists.txt`.

`-Werror=format` is deliberately **not** in that set: the root `CMakeLists.txt` appends a global
`-Wno-error=format` because the tesla-ble dependency prints `uint32_t` with `%u`, and our own format
strings have never been compiled under it across all four targets (`int32_t` is `long int` on xtensa
and plain `int` on riscv/host). Converting the call sites to `PRIu32`/`PRId32` and then arming it is
a change of its own, with a real build behind it.
