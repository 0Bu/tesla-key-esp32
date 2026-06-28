# Security Audit — tesla-key-esp32 (2026-06)

Full-codebase security review of the firmware at commit `5055f25`. Conducted as five
parallel domain audits — HTTP/network, OTA, crypto/key-storage, BLE radio input,
MQTT/provisioning — with the highest-severity findings cross-verified against source.

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

### H1 — Root private key generated with weak entropy (RF/TRNG off at first boot) — **FIXED**

`main/main.cpp` generated the P-256 key on first boot **before** WiFi or BLE was started, and
the app never called `bootloader_random_enable()`. The ESP32 hardware RNG only returns true
randomness while an entropy source is active (RF enabled, the bootloader running, or
`bootloader_random_enable()`'s SAR-ADC source); otherwise it returns pseudo-random values. The
key is the device's sole authenticator to the car and the OTA trust root, and tesla-ble seeds
its DRBG once and reuses it, so same-boot re-keys inherited the weak seed.

**Fix:** the initial key generation is now wrapped in
`bootloader_random_enable()` / `bootloader_random_disable()` (Espressif's documented pattern
for true randomness before RF is up), seeding the library DRBG from a real entropy source.
Devices first-keyed before this fix should be re-keyed + re-paired
(`/gen_keys?force=1`, then re-enrol).

### M1 — OTA downgrade: no version gate before flashing, no anti-rollback — **FIXED**

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

### L1 — Unbounded forward clock-set, unauthenticated

`http_server.cpp` (`/ota/check?ms=` and `/set_time`) accepts a browser epoch with only a lower
floor (~2023-11), no upper bound. Since the clock gates OTA TLS cert validity, a far-future skew
on NTP-blocked networks can make valid certs look expired / not-yet-valid certs look valid.
**Recommendation:** add an upper plausibility bound (e.g. reject more than a few years past build).

### L2 — VIN/SSID logged at INFO

`provisioning.cpp` and `main.cpp` log the VIN and SSID at INFO; `/diag` mirrors all logs
unauthenticated. The WiFi password is **not** logged. Keep the current "fingerprint-only, never
key bytes" discipline. **Recommendation:** drop VIN/SSID from INFO or gate behind debug verbosity.

### L3 — Unvalidated command integers

`http_server.cpp` forwards `amps`/`pct`/`start` to the car with no range clamp; `(int)valuedouble`
of a huge double is UB. The car is the backstop. **Recommendation:** clamp at the HTTP boundary
(amps 0–80, percent 50–100, start_minutes 0–1439).

### L4 — Rollback-cancel on steady-state, not health

`main.cpp` calls `esp_ota_mark_app_valid_cancel_rollback()` once tasks start, before any functional
check — a boots-but-broken OTA (e.g. OOM-loops only under load) loses its rollback safety net.
**Recommendation:** defer `mark_app_valid` until a meaningful health gate.

### L5 — `strstr`-based route + `force=1` matching

`http_server.cpp` matches routes and the `force=1` flag with unanchored substring matching; not a
privilege bypass (no auth by design) but fragile. **Recommendation:** match exact paths / use
`httpd_query_key_value`.

### L6 — Hardcoded CCCD handle

`ble_client.cpp` assumes the CCCD is at `notify_val_handle_ + 1` instead of discovering the
descriptor. **Recommendation:** discover the CCCD via `ble_gattc_disc_all_dscs`.

### L7 — Plaintext MQTT default

A bare `host:port` is normalized to `mqtt://`; credentials are cleartext unless the user supplies
`mqtts://` (which is supported). Consistent with LAN-only. **Recommendation:** default to TLS when
credentials are configured.

## Status

| ID | Severity | Status |
|----|----------|--------|
| H1 | High     | **Fixed** (this change) |
| M1 | Medium   | **Fixed** (this change) |
| M2 | Medium   | Open — maintainer decision (setup UX) |
| M3 | Medium   | Open — maintainer decision (BLE rotation) |
| M4 | Medium   | Open — maintainer decision (config UX) |
| L1–L7 | Low/Info | Open — hardening backlog |
