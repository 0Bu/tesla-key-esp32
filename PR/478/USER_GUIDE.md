# User guide

[Quick start](../README.md)

## Hardware

- **ESP32, ESP32-S3, ESP32-C3 and ESP32-C6** need at least 4 MB flash for dual OTA. WiFi uses 2.4 GHz.
- Classic ESP32 requires rev. v3.0 (ECO3) or newer. The installer blocks older detected revisions.
  If detection fails, the bootloader checks the revision at startup.
- ESP32-C3 requires rev. v0.3 or newer. Its 10 dBm WiFi limit can reduce range and is intended to
  mitigate [reported Super Mini startup failures](https://github.com/0Bu/tesla-key-esp32/issues/415).
  If startup fails, retain the serial log and check USB power and the antenna.

[C5 target decision](adr/0004-drop-esp32c5-target.md)

### Wired networking (optional, esp32s3)

The ESP32-S3 image supports **WIZnet W5500 Ethernet** via SPI. It probes these pinouts in order.

| Board | SCLK | CS | MISO | MOSI |
|---|---:|---:|---:|---:|
| M5Stack AtomS3 Lite + ATOMIC PoE Base (802.3af) | 5 | 6 | 7 | 8 |
| Waveshare ESP32-S3-ETH | 13 | 14 | 12 | 11 |

Custom builds can prepend a pinout in Kconfig. Connect Ethernet before boot. With an early DHCP
lease, WiFi stays off. Set the VIN in the web UI. Without a lease, the device falls back to WiFi or
the setup portal. Late Ethernet can coexist with station WiFi. See
[network startup and fallback](ARCHITECTURE.md#wifi--lan-connectivity).

## Provision without rebuilding

Use `provision.py` over HTTP to preserve NVS. A partial NVS image replaces the entire partition,
including private keys and sessions. On first boot, join `tesla-key-esp32-setup` and run the command below.

```bash
python3 provision.py --url http://192.168.4.1 --ssid MyNet --vin '<VIN>'
# password is prompted without echo; automation: --password-stdin or a chmod-600 --password-file
```

On the LAN, use `--url http://tesla-key-esp32.local` or the board's IP address. Use the IP address
when there are multiple boards. Failed WiFi changes roll back once. Use the separate VIN command
only for an intentional identity change, which clears pairing.

```bash
python3 provision.py --url 'http://<ESP32-IP>' --mode lan \
  --vin-only --vin '<VIN>' --confirm-vin-change
```

- SSID must be 1–32 UTF-8 bytes. Use an 8–63-byte UTF-8 WPA2 passphrase or 64 ASCII hex digits for a raw PSK.
- For open WiFi, leave the password blank in the setup portal or LAN API. Use `--open-network` with
  `provision.py`.
- For low-level recovery, verify a complete NVS backup before writing to `0x9000`.

## Pairing and identity

With a VIN configured, the device sends its Charging Manager key automatically. If the session is
still unconfirmed, retries back off to 60, 120, 240 and then 300 seconds. Keep the keycard on the
console reader and watch for the prompt. A failed storage read or active OTA pauses enrollment.

Changing VIN or regenerating the key clears the old pairing. Re-saving the same VIN is a no-op.
Connection recovery reuses the existing key. [Pairing lifecycle](ARCHITECTURE.md#pairing-lifecycle--invalidation)

If identity recovery is pending (`409`) or unreadable (`503`), leave VIN blank to save network
settings. Finish identity recovery before setting the VIN.

## evcc

- The web UI fills in the VIN and prefers the device's IPv4 address. Reserve that address in your
  router if evcc uses it.
- For the [YAML example](examples/evcc-custom-vehicle.yaml), replace `DEVICE_HOST` and
  `REPLACE_WITH_VIN` in every URI. The device uses HTTP port 80.
- Paste the mapping directly into **Custom device**. In `evcc.yaml`, put the mapping inside one
  `vehicles:` list item with `name: tesla` and `type: custom`, then restart evcc.
- Check vehicle state before repeating a command after a timeout. [Request deadlines and results](API.md#commands)

## Updates and recovery

In **Firmware**, click the version to choose **Release** (default) or **Development** (main builds).
Click the download icon, confirm the update and wait for reboot. OTA preserves configuration and
keys. If a channel change, another check or an updated candidate invalidates the offer, check again.

In the USB installer, **Connect** restarts the board to read its chip, layout and firmware. Choose
the write mode based on the result.

| Mode | Effect |
|---|---|
| **Flash** | Updates the bootloader, partition table, app and OTA selector. Preserves WiFi, VIN, pairing and sessions on a compatible tesla-key-esp32 layout. If the log reports `Installed firmware unknown`, choose this only for a known tesla-key-esp32 installation. |
| **Clean flash** | Erases all flash after confirmation. Use for first installation or intentional recovery. Back up NVS before erasing an existing configuration. |

- After an install the log shows `Booted <version>`, or a warning after 20 s.
- A single authorized port can auto-reconnect and reset the board. After a board reset or with
  several authorized ports, follow the log's prompt to click **Connect**.
- After a timeout, wait for the serial operation to release the port. If the port remains reserved,
  reconnect the board and reload the page.
- Add `?trace` to the installer URL for a serial packet trace.

Never flash an unsigned local build: it cannot boot. [Verified delivery procedures](DEVELOPMENT.md#delivery).

A reviewed same-repository PR with the `signed-preview` label may offer a signed installer at
`https://0bu.github.io/tesla-key-esp32/PR/<N>/`. Its version is `<stable>-PR-<N>` and it remains on
the selected update channel. The preview disappears when closed, force-pushed or unlabelled.
Preview OTA and Dev uploads still enforce numeric-core downgrade protection. A preview based on
an older stable core cannot replace a newer Development core; use its signed USB installer for
that case. Equal or newer preview cores can pass the version gate, subject to the normal OTA checks.

### Update memory recovery

An esp32c3 or classic esp32 board that refuses every update check with `not enough memory for
update` usually runs a build without its target's RAM settings: v1.6.3 (and for the classic esp32
also v1.6.4) or a Dev build around them. Such a board cannot fetch the settings over the air.
Reinstall it once with USB **Flash** (see [Updates and recovery](#updates-and-recovery)) from a
release that includes them — v1.6.4 or newer for esp32c3, a release newer than v1.6.4 for esp32 —
which keeps WiFi, VIN, pairing and sessions; update checks are then admitted again. The classic
esp32 keeps little margin, so a live vehicle link or MQTTS can still make a check refuse; retry
after a reboot. Boards on v1.6.2 or older are not refused, but update them only to such a
release: a build without the settings cannot confirm itself and rolls back on the next reboot.

An esp32s3 board without the S3 RAM settings (v1.6.5 and older, or a Dev build without them) can
settle below the memory OTA needs; this was observed on a paired board on Ethernet. An image
installed over the air then stays pending verification, update checks are refused with `not enough
memory for update`, and the next restart returns the board to its previous firmware. The firmware
has no restart control, so power-cycle the board once (unplug its PoE or USB power). Do not
reinstall v1.6.5. Use a newer release that includes the S3 RAM settings; if none is published,
use a provenance-verified, production-signed Dev build known to include them. If checks are
still refused, use its compatible signed installer for USB **Flash** once, preserving WiFi, VIN,
pairing and sessions. Follow [Updates and recovery](#updates-and-recovery) and the signed-image,
chip/bootloader compatibility and NVS-preservation checks. A signed app alone instead needs the
[app-only USB procedure](DEVELOPMENT.md#delivery); a locally unsigned build is not a recovery source.

## Home Assistant (MQTT)

Edit the broker (`host:port` or URI) in the web UI's **MQTT** row. The device stores it as `mqtt_uri`
and reboots. [MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery)
adds a **Tesla Key** device with read-only telemetry.

Use `mqtts://` for credentials and check `mqtt.tls` and `mqtt.error` in `/status`.
[Transport selection and trust](SECURITY.md#mqtt-transport)

Build defaults are in `scripts/idf-docker.sh idf.py menuconfig` → *Tesla Key Configuration*.

| Option | Default | Purpose |
|--------|---------|---------|
| `CONFIG_TESLA_MQTT_BROKER_URI` | `""` | Broker (`mqtt://host:port`; empty = disabled). NVS `mqtt_uri` overrides it. |
| `CONFIG_TESLA_MQTT_USERNAME` / `_PASSWORD` | `""` | Broker auth (optional). |
| `CONFIG_TESLA_MQTT_DISCOVERY_PREFIX` | `homeassistant` | HA discovery prefix. |
| `CONFIG_TESLA_MQTT_BASE_TOPIC` | `tesla-key` | State-topic prefix. |
| `CONFIG_TESLA_MQTT_PUBLISH_INTERVAL_S` | `15` | Republish cadence (also publishes on every reconnect). |

## Syslog

The device forwards console logs over UDP (RFC 5424) in **cleartext and unredacted**. Set `host:port`
in the **Syslog** row, or leave it blank to disable. Port 514 is the default. The device stores
`syslog_uri`, overriding `CONFIG_TESLA_SYSLOG_SERVER`, and reboots.

`syslog.reachable` in `/status` is an ARP/ICMP hint. Check the collector for actual delivery.
[Diagnostic export](SECURITY.md#syslog-and-diagnostic-export)

## Troubleshooting

Inspect the web UI first, then `/status`, `/diag` and `/heap` as needed. Device requests require
access to your trusted LAN. Commands, pairing, update checks, verbose logging and artifact deletion
change state; distinguish them from reading an existing log or cached status.

| Symptom | Check | Next step |
|---|---|---|
| Update check always reports insufficient memory, or an update rolls back | Target RAM settings and running version | Follow [update memory recovery](#update-memory-recovery); preserve NVS and use a compatible signed image. |
| Installer has no serial port | Data cable, browser Web Serial support, port already in use | Close other serial programs or tabs; hold BOOT while connecting or use BOOT + RESET. |
| Installer: version not seen within 20 s | Adapter without reset lines, a native USB port that reconnected late, or an image that does not boot | The firmware is already on flash: press **Reset** or the board's RESET button, or reconnect the cable, and read the log. If it still does not start, reinstall with **Flash** rather than **Clean flash**. |
| `.local` will not load | DHCP lease in router | Open the current IP. Use that IP or `.local` for browser saves. |
| No WiFi | SSID/password, open or WPA2 network | Use setup AP or `provision.py`; do not write partial NVS images. Failed LAN credential changes roll back once. With saved credentials the device keeps retrying them and the setup AP closes after 10 minutes without a client (30 minutes at most); power-cycle to reopen it. |
| No pairing prompt | VIN configured, `key_present`, BLE range, NFC card on reader | Place the keycard on the reader and wait for the automatic same-key retry with backoff, then confirm on the touchscreen. |
| BLE connection fails | `/status.ble.connect_fail`, `car_connectable`, log cause | Move closer; disconnect another phone/fob if the car advertises non-connectable. |
| Command times out | BLE availability and vehicle state | Check whether the command took effect before retrying. [Command results](API.md#commands) |
| evcc shows the car as disconnected while it is plugged in | BLE range, `link` in `/status` | Bring the device back into Bluetooth range. An unreachable car reports `charging_state: Unknown`, which evcc maps to status A until the car answers again. With a charger that reports its own status, only the vehicle card and vehicle detection change; [other chargers](API.md#evcc-integration) end the session. |
| evcc has no SOC/current | `port: 80`, VIN, cache freshness | Inspect `.response.response.charge_state` from `vehicle_data`; active stale telemetry returns `503`. |
| Key needs attention | `reauth`, stored session and BLE availability | Reconnection and same-key pairing run automatically. Place the NFC card on the reader and confirm a pairing prompt on the touchscreen. No firmware erase is required. |
| MQTT save fails | `mqtt.error`; HTTP `400`, `502` or `503` | Check credentials, reachability or contiguous heap respectively. A failed probe saves nothing. |
| Syslog says unreachable | Collector DNS and UDP delivery | ICMP is only advisory; a collector blocking ping may still receive logs. |
| Restart or crash | `sys.reset_reason`, `last_crash`, `last_reboot`, heap trend | Retain evidence and match a dump to the same build's ELF before clearing it. |

### Web status and pairing recovery

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
