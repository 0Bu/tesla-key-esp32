# Security & Hardening

## Threat model

The crown jewel is the **ECDSA P-256 private key** in NVS — it *is* a valid Tesla BLE
key. This firmware enrolls it as **Charging Manager only** (charging + wake), so a
compromised key cannot unlock/drive the car, but it can still control charging and the
WiFi password + VIN are also in NVS.

Relevant attackers:

- **Physical USB/serial access** — can dump flash and (without Secure Boot) replace firmware.
- **LAN peer** — the HTTP API is plaintext on port 80 with no TLS.
- **BLE/RF range** — pairing/commands; mitigated by the Tesla session crypto.
- **Supply chain** — OTA images are **signed** (RSA-3072, Secure Boot v2 scheme) and the
  signature is verified on every update, so integrity no longer rests on TLS alone (see
  [OTA self-update](#ota-self-update) and [Signed OTA](#signed-ota-images)).

**First-boot key entropy:** the P-256 key is generated under `bootloader_random_enable()`
(SAR-ADC hardware entropy) *before* WiFi/BLE start, so it draws from a true entropy source
rather than the RF-off pseudo-random RNG. Devices first-keyed before the entropy fix should
re-key + re-pair (`/gen_keys?force=1`, then re-enrol).

**Fail-closed key rotation:** the tesla-ble patch series reports key-generation and NVS
persistence failures and restores the previous in-memory key when the replacement cannot be
committed. The controller blocks signing, pairing, status polling, and background commands while
the runtime key identity is ambiguous; it only enables them after the durable key is verified and
the previous sessions are cleared. VIN changes use a persistent transition journal so interrupted
cross-namespace updates are completed or rolled back on the next boot.

**Exact NVS surface:** `main/logic/nvs_contract.hpp` declares all 19 records with namespace,
logical/stored name, storage API, owner, retention and secrecy. `NvsStorageAdapter` resolves every
access through it and rejects unknown namespaces, wrong APIs, name collisions and >15-byte stored
keys; it no longer truncates an unknown key. This includes the pinned tesla-ble mappings
`session_vcsec`→`sess_vcsec` and `session_infotainment`→`sess_info`. The host gate inventories direct
NVS calls in every shipped source/header/inline fragment and the operator-facing retention mirror is
in `docs/README.md`.

**BLE response anti-replay:** the pinned `yoziru/tesla-ble` v5.1.1 detects an invalid
CarServer response counter but, upstream, still dispatches that response to telemetry callbacks
and the command FIFO. The repository applies `patches/tesla-ble/` to every target at build time
so a rejected counter is logged and dropped before it can update state or complete a newer
command. Charging-current writes additionally require a fresh exact `ChargeState` readback;
an action acknowledgement alone is not reported as success.
All six RX framing/recovery callsites also use the third patch's rate-limited helper: warning and
error clocks are independent, the shared suppression count saturates rather than wrapping, and
only the severe-corruption path explicitly selects error severity.
`test/tesla_protocol_vectors.test.mjs` independently pins the public VIN-advertisement vector,
P-256 ECDH byte order, `SHA1(shared-secret)[:16]`, the `session info` HMAC label, AES-GCM
metadata/AAD/nonce/tag layout and all three local patch invariants. It uses public test keys only and
never reads device or vehicle identity material.

## Current device state (factory ESP32-S3)

`espefuse summary` on the connected unit (read-only check, 2026-06-16):

| eFuse | Value | Meaning |
|---|---|---|
| `SPI_BOOT_CRYPT_CNT` | `Disable` | **Flash Encryption OFF** |
| `SECURE_BOOT_EN` | `False` | **Secure Boot OFF** |
| `DIS_DOWNLOAD_MODE` | `False` | UART/USB download open |
| `ENABLE_SECURITY_DOWNLOAD` | `False` | no secure download |
| `DIS_USB_JTAG` / `DIS_PAD_JTAG` | `False` | JTAG enabled |

⇒ Anyone with USB access can read the whole flash in plaintext, including the private
key. Verify yourself (read-only, safe):

```bash
pip install esptool
espefuse --port /dev/cu.usbmodemXXXX summary
# The nvs partition (offset 0x9000, size 0x6000) — contains the key while unencrypted:
esptool --port /dev/cu.usbmodemXXXX read_flash 0x9000 0x6000 nvs_dump.bin
```

## HTTP API exposure

The HTTP API has **no authentication and no TLS** — by design. The primary consumer is
evcc, which talks to the device over plain HTTP and cannot send credentials, so locking
the API would break the main use case. Anyone on the LAN can therefore call **every**
endpoint — including ones that go beyond charging: wake, charging control, key
regeneration (`/gen_keys`) and pairing (`/send_key`), BLE scan (`/scan`), VIN change
(`/set_vin`, un-pairs + reboots), MQTT broker change (`/set_mqtt`, reboots), **WiFi
credential change (`/set_wifi`, reboots onto another network)**, crash-report deletion
(`/crash/dismiss`), the OTA self-update / reboot trigger (`/ota/update`, see below) and the
MCP endpoint (`/mcp`, which exposes the same charging command set to AI agents — nothing
beyond what the open REST routes already allow). This is acceptable only because:

- the enrolled key is **Charging Manager only** — it cannot unlock or drive the car, just
  control charging and wake (see the role restriction in `vehicle_pairing.cpp`); and
- the device is meant to live on a **trusted home LAN**, never exposed to the internet.

If you need access control, put the device behind a reverse proxy with TLS + auth, or
segment it onto a trusted VLAN. A proxy must send a device-owned upstream `Host` (the current
device IP or its `.local` name) and either remove `Origin` or rewrite its authority to that same
device authority. Forwarding the proxy's public hostname unchanged is intentionally rejected by
the browser-mutation gate described below.

The firmware does reject a narrower browser threat: a mutating request carrying an `Origin`
whose authority differs from `Host`, whose `Host` is neither the device name nor its current IP,
or whose `Sec-Fetch-Site` is `cross-site`, receives `403` before route dispatch. Binding `Host` to
a device-owned authority also closes the usual DNS-rebinding bypass where attacker-controlled
`Host` and `Origin` match. The gate covers every POST plus the legacy state-changing GET forms
`/ota/check`, `/diag?clear=1`, `/diag?verbose=0|1` and `/coredump?clear=1`. Same-origin UI requests
continue to work, and headerless clients such as evcc and curl remain compatible. If either
`Origin` or `Sec-Fetch-Site` is present, the device-owned `Host` check applies; this covers
same-origin browser GETs that legitimately omit `Origin`. This is **not authentication**: a raw
LAN peer can omit both browser headers and still call every endpoint, and an old/non-conforming
browser that sends neither header is indistinguishable from such a client. The trusted-LAN
boundary therefore remains mandatory.

Open the configuration UI through `http://tesla-key-esp32.local` or the current device IP when
you need to mutate settings. A router-expanded DHCP name such as
`tesla-key-esp32.router.example` may resolve for read-only/headerless clients, but is deliberately
not a device-owned browser authority and receives `403` on mutations.

`POST /set_wifi` deserves naming explicitly, because it is the one open route whose worst
case is *losing the device* rather than mis-charging the car: a LAN peer can point it at a
network you do not control, and the board reboots onto it. Two things bound that. The
credentials are only reachable from the LAN the device is already on, so this grants no
capability an attacker with that foothold lacks (they could equally re-flash it over the
same open API). And a change that does not work **undoes itself**: the previous SSID and
password are stashed as a one-shot backup inside the same atomic config entry, and the boot
that follows restores them unless the new network actually hands out a lease
(`logic/wifi_rollback.hpp`). What that does *not* protect against is a change to a network
the attacker genuinely controls — that association succeeds, so nothing rolls back. The
mitigation there is the same as for every other route on this list: a trusted LAN.

Three non-auth hardening measures remain in place:

- **Browser-origin gate** — rejects cross-site/DNS-rebound browser mutations (including the
  state-changing legacy GET forms) while retaining headerless evcc/curl compatibility; it does
  not restrict a raw LAN caller.
- **`/gen_keys` identity and overwrite guards** — refuses every key mutation while the running
  image is PendingVerify, its verification state is unknown, or an OTA/update owns the identity
  gate (returns `503` without changing a key). Once Stable, it still refuses to regenerate an
  existing key without `force=1` (returns `409`); regeneration un-pairs the vehicle.
- **Body size cap** — POST bodies over 2 KB are rejected (bounds the receive buffer).
- **Typed body/JSON failures** — empty, oversized, allocation-failed and receive-failed requests
  remain distinct. Syntactically valid JSON whose cJSON materialization fails is treated as OOM;
  malformed input stays a client error. The allocation-free syntax gate admits at most 16 nested
  arrays/objects and validates every raw UTF-8 string byte as a shortest-form Unicode scalar;
  invalid leads/continuations, truncation, overlong forms, UTF-16-surrogate encodings and values
  above U+10FFFF never reach cJSON. Deeper but otherwise valid JSON is a separate client-limit
  failure. The gate likewise rejects escaped U+0000 before materialization because cJSON exposes
  strings through a NUL-terminated API; otherwise an ID such as `a\u0000b` would be echoed as `a`.
  No rejected body reaches a command, NVS save, probe or
  restart, and body/parse owners are released before blocking work or input-independent large
  response construction. Response/MQTT builders use sticky ownership, so one failed cJSON
  Create/Add/print publishes no partial JSON. REST/MCP set 503 before their single fixed fallback
  send. Before notification detection or dispatch, MCP requires `jsonrpc` as the exact string
  `"2.0"` and rejects duplicate object keys recursively, including escaped-equivalent names.
  Numeric request IDs are accepted only when their original token is a canonical decimal integer
  in `[-9007199254740991, 9007199254740991]`; the raw token and cJSON value must agree, so exponent
  underflow, fractional values rounded by `double`, negative zero and values at `±2^53` cannot be
  mis-correlated. Accepted numbers use an internally generated exact decimal emitter. String IDs
  are copied into at most 64 bytes of fixed storage before the input tree is released; explicit
  null, booleans, arrays/objects, embedded NUL and oversized strings are rejected. Every allocation stage,
  including large-response print growth, is exercised against the exact pinned cJSON source with
  sanitizer runs.

## OTA self-update

The device can update itself **pull-based**: it fetches `manifest.json` and its per-target
app image from **fixed, compile-time HTTPS URLs** (`CONFIG_TESLA_OTA_MANIFEST_URL` and
`CONFIG_TESLA_OTA_FIRMWARE_BASE_URL` + `tesla-key-esp32<suffix>.bin`, where `<suffix>` is the
chip's short tag — `""`/`-s3`/`-c3`/`-c6` — so "esp32" appears once, default GitHub
Pages), compares the manifest `version` to the running firmware, and on confirmation flashes
the inactive OTA slot via `esp_https_ota`, then reboots. `esp_https_ota` verifies the image
chip-id, so a wrong-target image is refused. Implemented in `main/ota_update.cpp`.

Trust model:

- **Transport AND image are verified.** TLS server certificates are checked against the
  bundled CA roots (`esp_crt_bundle_attach`), so the connection to the configured host is
  authenticated — and, in addition, the downloaded image carries an **RSA-3072 application
  signature** that the running firmware verifies before accepting the update
  (`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`, see [Signed OTA](#signed-ota-images)).
  Whoever controls the update host can therefore no longer serve arbitrary firmware: an
  unsigned or wrongly-signed image is rejected at `esp_https_ota_finish()`.
- **The trigger is unauthenticated.** `POST /ota/update` (and `GET /ota/check`) are open on
  the LAN like the rest of the API. Because the download URL is **compile-time fixed** *and*
  the image must be signed, a LAN peer cannot point the device at attacker-controlled
  firmware — but it *can* force a fetch + reboot (a nuisance/DoS, and each reboot re-opens
  the BLE polling window so a parked car stops sleeping). Restricting who can reach
  `/ota/update` needs the same reverse-proxy / VLAN segmentation as the rest of the API.
- **Downgrade is blocked in software.** A signature proves authenticity, not freshness, so a
  hostile (or compromised) update host could otherwise serve an *old, legitimately-signed*
  image that re-introduces a patched vulnerability. Before flashing, `ota_task` reads the
  version from the downloaded image's app descriptor (`esp_https_ota_get_img_desc`) and refuses
  anything not strictly newer than what is running. Checking the image itself (not the
  manifest) also defeats a host that advertises a new version but serves an old binary. No
  eFuse anti-rollback is burned (by design), so this is the downgrade defense.

The **USB Web Serial installer** has a separate, explicit data contract. `manifest.json` schema
`layoutVersion:2` binds the site to one 40-hex `sourceSha`, exactly four chip families and, for each
family, bootloader/partition/app/otadata in fixed roles and offsets. Every part carries its expected
byte length and SHA-256; the browser downloads and verifies all four before erasing/writing, writes
the first three immutable parts, and writes `ota_data_initial@0xf000` last as the activation step.
This detects partial/mixed Pages deployments and corrupted downloads. It does not turn a compromised
Pages origin into a trust anchor — an explicit USB install still trusts the site selected by the
operator — while OTA app authenticity remains protected independently by RSA verification.

The installer executes no runtime CDN code. The official npm `esptool-js@0.6.1` native-ESM bundle
and Apache-2.0 license are stored under `docs/vendor/`; `scripts/verify-vendored-esptool-js.sh` pins
the npm tarball SRI and both extracted SHA-256 values. Pages uses `script-src 'self'` and serves that
reviewed copy same-origin.
- **Rollback is enabled** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`); `main.cpp` defers
  `esp_ota_mark_app_valid_cancel_rollback()` to a health gate (`logic/health_gate.hpp`) that keeps
  rollback armed until a freshly-flashed image has run ≈ 90 s, **proven it still has a network
  link and retained a non-critical largest contiguous INTERNAL heap block**. An image that boots
  but then crashes/OOM-reboots under load is reverted on the
  next boot — the old startup-time mark would have committed it before it proved itself — and so is
  an image that boots cleanly but never reaches the LAN, which is the one no later OTA could fix,
  because the fix would have to arrive over the link it broke. After ≈ 600 s without a link the
  image is simply left `PENDING_VERIFY` for the next reboot to roll back; it does not restart
  itself, which would let a long network outage silently downgrade a good build. Only a
  successfully persisted rebooting `/set_mqtt`, `/set_syslog`, `/set_wifi` or setup-portal save
  may confirm early. Timed and explicit confirmation both acquire the shared `HealthCommit` owner
  against OTA, identity and persisted `FaultRestart`, then re-check the INTERNAL largest block;
  admission or heap failure leaves rollback armed. `/set_vin` is different:
  `OtaIdentityMutationGuard` permits it only in the
  already-`Stable` state and while no OTA/update owns the mutation gate; `PENDING_VERIFY`, unknown
  verification state or an active OTA returns HTTP `503` before VIN/key mutation. VIN/recovery
  reboots are therefore never accepted as health evidence. A fatal essential-component failure during startup
  does not wait for another reset: while the image is still `PENDING_VERIFY`, `boot_fatal()`
  explicitly marks it invalid and reboots into the previous slot. The same failure on an
  already-valid image halts instead of entering an automatic reboot loop.

Signed OTA closes the *unsigned-artifact* gap without burning any eFuses. It does **not**
protect against a physical attacker reflashing over USB (no boot-time enforcement) — that
still requires full hardware Secure Boot v2 + Flash Encryption (below), which reuses the
**same signing key**.

## Signed OTA images

The firmware is built with the **Secure Boot v2 signature scheme but WITHOUT hardware
Secure Boot** (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` + `..._RSA_SCHEME` +
`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`, in `sdkconfig.defaults`). Every OTA image
must carry a valid **RSA-3072** signature, which the running app verifies before installing.
No eFuses are burned, so this is **reversible, cannot brick the device, and the web installer
keeps working** (with the RSA scheme the bootloader does not verify on boot — only the OTA
path does). `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`, so the build emits an *unsigned*
binary and CI signs it in a separate step — the private key never has to be present at
compile time.

> ⚠️ **A locally-built binary is unsigned and will _not_ boot.** The signature is also
> enforced *at runtime*: the running app calls `esp_secure_boot_init_checks()` at startup
> (`check_signature_on_update_check` in `bootloader_support`), which `abort()`s in a **reboot
> loop** whenever the app carries no signature block. This fires very early — before `app_main`,
> on every target — so a plain `idf.py build` image (unsigned by `..._BUILD_SIGNED_BINARIES=n`)
> **cannot be USB-flashed as-is**; it just crash-loops. To flash a development build, either use
> the **signed CI artifact**, or sign the local image first with the offline key
> (`espsecure.py sign_data --version 2 --keyfile <key> --output app.bin.signed app.bin` — the
> same step CI runs) and flash the signed copy. This has been true since the signed-OTA config
> landed — see the `flash-esp32` skill for the dev-flashing workflow.

### Trust anchor (trust-on-first-use)

With no eFuse digest, the trusted public key is taken from the **signature block of the
currently running app** (`esp_secure_boot_get_signature_blocks_for_running_app`). Practical
consequences:

- The **first** signed image is accepted by a device still on the *old, unsigned* firmware
  (firmware built **before** this signing config existed, so it performs no verification — a
  *current* build left unsigned won't boot at all, see the warning above), or can be
  USB-flashed. From then on, that device only accepts OTA images signed with the **same key**.
- This is a deliberate **one-way transition**: once a device runs a signed build it will
  **refuse an unsigned (or differently-signed) OTA**. A bad signed image still auto-rolls
  back via `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`; a downgrade to unsigned firmware needs a
  USB reflash.
- **Classic ESP32 requires chip rev v3.0+ (ECO3)** for the V2 RSA scheme — enforced by
  `CONFIG_ESP32_REV_MIN_3` in `sdkconfig.defaults.esp32`. ECO3 has been standard since ~2020.
  On pre-ECO3 ESP32 silicon the image's min-rev is checked during OTA validation, so the
  update is **rejected cleanly** (`esp_https_ota_finish` → "downloaded image is invalid") and
  the device keeps running its current firmware — it does not boot-loop, but it also can no
  longer OTA forward (a USB reflash with a rev-compatible image is the only path). This is a
  deliberate trade-off to keep one signing scheme + one key across all four targets;
  `esp32s3`/`c3`/`c6` support V2 RSA at their default min revision and need no such override.

The protected release pipeline has a separate, source-controlled authority check:
`scripts/ota-signing-public-key.sha256` pins the Secure Boot v2 public-key-block digest observed in
all four apps of the immutable production Release
[`v1.4.84`](https://github.com/0Bu/tesla-key-esp32/releases/tag/v1.4.84). After protected release or
PR-preview signing, and again for draft publication or immutable Release reuse, every app must pass
a full RSA-PSS/SHA-256 verification against that digest. This is
not a second device trust mechanism; it prevents a changed CI secret or substituted already-signed
asset from silently replacing the production authority. Updating the pin is therefore a reviewed
security migration, never a routine secret rotation.

The project flash, ship and USB-recovery procedures re-run the same app-only parser, exact
RSA-PSS/SHA-256 verification and production-authority pin check on every downloaded app before a
physical write. Size, version, chip metadata or byte identity with another downloaded artifact are
not substitutes for signer authentication.

### Create the signing key (if you don't have one yet)

Generate a **dedicated** key **offline**, on a trusted machine — do **not** reuse the GPG key
that signs git commits (wrong format/algorithm, and it conflates two separate trust domains),
and do **not** generate it in CI.

**1. Get the tooling** (`espsecure.py` ships with ESP-IDF; standalone it comes with esptool):

```bash
pip install esptool          # provides espsecure.py
```

**2. Generate the key** — RSA-3072, Secure Boot v2 scheme, the exact type CI expects:

```bash
espsecure.py generate_signing_key --version 2 --scheme rsa3072 ota_signing_key.pem
```

This writes an **unencrypted** PEM private key. (`--version 2 --scheme rsa3072` is mandatory:
a v1/ECDSA or EC key, an encrypted PEM, or a different RSA size is rejected by `sign_data`
with `Could not deserialize key data … unsupported key type`.)

**Alternative — plain OpenSSL.** The key is just a standard RSA-3072 keypair (nothing
ESP-specific lives in the key — only the *signature block* written later by `sign_data` is),
so OpenSSL produces an equivalent key if you'd rather not install esptool just for this:

```bash
openssl genrsa -out ota_signing_key.pem 3072       # PKCS#1 PEM; or use genpkey for PKCS#8:
# openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out ota_signing_key.pem
```

Constraints for Secure Boot v2 compatibility (all satisfied by the commands above):
**exactly 3072 bits**, **public exponent 65537** (OpenSSL's default), and **unencrypted** (do
**not** add `-aes256`/`-des3` — CI loads the key non-interactively and cannot supply a
passphrase). `espsecure.py sign_data` reads both PKCS#1 (`BEGIN RSA PRIVATE KEY`) and PKCS#8
(`BEGIN PRIVATE KEY`) unencrypted PEMs, so either form works. Verify it with step 3 below.

**3. Verify it before trusting it** — the same two checks CI does, run locally:

```bash
# (a) valid, UNENCRYPTED RSA-3072 key? expect "Private-Key: (3072 bit, 2 primes)", no prompt
openssl rsa -in ota_signing_key.pem -noout -text | head -1

# (b) does espsecure accept it exactly like CI? sign a throwaway file (expect "Signed … bytes")
head -c 4096 /dev/zero > /tmp/dummy.bin
espsecure.py sign_data --version 2 --keyfile ota_signing_key.pem --output /tmp/dummy.signed /tmp/dummy.bin
```

**4. Store & protect it:**

- **Losing it ⇒ no more OTA updates** (devices must be USB-reflashed). **Leaking it ⇒ signed
  OTA is worthless.** Treat it like a root key: keep it offline (password manager / hardware
  token / air-gapped), backed up in **≥2 separate locations**.
- Add it to CI as described under [Signing in CI](#signing-in-ci) below (the `OTA_SIGNING_KEY`
  secret). The repo already gitignores `*.pem` / `ota_signing_key.pem`, but never commit it.
- The same key later doubles as the hardware **Secure Boot v2** signing key (next section),
  so enabling full Secure Boot needs no key migration.

### Signing in CI

Compilation and signing are separate trust domains:

1. Store the PEM as the **`OTA_SIGNING_KEY`** secret of a protected GitHub Environment named
   **`firmware-signing`**. Paste the full, unencrypted RSA-3072 PEM — `BEGIN/END` lines included,
   with real newlines. Configure required reviewers on that Environment; the workflow alone
   cannot create this repository setting.
2. The ordinary `build` job is deliberately **unprivileged**. For a PR it checks out the exact
   `pull_request.head.sha` (the separate fast logic job may still exercise GitHub's synthetic merge
   ref), so the uploaded firmware bytes and `sourceSha` name the same source. It can execute PR
   source and the compiler, but has neither the signing key nor a write-capable token. It uploads
   only unsigned app/flash inputs plus ELF, map, generated sdkconfig, dependency lock and
   size/provenance data. That producer's metadata and self-authored inventory are **claims, not a
   trusted source attestation**. The canonical inventory covers exactly 53 allowlisted payload files
   plus its manifest and records the exact commit plus a content/mode fingerprint of all tracked and
   non-ignored source files (only the workflow-generated `version.txt` is excluded and separately
   bound). The validated display-version argument is also passed explicitly as CMake `PROJECT_VER`
   to every authoritative and independent configuration invocation; a stale repository-floor
   `version.txt` therefore cannot stamp different app-descriptor bytes in the documented local gate.
   `scripts/ci-build-all.sh` also projects the **minimal** 64 KiB alignment padding plus exactly one
   4 KiB signature sector, so an image cannot pass compilation and then unexpectedly overflow its
   OTA slot when signed. Main
   and PR authoritative builds disable ccache, so no PR-populated or caller-wrapped compiler object
   can feed a build that will be signed and published. Inside the pinned container it additionally rebuilds
   **all four targets** in fresh, independent build/SDKCONFIG directories and compares them
   byte-for-byte (both unsigned app and unstripped ELF), validates the effective
   target/optimisation/stack-usage flags, exact source/generated partition geometry, ESP image
   checksum/hash/chip/app descriptors and firmware-size baseline schema v2: reviewed per-target
   maxima for the raw unsigned app, ELF total, flash code plus rodata, static memory, `.bss` and
   IRAM. These growth maxima do not replace the independent projected-signed/OTA-slot hard gate.
   Stack baseline schema v2 inventories every compiler `.su` frame on every target, review-baselines each
   target/function frame at or above 256 bytes, rejects any frame above 4096 bytes and rejects
   unbounded dynamic frames. It is a per-frame gate and makes no call-depth claim. CI then exercises
   the actual current Ninja/GCC dependency record for every `__idf_main` object, not only the
   compile database. Repository-local dependencies must be the literal source or use one of the
   recursively reviewed local-code suffixes `.def`, `.h`, `.hh`, `.hpp`, `.hxx`, `.inc`, `.inl`,
   `.ipp` and `.tpp`; alternate repository include subroots and unreviewed suffixes fail closed.
   The presence—even with an empty value—of `CPATH`, `CPLUS_INCLUDE_PATH`, `C_INCLUDE_PATH`,
   `OBJC_INCLUDE_PATH`, `DEPENDENCIES_OUTPUT`, `SUNPRO_DEPENDENCIES`, `GCC_EXEC_PREFIX` or
   `COMPILER_PATH` is rejected before compilation, and caller `EXTRA_CFLAGS`/`EXTRA_CXXFLAGS`
   cannot be inherited around the reviewed sole `-fstack-usage` addition. Every caller
   `CCACHE_*` variable is likewise rejected by presence—including empty and future names. The
   pinned official IDF image's expected `IDF_CCACHE_ENABLE=1` default is unconditionally
   overwritten with exactly `0` before even the build-script self-test. The final
   `ninja -t commands -s` rule for each main object must also be token-identical to the
   compile-database command and start with the pinned compiler, so the authoritative compile has
   no invisible ccache launcher or external config path. CI then exercises
   the real signer + four-target manifest path with a disposable RSA-3072 key. A PR can therefore
   break neither reproducibility nor release assembly unnoticed; the production key remains absent.
   The effective configuration must also remain TLS-client-only: OTA and MQTTS retain certificate
   verification, while the unused TLS-server state machine stays out of the plain-HTTP firmware.
3. On `main`, a second, cache-free runner independently rebuilds the exact commit. The protected
   `publish` job enters `firmware-signing`, checks both complete inventories against its exact source
   checkout and compares all 53 payloads plus the canonical manifest byte-for-byte **before** it
   provisions the key. `scripts/ci-sign-artifacts.sh` repeats that comparison, opens roots,
   ancestors and leaves with `O_NOFOLLOW`, copies each single-link regular file into a private
   signer-owned stage, then rehashes the copy before reading the key. It signs those staged app bytes,
   immediately runs `espsecure.py verify_signature --version 2 --keyfile` on every output, requires
   actual size to equal the exact minimal projection, revalidates semantic image identity, and then
   independently verifies RSA-PSS/SHA-256 against `scripts/ota-signing-public-key.sha256`.
   Main revalidates the exact source as current main and the exact version as its Release candidate
   immediately before uploading the signed firmware Actions artifact. Before that upload in either
   closed mode, the job builds the local root Pages candidate and byte-binds all 16 manifest parts
   to the four complete merged images. The root is an exact inventory of twelve regular,
   single-link, non-empty files: four unversioned apps, four versioned apps and four versioned merged
   images. The same fail-closed inventory gate runs immediately before both the Actions upload and
   the draft-Release upload; all twelve names are explicitly listed, never selected by a wildcard.
   `create` proves the tag/Release is still absent immediately
   before key provisioning,
   uploads exactly 40 files into a **draft**, binds every byte, app alias, ELF checksum and every
   complete merged layout (signer-owned bootloader/partition/erased otadata/signed app, all erased
   gaps including NVS, and exact EOF). Drafts are not discoverable through GitHub's release-by-tag
   endpoint, so the upload action's numeric Release ID is validated and used for the draft API read;
   the response must carry the same ID. This also makes a protected retry safely reuse and rebind
   the existing draft instead of creating a second candidate. The job reruns the candidate check in
   the same shell step
   immediately before `PATCH draft=false`. All four apps must verify cryptographically against the
   production pin. A fresh API read must report the same stable Release with `immutable: true`.
   `reuse` accepts only that exact current immutable 40-file Release. It reads every download once
   through a directory-relative `O_NOFOLLOW` descriptor, checks API size/digest against the resulting
   immutable byte snapshot, and uses only those bytes for all signature, alias, merged-layout,
   diagnostic, root and staging decisions. A deterministic post-validation path-swap canary proves
   path replacement cannot substitute the staged bytes. It compares all 28 diagnostics and four
   signed/merged images to the current independent build, repeats the production-pin and full-layout
   checks, and stages Pages without provisioning
   the key, signing, re-uploading or mutating a Release. It still uploads one new SHA-bound Actions
   recovery artifact with the twelve verified app/merged aliases, the exact sixteen signer-owned
   per-target layout inputs and the already bound `_site/` for that successful reuse run. The
   protected `publish` job stops after a fresh API read reports
   the same stable Release with `immutable: true`; it never receives or exercises the branch deploy
   step.

   Every display-version consumer at the build, signer, Pages, manifest, Release and bench boundaries
   uses the same canonical grammar, rejects leading-zero core components such as `01.2.3`, and limits
   the value to the 31-byte ESP application descriptor.

   A separate main-push-only `deploy` job needs successful `build` and `publish`, but has no signing
   Environment, OTA key or OIDC permission. It checks out the exact SHA without persisted
   credentials, downloads only the exact SHA/version-bound Actions artifact, rechecks the twelve
   root files, the exact sixteen layout inputs, the site manifest and all 16 local Pages/merged byte
   relationships. Immediately before `gh-pages`, a fresh Release response binds all 40 remote asset
   metadata/digests to those root/layout inputs plus the 28 downloaded diagnostics while current
   immutable-Release and branch-backed Pages authority are also revalidated. Bounded cache-busted acceptance checks the live manifest plus all 16 parts
   byte-for-byte afterwards. A missing key in create mode or any build/sign/Release/deploy/live-
   channel mismatch fails closed.
4. A signed pre-merge hardware image is **opt-in**, not automatic. Add the `signed-preview` label
   to a same-repository PR after reviewing it. After its unprivileged build succeeds,
   `.github/workflows/signed-pr-preview.yml` runs from the default branch via `workflow_run`,
   verifies that the PR head is current, then launches a distinct default-branch-defined rebuild of
   that exact SHA. This rebuild may execute PR code, but has exact read-only permissions, no
   repository/Environment secret, Environment, identity token, restored cache or access to the
   primary artifact (its ephemeral `github.token` is read-scoped). Only data artifacts from that
   rebuild and the original secret-free build cross into the protected job. After the Environment
   approval wait, that job repeats the head/repository/state/label checks and independently derives
   the exact `<latest-complete-immutable-stable>-PR-<N>` version. It never checks out the PR: doing so
   in a write/key-capable `workflow_run` would create an untrusted-checkout TOCTOU path even if the
   intended use were read-only. Instead, the default-owned DAG binds both producing jobs to the same
   exact current PR head, and the trusted default-branch validator requires their canonical manifests
   and all 53 payload files to be byte-identical, structurally bounded and source-SHA/version-bound
   before provisioning the key; a different but regex-valid stable base fails too. No file from
   either artifact is executed in the protected job.
   Immediately before key provisioning, signed artifact upload and Pages publication, the protected
   job refetches the repository's current default-branch head and requires it to remain exactly the
   trusted `workflow_run` `github.sha`. A main advance therefore retires a queued old policy run
   instead of letting it sign or publish after a long Environment wait; a fresh build/run is needed.
   After signing, all four apps are independently RSA-PSS-verified against the source-controlled
   production-authority pin before either the hardware-test artifact or preview site can be
   published; verification only against the supplied Environment key is insufficient.
   PR state is checked again immediately before artifact upload and Pages publication. Fork PRs are
   ineligible. Without this labelled approval, every PR remains compile-only and unsigned. Preview
   signing and cleanup share one per-PR concurrency group, so close/force-push/label-removal cancels
   an in-flight publisher. PR-event cleanup runs only from the trusted `pull_request_target`
   definition and checks out the exact base SHA before executing the Pages validator or deletion
   script; PR workflow/code is never executed with the branch-write token. A daily and manually
   dispatchable reconciliation removes any gh-pages
   preview whose PR is no longer open, same-repository, labelled and at the manifest's `sourceSha`.

Pages has exactly one serving authority: GitHub's branch-backed legacy mode with source
`gh-pages:/`, holding both root and `PR/<N>/`. The workflows do not upload or deploy a Pages Actions
artifact. `scripts/check-pages-source.py` validates the Pages API mode/source and HTTPS URL before
protected signing, in the separate main deploy job and preview paths immediately before every branch
publication/deletion, and again when deriving the live URL for acceptance. A repository switched to Actions mode or another branch/path therefore
fails before key use or branch mutation instead of silently creating a second publication model.

These gates prove a closed, deterministic source-to-artifact relationship under the pinned build
contract; they do not prove that reviewed source is safe, that GitHub-hosted runners are trustworthy,
or that a signed image works on hardware. Code review, protected Environment approval and separate
bench/device acceptance remain distinct evidence boundaries.

The signer uses the immutable digest-pinned ESP-IDF image from `esp-idf-toolchain.txt`; rotating
that digest is therefore a security-sensitive review. For higher assurance, keep the key fully
offline and sign on a trusted machine / KMS instead of in CI (no device-workflow change is needed).

### Key rotation

This project's `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` mode deliberately supports exactly
one valid signature block in position zero. ESP-IDF v5.5 uses only that first running-app key to
verify the next OTA image; appending old and new signatures therefore **does not provide an OTA key
rotation path**. The validator rejects additional blocks so CI cannot imply otherwise.

Changing `OTA_SIGNING_KEY` or `scripts/ota-signing-public-key.sha256` alone would strand every
device anchored to the old key. A rotation requires a separately reviewed fleet migration: retain
the old key and recovery artifacts, prepare and verify the new key plus pin as one change, then
USB-flash a new-key-signed app on each explicitly authorized device while preserving NVS. Verify
each device on the new authority before retiring the old key. Rollback across authorities is also
USB-only. A future multi-key design requires a separate migration to hardware Secure Boot or a
reviewed ESP-IDF/protocol change; it is not part of this software-only TOFU contract.

## Enabling Flash Encryption + Secure Boot (recommended, IRREVERSIBLE)

This is the real fix for key-at-rest security and firmware tampering. **Burning these
eFuses is permanent and can lock you out of the device** — do it deliberately, per unit,
after testing the firmware. Read the Espressif guides first:
- <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/security/flash-encryption.html>
- <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/security/secure-boot-v2.html>

### 1. Secure Boot signing key (keep OFFLINE, back it up)

**Reuse the OTA signing key** from [Signed OTA](#signed-ota-images) — it is already the right
type (RSA-3072, v2). Hardware Secure Boot just additionally burns its public-key digest into
eFuse, so no second key and no re-signing of the existing release stream is needed. If you do
not have one yet:

```bash
espsecure.py generate_signing_key --version 2 --scheme rsa3072 ota_signing_key.pem
```

Losing this key means you can never sign an update again; leaking it defeats Secure Boot.

### 2. menuconfig

```
Security features →
  [*] Enable flash encryption on boot
        Flash encryption mode = Release        # Development mode is NOT secure
  [*] Enable hardware Secure Boot in bootloader (v2)
        Secure boot private signing key = ota_signing_key.pem   # the same key as Signed OTA
  [*] Enable NVS Encryption
```

### 3. Add an `nvs_keys` partition (required for NVS encryption)

In `partitions.csv`, add a key partition (the nvs XTS keys live here, itself protected
by flash encryption):

```
nvs_key,  data, nvs_keys, ,        0x1000, encrypted,
```

Make sure offsets still leave the `nvs` partition where it is (`0x9000`) so existing
data layout is unchanged.

### 4. Build and flash (first encrypted flash)

```bash
idf.py build
idf.py flash          # Release mode: device encrypts flash + burns eFuses on first boot
espefuse --port <PORT> summary   # confirm SPI_BOOT_CRYPT_CNT / SECURE_BOOT_EN now set
```

### 5. Optional lockdown

```bash
# Block read-back of flash over the ROM downloader (after you no longer need it):
espefuse --port <PORT> burn_efuse DIS_DOWNLOAD_MODE
# or keep download but force the secure variant:
espefuse --port <PORT> burn_efuse ENABLE_SECURITY_DOWNLOAD
```

### ⚠️ Consequence for the web installer

With flash encryption in Release mode the device only accepts **signed, and effectively
encrypted** images. The browser web-installer (esptool-js over Web Serial) writes *plaintext* parts
and can no longer update such a device. After hardening, deliver updates via **signed
OTA** or `idf.py flash` from a trusted machine. Plan the update path before burning.

## Development tooling trust boundary

[`AGENTS.md`](../AGENTS.md) is the canonical authorization policy. Analysis, review, diagnosis,
and triage are read-only by default, and an implementation request does not authorize commit,
push, merge, release, USB, flash, OTA, NVS, or live vehicle operations. The project Codex config
does not grant those mutations automatically; specialist reviewers under `.codex/agents/` run
with `sandbox_mode = "read-only"`, no model pin, and no approval escalation.

The project configuration calls the runner-neutral core under
[`tools/agent-hooks/`](../tools/agent-hooks/). Its secret, partition, and PR-gate checks are lexical
defense in depth: they do not replace sandboxing, explicit authorization, branch protection, CI
trust separation, or human review. In particular, no PR or agent-configuration gate receives
`OTA_SIGNING_KEY`; through agent tools, the only permitted signing-key use remains
an explicitly authorized, unchained `espsecure.py sign_data`/`sign-data` invocation that supplies
the key only as a keyfile path. Never print, copy, redirect, archive, upload, or pass private-key,
NVS, BLE-session, credential, or environment-dump material through an agent tool.

Repository CI actions are referenced by full commit SHA, and the firmware compiler/signing tools
run in the tag-plus-manifest-digest image from `esp-idf-toolchain.txt`. The GitHub-hosted runner OS
is still a managed external service, so only the digest-pinned container is the firmware-toolchain
identity; orchestration, host tests and GitHub itself remain part of the CI trust boundary.
Every workflow has explicit least-privilege permissions and a bounded job timeout. The read-only
`pull_request_target` PR-policy workflow checks out the exact trusted base SHA and evaluates only
server-side current-head records; it never checks out or executes PR code. It blocks merges only
after repository rules require `pr-policy / current-head-records`—a server setting the repository
cannot self-install. The manual bench workflow ingests one closed-schema `report-json` input,
checks its plausibility and exact equality to separately entered source/artifact/profile/target
values, fingerprints the validated JSON, then uploads that exact file with no intervening step and
records its SHA-256 plus GitHub's artifact ID/archive digest. Those digests cover the report, not
the firmware bytes. Report schema v2 requires recovery to start/end with
`initialBootFailCount`/`finalBootFailCount` zero, reach the safe-mode latch through at least four
fault resets, and use a separate non-fault reboot (at least five planned reboots total). Every
normal/final snapshot, including recovery's post-clear reboot, must report `httpd`, `vehicle` and
`mqtt` minimum-free-stack values at or above the reviewed one-eighth-stack policy floors
(1024/1024/768 B); optional `auto_pair` must retain 1024 B when present. These are schema acceptance
margins derived from configured stack sizes, not hardware-proven universal alarm thresholds. Source and
firmware-hash identity, signature verification, NVS preservation and every physical observation
remain operator declarations; the workflow does not collect evidence, contact a board/vehicle, or
receive a signing key.

The project MCP configuration invokes exact `@upstash/context7-mcp@4.0.2`, not a floating npm tag.
That prevents an unnoticed `latest` upgrade, but it is not a privacy sandbox or an npm integrity
lock: first use can download executable package code, and any prompt/code explicitly sent to the
external Context7 service crosses the local-agent boundary. Never send signing keys, credential
files, NVS dumps, unredacted VIN/BLE captures or CI secrets through it. Disable the project MCP
server locally when policy forbids that external service; neither firmware builds nor CI depend on
it.

## Other notes

- **Setup AP is open** (`WIFI_AUTH_OPEN`) and the WiFi password is submitted over plain
  HTTP during provisioning. Keep the setup window short; consider WPA2 on the AP.
- **Do not expose the device to the internet.** Home LAN only.
- The private key is **never logged**; VIN/MAC/SSID appear in serial logs (physical access).
- mDNS advertises firmware version but not the VIN; enumerating `_http._tcp` must not multicast a
  vehicle identifier before a trusted client explicitly queries the open LAN API.
