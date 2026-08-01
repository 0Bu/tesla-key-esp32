# ADR-0004: Drop the esp32c5 target

**Status:** accepted
**Date:** 2026-08-01
**Supersedes:** [`0001-esp32c5-target-upstreaming.md`](0001-esp32c5-target-upstreaming.md)
**Relates to:** [`../ARCHITECTURE.md`](../ARCHITECTURE.md) §"Pinned tesla-ble with one build-time patch"

## Context

esp32c5 was never a target the crypto library supported. tesla-ble v5.1.1 declares
`targets: [esp32, esp32s3, esp32c3, esp32c6]`, the ESP-IDF Component Manager enforces that list
at dependency resolution, and this project shipped the C5 anyway by cloning the pinned tag into
`third_party/tesla-ble` and appending one line to its manifest
(`scripts/prepare-tesla-ble-c5.sh` + mutually-exclusive `rules:` in `main/idf_component.yml`).
ADR-0001 recorded that as standing debt with a retirement plan: upstream the manifest line, then
delete the workaround. The upstream PR was never merged, so the debt stayed.

Two things then came due at once.

**The C5 ran out of flash.** It was always the largest image — it alone carried the on-device
display *and* PSRAM — and it cleared the `ci-build-all.sh` size gate by about 28 KB. Adding the
crash-forensics and recovery work (#215) pushed its content to **1 998 178 B**, past the
`0x1e0000` 64 KB Secure-Boot boundary, so the padded image became `0x1f0000` — *exactly* the OTA
slot — and the 4 KB signature that gets appended after padding made the signed image
`0x1f1000`, **4 KB larger than the partition it has to live in**. ESP-IDF's own
`check_sizes.py` reported `0x0 bytes (0%) free` before signing. Measured, not estimated: 32 098 B
over. Disabling the coredump component alone was measured too, and does **not** recover it — the
growth is spread across the change, not concentrated in one component.

**The remaining options all cost more than the target is worth.** Shrinking ~32 KB out of the C5
means levers this project has already spent (Package A, #154) or explicitly banned (whole-build
`-Os` hard-freezes under evcc+BLE load — rejected Package B). Enlarging the OTA slots moves
`ota_1`'s offset, which is the one change an OTA structurally cannot deliver: a device writes the
address its *installed* table names and boots another. Keeping the C5 on a reduced feature set
forks the firmware per chip, which is the property this project has most deliberately avoided.

## Decision

**Drop esp32c5.** Supported targets are exactly what yoziru/tesla-ble declares:
**esp32, esp32s3, esp32c3, esp32c6**.

Removed: `scripts/prepare-tesla-ble-c5.sh`, `sdkconfig.defaults.esp32c5`, the dual `rules:`
routing in `main/idf_component.yml` (back to one plain git dependency), the `third_party/`
gitignore entry, the C5 branches in `main/display.cpp`, the C5-only
`CONFIG_TESLA_WIFI_PREFER_5G` Kconfig option and its `main.cpp` block, and the `-c5` image
suffix / `ESP32-C5` platform string across `logic/target.hpp`, `platform.hpp`,
`ota_update.cpp`, `ci-build-all.sh`, `build-pages.sh` and the web installer.

The Component Manager's `targets:` enforcement is now treated as **the definition of
supported** rather than an obstacle to route around. Adding a chip it omits (esp32c5,
esp32c61) means upstreaming it there first.

## Consequences

- The supported chip list cannot drift from the crypto library's own, and a tesla-ble bump no
  longer carries a re-verify-the-patch step. The C5 workaround was the only reason
  `third_party/` existed.
- The size gate is no longer the binding constraint on every feature: the largest image is now
  esp32c6 at `0x1d1000` signed, ~92 KB under the gate, and esp32s3 — which still carries the
  display — sits at `0x191000`, ~360 KB under. The pressure that made #215 undeliverable is gone.
- **A T-Dongle-C5 already running this firmware keeps running it, and stops receiving updates.**
  No manifest entry, no `tesla-key-esp32-c5.bin`, so `/ota/check` finds nothing; the device is
  not bricked and not modified. Re-adding the target later means reverting this ADR's changes and
  finding the ~32 KB — the display support itself (`display.cpp`, the host-tested presenter, the
  four-orientation BOOT rotation) is shared with the T-Dongle-S3 and stays.
- The on-device display and status LED remain, on the T-Dongle-S3.
