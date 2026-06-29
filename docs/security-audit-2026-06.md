# Security Audit — tesla-key-esp32 (2026-06)

Full-codebase security review of the firmware at commit `5055f25`. Conducted as five
parallel domain audits — HTTP/network, OTA, crypto/key-storage, BLE radio input,
MQTT/provisioning — with the highest-severity findings cross-verified against source.

> **Remediation status (kept current).** This report is the living record of the audit.
> The per-finding status tags and the [Status](#status) table below are updated as fixes
> land: **H1** is fixed by the PR that adds this document; **M1, L1, L3, L4** landed in
> [#136](https://github.com/0Bu/tesla-key-esp32/pull/136); **L5, L6, L7** landed in
> [#138](https://github.com/0Bu/tesla-key-esp32/pull/138). The remaining open items —
> **M2, M3, M4, L2** — are maintainer decisions / hardening backlog (see each finding).
> The finding *descriptions* are preserved as written at the audit baseline (`5055f25`).

## Threat model

The project's documented decisions are **in scope as assumptions, not findings**:

- **No HTTP auth / TLS — "trusted LAN only"** (`docs/SECURITY.md`). evcc cannot send
  credentials, so the API is unauthenticated by design. Findings are things exploitable
  *within* that model or that weaken it further, not "add auth".
- **Private key in NVS unencrypted — "secure physical access"**. Physical/flash readout
  is an accepted risk.

The `yoziru/tesla-ble v5.1.1` library is fetched at build time and not in-tree; where a
finding depends on it, that is flagged (and grounded against the pinned upstream tag).

## Posture summary

The firmware is defensively written. Verified-sound highlights:

- **OTA signature is genuinely enforced** before install
  (`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y`; `esp_https_ota_finish` rejects a bad
  signature and does not reboot). A compromised update host cannot push unsigned firmware.
- **OTA TLS cert validation is on** (CA bundle attached to both the manifest GET and the
  image download; no skip flags; compile-time HTTPS URLs; no attacker-controlled URL/path).
- **MQTT bridge is read-only** — zero `esp_mqtt_client_subscribe` / `MQTT_EVENT_DATA`; no
  command topics. JSON/topic strings are built with cJSON (auto-escaped), so vehicle-reported
  strings cannot inject. `/status` leaks no MQTT credentials.
- **CI signing-key handling** is correct (gitignored, `chmod 600`, shredded after build,
  refuses to ship unsigned).
- HTTP bodies are length-capped; the wildcard handler has an OOM try/catch returning 503;
  `/diag` streams instead of building one big buffer; `send_json` handles a NULL from a
  fragmented heap; NVS blob I/O uses the correct two-call sizing; sessions are cleared on all
  three re-pair events and anti-replay is delegated to the vehicle clock (not the spoofable
  device wall clock); pairing-revocation heuristics are well-guarded against spurious-revocation
  DoS; BLE advertisement parsing is length-bounded with no OOB, the notify copy is
  `uint16_t`-bounded, and there is no use-after-free in NimBLE callbacks.

The real issues cluster in **entropy at key-gen**, **OTA downgrade**, **the provisioning AP**,
and **BLE peer identity**.

## Findings

### H1 — Root private key generated with weak entropy (RF/TRNG off at first boot) — **FIXED** (this PR)

`main/main.cpp` generated the P-256 key on first boot **before** WiFi or BLE was started, and
the app never called `bootloader_random_enable()`. The ESP32 hardware RNG only returns true
randomness while an entropy source is active (RF enabled, the bootloader running, or
`bootloader_random_enable()`'s SAR-ADC source); otherwise it returns pseudo-random values. The
key is the device's sole authenticator to the car and the OTA trust root, and tesla-ble seeds
its DRBG once and reuses it, so same-boot re-keys inherited the weak seed.

**Fix:** the initial key generation is now wrapped in
`bootloader_random_enable()` / `bootloader_random_disable()` (Espressif's documented pattern
for true randomness before RF is up), seeding the library DRBG from a real entropy source.
Adds the `bootloader_support` component to `main/CMakeLists.txt`. Devices first-keyed before
this fix should be re-keyed + re-paired (`/gen_keys?force=1`, then re-enrol).

### M1 — OTA downgrade: no version gate before flashing, no anti-rollback — **FIXED** (#136)

`ota_start()`/`ota_task` downloaded and flashed unconditionally; `ver_newer()` was used only in
the advisory `/ota/check`, never before the flash. With no eFuse anti-rollback (none burned by
design), an attacker controlling the manifest/image host could serve an **old, legitimately
signed** image carrying a since-patched vulnerability. Signature verification cannot stop this —
the old image is validly signed.

**Fix:** `ota_task` now re-fetches the manifest and refuses to flash anything not strictly newer
than the running version (and refuses if the manifest version is unreadable), right before
`esp_https_ota_begin`. Brick-free, no eFuses.

### M2 — Setup AP transmits home WiFi password over an open network — *open*

`main/provisioning.cpp` brings up the provisioning AP as `WIFI_AUTH_OPEN` (`max_connection=4`),
and `/save` accepts the user's home SSID and password in cleartext. There is no idle timeout, so
an un-configured device stays an open, config-accepting AP indefinitely.

**Recommendation:** WPA2 on the setup AP with a per-device password (MAC-derived / printed on a
label), reduce `max_connection` to 1, add a provisioning timeout — or at minimum document the
exposure window. *(Touches setup ergonomics; left for maintainer decision.)*

### M3 — BLE peer not identity-pinned; spoofable name → garbage into library parser — *open*

`connect()` ignores the address and selects a peer purely by the VIN-derived advertised name
(`main/ble_client.cpp`); the stored `ble_mac` is only a scan-skip optimization. An attacker in
range advertising the same name (the VIN is semi-public) can win the connection and feed crafted
notification fragments into tesla-ble's reassembler/protobuf decoder. Integrity rests on the
session HMAC (a spoofer cannot forge signed replies), so the residual risk is DoS (reset storms
that defeat car-sleep → vampire drain) plus exposing the unaudited library parser to malformed
input. The firmware's try/catch converts a *throw* into a safe link reset, but not a silent OOB
read.

**Recommendation:** prefer the learned `ble_mac` once known and treat a name match from a
different address as suspect; add per-peer reconnect backoff after repeated parse faults. Worth a
focused audit of tesla-ble v5.1.1's reassembly length-handling. *(Address rotation makes strict
pinning non-trivial; left for maintainer decision.)*

### M4 — Reboot-on-POST DoS via `/set_vin` and `/set_mqtt` — *open*

Both handlers `esp_restart()` ~800 ms after persisting a *changed* valid value. A LAN client
alternating two valid values reboots the device every request; `/set_vin` also wipes pairing each
time. Every reboot re-opens the BLE poll window, which also prevents the parked car from sleeping.

**Recommendation:** make reboot an explicit operator action ("saved — reboot to apply") or
rate-limit config-changing endpoints; at minimum document as an accepted LAN DoS. *(Changes
documented UX; left for maintainer decision.)*

### L1 — Unbounded forward clock-set, unauthenticated — **FIXED** (#136)

`http_server.cpp` (`/ota/check?ms=` and `/set_time`) accepted a browser epoch with only a lower
floor (~2023-11), no upper bound. Since the clock gates OTA TLS cert validity, a far-future skew
on NTP-blocked networks can make valid certs look expired / not-yet-valid certs look valid.
**Fix:** a build-relative upper plausibility bound (build year + 10) now rejects implausible
forward skew.

### L2 — VIN/SSID logged at INFO — *open*

`provisioning.cpp` and `main.cpp` log the VIN and SSID at INFO; `/diag` mirrors all logs
unauthenticated. The WiFi password is **not** logged. The current "fingerprint-only, never
key bytes" discipline holds. **Recommendation:** drop VIN/SSID from INFO or gate behind debug
verbosity. *(Open — hardening backlog.)*

### L3 — Unvalidated command integers — **FIXED** (#136)

`http_server.cpp` forwarded `amps`/`pct`/`start` to the car with no range clamp; `(int)valuedouble`
of a huge double is UB. **Fix:** `json_int_clamped()` clamps at the HTTP boundary (amps 0–80,
percent 50–100, start_minutes 0–1439), clamping the double before the int cast.

### L4 — Rollback-cancel on steady-state, not health — **FIXED** (#136)

`main.cpp` called `esp_ota_mark_app_valid_cancel_rollback()` once tasks start, before any functional
check — a boots-but-broken OTA (e.g. OOM-loops only under load) lost its rollback safety net.
**Fix:** `mark_app_valid` is deferred to `ota_health_gate_task`, which validates after the new image
has run healthily for ≈90 s.

### L5 — `strstr`-based route + `force=1` matching — **FIXED** (#138)

`http_server.cpp` matched routes and the `force=1` flag with unanchored substring matching; not a
privilege bypass (no auth by design) but fragile. **Fix:** the dispatcher matches the query-stripped
path (exact match for fixed routes, trailing-segment for the VIN-parameterized routes) and query
flags via `httpd_query_key_value` with an exact value compare.

### L6 — Hardcoded CCCD handle — **FIXED** (#138)

`ble_client.cpp` assumed the CCCD is at `notify_val_handle_ + 1` instead of discovering the
descriptor. **Fix:** the CCCD (0x2902) is discovered via `ble_gattc_disc_all_dscs` before enabling
notifications; not-found or error disconnects rather than enabling the wrong handle.

### L7 — Plaintext MQTT default — **FIXED** (#138)

A bare `host:port` was normalized to `mqtt://`; credentials were cleartext unless the user supplied
`mqtts://`. **Fix:** a schemeless broker defaults to `mqtts://` (CA-bundle-verified) when credentials
are present (configured username or `user:pass@host`); credential-free local brokers stay on plain
`mqtt`; no silent plaintext fallback, and the failure reason surfaces in `/status`.

## Status

| ID | Severity | Status |
|----|----------|--------|
| H1 | High     | **Fixed** (this PR) |
| M1 | Medium   | **Fixed** ([#136](https://github.com/0Bu/tesla-key-esp32/pull/136)) |
| M2 | Medium   | Open — maintainer decision (setup UX) |
| M3 | Medium   | Open — maintainer decision (BLE rotation) |
| M4 | Medium   | Open — maintainer decision (config UX) |
| L1 | Low      | **Fixed** ([#136](https://github.com/0Bu/tesla-key-esp32/pull/136)) |
| L2 | Low      | Open — hardening backlog |
| L3 | Low      | **Fixed** ([#136](https://github.com/0Bu/tesla-key-esp32/pull/136)) |
| L4 | Low      | **Fixed** ([#136](https://github.com/0Bu/tesla-key-esp32/pull/136)) |
| L5 | Low      | **Fixed** ([#138](https://github.com/0Bu/tesla-key-esp32/pull/138)) |
| L6 | Low      | **Fixed** ([#138](https://github.com/0Bu/tesla-key-esp32/pull/138)) |
| L7 | Low      | **Fixed** ([#138](https://github.com/0Bu/tesla-key-esp32/pull/138)) |
