# ADR-0002: IDF-6 / Mbed TLS 4 crypto seam — wait with a deadline, then shim

**Status:** accepted — dormant until the named trigger fires
**Date:** 2026-07-08
**Relates to:** issue [#61](https://github.com/0Bu/tesla-key-esp32/issues/61) (the full technical
inventory of what breaks), architecture review 2026-07 (P5/F3)

## Context

ESP-IDF 6.x bundles Mbed TLS 4, which removes the legacy crypto API surface and makes PSA
Crypto primary. `yoziru/tesla-ble` v5.1.1 — the protocol heart of this firmware — implements
Tesla's P-256 ECDH + AES-128-GCM directly against that removed surface: legacy `mbedtls_pk_*`
key handling, low-level `mbedtls_ecp_mul` with `MBEDTLS_PRIVATE` struct-field access, legacy
`mbedtls_gcm_*` streaming, CTR-DRBG/entropy contexts. Issue #61 documents every call site with
sources. As of its last check there is no upstream PSA branch or release, so **no version bump
restores an IDF-6 build**; this project deliberately holds on ESP-IDF 5.x (Mbed TLS 3.6 LTS —
supported, not EOL). The hold is safe today but has a clock on it: IDF 5.x support ends on
Espressif's schedule whether or not upstream moves.

## Options considered

| Option | Complexity | Risk | Note |
|--------|-----------|------|------|
| **A. Shim layer** — vendored tesla-ble fork replacing the legacy `ecp_mul`/`MBEDTLS_PRIVATE`/`pk_*`/`gcm_*` internals with PSA calls | Medium | Medium | Confined to `crypto_context.cpp`/`peer.cpp`; keeps the diff reviewable; candidate for upstreaming, which would end the fork |
| **B. Wait for upstream** Mbed TLS 4 support | None now | Timeline unknown | IDF 5.x end-of-support is the deadline clock |
| **C. Reimplement the crypto in-project** | High | High | **Rejected** — protocol crypto is exactly what you don't fork away from the reference implementation |

## Decision

**B with a deadline, then A.**

- **Now:** stay on ESP-IDF 5.x / tesla-ble v5.1.1. Re-check upstream (`yoziru/tesla-ble`
  branches/PRs/releases for PSA / Mbed TLS 4 / IDF-6 work) on **every Renovate
  `espressif/esp-idf` PR** — the `.github/renovate.json` `prBodyNotes` reminder on those PRs
  is the recurring checkpoint; it points at #61 and at this ADR's trigger.
- **Trigger to start A:** ESP-IDF 5.x (the line CI builds with, currently v5.5.x per
  `.github/workflows/build.yml`) enters its **final 12 months of Espressif support** with no
  usable upstream Mbed TLS 4 support released or imminent. When a Renovate check finds that
  true, open the port issue and begin the shim.
- **Executing A** (multi-day, later): fork tesla-ble at the pinned tag; port
  `crypto_context.cpp` + `peer.cpp` to PSA (`psa_generate_key`/`psa_raw_key_agreement` for
  P-256 ECDH, `psa_aead_*` for AES-128-GCM, PSA RNG replacing CTR-DRBG); leave the protocol
  code untouched; route ALL targets through the fork (the ADR-0001 `path:`/`rules:` plumbing
  is the template — this time not target-gated); offer the port upstream so the fork can die.
  Also fold in the mechanical IDF-6 items from #61: `json` component → `espressif/cjson`,
  warnings-as-errors fallout, `esp_https_ota` partial-download opt-in, then bump
  `esp_idf_version` in CI and hardware-smoke-test pairing/commands/telemetry/OTA.

## Consequences

- No effort is spent while upstream may still solve it — but the decision, the trigger and
  the execution sketch survive context loss (this ADR), so the port starts on schedule, not
  in a panic when IDF 5.x support ends.
- The Renovate reminder gains a concrete question to answer ("has the trigger fired?")
  instead of a vague caution.
- Accepting a temporary fork (option A) is a conscious trade: protocol correctness stays
  upstream's; only the crypto *bindings* are ported, and the fork is retired the moment
  upstream ships equivalent support.
