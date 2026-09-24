# ADR-0002: IDF-6 / Mbed TLS 4 crypto seam — wait with a deadline, then shim

**Status:** accepted — executed 2026-09-24 (option A, as the repository-owned patch
`patches/tesla-ble/0006-port-crypto-bindings-to-psa.patch`, on ESP-IDF v6.1)
**Date:** 2026-07-08
**Revised:** 2026-09-24 — facts refreshed (tesla-ble v5.2.0, ESP-IDF 6.1, trigger date); the
maintainer then chose to execute option A ahead of the trigger, delivered as a patch rather
than a fork (see "Execution")
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
(2026-09-23, tesla-ble v5.2.0) there was still no upstream PSA branch, PR or release, so **no
version bump restores an IDF-6 build**. Until the port recorded in "Execution", this project
deliberately held on ESP-IDF 5.x (Mbed TLS 3.6 LTS — supported, not EOL). The hold was safe but
had a clock on it: IDF 5.x support ends on Espressif's schedule whether or not upstream moves.

## Options considered

| Option | Complexity | Risk | Note |
|--------|-----------|------|------|
| **A. Shim layer** — local copy of tesla-ble's crypto bindings (a fork, or a repository-owned patch) replacing the legacy `ecp_mul`/`MBEDTLS_PRIVATE`/`pk_*`/`gcm_*` internals with PSA calls | Medium | Medium | Mostly confined to `crypto_context.{h,cpp}`/`peer.cpp`, plus `client.h` (`MBEDTLS_ECP_MAX_PT_LEN`) and `vin_utils.cpp` (SHA-1); keeps the diff reviewable; candidate for upstreaming, which would end the local copy |
| **B. Wait for upstream** Mbed TLS 4 support | None now | Timeline unknown | IDF 5.x end-of-support is the deadline clock |
| **C. Reimplement the crypto in-project** | High | High | **Rejected** — protocol crypto is exactly what you don't fork away from the reference implementation |

## Decision

**B with a deadline, then A.** (Kept as decided on 2026-07-08. "Execution" below records that A
was carried out on 2026-09-24, before the trigger fired; it supersedes "Now" and the trigger, and
the Renovate note on `espressif/idf` PRs now asks for the Mbed TLS pins instead, see
"Consequences".)

- **Now** (superseded by "Execution")**:** stay on ESP-IDF 5.x and the pinned tesla-ble (v5.2.0).
  Re-check upstream (`yoziru/tesla-ble` branches/PRs/releases for PSA / Mbed TLS 4 / IDF-6 work)
  on **every Renovate `espressif/idf` image PR** — the `.github/renovate.json` `prBodyNotes`
  reminder on those PRs was the recurring checkpoint; it pointed at #61 and at this ADR's trigger. The pinned
  v5.5 line is in its maintenance period, so those PRs are infrequent; re-check on the dated
  trigger below as well.
