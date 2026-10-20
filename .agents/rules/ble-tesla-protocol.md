# BLE protocol and vehicle safety

Apply [AGENTS.md](../../AGENTS.md), [runtime sleep/link policy](../../docs/ARCHITECTURE.md#sleep--link-state-the-single-source-of-truth)
and [ADR-0005](../../docs/adr/0005-tesla-ble-seam.md).

- No unsolicited command, wake or Infotainment session for a sleeping car. Only the scheduled non-waking
  VCSEC `GET_STATUS` health probe and bounded automatic same-key enrollment may connect outside
  the active window. Enrollment requires a configured VIN, readable storage, verified runtime key,
  command FIFO recheck and Stable-only OTA exclusion; it uses Charging Manager/CLOUD_KEY,
  NoWakeFail and backoff without immediate BLE retries. Plaintext status and recovery hints are
  advisory and never authorize key replacement or session deletion. The bounded wake/bootstrap one-shot may refresh charge
  and drive on an already connected independently awake car with NO_WAKE_SKIP, retry backoff and
  episode quiescence. Status, logs and telemetry read caches.
- Explicit wakes respect debounce/backoff. Reconnect uses exponential backoff, never a tight loop.
- Authenticate encrypted replies using their carried response counter and per-request replay window.
  Session counters advance only as `max(local, reported)`; a response counter ≤ session counter is
  not itself replay evidence (multipart/out-of-order replies are valid).
- Encrypted VCSEC requires initialized secret context and a non-empty request hash; missing
  context, bad tags or repeated counters (including zero) drop before decoding or telemetry.
- Never log/expose private keys, shared/session secrets or session blobs over HTTP/MQTT.
