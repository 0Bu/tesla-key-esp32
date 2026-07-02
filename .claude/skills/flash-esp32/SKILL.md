---
name: flash-esp32
description: Build and USB-flash the tesla-key-esp32 firmware (ESP32 / S3 / C3 / C6) over the serial port. Use when asked to "flash", "flashe", "flash the device", deploy firmware/web-UI changes to the physical board over USB, or reflash after editing main/. Defaults to esp32s3; set TARGET for other chips. Auto-detects the serial port and preserves NVS (pairing/key/VIN). For pull-based OTA updates instead, see docs / the web UI version tap.
---

# flash-esp32 — build & USB-flash the firmware

Builds the ESP-IDF project (in Docker) and flashes it to a connected **ESP32 board**
(esp32 / esp32s3 / esp32c3 / esp32c6 — set `TARGET`, default esp32s3) over
USB (from the host). NVS is left untouched, so the stored pairing, private key, VIN and
WiFi survive the flash (no re-pair needed). Use this after editing anything under `main/` —
including the embedded web UI (`main/www/` — `index.html` + `style.css` + `app.js`, spliced
into one page at build time), which is compiled into the app binary.

> This flashes over **USB**. For a remote, no-cable update use OTA (tap the firmware
> version in the web UI). USB flashing requires physical access and the board plugged in.

## Why two halves (Docker build, host flash)

There is **no local ESP-IDF install** — builds run via `scripts/idf-docker.sh`, which runs
the official `espressif/idf` Docker image **pinned to the exact version CI uses** (read at
runtime from `.github/workflows/build.yml`, so it never drifts; a new version auto-pulls on
first use). But **Docker Desktop on macOS has no USB passthrough**, so the *flash* step runs
on the **host** with `esptool` (`brew install esptool`). Build → produces `build/`, then
flash `build/` from the host.

## One-shot command

Run from the **repo root** (where `CMakeLists.txt` lives). Builds in Docker, then — only if
the build succeeded (`pipefail` + the `||` guard) — auto-detects the port and flashes:

```bash
set -o pipefail
TARGET=esp32s3   # chip being flashed: esp32s3 (default) | esp32 | esp32c3 | esp32c6
# 1) Build via the CI-pinned ESP-IDF Docker image (build/ stays host-owned).
#    First build only: set-target; afterwards plain `build` keeps it incremental & fast.
scripts/idf-docker.sh \
  sh -c "if [ -f sdkconfig ]; then idf.py build; else idf.py set-target $TARGET build; fi" \
  2>&1 | tail -15 || { echo "BUILD FAILED — not flashing"; exit 1; }
# 2) Flash from the HOST (Docker can't reach USB). @flash_args writes the bootloader (at the
#    target's own offset — 0x1000 on the classic esp32, 0x0 on s3/c3/c6), partition-table@0x8000,
#    otadata@0xf000, app@0x20000 — NOT nvs@0x9000, so pairing survives.
PORT=$(ioreg -l -w 0 2>/dev/null | grep -iE '"USB Product Name"|"IOCalloutDevice"' \
       | grep -iA1 '"USB Single Serial"' | grep -m1 -o '/dev/cu\.usbmodem[^"]*') \
  && echo "Flashing via $PORT" \
  && ( cd build && esptool --chip "$TARGET" -p "$PORT" -b 460800 \
        --before default_reset --after hard_reset write_flash "@flash_args" ) 2>&1 | tail -20
```

> **Port detection above targets S3/C3/C6 boards** (native USB = `/dev/cu.usbmodem*`). The
> **classic esp32** has no native USB — it appears as a USB-UART bridge `/dev/cu.usbserial-*`
> (CP210x/CH340), so for `TARGET=esp32` set `PORT=$(ls /dev/cu.usbserial-* | head -1)` instead.

**Success looks like:** `Hash of data verified.` for each region, then
`Hard resetting via RTS pin...` → `Done`. The app image lands at `0x20000` (dual-OTA
layout). After reset the device rejoins WiFi in a few seconds; reload
`tesla-key-esp32.local` to see UI changes.

## Picking the right serial port

This board exposes **two** USB serial interfaces — confirm before flashing:

| `/dev` node (example)        | USB product name              | What it is                     |
|------------------------------|-------------------------------|--------------------------------|
| `/dev/cu.usbmodem<SERIAL>`   | **USB Single Serial** (WCH)   | UART bridge — **use this one** |
| `/dev/cu.usbmodem<NNNN>`     | USB JTAG/serial debug unit    | S3/C3/C6 native USB (also works) |

The exact node name is device/cable-specific — **never hardcode it**; detect at runtime.
List both with their product names:

```bash
ioreg -l -w 0 2>/dev/null | grep -iE '"USB Product Name"|"IOCalloutDevice"' \
  | grep -iB1 usbmodem | sed -E 's/^[ |]+//'
```

Both interfaces can flash an S3. The one-shot command above targets the **WCH UART
bridge** ("USB Single Serial"), which is the conventional choice. If only the native
JTAG unit is present, target that node instead. If the `grep` finds nothing, the board
isn't connected (or is asleep) — check the cable, or drop `-p "$PORT"` to let esptool
auto-detect.

## Notes & gotchas

- **No local IDF** — every `idf.py` step goes through `scripts/idf-docker.sh`, which uses the
  `espressif/idf` image **pinned to the CI version** (from `.github/workflows/build.yml`); a
  new version auto-pulls on first use. The mounted `build/` dir persists on the host, so
  Docker builds stay incremental. Run `idf.py` ad-hoc the same way, e.g.
  `scripts/idf-docker.sh idf.py size`.
- **Don't run a serial monitor in an automated session** — it never returns and hangs the
  turn. Flash without it. `idf.py monitor` isn't available on the host; for serial logs use a
  host terminal: `screen /dev/cu.usbmodemXXXX 115200` (exit `Ctrl-A` then `K`), or
  `pipx install esp-idf-monitor` → `esp-idf-monitor -p <PORT>`.
- **First build only** is slow (managed_components fetch + full compile). A `build/` dir
  already present means subsequent flashes are incremental and fast.
- **NVS is preserved** by `@flash_args` (it never touches `nvs@0x9000`). To wipe
  pairing/key/VIN/WiFi instead, run `esptool --chip "$TARGET" -p <PORT> erase_flash` (forces a
  full re-pair afterwards).
- **Artifacts are host-owned** thanks to `-u $(id -u):$(id -g)` — no root-owned files in the
  worktree. `build/`, `managed_components/`, `sdkconfig` are all gitignored.

## After flashing

To confirm the live device is healthy (paired, BLE up, no evcc timeouts), run the
[`e2e-evcc`](../e2e-evcc/SKILL.md) skill.
