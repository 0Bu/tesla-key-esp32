---
name: flash-esp32
description: Build and USB-flash the tesla-key-esp32 firmware (ESP32-S3) over the serial port. Use when asked to "flash", "flashe", "flash the device", deploy firmware/web-UI changes to the physical board over USB, or reflash after editing main/. Auto-detects the serial port and preserves NVS (pairing/key/VIN). For pull-based OTA updates instead, see docs / the web UI version tap.
---

# flash-esp32 — build & USB-flash the firmware

Builds the ESP-IDF project and flashes it to the connected **ESP32-S3** over USB. NVS
is left untouched, so the stored pairing, private key, VIN and WiFi survive the flash
(no re-pair needed). Use this after editing anything under `main/` — including the
embedded web UI (`main/www/index.html`), which is compiled into the app binary.

> This flashes over **USB**. For a remote, no-cable update use OTA (tap the firmware
> version in the web UI). USB flashing requires physical access and the board plugged in.

## One-shot command

ESP-IDF is **not** on `PATH` by default — it must be sourced in the *same* shell as
`idf.py` (shell state doesn't persist between separate tool calls). Detect the port and
flash in a single command:

```bash
source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && \
PORT=$(ioreg -l -w 0 2>/dev/null | grep -iE '"USB Product Name"|"IOCalloutDevice"' \
       | grep -iA1 '"USB Single Serial"' | grep -m1 -o '/dev/cu\.usbmodem[^"]*') && \
echo "Flashing via $PORT" && \
idf.py -p "$PORT" flash 2>&1 | tail -20
```

`idf.py flash` builds first if anything changed (incremental — a web-UI-only edit is a
quick recompile, not a full clean build), then writes the app image.

**Success looks like:** `Hash of data verified.` for each region, then
`Hard resetting via RTS pin...` → `Done`. The app image lands at `0x20000` (dual-OTA
layout). After reset the device rejoins WiFi in a few seconds; reload
`tesla-key-esp32.local` to see UI changes.

## Picking the right serial port

This board exposes **two** USB serial interfaces — confirm before flashing:

| `/dev` node (example)        | USB product name              | What it is                     |
|------------------------------|-------------------------------|--------------------------------|
| `/dev/cu.usbmodem<SERIAL>`   | **USB Single Serial** (WCH)   | UART bridge — **use this one** |
| `/dev/cu.usbmodem<NNNN>`     | USB JTAG/serial debug unit    | ESP32-S3 native USB (also works) |

The exact node name is device/cable-specific — **never hardcode it**; detect at runtime.
List both with their product names:

```bash
ioreg -l -w 0 2>/dev/null | grep -iE '"USB Product Name"|"IOCalloutDevice"' \
  | grep -iB1 usbmodem | sed -E 's/^[ |]+//'
```

Both interfaces can flash an S3. The one-shot command above targets the **WCH UART
bridge** ("USB Single Serial"), which is the conventional choice. If only the native
JTAG unit is present, target that node instead. If the `grep` finds nothing, the board
isn't connected (or is asleep) — check the cable, or just run `idf.py flash` with no
`-p` to let esptool auto-detect.

## Notes & gotchas

- **Don't run `monitor` in an automated session** — it never returns and hangs the turn.
  Flash without it. If serial logs are needed, run `idf.py -p <PORT> monitor` separately
  and stop it with `Ctrl-]`.
- **First build only** is slow (managed_components fetch + full compile). A `build/` dir
  already present means subsequent flashes are incremental and fast.
- **Target** is `esp32s3` (already set in `sdkconfig`). If `sdkconfig` is missing, run
  `idf.py set-target esp32s3` first.
- **NVS is preserved** by a normal `flash`. To wipe pairing/key/VIN/WiFi instead, use
  `idf.py -p <PORT> erase-flash` (forces a full re-pair afterwards).
- ESP-IDF install path here is `~/esp/esp-idf`. If it lives elsewhere, source that
  copy's `export.sh`.

## After flashing

To confirm the live device is healthy (paired, BLE up, no evcc timeouts), run the
[`e2e-evcc`](../e2e-evcc/SKILL.md) skill.
