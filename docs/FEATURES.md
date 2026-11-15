# Feature catalog

Implementation index. `logic/` paths are under `main/`. See the [ESP-IDF inventory](ESP_IDF_MATRIX.md) for SDK coverage.

## 1. Boot, crash forensics and recovery

[recovery](ARCHITECTURE.md#boot-loop-safe-mode).

| Feature | Where | Prevents |
|---|---|---|
| Shared OTA entry and confirmation recovery policy | `logic/ota_recovery_memory.hpp`, `main/ota_recovery_memory.hpp`, `main/ota_update.cpp`, `main/ota_transport.cpp`; [reference contract](ARCHITECTURE.md) | Divergent entry/confirmation decisions and unsafe heap estimates |
| ESP32-C3, ESP32-S3 and classic esp32 INTERNAL RAM budget for OTA | `sdkconfig.defaults.esp32c3` and `sdkconfig.defaults.esp32s3` (WiFi IRAM placement, unused BLE controller features, controller activities and NimBLE msys pool, static WiFi RX buffers); `sdkconfig.defaults.esp32` (one controller connection, NimBLE msys pool, static WiFi RX buffers, `/diag` ring via `CONFIG_TESLA_DIAG_LOG_BYTES` in `main/Kconfig.projbuild`); [architecture](ARCHITECTURE.md#ota-self-update) | OTA refusal or rollback from fragmented INTERNAL memory |
| ESP32-C6 flash budget for the OTA slot | `sdkconfig.defaults.esp32c6` (NimBLE direct test mode and error-name tables off); [size contract](DEVELOPMENT.md#build-and-evidence-contracts) | Signed images overflowing the OTA slot |
| Shared reset-reason capture | `main/main.cpp`, `main/diag_crash.cpp`, `logic/reset_reason.hpp` | Unattributed reboots or inconsistent reset labels |
| Core dump to flash | `sdkconfig.defaults` (`ESP_COREDUMP_ENABLE_TO_FLASH`), `partitions.csv` (`coredump`, 48 KB at `0x12000`), `main/diag_crash.cpp` | Loss of panic evidence after restart |
| Bounded crash identity and summary | `logic/coredump_image.hpp`, `logic/crashinfo.hpp`, `main/coredump_hash.hpp`, `main/diag_crash.cpp` | Unchecked dump offsets and foreign-build ABI parsing |
| Preserved foreign or unverified dumps | `main/diag_crash.cpp`, `main/http_status.cpp` | Erasing evidence or advertising unverified current-build dumps |
| Request-owned crash export/deletion | `main/diag_crash.cpp`, `main/http_status.cpp`, `logic/effect_reply.hpp` | Deletion racing export or reporting false success |
| Boot-loop safe mode | `main/safe_mode.cpp`, `logic/boot_guard.hpp`, `logic/heap_supervision.hpp`, `main/heap_supervisor.cpp`, `main/vehicle_ctrl.cpp` `init_safe_mode`, `main/www/app.js` | Repeated automatic resets without proven recovery |
| Independent heap recovery | `main/heap_supervisor.cpp`, `logic/heap_watchdog.hpp`, `logic/heap_supervision.hpp`, `reboot_reason.hpp` | Stalled Vehicle task blocking heap recovery |
| Task watchdog (TWDT) | `sdkconfig.defaults` (60 s, `PANIC=y`), `vehicle_loop` and `mqtt_pub`; `mqtt_ha.cpp` feeds after each completed publish | Blocked task activity escaping heap supervision |
| Stack-overflow watchpoint | `sdkconfig.defaults` (`FREERTOS_WATCHPOINT_END_OF_STACK`) | Stack overruns corrupting adjacent memory |
| Stack-headroom telemetry for task and callback contexts | `main/stack_watch.cpp`, `logic/stack_context.hpp`, `logic/status_model.hpp`; [context semantics](API.md) | Unobserved stack growth in tasks and callbacks |
| OTA rollback health gate | `logic/health_gate.hpp` ordered sampler, `main.cpp` `ota_health_gate_task`, `main/ota_update.cpp` (`ota_runtime_health`, `OtaIdentityMutationGuard`, `OtaHealthCommitGuard`, `OtaConfigRestartGuard`) | Confirming OTA without stable runtime health |
| Default-off configuration-only USB maintenance | `logic/config_repair.hpp`, `main/config_repair{,_console}.cpp`, `main/config_repair_store.hpp`, separate build/role/artifact and exchange controls; [recovery contract](CONFIG_RECOVERY.md) | Unqualified writes, identity loss and uncertain-write replay |
| Essential-startup failure | `main/boot_fatal.hpp` (defined in `main.cpp`) | Partially initialized runtime reported as ready |
| Atomic vehicle-task start and runtime admission | `logic/task_start_gate.hpp`, `logic/runtime_admission.hpp`, `main/runtime_admission.cpp`; [recovery](ARCHITECTURE.md#boot-loop-safe-mode) | Vehicle work before both tasks are admitted |

## 2. Configuration and storage

[storage](SECURITY.md#nvs-retention).

| Feature | Where | Prevents |
|---|---|---|
| Request-local mutation replies | `logic/effect_reply.hpp`, `main/http_effect_reply.hpp`, HTTP/MCP handlers; [API](API.md#response-classes) | Lost effects, duplicate replies and lost restart continuation |
| Atomic CRC config blob and bounded save buffer | `logic/config_store.hpp`, `logic/service_target_limits.hpp`, `main/config_blob.cpp` | Torn settings, shared scratch corruption and truncated values |
| Legacy per-key fallback | `logic/nvs_string_load.hpp`, `main/nvs_storage.cpp`, `main/config_blob.cpp` `cfg_load()`, `cfg_load_for_update()` | Partial legacy data overwriting durable configuration |
| `[[nodiscard]]` NVS writes | `main/nvs_storage.hpp` + `-Werror=unused-result` | Silently ignoring persistent write errors |
| Exact fail-closed NVS registry | `logic/nvs_contract.hpp`, `main/nvs_storage.cpp`, `scripts/check-nvs-contract.py`, `test/test_nvs_storage.cpp` | Unknown keys, storage collisions and invalid persistence APIs |
| Request-scoped VCSEC replies | `main/logic/vcsec_response.hpp`, `main/vehicle_telemetry.cpp`, `main/logic/command_runner.hpp` | Late or passive status completing an unrelated request |
| Cached VCSEC session presence | `main/nvs_storage.{cpp,hpp}`, `main/vehicle_pairing.cpp` | Repeated flash-backed VCSEC presence queries |
| RAM session availability independent of persistence | `main/logic/session_availability.hpp`, `vehicle_telemetry.cpp`, `vehicle_pairing.cpp`, `logic/status_model.hpp`, `mqtt_payloads.hpp`, `mcp_json_payloads.hpp`; [runtime contract](ARCHITECTURE.md#pairing-lifecycle--invalidation) | Failed persistence disabling a usable live session |
| WiFi credential rollback | `logic/wifi_rollback.hpp`, `main.cpp`, `main/config_blob.cpp`, `POST /set_wifi` | Invalid credentials or Ethernet rescue losing the backup |
| Request-owned configuration restart and image result | `logic/config_commit.hpp`, `logic/effect_reply.hpp`, `main/ota_update.cpp`, HTTP config/common handlers, `main/provisioning.cpp` | Durable writes before admission or false OTA confirmation |
| Config survives OTA | `partitions.csv` (`nvs` at `0x9000`) | Loss of configuration and keys during OTA |
| Shared WiFi credential contract + safe host provisioning | `logic/wifi_credentials.hpp`, `main/provisioning.cpp`, `main/http_config.cpp`, `provision.py`; [storage](SECURITY.md#nvs-retention) | Invalid WiFi inputs and destructive partial NVS writes |
| MQTT broker test-before-persist + credential-free display | `logic/mqtt_uri.hpp`, `main/http_config.cpp` (`POST /set_mqtt`), `main/mqtt_ha.cpp` | Unusable saved broker or exposed credentials |
| Fail-closed HTTP body, JSON and config intake | `logic/http_body.hpp`, `json_syntax.hpp`, `config_request.hpp`, `main/json_input_validation.hpp`; [request contract](ARCHITECTURE.md#http-request-body-and-allocator-failure-contract) | Unbounded parsing and ambiguous input reaching effects |
| Ready-to-copy evcc vehicle configuration | `main/www/app.js`, `index.html`, `style.css`; `docs/examples/evcc-custom-vehicle.yaml`; [evcc setup](../README.md#3-connect-evcc) | Wrong host/VIN or stale evcc configuration |
| Cache-only vehicle_data/evcc read contract | `logic/vehicle_data.hpp`, `charge_control.hpp`, `vehicle_telemetry.cpp`, `http_api.cpp`; [API](API.md#vehicle-data) | Cache reads waking the car or asserting stale telemetry |
| MCP charge-cache availability | `vehicle_telemetry.cpp` `get_charge_state`, `main/mcp_server.cpp`, `mcp_json_payloads.hpp`; [MCP](MCP.md) | MCP bypassing shared charge-cache freshness rules |
| Single-source HA discovery registry | `logic/mqtt_discovery_registry.hpp`, `main/mqtt_ha.cpp`, `mqtt_payloads.hpp`, `test/test_mqtt_json_publish.cpp` | Discovery metadata and retained state diverging |
| Fail-closed retained MQTT JSON | `main/mqtt_ha.cpp`, `logic/mqtt_state_lifecycle.hpp`, `mqtt_json_publish.hpp`, `mqtt_payloads.hpp`; [storage](SECURITY.md#nvs-retention) | Partial retained JSON and lost stale-state clears |
| Vehicle-stable HA identity | `logic/ha_identity.hpp`, `main/mqtt_ha.cpp` | Duplicate HA device identity after board replacement |

## 3. Security

[security](SECURITY.md).

| Feature | Where | Prevents |
|---|---|---|
| Signed OTA (Secure Boot v2 RSA-3072, no hardware Secure Boot) | `sdkconfig.defaults`, `scripts/ci-build-all.sh`, `ci-sign-artifacts.sh`; [security](SECURITY.md) | Unsigned firmware or substituted signing authority |
| Provenance-bound signed PR previews | `.github/workflows/signed-pr-preview.yml`, `pr-preview-cleanup.yml`, `scripts/check-pr-preview-served.py`, `.agents/skills/flash-esp32/SKILL.md`; [served acceptance](DEVELOPMENT.md#served-pr-preview-acceptance), [security](SECURITY.md) | Signing foreign or stale source and leaving stale previews |
| Transactional key rotation + automatic same-key enrollment | `main/vehicle_pairing.cpp`, `main/provisioning.cpp`, `logic/key_rotation.hpp`, `logic/auto_enrollment.hpp`, `logic/vin_transition.hpp`, `main/main.cpp`; [pairing](USER_GUIDE.md#pairing-and-identity) | Reusing old keys/sessions across an identity change |
| Tesla protocol and patch known-answer gate | `test/tesla_protocol_vectors.test.mjs`, `test/test_tesla_ble_harness.cpp`, `scripts/test-tesla-ble-harness.sh`; [security](SECURITY.md) | Wire, key and response-authentication regressions |
| Existing-peer SessionInfo continuity | `patches/tesla-ble/0008-bind-existing-session-info-key.patch`, `main/vehicle_telemetry.cpp`, `test/test_tesla_ble_harness.cpp`; [lifetime](adr/0005-tesla-ble-seam.md#established-session-continuity-2026-10-23) | Refresh replacing an established peer's trusted key |
| Exact patch source admission | `CMakeLists.txt`, `scripts/apply-tesla-ble-patches.py`, `scripts/tesla-ble-patch-state.json`, `scripts/check-dependency-contract.py` | Fuzzy patches and stale admission state |
| Exact dependency source admission | `cmake/admit-dependency-sources.cmake`, `scripts/dependency_source.py`, `scripts/nanopb-source.json`, `test/test_dependency_sources.py`, `test/test_dependency_cmake.py`, `test/test_dependency_process.py`; [source/cache contract](DEVELOPMENT.md#exact-dependency-source-admission) | Mutable dependencies and unowned Git/cache inputs |
| Bounded dependency crypto/logging failures | `patches/tesla-ble/0006-port-crypto-bindings-to-psa.patch`, `CMakeLists.txt`, `test/test_dependency_guards.py`; [protocol seam](adr/0005-tesla-ble-seam.md) | Leaking failed decrypt output or evaluating disabled logs |
| Generation-bound deferred BLE command readiness | `logic/ble_readiness.hpp`, `ble_deferred_event.hpp`, `main/ble_client.{cpp,hpp}`; [security](SECURITY.md) | Premature readiness and stale BLE handle events |
| Deterministic BLE RX/TX framing and bounded deferred storage | `logic/rx_framing.hpp`, `logic/ble_deferred_queue.hpp`, `main/vehicle_ctrl.{cpp,hpp}`, `vehicle_telemetry.cpp`, `test/test_tesla_ble_harness.cpp`, `test/ble_deferred_delivery_fixture.hpp` | Lost RX timing, invalid frames and duplicate TX prefix |
| Bounded BLE command runner and execution uncertainty | `logic/command_runner.hpp`, `logic/command_fault.hpp`, `logic/command_deadline.hpp`, `logic/ble_dispatcher.hpp`, `logic/session_state.hpp`, `vehicle_commands.cpp`, `vehicle_telemetry.cpp`; [protocol contract](adr/0005-tesla-ble-seam.md) | Deadline renewal, stranded slots and uncertain command replay |
| Contextual charging no-op outcomes | `logic/command_result.hpp`, `logic/command_runner.hpp`, `vehicle_commands.cpp`, REST/MCP and web UI; [API](API.md#commands) | Treating already-satisfied results as physical transitions |
| Acknowledged NimBLE host startup | `logic/nimble_start_gate.hpp`, `main/ble_client.{cpp,hpp}`, `main.cpp`, `ota_update.cpp` | OTA confirmation without acknowledged NimBLE startup |
| Channel-aware eligibility + manifest/image match | `main/ota_update.cpp`, `ota_writer.cpp` | Ineligible channel/version or mismatched manifest and image |
| Bounded exact OTA manifest intake | `logic/ota_contract.hpp`, `ota_changelog_range.hpp`, `main/ota_manifest.hpp`; [security](SECURITY.md) | Ambiguous metadata and unbounded memory use |
| OTA buffer lifetimes | `main/ota_update.cpp`, `main/http_ota.cpp`, `main/www/app.js`; [architecture](ARCHITECTURE.md#ota-self-update) | HTTP scratch, UI polls and OTA allocations overlapping |
| Confirmed OTA offer and exact image binding | `logic/ota_contract.hpp`, `main/ota_update.cpp`, `ota_manifest.hpp`, `ota_hash.hpp`, `ota_writer.cpp`, `http_ota.cpp`, `main/www/app.js`; [API](API.md), [architecture](ARCHITECTURE.md#ota-self-update) | Applying expired confirmation or a substituted image |
| INTERNAL largest-block TLS attempt admission | `logic/tls_memory.hpp`, `mqtt_uri.hpp`, `logic/ota_recovery_memory.hpp`, `main/ota_transport.cpp`; [architecture](ARCHITECTURE.md#ota-self-update) | Starting TLS without enough contiguous memory |
| Absolute OTA lifetime and quarantined transport | `logic/ota_lifetime.hpp`, `main/ota_update.cpp`, `ota_transport.{hpp,cpp}`, `ota_http.cpp`, `ota_coordinator.cpp`, `ota_writer.cpp`; [architecture](ARCHITECTURE.md#ota-self-update) | Trickled data renewing deadlines or stale writer access |
| Compile-time HTTPS OTA endpoints | `logic/ota_contract.hpp`, `main/ota_update.cpp` | Plaintext firmware or manifest fetch |
| Dev-only direct signed HTTP app upload, advertised by MCP | `logic/ota_contract.hpp`, `main/http_ota.cpp`, `ota_update.cpp`, `ota_coordinator.cpp`, `ota_writer.cpp`, `mcp_server.cpp`; [API](API.md#direct-signed-firmware-upload) | Release upload and bypassed signature/probation checks |
| Local OTA evidence and one-upload execution | `scripts/dev-ota-upload.py`, `test/test_dev_ota_upload.py`; global `$ota`, [local test workflow](DEVELOPMENT.md#local-ota-test-before-merge) | Unbound build/target or repeated uncertain writes |
| Coherent allocation-free OTA status lock | `main/ota_update.cpp`, `test/test_runtime_boundary_contract.py` | Mixed-generation status or allocation under its lock |
| Fresh OTA probation admission and cached verification diagnostics | `logic/health_gate.hpp`, `logic/ota_contract.hpp`, `main/ota_update.cpp`, `http_ota.cpp`, `http_status.cpp` | New installs before image verification |
| OTA update channel (Release vs Dev) | `logic/ota_contract.hpp`, `main/ota_update.cpp`, `http_ota.cpp`; [security](SECURITY.md) | Unintended Dev updates on production boards |
| Firmware row: channel modal + on-demand check | `main/www/app.js`, `index.html`, `style.css`, `test/web_ui_http.test.mjs`, `test/web_ui_browser_gate.py` | Hidden refusals or inaccessible OTA controls |
| Readable device settings and keyboard feedback | `main/www/index.html`, `main/www/app.js`, `main/www/style.css`, `test/web_ui_browser_gate.py` | Clipped warnings, lost focus or feedback below 4.5:1 contrast |
| Bug-report redaction | `logic/redact.hpp`, `diag_log.cpp`, `GET /status?redact=1`, `GET /diag?redact=1`; `test/test_diagnostic_export_guidance.py` | Exposing vehicle, network or credential data in reports |
| Device-bound HTTP origin/Host checks and UI frame denial | `logic/http_origin.hpp`, `http_route.hpp`, `device_hostname.hpp`, `main/http_server.cpp`, `http_status.cpp`; [HTTP exposure](SECURITY.md#http-api-exposure) | Foreign browser mutations and framed controls |
| Private wake capture | `scripts/capture_wake.py` | Publishing raw VIN and authenticated BLE frames |
| Restricted setup-AP surface | `main/provisioning.cpp` | Management routes exposed through the open AP |
| Charging-Manager-only key role | `vehicle_pairing.cpp` | Unnecessary vehicle body privileges |
| No BLE link-layer pairing or bonding | `sdkconfig.defaults` (`CONFIG_BT_NIMBLE_SECURITY_ENABLE=n`), `scripts/check-build-gate-contract.py` (also refuses a per-target override); [ATT/SMP departures](adr/0005-tesla-ble-seam.md#2-platform-forced-departures-and-reference-deviations) | Unauthenticated SMP and excess C6 code |

The device HTTP API has no authentication or TLS; use it only on a trusted LAN
([security model](SECURITY.md#http-api-exposure)).

## 4. Network

[network](ARCHITECTURE.md#wifi--lan-connectivity).

| Feature | Where | Prevents |
|---|---|---|
| Captive portal | `main/provisioning.cpp`, `logic/captive.hpp` | Captive detection and DNS answer errors |
| ESP32-C3 WiFi power ceiling from startup | `sdkconfig.defaults.esp32c3` (`ESP_PHY_MAX_WIFI_TX_POWER`); [hardware limits](USER_GUIDE.md#hardware) | High transmit power during C3 startup |
| Recoverable setup portal | `main/provisioning.cpp`, `logic/setup_portal.hpp`, `main/main.cpp`, `main/net.cpp` | Stuck setup or an indefinitely open recovery AP |
| SPI Ethernet (W5500) | `net.cpp` (`CONFIG_TESLA_ETH_ENABLED`), `logic/eth_board.hpp`, `main/Kconfig.projbuild`; [network](ARCHITECTURE.md#wifi--lan-connectivity) | WiFi/BLE contention and callbacks using freed resources |
| One image, multiple boards | `main/board.{cpp,hpp}`, `logic/eth_board.hpp` | W5500 probes driving the T-Dongle display pin |
| Transport seam | `main/net.hpp`, `net.cpp`, `logic/net_link.hpp` | Runtime paths assuming every lease is WiFi |
| Gateway ICMP watchdog | `net.cpp` `net_watchdog_task`, `logic/net_link.hpp` `watch_step()` | A broken LAN link with no disconnect event |
| Generation-owned ICMP probe | `logic/ping_probe.hpp`, `main/ping_probe.hpp`, `net.cpp`, `syslog.cpp` | Late callbacks touching freed or reused ping state |
| Endless runtime reconnect | `net.cpp` `wifi_event_handler` | Temporary outages forcing setup unnecessarily |
| mDNS + DHCP hostname, privacy-bounded TXT | `main.cpp` | Manual discovery or broadcasting the VIN |
| Firmware-started ATT MTU exchange, bounded writes | `logic/ble_chunk.hpp`, `main/ble_client.{cpp,hpp}`, `sdkconfig.defaults` (`BT_NIMBLE_ATT_PREFERRED_MTU`); [ADR-0005 §2](adr/0005-tesla-ble-seam.md#2-platform-forced-departures-and-reference-deviations) | Oversize ATT writes and truncated response fragments |
| Bounded climate refresh priority | `logic/telemetry_poll.hpp`, `main/vehicle_telemetry.cpp`; [scheduling](ARCHITECTURE.md#read-only-telemetry-and-command-correctness) | Missed climate refresh or starvation of other domains |
| NTP authority, bounded checkpoints and temporary browser-clock fallback | `main/main.cpp`, `logic/clock_policy.hpp`, `time_sync.hpp`, `POST /set_time`, `mqtt_ha.cpp` | Untrusted time becoming durable or breaking TLS |
| UDP Syslog (RFC 5424) | `main/syslog.cpp`, `logic/syslog_policy.hpp` | Loss of reboot diagnostics |
| Commit-gated Syslog queue lifetime | `logic/syslog_start_gate.hpp`, `main/syslog.cpp` | Consumers touching uncommitted or freed queue state |
| Bounded iteration-fault reporting | `logic/iteration_fault.hpp`, `main/runtime_fault_log.hpp`; vehicle task catches | Repeated fault reports flooding log sinks |
| Bounded boot crash records | `logic/bootlog.hpp`, `main/diag_crash.cpp`, `main/syslog.cpp` | Truncated crash identity and final frames |
| Syslog boot replay | `logic/bootlog.hpp`, `logic/syslog_policy.hpp`, `main/syslog.cpp` | Lost boot evidence or replay of dismissed reports |

## 5. Diagnostics and observability

[diagnostics](API.md#management).

| Feature | Where | Prevents |
|---|---|---|
| Cached recovery visibility and owned automatic commit reload | `logic/runtime_admission.hpp`, `logic/identity_recovery.hpp`, `logic/key_rotation.hpp`, `logic/status_model.hpp`, `main/runtime_admission.cpp`, `http_status.cpp`, `mqtt_payloads.hpp`, `mqtt_ha.cpp`, `vehicle_pairing.cpp`, `main/www/app.js`; `test/test_identity_recovery_contract.py` | Inconsistent recovery status or repeated automatic reloads |
| `/status.sys` block | `logic/status_model.hpp`, `main/http_status.cpp` | Missing board identity and numeric reset reason |
| `/status.last_crash` | `logic/status_model.hpp`, `main/diag_crash.cpp` | Crash diagnosis requiring serial access |
| 24-hour heap trend (`GET /heap`) | `logic/heap_history.hpp`, `logic/uptime.hpp`, `main/heap_trend.cpp`, `main/heap_supervisor.cpp` | Leaked or fragmented heap hidden by one sample |
| Trend survives restart | `main/heap_trend.cpp` (`__NOINIT_ATTR`), `logic/heap_history.hpp` `HeapPersist` | Restart erasing the memory trend explaining it |
| Observable best-effort log capture | `main/diag_log.cpp`, `syslog.cpp`, `logic/status_model.hpp`; `test/test_runtime_boundaries.cpp` | Blocking log producers and hiding dropped lines |
| In-RAM diag ring (`GET /diag`) | `main/diag_log.cpp` (size `CONFIG_TESLA_DIAG_LOG_BYTES`, `main/Kconfig.projbuild`), `main/http_status.cpp` | Network log reads holding the ring lock |
| MQTT device diagnostics | `main/mqtt_ha.cpp` | Missing reset, heap and stack fleet diagnostics |
| Explicit web-feed availability and gauge actions | `main/www/app.js`, `index.html`, `style.css`; `test/web_ui_http.test.mjs`, `web_ui_browser_gate.py` | Failed polling presenting stale data as live |
| Cache-aware vehicle status card and climate details | `main/www/app.js`, `index.html`, `style.css`; `test/web_ui_http.test.mjs`, `web_ui_browser_gate.py` | Presenting a retained climate setting as current activity or attributing charger power to climate |
| Golden-pinned `/status` contract | `logic/status_model.hpp`, `test/test_logic.cpp` | Silent status schema drift |
| Independent live ChargeState freshness | `logic/charge_control.hpp`, `logic/status_model.hpp`, `vehicle_ctrl.cpp`, `http_status.cpp`, `main/www/app.js`; [status API](API.md#status-fields) | Unrelated telemetry refreshing a stale charge sample |
| evcc numeric field availability | `logic/vehicle_data.hpp`, `test/test_cjson_oom.cpp`; [vehicle data](API.md#vehicle-data) | Absent numbers reported as zero or invalid odometer success |

## 6. Build, test and CI

[build and review](DEVELOPMENT.md).

| Feature | Where | Prevents |
|---|---|---|
| Runner-neutral agent policy and SHA-bound PR records | `AGENTS.md`, `.agents/`, `.codex/`, `tools/agent-hooks/`, `tools/agent-config/`, `scripts/with-host-python.sh`; [build and review](DEVELOPMENT.md) and [bounded read forms](DEVELOPMENT.md#standalone-read-checks) | Stale approval and publishing personal/private identifiers or non-English PR/docs content |
| `$deploy` carries user authorization through publication and acceptance on the intended USB bench | `AGENTS.md`, `.agents/skills/deploy/SKILL.md`, `.agents/skills/deploy-bench/SKILL.md`, `test/test_deploy_target.py`; [delivery](DEVELOPMENT.md#delivery) | Bench actions on an unidentified board or mismatched artifact |
| Shared app-only USB safety preflight | [Installed-layout proof](DEVELOPMENT.md#installed-layout-proof), `scripts/check-bootloader-compat.py`, `scripts/bootloader-references.json`, USB paths in `flash-esp32`, `ship` and `usb-recovery`; [retained bootloader](../.agents/skills/usb-recovery/SKILL.md#retained-bootloader-compatibility) | Incompatible bootloader, partition or slot writes |
| OTA acceptance tied to one boot | [Ship verification](../.agents/skills/ship/SKILL.md#5-verify--the-loop-is-closed-only-when-the-device-says-so), `GET /ota/status` | Combining health observations across boots |
| Resource-bounded, init-reaped build container with exact mounts | `scripts/idf-docker.sh`, `scripts/check-idf-container-mounts.py`, `scripts/check-build-gate-contract.py` | Unbounded build resources and writable external Git state |
| Explicit task retirement and fault containment | `main/task_watchdog_subscription.hpp`, `main/mqtt_ha.cpp` (current-task TWDT release), `main/net.cpp` (iteration proof reset with retained asynchronous ping ownership), `main/ble_client.{cpp,hpp}` (bounded callback faults); [host limits](../test/README.md#actual-task-and-runtime-layers) | Self-deleting a task before confirmed cleanup |
| Fail-closed host verification | `scripts/run-mock-tests.sh --require-all`, `run-sanitizer-tests.sh`, `run-fuzz-smoke.sh`; [test scope](../test/README.md) | Skipped required host/runtime/browser checks |
| Efficient complete host contracts | `test/test_runtime_boundary_contract.py`, `test/test_deploy_candidate.py`, `scripts/check-host-gate-contract.py`, `scripts/check-build-gate-contract.py`, `scripts/check-dependency-contract.py`, `scripts/test-build-contracts.sh`; [verification](DEVELOPMENT.md#validate-a-change) | Repeated fixture setup obscuring gate cost |
| Reuse of identical local host results | `scripts/run-local-checks.py`, `test/test_local_checks.py`, Stop/pre-commit hooks, `tools/agent-hooks/run_with_timeout.py`; [local checks](DEVELOPMENT.md#validate-a-change) | Stale results and duplicate worktree checks |
| One cumulative CI scheduling decision | `.github/workflows/build.yml` (`prepare`), `scripts/check-workflow-policy.py` | Repeated upstream baseline reads per build target |
| Pre-push local verification and PR records | `scripts/check-local.sh`, `.github/PULL_REQUEST_TEMPLATE.md`, `.github/workflows/pr-policy.yml`; [verification](DEVELOPMENT.md#validate-a-change) | Merging with stale or missing local records |
| Parallel trusted preview rebuild | `.github/workflows/signed-pr-preview.yml` (`preview-version`, `trusted-rebuild-target`, `trusted-rebuild`); [preview acceptance](DEVELOPMENT.md#served-pr-preview-acceptance) | Publication before all independent targets complete |
| Key-free immutable Release site recovery | `scripts/recover-release-pages.py`, `publish-pages-branch.sh` (`root-recovery`), `test/test_release_pages_recovery.py`; [recovery](DEVELOPMENT.md#recover-a-published-stable-site) | Re-signing immutable bytes or recovering a changed site |
| Generation-owned BLE lifecycle retries | `main/ble_client.cpp`, `vehicle_telemetry.cpp`, `test/test_ble_lifecycle.py` | Retired links or scan intents corrupting newer work |
| Split publication host gates and concurrent artifact assembly | `.github/workflows/build.yml` (`logic-harness`, `logic-test`, `build`, `independent-rebuild`, `publish`), `scripts/check-workflow-policy.py`; [publication](SECURITY.md#release-pipeline-and-trust-boundaries) | Signing before both host jobs and byte comparison pass |
| Four-target signed candidate admission before Pages writes | `.github/workflows/build.yml`, `scripts/check-workflow-policy.py`, `test/test_deploy_candidate.py` | Pages writes with missing or invalid production signatures |
| Linux-only browser execution, trusted keyboard and bounded startup | `test/web_ui_browser_gate.py`, `test/browser_cdp.py`, their policy/transport tests, `scripts/run-browser-tests-linux.sh`, `tools/browser/Dockerfile`, `scripts/run-mock-tests.sh` | Skipped browser coverage or broken keyboard activation |
| Repository/workflow lint with mutation canaries | `scripts/repo-lint.sh`, `check-workflow-policy.py`, `check-host-gate-contract.py`, `check-markdown-links.py` | Broken links, malformed workflows and fail-open gates |
| ESP-IDF matrix audit | `scripts/check-esp-idf-matrix.py` (via `repo-lint.sh`); [ESP-IDF matrix](ESP_IDF_MATRIX.md#what-the-gate-proves) | Inventory drift from IDF source and defaults |
| Fail-closed Web Serial installer with installed-firmware check | `docs/index.html`, `installer-bootstrap.mjs`, `web-installer.mjs`, `test/web_installer.test.mjs`; [NVS retention](SECURITY.md#nvs-retention) | Accidental NVS erase and incompatible or corrupt installs |
| Vendored installer runtime and source verification | `docs/vendor/`, `scripts/verify-vendored-esptool-js.sh`, `build.yml` host gate; [third-party notices](THIRD_PARTY.md) | Mutable third-party runtime and missing license notices |
| Single-open serial-port ownership | `docs/index.html`, `serial-port-release.mjs`, `web-installer.mjs`, `test/web_installer.test.mjs`, `test/serial_port_release.test.mjs`, `scripts/build-pages.sh` | Port lockup and overlapping serial operations |
| Trusted cleanup and runner-context bootstrap | `.github/workflows/pr-preview-cleanup.yml`, `bench-acceptance.yml`, `scripts/check-workflow-policy.py` | PR code gaining Pages write authority |
| Release-bound signed Pages channel | `.github/workflows/build.yml`, `signed-pr-preview.yml`, `pr-preview-cleanup.yml`, `scripts/prefetch-idf-image.sh`, `pages_publish_control.py`, `wait-pages-build.py`, `check-dev-pages.py`, `check-published-release.py`, `release-relevance.sh`; [delivery verification](../.agents/skills/ship/SKILL.md#4-select-target-then-usb-flash-or-ota); [build and review](DEVELOPMENT.md) | Mixed or stale served bytes and unverified deployment |
| Warning contract on `main/` | `main/CMakeLists.txt`, `test/test_runtime_boundary_contract.py` | Build warnings depending on SDK defaults |
| Browser presenter parity | `scripts/check-ble-row-parity.sh`, `logic/ble_row.hpp`, `test/web_presentation_reference.hpp` | JS and C++ presentation decisions diverging |
| Reproducible toolchain and complete effective-build graph | `esp-idf-toolchain.txt`, `main/idf_component.yml`, `dependencies.lock.*`, `scripts/check-build-semantics.py`; [build and review](DEVELOPMENT.md) | Unpinned compilers, hidden inputs and unexpected flags |
| Closed OTA size profile | `main/CMakeLists.txt`, `scripts/check-build-semantics.py`; [exact six-unit policy](DEVELOPMENT.md#build-and-evidence-contracts) | Signed C6 overflow or overly broad optimization |
| Web asset pruning before gzip | `main/www/prune_css_comments.py`, `inline_assets.cmake`, `main/CMakeLists.txt`; [web assets](DEVELOPMENT.md#embedded-web-assets) | Flash waste and changed embedded source semantics |
| Artifact, partition and memory-budget identity | `scripts/check-build-artifact-inventory.py`, `check-build-gate-contract.py`, `check-partition-contract.py`; [build and review](DEVELOPMENT.md) | Invalid artifacts and unreviewed budget/layout growth |
| Reviewed memory-budget ratchets | `scripts/report-firmware-size.py`, `check-firmware-size.sh`, `check-stack-usage.py`; [size rule](../.agents/rules/firmware-size-budget.md) | Budget drift and unreviewed 64 KiB size steps |
| Incremental all-target baselines and concurrent local checks | `scripts/precheck-firmware.sh`, `.githooks/pre-push`, `scripts/check-local.sh`, `tools/agent-hooks/parallel-checks.sh`; [verification](DEVELOPMENT.md#validate-a-change) | Missing target checks and hidden worker failure |
| Parallel reproducibility leg | `.github/workflows/build.yml` (`build-target` `leg` matrix, `build`), `scripts/check-reproducible-build.sh --emit`, `scripts/check-reproducible-artifacts.py`, `scripts/ci-build-verify.sh --skip-repro` | Unnecessary serial waits for independent rebuilds |
| Four targets, one tree | `sdkconfig.defaults.*`, `scripts/ci-build-all.sh`, `.github/workflows/build.yml`, `scripts/release-relevance.sh` | Per-target drift and silently missing firmware jobs |
| Vehicle-free bench report ingest | `.github/workflows/bench-acceptance.yml`, `scripts/check-bench-acceptance.py`, `check-workflow-policy.py` | Importing private or mismatched reports as bench evidence |
