# Security & Hardening

Owns: threat model, API exposure, key material, OTA signing and the CI/release trust boundaries.
Runtime OTA/rollback mechanics live in [`ARCHITECTURE.md`](ARCHITECTURE.md#ota-self-update); the
feature catalog with per-mechanism evidence is [`FEATURES.md`](FEATURES.md).

## Threat model

The crown jewel is the **ECDSA P-256 private key** in NVS — it *is* a valid Tesla BLE key. The
firmware enrolls it as **Charging Manager only** (charging + wake), so a leaked key cannot unlock
or drive the car, but it can still control charging. WiFi password and VIN are in NVS too.

| Attacker | Exposure and mitigation |
|---|---|
| Physical USB/serial | Can dump flash and, without Secure Boot, replace firmware (see [hardening](#enabling-flash-encryption--secure-boot-recommended-irreversible)). |
| LAN peer | HTTP API is plaintext on port 80 without auth (see [exposure](#http-api-exposure)). |
| BLE/RF range | Pairing and commands; mitigated by Tesla's session crypto. |
| Supply chain | OTA images are RSA-3072 signed and verified on every update, so integrity does not rest on TLS alone. |

**First-boot key entropy.** The P-256 key is generated under `bootloader_random_enable()` (SAR-ADC
hardware entropy) *before* WiFi/BLE start. On ESP-IDF 6 the PSA RNG has no DRBG of its own
(`MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG`), so every draw reads the hardware RNG. Devices first-keyed
before that fix should re-key and re-pair (`/gen_keys?force=1`).

**Fail-closed key rotation.** `tk::regenerate_private_key()` (`main/logic/key_rotation.hpp`)
reports generation and NVS persistence failures and restores the previous in-memory key when the
replacement cannot be committed. Signing, pairing, polling and background commands stay blocked
while the runtime key identity is ambiguous; they resume only after the durable key is verified
and old sessions are cleared. VIN changes use a persistent transition journal
(`tesla_cfg/vin_txn`), so an interrupted cross-namespace update is completed or rolled back on the
next boot.

**Exact NVS surface.** `main/logic/nvs_contract.hpp` declares all 19 records (namespace,
logical/stored name, storage API, owner, retention, secrecy). `NvsStorageAdapter` rejects unknown
namespaces, wrong APIs, name collisions and stored keys over 15 bytes instead of truncating. The
operator-facing retention table is in [`README.md`](README.md#upgrading).

**BLE anti-replay and framing.** Native orchestration (`logic/rx_framing.hpp`,
`ble_dispatcher.hpp`, `command_runner.hpp`) uses strict 2-byte big-endian length framing, routes
every CarServer response by request UUID, and drops replayed counters and foreign UUIDs
fail-closed. A charging-current write additionally needs a fresh exact `ChargeState` readback; an
ACK alone is not success. Known-answer protocol vectors are in
`test/tesla_protocol_vectors.test.mjs` (public test keys only) and
`test/test_tesla_ble_harness.cpp`; design rationale is in
[ADR-0005](adr/0005-tesla-ble-seam.md).

## Unencrypted-flash reality (factory devices)

A factory ESP32 ships with Flash Encryption and Secure Boot **off**, ROM download mode open and
JTAG enabled. Anyone with USB access can therefore read the whole flash — including the key — in
plaintext. Check a unit yourself (read-only):

```bash
pip install esptool
espefuse --port /dev/cu.usbmodemXXXX summary     # SPI_BOOT_CRYPT_CNT, SECURE_BOOT_EN, DIS_*
# the nvs partition (0x9000, size 0x6000) holds the key while unencrypted:
esptool --port /dev/cu.usbmodemXXXX read_flash 0x9000 0x6000 nvs_dump.bin
```

Treat any such dump as secret material; never attach it to an issue.

## HTTP API exposure

The HTTP API has **no authentication and no TLS** — by design. Its primary consumer, evcc, speaks
plain HTTP and cannot send credentials. Anyone on the LAN can call **every** endpoint: wake,
charging, key regeneration (`/gen_keys`), pairing (`/send_key`), BLE scan, VIN change (`/set_vin`,
un-pairs + reboots), MQTT/Syslog/WiFi/OTA-channel configuration (reboots), crash-report deletion,
OTA update/reboot trigger (`/ota/update`) and `/mcp` (the same charging command set for AI agents).
This is acceptable only because the enrolled key is Charging Manager only and the device lives on
a **trusted home LAN**, never exposed to the internet. For access control, use a TLS/auth reverse
proxy or a VLAN.

Reverse-proxy rules: send a device-owned upstream `Host` (device IP or `.local` name) and either
remove `Origin` or rewrite it to that authority. A proxy that forwards its public hostname is
rejected by the browser gate below. Likewise, open the configuration UI via
`http://tesla-key-esp32.local` or the current IP; a router-expanded DHCP FQDN may work for
read-only clients but gets `403` on browser mutations.

Hardening that remains (none of it is authentication):

- **Browser-origin gate.** A mutating request whose `Origin` authority differs from `Host`, whose
  `Host` is neither the device name nor an IPv4 address the board holds right now (the active transport's lease or the address the request itself arrived on — WiFi and Ethernet keep their leases side by side, so a page opened through the WiFi address keeps working after an Ethernet takeover), or whose `Sec-Fetch-Site` is `cross-site`,
  gets `403` before dispatch. Binding `Host` to a device-owned authority also closes DNS
  rebinding. It covers every POST plus the legacy state-changing GET forms `/ota/check`,
  `/diag?clear=1`, `/diag?verbose=0|1` and `/coredump?clear=1`; query **keys match
  case-insensitively** (as `esp_http_server` does) while **values stay exact and undecoded**.
  Headerless POST clients (evcc, curl) stay compatible, and so does a raw LAN peer that sends POST
  or adds a custom header (e.g. `X-Requested-With`) on mutating GETs — headerless mutating GET
  requests receive `403` to prevent browser CSRF; the trusted-LAN boundary remains mandatory.
- **Identity guards.** `/gen_keys` and `/set_vin` answer `503` unless the running image is
  `Stable` and no OTA/identity work owns the gate. Once stable, `/gen_keys` still refuses to
  replace an existing key without `force=1` (`409`).
- **Active-update guard (`409`)** on the mutating config routes during a firmware flash — details in
  [ARCHITECTURE](ARCHITECTURE.md#http-request-body-and-allocator-failure-contract).
- **Bounded, typed intake.** 2 KiB body cap, allocation-free JSON syntax gate (16 levels, strict
  UTF-8, no U+0000), sticky response builders and exact request-ID rules; no rejected request
  reaches a command, NVS save, probe or restart. Contract and status mapping:
  [ARCHITECTURE](ARCHITECTURE.md#http-request-body-and-allocator-failure-contract).

`POST /set_wifi` deserves a name because it is the one open route whose worst case is *losing the
device* rather than mis-charging: a LAN peer can point it at a network you do not control. Two
things bound that. It grants nothing an attacker already on the LAN lacks (they could equally
re-flash via the open API), and a change that does not work **undoes itself** — the previous
credentials are a one-shot backup in the same atomic config entry, restored on the next boot
unless the new network hands out a lease (`logic/wifi_rollback.hpp`). It does *not* protect
against a network the attacker genuinely controls, since that association succeeds.

## Syslog and diagnostic export

The optional Syslog forwarder (`main/syslog.cpp`, `POST /set_syslog` or NVS `syslog_uri`) sends
**unredacted** logs over **cleartext UDP (RFC 5424)** without authentication. The `?redact=1` rules
of `/diag` do not apply. Logs carry the VIN on every REST command; VIN, vehicle BLE MAC and board
MAC at boot; the WiFi SSID on connect. Raw private keys and WiFi passwords are never deliberately
logged. Forward only to trusted collectors on an isolated VLAN; leave Syslog off on shared networks.

**Core dumps** go to the `coredump` partition (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`, ELF format)
and are served unauthenticated by `GET /coredump`. `CONFIG_ESP_COREDUMP_CAPTURE_DRAM` stays off so
the general heap is not dumped, but task stacks live at crash time can hold transient secrets
(key-exchange values, session state, decrypted buffers). Never attach a dump to a public issue.

## OTA self-update

The device pulls `manifest.json` and its per-target app image from **fixed compile-time HTTPS
URLs** (`CONFIG_TESLA_OTA_MANIFEST_URL`, `CONFIG_TESLA_OTA_FIRMWARE_BASE_URL`; default GitHub
Pages), compares versions and flashes the inactive slot via `esp_https_ota`, which also refuses a
wrong-target image. Runtime mechanics: [ARCHITECTURE](ARCHITECTURE.md#ota-self-update).

- **Transport and image are both verified.** TLS certificates are checked against the bundled CA
  roots, *and* the image carries an RSA-3072 application signature the running firmware verifies
  (`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`) before accepting it. Whoever controls the
  update host cannot serve arbitrary firmware.
- **The trigger is unauthenticated.** `POST /ota/update` and `GET /ota/check` are open like the
  rest of the API. Because the URL is compile-time fixed *and* the image must be signed, a LAN peer
  cannot point the device at attacker firmware, but it can force a fetch + reboot (a nuisance; each
  reboot re-opens the BLE polling window so a parked car stops sleeping).
- **Downgrade is blocked in software.** A signature proves authenticity, not freshness. Before
  flashing, `ota_task` reads the version from the downloaded image's own app descriptor and refuses
  anything not strictly newer — which also defeats a host that advertises a new manifest version
  but serves an old binary. No eFuse anti-rollback is burned.
- **Rollback is armed** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) behind a health gate: proven
  network link, non-critical INTERNAL largest heap block and ≈ 90 s uptime, with VIN/key/recovery
  reboots never counted as health evidence. An image that never reaches the LAN is reverted on the
  next reboot rather than committed. A fatal essential-component failure on a still-unverified
  image marks it invalid and reboots into the previous slot; on an already-valid image it halts
  instead of looping.

**USB Web Serial installer.** `manifest.json` (`layoutVersion:2`) binds the site to one 40-hex
`sourceSha`, exactly four chip families and, per family, bootloader/partition/app/otadata in fixed
roles and offsets with byte length and SHA-256. The browser downloads and verifies all four before
erasing or writing, writes the first three, and writes `ota_data_initial@0xf000` **last** as the
activation step. That detects partial/mixed Pages deployments and corrupt downloads; it does not
make a compromised Pages origin a trust anchor — a USB install trusts the site the operator chose,
while OTA authenticity remains protected independently by RSA verification. The page runs no CDN
code: the official `esptool-js@0.6.1` ESM bundle and license live in `docs/vendor/`,
`scripts/verify-vendored-esptool-js.sh` pins the tarball SRI and both extracted hashes, and Pages
serves it under `script-src 'self'`.

## Signed OTA images

The firmware uses the **Secure Boot v2 signature scheme without hardware Secure Boot**
(`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`, `..._RSA_SCHEME`,
`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`). No eFuses are burned, so this is reversible, cannot
brick a device and keeps the web installer working (with the RSA scheme only the OTA path verifies).
`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`: the build emits an *unsigned* binary and CI signs it
in a separate step, so the key is never present at compile time.

> ⚠️ **A locally built binary is unsigned and will _not_ boot.** The running app calls
> `esp_secure_boot_init_checks()` at startup, which `abort()`s in a **reboot loop** whenever the app
> has no signature block — before `app_main`, on every target. A plain `idf.py build` image cannot
> be USB-flashed as-is. Use the signed CI artifact, or sign the local image with the offline key
> (`espsecure.py sign_data --version 2 --keyfile <key> --output app.bin.signed app.bin`, the same
> step CI runs). See the `flash-esp32` skill for the dev-flash workflow.

### Trust anchor (trust-on-first-use)

With no eFuse digest, the trusted public key is taken from the **signature block of the currently
running app**. Consequences:

- The first signed image is accepted by a device still on old *unsigned* firmware (built before the
  signing config existed) or is USB-flashed; from then on the device only accepts OTA images signed
  with the **same key**. This is a one-way transition: a signed device refuses unsigned or
  differently-signed OTA, and a downgrade to unsigned firmware needs a USB reflash. A bad signed
  image still auto-rolls back.
- **Classic ESP32 needs chip revision v3.0+ (ECO3)** for the V2 RSA scheme
  (`CONFIG_ESP32_REV_MIN_3` in `sdkconfig.defaults.esp32`). On older silicon an OTA is rejected
  cleanly ("downloaded image is invalid") and the device keeps running, but it can no longer OTA
  forward — only a USB reflash helps. This keeps one signing scheme and key across all four
  targets; `esp32s3`/`c3`/`c6` need no such override.
- **Production authority pin.** `scripts/ota-signing-public-key.sha256` pins the Secure Boot v2
  public-key-block digest observed in all four apps of the immutable Release
  [`v1.4.84`](https://github.com/0Bu/tesla-key-esp32/releases/tag/v1.4.84). After signing, and
  again at draft publication and immutable-Release reuse, every app must pass a full
  RSA-PSS/SHA-256 verification against it. It is not a second device trust mechanism; it stops a
  changed CI secret or substituted signed asset from silently replacing the production authority.
  Updating the pin is a reviewed security migration, never routine rotation.
- The flash, ship and USB-recovery procedures repeat the same app-only parse, RSA-PSS check and pin
  check on every downloaded app before a physical write. Size, version, chip metadata or byte
  identity with another artifact never substitute for signer authentication.

### Create the signing key

Generate a **dedicated** key **offline** on a trusted machine. Do not reuse your git-commit GPG key
(wrong format, conflates trust domains) and do not generate it in CI.

```bash
pip install esptool                                   # provides espsecure.py
espsecure.py generate_signing_key --version 2 --scheme rsa3072 ota_signing_key.pem
# equivalent plain OpenSSL (key is a standard RSA-3072 pair; nothing ESP-specific lives in it):
openssl genrsa -out ota_signing_key.pem 3072
```

Requirements for Secure Boot v2: **exactly 3072 bits**, **public exponent 65537**, **unencrypted**
PEM (CI loads it non-interactively; PKCS#1 and PKCS#8 both work). A v1/ECDSA/EC key, an encrypted
PEM or another RSA size fails in `sign_data` with `unsupported key type`. Verify locally exactly
as CI does:

```bash
openssl rsa -in ota_signing_key.pem -noout -text | head -1     # "Private-Key: (3072 bit, 2 primes)"
head -c 4096 /dev/zero > /tmp/dummy.bin
espsecure.py sign_data --version 2 --keyfile ota_signing_key.pem --output /tmp/dummy.signed /tmp/dummy.bin
```

**Protect it.** Losing it means no more OTA updates (USB reflash for every device); leaking it makes
signed OTA worthless. Keep it offline (password manager, hardware token or air-gapped) with ≥ 2
backups. The repository gitignores `*.pem`, but never commit it. The same key can later double as
the hardware Secure Boot v2 key, so enabling full Secure Boot needs no key migration.

### Release pipeline and trust boundaries

Compilation and signing are separate trust domains. The counts below are the ones the
`check-*` scripts enforce: **53** allowlisted payload files per build inventory, **12** root
release files (4 unversioned apps + 4 versioned apps + 4 versioned merged images), **28**
diagnostics (7 per target), **40** Release assets (12 + 28) and **16** manifest parts (4 per
target: bootloader, partition table, signed app, `ota_data_initial`).

1. **Unprivileged `build`.** Runs PR source and the compiler with neither the signing key nor a
   write token; for a PR it checks out the exact `pull_request.head.sha`. It uploads only unsigned
   app/flash inputs plus ELF, map, sdkconfig, lock and size/provenance data. Its inventory is a
   **claim, not a trusted attestation**: 53 payload files plus a manifest, bound to the commit and
   a content/mode fingerprint of all tracked and non-ignored source. Inside the pinned container it
   rebuilds **all four targets** in fresh directories and compares unsigned app and ELF
   byte-for-byte, and it enforces the projected-signed OTA-slot limit (64 KiB alignment + one 4 KiB
   signature sector), the size/stack baselines and the effective-build closure described in
   [FEATURES](FEATURES.md#6-build-test-and-ci). ccache is disabled, and caller compiler/include/
   `CCACHE_*` environment is rejected. A disposable-key run of the real signer and four-target
   manifest path means a PR cannot silently break reproducibility or release assembly.
2. **Protected `publish` on `main`.** Enters the `firmware-signing` Environment (secret
   `OTA_SIGNING_KEY`, an unencrypted RSA-3072 PEM; required reviewers must be configured in the
   repository settings). Separate, cache-free runners (one per target, concurrent with the producer builds) independently
   rebuild the exact commit and a secret-free join job binds them into one evidence artifact, and
   the job compares both inventories (53 payloads + manifest) byte-for-byte **before** it
   provisions the key. `scripts/ci-sign-artifacts.sh` repeats the comparison, opens files with
   `O_NOFOLLOW`, copies single-link regular files into a private stage and rehashes them before
   reading the key; it signs, runs `espsecure.py verify_signature`, requires the exact minimal size
   projection, re-validates image identity and verifies RSA-PSS against the authority pin. Two
   closed modes:
   - **`create`** proves the tag/Release are still absent immediately before key use, uploads exactly
     40 files into a **draft**, and binds every byte, alias, ELF checksum and full merged layout
     (signer-owned bootloader/partition/erased otadata/signed app, all erased gaps including NVS,
     exact EOF). The draft API read uses the upload action's numeric Release ID (drafts are not
     reachable by tag), so a protected retry safely rebinds the existing draft. The candidate is
     re-checked in the same step immediately before `PATCH draft=false`; only a fresh
     `immutable: true` response becomes authority.
   - **`reuse`** (same-SHA retry) accepts only that exact current immutable 40-asset Release. It
     reads each download once through a directory-relative `O_NOFOLLOW` descriptor, binds API
     size/digest to the resulting byte snapshot and uses only that snapshot for every later check
     (a path-swap canary proves later replacement cannot alter staged bytes). It compares the 28
     diagnostics and four signed/merged images with the independent build, **never signs,
     re-uploads or mutates** the Release, and uploads one new SHA-bound Actions **recovery
     artifact** with the twelve verified files so USB recovery stays available.

   In both modes the local root site is assembled and byte-bound (16/16 manifest parts against the
   four merged images) *before* any artifact upload or Release mutation, and the root must contain
   exactly the twelve regular, single-link, non-empty files, listed by name and never by glob. The
   artifact also carries the sixteen signer-owned per-target layout inputs. `publish` never writes
   `gh-pages`. Every display version uses one canonical grammar (no leading-zero core component,
   ≤ 31 bytes for the ESP app descriptor) at every boundary.
3. **`deploy` (main push only).** No signing Environment, key or OIDC. Checks out the exact SHA
   without persisted credentials, downloads only the named artifact, re-verifies the twelve-file
   root, sixteen layout inputs, site manifest and 16 local byte relationships, then binds them plus
   the 28 diagnostics to fresh metadata for all 40 immutable Release assets. It revalidates Release
   and Pages authority immediately before writing the branch, then reads the live manifest and all
   16 parts back (bounded, cache-busted) and compares them byte-for-byte to the immutable merged
   assets.
4. **Signed PR preview (opt-in).** A maintainer adds the `signed-preview` label to a same-repository
   PR. After its unprivileged build, `signed-pr-preview.yml` runs from the default branch via
   `workflow_run`, verifies the head is current and launches a separate default-branch-defined
   rebuild of that exact SHA. That rebuild may execute PR code but has read-only permissions and
   no secret, Environment, identity token, restored cache or access to the primary artifact. The
   protected signer **never checks out the PR** (that would create an untrusted-checkout TOCTOU in
   a key-capable job); it treats both artifacts only as bounded data, requires their 53 payloads
   and manifests to be byte-identical and source/version-bound, and derives the
   `<latest-complete-immutable-stable>-PR-<N>` version itself after the approval wait. Before key
   provisioning, artifact upload and Pages publication it refetches the default-branch head and
   requires it to equal the trusted workflow's `github.sha`, so a main advance retires a stale
   queued run. Every signed app must pass the authority-pin RSA-PSS check. Fork PRs are ineligible;
   unlabelled PRs stay unsigned compile checks. Signing and cleanup share a per-PR concurrency
   group: close, force-push or label removal deletes the preview and cancels a running publisher
   (cleanup is a trusted-base `pull_request_target` job that checks out the exact base SHA). A daily
   reconciliation removes any preview whose PR is not open, same-repository, labelled and at the
   manifest's `sourceSha`. The `trusted-rebuild` job reads `esp-idf-toolchain.txt` from the PR head
   so a toolchain-bump PR builds with its own digest.
5. **One Pages authority.** GitHub's branch-backed legacy mode, source `gh-pages:/`, holds root
   (**Release** channel), `dev/` (**Dev** channel) and `PR/<N>/` previews. No Pages Actions artifact
   is used. `scripts/check-pages-source.py` validates the API mode/source and HTTPS URL before
   signing, again right before each branch write or deletion, and when deriving the live URL for
   acceptance, so a repository switched to Actions mode or another branch fails before key use.
6. **Dev feed and manual releases.** A push to `main` builds a **Dev** build (`mode=dev`), signed by
   the protected key and deployed to `/dev/`; it creates no GitHub Release, and re-running a `main`
   push signs fresh bytes (RSA-PSS is randomized). Official Releases are cut manually with
   `workflow_dispatch` `release: true` (optional `bump: patch|minor|major` or
   `release_version: x.y.z`), creating the immutable tagged Release and deploying to root Pages. A
   `workflow_dispatch` without `release: true` runs in `test` mode and never reaches protected
   signing or publishing.

These gates prove a closed, deterministic source-to-artifact relationship under the pinned build
contract. They do not prove that reviewed source is safe, that GitHub-hosted runners are
trustworthy or that a signed image works on hardware; code review, Environment approval and bench
acceptance remain separate evidence boundaries. The signer runs in the digest-pinned image from
`esp-idf-toolchain.txt`, so rotating that digest is a security-sensitive review. For higher
assurance, keep the key fully offline and sign on a trusted machine or KMS instead of in CI.

### Key rotation

`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` supports exactly one valid signature block in
position zero; ESP-IDF (v5.5, unchanged in v6.1) verifies the next OTA image only against the first
running-app key. Appending old and new signatures therefore **is not an OTA rotation path**, and the
validator rejects extra blocks. Changing `OTA_SIGNING_KEY` or the pin alone would strand every
device anchored to the old key. A rotation is a separately reviewed fleet migration: keep the old
key and recovery artifacts, prepare key and pin as one verified change, USB-flash a new-key-signed
app on each authorized device **preserving NVS**, verify each device on the new authority, and only
then retire the old key. Rollback across authorities is USB-only. A multi-key design would need a
migration to hardware Secure Boot or a reviewed ESP-IDF change.

## Enabling Flash Encryption + Secure Boot (recommended, IRREVERSIBLE)

This is the real fix for key-at-rest and firmware tampering. **Burning these eFuses is permanent
and can lock you out** — do it deliberately, per unit, after testing. Read the Espressif guides for
[flash encryption](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/security/flash-encryption.html)
and [Secure Boot v2](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/security/secure-boot-v2.html)
first.

1. **Signing key.** Reuse the OTA signing key (RSA-3072, v2): hardware Secure Boot just also burns
   its public-key digest into eFuse, so no re-signing of the release stream is needed.
2. **menuconfig** → *Security features*: enable flash encryption (**Release** mode; Development mode
   is not secure), hardware Secure Boot v2 with `ota_signing_key.pem`, and NVS encryption.
3. **`nvs_keys` partition** (required for NVS encryption) in `partitions.csv`, leaving `nvs` at
   `0x9000` so existing data stays in place:
   `nvs_key, data, nvs_keys, , 0x1000, encrypted,`
4. **First encrypted flash:** `idf.py build && idf.py flash` (the device encrypts flash and burns
   eFuses on first boot), then `espefuse --port <PORT> summary` to confirm `SPI_BOOT_CRYPT_CNT` /
   `SECURE_BOOT_EN`.
5. **Optional lockdown:** `espefuse --port <PORT> burn_efuse DIS_DOWNLOAD_MODE` (block read-back
   over the ROM downloader) or `ENABLE_SECURITY_DOWNLOAD` (secure variant only).

⚠️ **Consequence for the web installer:** with flash encryption in Release mode the device accepts
only signed, effectively encrypted images; the browser installer writes plaintext parts and can no
longer update it. Deliver updates via signed OTA or `idf.py flash` from a trusted machine, and plan
that path before burning anything.

## Development tooling trust boundary

[`AGENTS.md`](../AGENTS.md) is the canonical authorization policy: analysis, review and diagnosis
are read-only by default, and an implementation request does not authorize commit, push, merge,
release, USB, flash, OTA, NVS or live-vehicle operations. Specialist reviewers in
`.agents/subagents.json` run with `SandboxMode = "read-only"`, no model pin and no approval
escalation. The runner-neutral hook core under [`tools/agent-hooks/`](../tools/agent-hooks/) checks
secrets, partitions and PR gates lexically — defense in depth, not a substitute for sandboxing,
explicit authorization, branch protection, CI trust separation or human review. No PR or agent gate
receives `OTA_SIGNING_KEY`; through agent tools the only permitted key use is an explicitly
authorized, unchained `espsecure.py sign_data` call that passes the key as a keyfile path. Never
print, copy, redirect, archive, upload or pass private-key, NVS, BLE-session, credential or
environment-dump material through an agent tool.

CI actions are pinned by full commit SHA, firmware tooling runs in the tag-plus-digest image from
`esp-idf-toolchain.txt`, and every workflow has least-privilege permissions and a job timeout. The
GitHub-hosted runner OS, orchestration and GitHub itself remain part of the trust boundary. The
read-only `pull_request_target` PR-policy workflow checks out the trusted base SHA, evaluates
server-side current-head records and never executes PR code; it blocks merges only once repository
rules require `pr-policy / current-head-records`, a setting the repository cannot self-install. The
manual bench-acceptance workflow ingests one closed-schema report and records its digests; those
cover the report, not the firmware, and all physical observations remain operator declarations
([FEATURES §6](FEATURES.md#6-build-test-and-ci)).

The project MCP config pins `@upstash/context7-mcp@4.0.2` instead of a floating tag. That avoids
unnoticed upgrades but is not a privacy sandbox or npm integrity lock: first use downloads
executable code, and anything sent to the Context7 service leaves the local boundary. Never send
signing keys, credentials, NVS dumps, unredacted VIN/BLE captures or CI secrets through it; disable
it locally if policy forbids it. Neither firmware builds nor CI depend on it.

## Other notes

- **The setup AP is open** (`WIFI_AUTH_OPEN`) and the WiFi password is submitted over plain HTTP
  during provisioning. Keep the setup window short.
- **Do not expose the device to the internet.** Home LAN only.
- The private key is never logged; VIN/MAC/SSID appear in serial logs (physical access).
- mDNS advertises the firmware version but not the VIN, so enumerating `_http._tcp` never multicasts
  a vehicle identifier.
