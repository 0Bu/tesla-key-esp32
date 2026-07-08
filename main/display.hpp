#pragma once

class VehicleController;

// ─── On-device status display (LilyGo T-Dongle-C5 / T-Dongle-S3, ST7735 80x160) ──
// Renders the charge/connection state directly on the panel. The BOOT button rotates the panel
// 90° per press through 4 orientations — LANDSCAPE (160x80) and PORTRAIT (80x160), each plus its
// 180° flip — persisted in NVS (tesla_cfg/disp_rot). Landscape lays the header across the top and
// a HORIZONTAL battery below; portrait stacks a two-row header over a VERTICAL battery (filling
// bottom→top). Both draw the same content:
//   • header: WiFi signal bars + SSID (scrolls horizontally if too long),
//             Bluetooth symbol + BLE bars
//   • body:   a battery filled to the SoC with a red→amber→green gradient; a
//             charging bolt overlays while charging (hidden at 100%); the asleep
//             state dims the fill ("ASLEEP").
//   • searching/pairing: when a link isn't ready the battery is replaced, by
//             priority — WiFi search > pairing > battery > BLE search. A "search"
//             is a link label (the word "WiFi", or a Bluetooth glyph for BLE) plus
//             a compact bar cluster whose dark-green highlight ping-pongs across
//             light-green bars. The BLE search bars show ONLY when the car is out
//             of range; once a BLE link is up but not yet paired it shows a big
//             animated "Pairing…" instead. The header hides whichever small
//             indicator is the active search.
// Mirrors tools/display_sim.py one-to-one (the offline pixel-exact renderer and
// font source of truth — regenerate display_font.h from it).
//
// Design constraints (match the rest of the firmware):
//   • Reads ONLY cached state via VehicleController::get_cached_charge() and the
//     ble_*/link_state accessors — it NEVER triggers a BLE round-trip, so it can
//     never wake the car or queue behind a poll in the single BLE FIFO.
//   • Independent of MQTT — needs no live MQTT session.
//   • Framebuffer lives in PSRAM where present (the T-Dongle-C5's 8 MB), so it costs
//     no internal SRAM; only a one-line bounce buffer is internal/DMA. The T-Dongle-S3
//     build (no PSRAM) uses ~25 KB internal SRAM, allocated once at boot. On the C5, if
//     PSRAM is unavailable the display disables itself rather than steal that board's
//     scarce contiguous internal SRAM.
//
// Compiled only when CONFIG_TESLA_DISPLAY_ENABLED (set for esp32c5 AND esp32s3 in their
// sdkconfig.defaults.*); a no-op stub on the other targets, so one source tree serves every
// board. The per-board pins come from Kconfig; the wiring is applied in display_start(),
// which on esp32s3 first auto-detects the T-Dongle-S3 (a generic ESP32-S3 stays a no-op).
void display_start(VehicleController& vehicle);
