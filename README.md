# tesla-key-esp32

Turns an ESP32-S3 into a Bluetooth key for a Tesla, so charging software such as
[evcc](https://evcc.io) can wake the car, read the battery level and start/stop charging
over the local network. No cloud, no Tesla API, no fees.

Build from source, full API and security model: [docs/README.md](docs/README.md).

---

## Requirements

- An ESP32-S3 board
- A browser with Web Serial support (Chrome / Edge, desktop)
- The Tesla's 17-character VIN

---

## Step 1 — Install the firmware

- Open the web installer: **https://0bu.github.io/tesla-key-esp32/**
- Click **Install** and select the serial port.

---

## Step 2 — Connect to WiFi

The installer flashes firmware only; it does not set WiFi.

1. Join the device's `tesla-key-esp32-setup` WiFi network.
2. The setup page opens automatically (else: `http://192.168.4.1`).
3. Enter the WiFi credentials and VIN, save. The device reboots and joins the network.

---

## Step 3 — Open the web UI

`http://tesla-key-esp32.local`

Shows WiFi, Bluetooth and pairing status.

---

## Step 4 — Pair with the car

One-time. The key is generated automatically, and pairing starts automatically when the
device is near the car — there is no button to press.

1. Park within Bluetooth range (~10 m). The web UI status goes *searching… → connecting…*.
2. The car's touchscreen shows a pairing request (*"Add new key"* / *"Bestätigen"*).
3. Confirm on the touchscreen. The web UI then shows **paired**.

The new key appears as *"Unknown key"* in the car's key list. It pairs as a Charging Manager
key only: charging and wake, no door or drive access. A Tesla allows at most 3 Bluetooth keys
per car (manage in Tesla app → Security → Keys).

---

## Step 5 — Add to evcc

```yaml
vehicles:
  - name: tesla
    template: tesla-ble
    vin: YOUR-17-CHAR-VIN
    url: http://tesla-key-esp32.local   # or http://<device-ip>
    port: 80                            # this device uses port 80
```

Restart evcc.

---

## Notes

- Keep the device on permanent USB power near the parking spot.
- The first command after idle takes a few seconds (Bluetooth reconnect); later ones are fast.
- Re-running the [web installer](https://0bu.github.io/tesla-key-esp32/) updates in place; WiFi, VIN and key are preserved.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Installer shows no serial port | Chrome/Edge desktop, data cable, hold **BOOT** while plugging in. |
| `tesla-key-esp32.local` won't load | Use the device IP from the router. |
| Need to change WiFi | The `tesla-key-esp32-setup` network reappears whenever no WiFi is configured — rejoin it. |
| No pairing prompt on the car | Bring the device closer; car must be awake; wait for *connecting…* in the web UI. |
| evcc shows no battery / current | Set `port: 80`; car must be in Bluetooth range. |

Full reference: [docs/README.md](docs/README.md).

---

## Relation to TeslaBleHttpProxy

This firmware implements the same HTTP API as
[TeslaBleHttpProxy](https://github.com/wimaha/TeslaBleHttpProxy), a mature and well-maintained
project that this one builds on. TeslaBleHttpProxy runs as a Docker container or host service
and is a good fit when a server, NAS or Raspberry Pi is already running near the car. Because
the API is shared, evcc talks to either one without changes.

The difference is the hardware. This project runs the proxy directly on an ESP32-S3 board on a
USB charger, with no separate computer and lower power draw. Both reach the same result;
choose by whichever hardware you already have.

---

## Notes on scope

- Local network only — keep it on a trusted LAN, never expose it to the internet ([security](docs/SECURITY.md)).
- API-compatible with [TeslaBleHttpProxy](https://github.com/wimaha/TeslaBleHttpProxy).
- MIT licensed. Built on [yoziru/tesla-ble](https://github.com/yoziru/tesla-ble).
