# Tesla BLE Protocol & Vehicle Safety Invariants

This scoped policy governs BLE communications, VCSEC protocol handling, and vehicle safety boundaries.
It complements the canonical repository policy in [`../../AGENTS.md`](../../AGENTS.md).

## 1. Vehicle Sleep & Vampire Drain Prevention

- Never proactively send unsolicited commands or connection requests to a sleeping vehicle.
- Passive telemetry and status endpoints (`GET /status`, `GET /diag`) must consume cached link and state information from RAM and must not initiate a BLE connection or wake sequence.
- Vehicle wake operations (`POST /wake`) require explicit intent and must respect debouncing and backoff limits.

## 2. VCSEC Anti-Replay & Session Security

- Responses with a session counter less than or equal to the recorded counter must be strictly rejected (anti-replay protection).
- Ephemeral session keys, shared secrets, vehicle private keys, and session blobs must never be logged, printed, or exposed via HTTP or MQTT.
- On connection drop, apply exponential backoff. Never loop tightly on reconnection attempts to avoid draining the vehicle or device battery.