- **Trigger to start A** (moot since "Execution")**:** ESP-IDF 5.x (the line pinned by tag+digest in
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

## Execution (2026-09-24)

The maintainer decided to execute option A ahead of the dated trigger instead of waiting for
upstream. The evaluation recorded in #61 found no reason for a fork:

- **Delivery vehicle: patch, not fork.** The port is one more entry in the ordered, hash-checked
  `patches/tesla-ble/` series (0006, after 0004/0005) against the unchanged pin `v5.2.0`. The
  Component Manager only verifies local modifications of managed components when
  `IDF_COMPONENT_STRICT_CHECKSUM` is set, so the existing apply step works on ESP-IDF 6 as it
  did on 5.x. Renovate keeps tracking upstream, a tesla-ble change that touches the patched
  lines fails the build instead of silently mis-applying, and the same diff can be offered
  upstream. A fork would carry identical code and flash cost plus a second repository.
- **Scope of 0006.** `crypto_context.{h,cpp}`, `peer.cpp`, `client.h` and `vin_utils.cpp`: the
  client key lives in the PSA key store (`psa_generate_key`, `psa_raw_key_agreement`,
  `psa_export_public_key`), AES-128-GCM and HMAC-SHA256 use `psa_aead_*`/`psa_mac_compute`,
  SHA-1/SHA-256 use `psa_hash_compute`, and `psa_generate_random` replaces CTR-DRBG.
  `mbedtls_pk` remains only for SEC1 PEM import/export, so stored keys and pairings survive. The
  firmware's key-fingerprint helper moved to the same public PK→PSA path. A failed RNG read for a
  GCM nonce now fails `encrypt` instead of encrypting under an unfilled nonce.
- **Response authentication — the one behaviour change.** Upstream computes the AES-GCM tag of an
  encrypted response (`mbedtls_gcm_finish`) and never compares it, so the ESP-IDF 5 firmware
  accepted any response tag. `psa_aead_verify` compares it, and 0006 keeps that, as
  vehicle-command's `Signer.Decrypt` does. Verifying the tag exposed an upstream departure the
  unchecked tag had hidden: tesla-ble built the response metadata from its own request counter,
  while protocol.md and `Signer.Decrypt` bind `TAG_COUNTER` to the counter the response carries
  (the vehicle counts responses per request; VCSEC sends up to three). 0006 follows the reference:
  `Peer::decrypt_response` takes the response's counter, `construct_response_ad_buffer` builds
  the response metadata, `construct_ad_buffer` refuses the response type, and all three call
  sites (tesla-ble `client.cpp` and `vehicle.cpp`, `VehicleController::handle_vcsec_frame_()`)
  pass it. ADR-0005 lists the departure.
- **Wire and key invariants, with evidence.** `test/test_tesla_ble_harness.cpp` part V1 runs the
  vehicle-command protocol vectors through the patched library, built against the Mbed TLS commit
  ESP-IDF v6.1 pins (`MBEDTLS_REF` in `scripts/test-tesla-ble-harness.sh`; the host builds its
  builtin software drivers, while the firmware sends AES-GCM through the ESP PSA driver): official
  client public key, ECDH session key `SHA1(X)[:16]`, session-info HMAC and the official
  session-info tag, AES-GCM request bytes and tag against an independent one-shot reference,
  response metadata serialized independently from protocol.md with a response counter that
  differs from the request counter (an AAD from the request counter is refused), tampered-tag
  rejection with plaintext wiping, VIN BLE name, a PEM export byte-identical to the Mbed TLS 3.6
  export, and a host mirror of the firmware fingerprint (`compute_key_fingerprint_()` itself is
  IDF code) equal to tesla-ble's key id.
- **Randomness.** ESP-IDF 6 configures `MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG`: every PSA random draw
  (key generation, key-parse blinding, GCM nonces) reads the hardware RNG at call time; there is
  no DRBG seeded once. `main/main.cpp` keeps SAR-ADC entropy enabled across key load and
  first-boot key generation, before Wi-Fi/BLE supply RF entropy.
- **Toolchain.** ESP-IDF v6.1 (Mbed TLS 4.1.0 / TF-PSA-Crypto 1.1.0, GCC 15.2, Picolibc), with
  `espressif/mdns` and `espressif/w5500` (esp32s3 only) taken from their Git release sources,
  and `espressif/cjson` 1.7.19~2 and `espressif/mqtt` 1.1.0 from the component registry, all
  pinned per target in `dependencies.lock.*`. The two registry components carry Git submodules
  (upstream cJSON; the paho MQTT test broker); a Git resolution hashes the submodule's `.git`
  gitlink, whose `gitdir:` path depends on where the Component Manager cache sits, so such a
  lock hash only verifies on the machine that wrote it. The registry archives are fixed bytes
  built from the same sources.
  `esp_https_ota` partial download is not used, so its new opt-in stays off.
- **Bootloader compatibility.** ESP-IDF supports booting apps built by a newer IDF with an older
  bootloader, which is the OTA path from 5.5.5 firmware; the chip-revision bounds and the 64 KiB
  flash MMU page layout are unchanged. The reverse is not supported: a device installed with the
  v6.1 bootloader must not be downgraded to a 5.x-built release.
- **Bench evidence (2026-09-24).** A bench esp32s3 as installed in the field (v5.5.5
  second-stage bootloader, pre-coredump partition table, a key in NVS that is not enrolled in the
  car). OTA URLs are compile-time and the signed PR preview rebuilds with the pin on the default
  branch (the intended trust boundary), so a real OTA from 5.5.5 used a bench channel: a v5.5.5
  build of the default branch whose manifest URL points at a local HTTPS server, with a throwaway
  CA appended to the bundle. Every image was signed with the production key and verified against
  `scripts/ota-signing-public-key.sha256`; bench versions sat below the current release so the
  board returned to production over the real Pages channel. Proven: OTA 5.5.5 → 6.1 with the
  v5.5.5 bootloader booting the v6.1 app and the health gate committing it; OTA 6.1 → 6.1;
  `/ota/check` against Pages and OTA 6.1 → the production release; signature enforcement on 6.1
  (a throwaway-key-signed and an unsigned image are both rejected); key fingerprint, key creation
  time, VIN and Wi-Fi unchanged through every step; BLE finds the car by its VIN-derived name.
- **Live acceptance before merge (owner decision, 2026-09-24).** The enrolled-key checks run on
  the production esp32s3 W5500 board: BLE session setup with the existing enrolled key, signed
  commands, telemetry, the evcc end-to-end path, sleep/wake, and the W5500 Ethernet path, which
  no bench board covers. Once #327 added OTA from signed PR preview channels, the owner moved them
  ahead of the merge: the board installs this PR's signed preview (`/ota/check?pr=<N>`,
  `/ota/update?pr=<N>`), whose `<release>-PR-<N>` version a plain update check replaces with the
  stable release of the same core. An image that never gets a lease stays pending under the
  health gate, and the next power cycle rolls the board back. The health gate does not look at
  BLE, so the BLE and command checks are the acceptance, not the gate. esp32, esp32c3 and esp32c6
  are covered by builds only.
