# ADR-0005: Own the BLE orchestration layer modelled on teslamotors/vehicle-command

**Status:** accepted
**Date:** 2026-09-20
**Supersedes:** [`0003-reject-replayed-tesla-responses.md`](0003-reject-replayed-tesla-responses.md)
**Relates to:** [`0002-idf6-mbedtls4-crypto-seam.md`](0002-idf6-mbedtls4-crypto-seam.md), Issue [#306](https://github.com/0Bu/tesla-key-esp32/issues/306), Issue [#61](https://github.com/0Bu/tesla-key-esp32/issues/61), Issue [#65](https://github.com/0Bu/tesla-key-esp32/issues/65), [`../ARCHITECTURE.md`](../ARCHITECTURE.md) §"Pinned tesla-ble with an ordered build-time patch series"

## Context

The firmware integrates `yoziru/tesla-ble` v5.2.0 as an ESP-IDF component. Upstream `tesla-ble` divides into two distinct layers:
1. **The protocol crypto core** (`client.h`, `peer.h`, `crypto_context.h`, generated Nanopb protobufs): handles P-256 ECDH, HMAC key derivation, AES-GCM encryption/decryption, and protobuf serialization.
2. **The vehicle orchestration layer** (`vehicle.cpp`, `tb_logging.cpp`): manages BLE stream reassembly, request-response dispatch, command queuing, authentication flows, and retry/timeout heuristics.

While the protocol crypto core is sound, the upstream orchestration layer has proven fragile across repeated production incidents:
- **RX framing failure modes**: The upstream framing code in `vehicle.cpp` attempts heuristic recovery loops when bytes are corrupted or split unexpectedly, causing log floods (which required patch 0003 to rate-limit). Upstream's v5.1.4 response adds complex speculative scanning rather than deterministic framing.
- **Incomplete anti-replay protection**: Patch 0001 (and ADR-0003) were introduced to halt execution in `handle_carserver_message_` when `validate_response_counter()` detects a duplicate counter. However, ADR-0003's rationale was incomplete: unauthenticated CarServer responses carry `response_counter == 0`, bypassing the replay check entirely in `vehicle.cpp`.
- **Non-transactional key regeneration**: Upstream's `Vehicle::regenerate_key()` replaces the in-memory key before persistence without roll-back safety on storage error (requiring patch 0002).
- **Session counter monotonicity inversion**: Upstream forces lower reported counters rather than maintaining `max(local, reported)` as the reference requires (requiring patch 0005).
- **Dead code bloat**: Unused Parental Controls actions in the protobufs waste flash on size-constrained targets like `esp32c6` (requiring patch 0004).

The repository previously maintained four local patches against v5.2.0 (0002–0005; patch 0001 was retired earlier in favor of native dispatch). Maintaining source patches against an upstream orchestration layer is unsustainable. Furthermore, it tightly couples transport quirks to protocol decoding.

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
- **Sleep state evaluation vs wake advancement (R1)**: The raw `vehicleSleepStatus` from VCSEC determines the reported sleep state (`vcsec_sleep_state_`). A VCSEC `AWAKE`, or a `VehicleStatus` carrying `closureStatuses`, that arrives while a command waits for its wake confirms that wake for the command (`CommandRequest::wake_confirmed`). Unlike PR #322 where `wake_confirmed` was unconditionally sticky across all retries, on step timeout, link loss, or auth failure `wake_confirmed` is reset to false to re-evaluate sleep; however, on command payload response timeout, `wake_confirmed` is preserved so payload retries use exponential backoff rather than regressing into `SendWake`.
- **VCSEC operation status handling (F3)**: The reference (`vehicle-command`) treats VCSEC command status with `operationStatus` `OPERATIONSTATUS_WAIT` as an in-progress indicator, `OPERATIONSTATUS_OK` as success, and others as errors (`pkg/vehicle/vcsec.go`). The firmware evaluates `operationStatus`: `OPERATIONSTATUS_WAIT` keeps waiting for completion, `OPERATIONSTATUS_OK` completes the command successfully, and error statuses abort with an error.
- **Dispatcher-bypass protection and telemetry handling on unauthenticated frames (F2)**: In reference `vehicle-command` (`dispatcher.go:287-292`), unmatched responses are dropped before callbacks are invoked. In this firmware: (1) in `main/vehicle_telemetry.cpp`, incoming `SessionInfo` frames are validated against the outstanding `request_uuid` before HMAC or tag checks, dropping mismatched or orphan packets fail-closed with `ESP_LOGW` without aborting active authentication requests; SessionInfo HMAC or verification errors only fail an in-flight command if the command is actively awaiting authentication (`WaitingVcsecAuth` or `WaitingInfoAuth`) for that specific domain, dropping orphan or unrequested SessionInfo frames fail-closed without aborting unrelated commands; signed message faults do not carry request UUIDs, so they are filtered by domain and only notify an in-flight command if an active command in that domain exists (or a VCSEC fault during VCSEC auth; Infotainment faults are ignored while awaiting VCSEC auth or wake); (2) VCSEC `VehicleStatus` frames update `vcsec_sleep_state_`, the ASLEEP debounce clock, and invoke `vehicle_status_callback_` (satisfying `get_vehicle_status()` generation polling) before `handle_response()` routes the frame against the command queue, preserving passive sleep tracking and status telemetry even when `VehicleStatus` arrives unrequested or as a broadcast.
- **Proactive session info on fault & universal revocation check (R3)**: When a signed message fault carries a proactive `session_info` payload (`dispatcher.go:295-299`), it is applied inline; key revocation detection (`on_vehicle_message_`) runs for every fault message regardless of command in-flight state.
- **Key fingerprint (`key_fingerprint()`)**: Retained natively in `main/vehicle_pairing.cpp` using mbedtls SHA-1 on the exported public key (`SHA1(pubkey_bytes)[:4]`), keeping it decoupled from `tesla-ble` internals.
- **Serialized command execution**: Protected by `command_mutex_` to prevent BLE radio contention and preserve FreeRTOS task high-water mark.

### 2.1 Framing Edge Cases and Protocol Deviations (F8)

The native `tk::RxFramer` reassembly implementation covers the critical framing boundary conditions identified during v5.1.4 analysis and verified in `test/test_logic.cpp`:
- **Case 2 (Split frame across multiple notifications)**: Frames fragmented across several BLE notifications are reassembled cleanly without premature completion or state corruption.
- **Case 3 (Several frames in one single notification)**: Multiple concatenated frames arriving back-to-back in a single notification are sliced and delivered sequentially.
- **Case 4 (Corrupt length before split frame)**: A chunk declaring an invalid length (> 2048 B) is rejected immediately as `CorruptLength`, discarding corrupt bytes while allowing subsequent valid split frames to reassemble normally.
- **Case 5 (Zero-length treated as corrupt)**: Frames with decoded length 0 are rejected immediately as `CorruptLength`, preventing infinite or zero-byte parsing loops.
- **Case 13 (Malformed payload inside frame)**: A frame with a malformed payload is delivered deterministically to the upper protocol layer, which handles decoding errors while the framer continues processing subsequent frames.
- **Case 14 / 14a (Duplicate fragment and duplicate frame arrival)**: Duplicate notifications or fragments during a split frame deliver a frame containing the duplicate bytes (which upper-layer decoding rejects fail-closed); extra duplicate residual bytes trigger `CorruptLength` or `BufferOverflow` on subsequent frames or are discarded cleanly on the 3 s inter-chunk timeout.

### 3. Superseding of ADR-0003

ADR-0003 introduced patch 0001 to drop replayed CarServer responses. However, as noted in Issue #306, ADR-0003 only blocked replayed messages where `response_counter > 0`. Plaintext or unauthenticated CarServer responses (`response_counter == 0`) remained open to attribution confusion in `vehicle.cpp`. Under the new dispatcher model, every response must match an active request by UUID (or be an allowed empty-UUID VCSEC broadcast). Thus, ADR-0003 is formally **superseded** by this ADR.

### 4. Relationship to ADR-0002 and Issues #61 / #65

This architecture leaves `crypto_context.cpp` and `peer.cpp` upstream in `yoziru/tesla-ble`. It does not move cryptographic operations into the firmware. Consequently:
- It is fully complementary to ADR-0002 and Issues #61/#65 (the future ESP-IDF 6.x / Mbed TLS 4 / PSA crypto migration).
- Dropping `vehicle.cpp` simplifies any future PSA bridge fork by reducing the patch footprint.
- Issues #61 and #65 remain **OPEN** and tracking the upstream crypto seam.

### 5. Delivered Architecture and Patch Retirement

The native BLE orchestration layer is implemented across:
- `main/logic/rx_framing.hpp`: Deterministic 2-byte BE length-prefix framing (`tk::RxFramer`) and the TX framing helpers `drive_command_runner_()` uses (`tk::build_ble_tx_frame`, `tk::is_well_formed_ble_frame`).
- `main/logic/ble_dispatcher.hpp`: Multi-slot UUID and domain dispatcher (`tk::BleDispatcher`).
- `main/logic/session_state.hpp`: Thin `tk::SessionTracker` mirror with monotonic counter progression (`max(local, reported)`), zero in-house crypto.
- `main/logic/command_runner.hpp`: Command state machine arbitrating VCSEC auth, Wake, infotainment auth, and payload dispatch.
- `main/logic/command_result.hpp`: Normative `is_nominal_already_set()` outcome evaluation.
- `main/vehicle_ctrl.{hpp,cpp}`, `main/vehicle_commands.cpp`, `main/vehicle_telemetry.cpp`, `main/vehicle_pairing.cpp`: Direct integration with `TeslaBLE::Client`.
- Native transactional key generation: `tk::regenerate_private_key()` in `main/logic/key_rotation.hpp` (2048 B PEM export, fail-closed rollback), called by `regenerate_key_native_()` in `main/vehicle_pairing.cpp`.
- `test/test_tesla_ble_harness.cpp` (`scripts/test-tesla-ble-harness.sh`, run in the CI `logic-test` job) exercises the TX framing helpers and `tk::regenerate_private_key()` against the real tesla-ble v5.2.0, Nanopb and Mbed TLS.

With this architecture in place, patches 0001, 0002, 0003, and the `vehicle.cpp` portion of 0005 were retired. The active patch series in `patches/tesla-ble/` consists strictly of:
1. `0004-drop-unused-parental-controls-actions.patch` (trim unused Nanopb message descriptors for target size budget)
2. `0005-align-session-counter-replay-with-signer-go.patch` (`peer.cpp` session counter monotonic progression alignment per `signer.go`)

### 6. Compilation of Dead Translation Units (TESLABLE_SRCS)

Upstream `CMakeLists.txt` lists `vehicle.cpp` and `tb_logging.cpp` in `TESLABLE_SRCS`. These translation units are compiled during build, but their unused symbols are dropped at link time by the linker's `--gc-sections` flag. This compile-but-drop trade-off is accepted over maintaining an additional source-removal patch, keeping the local patch series strictly minimized to two patches (0004 and 0005).

## Consequences

- **Framing Robustness**: Eliminates buffer-recovery heuristics and log storms. BLE stream reassembly matches Tesla reference behavior.
- **Security & Anti-Replay**: Eliminates the plaintext CarServer replay window. Response attribution is strictly bound by request UUID.
- **NVS & Key Preservation**: Zero modification to durable NVS schemas, key formats, or session storage. Existing paired vehicles survive update without re-pairing.
- **Heap & Memory Budget**: Pure decision logic is isolated in `main/logic/` and host-tested. Allocations on the `vehicle_loop` stack remain strictly bounded.
- **Dependency Contract**: Upstream `yoziru/tesla-ble` dependency pin (v5.2.0) and `targets:` enforcement remain intact.
