#pragma once

class VehicleController;

// ─── On-device status display (LilyGo T-Dongle-C5, 0.96" ST7735, landscape 160x80) ─
// Renders the charge/connection state directly on the panel, in landscape:
//   • header: WiFi signal bars + SSID (left, scrolls horizontally if too long),
//             Bluetooth symbol + BLE bars (right)
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
//   • Framebuffer lives in the T-Dongle-C5's 8 MB PSRAM, so it costs no internal
//     SRAM; only a one-line bounce buffer is internal/DMA. If PSRAM is unavailable
//     the display disables itself (never steals scarce contiguous internal SRAM).
//
// Compiled only when CONFIG_TESLA_DISPLAY_ENABLED (set for esp32c5 in
// sdkconfig.defaults.esp32c5); a no-op stub on the other four targets, so one
// source tree serves every board. The C5 panel wiring is fixed in display_start().
void display_start(VehicleController& vehicle);
