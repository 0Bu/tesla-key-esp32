# tesla-key-esp32

An ESP32-S3 acting as a BLE key for Tesla vehicles, exposing a REST API on your local network.  
API-compatible with [TeslaBleHttpProxy](https://github.com/wimaha/TeslaBleHttpProxy) — works as a drop-in replacement for [evcc](https://evcc.io) BLE vehicle integration.

```
[evcc / Home Assistant]
        │  HTTP (port 80)
        ▼
  [ESP32-S3] ─── BLE ───► [Tesla Model 3 / Y / S / X]
  (home network)
```

## Hardware Requirements

### Supported Boards

| Board | Flash | BLE | PSRAM | Notes |
|-------|-------|-----|-------|-------|
| [ESP32-S3-DevKitC-1](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/hw-reference/esp32s3/user-guide-devkitc-1.html) | 8 MB | ✓ | optional | ⭐ Recommended for beginners |
| [Seeed XIAO ESP32S3](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/) | 8 MB | ✓ | — | Compact and affordable |
| [M5Stack CoreS3](https://docs.m5stack.com/en/core/CoreS3) | 16 MB | ✓ | 8 MB | Includes display |
| [Waveshare ESP32-S3](https://www.waveshare.com/wiki/ESP32-S3-DEV-KIT-N8R8) | 8 MB | ✓ | 8 MB | Budget option |

**Minimum requirements:**
- Chip: **ESP32-S3** (integrated Bluetooth 5.0 LE — required)
- Flash: **≥ 4 MB** (8 MB recommended; firmware is ~2.5 MB)
- RAM: **512 KB SRAM** (no external PSRAM needed)
- Connection: USB-C or Micro-USB for initial flashing
- Power: 3.3 V or 5 V via USB

> **Not compatible:** Original ESP32, ESP32-S2, ESP32-C3 — wrong chip type or no Bluetooth.

### Accessories
- USB data cable (not a charge-only cable)
- Optional: permanent USB power supply near your Tesla parking spot

---

## Quick Start (no toolchain)

The fastest path — no ESP-IDF, no compiler, just a browser:

1. **Flash from your browser.** Open the web flasher (GitHub Pages of this repo,
   `docs/`), plug the ESP32-S3 into a computer, click **Install**. Uses Chrome/Edge
   Web Serial (desktop). On a device that already runs this firmware the flasher does
   an **update** (no erase) — WiFi, VIN and key are preserved (see
   [Upgrading](#upgrading)).
   *(Alternatively download the release assets and flash the parts at their offsets —
   this preserves the `nvs` partition: `esptool.py --chip esp32s3 write_flash 0x0
   bootloader.bin 0x8000 partition-table.bin 0x10000 tesla-key-esp32-<version>.bin`.
   For a clean full flash of a brand-new device use the single
   `tesla-key-esp32-<version>-merged.bin` at offset `0x0` — note this erases NVS.)*
2. **Set up WiFi via the captive portal.** The browser does **not** configure WiFi.
   After flashing, the device opens a **`tesla-key-esp32-setup`** network. Join it from a
   phone or laptop (incl. iPhone) — the setup form pops up automatically (captive
   portal) at `http://192.168.4.1`. Enter your WiFi **and** the VIN there; the device
   reboots and joins your network.
3. **Open the web UI.** Browse to **`http://tesla-key-esp32.local`**. It shows status (BLE,
   key, paired) and has buttons to generate the key, set the VIN, and pair with your
   vehicle — see [Tesla Key Activation](#tesla-key-activation).
4. **Add to evcc.** See [evcc Integration](#evcc-integration).

> The `tesla-key-esp32-setup` portal reappears whenever the device has no WiFi configured
> (e.g. after `erase-flash`), so you never need a USB cable to (re)configure WiFi.

> If `tesla-key-esp32.local` doesn't resolve (some networks block mDNS), use the IP shown in
> your router's client list.

Building from source (below) is only needed for development or to change defaults.

---

## Prerequisites (one-time setup)

### 1. Install ESP-IDF

```bash
# macOS / Linux
brew install cmake ninja dfu-util       # macOS (Homebrew)

mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.3.2    # or latest stable release
./install.sh esp32s3

# Add to ~/.zshrc or ~/.bashrc
echo 'alias get_idf=". ~/esp/esp-idf/export.sh"' >> ~/.zshrc
source ~/.zshrc
```

### 2. Activate ESP-IDF

Run this in every new shell session before working on the project:

```bash
get_idf
```

Verify:
```bash
idf.py --version   # should print "ESP-IDF v5.x.x"
```

### 3. Clone the repository

```bash
cd ~/esp   # or any directory of your choice
git clone https://github.com/0Bu/tesla-key-esp32.git
cd tesla-key-esp32
```

---

## Build & Flash

### Step 1 — Set the target chip

```bash
idf.py set-target esp32s3
```

### Step 2 — Configure WiFi and VIN

```bash
idf.py menuconfig
```

Navigate to **Tesla Key Configuration** and fill in:

| Setting | Description |
|---------|-------------|
| `WiFi SSID` | Your home network name |
| `WiFi Password` | Your WiFi password |
| `Tesla Vehicle VIN` | 17-character vehicle identification number |
| `Tesla BLE MAC` | Leave empty — auto-discovered on first boot |

> Find your VIN in the Tesla app under **Vehicle → About** or on your registration document.

> You can leave WiFi/VIN blank here and set them later via the **`tesla-key-esp32-setup`**
> portal (see [Quick Start](#quick-start-no-toolchain)) — handy for sharing one build.

Press `Q` to exit, save when prompted.

### Step 3 — Compile

```bash
idf.py build
```

On the first build, `yoziru/tesla-ble` is automatically downloaded from GitHub (requires internet). This takes 2–4 minutes the first time.

### Step 4 — Connect and flash the ESP32

Plug in the USB cable, then:

```bash
# List available serial ports
ls /dev/tty.usb*           # macOS
ls /dev/ttyUSB* /dev/ttyACM*   # Linux

# Flash (replace the port with yours)
idf.py -p /dev/tty.usbserial-0001 flash
```

If the ESP32 does not automatically enter flash mode:
1. Hold the `BOOT` button
2. Briefly press `RESET`
3. Release `BOOT`
4. Run the flash command

### Step 5 — Monitor serial output

```bash
idf.py -p /dev/tty.usbserial-0001 monitor
```

Expected output on first boot:
```
I (500) main: VIN: 5YJ3E1EA1JF000001  BLE MAC: (scan)
I (600) wifi: WiFi connected to 'MyNetwork'
I (650) main: IP: 192.168.1.42
I (700) http_server: HTTP server started on :80
I (700) main: tesla-key-esp32 running. API on port 80.
```

Press **Ctrl+]** to exit the monitor.

---

## Upgrading

WiFi credentials, the VIN, the private key and the BLE sessions all live in the single
`nvs` partition (offset `0x9000`, namespaces `tesla_cfg` + `tesla_ble`). An upgrade
**preserves** them as long as that partition is not erased or overwritten:

- **Web flasher / ESP Web Tools** — when the device already runs `tesla-key-esp32`, the
  flasher performs an *update* and writes only bootloader / partition table / app at
  their own offsets. The `nvs` partition is never touched → **data is kept.** A
  brand-new (or different) firmware triggers a clean install with full erase.
- **`idf.py flash`** — flashes the parts at their offsets, does not erase `nvs` →
  **data is kept.** `idf.py app-flash` rewrites only the app and is the fastest.
- **`esptool.py write_flash 0x0 …-merged.bin`** and `idf.py erase-flash` — these
  **erase the whole flash including `nvs`** → WiFi / VIN / key are lost. Only use the
  merged image / erase for a fresh device or an intentional reset.

> ⚠️ The `nvs` partition must stay at the same offset/size across versions. Do not
> change the `nvs` row in `partitions.csv` for an in-place upgrade, otherwise the old
> data is stranded and effectively lost.

---

## Alternative: Provision Without Recompiling

WiFi credentials and VIN can be written to an already-flashed device without rebuilding:

```bash
python provision.py \
    --port /dev/tty.usbserial-0001 \
    --ssid "MyNetwork" \
    --password "secret" \
    --vin "5YJ3E1EA1JF000001"
```

This only rewrites the NVS configuration partition — the firmware is left untouched.

---

## Tesla Key Activation

This is a one-time procedure that registers the ESP32 as a trusted BLE key on your vehicle.

> 💡 **Easiest:** open `http://tesla-key-esp32.local` and use the **Pair** button in the web UI.
> The ECDSA key is generated automatically on first boot, so you normally only pair.

### Prerequisites
- ESP32 is running, connected to WiFi, and has an IP address (check the serial monitor)
- Tesla vehicle is within BLE range (~10 meters)
- You have physical access to the vehicle
- You have your Tesla NFC card (the one that came with the car)

### Step 1 — Key is generated automatically

On first boot the device generates an ECDSA P-256 key pair and stores it in NVS (it never
leaves the device). The web UI shows the key's **fingerprint** — the Tesla public-key id
(`SHA-1(pubkey)[:4]`, e.g. `0E:8A:1D:BE`) — which matches the entry shown in the vehicle's
key list after pairing.

To **regenerate** the key, use the **Regenerate key** button (it asks for confirmation,
since the existing key is deleted and the vehicle is un-paired) or call:

```bash
curl -X POST "http://192.168.1.42/gen_keys?force=1"
```

> ⚠️ `force=1` is required to replace an existing key — `POST /gen_keys` without it returns
> `409` so a working key is never overwritten by accident.

### Step 2 — Send the key to the vehicle

```bash
# Enrolls a Charging Manager key (charging + wake only)
curl -X POST http://192.168.1.42/send_key
```

Response:
```json
{
  "result": true,
  "role": "charging_manager",
  "reason": "key sent — tap NFC card on Tesla center console to confirm"
}
```

### Step 3 — Confirm on the vehicle

> ⚠️ Complete this step **within ~30 seconds** of calling `/send_key`.

1. **Unlock and enter the vehicle** (the car must be awake and unlocked)
2. On the **touchscreen**: a prompt appears — *"Add new key?"*
3. Place your **NFC card** on the **card reader on the center console** (below the armrest)
4. The touchscreen confirms — *"Key added"*

The ESP32 is now a trusted key and can send BLE commands to the vehicle.

### Step 4 — Test the connection

```bash
# Wake the vehicle
curl -X POST http://192.168.1.42/api/1/vehicles/5YJ3E1EA1JF000001/command/wake_up

# Read charge state
curl http://192.168.1.42/api/1/vehicles/5YJ3E1EA1JF000001/vehicle_data
```

The first command after an idle period takes 3–8 seconds (BLE scan + connect + session handshake). Subsequent commands reuse the existing connection.

### Key Roles

Unlike TeslaBleHttpProxy, this firmware enrolls **only** a Charging Manager key — by
design, since its sole purpose is the evcc BLE integration. The owner role is not
exposed; `/send_key?role=owner` is rejected with `403`. Limiting the role means the
key stored on the device cannot unlock doors, control climate, etc., even if the
device is compromised.

| Role | Capabilities |
|------|-------------|
| `charging_manager` (only) | Control charging, wake vehicle, read charge state |

> ℹ️ Tesla allows **at most 3 active BLE keys** per vehicle.  
> Remove an old key: Tesla app → Security → Keys → delete.

---

## API Reference

Base URL: `http://<ESP32-IP>`

### Commands

```
POST /api/1/vehicles/{VIN}/command/{command}
Content-Type: application/json
```

| Command | Body | Description |
|---------|------|-------------|
| `wake_up` | — | Wake the vehicle |
| `charge_start` | — | Start charging |
| `charge_stop` | — | Stop charging |
| `set_charging_amps` | `{"charging_amps": 11}` | Set charge current in amps (0–32) |
| `set_charge_limit` | `{"percent": 80}` | Set charge limit in % (50–100) |
| `charge_port_door_open` | — | Open charge port |
| `charge_port_door_close` | — | Close charge port |
| `door_lock` | — | Lock doors |
| `door_unlock` | — | Unlock doors |
| `flash_lights` | — | Flash lights |
| `honk_horn` | — | Honk horn |
| `set_sentry_mode` | `{"on": true}` | Enable/disable sentry mode |
| `auto_conditioning_start` | — | Start climate |
| `auto_conditioning_stop` | — | Stop climate |

**Example response:**
```json
{
  "response": {
    "result": true,
    "command": "charge_start",
    "vin": "5YJ3E1EA1JF000001",
    "reason": "command executed successfully"
  }
}
```

### Vehicle Data

```
GET /api/1/vehicles/{VIN}/vehicle_data
```

```json
{
  "response": {
    "result": true,
    "vin": "5YJ3E1EA1JF000001",
    "response": {
      "charge_state": {
        "charging_state": "Charging",
        "battery_level": 72,
        "charge_limit_soc": 80,
        "charger_power": 11,
        "charge_rate": 58.3,
        "charge_amps": 16,
        "battery_range": 280.5
      }
    }
  }
}
```

> The doubled `response` and the field name `charge_amps` are intentional — they match
> the Tesla Fleet API / TeslaBleHttpProxy shape that evcc's `tesla-ble` template parses.

### Body Controller State (without waking)

```
GET /api/1/vehicles/{VIN}/body_controller_state
```

```json
{
  "response": {
    "result": true,
    "data": {
      "vehicle_lock_state": "LOCKED",
      "vehicle_sleep_status": "ASLEEP",
      "user_presence": "NOT_PRESENT"
    }
  }
}
```

### Key Management

```
GET  /                          Web UI (status + pairing + quick commands)
GET  /status                    JSON: { vin, ip, version, key_present, key_fingerprint, paired,
                                         wifi: { ssid, rssi },
                                         ble: { connected, scanning, rssi, addr | devices:[{addr,name,rssi}] },
                                         vehicle: { soc, status } (when connected, from cache) }
POST /scan                      Start a time-limited BLE discovery scan (populates ble.devices)
GET  /diag                      Plain-text in-memory diagnostic log (for on-demand analysis)
POST /gen_keys[?force=1]        Generate ECDSA P-256 key pair (refuses to overwrite without force)
POST /send_key                  Pair with vehicle (charging_manager role only)
POST /set_vin                   Persist VIN and reboot
GET  /api/proxy/1/version       Firmware version
```

The API has no authentication (see [docs/SECURITY.md](docs/SECURITY.md)) — keep the device
on a trusted home LAN.

---

## evcc Integration

Use evcc's built-in **`tesla-ble`** template (it talks to the TeslaBleHttpProxy API,
which this device implements). In `evcc.yaml`:

```yaml
vehicles:
  - name: tesla
    template: tesla-ble
    vin: 5YJ3E1EA1JF000001
    url: http://tesla-key-esp32.local   # or http://<ESP32-IP>
    port: 80                      # IMPORTANT: this device serves on 80 (evcc default is 8080)
```

evcc will automatically call:
- `GET /api/1/vehicles/{VIN}/vehicle_data?endpoints=charge_state` — SOC, charge limit, current, range, charging state
- `POST /api/1/vehicles/{VIN}/command/charge_start` / `charge_stop` — control charging
- `POST /api/1/vehicles/{VIN}/command/set_charging_amps` — set current
- `POST /api/1/vehicles/{VIN}/command/wake_up` — wake the vehicle

> ℹ️ evcc reads the state of charge from `.response.response.charge_state.battery_level`
> and the current from `…charge_state.charge_amps`. The firmware returns exactly this
> Fleet-API-compatible shape (see the `vehicle_data` example below).

---

## Troubleshooting

### ESP32 does not connect to WiFi
- Check SSID and password in `menuconfig` (case-sensitive)
- WPA2 networks only; enterprise WiFi is not supported
- After changes: reflash or use `provision.py`

### BLE scan does not find the vehicle
- Vehicle must be within range (≤10 m, no thick concrete walls between)
- BLE scanning starts after WiFi connects — both must be active
- Check serial monitor for `"Tesla BLE found"` or `"scanning for Tesla BLE..."`
- Wake the car with the Tesla app, then retry

### Command times out
```
W vehicle_ctrl: 'charge_start' timed out
```
- The car is in deep sleep and not responding over BLE
- Call `wake_up` first, wait 5 seconds, then retry the command
- Session data may have expired — erase and reflash:
  ```bash
  idf.py erase-flash && idf.py flash
  ```

### "key sent" but vehicle does not show confirmation prompt
- Step 3 must be completed **within 30 seconds**
- The car must be unlocked and awake (not in deep sleep)
- Place the NFC card flat on the center console pad, not on the touchscreen glass
- Retry the full sequence: `/gen_keys` → `/send_key` → NFC card

### Key rejected by vehicle
Remove the old key and re-register:
1. Tesla app → Security → Keys → delete the tesla-key-esp32 entry
2. Erase flash: `idf.py erase-flash && idf.py flash`
3. Repeat the full key activation process

### Serial port permission denied (Linux)
```bash
sudo usermod -aG dialout $USER
newgrp dialout
```

### evcc shows empty SoC / no charge current
- Make sure `port: 80` is set in the `tesla-ble` vehicle config (evcc defaults to 8080).
- Verify the data shape directly:
  ```bash
  curl http://tesla-key-esp32.local/api/1/vehicles/<VIN>/vehicle_data \
    | jq '.response.response.charge_state.battery_level'
  ```
  This must return a number, not `null`. If it's `null`, the firmware is too old —
  reflash; the charge data must live under `response.response.charge_state` with the
  field `charge_amps` (not `charging_amps`).
- The car must be reachable over BLE. Check `/status` → `ble_connected: true`.

---

## Security Notes

See **[docs/SECURITY.md](docs/SECURITY.md)** for the full threat model and a step-by-step
guide to enabling Flash Encryption + Secure Boot.

- The key is enrolled as **Charging Manager only** — even with physical access to the
  device, the stored key cannot unlock doors, control climate, etc. (charging + wake only)
- The **private BLE key** is stored in NVS, **unencrypted by default**. On a factory
  ESP32-S3 anyone with USB access can dump it. Enable Flash Encryption + NVS Encryption
  to protect it (see SECURITY.md) — irreversible, so plan the update path first.
- The HTTP API has **no authentication and no HTTPS** — by design, since evcc (the main
  consumer) cannot send credentials. Anyone on the LAN can call it. `/gen_keys` still
  refuses to overwrite an existing key without `?force=1`.
- Keep the device on a **trusted home LAN** and never expose it to the public internet.
  For access control, front it with a reverse proxy (TLS + auth) or a separate VLAN.

---

## Technical Details

| | |
|---|---|
| **BLE protocol** | Tesla VCSEC + Infotainment (Protobuf over GATT) |
| **Service UUID** | `00000211-b2d1-43f0-9b88-960cebf8b91e` |
| **Encryption** | ECDH key exchange + AES-GCM (via mbedTLS, built into ESP-IDF) |
| **Signing** | ECDSA P-256 (key stored in ESP32 NVS) |
| **BLE library** | [yoziru/tesla-ble](https://github.com/yoziru/tesla-ble) v5.1.1 |
| **BLE stack** | NimBLE (~200 KB smaller than Bluedroid) |
| **Fragment size** | 20 bytes per BLE write chunk |
| **HTTP server** | `esp_http_server` on port 80 |

## License

MIT — the [yoziru/tesla-ble](https://github.com/yoziru/tesla-ble) dependency is also MIT licensed.
