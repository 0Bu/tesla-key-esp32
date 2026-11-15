# tesla-key-esp32

An ESP32 Bluetooth key for local Tesla charging control with [evcc](https://evcc.io).
**Charging Manager** access covers charging, wake and vehicle data. Includes a web UI, HTTP/MCP,
MQTT/Home Assistant telemetry and signed OTA updates.

## Requirements

- **ESP32, ESP32-S3, ESP32-C3 or ESP32-C6** with at least 4 MB flash. Classic ESP32 needs rev. v3.0+.
  [Board details](docs/USER_GUIDE.md#hardware)
- USB data cable, desktop browser with Web Serial, 17-character VIN and Tesla NFC keycard.
- Continuous power within Bluetooth range of the car. Keep the unauthenticated HTTP API on a trusted LAN.

## 1. Install and configure

1. In the [web installer](https://0bu.github.io/tesla-key-esp32/), click **Connect** and select the
   serial port. For a first install, use **Clean flash** (erases all flash). For an existing
   installation, see [USB recovery](docs/USER_GUIDE.md#updates-and-recovery).
2. Join the open `tesla-key-esp32-setup` network. Save WiFi credentials and VIN at `http://192.168.4.1`.
3. After reboot, open `http://tesla-key-esp32.local` or the IP shown by your router.

Supported [Ethernet boards](docs/USER_GUIDE.md#wired-networking-optional-esp32s3) use DHCP. Set the
VIN in the web UI.

## 2. Pair with the car

Wait for **Ready to pair**. Place the Tesla keycard on the console reader and approve **Add key**
on the touchscreen. The web UI confirms pairing when the session is established.

## 3. Connect evcc

1. In the device web UI, choose **Vehicle → evcc → Copy**.
2. In evcc, open **Settings → Vehicles → Add → Custom device**, paste the YAML and set the battery
   capacity. [Manual configuration](docs/USER_GUIDE.md#evcc)

## Updates and help

- Update from the web UI's **Firmware** row. [Updates and recovery](docs/USER_GUIDE.md#updates-and-recovery)
- [Troubleshooting](docs/USER_GUIDE.md#troubleshooting) · [Documentation](docs/README.md)

[AGPL-3.0](LICENSE) · Inspired by [TeslaBleHttpProxy](https://github.com/wimaha/TeslaBleHttpProxy).
