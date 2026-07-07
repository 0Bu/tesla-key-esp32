# tesla-key-esp32

Turns an ESP32 (esp32 / esp32s3 / esp32c3 / esp32c6) into a Bluetooth key for a Tesla, so charging software such as
[evcc](https://evcc.io) can wake the car, read the battery level and start/stop charging
over the local network. No cloud, no Tesla API, no fees. It can also stream all vehicle
telemetry to [Home Assistant over MQTT](#step-6--home-assistant-optional).

Build from source, full API and security model: [docs/README.md](docs/README.md).

---

## Requirements

- An ESP32 board — esp32, esp32s3, esp32c3 or esp32c6 (the installer auto-detects which)
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

One-time. The key is generated automatically, and the device starts sending its enrolment
request automatically when it is near the car — there is no button to press. The car only
shows the pairing dialog while a Tesla NFC keycard is lying on the center-console card reader.

1. Park within Bluetooth range (~10 m). The web UI status goes *searching… → connecting…*.
2. Place an existing Tesla NFC keycard on the center console (the card reader behind the
   cup holders). Only then does the car's touchscreen show a pairing request
   (*"Add key"* → *"Confirm"*).
3. Confirm on the touchscreen. The web UI then shows **paired**.

The new key appears as *"Unknown key"* in the car's key list. It pairs as a Charging Manager
key only: charging and wake, no door or drive access. A Tesla keeps at most ~3 *simultaneous*
Bluetooth connections (shared across phone keys and fobs); if pairing fails at that limit,
disconnect another BLE device or remove an unused key (Tesla app → Security → Keys).

---

## Step 5 — Add to evcc

In the evcc UI go to **Settings → Vehicles → Add → Custom device** and paste the
flat config below (no `vehicles:` wrapper, no list dash — the editor handles that):

```yaml
name: tesla
type: template
template: tesla-ble
title: Tesla Key ESP32              # optional, shown in evcc UI
vin: YOUR-17-CHAR-VIN
capacity: 60                        # optional, battery kWh
url: http://tesla-key-esp32.local   # or http://<device-ip>
port: 80                            # this device uses port 80 (template default is 8080)
```

If you edit `evcc.yaml` by hand instead, nest the same fields under `vehicles:`
as a list item (`  - name: tesla` …). Restart evcc.

---

## Step 6 — Home Assistant (optional)

The device can publish everything it reads — battery, charge, climate, tyre pressures,
doors/locks, odometer, and its own WiFi/Bluetooth health — straight into Home Assistant
over MQTT. It's read-only (HA only *sees* the data; it never sends commands or wakes the car).

1. In the web UI, open the **Connection** panel and tap the **MQTT** row.
2. Enter your broker as `IP:PORT` (e.g. `192.168.1.10:1883`) and save. The device reboots.
3. With the [Home Assistant MQTT integration](https://www.home-assistant.io/integrations/mqtt/)
   enabled, a **Tesla Key** device appears automatically with all sensors — no YAML needed
   (Home Assistant MQTT Discovery). Leave the field empty to turn MQTT off again.

If the broker needs a username/password, set them once at build time via
`scripts/idf-docker.sh idf.py menuconfig` (*Tesla Key Configuration → MQTT*) — the web UI
only edits the broker address.

---

## Notes

- Keep the device on permanent USB power near the parking spot.
- The first command after idle takes a few seconds (Bluetooth reconnect); later ones are fast.
  evcc reads (state of charge) are always served instantly from cache, but a charge-current
  change sent in that cold window may only take effect on evcc's next retry once the link is warm.
- **Updates are over-the-air:** open `http://tesla-key-esp32.local`, tap the firmware version
  (top line) to check for a new release, confirm, and the device updates itself and reboots —
  WiFi, VIN and key are preserved.
- The [web installer](https://0bu.github.io/tesla-key-esp32/) is only needed for the very first
  install (or to recover a device). It does a full erase, so WiFi/VIN/key are reset and you
  re-pair once; after that, use OTA.
- **Try a PR before merge:** every same-repo pull request publishes a signed preview build at
  `https://0bu.github.io/tesla-key-esp32/#<PR-number>` (e.g. `…/#157`) that you can browser-flash
  like the release. It reports version `<latest>-PR-<N>` and still checks for OTA updates from
  `main`, so a later release moves the device forward. Removed automatically when the PR closes.
- **MCP endpoint:** AI agents (Claude Desktop/Code, VS Code, …) can talk to the device
  directly via the [Model Context Protocol](https://modelcontextprotocol.io/) at
  `http://<ESP32-IP>/mcp` (Streamable HTTP). The exposed tools mirror the charging command
  set plus a read-only state tool that never wakes the car — same trusted-LAN-only caveat
  as the rest of the API ([integration guide](docs/MCP.md)).

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Installer shows no serial port | Chrome/Edge desktop, data cable, hold **BOOT** while plugging in. |
| `tesla-key-esp32.local` won't load | Use the device IP from the router. |
| Need to change WiFi | The `tesla-key-esp32-setup` network reappears whenever no WiFi is configured — rejoin it. |
| No pairing prompt on the car | Place a Tesla NFC keycard on the center-console reader — the dialog only appears while a card is present; also bring the device closer, car must be awake, wait for *connecting…* in the web UI. |
| evcc shows no battery / current | Set `port: 80`; car must be in Bluetooth range. |

Full reference: [docs/README.md](docs/README.md).

---

## Notes on scope

- Local network only — keep it on a trusted LAN, never expose it to the internet ([security](docs/SECURITY.md)).
- Inspired by and API-compatible with [TeslaBleHttpProxy](https://github.com/wimaha/TeslaBleHttpProxy) — evcc talks to either without changes.
- Licensed under **AGPL-3.0** ([LICENSE](LICENSE)). The firmware statically links the AGPL-3.0 [yoziru/tesla-ble](https://github.com/yoziru/tesla-ble) library, so the combined work is itself AGPL-3.0 — see [docs](docs/README.md#license).
