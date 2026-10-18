# User guide

Install and pair using the [quick start](../README.md). This guide covers ongoing operation.

## Hardware

Supports **esp32, esp32s3, esp32c3 and esp32c6**, with 2.4 GHz WiFi/BLE and at least 4 MB flash
for dual OTA; PSRAM is not required. First installation needs a USB data cable. Classic esp32
requires revision v3.0 (ECO3) or newer; pre-ECO3 chips such as ESP32-PICO-D4 are refused before
flashing. Other targets have no revision floor. ESP32-S2 lacks BLE; H2/P4 lack WiFi.

The pinned library also declares C5, but this firmware does not support it: restoring C5 requires
separate firmware, signing-size and hardware acceptance. Undeclared chips such as esp32c61 need
upstream support first. [Target decision](adr/0004-drop-esp32c5-target.md).

### Wired networking (optional, esp32s3)

The esp32s3 image also drives a **WIZnet W5500 over SPI**, so the device can run on Ethernet —
including **PoE** (one cable for power and network). That matters because BLE range to the car is
signal-limited, so mounting the device where the car is beats any radio tweak. Verified pinouts,
probed in order: **M5Stack AtomS3 Lite on an ATOMIC PoE Base** (802.3af; SCLK 5 / CS 6 / MISO 7 /
MOSI 8) and **Waveshare ESP32-S3-ETH** (SCLK 13 / CS 14 / MISO 12 / MOSI 11); custom builds can
prepend a validated four-pin mapping through Kconfig.

