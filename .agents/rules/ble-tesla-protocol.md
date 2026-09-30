# Tesla BLE Protocol & Vehicle Safety Invariants

This scoped policy governs BLE communications, VCSEC protocol handling, and vehicle safety boundaries.
It complements the canonical repository policy in [`../../AGENTS.md`](../../AGENTS.md).

## 1. Vehicle Sleep & Vampire Drain Prevention

- Never send an unsolicited command, wake request or infotainment session to a sleeping vehicle. The deliberately scheduled signed VCSEC `health_probe_` (session/revocation check) may connect outside the active infotainment window because it does not wake the vehicle; no other background path may open a connection or session (see `docs/ARCHITECTURE.md` sleep/link state and ADR-0005).
- Passive telemetry and status endpoints (`GET /status`, `GET /diag`) must consume cached link and state information from RAM and must not initiate a BLE connection or wake sequence.
- Vehicle wake operations (`POST /api/1/vehicles/<VIN>/command/wake_up`) require explicit intent and must respect debouncing and backoff limits.

## 2. VCSEC Anti-Replay & Session Security

- Anti-replay follows ADR-0005: an encrypted response is authenticated against the counter it carries (`AES_GCM_Response_data.counter`) and checked through the replay window of `Peer::validate_response_counter()` per outstanding request, while the session counter only ever advances as `max(local, reported)`. Do not reject a response solely because its counter is less than or equal to the session counter (the vehicle counts responses per request, so out-of-order and multi-part responses are valid), and never lower a session counter.
- Ephemeral session keys, shared secrets, vehicle private keys, and session blobs must never be logged, printed, or exposed via HTTP or MQTT.
- On connection drop, apply exponential backoff. Never loop tightly on reconnection attempts to avoid draining the vehicle or device battery.
