# Security

This document owns exposure, secrets, signing and publication trust. Runtime recovery is in
[Architecture](ARCHITECTURE.md); API fields/errors are in [API](API.md).

## Threat model

The NVS P-256 private key is a valid Tesla **Charging Manager** key: it can read, wake and control
charging, but cannot unlock or drive. NVS also holds WiFi credentials, VIN and MQTT configuration.

| Boundary | Risk and mitigation |
|---|---|
| Physical USB/JTAG | Factory flash is readable and firmware replaceable; protection requires separately planned hardware hardening. |
| LAN/API | Unauthenticated plaintext control; trusted LAN, firewall/VLAN or authenticated reverse proxy. |
| BLE | Tesla application crypto, authenticated responses, request binding and replay windows; NimBLE SMP/bonding is compiled out on all targets. |
| Build/publication | Pinned inputs, independent rebuild, protected signing and byte-bound artifacts. |

First-boot key generation enables hardware entropy before WiFi/BLE. IDF6 PSA draws from the hardware
RNG. Devices keyed before this fix need a deliberate re-key/re-pair, not an automatic identity change.
Remote pairing faults and error text are advisory only: they cannot rotate the key, erase sessions,
or trigger enrollment. Re-enrollment uses the stored key only after an explicit user request.
Rotation rolls back failed persistence and blocks signing while identity/cleanup is ambiguous.
VIN transitions durably journal cross-namespace work. Protocol/key compatibility and reference
departures are recorded in [ADR-0002](adr/0002-idf6-mbedtls4-crypto-seam.md) and
[ADR-0005](adr/0005-tesla-ble-seam.md).

## Unencrypted-flash reality (factory devices)

Factory devices normally have Flash Encryption/Secure Boot off and ROM download/JTAG open.
Physical access can expose plaintext secrets. A read-only `espefuse --port <PORT> summary` identifies
per-unit protection; do not infer it from signed OTA support. NVS backups are secret files: never
log, commit, upload, publicly attach or casually inspect them.

### NVS retention

`main/logic/nvs_contract.hpp` owns the exact namespace/key/API/retention/secrecy inventory.
The storage adapter rejects unknown records, wrong APIs, collisions and names over 15 bytes.

| Namespace / logical key (stored key where different) | Owner | Retention |
|---|---|---|
| `tesla_cfg/cfg` | HTTP/provisioning atomic config | durable across OTA |
| `tesla_cfg/wifi_ssid`, `wifi_pass`, `vin`, `mqtt_uri`, `syslog_uri` | legacy config mirror | downgrade compatibility |
| `tesla_cfg/last_time` | clock | replaceable cache |
| `tesla_cfg/vin_txn` | VIN transition | recovery journal |
| `tesla_cfg/ble_mac` | BLE discovery | replaceable cache |
| `tesla_cfg/reboot_why` | heap watchdog | recovery journal |
| `tesla_cfg/boot_fails` | boot guard | recovery journal |
| `tesla_cfg/disp_rot` | display | durable across OTA |
| `tesla_cfg/disp_flip` | display | read-only legacy migration |
| `tesla_ble/private_key` | pinned tesla-ble library | durable across OTA |
| `tesla_ble/session_vcsec` (`sess_vcsec`), `session_infotainment` (`sess_info`) | pinned tesla-ble library | replaceable session cache |
| `tesla_ble/paired_at` | pairing | replaceable metadata |
| `tesla_ble/key_created` | pairing | durable across OTA |
| `tesla_ble/key_rotate` | key rotation | recovery journal |

OTA, verified signed app-only USB delivery and installer **Keep configuration** preserve NVS.
**Erase everything**, merged-image writes and full flash erases destroy it. NVS remains at
`0x9000` for `0x6000` bytes. Firmware rollback does not restore keys/configuration. Destructive or
identity work needs explicit authorization and a backup/rollback plan.

## HTTP API exposure

Port 80 has no authentication or TLS. Any LAN peer can call commands, change configuration/identity,
trigger updates or remove crash evidence; the key role bounds car privileges, not device control.
Never port-forward the device. Setup AP is open and provisioning uses plaintext HTTP; keep its
window short. With stored credentials the open setup AP closes when the network comes up, after 10
minutes without a client, and 30 minutes after opening even with one; a failed close is retried
with backoff, the boot does not continue while the AP is up, and a power cycle reopens it.
First-time setup, stored credentials that fail validation, and a credential rollback that could not
be saved keep it open until the form is saved. mDNS advertises version, not VIN.

Browser mutations require a device-owned Host (name or current lease IP), matching Origin and no
cross-site fetch. This includes POST and legacy mutating GETs for update check, diagnostic verbosity/
clear and coredump clear. Query keys are case-insensitive; values stay exact. Headerless POST clients remain compatible. Mutating GETs need matching Origin/Referer or allowed
fetch-site provenance, or a custom header such as X-Requested-With; otherwise they return 403.
POST aliases exist for ota/check, diag and coredump. The open provisioning AP has its separate
physical-range boundary. This is CSRF/rebinding protection, not authentication.

