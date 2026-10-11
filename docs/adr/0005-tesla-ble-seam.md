# ADR-0005: Own the BLE orchestration layer modelled on teslamotors/vehicle-command

**Status:** accepted
**Date:** 2026-09-20
**Supersedes:** [`0003-reject-replayed-tesla-responses.md`](0003-reject-replayed-tesla-responses.md)
**Relates to:** [`0002-idf6-mbedtls4-crypto-seam.md`](0002-idf6-mbedtls4-crypto-seam.md), Issue [#306](https://github.com/0Bu/tesla-key-esp32/issues/306), Issue [#61](https://github.com/0Bu/tesla-key-esp32/issues/61), Issue [#65](https://github.com/0Bu/tesla-key-esp32/issues/65), [`../ARCHITECTURE.md`](../ARCHITECTURE.md#pinned-tesla-ble-and-native-orchestration)

## Context

Upstream Vehicle orchestration combined heuristic framing, incomplete plaintext attribution,
non-transactional key replacement and session-counter divergence. Keep protocol crypto upstream,
but own bounded framing/dispatch/arbitration in the firmware. This replaces patches 0001–0003.

### Normative Reference: `teslamotors/vehicle-command`

Tesla's official [`teslamotors/vehicle-command`](https://github.com/teslamotors/vehicle-command) repository is the authoritative, normative specification for vehicle BLE protocol behavior. Where `tesla-ble` and `vehicle-command` diverge, `vehicle-command` defines correct behavior.

## Decision

**Own the BLE orchestration layer in this firmware, replacing `vehicle.cpp`, while retaining `yoziru/tesla-ble`'s protocol crypto core upstream.**

The architecture is implemented according to the reference specifications:

### 1. Normative Reference Mapping

| Component | Reference Implementation | Firmware Implementation | Key Characteristics |
|---|---|---|---|
| **RX/TX Framing** | `pkg/connector/ble/ble.go:67-127` | `main/logic/rx_framing.hpp` (`tk::RxFramer`, `tk::build_ble_tx_frame`, `tk::is_well_formed_ble_frame`) | Deterministic 2-byte big-endian length prefix. RX discards the buffer on inter-chunk timeout (3000 ms as configured by `tk::CommandRunner`; see departures) or invalid length (0 or > 2048). No heuristic scanning or speculative probe loops. TX writes the tesla-ble builder output unchanged and refuses a structurally malformed frame. |
| **Message Dispatch** | `internal/dispatcher/dispatcher.go` | `main/logic/ble_dispatcher.hpp` (`tk::BleDispatcher`), `main/vehicle_telemetry.cpp` | Route response to matching outstanding request keyed by UUID and domain. For command responses, unauthenticated/orphan responses are dropped fail-closed before telemetry callbacks. Passive telemetry frames (VCSEC `VehicleStatus`) update sleep and status caches before command routing. VCSEC responses are exempt from UUID matching only when request UUID is empty. |
| **Anti-Replay Window** | `internal/dispatcher/dispatcher.go:245-315` | `main/logic/ble_dispatcher.hpp`, `main/vehicle_telemetry.cpp` | Monotonic counter tracking per outstanding request and via upstream `Peer::validate_response_counter()`. Plaintext CarServer responses with foreign UUIDs are dropped before telemetry processing. |
| **Session & Auth State** | `pkg/protocol/signer.go` | `main/logic/session_state.hpp` (`tk::SessionTracker`), `main/vehicle_commands.cpp` | Counter progression follows `max(local, reported)`. Zero in-house crypto (P-256 ECDH, HMAC and AES-GCM remain upstream in `TeslaBLE::Client`/`Peer`). Durable NVS format and keys are strictly preserved. |
| **Outcome Classification** | `internal/dispatcher/dispatcher.go`, `cmd/tesla-control` | `main/logic/command_result.hpp` (`tk::is_nominal_already_set`) | Maps nominal vehicle outcomes (`already_set` when vehicle setpoint is already at requested value) as idempotent success at the application layer, separating transport ACK from semantic outcome. |

### 2. Platform-Forced Departures and Reference Deviations

The reference is written in Go, relying on goroutines, runtime-managed channels, garbage collection, and dynamic heap allocation. On ESP32 with FreeRTOS and tight memory constraints (8192 B `vehicle_loop` stack), the following deliberate adaptations and departures from `vehicle-command` are made:
- **No goroutines / channels**: Replaced by deterministic FreeRTOS queues (`ble_event_queue_`) and the single `vehicle_loop` execution thread.
- **Bounded vector buffers**: `tk::RxFramer` uses a `std::vector<uint8_t>` capped strictly to `kMaxFrameLength = 2048` (+2 length header bytes), avoiding unbounded heap allocation while preserving stack budget.
- **Max frame length (2048 vs 1024 B)**: `ble.go:21` defines `maxBLEMessageSize = 1024`. tesla-ble's own reassembly used `Vehicle::MAX_MESSAGE_SIZE = 2048` (in the `Vehicle` class this firmware no longer calls), while a `UniversalMessage_RoutableMessage` is at most 741 B. `kMaxFrameLength = 2048` keeps that historical tesla-ble limit.
- **Inter-chunk timeout (3000 ms vs 1 s)**: `ble.go` drops a partial frame when the next chunk arrives more than `rxTimeout` = 1 s later. `tk::CommandRunner` configures `tk::RxFramer` with `kDefaultRxInterChunkTimeoutMs = 3000` (#320); `tk::RxFramer` itself still defaults to 1000 ms. A partial frame therefore survives notification gaps of up to 3 s, at the cost of holding a torn frame for up to 3 s before the buffer is cleared.
- **Length 0 rejected**: `tk::RxFramer` rejects incoming frames with decoded length 0 fail-closed (`CorruptLength`), preventing zero-byte framing lockups.
- **No session-info latency expiry (`maxLatency`)**: `vehicle-command` evaluates session expiration against a real-time `maxLatency` window. The firmware's `load_nvs_sessions_()` (`main/vehicle_telemetry.cpp`) inherits upstream `vehicle.cpp`'s check comparing Unix time (`time(nullptr)`) against vehicle epoch seconds (`SessionInfo.clock_time`), which causes stored sessions to be rejected once the system clock is set rather than acting as a real-time 1-hour reuse window. Restoring the wall clock before `VehicleController::init()` ensures the clock is set, rejecting stale sessions fail-closed.
- **Routing-address matching**: `vehicle-command` generates ephemeral 16-byte routing addresses for VCSEC requests (`dispatcher.go:400-409`). The firmware uses the connection channel and `last_request_hash_` on the single client instance, bypassing routing address generation.
- **One-plaintext-response rule**: Unlike `vehicle-command` (which applies no replay check to plaintext, `dispatcher.go:301-308`), this firmware's `BleDispatcher` enforces a strict one-unauthenticated-response rule where foreign-UUID responses are dropped before reaching telemetry callbacks.
- **Tick-based backoff vs 1 s RetryInterval**: `vehicle-command` uses a fixed 1 s retry interval. The firmware's `CommandRunner` coordinates multi-phase step timeouts (5 s) and bounded retry budgets (3 retries). On retry, an exponential backoff delay (`next_retry_delay_ms`, starting at 500 ms and doubling up to 2000 ms) is enforced before `TxAction::SendCommandPayload` is emitted.
- **Wake completion on transmission (N1)**: The explicit `Wake` command (`wake_up()`) completes when its RKE frame is transmitted (`CommandRunner::notify_tx_complete` for the command named `Wake`), whereas `vehicle-command` waits for the vehicle's reply (`pkg/vehicle/vcsec.go:174-193`, `readUntil`). `wake_up()` confirms the wake separately through live telemetry. The runner's own wake prerequisite (`TxAction::SendWake`) does not complete on transmission; it waits for VCSEC to confirm the wake (see R1).
- **Sleep state evaluation vs wake advancement (R1)**: The raw `vehicleSleepStatus` from VCSEC determines the reported sleep state (`vcsec_sleep_state_`). A VCSEC `AWAKE`, or a `VehicleStatus` carrying `closureStatuses`, that arrives while a command waits for its wake confirms that wake for the command (`CommandRequest::wake_confirmed`). `VehicleSleepStatus_E` is three-valued (`UNKNOWN`, `AWAKE`, `ASLEEP`): a frame reporting `UNKNOWN` (without `closureStatuses`) stores `SleepState::Unknown`, leaves the ASLEEP debounce clock untouched and confirms no wake, whereas upstream v5.2.0 treats every value except `ASLEEP` as awake (`tk::sleep_state_from_vcsec`, `tk::vcsec_status_confirms_wake`). Unlike PR #322 where `wake_confirmed` was unconditionally sticky across all retries, on step timeout, link loss, or auth failure `wake_confirmed` is reset to false to re-evaluate sleep; however, on command payload response timeout, `wake_confirmed` is preserved so payload retries use exponential backoff rather than regressing into `SendWake`.
- **VCSEC operation status handling (F3)**: The reference (`vehicle-command`) treats VCSEC command status with `operationStatus` `OPERATIONSTATUS_WAIT` as an in-progress indicator, `OPERATIONSTATUS_OK` as success, and others as errors (`pkg/vehicle/vcsec.go`). The firmware evaluates `operationStatus`: `OPERATIONSTATUS_WAIT` keeps waiting for completion, `OPERATIONSTATUS_OK` completes the command successfully, and error statuses abort with an error.
- **Dispatcher-bypass protection and telemetry handling on unauthenticated frames (F2)**: In reference `vehicle-command` (`dispatcher.go:287-292`), unmatched responses are dropped before callbacks are invoked. In this firmware: (1) in `main/vehicle_telemetry.cpp`, incoming `SessionInfo` frames are validated against the outstanding `request_uuid` before HMAC or tag checks, dropping mismatched or orphan packets fail-closed with `ESP_LOGW` without aborting active authentication requests; SessionInfo HMAC or verification errors only fail an in-flight command if the command is actively awaiting authentication (`WaitingVcsecAuth` or `WaitingInfoAuth`) for that specific domain, dropping orphan or unrequested SessionInfo frames fail-closed without aborting unrelated commands; unlike reference `vehicle-command` (where signed message faults can carry a `request_uuid` and route through its dispatcher, `dispatcher_test.go`), this firmware filters signed message faults by domain and execution phase (VCSEC faults match VCSEC commands or commands in `WaitingVcsecAuth`; Infotainment faults only notify an in-flight Infotainment command during active Infotainment execution phases `WaitingInfoAuth`, `Ready`, `AwaitingResponse`, dropping stale faults during prerequisite VCSEC auth or wake fail-closed); (2) VCSEC `VehicleStatus` frames update `vcsec_sleep_state_`, the ASLEEP debounce clock, and invoke `vehicle_status_callback_` (satisfying `get_vehicle_status()` generation polling) before `handle_response()` routes the frame against the command queue, preserving passive sleep tracking and status telemetry even when `VehicleStatus` arrives unrequested or as a broadcast.
- **Proactive session info on fault & universal revocation check (R3)**: When a signed message fault carries a proactive `session_info` payload (`dispatcher.go:295-299`), it is applied inline; key revocation detection (`on_vehicle_message_`) runs for every fault message regardless of command in-flight state.
- **Key fingerprint (`key_fingerprint()`)**: Retained natively in `main/vehicle_pairing.cpp`: Mbed TLS PK parses the stored PEM, the key moves into a volatile PSA key, and PSA SHA-1 over the exported public point gives `SHA1(pubkey_bytes)[:4]`, keeping it decoupled from `tesla-ble` internals. The function itself is IDF code; the host harness checks a mirror of it against tesla-ble's key id.
- **Crypto bindings on PSA (patch 0006)**: Upstream tesla-ble v5.2.0 binds the legacy Mbed TLS 3 crypto API, which ESP-IDF 6's Mbed TLS 4 removed. `0006-port-crypto-bindings-to-psa.patch` re-binds the same operations to PSA Crypto (ADR-0002, "Execution"). ECDH X-coordinate byte order and `SHA1(X)[:16]`, the "session info" HMAC-SHA256, AES-128-GCM with a 12-byte nonce, AAD = `SHA256(metadata)` and a 16-byte tag, the VIN BLE name and the SEC1 PEM key format are unchanged, and the host harness's V1 part proves them against the protocol.md vectors. A failed RNG read for a nonce now fails `Peer::encrypt`; upstream only logged it and encrypted under an unfilled nonce.
- **Response authentication enforced, bound to the response counter (patch 0006)**: Upstream computes the AES-GCM tag of an encrypted response (`mbedtls_gcm_finish`) but never compares it, so any response tag was accepted. On PSA, `psa_aead_verify` compares it, and 0006 keeps that: a response that fails authentication is refused and its plaintext wiped, as `vehicle-command`'s `Signer.Decrypt` (`internal/authentication/signer.go`) does. The unchecked tag had hidden a second departure: tesla-ble put its own *request* counter into the response metadata, while protocol.md ("Response metadata") and `Signer.Decrypt` use the counter the response carries in `AES_GCM_Response_data.counter`. The vehicle counts responses per request (`Verifier.Encrypt`; VCSEC sends up to three), so the two need not match. 0006 follows the reference: `Peer::decrypt_response` takes the response's counter, `Peer::construct_response_ad_buffer` builds the response metadata, and `Peer::construct_ad_buffer` refuses the response type. All three call sites (tesla-ble `client.cpp` and `vehicle.cpp`, and `VehicleController::handle_vcsec_frame_()` in `main/vehicle_telemetry.cpp`) pass the response counter. V1 pins it with a response sealed under independently serialized protocol.md metadata whose counter differs from the request counter; an AAD built from the request counter is refused.
- **Serialized command execution**: Protected by `command_mutex_` to prevent BLE radio contention and preserve FreeRTOS task high-water mark.

### 2.1 Framing Edge Cases and Protocol Deviations (F8)

Host CHECKs cover split and concatenated frames, corrupt/zero lengths, malformed payloads and
repeated fragments. The framer delivers deterministic bytes; upper-layer authentication/decoding
rejects malformed or duplicated content. Residual corrupt bytes fail length/buffer bounds or expire
on the configured inter-chunk timeout. No heuristic recovery scans for plausible payloads.

### 3. Superseding of ADR-0003

[ADR-0003](0003-reject-replayed-tesla-responses.md) is superseded: command responses require an
active UUID/domain, with only the documented empty-UUID VCSEC broadcast exception.

### 4. Relationship to ADR-0002 and Issue #61

[ADR-0002](0002-idf6-mbedtls4-crypto-seam.md) owns PSA/key compatibility and evidence. Crypto stays
in upstream Client/Peer; firmware owns orchestration. The active patches are 0004 descriptor trim,
0005 counter alignment and 0006 PSA bindings/response authentication.

### 5. Delivered Architecture and Patch Retirement

`rx_framing`, `ble_dispatcher`, `session_state`, `command_runner`, `command_result` and `key_rotation`
under `main/logic/` delegate through vehicle controller/commands/telemetry/pairing shells.
Transactional key generation has a 2048 B PEM bound and durable failure rollback. The real patched
library harness exercises production helpers and V1 vectors against pinned Nanopb/Mbed TLS.
Patches 0001–0003 and the Vehicle portion of 0005 are retired.

### 6. Compilation of Dead Translation Units (TESLABLE_SRCS)

Upstream `CMakeLists.txt` lists `vehicle.cpp` and `tb_logging.cpp` in `TESLABLE_SRCS`. These translation units are compiled during build, but their unused symbols are dropped at link time by the linker's `--gc-sections` flag. This compile-but-drop trade-off is accepted over maintaining an additional source-removal patch, keeping the local patch series minimal (0004 and 0005, plus the ESP-IDF 6 crypto port 0006).

## Consequences

- **Framing Robustness**: Eliminates buffer-recovery heuristics and log storms. BLE stream reassembly matches Tesla reference behavior.
- **Security & Anti-Replay**: Eliminates the plaintext CarServer replay window. Response attribution is strictly bound by request UUID.
- **NVS & Key Preservation**: Zero modification to durable NVS schemas, key formats, or session storage. Existing paired vehicles survive update without re-pairing.
- **Heap & Memory Budget**: Pure decision logic is isolated in `main/logic/` and host-tested. Allocations on the `vehicle_loop` stack remain strictly bounded.
- **Dependency Contract**: Upstream `yoziru/tesla-ble` dependency pin (v5.2.0) and `targets:` enforcement remain intact.