If wired boot obtains a DHCP lease before fallback, WiFi **never starts**, removing WiFi/BLE radio
coexistence and freeing ~57 KB of contiguous heap. No WiFi credentials are needed; set the VIN in
the web UI. Without a usable wired link it falls back to WiFi or the setup portal. A cable plugged
in later takes over routing but can leave WiFi running.
The **same** esp32s3 image serves a LilyGo T-Dongle-S3, a bare ESP32-S3 and both PoE boards, each
detected at boot; the driver is not compiled into the other targets. Design:
[ARCHITECTURE](ARCHITECTURE.md#wifi--lan-connectivity).

## Provision without rebuilding

Use the device's transactional HTTP path; **never generate and flash a partial NVS image** — it spans
the whole `0x6000` partition and erases the private key, pairing sessions and every omitted setting.
On a first boot join `tesla-key-esp32-setup`, then:

```bash
python3 provision.py --url http://192.168.4.1 --ssid MyNet --vin '<VIN>'
# password is prompted without echo; automation: --password-stdin or a chmod-600 --password-file
```

For an already reachable device use `--url http://tesla-key-esp32.local`, which calls
`POST /set_wifi` with its one-shot rollback. A vehicle identity change is deliberately separate
because it clears the old pairing:

```bash
python3 provision.py --url http://tesla-key-esp32.local --mode lan \
  --vin-only --vin '<VIN>' --confirm-vin-change
```

The retired `--port` path fails closed with a data-loss explanation; for genuine low-level recovery,
make and verify a complete NVS backup before writing at `0x9000`. The WiFi contract is identical in
the setup portal, LAN API and host tool: a 1–32-byte UTF-8 SSID plus an explicitly selected open
network, an 8–63-byte UTF-8 WPA2 passphrase, or exactly 64 ASCII hex characters (raw PSK).
Enterprise authentication is not supported.

## Pairing and identity

A configured VIN is required before any automatic enrollment. The key remains on the device.
Pairing enrolls Charging Manager only; `POST /send_key` can retry enrollment while the NFC keycard
is on the console reader. `POST /send_key?role=owner` is refused (`403`).

Changing VIN or forcing key generation removes the old pairing. Re-saving the same VIN is a no-op.
Use these operations only for an intentional identity change, then pair again. Connection timeouts
alone do not require a new key. [Pairing lifecycle](ARCHITECTURE.md#pairing-lifecycle--invalidation).

## Updates and recovery

In the web UI, click the download icon on **Firmware**, confirm the offered update and wait for
the device to reboot. Click the version to select **Release** (official releases, default) or
**Dev** (main builds). OTA preserves configuration and keys.

For USB recovery, **Keep configuration** is available for an existing dual-OTA layout.
**Erase everything** and a merged-image write erase WiFi, VIN and pairing; back up secret NVS
before an intentional destructive recovery. Never flash an unsigned local build: it cannot boot.
[Verified delivery procedures](DEVELOPMENT.md#delivery).

A reviewed same-repository PR with the `signed-preview` label may offer a signed installer at
`https://0bu.github.io/tesla-key-esp32/PR/<N>/`. Its version is `<stable>-PR-<N>` and it remains on
the selected update channel. The preview disappears when closed, force-pushed or unlabelled.

## Home Assistant (MQTT)

`main/mqtt_ha.cpp` mirrors every cached reading to MQTT using Home Assistant's
[MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery), so a **Tesla Key**
device with all entities appears automatically — no YAML. **Read-only:** no command topics are
subscribed, so HA cannot control or wake the car. The bridge runs in its own task, independent of
evcc, BLE and pairing. Runtime design:
[ARCHITECTURE](ARCHITECTURE.md#home-assistant-mqtt-bridge).

**Enable:** set the broker in the web UI (**MQTT** row, pencil icon, `IP:PORT`); it is stored in NVS
(`mqtt_uri`) and applied after the reboot it triggers. Compile-time defaults/credentials live in
`scripts/idf-docker.sh idf.py menuconfig` → *Tesla Key Configuration*:

| Option | Default | Purpose |
|--------|---------|---------|
| `CONFIG_TESLA_MQTT_BROKER_URI` | `""` | Broker (`mqtt://host:port`; empty = disabled). NVS `mqtt_uri` overrides it. |
| `CONFIG_TESLA_MQTT_USERNAME` / `_PASSWORD` | `""` | Broker auth (optional). |
| `CONFIG_TESLA_MQTT_DISCOVERY_PREFIX` | `homeassistant` | HA discovery prefix. |
| `CONFIG_TESLA_MQTT_BASE_TOPIC` | `tesla-key` | State-topic prefix. |
| `CONFIG_TESLA_MQTT_PUBLISH_INTERVAL_S` | `15` | Republish cadence (also publishes on every reconnect). |

## Syslog

`main/syslog.cpp` forwards the same output as `GET /diag` — the device's console log — to a UDP
Syslog collector (RFC 5424), best-effort. Useful to watch a pairing/reconnect live, or to keep history
past the in-RAM ring's ~16 KB and past a reboot. It is **cleartext and unredacted**
([SECURITY](SECURITY.md#syslog-and-diagnostic-export)); design: [ARCHITECTURE](ARCHITECTURE.md#syslog-forwarder).

**Enable:** set the server in the web UI (**Syslog** row, pencil icon, `IP:PORT`, e.g.
`192.0.2.1:514`; a bare host defaults to 514) — stored in NVS (`syslog_uri`), applied after the reboot
it triggers; empty disables. Compile-time default: `CONFIG_TESLA_SYSLOG_SERVER` (`""`), overridden by
NVS. Delivery only needs the hostname/IP to resolve (UDP, no ack); `/status.syslog.reachable` is an
advisory ARP/ICMP hint, so a collector behind firewalled ICMP still receives lines with
`reachable:false` shown.

## Troubleshooting

Inspect the web UI first, then `/status`, `/diag` and `/heap` as needed. Device requests require
access to your trusted LAN. Commands, pairing, update checks, verbose logging and artifact deletion
change state; distinguish them from reading an existing log or cached status.

| Symptom | Check | Next step |
|---|---|---|
| Installer has no serial port | Data cable, browser Web Serial support, port already in use | Close the serial monitor; hold BOOT while connecting or use BOOT + RESET. |
| `.local` will not load | DHCP lease in router | Open the current IP. Use that IP or `.local` for browser saves. |
| No WiFi | SSID/password, open or WPA2 network | Use setup AP or `provision.py`; do not write partial NVS images. Failed LAN credential changes roll back once. With saved credentials the device keeps retrying them and the setup AP closes after 10 minutes without a client (30 minutes at most); power-cycle to reopen it. |
| No pairing prompt | VIN configured, `key_present`, BLE range, NFC card on reader | Keep car awake and confirm on its touchscreen; retry enrollment if the prompt expired. |
| BLE connection fails | `/status.ble.connect_fail`, `car_connectable`, log cause | Move closer; disconnect another phone/fob if the car advertises non-connectable. |
| Command times out | BLE availability and car asleep | An intentional wake may be needed; wait for the link and retry. Preserve keys and logs. |
| evcc has no SOC/current | `port: 80`, VIN, cache freshness | Inspect `.response.response.charge_state` from `vehicle_data`; active stale telemetry returns `503`. |
| Key no longer paired | Key removed on car or intentional VIN/key change | Re-pair with the NFC card after confirming the cause. No firmware erase is required. |
| MQTT save fails | `mqtt.error`; HTTP `400`, `502` or `503` | Check credentials, reachability or contiguous heap respectively. A failed probe saves nothing. |
| Syslog says unreachable | Collector DNS and UDP delivery | ICMP is only advisory; a collector blocking ping may still receive logs. |
| Restart or crash | `sys.reset_reason`, `last_crash`, `last_reboot`, heap trend | Retain evidence and match a dump to the same build's ELF before clearing it. |

For sharing, use `/status?redact=1` and `/diag?redact=1`, then inspect the output: the board MAC
remains visible. Core dumps, NVS backups and unredacted Syslog are private artifacts.
[Diagnostic export](SECURITY.md#syslog-and-diagnostic-export).

For a specifically authorized wake investigation, `scripts/capture_wake.py <url> --wake` writes
an exclusive 0600 log under a private 0700 directory, redacts VIN/frame bytes by default and resets
verbose logging in `finally`. `--include-sensitive` produces private evidence; never publish it.
