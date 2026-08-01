# ADR-0003: Reject replayed CarServer responses before dispatch

Status: accepted

## Context

`yoziru/tesla-ble` v5.1.1 decrypts and parses a CarServer response, calls
`Peer::validate_response_counter()`, and logs when that anti-replay check fails. It nevertheless
continues processing the invalid response: vehicle-data callbacks run and an `actionStatus` or
`vehicleData` payload can complete the command currently at the head of the library's single
FIFO.

That is not only noisy duplicate telemetry. If an older response is recovered from a damaged BLE
receive buffer while a later command is waiting, the replay can be attributed to the later
command. For charging-current control this makes a historical `actionStatus=OK` look like the
acknowledgement for the latest amp request, or makes historical ChargeState data look fresh.

The production incident that motivated this decision showed all three conditions together:
repeated receive-buffer recovery, duplicate response-counter warnings, and charging-current
commands whose accepted response did not prove the car's effective current.

## Decision

Keep the dependency pinned at upstream v5.1.1 and commit a minimal source patch under
`patches/tesla-ble/`: after a non-zero response counter fails validation, log it and immediately
return from `handle_carserver_message_()`. No state callback runs and the replay cannot complete
the FIFO head.

Root CMake invokes `scripts/apply-tesla-ble-patches.sh` after ESP-IDF dependency resolution and
before compilation. The script patches both possible source locations:

- `managed_components/yoziru__tesla-ble` for esp32, esp32s3, esp32c3 and esp32c6;

It is idempotent and fails closed if the patch no longer applies to the pinned source. This makes
an upstream version bump require an explicit rebase/review instead of silently losing the fix.

The firmware separately treats the Tesla charging action ACK as provisional and verifies the
requested amp value using a new, explicit ChargeState request. Each decoded ChargeState first
clears the previous snapshot so a newly omitted field cannot inherit an old value/presence bit.
These controls are
complementary: the dependency patch establishes that the response is not a replay; the firmware
readback establishes that the accepted action actually changed the car's effective setting.

## Consequences

- All five targets compile the same anti-replay behavior.
- A replay is visible in logs but cannot alter caches or command outcomes.
- Dependency source is modified only in ignored, generated checkouts; the reviewable patch is
  committed.
- Builds require the standard `patch` utility, present in the pinned ESP-IDF image.
- When upstream rejects invalid counters itself, remove this patch and ADR wiring after verifying
  the pinned release contains equivalent behavior.
