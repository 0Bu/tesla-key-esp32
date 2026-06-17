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
- **Supply chain** — unsigned firmware artifacts.

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
the API would break the main use case. Anyone on the LAN can therefore call the endpoints
(wake, charging control, key regeneration, pairing). This is acceptable only because:

- the enrolled key is **Charging Manager only** — it cannot unlock or drive the car, just
  control charging and wake (see the role restriction in `vehicle_ctrl.cpp`); and
- the device is meant to live on a **trusted home LAN**, never exposed to the internet.

If you need access control, put the device behind a reverse proxy with TLS + auth, or
segment it onto a trusted VLAN.

Two non-auth hardening measures remain in place:

- **`/gen_keys` overwrite guard** — refuses to regenerate when a key already exists
  (returns `409`); regenerating un-pairs the vehicle. Use `/gen_keys?force=1` to replace.
- **Body size cap** — POST bodies over 2 KB are rejected (bounds the receive buffer).

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
