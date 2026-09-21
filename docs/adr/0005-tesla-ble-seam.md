# ADR-0005: Own the BLE orchestration layer modelled on teslamotors/vehicle-command

**Status:** accepted
**Date:** 2026-09-20
**Supersedes:** [`0003-reject-replayed-tesla-responses.md`](0003-reject-replayed-tesla-responses.md)
**Relates to:** [`0002-idf6-mbedtls4-crypto-seam.md`](0002-idf6-mbedtls4-crypto-seam.md), Issue [#306](https://github.com/0Bu/tesla-key-esp32/issues/306), Issue [#61](https://github.com/0Bu/tesla-key-esp32/issues/61), Issue [#65](https://github.com/0Bu/tesla-key-esp32/issues/65), [`../ARCHITECTURE.md`](../ARCHITECTURE.md) §"Pinned tesla-ble with an ordered build-time patch series"

## Context

The firmware integrates `yoziru/tesla-ble` v5.1.3 as an ESP-IDF component. Upstream `tesla-ble` divides into two distinct layers:
1. **The protocol crypto core** (`client.h`, `peer.h`, `crypto_context.h`, generated Nanopb protobufs): handles P-256 ECDH, HMAC key derivation, AES-GCM encryption/decryption, and protobuf serialization.
2. **The vehicle orchestration layer** (`vehicle.cpp`, `tb_logging.cpp`): manages BLE stream reassembly, request-response dispatch, command queuing, authentication flows, and retry/timeout heuristics.

While the protocol crypto core is sound, the upstream orchestration layer has proven fragile across repeated production incidents:
- **RX framing failure modes**: The upstream framing code in `vehicle.cpp` attempts heuristic recovery loops when bytes are corrupted or split unexpectedly, causing log floods (which required patch 0003 to rate-limit). Upstream's v5.1.4 response adds complex speculative scanning rather than deterministic framing.
- **Incomplete anti-replay protection**: Patch 0001 (and ADR-0003) were introduced to halt execution in `handle_carserver_message_` when `validate_response_counter()` detects a duplicate counter. However, ADR-0003's rationale was incomplete: unauthenticated CarServer responses carry `response_counter == 0`, bypassing the replay check entirely in `vehicle.cpp`.
- **Non-transactional key regeneration**: Upstream's `Vehicle::regenerate_key()` replaces the in-memory key before persistence without roll-back safety on storage error (requiring patch 0002).
- **Session counter monotonicity inversion**: Upstream forces lower reported counters rather than maintaining `max(local, reported)` as the reference requires (requiring patch 0005).
- **Dead code bloat**: Unused Parental Controls actions in the protobufs waste flash on size-constrained targets like `esp32c6` (requiring patch 0004).

Maintaining five source patches against an upstream library that continues to add heuristic orchestration complexity is unsustainable. Furthermore, it tightly couples transport quirks to protocol decoding.

### Normative Reference: `teslamotors/vehicle-command`

Tesla's official [`teslamotors/vehicle-command`](https://github.com/teslamotors/vehicle-command) repository is the authoritative, normative specification for vehicle BLE protocol behavior. Where `tesla-ble` and `vehicle-command` diverge, `vehicle-command` defines correct behavior.

## Decision

**Own the BLE orchestration layer in this firmware, replacing `vehicle.cpp`, while retaining `yoziru/tesla-ble`'s protocol crypto core upstream.**

The architecture is implemented according to the reference specifications:

### 1. Normative Reference Mapping

| Component | Reference Implementation | Firmware Implementation | Key Characteristics |
|---|---|---|---|
| **RX Framing** | `pkg/connector/ble/ble.go:67-105` | `main/logic/rx_framing.hpp` (`tk::RxFramer`) | Deterministic 2-byte big-endian length prefix. Discards buffer on inter-chunk timeout (1000 ms) or invalid length (0 or > 2048). No heuristic scanning or speculative probe loops. |
| **Message Dispatch** | `internal/dispatcher/dispatcher.go` | `main/` dispatcher seam | Route response to matching outstanding request keyed by UUID and domain. VCSEC responses are exempt from UUID matching. Unmatched or orphan responses are dropped fail-closed. |
| **Anti-Replay Window** | `internal/dispatcher/dispatcher.go:245-315` | Per-request replay window | Monotonic counter tracking per outstanding request rather than a global shared slot (`last_request_hash_`). Replay protection applies uniformly to authenticated and plaintext responses. |
| **Session & Auth State** | `pkg/protocol/signer.go` | Session manager | Counter progression follows max(local, reported). Session info HMAC is verified against request UUID. Durable NVS format and keys are strictly preserved. |
| **Outcome Classification** | `pkg/protocol/protocol.md` | `main/logic/command_result.hpp` | `is_nominal_already_set()` treats nominal `already_set` vehicle responses as idempotent success at the application layer, separating transport ACK from semantic outcome. |

### 2. Platform-Forced Departures

The reference is written in Go, relying on goroutines, runtime-managed channels, garbage collection, and dynamic heap allocation. On ESP32 with FreeRTOS and tight memory constraints (e.g. 8192 B `vehicle_loop` stack), the following deliberate adaptations are made:
- **No goroutines / channels**: Replaced by deterministic FreeRTOS queues (`ble_event_queue_`) and the single `vehicle_loop` execution thread.
- **Bounded static buffers**: `tk::RxFramer` enforces `kMaxBufferSize = 2 + 2048 + 512` with explicit overflow checks, avoiding heap fragmentation and largest-contiguous-block depletion.
- **Serialized command execution**: Protected by `command_mutex_` to prevent BLE radio contention and preserve FreeRTOS task high-water mark.

### 3. Superseding of ADR-0003

ADR-0003 introduced patch 0001 to drop replayed CarServer responses. However, as noted in Issue #306, ADR-0003 only blocked replayed messages where `response_counter > 0`. Plaintext or unauthenticated CarServer responses (`response_counter == 0`) remained open to attribution confusion in `vehicle.cpp`. Under the new dispatcher model, every response must match an active request by UUID (or be an allowed VCSEC broadcast). Thus, ADR-0003 is formally **superseded** by this ADR.

### 4. Relationship to ADR-0002 and Issues #61 / #65

This architecture leaves `crypto_context.cpp` and `peer.cpp` upstream in `yoziru/tesla-ble`. It does not move cryptographic operations into the firmware. Consequently:
- It is fully complementary to ADR-0002 and Issues #61/#65 (the future ESP-IDF 6.x / Mbed TLS 4 / PSA crypto migration).
- Dropping `vehicle.cpp` simplifies any future PSA bridge fork by reducing the patch footprint.
- Issues #61 and #65 remain **OPEN** and tracking the upstream crypto seam.

### 5. Migration and Phasing Plan

To ensure continuous system stability and zero regression on hardware:
- **Phase 1 (Step 1)**: Deploy pure-logic, host-tested `tk::RxFramer` (`main/logic/rx_framing.hpp`) and wire it directly into the BLE event pipeline (`main/vehicle_telemetry.cpp`). `tk::RxFramer` acts as a fail-closed pre-filter ensuring only complete, verified frames reach `vehicle_`.
- **Phase 2 (Steps 2–4)**: Implement pure-logic message dispatch, session state machine, and command FIFO in `main/logic/` with full host unit test coverage in `test/test_logic.cpp`.
- **Phase 3 (Step 5 - Cutover)**: Cut over `main/vehicle_ctrl.cpp` to use the new native orchestration layer directly, bypassing `TeslaBLE::Vehicle`.
  - *Patch Retirement*: Only at Phase 3 cutover are patches 0001, 0002, 0003, and the `vehicle.cpp` half of 0005 removed from `patches/tesla-ble/` and `scripts/apply-tesla-ble-patches.sh`.
  - *Safety constraint*: Patches 0001 and 0002 must NOT be deleted prior to Phase 3 cutover, as `TeslaBLE::Vehicle` still requires them for transactional key generation (`has_private_key()`, `regenerate_key()`) and CarServer replay protection during the transition.

### 6. Compilation of Dead Translation Units (TESLABLE_SRCS)

Upstream `CMakeLists.txt` lists `vehicle.cpp` and `tb_logging.cpp` in `TESLABLE_SRCS`. These translation units are compiled during build, but their unused symbols are dropped at link time by the linker's `--gc-sections` flag. This compile-but-drop trade-off is accepted over maintaining an additional source-removal patch, keeping the local patch series strictly minimized to two patches (0004 and 0005).

## Consequences

- **Framing Robustness**: Eliminates buffer-recovery heuristics and log storms. BLE stream reassembly matches Tesla reference behavior.
- **Security & Anti-Replay**: Eliminates the plaintext CarServer replay window. Response attribution is strictly bound by request UUID.
- **NVS & Key Preservation**: Zero modification to durable NVS schemas, key formats, or session storage. Existing paired vehicles survive update without re-pairing.
- **Heap & Memory Budget**: Pure decision logic is isolated in `main/logic/` and host-tested. Allocations on the `vehicle_loop` stack remain strictly bounded.
- **Dependency Contract**: Upstream `yoziru/tesla-ble` dependency pin and `targets:` enforcement remain intact.
