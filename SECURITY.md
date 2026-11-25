# Security

This document owns exposure, secrets, signing and publication trust. Runtime recovery is in
[Architecture](ARCHITECTURE.md); API fields/errors are in [API](API.md).

## Threat model

NVS contains the P-256 Tesla **Charging Manager** key, WiFi credentials, VIN and MQTT settings.
The key grants vehicle reads, wake and charging. Protect it as a vehicle credential.

| Boundary | Risk and control |
|---|---|
| USB/JTAG | Physical access can read secrets and replace firmware. [Hardware protection](#enabling-flash-encryption--secure-boot-recommended-irreversible) |
| LAN/API | Plain HTTP control is unauthenticated. [Network isolation](#http-api-exposure) |
| BLE | Encrypted replies are authenticated and replay-checked. UUID/address and one-response checks on accepted plaintext replies do not authenticate the sender. SMP and bonding are compiled out. [Protocol departures](adr/0005-tesla-ble-seam.md#2-platform-forced-departures-and-reference-deviations) |
| Build/publication | Pinned source, independent rebuild, protected signing and byte-bound artifacts. [Publication pipeline](#release-pipeline-and-trust-boundaries) |

First-boot generation enables hardware entropy before WiFi/BLE. IDF6 PSA uses the hardware RNG.
Keys from before the entropy fix need deliberate re-key/re-pair. Remote faults cannot authorize
identity rotation or session erase. Private-key buffers read into RAM during controller startup or
fingerprint calculation are wiped via RAII across normal return and exception unwind, and HTTP
identity mutations seal their `OtaIdentityMutationGuard` on committed/ambiguous recovery unwind so
the operation gate stays held until reboot. See [pairing lifecycle](ARCHITECTURE.md#pairing-lifecycle--invalidation)
and the [lifetime of an established peer binding](adr/0005-tesla-ble-seam.md#established-session-continuity-2026-10-23).

## Unencrypted-flash reality (factory devices)

Factory devices normally have Flash Encryption/Secure Boot off and ROM download/JTAG open.
Physical access can expose plaintext secrets. A read-only `espefuse --port <PORT> summary` identifies
per-unit protection; do not infer it from signed OTA support. NVS backups are secret files: never
log, commit, upload, publicly attach or casually inspect them.

### NVS retention

[`nvs_contract.hpp`](../main/logic/nvs_contract.hpp) owns application and library namespaces,
stored keys, APIs, owners, retention and secret classification. The adapter rejects unknown
records, aliases that collide, invalid APIs and names over 15 bytes. Secret classification
provides neither encryption nor redaction. The same partition also holds SDK WiFi and PHY records.
Legacy display keys are ignored and left in NVS.

NVS stays at `0x9000/0x6000`. OTA and verified app-only USB preserve it. The runtime accepts OTA
slots at `0x20000` and `0x210000`, each `0x1f0000` bytes. Historical slot layouts need separate
key-preserving USB/layout recovery. App-only writes leave the partition table intact.
[App-only procedure and layout proof](DEVELOPMENT.md#installed-layout-proof)

Installer **Flash** preserves NVS by checking the installed boot-slot project and the NVS range.
An inconclusive read leaves both options available. In that case select **Flash** only for a known
tesla-key-esp32 installation. Use the flasher stub: the ROM loader's 16 KiB erase blocks can hit
NVS during a partition-table write. A foreign NVS layout or contents can prevent boot.

**Clean flash**, merged writes and whole-chip erase destroy NVS. Confirm the destructive action
and arrange a private NVS backup before writing. Firmware rollback cannot restore keys or settings.
Identity changes and NVS writes require their own authorized scope.
[Installer operation](USER_GUIDE.md#updates-and-recovery)

## MQTT transport

Broker URIs accept optional case-insensitive `mqtt://` and `mqtts://` schemes and reject all other
schemes. Schemeless broker entries select TLS (`mqtts://`) when a username or password or URI
userinfo exists; credential-free entries select plaintext (`mqtt://`). Explicit `mqtt://` and
`mqtts://` schemes are normalized to lowercase scheme prefixes while keeping authority/userinfo case
and honoring the explicit transport direction, so an explicit `mqtt://` URI can send credentials
without encryption. TLS verifies bundled CA roots and has no plaintext fallback. Status exposes the
selected transport and connection error without broker credentials. Configuration belongs to the
[user guide](USER_GUIDE.md#home-assistant-mqtt); the
[runtime owner](ARCHITECTURE.md#home-assistant-mqtt-bridge) defines admission and retries.

## HTTP API exposure

Port 80 has no authentication or TLS. Any LAN peer can call commands, change configuration/identity,
trigger updates or remove crash evidence; the key role bounds car privileges, not device control.
Never port-forward the device. Setup AP is open and provisioning uses plaintext HTTP; keep its
window short. With a running WiFi station or a successfully started Ethernet driver waiting for a lease, the open
setup AP closes when the network comes up, after 10
minutes without a client, and 30 minutes after opening even with one; a failed close is retried
with backoff, the boot does not continue while the AP is up, and a power cycle reopens it.
Without either recovery transport, first-time setup or unusable credentials keep it open until
the form is saved, including after Ethernet driver construction fails. An unsaved rollback also keeps the
terminal portal open. mDNS advertises version, not VIN.

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
a device-owned Host before gathering or sending state, including redacted exports.
The foreground `body_controller_state` GET also requires an owned Host, while evcc clients without
Origin/Referer remain supported. A cross-site request addressed directly to a known device IP can
still pass this Host check and cause a foreground BLE read; Host validation does not establish
caller identity. The evcc version and cache-only vehicle-data routes retain their compatibility behavior. `GET /mcp`
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
but retain the physical board MAC. The ring flattens embedded line breaks in recognized
identifier-bearing records so a value cannot escape per-line redaction. This also changes raw
RAM-log exports; other Serial/Syslog records keep their original bytes. Inspect before sharing.
Overlong/truncated logical log lines
fail closed; do not join partial generations.

Core dumps are unauthenticated downloads. General DRAM capture is off, but task stacks can contain
transient secrets. Keep dumps private and decode with the exact build ELF. Preserve evidence before
intentional dismissal/deletion.

## OTA self-update

Fixed HTTPS URLs and bundled CA verification protect feed transport. The pinned signature
authenticates application bytes. Channel and descriptor admission enforce the version floor.
There is no eFuse anti-rollback. A LAN peer can change channel, select a signed preview and
consume fetch/reboot resources, but cannot supply a custom feed URL.
[Runtime OTA and rollback](ARCHITECTURE.md#ota-self-update)

Offer IDs bind confirmation to the checked channel, PR, notes/version and image hash/size.
They prevent accidental substitution. Treat neither the opaque ID nor the unsigned manifest as
authentication or a CSRF credential. Verify the confirmed image bytes and existing RSA authority.
[Offer API](API.md#management)

Saved `dev` permits a signed app upload without feed publication. Any LAN peer can set `dev`:
it is an operator opt-in, not authentication. HTTP does not attest source SHA, review or PR
gates. Select the exact authorized signed image and verify the target before uploading. An
uncertain response never authorizes a retry.
[Upload API](API.md#direct-signed-firmware-upload) · [Local OTA procedure](DEVELOPMENT.md#local-ota-test-before-merge)

The Web Serial installer checks the schema-v2 source SHA format and roles/offsets for four targets.
It bounds the detected target's four downloads to declared sizes and 60 s, rejects redirects and
checks lengths/hashes before any write. Otadata activation is last. The unsigned manifest does not
prove source or authenticate the other chips' bodies. Trust the selected Pages origin.
CSP `script-src 'self'` limits execution to the site and its
[vendored bundle](THIRD_PARTY.md).

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

Enforced inventories include 53 unsigned payloads, 12 signed root firmware files, 28 diagnostics,
40 stable Release assets and 16 manifest parts. Use the validator-owned file sets.

| Phase | Proof and authority |
|---|---|
| Unprivileged build | Exact source SHA/fingerprint, four locked targets, size/stack/layout/effective-build gates and key-free output |
| Independent rebuild | Fresh exact-source target legs and their own complete cache-free inventory with no producer artifact access |
| Protected publish | Both host jobs and both artifact joins complete. Compare inventories and bytes before `firmware-signing` key use. Stage regular single-link files with O_NOFOLLOW, rehash, sign, remove the key, then verify RSA-PSS pin and signed size. |
| Deploy | Key-free consumer validates the named artifact, signature/layout/site bytes and live authority before Pages writes. [Served acceptance](DEVELOPMENT.md#served-pages-acceptance) |

On main, the cumulative relevance decision admits the host suites for publication work.
For publishing candidates, the producer and independent matrices run alongside both host jobs.
Their artifact joins finish as soon as their respective legs pass. The protected publisher and
deploy alone use `filter: blob:none`.
They retain full history/tags and hydrate the exact source tree before any key use. No lazy Git
fetch or history reads while the key is provisioned. Dev signing overlaps a pinned-image prefetch
with key-free admission. Require a successful pull and refreshed source/version before provisioning.
[Build inputs and proof](DEVELOPMENT.md#build-and-evidence-contracts)

Relevance is a scheduling decision. Mutation boundaries recheck current authority. Top-level
`docs/*.md` ships to Pages and selects normal four-target Dev publication. Never relabel older
bytes with a newer source SHA. Root Markdown and nested ADRs do not independently select it.
Ordinary PRs skip builds. `current-head-records` checks operator records for the exact head.
Publication needs its own GitHub build and acceptance.

Build display versions accept a numeric `X.Y.Z` core without leading zeroes and an optional
nonempty ASCII alphanumeric/dot/hyphen suffix. The entire version is at most 31 bytes. Stable requires exact `X.Y.Z`.
Main publishes to `dev/` without a GitHub Release. Stable is a manual `release: true` dispatch.
Dispatch without it is test-only. Dev retries may sign fresh RSA-PSS bytes.

- **Stable creation.** Bind current main/tag and require Release API 404 before signing. An
  existing matching tag is allowed. Upload exact assets to a numeric draft ID. Verify aliases,
  ELF checksums, merged layout, erased gaps/NVS and EOF. Recheck authority before publishing and
  confirm the fresh immutable Release.
- **Stable reuse.** Only the exact current immutable Release for the same SHA qualifies. Read
  each asset once through no-follow descriptors. Bind digest/size to the snapshot and compare
  diagnostics/apps/merged images to the rebuild. Reuse is key-free and leaves the Release intact.
- **Root recovery.** Authenticate the original unexpired Actions ZIP and all 40 current immutable
  Release assets, signatures, tag/source and main ancestry. Preserve the complete original site.
  Repeat authority checks for every push/build request. Expired, changed or unauthenticated input
  stops recovery. [Recovery procedure](DEVELOPMENT.md#recover-a-published-stable-site)

Bind all 16 Pages parts to the verified merged images. First stable publication and Dev recheck
staged `SOURCE_SHA` against live main for each branch push or unchanged-byte build request.
The publisher maintains one empty, regular single-link root `.nojekyll`. Reject a nonempty,
linked or nonregular marker before and after staging. Legacy recovery permits exactly this extra control file. Relevance reads
manifest and changelog from one exact branch commit. Stable binds to immutable Release/tag. Dev
binds declared size/hash. Missing Dev authority falls back to stable. Known mismatches keep
reconciliation open.

**Previews** require an open labelled same-repository PR, exact live head/label, complete host and
protocol checks, independent byte equality and production signatures. Trusted code chooses the
version from `releases/latest` (immutable canonical tag, four uniquely named merged assets with
digest/size). It does not scan for a substitute or verify bytes merely to select the version.
Recheck live default-branch head before key use, upload and Pages writes. Per-PR concurrency
couples publication with cleanup on close, force-push or label removal. Daily reconciliation
removes stale manifests. Cleanup runs current default-branch code.
[Served preview acceptance](DEVELOPMENT.md#served-pr-preview-acceptance)

Serving authority is branch-backed `gh-pages:/` at root, `dev/` and `PR/<N>/`. Check Pages
mode/source/URL before signing and branch mutation and at served acceptance. These checks do not
lock repository settings or run inside every ordinary branch retry. Environment approval,
required repository checks, source review and bench acceptance remain independent.

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

Generated Codex and Antigravity adapters must match the canonical reviewer and hook sources.
The synchronous PreToolUse hooks guard secrets, partitions and protected GitHub actions.
Trust the hook definitions through the runner UI. Codex worktrees can load declarations from
the primary checkout. Codex reviewers inherit parent permissions. Antigravity reviewer
registrations declare read tools only and disable shell and MCP inheritance. Verify these
permissions in the running client before relying on a reviewer.

The pinned Context7 package avoids floating updates but is neither an npm integrity lock nor privacy
sandbox. First use executes downloaded code; service inputs leave the local boundary. Never send
keys, credentials, dumps, unredacted captures or CI secrets; disable locally if policy forbids it.
Firmware builds and CI do not depend on Context7.

## Configuration-only physical maintenance

The [default-off maintenance role](CONFIG_RECOVERY.md) can write only the recognized corrupt
`tesla_cfg/cfg` blob. It requires independent full board/key/VIN authority, absent identity
journals and non-cfg logical retention. An uncertain commit parks without retry. Admit the
exact signed maintenance artifact separately from the ordinary app and channel pipeline.
[Qualification and physical limits](CONFIG_RECOVERY.md#qualification-and-physical-use-boundaries)
