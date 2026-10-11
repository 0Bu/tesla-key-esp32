# ADR-0002: Port crypto bindings to PSA for ESP-IDF 6

**Status:** accepted; executed 2026-09-24 as patch 0006 on ESP-IDF v6.1.
**Date:** 2026-07-08; revised 2026-09-24.
**References:** issues [#61](https://github.com/0Bu/tesla-key-esp32/issues/61),
[#65](https://github.com/0Bu/tesla-key-esp32/issues/65), [ADR-0005](0005-tesla-ble-seam.md).

## Context

Mbed TLS4 removes the legacy bindings used by tesla-ble v5.2.0. Waiting for upstream preserved IDF5
compatibility but depended on its support deadline. Reimplementing protocol crypto was rejected.

## Decision

The July decision was to wait until the final twelve months of IDF5 support, then port the bindings.
The owner chose to execute early in September. Carry one ordered hash-checked repository patch,
not a second fork, retain upstream protocol code and PEM/NVS keys, and retire it when equivalent
upstream support becomes available. The old deadline is superseded; it is no longer an action trigger.

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
- **Response authentication — the one behaviour change in September.** Upstream v5.2.0 computed the AES-GCM tag of an
  encrypted response (`mbedtls_gcm_finish`) and never compared it, so the ESP-IDF 5 firmware
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

## Rebase onto v5.3.0 (2026-10-11)

The new release fixes response-tag verification and response-counter metadata upstream, but still
uses the Mbed TLS 3 crypto API. Patch 0006 remains necessary and is rebased onto v5.3.0. Its
legacy caller edits in `client.cpp` and `vehicle.cpp` are retired because upstream now passes the
response counter itself. The PSA implementation preserves authenticated decryption, failure
wiping, wire bytes and PEM/NVS key reuse; the separate response-AAD helper remains an explicit
API departure in ADR-0005. New upstream peer-validity logic is retained: session metadata without
a shared secret cannot initialize a fresh peer. The V1 vectors and real-library session/hash
regressions are the migration contract; build results alone do not prove vehicle acceptance.

## Consequences

Every IDF migration also updates the harness `MBEDTLS_REF` and dependency contract `MBEDTLS_COMMIT`
to the image's exact submodule, then reruns V1 vectors. The build fails closed on a mismatch.
The local binding patch is an explicit temporary maintenance obligation, not permission to diverge
from Tesla wire semantics. Recorded bench/live scope above is historical evidence, not acceptance
of later source revisions or all target hardware.