- **Mbed TLS optimization.** ESP-IDF 6 builds the Mbed TLS / TF-PSA-Crypto libraries at `-Os` by
  default, as a component-private option. `sdkconfig.defaults` pins it and
  `scripts/check-build-semantics.py` enforces it: at `-Og` esp32c6 reaches a projected signed
  `0x1f1000`, past the `0x1e8000` policy and the slot. It is not the rejected whole-build `-Os`
  (ADR-0004); the live acceptance runs under the evcc + BLE load that froze whole-build `-Os`.
- **USB recovery.** Because a v6.1 bootloader does not boot a 5.x-built app, `$usb-recovery`
  reads the installed bootloader's ESP-IDF version and refuses an app built by an older one.
- **Other ESP-IDF 6.1 defaults the firmware inherits.** Picolibc replaces newlib (the
  double-precision `printf`/`scanf` variants are linked, so cJSON number output is unchanged);
  `CONFIG_FREERTOS_IN_IRAM` is off (ISR-context FreeRTOS functions stay in IRAM, and the firmware
  installs no ISR of its own); `CONFIG_MBEDTLS_THREADING_C` is on (PSA key-store locking);
  `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` is off; `CONFIG_HTTPD_ENABLE_EVENTS`,
  `CONFIG_ESP_HTTP_CLIENT_STRICT_HEADER_BUFFER` and `CONFIG_ESP_HTTPS_OTA_VERIFY_SPI_MODE` are on.
  Assertions stay enabled in silent mode, so `CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE=n` has no
  effect, and `CONFIG_FREERTOS_ISR_STACKSIZE` stays 2096 because the core dump is enabled.

## Consequences

- Decision-time (superseded by "Execution"): no effort was spent while upstream might still
  solve it, and the decision, the trigger and the execution sketch survived context loss (this
  ADR), so the port could start on schedule rather than in a panic when IDF 5.x support ends.
- Decision-time: the Renovate reminder on `espressif/idf` PRs asked a concrete question ("has
  the trigger fired?"). Since the execution it asks instead to move `MBEDTLS_REF` and
  `MBEDTLS_COMMIT` and rerun the V1 vectors (last consequence below).
- Accepting a temporary local copy of the crypto bindings (option A, delivered as patch 0006)
  is a conscious trade: protocol correctness stays upstream's; only the crypto *bindings* are
  ported, and the patch is retired the moment upstream ships equivalent support.
- Every ESP-IDF bump now also moves the Mbed TLS / TF-PSA-Crypto the port runs on. The firmware
  build fails closed (`scripts/check-dependency-contract.py --idf-path`) until the harness
  `MBEDTLS_REF` and the contract's `MBEDTLS_COMMIT` name the new image's mbedtls submodule
  commit; update both and rerun the V1 vectors (the Renovate note on `espressif/idf` PRs says so).
