# User guide

Install and pair using the [quick start](../README.md). This guide covers ongoing operation.

## Hardware

Supports **esp32, esp32s3, esp32c3 and esp32c6**, with 2.4 GHz WiFi/BLE and at least 4 MB flash
for dual OTA; PSRAM is not required. First installation needs a USB data cable. Classic esp32
requires revision v3.0 (ECO3) or newer. The installer refuses a detected pre-ECO3 revision before
flashing; if the revision cannot be read, it continues and the bootloader remains the final check.
An unreadable revision is not evidence of compatibility. esp32c3 builds keep ESP-IDF's default minimum of revision v0.3; esp32s3 and esp32c6
have no revision floor. ESP32-S2 lacks BLE; H2/P4 lack WiFi.

The esp32c3 image limits WiFi transmit power to **10 dBm**, including the setup AP and station
startup. This is intended to mitigate the reported Super Mini startup failures in
[#415](https://github.com/0Bu/tesla-key-esp32/issues/415), but can reduce WiFi range. The report has
no serial reset log, so a brownout is not confirmed. If startup still fails, retain a serial boot
log and check the USB cable, power supply and board antenna before erasing configuration.

An esp32c3 board that refuses every update check with `not enough memory for update` usually runs
v1.6.3 or a Dev build around it. Those builds lack the C3 RAM settings and cannot fetch them over
the air. Once a release newer than v1.6.3 is published, reinstall such a board once with USB
**Flash** (see [Updates and recovery](#updates-and-recovery)), which keeps WiFi, VIN, pairing and
sessions; update checks are then admitted again. Boards on v1.6.2 or older are not refused, but
update them only to a release newer than v1.6.3: v1.6.3 itself cannot confirm on a C3 and rolls
back on the next reboot.

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
make and verify a complete NVS backup before writing at `0x9000`. All three provisioning paths
require a 1–32-byte UTF-8 SSID. The password is an 8–63-byte UTF-8 WPA2 passphrase or exactly
64 ASCII hex characters (raw PSK). In the setup portal and LAN API an empty password means an
open network; the host tool requires `--open-network` to select that explicitly. Enterprise
authentication is not supported.

## Pairing and identity

A configured VIN is required before pairing. The device sends its existing Charging Manager key
automatically; there is no manual send button or HTTP endpoint. Place the Tesla NFC keycard on the
console reader and approve the request on the touchscreen. The hero shows the pairing status.
Sending the request does not confirm vehicle approval; wait for the session to be established.

Before each attempt the device probes status. Unconfirmed attempts use backoff of 60, 120, 240,
then 300 seconds, allowing a keycard placed later to complete pairing. Connection-recovery hints
can request the same bounded flow with an existing session; they never replace the key or delete
sessions. A failed storage read or active OTA operation prevents enrollment.

Changing VIN or forcing key generation removes the old pairing. Re-saving the same VIN is a no-op.
Use these operations only for an intentional identity change, then pair again. Connection timeouts
alone do not require a new key. [Pairing lifecycle](ARCHITECTURE.md#pairing-lifecycle--invalidation).

## Updates and recovery

In the web UI, click the download icon on **Firmware**, confirm the offered update and wait for
the device to reboot. Click the version to select **Release** (official releases, default) or
**Dev** (main builds). OTA preserves configuration and keys.
The update dialog shows up to five change entries at once. Scroll inside the list to read more.

For USB recovery, **Connect** checks the chip and reads the installed partition table and firmware
without writing. This restarts the board through its bootloader; the log then follows its boot.
**Flash** writes the bootloader, partition table, app and OTA selector and keeps WiFi, VIN, pairing
and sessions; it is offered for tesla-key-esp32 on a layout with the same NVS partition. Otherwise
only **Clean flash** remains. Clean flash and a merged-image write erase WiFi, VIN and pairing, and
Clean flash asks for confirmation first; back up secret NVS before an intentional destructive
recovery. If the log reports `Installed firmware unknown`, both choices stay available: use Flash
only when the board already runs tesla-key-esp32, because Flash over another firmware's data can
stop the new firmware at boot.

- After an install the log shows `Booted <version>`, or a warning after 20 s.
- **Disconnect** also removes the browser's permission for the port where the browser supports it.
- A single port that is still paired reconnects by itself when the page loads, or when the board
  is plugged in more than 5 s after it was unplugged or last used by the page; that check restarts
  the board. Sooner, for example after the board's own reset, the log asks for **Connect**, so the
  board is not reset again. Several paired ports always wait for **Connect**.
- After a timeout, **Reset**, **Flash** and **Clean flash** stay disabled until the stopped serial
  operation has released the port; then retry.
- After a USB disconnect, plug the board back in; click **Connect** if it does not reconnect.
- If the log says the port remains reserved, reconnect the board and reload the page.
- Add `?trace` to the installer address to log esptool-js's packet trace.

Never flash an unsigned local build: it cannot boot. [Verified delivery procedures](DEVELOPMENT.md#delivery).

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

The web page labels a failed or timed-out device connection and retains the last received values.
A successful status refresh clears the notice. Vehicle actions use the named button inside the gauge;
tapping the surrounding status card does not send a command.

If the hero reports **Pairing paused**, a local storage or identity recovery condition blocks
normal automatic enrolment. The recovery notice describes it; storage checks and saved-key cleanup
retry automatically. An ambiguous automatic first-key commit can request one guarded restart to
reload the saved identity. Other mismatches remain paused; inspect diagnostics if the notice remains.
Do not regenerate the key as a recovery step. Normal pairing has no extra key-send notice or button.
**Device recovery required** names the first fatal component category and means vehicle services
stopped for this process. HTTP diagnostics exist only when HTTP startup succeeded; early boot
failures can require serial diagnostics. Keep the device powered and retain diagnostic evidence
before following a recovery procedure.

## Troubleshooting

Inspect the web UI first, then `/status`, `/diag` and `/heap` as needed. Device requests require
access to your trusted LAN. Commands, pairing, update checks, verbose logging and artifact deletion
change state; distinguish them from reading an existing log or cached status.

| Symptom | Check | Next step |
|---|---|---|
| Installer has no serial port | Data cable, browser Web Serial support, port already in use | Close other serial programs or tabs; hold BOOT while connecting or use BOOT + RESET. |
| Installer: version not seen within 20 s | Adapter without reset lines, a native USB port that reconnected late, or an image that does not boot | The firmware is already on flash: press **Reset** or the board's RESET button, or reconnect the cable, and read the log. If it still does not start, reinstall with **Flash** rather than **Clean flash**. |
| `.local` will not load | DHCP lease in router | Open the current IP. Use that IP or `.local` for browser saves. |
| No WiFi | SSID/password, open or WPA2 network | Use setup AP or `provision.py`; do not write partial NVS images. Failed LAN credential changes roll back once. With saved credentials the device keeps retrying them and the setup AP closes after 10 minutes without a client (30 minutes at most); power-cycle to reopen it. |
| No pairing prompt | VIN configured, `key_present`, BLE range, NFC card on reader | Place the keycard on the reader and wait for the automatic same-key retry with backoff, then confirm on the touchscreen. |
| BLE connection fails | `/status.ble.connect_fail`, `car_connectable`, log cause | Move closer; disconnect another phone/fob if the car advertises non-connectable. |
| Command times out | BLE availability and car asleep | An intentional wake may be needed; wait for the link and retry. Preserve keys and logs. |
| evcc has no SOC/current | `port: 80`, VIN, cache freshness | Inspect `.response.response.charge_state` from `vehicle_data`; active stale telemetry returns `503`. |
| Key needs attention | `reauth`, stored session and BLE availability | Reconnection and same-key pairing run automatically. Place the NFC card on the reader and confirm a pairing prompt on the touchscreen. No firmware erase is required. |
| MQTT save fails | `mqtt.error`; HTTP `400`, `502` or `503` | Check credentials, reachability or contiguous heap respectively. A failed probe saves nothing. |
| Syslog says unreachable | Collector DNS and UDP delivery | ICMP is only advisory; a collector blocking ping may still receive logs. |
| Restart or crash | `sys.reset_reason`, `last_crash`, `last_reboot`, heap trend | Retain evidence and match a dump to the same build's ELF before clearing it. |

### Safe mode and fatal startup

Safe mode keeps networking, the web UI and OTA available when those services start successfully;
BLE, vehicle work and MQTT stay disabled. Check `sys.safe_mode` and retain the boot log before
changing configuration or restarting. Identify the first crash or counter-storage error.

After correcting the cause, one intentional restart can clear the crash sequence only if its
counter is readable, valid and successfully reset in NVS. Check that `sys.safe_mode` becomes false
and normal startup completes. If storage errors remain or safe mode returns, preserve the log
and investigate storage recovery; repeated resets or firmware installs do not repair shared NVS.
Preserve keys and configuration. Erase, restore or key replacement requires a separate recovery
plan; see [NVS retention](SECURITY.md#nvs-retention).

A fatal startup failure can halt an already valid image before HTTP or Syslog has started. If
the web UI is unavailable, retain the serial boot record and the last `FATAL` component before
another attempt. A pending OTA image instead attempts rollback. Use the identified, signed
[delivery/recovery procedure](DEVELOPMENT.md#delivery) if firmware repair is needed; a halt alone
does not justify erasing configuration or changing identity.

### Sharing diagnostics

For sharing, use `/status?redact=1` and `/diag?redact=1`, then inspect the output: the board MAC
remains visible. Core dumps, NVS backups and unredacted Syslog are private artifacts.
[Diagnostic export](SECURITY.md#syslog-and-diagnostic-export).

For a specifically authorized wake investigation, `scripts/capture_wake.py <url> --wake` writes
an exclusive 0600 log under a private 0700 directory, redacts VIN/frame bytes by default and resets
verbose logging in `finally`. `--include-sensitive` produces private evidence; never publish it.
