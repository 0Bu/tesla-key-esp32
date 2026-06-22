#pragma once

#include <cstdint>

class VehicleController;

// Runtime panel wiring for one board. Chosen at boot from the NVS `board` key via
// display_board_preset(), so a SINGLE firmware image drives the panel on a board that
// has one and is a no-op on a board that doesn't — no per-board build or OTA channel.
struct DisplayConfig {
    bool    enabled       = false;        // false → display_start() is a no-op
    int     mosi = -1, sck = -1, cs = -1, dc = -1, rst = -1, bl = -1;   // SPI + control GPIOs
    bool    bl_active_low = false;        // drive backlight LOW to light it
    int     x_off = 0, y_off = 0;         // ST7735 RAM offsets (landscape col/row start)
    uint8_t madctl = 0x00;                // memory-access / rotation byte
};

// Preset wiring for a known board ("t-dongle-s3"); unknown/"generic" → {enabled=false}.
DisplayConfig display_board_preset(const char* board);

// Auto-detect the board from a hardware signature: returns "t-dongle-s3" if the LilyGo
// TF-card SDMMC pull-ups are present, else "generic". The only board selector — there is
// no manual override. See the implementation for the probe. Safe to call once at boot.
const char* display_detect_board();

// ─── On-device status display (LilyGo T-Dongle-S3 / -C5, 0.96" ST7735, 160x80) ─
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
//   • Framebuffer lives in PSRAM where available (the T-Dongle-C5's 8 MB) so it
//     costs no internal SRAM; only a one-line bounce buffer is internal/DMA. On a
//     no-PSRAM board (T-Dongle-S3) it falls back to ~25 KB of internal SRAM — a
//     real cost on a RAM-tight build, so watch the post-init heap-attribution log.
//
// Always compiled. With cfg.enabled == false (a panel-less board, e.g. "generic")
// this is a no-op and costs no SRAM — so one image serves every ESP32-S3 board. Pass
// display_board_preset(<nvs board>).
void display_start(VehicleController& vehicle, const DisplayConfig& cfg);
