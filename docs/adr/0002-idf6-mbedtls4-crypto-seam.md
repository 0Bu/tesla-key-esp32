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

Patch 0006 ported v5.2.0 to PSA on ESP-IDF v6.1. PSA owns P-256 key generation/agreement,
AES-128-GCM, HMAC-SHA256, hashes and RNG. `mbedtls_pk` still imports/exports legacy SEC1 PEM,
preserving enrolled keys and NVS. A failed nonce draw aborts encryption. First-boot key load and
generation run with SAR-ADC entropy before the radios start.

The previous binding computed but did not check response tags. `psa_aead_verify` verifies them.
Response AAD uses the counter in the response (`TAG_COUNTER`), which can differ from the request
counter. Failed authentication wipes plaintext. [Protocol mapping](0005-tesla-ble-seam.md#2-platform-forced-departures-and-reference-deviations)

V1 uses the pinned Mbed TLS software drivers and tests P-256 public export, `SHA1(X)[:16]` ECDH
derivation, session HMAC/tag, GCM ciphertext/tag, independent response AAD, wrong-counter and
tampered-tag refusal, VIN BLE name, legacy PEM export/reload and the mirrored key fingerprint.
It does not execute Mbed TLS 3.6, the target's PSA hardware driver or the firmware fingerprint
helper. See the [protocol harness](../../test/README.md#logic-ble-protocol-and-presentation).

The pinned v6.1 build uses registry archives for cJSON and MQTT: Git resolution of their
submodules hashes cache-dependent gitlink paths. IDF's component-private Mbed TLS `-Os` remains
necessary: the historical C6 `-Og` variant projected signed `0x1f1000`, beyond slot and policy.
[Current build inputs and flags](../DEVELOPMENT.md#build-and-evidence-contracts)

| Historical scope | Result or required proof |
|---|---|
| ESP32-S3 bench, IDF 5.5.5 bootloader, pre-coredump table and unenrolled key | Signed OTA to 6.1, 6.1-to-6.1 and return via Pages passed. Unsigned and throwaway-key candidates were refused. Key fingerprint/creation time, VIN and WiFi persisted. BLE found the VIN-derived name. The local HTTPS bench channel used a dedicated CA. |
| Enrolled W5500 S3 | The owner required separate pre-merge signed-preview checks for existing-key sessions, commands, telemetry, evcc, sleep/wake and Ethernet. OTA health confirmation does not establish those results. |
| esp32, esp32c3, esp32c6 | Build evidence only in this evaluation. |

The forward 5.5.5-to-6.1 OTA reused the installed bootloader. Do not install a 5.x-built app
under a v6.1 bootloader. [Retained-bootloader admission](../DEVELOPMENT.md#retained-bootloader-references)

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
