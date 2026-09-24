# ADR-0002: IDF-6 / Mbed TLS 4 crypto seam — wait with a deadline, then shim

**Status:** accepted — dormant until the named trigger fires
**Date:** 2026-07-08
**Revised:** 2026-09-24 — facts refreshed (tesla-ble v5.2.0, ESP-IDF 6.1, trigger date, delivery
vehicle for option A); the decision is unchanged
**Relates to:** issue [#61](https://github.com/0Bu/tesla-key-esp32/issues/61) (the full technical
inventory of what breaks), issue [#65](https://github.com/0Bu/tesla-key-esp32/issues/65) (the
PSA-port evaluation, closed as not planned on 2026-09-23; its findings are folded into #61),
[`0005-tesla-ble-seam.md`](0005-tesla-ble-seam.md), architecture review 2026-07 (P5/F3)

## Context

ESP-IDF 6.x bundles Mbed TLS 4 (TF-PSA-Crypto), which takes most of the legacy crypto API out of
the public interface and makes PSA Crypto primary. `yoziru/tesla-ble` — the protocol heart of
this firmware, pinned at v5.2.0 in `main/idf_component.yml` — implements Tesla's P-256 ECDH +
AES-128-GCM directly against that legacy surface: legacy `mbedtls_pk_*` key handling, low-level
`mbedtls_ecp_mul` with `MBEDTLS_PRIVATE` struct-field access, legacy `mbedtls_gcm_*` streaming,
CTR-DRBG/entropy contexts. Issue #61 documents every call site with sources. As of the last check
(2026-09-23, tesla-ble v5.2.0) there is still no upstream PSA branch, PR or release, so **no
version bump restores an IDF-6 build**; this project deliberately holds on ESP-IDF 5.x (Mbed TLS
3.6 LTS — supported, not EOL). The hold is safe today but has a clock on it: IDF 5.x support
ends on Espressif's schedule whether or not upstream moves.

## Options considered

| Option | Complexity | Risk | Note |
|--------|-----------|------|------|
| **A. Shim layer** — local copy of tesla-ble's crypto bindings (a fork, or a repository-owned patch) replacing the legacy `ecp_mul`/`MBEDTLS_PRIVATE`/`pk_*`/`gcm_*` internals with PSA calls | Medium | Medium | Mostly confined to `crypto_context.{h,cpp}`/`peer.cpp`, plus `client.h` (`MBEDTLS_ECP_MAX_PT_LEN`) and `vin_utils.cpp` (SHA-1); keeps the diff reviewable; candidate for upstreaming, which would end the local copy |
| **B. Wait for upstream** Mbed TLS 4 support | None now | Timeline unknown | IDF 5.x end-of-support is the deadline clock |
| **C. Reimplement the crypto in-project** | High | High | **Rejected** — protocol crypto is exactly what you don't fork away from the reference implementation |

## Decision

**B with a deadline, then A.**

- **Now:** stay on ESP-IDF 5.x and the pinned tesla-ble (v5.2.0). Re-check upstream
  (`yoziru/tesla-ble` branches/PRs/releases for PSA / Mbed TLS 4 / IDF-6 work) on **every
  Renovate `espressif/idf` image PR** — the `.github/renovate.json` `prBodyNotes` reminder on
  those PRs is the recurring checkpoint; it points at #61 and at this ADR's trigger. The pinned
  v5.5 line is in its maintenance period, so those PRs are infrequent; re-check on the dated
  trigger below as well.
- **Trigger to start A:** ESP-IDF 5.x (the line pinned by tag+digest in
  `esp-idf-toolchain.txt`) enters its **final 12 months of Espressif support** with no
  usable upstream Mbed TLS 4 support released or imminent. For v5.5 (first released
  2025-07-21), the 30-month window in Espressif's `SUPPORT_POLICY.md` puts end of support at
  ≈ January 2028, so the trigger fires **≈ January 2027**; confirm the exact date against
  Espressif's published support-period chart. v5.5 is the last 5.x minor release, so no newer
  5.x line can extend the hold. When a Renovate check or that dated checkpoint finds the
  trigger true, open the port issue and begin the shim.
- **Executing A** (multi-day, later): port the crypto bindings of the pinned tag —
  `crypto_context.{h,cpp}` + `peer.cpp`, plus `client.h` (`MBEDTLS_ECP_MAX_PT_LEN`) and
  `vin_utils.cpp` (SHA-1) — to PSA (`psa_generate_key`/`psa_raw_key_agreement` for P-256 ECDH,
  `psa_aead_*` for AES-128-GCM, PSA RNG replacing CTR-DRBG); keep `mbedtls_pk` for PEM
  import/export so the NVS key format and existing pairings survive; leave the protocol code
  untouched. Carry the port for ALL targets either as a fork or as a repository-owned patch in
  the ordered, hash-checked `patches/tesla-ble/` series (see
  [`0005-tesla-ble-seam.md`](0005-tesla-ble-seam.md)); the choice is tracked in #61. (The
  ADR-0001 `path:`/`rules:` plumbing this sketch originally named as the template was removed
  with [`0004-drop-esp32c5-target.md`](0004-drop-esp32c5-target.md).) Offer the port upstream
  so the local copy can be retired. Port the first-party key-fingerprint helper
  (`compute_key_fingerprint_()` in `main/vehicle_pairing.cpp`) in the same change. Also fold in
  the mechanical IDF-6 items from #61: `json` → `espressif/cjson`, `mqtt` → `espressif/mqtt`,
  the W5500 driver → `esp-eth-drivers`, the SECP192/224 Kconfig keys IDF 6 no longer defines
  (set in `sdkconfig.defaults`), warnings-as-errors fallout, the `esp_https_ota` partial-download
  opt-in, the contracts and baselines bound to v5.5.5, and the esp32c6 flash budget; then bump
  the image and digest in `esp-idf-toolchain.txt` and hardware-smoke-test
  pairing/commands/telemetry/OTA.

## Consequences

- No effort is spent while upstream may still solve it — but the decision, the trigger and
  the execution sketch survive context loss (this ADR), so the port starts on schedule, not
  in a panic when IDF 5.x support ends.
- The Renovate reminder gains a concrete question to answer ("has the trigger fired?")
  instead of a vague caution.
- Accepting a temporary local copy of the crypto bindings (option A, as a fork or a patch) is a
  conscious trade: protocol correctness stays upstream's; only the crypto *bindings* are
  ported, and the local copy is retired the moment upstream ships equivalent support.
