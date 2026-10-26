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
Remote pairing faults and error text are advisory only: they cannot rotate the key or erase sessions.
Automatic enrollment reuses the stored Charging Manager key with bounded backoff after status
probes; a recovery hint is not proof that the car removed a key. Vehicle approval still requires
the NFC keycard and touchscreen confirmation. See [pairing](USER_GUIDE.md#pairing-and-identity).
Rotation rolls back failed persistence and blocks signing while identity/cleanup is ambiguous.
VIN transitions durably journal cross-namespace work. Protocol/key compatibility and reference
departures are recorded in [ADR-0002](adr/0002-idf6-mbedtls4-crypto-seam.md) and
[ADR-0005](adr/0005-tesla-ble-seam.md).

An existing domain peer binds SessionInfo refreshes to its retained vehicle public key and secret,
even if its session is temporarily invalid. Rejected key changes preserve peer state, availability
and session storage. Explicit peer clear/reset removes that binding; it is not a persistent
VIN-to-vehicle-key pin across reconnects or reboots. The exact lifetime and bootstrap limits are in
[ADR-0005](adr/0005-tesla-ble-seam.md#established-session-continuity-2026-10-23).

## Unencrypted-flash reality (factory devices)

Factory devices normally have Flash Encryption/Secure Boot off and ROM download/JTAG open.
Physical access can expose plaintext secrets. A read-only `espefuse --port <PORT> summary` identifies
per-unit protection; do not infer it from signed OTA support. NVS backups are secret files: never
log, commit, upload, publicly attach or casually inspect them.

### NVS retention

`main/logic/nvs_contract.hpp` owns the exact namespace/key/API/retention inventory and records
which entries contain secrets. The secret flag is classification metadata; it does not encrypt or
redact stored values. The storage adapter rejects unknown records, wrong APIs, collisions and names
over 15 bytes.

| Namespace / logical key (stored key where different) | Owner | Retention |
|---|---|---|
| `tesla_cfg/cfg` | HTTP/provisioning atomic config; separately gated default-off physical cfg repair | durable across OTA |
| `tesla_cfg/wifi_ssid`, `wifi_pass`, `vin`, `mqtt_uri`, `syslog_uri` | legacy config mirror | downgrade compatibility |
| `tesla_cfg/last_time` | clock | replaceable cache |
| `tesla_cfg/vin_txn` | VIN transition | recovery journal |
| `tesla_cfg/ble_mac` | BLE discovery | replaceable cache |
| `tesla_cfg/reboot_why` | independent heap supervisor | recovery journal |
| `tesla_cfg/boot_fails` | boot guard | recovery journal |
| `tesla_cfg/disp_rot` | display | durable across OTA |
| `tesla_cfg/disp_flip` | display | read-only legacy migration |
| `tesla_ble/private_key` | pinned tesla-ble library | durable across OTA |
| `tesla_ble/session_vcsec` (`sess_vcsec`), `session_infotainment` (`sess_info`) | pinned tesla-ble library | replaceable session cache |
| `tesla_ble/paired_at` | pairing | replaceable metadata |
| `tesla_ble/key_created` | pairing | durable across OTA |
| `tesla_ble/key_rotate` | key rotation | recovery journal |

OTA, verified signed app-only USB delivery and installer **Keep configuration** preserve NVS.
The installer defaults to Keep. Each deliberate **Erase everything** selection requires a separate
confirmation with a secret-NVS backup reminder before any flash work starts.
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

Browser mutations require a device-owned Host, matching Origin when present, and no cross-site
fetch. Owned names are the shared DHCP boot name and the current mDNS name (bare or `.local`),
including a conflict rename; owned IPv4 addresses are the active lease and the connection-local
socket address. A failed mDNS lookup grants no mDNS alias. Router-added DNS suffixes are not trusted.
The socket policy is IPv4-only; compilation rejects IPv6 enablement until it has a reviewed policy.
This includes POST and legacy mutating GETs for update check, diagnostic verbosity/clear and coredump
clear. Query keys are case-insensitive; values stay exact, and literal fragments end the query.
Headerless POST clients remain compatible. Mutating GETs need matching Origin/Referer or allowed
fetch-site provenance, or the specific `X-Requested-With` header; otherwise they return 403.
POST aliases exist for ota/check, diag and coredump.

Diagnostic GETs `/status`, `/diag`, `/heap`, `/coredump`, `/ota/status` and `/ota/changelog` require
a device-owned Host before gathering or sending state, including redacted exports. The evcc version,
vehicle-data and body-controller routes retain their existing compatibility behavior. `GET /mcp`
returns a fixed 405 with no diagnostic data. The static UI at `/` and `/index.html` sends
`X-Frame-Options: DENY` and `Content-Security-Policy: frame-ancestors 'none'`; embedding it in an iframe
is unsupported. These headers do not restrict the page's scripts/styles. The open provisioning AP
has its separate physical-range boundary. This is CSRF/rebinding protection, not authentication.

An access-control reverse proxy must send the device IP or `.local` name as upstream Host and
remove or rewrite Origin to match. Browser saves through a router-expanded FQDN can return 403.
Identity guards, update conflicts and typed bounded intake fail before mutation; see
[Architecture](ARCHITECTURE.md#http-request-body-and-allocator-failure-contract).
WiFi rollback restores failed trial credentials once, but cannot protect against an attacker network
that actually grants a lease.

## Syslog and diagnostic export

Syslog sends unredacted console logs over cleartext, unauthenticated UDP. VIN, SSID and MACs may
appear; keep raw logs private and use trusted collectors. Known MQTT URI parse-error records
are replaced before Serial, RAM and Syslog, including when the optional RAM-ring mutex fails.
This narrow guard does not sanitize arbitrary library or fragmented driver output. `/status?redact=1` and `/diag?redact=1` redact known identifiers,
but retain the physical board MAC. Inspect before sharing. Overlong/truncated logical log lines
fail closed; do not join partial generations.

Core dumps are unauthenticated downloads. General DRAM capture is off, but task stacks can contain
transient secrets. Keep dumps private and decode with the exact build ELF. Preserve evidence before
intentional dismissal/deletion.

## OTA self-update

Fixed compile-time HTTPS URLs, bundled-CA verification, target checks and RSA signatures protect
OTA intake. A LAN peer can choose Release, Dev or an available signed PR preview through the
channel and PR selectors, subject to the device's intake/version rules, and can initiate expensive
fetch/reboot work. A peer cannot supply a custom download URL through the API. Signatures
authenticate bytes, not freshness; app-descriptor/channel checks enforce versions.
No eFuse anti-rollback is burned. Pending images retain rollback until runtime health is proven;
[Architecture](ARCHITECTURE.md#ota-self-update) owns confirmation rules and allowed channel transitions.

The USB installer validates the schema-v2 source SHA's format and the four-target roles/offsets.
It verifies byte lengths and SHA-256 hashes for the detected chip's four parts **before erasing/writing**;
otadata activation is last. The manifest is unsigned: the browser does not authenticate its claimed
source commit or inspect the other three chips' bytes. These checks detect mixed/corrupt downloads,
but USB still trusts the chosen Pages origin. Vendored hash-pinned
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
prepare; then the build and independent-rebuild joins → publish → deploy. `build` requires prepare,
both host jobs (`logic-test` and `logic-harness`) and target builds. It is a required status check,
and GitHub treats a check skipped because a need failed as passed, so `build` runs with
`if: ${{ always() }}` and fails first unless every need succeeded. The independent join has no key.
Producer/independent target compiles run concurrently before protected publication.

| Phase | Required proof and authority |
|---|---|
| Unprivileged build | Exact source SHA/fingerprint, four target graphs, size/stack/layout/effective-build checks and unsigned inventory; no signing key or write token. PRs build exact head. |
| Independent rebuild | Fresh cache-free exact-source target legs run concurrently with producer legs; the secret-free join assembles their inventory. The signer compares both complete inventories and payloads before key use. |
| Protected publish | `firmware-signing` Environment; stage regular single-link files with O_NOFOLLOW, rehash, sign, remove key, verify RSA-PSS pin and minimal signed size. Emit only SHA/version-bound artifact. |
| Deploy | No signing Environment/key/OIDC. Consume only named artifact, revalidate inventory/layout/site bytes and channel authority, then write branch-backed Pages and check live bytes. |

The unsigned inventory is a producer claim, not an attestation. Toolchain/build closure is in
[Development](DEVELOPMENT.md#build-and-evidence-contracts); signing runs in the pinned image too.
CI display-version intake requires a numeric `X.Y.Z` core without leading zeroes, an optional
nonempty ASCII alphanumeric/dot/hyphen suffix, and at most 31 bytes. This is not a full SemVer
identifier check. Stable selection requires exactly `X.Y.Z`; device channel/version admission has
its own narrower rules described in [Architecture](ARCHITECTURE.md#ota-self-update).

Stable publication has two fail-closed modes:

- **Create:** bind the current main/tag candidate and prove the Release API returns 404 before
  key use (an existing matching tag is allowed); upload exact assets to a draft; verify aliases,
  ELF checksums and full merged layout, including erased gaps/NVS and exact EOF. Address drafts by
  numeric Release ID. Recheck candidate immediately before making public; require a fresh immutable
  response before accepting it as authority.
- **Reuse:** same-SHA retry accepts only the exact current immutable Release. Read each asset once
  through directory-relative no-follow descriptors, bind API digest/size to that snapshot and compare
  diagnostics/apps/merged images to the independent build. Never sign, upload or mutate that Release.
  Emit a new verified SHA-bound recovery artifact from the snapshot.

Before any upload or mutation, bind all 16 local Pages parts to the four merged images. The artifact
carries the exact 12 root files, 16 signer-owned layout inputs, `dist/` diagnostics/inventory and
`_site/` Pages content. Stable deploy rechecks the root/site binding and Release assets, then
revalidates authority before invoking the branch publisher. Dev deploy binds current main and the
exact staged snapshot without Release metadata and rechecks Pages source before invoking its
publisher. Each root or Dev push attempt and unchanged-byte build request rechecks staged
SOURCE_SHA against fresh remote main. Root uses the same per-attempt staged-source/current-main
admission; immutable Release authority is checked before the publisher.
Cumulative relevance reads the manifest and changelog from one exact branch commit. Stable
acceptance binds firmware to an immutable Release/tag; Dev checks its declared sizes/hashes without
requiring a Release. Both require exact served changelog bytes. Absent Dev authority falls back to
the stable baseline; known metadata or served-byte mismatches keep reconciliation open.

Stable and Dev acceptance first observe the latest Pages build with up to eight status reads and
exponential delays capped at 60 seconds (310 seconds of default waiting). Queued/building, failed
builds and unavailable status have distinct diagnostics before byte acceptance starts. This latest
build observation is a readiness hint, not provenance for a particular firmware snapshot.
Post-publication byte acceptance then retries the complete manifest, all 16 Pages parts and changelog with
exponential delays capped at 60 seconds (190 seconds of default waiting). Each attempt uses a fresh
cache key and must independently pass all
identity, size, hash and byte-binding checks. Results from incomplete attempts are never combined.
Invalid staged Dev bytes, expected changelog or immutable Release metadata/assets fail before Pages polling. This
handles delayed CDN propagation; it does not establish an atomic CDN snapshot.
A stale run must not replace unrepresented firmware. Top-level `docs/*.md` files are shipped Pages
content, so their changes follow the normal four-target build, independent rebuild and protected
Dev publication. Older firmware is never relabelled with a newer documentation source SHA. Unshipped
root Markdown and nested ADR files do not independently select publication; stable Release remains manual.

`main` pushes publish signed **Dev** bytes to `dev/` without a GitHub Release; retries may sign fresh
RSA-PSS bytes. Official stable releases use manual `workflow_dispatch` with `release: true`, publishing
an immutable tagged Release and root Pages. Dispatch without that flag is test-only.

Signed previews require an open labelled same-repository PR, exact live head/label after approval,
byte-identical source/version-bound trusted rebuilds and production signature verification. Version
uses GitHub's `releases/latest` entry, requiring a canonical immutable stable tag and the four
uniquely named digest/size-bearing merged assets; it does not scan for a newer eligible Release or
byte-validate that numeric base's assets. Refetch default-branch head before key,
upload and Pages boundaries; main advancement retires stale runs. Per-PR concurrency couples publisher
and trusted-base cleanup on close/force-push/unlabel; daily reconciliation removes stale manifests.
Key-free [served preview acceptance](DEVELOPMENT.md#served-pr-preview-acceptance) compares all 16
published parts and checks bounded URL absence after removal; a branch write alone is not acceptance.

The only serving authority is branch-backed `gh-pages:/`: root Release, `dev/`, `PR/<N>/`.
Check API Pages mode/source/URL before signing, before invoking branch publication/deletion and
at live acceptance. These workflow checks do not hold a lock on repository Pages settings and are
not repeated inside every branch-push retry.
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

## Configuration-only physical maintenance

The default-off [maintenance role](CONFIG_RECOVERY.md) can replace only the recognized corrupt
`tesla_cfg/cfg` blob after independent full board/key/VIN authority, absent identity journals and
internal non-cfg logical retention checks. It does not erase NVS, force legacy fallback or rotate
identity. One uncertain set/commit parks without retry. Its separate non-OTA app/project/manifest
requires independent source and signed-artifact admission; the ordinary protected publication and
16-part channel paths do not publish maintenance artifacts. This source integration is unqualified
until the owner document's build, signer and physical evidence are independently accepted.
