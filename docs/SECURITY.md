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
(`/set_vin`, un-pairs + reboots), MQTT broker change (`/set_mqtt`, reboots), the OTA
self-update / reboot trigger (`/ota/update`, see below) and the MCP endpoint (`/mcp`,
which exposes the same charging command set to AI agents — nothing beyond what the open
REST routes already allow). This is acceptable only because:

- the enrolled key is **Charging Manager only** — it cannot unlock or drive the car, just
  control charging and wake (see the role restriction in `vehicle_pairing.cpp`); and
- the device is meant to live on a **trusted home LAN**, never exposed to the internet.

If you need access control, put the device behind a reverse proxy with TLS + auth, or
segment it onto a trusted VLAN.

Two non-auth hardening measures remain in place:

- **`/gen_keys` overwrite guard** — refuses to regenerate when a key already exists
  (returns `409`); regenerating un-pairs the vehicle. Use `/gen_keys?force=1` to replace.
- **Body size cap** — POST bodies over 2 KB are rejected (bounds the receive buffer).

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
- **Rollback is enabled** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`); `main.cpp` defers
  `esp_ota_mark_app_valid_cancel_rollback()` to a health gate that keeps rollback armed until a
  freshly-flashed image has run healthily for a window (≈ 90 s). An image that boots but then
  crashes/OOM-reboots under load is reverted on the next boot — the old startup-time mark would
  have committed it before it proved itself.

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
  `esp32s3`/`c3`/`c6` need no such override.

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

CI signs every image with this key, supplied via an encrypted repository secret:

1. Store the PEM as the **`OTA_SIGNING_KEY`** Actions secret (ideally scoped to a protected
   GitHub *Environment*). Paste the **full, unencrypted** RSA-3072 PEM — `BEGIN/END` lines
   included, with real newlines (not base64-wrapped, not single-line).
2. Signing runs on a push or manual `workflow_dispatch` on `main` (the paths that publish bins
   — a GitHub release and/or the GitHub Pages OTA channel, redeployed on every main push and on
   a manual re-publish dispatch) **and on `pull_request`**, so a PR uploads a *signed*,
   boot-able firmware artifact you can flash to try the change before merge. (An unsigned image
   `abort()`s in a reboot loop on a signed-build device — see the warning above — so an unsigned
   PR artifact would not be testable.) `.github/workflows/build.yml` writes the secret to
   `ota_signing_key.pem` for the run (gitignored, shredded afterwards), and
   `scripts/ci-build-all.sh` signs each built image in place with `espsecure.py sign_data
   --version 2`. **Trade-off:** the key is therefore exposed to same-repo (branch) PR CI too,
   not only main — acceptable for a single-maintainer repo where PRs come from trusted branches.
3. A publish run on `main` **fails** if the secret is missing (refuses to publish unsigned
   firmware to the OTA channel). A **fork** PR gets no repository secrets, so it builds
   **unsigned** (a compile-only check — that artifact won't boot on a signed-build device); the
   size gate still absorbs the ~4 KB a signed build adds.

For higher assurance, keep the key fully offline and sign on a trusted machine / KMS instead
of in CI (no workflow change to the device is needed — only where `sign_data` runs).

### Key rotation

The v2 scheme allows up to **3 trusted public keys** at once. To rotate: ship a release
signed with both old+new keys (so currently-deployed devices, anchored on the old key, still
accept it and pick up the new one), update all devices, then drop the old key from later
releases.

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
encrypted** images. The browser web-installer (ESP Web Tools) writes *plaintext* parts
and can no longer update such a device. After hardening, deliver updates via **signed
OTA** or `idf.py flash` from a trusted machine. Plan the update path before burning.

## Other notes

- **Setup AP is open** (`WIFI_AUTH_OPEN`) and the WiFi password is submitted over plain
  HTTP during provisioning. Keep the setup window short; consider WPA2 on the AP.
- **Do not expose the device to the internet.** Home LAN only.
- The private key is **never logged**; VIN/MAC/SSID appear in serial logs (physical access).
