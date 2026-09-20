# ADR-0003: Reject replayed CarServer responses before dispatch

Status: superseded by upstream yoziru/tesla-ble v5.2.0 (incorporated into upstream `src/vehicle.cpp` and closed by Request-UUID gating)

## Context

In `yoziru/tesla-ble` v5.1.1, the library decrypted and parsed CarServer responses, called
`Peer::validate_response_counter()`, and logged when that anti-replay check failed. It nevertheless
continued processing the invalid response: vehicle-data callbacks ran and an `actionStatus` or
`vehicleData` payload could complete whatever sat at the head of the library's single command FIFO.

Historical analysis initially characterized this as risk of an earlier command's response
completing a later waiting command. In cryptographic reality, that cross-command path was not
reachable for encrypted responses: their AEAD Associated Data (AD) binds the SHA-1 request hash
under the derived session keys, so a stale encrypted response from an earlier command fails
decryption entirely. What patch 0001 actually blocked was duplicate delivery of the exact same
response when the old, unbuffered RX recovery algorithm rescanned a damaged buffer and re-emitted
already processed frames.

Conversely, the genuinely unguarded cross-command gap existed for unauthenticated *plaintext*
CarServer responses: `parse_payload_car_server_response()` returns `response_counter = 0` for
unauthenticated frames, and the anti-replay check was strictly gated on `response_counter > 0`.
Patch 0001 therefore never ran for plaintext responses at all. In the upstream Go reference
implementation (`teslamotors/vehicle-command`), both plaintext and encrypted responses pass
through a unified UUID-keyed handler map (`internal/dispatcher/dispatcher.go:245-315`), making
cross-command misattribution impossible.

## Historical Decision (v5.1.1–v5.1.3)

Keep the dependency pinned and apply a minimal source patch (`patches/tesla-ble/0001-...`): after
a non-zero response counter fails validation, log it and immediately return from
`handle_carserver_message_()`, dropping duplicate frame deliveries before callbacks or FIFO
completion.

Complementarily, the firmware treated charging action ACKs as provisional and verified requested
current through an explicit subsequent `ChargeState` readback.

## Superseded in v5.2.0

Upstream `yoziru/tesla-ble` v5.1.4 / v5.2.0 resolved this across both dimensions:

1. **Anti-replay early return is upstream**: `src/vehicle.cpp:965-966` returns immediately on
   failed `validate_response_counter()`, identical to former patch 0001. Patch 0001 was dropped.
2. **Request-UUID gating closes the cross-command gap**: `src/vehicle.cpp:931-941` matches incoming
   CarServer responses against the last requested UUID (`get_last_request_uuid()`). A late or
   foreign response is discarded with `LOG_WARNING("Ignoring CarServer response for a different request")`,
   so foreign or plaintext responses end in a clean timeout rather than false completion.

## Consequences

- Patch 0001 is deleted from the repository patch series.
- Cross-command misattribution is closed cryptographically for encrypted responses (AEAD AD binding)
  and architecturally for all CarServer responses (Request-UUID matching).
- Mismatched CarServer responses manifest as timeouts rather than false completions.
