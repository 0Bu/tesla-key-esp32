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
- **Supply chain** — OTA pulls *unsigned* firmware images; integrity rests on TLS to the
  configured HTTPS host, not on an image signature (see [OTA self-update](#ota-self-update)).

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
(`/set_vin`, un-pairs + reboots), MQTT broker change (`/set_mqtt`, reboots) and the OTA
self-update / reboot trigger (`/ota/update`, see below). This is acceptable only because:

- the enrolled key is **Charging Manager only** — it cannot unlock or drive the car, just
  control charging and wake (see the role restriction in `vehicle_ctrl.cpp`); and
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
`CONFIG_TESLA_OTA_FIRMWARE_BASE_URL` + `tesla-key-esp32-<target>.bin`, default GitHub
Pages), compares the manifest `version` to the running firmware, and on confirmation flashes
the inactive OTA slot via `esp_https_ota`, then reboots. `esp_https_ota` verifies the image
chip-id, so a wrong-target image is refused. Implemented in `main/ota_update.cpp`.

Trust model of the **current (pre-hardening) build**:

- **Transport is verified, the image is not.** TLS server certificates are checked against
  the bundled CA roots (`esp_crt_bundle_attach`), so the connection to the configured host
  is authenticated — but the downloaded image carries **no application signature** (image
  signing only takes effect once Secure Boot v2 is enabled, see below). Integrity therefore
  rests entirely on TLS to the configured HTTPS host. Whoever controls that host (or its
  GitHub Pages source) can serve arbitrary firmware to every device.
- **The trigger is unauthenticated.** `POST /ota/update` (and `GET /ota/check`) are open on
  the LAN like the rest of the API. Because the download URL is **compile-time fixed**, a
  LAN peer cannot point the device at attacker-controlled firmware — but it *can* force a
  fetch + reboot (a nuisance/DoS, and each reboot re-opens the BLE polling window so a
  parked car stops sleeping).
- **Rollback is enabled** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`); `main.cpp` calls
  `esp_ota_mark_app_valid_cancel_rollback()` only after a healthy startup, so a bad image
  is reverted on the next boot.

To raise the bar, enable Secure Boot v2 (below): `esp_https_ota` then additionally requires
a validly **signed** image, closing the unsigned-artifact gap. Restricting who can reach
`/ota/update` needs the same reverse-proxy / VLAN segmentation as the rest of the API.

## Enabling Flash Encryption + Secure Boot (recommended, IRREVERSIBLE)

This is the real fix for key-at-rest security and firmware tampering. **Burning these
eFuses is permanent and can lock you out of the device** — do it deliberately, per unit,
after testing the firmware. Read the Espressif guides first:
- <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/security/flash-encryption.html>
- <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/security/secure-boot-v2.html>

### 1. Generate a Secure Boot signing key (keep OFFLINE, back it up)

```bash
espsecure generate_signing_key --version 2 secure_boot_signing_key.pem
```

Losing this key means you can never sign an update again; leaking it defeats Secure Boot.

### 2. menuconfig

```
Security features →
  [*] Enable flash encryption on boot
        Flash encryption mode = Release        # Development mode is NOT secure
  [*] Enable hardware Secure Boot in bootloader (v2)
        Secure boot private signing key = secure_boot_signing_key.pem
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
