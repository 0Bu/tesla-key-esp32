#pragma once

class VehicleController;

// ─── On-device status display (LilyGo T-Dongle-S3 / -C5, 0.96" ST7735, 160x80) ─
// Renders the charge/connection state directly on the panel, in landscape:
//   • header: WiFi signal bars + SSID (left), Bluetooth symbol + BLE bars (right)
//   • body:   a battery filled to the SoC with a red→amber→green gradient; a
//             charging bolt overlays while charging (hidden at 100%); the asleep
//             state dims the fill ("ASLEEP") and the unreachable state shows an
//             empty shell ("OFFLINE").
// Mirrors tools/display_sim.py one-to-one (the offline pixel-exact renderer and
// font source of truth — regenerate display_font.h from it).
//
// Design constraints (match the rest of the firmware):
//   • Reads ONLY cached state via VehicleController::get_cached_charge() and the
//     ble_*/link_state accessors — it NEVER triggers a BLE round-trip, so it can
//     never wake the car or queue behind a poll in the single BLE FIFO.
//   • Independent of MQTT — needs no live MQTT session.
//   • Framebuffer lives in PSRAM where available (the T-Dongle-C5's 8 MB) so it
//     costs no internal SRAM; only a one-line bounce buffer is internal/DMA. On a
//     no-PSRAM board (T-Dongle-S3) it falls back to ~25 KB of internal SRAM — a
//     real cost on a RAM-tight build, so watch the post-init heap-attribution log.
//
// No-op unless CONFIG_TESLA_DISPLAY_ENABLED — so the default ESP32-S3 build (no
// panel) is completely unaffected.
void display_start(VehicleController& vehicle);
