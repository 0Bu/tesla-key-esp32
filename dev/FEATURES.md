# Feature catalog

Find a mechanism, its implementation and the failure it prevents. Each section links its owning
contract; this catalog is not a second narrative. `logic/` paths are under `main/`.

## 1. Boot, crash forensics and recovery

Details: [recovery](ARCHITECTURE.md#boot-loop-safe-mode).

| Feature | Where | Prevents |
|---|---|---|
| Reset-reason capture | `main/diag_crash.cpp`, `logic/reset_reason.hpp` | A reboot nobody can attribute. |
| Core dump to flash | `sdkconfig.defaults` (`ESP_COREDUMP_ENABLE_TO_FLASH`), `partitions.csv` (`coredump`, 48 KB at `0x12000`), `main/diag_crash.cpp` | A panic leaving no artifact. |
| Backtrace is Xtensa-only | `main/diag_crash.cpp` (`CONFIG_IDF_TARGET_ARCH_XTENSA`) | Publishing raw stack words as return addresses. |
| Orphan-dump erase | `logic/crashinfo.hpp` `coredump_is_foreign()` | Advertising a download the decoder will reject (the partition survives an OTA, so a dump may belong to an older build). |
| Crash report dismissal | `POST /crash/dismiss`, `main/diag_crash.cpp`, `logic/crashinfo.hpp` `crash_erase_permits_dismiss()` | A crash banner returning on every reload, or a dismissal racing MQTT/status readers. |
| Boot-loop safe mode | `main/safe_mode.cpp`, `logic/boot_guard.hpp`, `main/vehicle_ctrl.cpp` `init_safe_mode`, `main/www/app.js` | A device that crashes its way out of reach. |
| Heap watchdog | `logic/heap_watchdog.hpp`, `reboot_reason.hpp`, sampled in `vehicle_telemetry.cpp` | A wedge worse than a crash. |
| Task watchdog (TWDT) | `sdkconfig.defaults` (60 s, `PANIC=y`), `vehicle_loop` and `mqtt_pub`; `mqtt_ha.cpp` feeds after each completed publish | The half the heap watchdog cannot see: a task blocked forever on a semaphore, BLE stack or socket while the heap looks healthy. |
| Stack-overflow watchpoint | `sdkconfig.defaults` (`FREERTOS_WATCHPOINT_END_OF_STACK`) | A sparsely-writing frame stepping over the canary and corrupting a neighbour so the crash surfaces somewhere innocent; the watchpoint panics at the offending instruction. |
| Per-task stack-headroom telemetry | `main/stack_watch.cpp`; `http_server.cpp`, `vehicle_telemetry.cpp`, `vehicle_pairing.cpp`, `mqtt_ha.cpp`; `logic/status_model.hpp` | Finding out about frame growth only after the watchpoint panicked. |
| OTA rollback health gate | `logic/health_gate.hpp`, `main.cpp` `ota_health_gate_task`, `main/ota_update.cpp` (`OtaIdentityMutationGuard`, `OtaHealthCommitGuard`, `ota_config_restart_begin`) | Committing an image on the wrong evidence. |
| Essential-startup failure | `main/boot_fatal.hpp` (defined in `main.cpp`) | A half-initialised firmware pretending to run. |
| Atomic vehicle-task start and runtime admission | `logic/task_start_gate.hpp`, `logic/runtime_admission.hpp`, `main/runtime_admission.cpp`; [recovery](ARCHITECTURE.md#boot-loop-safe-mode) | A first task running against partially-created peers, an unsafe external delete on the other core, or a half-boot contacting the vehicle. |

## 2. Configuration and storage

Details: [storage](SECURITY.md#nvs-retention).

| Feature | Where | Prevents |
|---|---|---|
| Atomic CRC config blob | `logic/config_store.hpp`, `main/config_blob.cpp` | A torn credential state. |
| Legacy per-key fallback | `logic/nvs_string_load.hpp`, `main/nvs_storage.cpp`, `main/config_blob.cpp` `cfg_load()`, `cfg_load_for_update()` | Stale or incomplete legacy snapshots overwriting committed identity/settings; one unreadable key blocks mutation and journal recovery. |
| `[[nodiscard]]` NVS writes | `main/nvs_storage.hpp` + `-Werror=unused-result` | A silently dropped write of something unrebuildable: the private key, the VIN, the safe-mode counter. |
| Exact fail-closed NVS registry | `logic/nvs_contract.hpp`, `main/nvs_storage.cpp`, `scripts/check-nvs-contract.py`, `test/test_nvs_storage.cpp` | Unknown or colliding keys, wrong APIs and unreviewed persistence surfaces. |
| Cached VCSEC session presence | `main/nvs_storage.{cpp,hpp}`, `main/vehicle_pairing.cpp` | The 50 ms vehicle loop probing flash-backed NVS to rediscover a stable bit. |
| WiFi credential rollback | `logic/wifi_rollback.hpp`, `main.cpp`, `POST /set_wifi` | An unrecoverable typo. |
| Config survives OTA | `partitions.csv` (`nvs` at `0x9000`) | Losing pairing/keys on upgrade. |
| Shared WiFi credential contract + safe host provisioning | `logic/wifi_credentials.hpp`, `main/provisioning.cpp`, `main/http_config.cpp`; [storage](SECURITY.md#nvs-retention) | Invalid provisioning credentials or a partial NVS image erasing keys and sessions. |
| MQTT broker test-before-persist + credential-free display | `logic/mqtt_uri.hpp`, `main/http_config.cpp` (`POST /set_mqtt`), `main/mqtt_ha.cpp` | Saving an unusable broker or exposing its credentials. |
| Fail-closed HTTP body, JSON and config intake | `logic/http_body.hpp`, `json_syntax.hpp`, `config_request.hpp`; [storage](SECURITY.md#nvs-retention) | Rejected or exhausted requests dispatching commands, persisting unintended configuration, or returning partial success. |
| Single-source HA discovery registry | `logic/mqtt_discovery_registry.hpp`, `main/mqtt_ha.cpp`, `mqtt_payloads.hpp`, `test/test_mqtt_json_publish.cpp` | Entity/topic/type/presence drift between discovery and actual retained payloads. |
| Fail-closed retained MQTT JSON | `main/mqtt_ha.cpp`, `mqtt_json_publish.hpp`, `mqtt_payloads.hpp`; [storage](SECURITY.md#nvs-retention) | Partial retained documents or incomplete reconnect sequences replacing valid readings. |
| Vehicle-stable HA identity | `logic/ha_identity.hpp`, `main/mqtt_ha.cpp` | A replacement ESP32 creating a second HA device and 55 new entities. |

## 3. Security

Details: [security](SECURITY.md).

| Feature | Where | Prevents |
|---|---|---|
| Signed OTA (Secure Boot v2 RSA-3072, no hardware Secure Boot) | `sdkconfig.defaults`, `scripts/ci-build-all.sh`, `ci-sign-artifacts.sh`; [security](SECURITY.md) | A compromised update host pushing unsigned firmware, a changed CI secret replacing the production authority, or an image with a bad signature/extra padding block. |
| Provenance-bound signed PR previews | `.github/workflows/signed-pr-preview.yml`, `pr-preview-cleanup.yml`, `.agents/skills/flash-esp32/SKILL.md`; [security](SECURITY.md) | Signing untrusted, stale or foreign source, or leaving obsolete previews public. |
| Transactional key rotation + VIN journal | `main/vehicle_pairing.cpp`, `logic/key_rotation.hpp`, `logic/vin_transition.hpp`, `main/main.cpp` | Reporting a failed rotation as success, reusing old sessions after a durable replacement key, or a power cut leaving a new VIN with the previous vehicle's key/session. |
| Tesla protocol and patch known-answer gate | `test/tesla_protocol_vectors.test.mjs`, `test/test_tesla_ble_harness.cpp`, `scripts/test-tesla-ble-harness.sh`; [security](SECURITY.md) | A compiling dependency/crypto migration changing wire bytes, key compatibility, response authentication or the harness crypto pin. |
| Generation-bound deferred BLE command readiness | `logic/ble_readiness.hpp`, `ble_deferred_event.hpp`, `main/ble_client.{cpp,hpp}`; [security](SECURITY.md) | Premature GATT readiness, allocating callbacks on the NimBLE host or stale events applied after handle reuse. |
| Deterministic BLE RX/TX framing | `logic/rx_framing.hpp`, `main/vehicle_ctrl.hpp`, `vehicle_telemetry.cpp`, `test/test_tesla_ble_harness.cpp` | Upstream's heuristic recovery loops, offset shifting and log floods on corrupt/split frames, and outgoing frames the car cannot decode (a second length prefix). |
| Bounded BLE command runner | `logic/command_runner.hpp`, `ble_dispatcher.hpp`, `session_state.hpp`; [security](SECURITY.md) | Command timeouts, unbounded queue growth, blocking during prerequisite setup, lost wake arbitration. |
| Acknowledged NimBLE host startup | `logic/nimble_start_gate.hpp`, `main/ble_client.{cpp,hpp}`, `main.cpp`, `ota_update.cpp` | Admitting a missing/late host or confirming OTA after a host reset. |
| Channel-aware eligibility + manifest/image match | `main/ota_update.cpp` | Manifest/image disagreement or candidates outside the selected channel/PR policy; intentional return-to-Release exceptions remain explicit. |
| Bounded exact OTA manifest intake | `logic/ota_contract.hpp`, `ota_changelog_range.hpp`, `main/ota_manifest.hpp`; [security](SECURITY.md) | Ambiguous, truncated or oversized input bypassing version validation or exhausting contiguous heap. |
| Coherent allocation-free OTA status lock | `main/ota_update.cpp`, `test/test_runtime_boundary_contract.py` | A first-use lock allocation failure exposing unprotected status, mixed-generation fields, or `std::string` allocation under the status mutex. |
| OTA update channel (Release vs Dev) | `logic/ota_contract.hpp`, `main/ota_update.cpp`, `http_ota.cpp`; [security](SECURITY.md) | Installing unstable dev builds on production devices, or being unable to test `main` without manual builds. |
| Firmware card: channel modal + on-demand check | `main/www/app.js`, `index.html`, `test/web_ui_http.test.mjs` | Accidental channel selection, unrequested dialogs while up to date, or installing without reading release notes. |
| Bug-report redaction | `logic/redact.hpp`, `GET /status?redact=1`, `GET /diag?redact=1` | A snapshot carrying VIN, SSID, IP, vehicle/nearby BLE MACs, advertised names, broker or syslog host into a public issue. |
| Device-bound browser mutation gate | `logic/http_origin.hpp`, `main/http_server.cpp` | Cross-site or DNS-rebound browser access mutating trusted-LAN device state. |
| Private wake capture | `scripts/capture_wake.py` | A full VIN and raw authenticated BLE frames landing as a commit-ready 0644 file. |
| Restricted setup-AP surface | `main/provisioning.cpp` | The open setup AP exposing `/gen_keys`, `/diag`, `/status` or the command API; only provisioning routes exist in AP mode. |
| Charging-Manager-only key role | `vehicle_pairing.cpp` | Enrolling a key with door/body privilege the integration does not need. |
| No BLE link-layer pairing or bonding | `sdkconfig.defaults` (`CONFIG_BT_NIMBLE_SECURITY_ENABLE=n`), `scripts/check-build-gate-contract.py`; [ATT/SMP departures](adr/0005-tesla-ble-seam.md#2-platform-forced-departures-and-reference-deviations) | Unauthenticated SMP pairing and unused code crossing the C6 signing boundary. Tesla application crypto remains the protection; peer SMP is dropped with `rc=6`, not answered. |

The device HTTP API has no authentication or TLS; use it only on a trusted LAN
([security model](SECURITY.md#http-api-exposure)).

## 4. Network

Details: [network](ARCHITECTURE.md#wifi--lan-connectivity).

| Feature | Where | Prevents |
|---|---|---|
| Captive portal | `main/provisioning.cpp`, `logic/captive.hpp` | A setup portal that does not pop. |
| SPI Ethernet (W5500) | `net.cpp` (`CONFIG_TESLA_ETH_ENABLED`), `logic/eth_board.hpp`, `main/Kconfig.projbuild`; [network](ARCHITECTURE.md#wifi--lan-connectivity) | Running the BLE proxy on a radio that shares ONE antenna path with BLE. |
| One image, multiple boards | `main/board.{cpp,hpp}`, `logic/eth_board.hpp` | Two copies of "which board is this" disagreeing on a SHARED pin (the T-Dongle-S3 panel clock is GPIO5, the PoE base's SPI SCLK). |
| Transport seam | `main/net.hpp`, `net.cpp`, `logic/net_link.hpp` | A network layer that can only be WiFi: one contract (`net_is_up` / `net_kind` / `net_active_netif`) replaces a predicate `extern`ed in five modules and an ifkey hardcoded in three. |
| Gateway ICMP watchdog | `net.cpp` `net_watchdog_task`, `logic/net_link.hpp` `watch_step()` | The "ghost association" that fires no disconnect event. |
| Generation-owned ICMP probe | `logic/ping_probe.hpp`, `main/ping_probe.hpp`, `net.cpp`, `syslog.cpp` | A timed-out esp_ping worker calling into a dead stack frame, a stopped handle deleted/reused before `on_ping_end`, or a late callback completing a newer probe. |
| Endless runtime reconnect | `net.cpp` `wifi_event_handler` | Surrendering to the setup portal on a transient outage once credentials are known-good. |
| mDNS + DHCP hostname, privacy-bounded TXT | `main.cpp` | Having to find the IP, or making the VIN multicast-browseable. |
| Bounded 20-byte BLE writes | `logic/ble_chunk.hpp`, `main/ble_client.{cpp,hpp}` | Invalid chunk sizes or lost framing. MTU is 23 in this build; the negotiated-MTU helper is unreachable. See ADR-0005 for transport departures. |
| SNTP + browser-clock fallback with authoritative sync gate | `main/main.cpp`, `time_sync.hpp`, `POST /set_time`, `mqtt_ha.cpp` | Invalid OTA certificate dates or HA/key/pairing timestamps derived from a stale restored clock. |
| UDP Syslog (RFC 5424) | `main/syslog.cpp`, `logic/syslog_policy.hpp` | Losing the log that explains a reboot (`/diag` is RAM). |
| Commit-gated Syslog queue lifetime | `logic/syslog_start_gate.hpp`, `main/syslog.cpp` | A consumer touching startup-owned resources before creation commits, or the log hook retaining a queue that startup deleted. |
| Syslog boot replay | `logic/bootlog.hpp`, `main/syslog.cpp` | The crash record never reaching the collector — captured before WiFi exists, so it is replayed once when a destination first resolves, with a build-identity line. |

## 5. Diagnostics and observability

Details: [diagnostics](API.md#management).

| Feature | Where | Prevents |
|---|---|---|
| `/status.sys` block | `logic/status_model.hpp`, `main/http_status.cpp` | A primary API that cannot identify the controller or the number causing its reboots. |
| `/status.last_crash` | `logic/status_model.hpp`, `main/diag_crash.cpp` | Needing serial access to learn a device crashed. |
| 24-hour heap trend (`GET /heap`) | `logic/heap_history.hpp`, `main/heap_trend.cpp` | A spot value that cannot show DRIFT (a leak is a slope; fragmentation is the two lines separating). |
| Trend survives restart | `main/heap_trend.cpp` (`__NOINIT_ATTR`), `logic/heap_history.hpp` `HeapPersist` | The instrument being erased by the event it explains: the heap watchdog's answer to exhaustion IS a restart, and a `.bss` ring came back empty. |
| In-RAM diag ring (`GET /diag`) | `main/diag_log.cpp`, `main/http_status.cpp` | Needing serial to read the log, or a slow client holding the ring mutex across network I/O. |
| MQTT device diagnostics | `main/mqtt_ha.cpp` | Reboots, link churn and shrinking stacks invisible from fleet telemetry: reset reason (slug AND number, since metrics stores drop strings), safe mode, crash-dump flag, both heap figures, reconnect counters, per-task stack minima. |
| Golden-pinned `/status` contract | `logic/status_model.hpp`, `test/test_logic.cpp` | The field contract drifting silently under the web UI. |

## 6. Build, test and CI

Details: [build and review](DEVELOPMENT.md).

Every `$project-review` includes the [documentation quality assessment](../.agents/skills/project-review/SKILL.md#documentation-quality-required-in-every-review);
unresolved material findings prevent a clean verdict.

| Feature | Where | Prevents |
|---|---|---|
| Runner-neutral agent policy and five SHA-bound PR gates | `AGENTS.md`, `.agents/`, `.codex/`, `scripts/with-host-python.sh`; [build and review](DEVELOPMENT.md) | Unsafe authority, stale review records, or publishing personal/private identifiers or non-English PR/docs content. |
| Resource-bounded build container | `scripts/idf-docker.sh`, `scripts/check-build-gate-contract.py` | A local ESP-IDF compile starving a shared k3s/Docker node. |
| Fail-closed host verification | `scripts/run-mock-tests.sh --require-all`, `run-sanitizer-tests.sh`, `run-fuzz-smoke.sh`; [test scope](../test/README.md) | Skipped CI coverage, unsafe runtime boundaries, partial JSON, parser UB or browser-only failures. |
| Split host gates and concurrent target builds | `.github/workflows/build.yml` (`logic-test`, `logic-harness`, `build-target`), `scripts/check-workflow-policy.py` | Serial host/firmware waits or an aggregate build that omits a host gate. |
| Concurrent independent rebuild | `.github/workflows/build.yml` (`independent-rebuild-target`, `independent-rebuild`); [publication](SECURITY.md#release-pipeline-and-trust-boundaries) | Serial fresh builds or producer claims reaching the signer without a secret-free byte comparison. |
| Shared-runner browser cold-start budget | `test/web_ui_browser_gate.py`, `scripts/run-mock-tests.sh` | A healthy real-browser gate flaking because a loaded runner needs > 20 s to start Chrome, or a relaxed inner deadline exceeding the outer bound. |
| Repository/workflow lint with mutation canaries | `scripts/repo-lint.sh`, `check-workflow-policy.py`, `check-host-gate-contract.py`, `check-markdown-links.py` | Malformed or unsafe workflows, broken links, invalid ownership and fail-open Stop hooks. |
| Fail-closed Web Serial installer + monitor | `docs/index.html`, `installer-bootstrap.mjs`, `web-installer.mjs`; [build and review](DEVELOPMENT.md) | A dialog hiding chip mismatches, mixed deployments or corrupt downloads, or a pre-ECO3 classic ESP32 that writes to 100 % and then fails to boot. |
| Vendored installer runtime | `docs/vendor/`, `scripts/verify-vendored-esptool-js.sh` | Executing mutable CDN JavaScript inside the USB firmware trust boundary. |
| Browser installer port release | `docs/index.html`, `serial-port-release.mjs`, `test/serial_port_release.test.mjs`, `scripts/build-pages.sh` | A previously granted serial port staying "paired" with no way to revoke it, or a release interrupting an active flash. |
| Trusted cleanup and runner-context bootstrap | `.github/workflows/pr-preview-cleanup.yml`, `bench-acceptance.yml`, `scripts/check-workflow-policy.py` | PR-authored code gaining the `gh-pages` write token, a new default-branch validator missing for the PR that introduces it, or a runner-only context resolved before a runner exists. |
| Release-bound signed Pages channel | `.github/workflows/build.yml`, `signed-pr-preview.yml`, `pr-preview-cleanup.yml`; [build and review](DEVELOPMENT.md) | Stale, partial or mixed publication being mistaken for the exact signed channel. |
| Warning contract on `main/` | `main/CMakeLists.txt`, `test/test_runtime_boundary_contract.py` | Own code depending on IDF defaults. |
| Presenter parity checks | `scripts/check-display-sim-parity.sh`, `check-ble-row-parity.sh` | The Python display sim and browser JS drifting from the C++ presenters they mirror. |
| Reproducible toolchain and complete effective-build graph | `esp-idf-toolchain.txt`, `main/idf_component.yml`, `dependencies.lock.*`; [build and review](DEVELOPMENT.md) | Unpinned, hidden or substituted compiler/generated/link inputs escaping the reviewed source graph. |
| Artifact, partition and memory-budget identity | `scripts/check-build-artifact-inventory.py`, `check-build-gate-contract.py`, `check-partition-contract.py`; [build and review](DEVELOPMENT.md) | Corrupt or substituted artifacts, layout drift and unreviewed physical memory or stack growth. |
| Reviewed memory-budget ratchets | `scripts/report-firmware-size.py`, `check-firmware-size.sh`; [size rule](../.agents/rules/firmware-size-budget.md) | Size reductions becoming unreviewed future headroom, or changing capacities while ratcheting maxima. |
| Incremental all-target pre-push baseline check | `scripts/precheck-firmware.sh`, `.githooks/pre-push`, `scripts/test-build-contracts.sh` | Late CI failures from size or stack growth on a target omitted by local checks. |
| Parallel reproducibility leg | `.github/workflows/build.yml` (`build-target` `leg` matrix, `build`), `scripts/check-reproducible-build.sh --emit`, `scripts/check-reproducible-artifacts.py`, `scripts/ci-build-verify.sh --skip-repro` | Reproducibility costing ~100 s of serial rebuild inside every PR build job. |
| Four targets, one tree | `sdkconfig.defaults.*`, `scripts/ci-build-all.sh`, `.github/workflows/build.yml`, `scripts/release-relevance.sh` | Per-chip divergence. |
| Vehicle-free bench report ingest | `.github/workflows/bench-acceptance.yml`, `scripts/check-bench-acceptance.py`, `check-workflow-policy.py` | Treating compilation as hardware acceptance, or ingesting incomplete, private or mismatched physical reports. |

Native Codex registration is generated from canonical reviewers/hooks and checked for parity;
current hook trust and primary-checkout declarations remain separate runtime conditions. Both host
jobs are mandatory for the aggregate build, alongside concurrent producer/independent target legs.
The PSA harness verifies v6.1.1 peer admission, domain hash isolation, encrypted VCSEC denial
before telemetry and message refusal when the RNG fails.
[Runtime contract](ARCHITECTURE.md#pinned-tesla-ble-and-native-orchestration).
