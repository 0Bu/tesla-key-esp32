# ADR-0005: Own the BLE orchestration layer modelled on teslamotors/vehicle-command

**Status:** accepted
**Date:** 2026-09-20
**Supersedes:** [`0003-reject-replayed-tesla-responses.md`](0003-reject-replayed-tesla-responses.md)
**Relates to:** [`0002-idf6-mbedtls4-crypto-seam.md`](0002-idf6-mbedtls4-crypto-seam.md), Issue [#306](https://github.com/0Bu/tesla-key-esp32/issues/306), Issue [#61](https://github.com/0Bu/tesla-key-esp32/issues/61), Issue [#65](https://github.com/0Bu/tesla-key-esp32/issues/65), [`../ARCHITECTURE.md`](../ARCHITECTURE.md#pinned-tesla-ble-and-native-orchestration)

## Context

The firmware integrates `yoziru/tesla-ble` v6.3.0 as an ESP-IDF component. Upstream `tesla-ble` divides into two distinct layers:
1. **The protocol crypto core** (`client.h`, `peer.h`, `crypto_context.h`, generated Nanopb protobufs): handles P-256 ECDH, HMAC key derivation, AES-GCM encryption/decryption, and protobuf serialization.
2. **The vehicle orchestration layer** (`vehicle.cpp`, `tb_logging.cpp`): manages BLE stream reassembly, request-response dispatch, command queuing, authentication flows, and retry/timeout heuristics.

The protocol primitives remain upstream with the reviewed binding patches below. The upstream orchestration layer has proven fragile across repeated production incidents:
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

### Dependency source provenance

Repository consumers admit the already selected nanopb commit before IDF component configuration
and before incremental component compilation. The native harness uses complete exact-commit
pristine caches and a separately verified Tesla patch work copy. The
[Development source/cache contract](../DEVELOPMENT.md#exact-dependency-source-admission) owns the
failure and offline rules. This is a build-provenance boundary: the Tesla pin, five patch bytes,
seven nanopb runtime digests, generated protocol bytes and wire/key/session behavior are unchanged.
It does not correct the upstream standalone tag-fetch path or establish hardware acceptance.

### Pairing recovery and enrollment (2026-10-20)

Remote VCSEC faults and command error text are advisory recovery signals, not authority to replace
the local key or erase persisted sessions. The configured supervisor probes status, then sends the
stored Charging Manager key automatically with bounded backoff if a session is verifiably absent
or an advisory recovery hint persists. There is no manual enrollment endpoint. The hint is not an
authenticated key-removal diagnosis: it can cause only another request for the same key and role,
with NFC-card and touchscreen approval still required. Local transmission completion is not car
approval. `paired` continues to mean that a VCSEC session is stored.
Verified SessionInfo establishes the RAM peer and runner before best-effort persistence. Save
failure leaves that RAM session usable and suppresses enrollment without a recovery hint. The
identity-bound cache exposes VCSEC `session_available` separately from durable `paired`, plus
per-domain runtime and current-save metadata; Infotainment alone never supplies global availability.
This changes local availability reporting and admission, without changing frames, crypto,
storage format or the existing sleep/wake and command arbitration rules.
Automatic key generation is limited to verified first-boot key absence; identity changes remain
explicit operations. This is a deliberate native orchestration policy and does not change Tesla BLE
wire encoding or the stored key/session formats.

The architecture is implemented according to the reference specifications:

### 1. Normative Reference Mapping

| Component | Reference Implementation | Firmware Implementation | Key Characteristics |
|---|---|---|---|
| **RX/TX Framing** | `pkg/connector/ble/ble.go:67-127` | `main/logic/rx_framing.hpp` (`tk::RxFramer`, `tk::build_ble_tx_frame`, `tk::is_well_formed_ble_frame`) | Deterministic 2-byte big-endian length prefix. RX discards the buffer on inter-chunk timeout (3000 ms as configured by `tk::CommandRunner`; see departures) or invalid length (0 or > 2048). No heuristic scanning or speculative probe loops. TX writes the tesla-ble builder output unchanged and refuses a structurally malformed frame. The ATT transport underneath departs from the reference: TX chunks are Write Commands, RX uses notifications, the ATT MTU stays at 23 because the firmware neither starts nor accepts an MTU exchange, and inbound ATT requests go unanswered (see §2). |
| **Message Dispatch** | `internal/dispatcher/dispatcher.go` | `main/logic/ble_dispatcher.hpp` (`tk::BleDispatcher`), `main/vehicle_telemetry.cpp` | Route responses by UUID and domain. Routable VCSEC attempts also require the exact request-scoped reply address and a live transmitted command phase; only that context permits an empty response UUID. Passive plaintext `VehicleStatus` updates sleep/wake state without satisfying a status poll or action. Auth, payload and the Infotainment wake prerequisite have distinct typed admission. |
| **Anti-Replay Window** | `internal/dispatcher/session.go:43-53` | `main/logic/ble_dispatcher.hpp`, `main/vehicle_telemetry.cpp` | Replay windows follow successful authentication, including counter zero. Plaintext retains its separate per-request rule. UUID routing precedes telemetry. |
| **Session & Auth State** | `internal/authentication/signer.go` | `main/logic/session_state.hpp` (`tk::SessionTracker`), `main/vehicle_commands.cpp` | Counter progression follows `max(local, reported)`. Zero in-house crypto (P-256 ECDH, HMAC and AES-GCM remain upstream in `TeslaBLE::Client`/`Peer`). Durable NVS format and keys are strictly preserved. |
| **Outcome Classification** | `pkg/vehicle/infotainment.go` (`getCarServerResponse`), `pkg/proxy/proxy.go` (`handleVehicleCommand`, `httpStatusCode`) | `main/logic/command_result.hpp`, `main/logic/command_runner.hpp` | Deliberate local compatibility policy: preserve exact `already_set`, and normalize only authenticated current charge-start/stop reason pairs. The reference reports nominal errors rather than these success exceptions; see the outcome-status departure in §2. |

### 2. Platform-Forced Departures and Reference Deviations

The reference is written in Go, relying on goroutines, runtime-managed channels, garbage collection, and dynamic heap allocation. On ESP32 with FreeRTOS and tight memory constraints (10240 B `vehicle_loop` stack), the following deliberate or inherited adaptations and departures from `vehicle-command` are made:
- **Response replay window (64 vs 32)**: The pinned `Peer`/`SlidingWindow` and native dispatcher retain 64 response counters, while the reference `internal/authentication/crypto.go` selects 32. Authentication still precedes either local replay admission; a wider retained window is not reference equivalence.
- **SessionInfo age and tag length**: The reference BLE connector allows 4 s handshake latency and the dispatcher rejects older responses. Native admission instead uses the transmitted request's phase/command deadlines, with 5 s step and 7 s response timeout constants; it does not implement a separate 4 s age check. Pinned `Client::verify_session_info_tag` also accepts a matching 16-byte HMAC prefix, in addition to the reference's full 32-byte tag. These inherited library/orchestration choices are retained here; no crypto or timeout change is implied.
- **Advertisement admission**: Named target adverts can start a connection even when their connectable flag is false. The reference rejects that flag as a connection-limit signal. Local failure classification uses the flag, but that diagnostic is not a scan-admission guard.
- **Request-counter rollover**: The reference signer refuses `0xffffffff` before incrementing. Pinned `Peer::increment_counter()` has no equivalent guard. Monotonic session updates do not establish rollover protection; changing the library behavior requires its own reviewed migration.
- **VCSEC UUID strictness**: The reference exempts VCSEC response UUIDs. This adapter permits an empty UUID only for the current request-scoped address/phase; a nonempty UUID must match the captured request. This is deliberately stricter attribution.
- **Implicit foreground wake**: `WakeIfNeeded` can insert a VCSEC wake before an Infotainment command. This local prerequisite policy is distinct from the reference's explicit wake call; background `NoWakeSkip`/`NoWakeFail` paths do not gain that permission.

The entries above were checked against [vehicle-command `83baa078`](https://github.com/teslamotors/vehicle-command/tree/83baa078e81d08982dd997b8c8e4644e33a046a8), pinned tesla-ble v6.3.0 and the native adapter. They document current departures, not hardware acceptance or fixes to them.

- **No goroutines / channels**: Replaced by a fixed byte-packing `logic/ble_deferred_queue.hpp` mailbox under its own short critical section and the single `vehicle_loop` execution thread. Adjacent RX bytes coalesce only within the same generation and a3s arrival window; first/last arrival times survive splitting and delayed consumption. The framer tests the preceding gap at first arrival and renews expiry at last arrival. Timestamp/enqueue and empty-queue/expiry-clock observations share the mailbox mux. Link records remain ordered barriers; pop slices stay at20 bytes. This changes local storage, not ATT framing, MTU, subscriptions, crypto, command authority or key/session format.
- **Bounded vector buffers**: `tk::RxFramer` limits frame payloads to 2048 bytes and buffered-data admission to 2562 bytes (payload + 2-byte header + 512-byte chunk headroom). This bounds buffered size, not the exact allocated capacity of its `std::vector<uint8_t>`.
- **Max frame length (2048 vs 1024 B)**: `ble.go:21` defines `maxBLEMessageSize = 1024`. tesla-ble's own reassembly used `Vehicle::MAX_MESSAGE_SIZE = 2048` (in the `Vehicle` class this firmware no longer calls), while a `UniversalMessage_RoutableMessage` is at most 741 B. `kMaxFrameLength = 2048` keeps that historical tesla-ble limit.
- **Inter-chunk timeout (3000 ms vs 1 s)**: `ble.go` drops a partial frame when the next chunk arrives more than `rxTimeout` = 1 s later. `tk::CommandRunner` configures `tk::RxFramer` with `kDefaultRxInterChunkTimeoutMs = 3000` (#320); `tk::RxFramer` itself still defaults to 1000 ms. A partial frame therefore survives notification gaps of up to 3 s, at the cost of holding a torn frame for up to 3 s before the buffer is cleared.
- **Length 0 rejected**: `tk::RxFramer` rejects incoming frames with decoded length 0 fail-closed (`CorruptLength`), preventing zero-byte framing lockups.
- **Session-info latency (`maxLatency`) and persisted age**: `vehicle-command` limits live SessionInfo response latency to 4 s on BLE; this is separate from persisted-session age. The firmware's `load_nvs_sessions_()` (`main/vehicle_telemetry.cpp`) inherits a comparison of Unix time against vehicle epoch seconds (`SessionInfo.clock_time`). A successfully restored Unix clock rejects these persisted sessions; it does not implement a real-time 1-hour reuse window. Restore runs before controller initialization, but absent/unreadable `last_time` or an unsuccessful clock set can leave the inherited negative-age acceptance path. Session expiry is therefore not guaranteed by clock restore alone.
- **Request-scoped VCSEC routing**: As in `vehicle-command` (`dispatcher.go:400-409`), each newly serialized routable VCSEC attempt gets a fresh nonzero 16-byte reply address. `logic/vcsec_response.hpp` captures its actual frame metadata without allocating another message buffer and binds generation, command, attempt, kind and deadlines. Only a successful BLE write activates the context. Timeouts, failed builds/writes, retries, flushes and link loss retire it. Auth and the Infotainment wake prerequisite admit their correlated proactive SessionInfo/fault replies without allowing them to acknowledge the payload. Infotainment retains a stable ordinary address, restored after every temporary VCSEC builder scope. Raw WhitelistAdd remains a non-routable transmit-only request. This routing match is attribution, not authentication of plaintext replies.
- **Charge-port command domain**: The reference uses Infotainment CarServer open/close actions; this firmware uses VCSEC `ClosureMoveRequest.chargePort`. Both builders exist at v6.3.0. This preserves the existing local policy; static conformance evidence does not establish role acceptance or hardware behavior.
- **One-plaintext-response rule**: Unlike `vehicle-command` (which applies no replay check to plaintext, `dispatcher.go:301-308`), this firmware's `BleDispatcher` enforces a strict one-unauthenticated-response rule where foreign-UUID responses are dropped before reaching telemetry callbacks.
- **Wake completion on transmission (N1)**: The runner's explicit `Wake` request completes when its RKE frame is transmitted, whereas `vehicle-command` waits for a reply (`pkg/vehicle/vcsec.go`, `readUntil`). Public `wake_up()` separately requires fresh Infotainment evidence. A definite send failure returns immediately; possible submission preserves its original uncertainty while waiting within the same deadline, without another wake. TX-only success never stamps vehicle reachability. The runner's own wake prerequisite (`TxAction::SendWake`) waits for VCSEC confirmation rather than completing on transmission (see R1).
- **Sleep state evaluation vs wake advancement (R1)**: The raw `vehicleSleepStatus` from VCSEC determines the reported sleep state (`vcsec_sleep_state_`). A VCSEC `AWAKE`, or a `VehicleStatus` carrying `closureStatuses`, that arrives while a command waits for its wake confirms that wake for the command (`CommandRequest::wake_confirmed`). `VehicleSleepStatus_E` is three-valued (`UNKNOWN`, `AWAKE`, `ASLEEP`): a frame reporting `UNKNOWN` (without `closureStatuses`) stores `SleepState::Unknown`, resets the ASLEEP debounce clock and confirms no wake, whereas upstream v5.2.0 treats every value except `ASLEEP` as awake (`tk::sleep_state_from_vcsec`, `tk::vcsec_status_confirms_wake`). Unlike PR #322 where `wake_confirmed` was unconditionally sticky across all retries, on step timeout, link loss, or auth failure `wake_confirmed` is reset to false to re-evaluate sleep; however, on a safe read-only payload response-timeout retry, `wake_confirmed` is preserved so the query uses backoff rather than regressing into `SendWake`. A possibly submitted mutation ends instead of retrying.
- **VCSEC operation status handling (F3)**: `protocol.md` advises discarding legacy `CommandStatus OPERATIONSTATUS_ERROR` while waiting for a specific error; the Go `pkg/vehicle/vcsec.go` parser also has terminal error cases. This adapter retains terminal handling for an attributed current-request ERROR. Only a known authenticated current-payload rejection makes failure definite; plaintext or unknown inner values retain execution uncertainty. WAIT remains in progress and OK may complete the matching request. This legacy-error departure is retained rather than presented as exact protocol equivalence.
- **Ordinary Infotainment reply address**: The [reference dispatcher](https://github.com/teslamotors/vehicle-command/blob/d6239bf377b8a5859e8f5c9d64409df2a057934a/internal/dispatcher/dispatcher.go#L262) requires a matching destination routing address for every reply. Ordinary CarServer replies locally use domain/request-UUID matching and, when encrypted, GCM/replay validation, but do not separately compare `to_destination.routing_address`. SessionInfo/fault handling has the explicit current-context address check described below. This existing attribution departure is not evidence of cross-request cryptographic replay or a cause of observed RX failures.
- **Dispatcher-bypass protection and passive telemetry (F2)**: SessionInfo and signed-fault
  handling first require the current transmitted request's routing address, phase and deadline.
  VCSEC also checks its captured generation and optional response UUID; Infotainment requires its
  current request UUID. Completion callbacks check generation before publishing. Foreign or
  retired replies are silent. SessionInfo still needs its separate HMAC; a bad HMAC fails only the matching active
  authentication phase. Fault classification, authenticated session repair and uncertainty follow
  the command-outcome contract below. Passive plaintext VCSEC VehicleStatus updates sleep/debounce
  and wake progress without completing a command or invoking the poll callback. Matched action
  replies accept valid non-error payloads, including empty replies; malformed protobuf, unrelated
  universal payloads, SessionInfo and signed errors cannot become empty success. Encrypted empty
  replies require tag verification and zero-inclusive replay checks.
- **Signed-fault recovery hint (R3)**: When a signed message fault carries a proactive `session_info` payload (`dispatcher.go:295-299`), it is applied inline. Selected universal-message key faults can set a volatile recovery hint in `on_vehicle_message_`; that advisory does not prove vehicle-side key deletion and cannot erase persisted sessions or replace the key. The automatic pairing policy may retry enrollment with that same key after status probes and bounded backoff. The periodic VCSEC `GET_STATUS` health request is a separate plaintext path: the pinned Client builder disables payload encryption, matching the reference `AuthMethodNone` request in `pkg/vehicle/vcsec.go` and `state.go`. Establishing a session first does not authenticate this request or its response; a successful status response can clear only the advisory hint.
- **Key fingerprint (`key_fingerprint()`)**: Retained natively in `main/vehicle_pairing.cpp`: Mbed TLS PK parses the stored PEM, the key moves into a volatile PSA key, and PSA SHA-1 over the exported public point gives `SHA1(pubkey_bytes)[:4]`, keeping it decoupled from `tesla-ble` internals. The function itself is IDF code; the host harness checks a mirror of it against tesla-ble's key id.
- **Crypto bindings on PSA (patch 0006)**: Upstream tesla-ble v6.3.0 still binds the legacy Mbed TLS 3 crypto API, which ESP-IDF 6's Mbed TLS 4 removed. `0006-port-crypto-bindings-to-psa.patch` re-binds the same operations to PSA Crypto (ADR-0002, "Execution"). ECDH X-coordinate byte order and `SHA1(X)[:16]`, the "session info" HMAC-SHA256, AES-128-GCM with a 12-byte nonce, AAD = `SHA256(metadata)` and a 16-byte tag, the VIN BLE name and the SEC1 PEM key format are unchanged, and the host harness's V1 part proves them against the protocol.md vectors. A failed RNG read for a nonce now fails `Peer::encrypt`; upstream only logged it and encrypted under an unfilled nonce.
- **Response authentication and explicit response AAD (patch 0006)**: v5.3.0 now verifies the AES-GCM response tag and uses the response-carried counter upstream, matching `vehicle-command`'s `Signer.Decrypt`. The PSA rebase preserves this behavior with `psa_aead_verify` and plaintext wiping on authentication failure. It retains the separate `construct_response_ad_buffer` helper and makes the request-only `construct_ad_buffer` reject response signatures; upstream instead overloads one metadata helper with an optional counter. All response callers pass `AES_GCM_Response_data.counter`. V1 proves a response whose counter differs from the request counter, rejects request-counter AAD and rejects tampered tags. The unverified-tag/request-counter defects described in the September execution of ADR-0002 were present in v5.2.0 and are fixed upstream in v5.3.0.
- **Failed-decrypt cleanup and library logging**: Patch 0006 wipes the full caller-provided output capacity after an AEAD-operation failure, including bytes a failing driver may write without reporting. Successful crypto and key formats are unchanged. The component and native harness compile at INFO (`TESLA_BLE_LOG_LEVEL=2`), excluding DEBUG/VERBOSE argument evaluation and formatting. Known public-key/packet log formats also have a redacted-export guard. `test/test_dependency_guards.py` compiles the actual patched method with synthetic write-before-error drivers and the upstream logging macros/formatter, including removed-fix controls; the separate V1 vectors own real crypto answers. These checks do not establish an ESP-driver leak, physical key reuse or hardware acceptance.
- **Serialized command execution**: Protected by `command_mutex_` to prevent BLE radio contention and preserve FreeRTOS task high-water mark.
- **Encrypted VCSEC admission**: An AES-GCM-declared response requires an initialized VCSEC peer and a non-empty request hash before decryption. Missing context, a failed tag check or a repeated response-carried counter (including zero) drops it before protobuf decoding, sleep updates or status callbacks; it cannot fall through as plaintext. `Peer::validate_response_counter()` supplies the per-request window reset by `Client` when signing a new request, following `vehicle-command`'s decryption-context and counter handling. Intentional passive plaintext `VehicleStatus` broadcasts remain accepted by the separate status path.
- **Contextual application no-ops (F033)**: upstream
  [`getCarServerResponse`](https://github.com/teslamotors/vehicle-command/blob/fc5a1910ad45c42921d541089e3e366d0b897f52/pkg/vehicle/infotainment.go)
  returns a NominalError for an application ERROR; that error is not universal success. This
  firmware deliberately accepts only the command/reason pairs owned by [API](../API.md#commands),
  informed by the official [Fleet reason catalog](https://developer.tesla.com/docs/fleet-api/endpoints/vehicle-commands).
  The catalog does not prove which strings every BLE vehicle emits. An explicit per-request policy
  defaults to existing-only; only start/stop producers opt in. The runner requires its existing
  current-UUID/replay admission plus an authenticated Infotainment ERROR with counter evidence
  before one completion callback. The exact optional `Infotainment action failed: ` prefix is
  stripped; fuzzy, inverse, unrelated and uncertain reasons are not accepted. An already-finished
  no-op does not synthesize charge telemetry or change retry, wake, session or key behavior.
- **Command outcomes and retries**: `logic/command_fault.hpp` pins all 29 universal fault values
  to `vehicle-command` commit `83baa078e81d08982dd997b8c8e4644e33a046a8` (`pkg/protocol/error.go`).
  BUSY, TIMEOUT, INVALID_SIGNATURE, INVALID_TOKEN_OR_COUNTER, INTERNAL, INCORRECT_EPOCH,
  TIME_EXPIRED and TIME_TO_LIVE_TOO_LONG are temporary. RESPONSE_MTU_EXCEEDED may have executed;
  unknown faults are conservatively uncertain. Payload submission is recorded before the BLE
  write, including a write that returns false after submitting its last chunk. A timeout,
  disconnect, cancellation or unauthenticated fault then ends a mutating command without another
  serialization. Authentication and wake prerequisites do not count as the command payload.
  Known temporary rejections may clear this uncertainty only after the current actual payload's
  GCM fault/AAD, peer replay window and outstanding-request window have all verified. Known
  authenticated inner rejections retain a definite, permanent failure. Explicit read-only queries
  may retry; unlike the reference fixed one-second interval, retry backoff starts at one second,
  doubles up to five seconds, stays within the original deadline and retry limit,
  and precedes prerequisite or payload progression. Typed severity/outcome survives the native
  callback, bounded completion and shared HTTP/MCP reason; allocation failures cannot reopen a
  terminal command. Successful Wake/Whitelist TX-only completion remains a transmission contract.
  Conservative departures are intentional: outer ERROR/NONE reports an unknown outcome without
  retry, whereas the reference `GetError()` returns nil for ERROR/NONE. Unknown outer status stays
  unknown without retry (the reference returns `ErrUnknown`). Outer WAIT/NONE remains pending
  without starting a new mutation, whereas the reference returns `ErrBusy`. Outer `operation_status` is not
  authenticated by the GCM AAD; numeric fault is. Inner VCSEC CommandStatus WAIT remains pending.
  Host controls compile the native receive/transport/result methods with the real patched library,
  independent GCM fixtures and allocation failure injection. These checks do not prove physical
  ATT delivery, pairing, NVS retention, or vehicle acceptance.
- **Command outcome status (REST/MCP)**:
  - *Reference*: every CarServer refusal (`OPERATIONSTATUS_ERROR`) and every VCSEC nominal error becomes a `protocol.NominalError` (`pkg/vehicle/infotainment.go`, `getCarServerResponse`; `pkg/vehicle/vcsec.go`), which the HTTP proxy answers with HTTP 200 and `result:false` (`pkg/proxy/proxy.go`, `handleVehicleCommand`). Other execution errors get Fleet API codes from `httpStatusCode`: 408 for a vehicle that is not awake, 412 for an unpaired key (`pkg/protocol/error.go`, `GetError` → `ErrKeyNotPaired`) and 500 otherwise; a per-VIN lock timeout is 503. Their body is `{"response":null,"error":"<text>","error_description":""}` and carries no `result`. An error that is itself a Tesla server reply (`inet.HTTPError`, Fleet API path only) passes through with that server's status and body (`writeJSONError`).
  - *Firmware*: in a complete reply, success, exact `already_set` and the contextual charge start/stop no-ops described above are HTTP 200 with `result:true`; the reference has no corresponding success exceptions. Every other failed command, whether the car refused it or the device could not deliver it, is HTTP 502 with `response.result:false` and the reason, and MCP marks it `isError`. Intake and out-of-memory failures before dispatch keep their own codes. If the reply cannot be built after the attempt, the degraded HTTP 200 reply reports `outcome:"succeeded"` only for a successful command, including `already_set` and these no-ops, otherwise `result:false` and `outcome:"unknown"`. [API](../API.md#response-classes) owns these response classes.
  - *Why*: status-only clients such as evcc's `tesla-ble` template retry a 502 instead of accepting an undelivered setpoint (#211). The degraded reply is the exception: its 200 alone does not establish success.
  - *Owner and evidence*: `main/http_api.cpp` (`send_command_result`) chooses 200 or 502, `main/mcp_server.cpp` sets `isError`, and `main/logic/command_result.hpp` supplies the exact no-op policies and result text; `main/logic/command_runner.hpp` binds contextual acceptance to the authenticated current reply. Evidence status: static source reading of the firmware, `vehicle-command` at `a4b43c1` and evcc at `2778b8a` (`templates/definition/vehicle/tesla-ble.yaml`, `plugin/http.go`, `util/request/functions.go`, `core/loadpoint.go`, `charger/vehicle-api.go`).
- **Explicit identity cleanup**: Key rotation resets both RAM peers, runner sessions, pending commands and dispatcher state even when session persistence failed and no link is ready. The RX owner receives a deferred reset request. A failed lock or cleanup erase keeps signing gated and the durable rotation journal armed; the journal is removed last. The host harness compiles the native cleanup methods with the real Client/Peer and command runner, using only simulated storage, locks and cache sinks. This does not establish the cause of a historical device key change or replace hardware NVS-retention evidence.
- **CarServer response counters**: A shared read-only dispatcher match rejects foreign UUIDs before parsing or consuming replay state. The AES-GCM response signature variant identifies an authenticated response, including counter zero. After the library verifies the tag, the native adapter applies both the peer and outstanding-request replay windows before command completion or telemetry delivery. Plaintext remains separate. The host harness compiles this adapter directly with the real patched library and public protocol test keys; it checks zero, duplicates, bad tags, nonzero/out-of-order replies, plaintext and foreign UUIDs followed by their correctly addressed replies. No wire or key format changes.
- **SMP channel (NimBLE Security Manager compiled out)**: The reference client (`vehicle-command` on
  Linux, which pins go-ble `8c5522f54333`) has no SMP implementation. Its stub `handleSMP` reads
  `p[0]` of the whole L2CAP PDU, which is the low byte of the L2CAP length and not the SMP opcode
  (`linux/hci/smp.go:44`, `conn.go:259/288`), so it replies only to PDUs whose L2CAP length byte is
  0x01-0x0E (this includes a 2-byte Security Request). The reply is Pairing Failed (opcode 0x05)
  with reason 0x05 "Pairing Not Supported", and its frame declares 6 payload bytes but carries 2
  (`smp.go:28`). This firmware builds ESP-IDF 6.1 NimBLE with `CONFIG_BT_NIMBLE_SECURITY_ENABLE=n`,
  so `ble_sm_cmd_get()` returns NULL, it sends no reply and logs one `ble_sm_rx` ERROR with `rc=6`
  (numerically `BLE_HS_ENOMEM`, although no allocation failed). Neither side pairs, and Tesla does
  not require link-layer security on the VCSEC/infotainment characteristics (`protocol.md` lists no
  link-security requirement). Evidence status: static source reading plus a bench boot/scan on an
  S3; the link-level hold test with the car still open is pending.
- **TX ATT write type (Write Command instead of "write with response")**: `protocol.md` tells
  clients to use the BLE library's "write with response" API for the `0x212` characteristic, and
  `ble.go:121` (`WriteCharacteristic(..., false)`; go-ble `linux/gatt/client.go:249-252`) sends
  every block as an ATT Write Request, so a vehicle ATT Error Response reaches the caller. This
  firmware sends each chunk as an ATT Write Command (`ble_gattc_write_no_rsp_flat` in
  `BleClient::write_chunk_()`, `main/ble_client.cpp`), 10 ms apart (`BleClient::write()`). The
  choice dates from the initial implementation and has no recorded rationale. Neither tesla-ble
  (`BleAdapter::write` only reports "enqueued") nor NimBLE (`ble_gattc_write_flat` already writes
  the CCCD) forces it. Consequence: a successful `write()` only means NimBLE queued the PDUs. A
  chunk the vehicle's ATT layer discards (for example with Insufficient Authentication 0x05 or
  Insufficient Encryption 0x0F) raises no error. The command then fails only through
  `tk::CommandRunner`'s 7 s response or 5 s step timeout and its retries, which looks the same as
  radio loss, and the transmit-completed `Wake` (N1) carries no vehicle-side acceptance at all. The
  vehicle accepts Write Commands in field operation, but Tesla documents no support for them.
  Status: inherited, unchanged by this record. Aligning is a separate wire change that needs
  hardware validation: NimBLE queues ATT requests while one is outstanding (`client_att_busy` in
  `ble_att_cmd.c`), but each Write Request is a GATT procedure bounded by
  `CONFIG_BT_NIMBLE_GATT_MAX_PROCS`, so each chunk would wait for its Write Response through a
  bounded, generation-keyed completion, adding about one connection interval per chunk. No test
  pins the write type. Evidence status: static source reading of the
  firmware, the pinned NimBLE, `vehicle-command` at `a4b43c1` and go-ble `8c5522f54333`, plus field
  operation (the firmware has commanded the vehicle with Write Commands since its initial
  implementation).
- **RX subscription type (notifications instead of indications)**: `protocol.md` names the `0x213`
  characteristic without prescribing notifications or indications. `ble.go:345` subscribes with
  `ind = true`. On Linux, go-ble then writes `0x0002` to the CCCD without consulting the
  characteristic's properties (`linux/gatt/client.go:290-302`, written in `setHandlers` at
  `:318-342`), confirms each indication without waiting for its handler
  (`linux/att/client.go:560-569`) and drops notifications for that subscription
  (`linux/gatt/client.go:379-395`); on macOS it ignores the flag and lets CoreBluetooth choose. This
  firmware discovers the CCCD and writes `0x0001` (notifications) with a Write Request
  (`BleClient::on_dsc_disc()`); readiness requires that write to succeed (`on_subscribe_write()`).
  The `BLE_GAP_EVENT_NOTIFY_RX` handler accepts notifications and indications alike, and NimBLE
  confirms an indication itself once that synchronous callback returns. Rationale: the vehicle
  delivers notifications under `0x0001`, and integrity rests on the application layer rather than
  on ATT confirmation: the length-prefix framer with its 3 s inter-chunk timeout, AES-GCM tags,
  request-UUID matching, and response timeouts with retries. An overflow of the fixed RX slot or of
  the deferred queue drops the link fail-closed. Indications would allow one PDU per confirmation
  round trip and would still not be end-to-end: go-ble confirms without waiting for its handler,
  and this firmware's callback only copies the PDU into the deferred queue that `vehicle_loop`
  parses later.
  Consequence: an RX PDU lost inside the host is recovered only through the framer and command
  timeouts. Pinned only incidentally, by the `on_dsc_disc` program fingerprint in
  `test/test_runtime_boundary_contract.py`. Evidence status: static source reading plus field
  operation; the vehicle's advertised properties for `0x212`/`0x213` have not been read on hardware.
- **No MTU exchange (ATT MTU fixed at 23)**: After subscribing, `ble.go:349` calls `ExchangeMTU`
  and sizes its blocks from the result, falling back to the default MTU on error; go-ble also
  answers a peer's Exchange MTU Request (`linux/att/client.go:542-551`, `:574-591`). This firmware
  neither starts nor accepts the exchange. It only sets the preferred MTU
  (`ble_att_set_preferred_mtu(247)` in `main/ble_client.cpp`, `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=247`)
  and never calls `ble_gattc_exchange_mtu`; nothing linked into it does (in the ESP-IDF 6.1 NimBLE
  tree only sample apps and tests call it, and the link map discards it). Since the ESP-IDF 5.5
  update, a vehicle-initiated exchange cannot take effect either: `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=n`
  disables the NimBLE GATT
  server (`BT_NIMBLE_GATT_SERVER` depends on it, so `MYNEWT_VAL(BLE_GATTS)` is 0), and the
  `BLE_ATT_OP_MTU_REQ` handler is compiled only with `BLE_GATTS` (`ble_att.c`), so such a request
  is dropped (see the next entry). The ATT MTU is therefore 23 by construction, and the
  `BLE_GAP_EVENT_MTU` path that would raise the write payload to `min(MTU - 3, 244)`
  (`tk::ble_write_payload_for_mtu`) is unreachable in this build. A field log is consistent with
  that: on a paired ESP32-S3 at 1.6.3-dev.7, verbose RX logging (`/diag?verbose=1`, about
  15 minutes of VCSEC traffic while the vehicle slept) showed only 4- and 20-byte notifications,
  with longer frames arriving as runs of 20-byte chunks. Consequence: every write uses the 20-byte
  payload (`tk::kBleDefaultWritePayload`), so a 741 B `RoutableMessage` plus its length prefix
  needs 38 chunks, 10 ms apart, instead of 4. TX takes longer and occupies more of the link;
  correctness is unaffected because the frame is length-prefixed. Status: inherited, unchanged by
  this record; having the firmware start the exchange (its GATT client side is compiled) is a
  separate wire change that needs hardware validation. Evidence status: static source reading of
  the firmware, its effective configuration and the pinned NimBLE in the ESP-IDF 6.1 image, plus
  the field log above.
- **No ATT server (inbound ATT requests unanswered)**: go-ble answers a peer's Exchange MTU Request
  directly (`linux/att/client.go:542-551`); any other inbound request goes to its unbuffered
  response channel (`:553-556`) and is answered with "Request Not Supported" once go-ble's own next
  request is pending (`:490-497`), while its RX loop blocks until then. This firmware is built
  without the NimBLE GATT server (`CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=n`). For an
  inbound request without a handler, NimBLE logs "ATT handler not found" at INFO (hidden by the WARN
  level of the NimBLE log tag) and, without `BLE_GATTS`, frees the PDU without any Error Response
  (`ble_att_rx_handle_unknown_request()` in `ble_att.c`). Consequence: a vehicle request (for
  example an Exchange MTU Request) would time out on the vehicle side after the 30 s ATT
  transaction timeout, after which a strictly compliant peer sends no further ATT PDUs on that
  bearer, notifications included, so the link would look connected but stay silent. This has not
  been observed: the field log above shows notifications arriving continuously. Status: inherited,
  unchanged by this record. `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=n` dates from the initial
  implementation, but the gating of the server-side request handlers on `BLE_GATTS` arrived with
  the ESP-IDF 5.5 update (`707eb35`); the NimBLE of the earlier ESP-IDF 5.3 toolchain still
  dispatched the MTU request. Evidence status: static source reading
  of the effective configuration and the pinned NimBLE; whether the vehicle ever sends an ATT
  request to this firmware has not been measured.

### 2.1 Framing Edge Cases and Protocol Deviations (F8)

The native `tk::RxFramer` reassembly implementation covers the critical framing boundary conditions identified during v5.1.4 analysis and verified in `test/test_logic.cpp`:
- **Case 2 (Split frame across multiple notifications)**: Frames fragmented across several BLE notifications are reassembled cleanly without premature completion or state corruption.
- **Case 3 (Several frames in one single notification)**: Multiple concatenated frames arriving back-to-back in a single notification are sliced and delivered sequentially.
- **Case 4 (Corrupt length before split frame)**: A chunk declaring an invalid length (> 2048 B) is rejected immediately as `CorruptLength`, discarding corrupt bytes while allowing subsequent valid split frames to reassemble normally.
- **Case 5 (Zero-length treated as corrupt)**: Frames with decoded length 0 are rejected immediately as `CorruptLength`, preventing infinite or zero-byte parsing loops.
- **Case 13 (Malformed payload inside frame)**: A frame with a malformed payload is delivered deterministically to the upper protocol layer, which handles decoding errors while the framer continues processing subsequent frames.
- **Case 14 / 14a (Duplicate fragment and duplicate frame arrival)**: Duplicate notifications or fragments during a split frame deliver a frame containing the duplicate bytes (which upper-layer decoding rejects fail-closed); extra duplicate residual bytes trigger `CorruptLength` or `BufferOverflow` on subsequent frames or are discarded cleanly on the 3 s inter-chunk timeout.

### 3. Superseding of ADR-0003

ADR-0003 introduced patch 0001 to drop replayed CarServer responses. However, as noted in Issue #306, ADR-0003 only blocked replayed messages where `response_counter > 0`. Plaintext or unauthenticated CarServer responses (`response_counter == 0`) remained open to attribution confusion in `vehicle.cpp`. Under the new dispatcher model, every response must match an active request by UUID (or carry an empty VCSEC UUID with a matching active request-scoped reply address). Thus, ADR-0003 is formally **superseded** by this ADR.

### 4. Relationship to ADR-0002 and Issue #61

This architecture leaves `crypto_context.cpp` and `peer.cpp` upstream in `yoziru/tesla-ble`. It does not move cryptographic operations into the firmware. Consequently:
- It is fully complementary to ADR-0002 and Issue #61 (the ESP-IDF 6.x / Mbed TLS 4 / PSA crypto migration).
- Dropping `vehicle.cpp` kept the PSA bridge small: it is the repository-owned patch `0006-port-crypto-bindings-to-psa.patch`, confined to tesla-ble's crypto bindings (ADR-0002, "Execution").
- Issue #61 tracks that migration until the change carrying patch 0006 is merged. Issue #65 (the PSA-port evaluation) was closed as not planned on 2026-09-23; its findings are folded into #61.

### 5. Delivered Architecture and Patch Retirement

The native BLE orchestration layer is implemented across:
- `main/logic/rx_framing.hpp`: Deterministic 2-byte BE length-prefix framing (`tk::RxFramer`) and the TX framing helpers `drive_command_runner_()` uses (`tk::build_ble_tx_frame`, `tk::is_well_formed_ble_frame`).
- `main/logic/ble_dispatcher.hpp`: Multi-slot UUID and domain dispatcher (`tk::BleDispatcher`).
- `main/logic/session_state.hpp`: Thin `tk::SessionTracker` mirror with monotonic counter progression (`max(local, reported)`), zero in-house crypto.
- `main/logic/command_runner.hpp`: Command state machine arbitrating VCSEC auth, Wake, infotainment auth, and payload dispatch.
- `main/logic/command_result.hpp`: Local exact `already_set` and command-context charging outcome policies.
- `main/vehicle_ctrl.{hpp,cpp}`, `main/vehicle_commands.cpp`, `main/vehicle_telemetry.cpp`, `main/vehicle_pairing.cpp`: Direct integration with `TeslaBLE::Client`.
- Native transactional key generation: `tk::regenerate_private_key()` in `main/logic/key_rotation.hpp` (2048 B PEM export, fail-closed rollback), called by `regenerate_key_native_()` in `main/vehicle_pairing.cpp`.
- `test/test_tesla_ble_harness.cpp` (`scripts/test-tesla-ble-harness.sh`, run in the CI `logic-harness` job) exercises the TX framing helpers, `tk::regenerate_private_key()` and the protocol-vector known answers (V1) against the real patched tesla-ble v6.3.0, Nanopb and the exact Mbed TLS 4.1 / TF-PSA-Crypto commit that ESP-IDF v6.1 pins.

With this architecture in place, patches 0001, 0002, 0003, and the `vehicle.cpp` portion of 0005 were retired. The active patch series in `patches/tesla-ble/` consists strictly of:
1. `0004-drop-unused-parental-controls-actions.patch` (trim unused Nanopb message descriptors for target size budget)
2. `0005-align-session-counter-replay-with-signer-go.patch` (`peer.cpp` session counter monotonic progression alignment per `signer.go`)
3. `0006-port-crypto-bindings-to-psa.patch` (crypto bindings on PSA Crypto for ESP-IDF 6 / Mbed TLS 4; wire bytes and key format unchanged, response tags now verified against the response's own counter, see §2)
4. `0007-fail-closed-on-request-entropy-errors.patch` (reject message construction if routing-address or request-UUID randomness fails)
5. `0008-bind-existing-session-info-key.patch` (domain-specific existing-peer key/secret continuity before SessionInfo refresh or state changes)

### 6. Compilation of Dead Translation Units (TESLABLE_SRCS)

Upstream `CMakeLists.txt` lists `vehicle.cpp` and `tb_logging.cpp` in `TESLABLE_SRCS`. These translation units are compiled during build, but their unused symbols are dropped at link time by the linker's `--gc-sections` flag. This compile-but-drop trade-off avoids a separate source-removal patch.

## v5.3.0 migration (2026-10-11)

The protocol core now stores request hashes separately for VCSEC and Infotainment, requires a
shared secret before a peer becomes valid, and bounds message builders. Native session admission
requires `Peer::is_initialized()` after an update, including restored NVS sessions, so metadata
without a public key cannot mark a fresh peer established. The real-library harness checks fresh,
forced, established and cleared-secret sessions, monotonic counter updates, cross-domain request
hash isolation and invalid-domain lookup, in addition to the unchanged V1 crypto/key vectors.

The firmware still bypasses upstream `Vehicle`/`MessageProcessor`. New upstream timeout/retry,
logging controls and additional action builders therefore do not change native command policy or
expose new HTTP/MCP commands. **Payload retry departure**: native `CommandRunner` rebuilds and
signs a payload for each bounded retry (new UUID/counter), whereas v5.3.0's upstream `Vehicle` can
resend identical serialized bytes. The native policy and its dispatcher request window remain
unchanged; this migration does not import that upstream orchestration behavior. Existing PEM keys,
NVS formats, pairing identity, sleep/wake arbitration and the four firmware targets are preserved.

## v6.1.1 migration (2026-10-16)

The pin resolves to `283490ab49c0624c0ac628761f70459a5ce4b9ae`. Patch 0006 is rebased onto
upstream's removal of session-key logging and preserves the PSA wire/key invariants. Patches
0004 and 0005 still apply. Patch 0007 rejects a failed PSA RNG before encoding or recording a
new request: upstream otherwise uses a clock/address-seeded PRNG for UUIDs and routing addresses.
The harness disables the real PSA RNG to exercise both failure paths, alongside the V1 vectors.

Upstream's request queues, wake arbitration, key-not-paired callbacks and unsigned-fault handling
are in `Vehicle`, which this firmware bypasses. They do not replace native command arbitration,
authentication or the firmware's advisory recovery-hint policy. The new software-update parser
argument is optional and unused; additional action builders expose no new HTTP/MCP commands. `Client` is now immovable;
the firmware already owns it through `unique_ptr`.

Native scans still connect only to the exact VIN-derived `S<16 hex>C` name. Upstream now accepts
other beacon suffixes and `Tesla <VIN suffix>` names, but that broader helper does not expand the
firmware's connection policy. PEM/NVS formats, pairing identity, session persistence and the four
firmware targets are unchanged. Builds and host vectors require separate signed hardware and
live vehicle acceptance.

## v6.2.0 migration (2026-10-18)

The pin resolves to `a00f3872bf4ed166e1c81100ad33a072b126efa1`. All four repository patches
apply unchanged. Upstream adds `ChargingPowerEstimator` and invokes it from `Vehicle` without
changing generated protobufs, `Client`, `Peer`, message builders or crypto bindings. The firmware
uses `Client` directly, so this update does not activate inferred AC power or effective phases:
`parse_charge_state()` still copies vehicle-reported power and phase fields. It adds no commands,
polls or wake paths and changes no PEM/NVS format, pairing identity or session policy.

The comparison uses `teslamotors/vehicle-command` at
`a4b43c1eff0e09d77deb9f2dce97031141fe8c8a`, whose `pkg/vehicle/state.go` returns the decoded
vehicle data. The optional upstream estimator does not change that wire contract. Host vectors
and pinned builds remain separate from signed hardware and live vehicle acceptance.

## v6.3.0 migration (2026-10-27)

The pin resolves to `f7e288e026a19e6bfac0b207922086e57460d860`. All five repository patches
apply unchanged. The ordered patch hashes stay fixed; the source commit and complete
`src/vehicle.cpp` preimage/postimage hashes advance with the upstream source.

Upstream adds an optional terminal-result callback for individual, bulk and follow-up telemetry
polls in `Vehicle`. A completion is an operation outcome, not a fresh state observation. The native
controller uses `Client` directly and does not register or call this API. `polling_policy.h` changes
only its explanation of idle timing. Generated protobufs, `Client`, `Peer`, message builders and
crypto bindings are byte-identical to v6.2.0. No new commands, polls or wake paths are activated;
PEM/NVS formats, pairing identity and session policy remain unchanged.

The existing vehicle-command comparisons and V1 crypto vectors still apply to these unchanged
protocol sources. Host tests and pinned builds do not establish hardware or live vehicle acceptance.

## Consequences

- **Framing Robustness**: Eliminates buffer-recovery heuristics and log storms. BLE stream reassembly matches Tesla reference behavior.
- **Security & Anti-Replay**: Eliminates the plaintext CarServer replay window. Response attribution is strictly bound by request UUID.
- **NVS & Key Preservation**: Zero modification to durable NVS schemas, key formats, or session storage. Existing paired vehicles survive update without re-pairing.
- **Memory evidence**: Pure decisions in `main/logic/` have host tests. Compiler frames and runtime stack minima are separate checks under the [concurrency contract](../ARCHITECTURE.md#concurrency-normative-contract); neither establishes a universal bound on composed call depth or heap demand.
- **Dependency Contract**: Upstream `yoziru/tesla-ble` dependency pin (v6.3.0) and four firmware targets remain explicit; upstream also declares esp32c5.


## Established-session continuity (2026-10-23)

Patch 0008 corrects an inherited refresh difference from
[vehicle-command `fc5a1910`](https://github.com/teslamotors/vehicle-command/tree/fc5a1910ad45c42921d541089e3e366d0b897f52),
`Signer.UpdateSignedSessionInfo` and `UpdateSessionInfo`. A Client verifier must name VCSEC or
Infotainment explicitly. While that domain retains shared key material, the incoming vehicle
public key must match its stored key and the HMAC uses the retained secret. An invalid epoch or
session flag does not remove that binding. A fresh peer still derives its initial secret through
the existing ECDH path. HMAC layout, accepted tag lengths and successful V1 vector bytes are unchanged.
The serialized payload is passed by bounded const reference and its declared length is checked
before use. This avoids a separate pointer/length argument pair at the caller without copying the
payload, allocating memory or adding a helper frame to the verification path.

Native admission and both Peer update methods refuse missing, malformed or different keys before
changing existing counters, epoch, time, replay window, availability or session storage. Direct key
loading also refuses replacement while material remains. The library's own Vehicle caller uses
the same mandatory domain-aware API even though firmware owns orchestration.

This is a peer-lifetime binding, not a durable VIN-to-vehicle-key pin. Existing explicit
`clear_shared_secret/reset` discards the stored public key and secret. Native link-loss reset,
authenticated domain repair and identity cleanup retain that behavior; initial NVS session loading
also retains its existing trust/age limits above. No new key rotation or storage format is added.
The ordinary real-library harness checks both domains, valid refresh, temporary invalidity and
state-preserving refusal. Synthetic driver checks additionally pin secret selection and removed
checks; they do not prove HMAC correctness or radio/vehicle behavior. No real compromise is inferred.