An access-control reverse proxy must send the device IP or `.local` name as upstream Host and
remove or rewrite Origin to match. Browser saves through a router-expanded FQDN can return 403.
Identity guards, update conflicts and typed bounded intake fail before mutation; see
[Architecture](ARCHITECTURE.md#http-request-body-and-allocator-failure-contract).
WiFi rollback restores failed trial credentials once, but cannot protect against an attacker network
that actually grants a lease.

## Syslog and diagnostic export

Syslog sends unredacted console logs over cleartext, unauthenticated UDP. VIN, SSID and MACs may
appear; use trusted collectors. `/status?redact=1` and `/diag?redact=1` redact known identifiers,
but retain the physical board MAC. Inspect before sharing. Overlong/truncated logical log lines
fail closed; do not join partial generations.

Core dumps are unauthenticated downloads. General DRAM capture is off, but task stacks can contain
transient secrets. Keep dumps private and decode with the exact build ELF. Preserve evidence before
intentional dismissal/deletion.

## OTA self-update

Fixed compile-time HTTPS URLs, bundled-CA verification, target checks and RSA signatures protect
OTA intake. A LAN peer cannot select arbitrary firmware but can initiate expensive fetch/reboot
work. Signatures authenticate bytes, not freshness; app-descriptor/channel checks enforce versions.
No eFuse anti-rollback is burned. Pending images retain rollback until runtime health is proven;
[Architecture](ARCHITECTURE.md#ota-self-update) owns confirmation rules and allowed channel transitions.

The USB installer verifies schema-v2 manifest source SHA, exact four-target roles/offsets, byte lengths
and all SHA-256 hashes **before erasing/writing**; otadata activation is last. These hashes detect
mixed/corrupt deployments, but USB still trusts the chosen Pages origin. Vendored hash-pinned
`esptool-js@0.6.1` runs under `script-src 'self'`, with no CDN execution.

## Signed OTA images

Secure Boot v2 RSA-3072 signatures are checked without hardware Secure Boot. No eFuses are burned;
the compile phase emits unsigned bytes and never receives the production key.
**Unsigned local images cannot boot**: startup signature checks cause a loop before `app_main`.
Use a verified signed artifact and an explicitly authorized delivery procedure.

### Trust anchor (trust-on-first-use)

Without an eFuse key digest, the running signed app supplies the trusted key. Later OTA must use
that same authority. Classic esp32 needs ECO3/revision v3.0+; the other floors are in the
[user guide](USER_GUIDE.md).
USB can establish a different authority, but ordinary OTA cannot.

`scripts/ota-signing-public-key.sha256` independently pins production authority. Every production app
passes full RSA-PSS/SHA-256 verification against it after signing, during publication/reuse and before
physical delivery. Chip, size, version and cross-artifact equality do not authenticate the signer.
This pipeline pin is not a second device trust anchor.

### Create the signing key

A separately authorized key setup uses a dedicated offline RSA-3072 key, exponent 65537,
unencrypted PKCS#1/PKCS#8 PEM. Do not generate production keys in CI or reuse a GPG key.
Run **one** of the following alternatives in a private offline directory outside the repository.
Do not overwrite an existing signing key; `umask 077` restricts newly created files to owner access.

With esptool v5 or later, use the current `espsecure` command and hyphenated subcommand:

```bash
umask 077
espsecure generate-signing-key --version 2 --scheme rsa3072 ota_signing_key.pem
```

Alternatively, generate the same key type with [OpenSSL](https://docs.openssl.org/3.6/man1/openssl-genpkey/):

```bash
umask 077
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 \
  -pkeyopt rsa_keygen_pubexp:65537 -out ota_signing_key.pem
```

OpenSSL writes unencrypted PKCS#8 PEM, which `espsecure` accepts directly.
Validate with a disposable payload on the trusted machine. Protect at least two offline backups.
Loss requires fleet USB recovery; compromise defeats signed OTA. Never pass the real key through
agent tools, logs, repository files or tests. Tests use throwaway keys only.

### Release pipeline and trust boundaries

Counts and file sets are enforced by validators: 53 allowlisted unsigned payloads, 12 root firmware
files, 28 diagnostics, 40 stable Release assets and 16 manifest parts. Do not use globs to broaden them.
The main DAG starts prepare and both host gates together; the two per-target build legs need only
prepare; then the build and independent-rebuild joins → publish → deploy. `build` requires prepare, both host jobs (`logic-test`
and `logic-harness`) and target builds; the independent join has no key. Producer/independent target
compiles run concurrently before protected publication.

| Phase | Required proof and authority |
|---|---|
| Unprivileged build | Exact source SHA/fingerprint, four target graphs, size/stack/layout/effective-build checks and unsigned inventory; no signing key or write token. PRs build exact head. |
| Independent rebuild | Fresh cache-free exact-source target legs run concurrently with producer legs; secret-free join binds the unsigned comparison before any key. |
| Protected publish | `firmware-signing` Environment; stage regular single-link files with O_NOFOLLOW, rehash, sign, remove key, verify RSA-PSS pin and minimal signed size. Emit only SHA/version-bound artifact. |
| Deploy | No signing Environment/key/OIDC. Consume only named artifact, revalidate inventory/layout/site bytes and channel authority, then write branch-backed Pages and check live bytes. |

The unsigned inventory is a producer claim, not an attestation. Toolchain/build closure is in
[Development](DEVELOPMENT.md#build-and-evidence-contracts); signing runs in the pinned image too.
Every display version obeys the canonical grammar and 31-byte bound.

Stable publication has two fail-closed modes:

- **Create:** prove tag/Release absent before key use; upload exact assets to a draft; verify aliases,
  ELF checksums and full merged layout, including erased gaps/NVS and exact EOF. Address drafts by
  numeric Release ID. Recheck candidate immediately before making public; require a fresh immutable
  response before accepting it as authority.
- **Reuse:** same-SHA retry accepts only the exact current immutable Release. Read each asset once
  through directory-relative no-follow descriptors, bind API digest/size to that snapshot and compare
  diagnostics/apps/merged images to the independent build. Never sign, upload or mutate that Release.
  Emit a new verified SHA-bound recovery artifact from the snapshot.

Before any upload or mutation, bind all 16 local Pages parts to the four merged images. The artifact
carries the exact 12 root files plus 16 signer-owned layout inputs. Stable deploy rechecks these and all Release assets, then revalidates authority immediately before
branch writes. Dev deploy instead binds current main and the exact staged snapshot without Release
metadata; every push attempt or unchanged-byte build request rechecks staged SOURCE_SHA against
fresh remote main. Conflict retries cannot restore superseded Dev bytes. Cumulative
release relevance starts from a branch/live snapshot whose manifest SHA, immutable Release/tag and
assets agree; a docs-only or stale run must not replace unrepresented firmware.

`main` pushes publish signed **Dev** bytes to `dev/` without a GitHub Release; retries may sign fresh
RSA-PSS bytes. Official stable releases use manual `workflow_dispatch` with `release: true`, publishing
an immutable tagged Release and root Pages. Dispatch without that flag is test-only.

Signed previews require an open labelled same-repository PR, exact live head/label after approval,
byte-identical source/version-bound trusted rebuilds and production signature verification. Version
is derived from the newest complete immutable stable Release. Refetch default-branch head before key,
upload and Pages boundaries; main advancement retires stale runs. Per-PR concurrency couples publisher
and trusted-base cleanup on close/force-push/unlabel; daily reconciliation removes stale manifests.

The only serving authority is branch-backed `gh-pages:/`: root Release, `dev/`, `PR/<N>/`.
Check API Pages mode/source/URL before signing, every branch write/delete and live acceptance.
Environment reviewer configuration and required PR-policy checks are repository settings; code cannot
install them. Green provenance proves source-to-bytes relationships, not source safety, runner trust
or physical operation. Independent review, Environment approval and bench acceptance remain separate.

### Key rotation

Software TOFU accepts one signature block at position zero. Adding an old/new block pair or changing
the CI key/pin cannot rotate the installed fleet. Plan key and pin together, retain old-key recovery,
USB-install new-key signed apps on explicitly authorized boards while preserving NVS, verify each
new authority, then retire the old key. Cross-authority rollback is USB-only.

## Enabling Flash Encryption + Secure Boot (recommended, IRREVERSIBLE)

Hardware protection requires a separate per-unit migration. eFuse changes are permanent and can
remove USB recovery. Read the target-specific [Flash Encryption](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/security/flash-encryption.html)
and [Secure Boot](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/security/secure-boot-v2.html)
contracts before designing it.

Use release-mode encryption, hardware Secure Boot v2 and NVS encryption with a reviewed `nvs_keys`
partition migration that preserves NVS offsets. Prepare signed/encrypted delivery and recovery before
burning anything; the ordinary Web Serial installer writes plaintext and is no longer a valid update
path. This repository's unchanged partition table and normal build flow do not perform that migration.

## Development tooling trust boundary

[AGENTS.md](../AGENTS.md) owns authorization. Read-only reviewers and lexical hooks do not replace
sandboxing, protected environments or human approval. Actions are full-SHA pinned with limited
permissions/timeouts; GitHub's runner OS and orchestration remain trusted infrastructure.
Bench ingest hashes the report, not proof of physical firmware/signature/NVS observations.

Generated `.codex/` adapters must match canonical reviewer/hook sources. The native PreToolUse
adapter runs the safety core before the PR core synchronously; malformed or oversized payloads and
bootstrap failures deny execution. Correct configuration alone does not grant hook trust: approve
project hooks through the runner UI and verify their effective state. Linked worktrees load native
declarations from the primary checkout. Read-only reviewer defaults can inherit parent runtime
permission overrides; confirm the effective sandbox before treating a reviewer as independent.

The pinned Context7 package avoids floating updates but is neither an npm integrity lock nor privacy
sandbox. First use executes downloaded code; service inputs leave the local boundary. Never send
keys, credentials, dumps, unredacted captures or CI secrets; disable locally if policy forbids it.
Firmware builds and CI do not depend on Context7.
