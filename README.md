# tesla-key-esp32

Use an ESP32 as a Tesla Bluetooth key for local charging control with [evcc](https://evcc.io).
The firmware provides a web UI, HTTP and MCP APIs, MQTT/Home Assistant telemetry and signed OTA
updates. It needs no Tesla cloud API. The enrolled key has the **Charging Manager** role: charging,
wake and read access.

## Requirements

- An esp32, esp32s3, esp32c3 or esp32c6 board with at least 4 MB flash; classic esp32 requires
  revision v3.0 or newer. [Board details](docs/USER_GUIDE.md#hardware).
- A USB data cable and desktop browser with Web Serial support for installation.
- The car's 17-character VIN and an existing Tesla NFC keycard.
- Permanent power near the parking spot and a trusted LAN. The API has no authentication or TLS;
  never expose it to the internet. [Security model](docs/SECURITY.md#http-api-exposure).

## 1. Install and configure

1. Open the [web installer](https://0bu.github.io/tesla-key-esp32/), click **Select USB device** and
   select the serial port. Choose **Erase everything** for a first installation, which erases
   existing configuration and keys, or **Keep configuration** for an already compatible
   installation. Then click **Install firmware**.
2. Join `tesla-key-esp32-setup`. Open `http://192.168.4.1` if the setup page does not appear.
3. Save WiFi credentials and VIN. After the reboot, open `http://tesla-key-esp32.local`, or use
   the IP shown by your router. The installer itself does not configure WiFi.

An ESP32-S3 with a supported wired board uses DHCP; configure its VIN in the web UI.

## 2. Pair with the car

After a VIN is configured, the device creates its first key if needed and sends it automatically.

1. Park within Bluetooth range, approximately 10 m, and wait for the web UI to show **Ready to pair**.
2. Place an existing Tesla NFC keycard on the center-console reader behind the cup holders.
3. Confirm **Add key** on the car's touchscreen while the card is present. The web UI shows
   **paired** after the session is established; the car lists an **Unknown key**.

If pairing fails, check [troubleshooting](docs/USER_GUIDE.md#troubleshooting). Tesla's roughly
three simultaneous BLE connections are shared with phone keys and fobs.

## 3. Connect evcc

In evcc, open **Settings → Vehicles → Add → Custom device** and paste the
[custom vehicle example](docs/examples/evcc-custom-vehicle.yaml). Replace `DEVICE_HOST` and
`REPLACE_WITH_VIN` in every URI; the device uses HTTP port 80. Adjust the battery capacity.
The Settings editor takes the example as-is, without `name`, `type` or a `vehicles` wrapper.

For a hand-edited `evcc.yaml`, create one item under `vehicles:` with `name: tesla` and
`type: custom`, then indent the example's entire mapping beneath that item and restart evcc.
The example sets 30 s on reads and writes to cover the firmware's 20 s command budget with
reserve; queued requests can still exceed it. Reads use cached telemetry. A timeout does not
prove that a command failed: check vehicle state before repeating an uncertain action.
See the [command contract](docs/API.md#commands).

## Further use

- [User guide](docs/USER_GUIDE.md): hardware, MQTT, Syslog, updates and troubleshooting.
- [Documentation index](docs/README.md): APIs, development, architecture and security.

Update through the web UI's **Firmware** row. OTA preserves WiFi, VIN and pairing keys; use the
USB installer for first installation or recovery. Do not erase flash to resolve an ordinary
connection problem.

Inspired by [TeslaBleHttpProxy](https://github.com/wimaha/TeslaBleHttpProxy).
Licensed under [AGPL-3.0](LICENSE), including the linked tesla-ble library. Network users must have
access to the corresponding source for the firmware version they use.
