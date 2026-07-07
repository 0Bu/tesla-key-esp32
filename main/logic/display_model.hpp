#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>
#include "logic/ui_state.hpp"
#include "logic/link_state.hpp"

// Pure, hardware-free PRESENTER for the ST7735 status display — the decision half of the
// former display.cpp compose(): from a UiSnapshot it produces a Model that says WHAT to
// show (which centre hero, which header indicators, the battery fill colour, the SSID
// marquee offset, whether to keep animating). The renderer (main/display.cpp) then becomes
// a thin driver that only DRAWS a Model — no branching on vehicle state, no reach into
// VehicleController. Keeping the decision here (IDF-free) lets test/ host-test the priority
// ladder, the SoC gradient, the RSSI→bars thresholds and the SSID scroll ping-pong in
// seconds without a board, and pins the layout geometry as the single source of truth that
// main/display.cpp and tools/display_sim.py both mirror (regenerate the sim from these).
namespace tk {
namespace display {

// ── layout geometry (mirrors main/display.cpp + tools/display_sim.py) ──
inline constexpr int kPanelRight   = 158;  // right edge used for the header layout
inline constexpr int kSsidX        = 28;   // SSID text left edge
inline constexpr int kBleReserve   = 32;   // px the BLE header steals from the SSID row
inline constexpr int kSsidScale    = 2;    // SSID glyph scale
inline constexpr int kGlyphAdvance = 6;    // px advanced per char at scale 1 (5px + 1 gap)

// ── SSID marquee tuning (mirrors scroll_offset() in display.cpp / display_sim.py) ──
inline constexpr int kScrollPause = 8;     // ticks paused at each end
inline constexpr int kScrollSpeed = 2;     // px per tick

// ── asleep dim target: the battery fill is blended 50% toward the panel colour. Must equal
// C_PANEL's raw RGB in display.cpp (rgb565(26,32,45)) so the dimmed colour matches. ──
inline constexpr int kAsleepDimR = 26, kAsleepDimG = 32, kAsleepDimB = 45;

// Text width in px at an integer scale (font is 5px wide + 1px gap = 6 px/char).
inline int text_w(const char* s, int scale) {
    return static_cast<int>(std::strlen(s)) * kGlyphAdvance * scale;
}

// RSSI (dBm) → 0..4 signal bars. Shared by the WiFi and BLE header indicators.
inline int rssi_bars(int rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

inline int lerp8(int a, int b, float t) { return a + static_cast<int>(std::lroundf((b - a) * t)); }

// SoC 0..100 → red→amber→light-green→green→deep-green (mirrors soc_rgb() in display.cpp).
struct GradStop { float p; uint8_t r, g, b; };
inline constexpr GradStop kGrad[] = {
    {0.00f, 231,  76,  60}, {0.18f, 240, 190,  40}, {0.45f, 120, 200,  90},
    {0.80f,  60, 175,  80}, {1.00f,  30, 140,  60},
};
inline void soc_rgb(int soc, int& r, int& g, int& b) {
    float p = soc <= 0 ? 0.0f : (soc >= 100 ? 1.0f : soc / 100.0f);
    const int n = static_cast<int>(sizeof(kGrad) / sizeof(kGrad[0]));
    for (int i = 0; i < n - 1; ++i) {
        if (p <= kGrad[i + 1].p) {
            float span = kGrad[i + 1].p - kGrad[i].p;
            float t = span <= 0 ? 0.0f : (p - kGrad[i].p) / span;
            r = lerp8(kGrad[i].r, kGrad[i + 1].r, t);
            g = lerp8(kGrad[i].g, kGrad[i + 1].g, t);
            b = lerp8(kGrad[i].b, kGrad[i + 1].b, t);
            return;
        }
    }
    r = kGrad[n - 1].r; g = kGrad[n - 1].g; b = kGrad[n - 1].b;
}

// Horizontal marquee offset (0..span) for an over-long SSID: ping-pong pause / scroll-out /
// pause / scroll-back so the whole name is readable. Verbatim from display.cpp scroll_offset().
inline int scroll_offset(uint32_t tick, int span) {
    if (span <= 0) return 0;
    int travel = (span + kScrollSpeed - 1) / kScrollSpeed;   // ticks to cover the span
    int period = 2 * kScrollPause + 2 * travel;
    int p = static_cast<int>(tick % static_cast<uint32_t>(period));
    if (p < kScrollPause) return 0;                          // pause at start
    if (p < kScrollPause + travel) {                         // scroll out
        int o = (p - kScrollPause) * kScrollSpeed;
        return o < span ? o : span;
    }
    if (p < 2 * kScrollPause + travel) return span;          // pause at end
    int o = span - (p - 2 * kScrollPause - travel) * kScrollSpeed;  // scroll back
    return o > 0 ? o : 0;
}

// Which big centre element is shown, by priority: WiFi search > pairing > BLE search >
// battery. (WiFi/BLE "search" = the ping-pong sweep; pairing = the animated "Pairing…".)
enum class Hero : uint8_t { WifiSearch, Pairing, BleSearch, Battery };

// The fully-decided frame. The renderer draws exactly this — it branches on nothing else.
struct Model {
    Hero hero = Hero::BleSearch;

    // header — WiFi side (bars + SSID). Hidden while WiFi is the active search hero.
    bool show_wifi       = false;
    int  wifi_bars       = 0;      // 0..4
    bool ssid_scrolling  = false;  // SSID longer than its row → marquee
    int  ssid_scroll_off = 0;      // px to shift the SSID left (0 unless scrolling)
    int  ssid_avail      = 0;      // clip-window width in px

    // header — BLE side (bars + BT glyph). Hidden while BLE is the active search hero.
    bool show_ble        = false;
    int  ble_bars        = 0;      // 0..4 (0 when no live RSSI)
    bool ble_glyph_on    = false;  // BT glyph bright (connected) vs grey

    // battery body — meaningful only when hero == Battery.
    int     soc       = 0;         // 0..100 (clamped)
    uint8_t fill_r    = 0, fill_g = 0, fill_b = 0;   // post-asleep-dim fill (raw RGB)
    bool    asleep    = false;     // dim fill + "ASLEEP" label
    bool    show_bolt = false;     // charging bolt overlay

    bool animating = false;        // a search/pairing hero OR a scrolling SSID
};

// Compose one frame from a snapshot. `tick` is a monotonic frame counter driving the search
// sweep, the pairing dots and the SSID marquee. Pure — no side effects, no I/O — a faithful
// extraction of display.cpp compose()'s decision half.
inline Model compose(const UiSnapshot& s, uint32_t tick) {
    Model m;

    const bool unreachable    = (s.link_state == LinkState::Unreachable);
    const bool wifi_searching = !s.wifi_on;
    const bool pairing        = s.ble_connected && !s.paired;   // BLE up, not yet paired
    const bool battery_ok     = !unreachable && s.have_soc;
    // BLE search bars appear ONLY when nothing else claims the centre (car out of range).
    const bool ble_bars       = !(wifi_searching || pairing || battery_ok);

    if      (wifi_searching) m.hero = Hero::WifiSearch;
    else if (pairing)        m.hero = Hero::Pairing;
    else if (!battery_ok)    m.hero = Hero::BleSearch;
    else                     m.hero = Hero::Battery;

    // ── header: WiFi bars + SSID (hidden when WiFi is the search hero) ──
    m.show_wifi = !wifi_searching;
    if (m.show_wifi) {
        m.wifi_bars = rssi_bars(s.wifi_rssi);
        const int avail = ble_bars ? (kPanelRight - kSsidX)
                                   : (kPanelRight - kSsidX - kBleReserve);
        m.ssid_avail = avail;
        const int tw = text_w(s.ssid, kSsidScale);
        if (tw > avail) {                          // too long → horizontal marquee
            m.ssid_scrolling  = true;
            m.ssid_scroll_off = scroll_offset(tick, tw - avail);
        }
    }
    // ── header: BLE bars + BT glyph (hidden when BLE is the search hero) ──
    m.show_ble = !ble_bars;
    if (m.show_ble) {
        m.ble_bars     = s.ble_rssi_valid ? rssi_bars(s.ble_rssi) : 0;
        m.ble_glyph_on = s.ble_connected;
    }

    // ── battery body ──
    if (m.hero == Hero::Battery) {
        int soc = s.soc;
        if (soc < 0)   soc = 0;
        if (soc > 100) soc = 100;
        m.soc    = soc;
        m.asleep = (s.link_state == LinkState::Asleep);
        int r, g, b;
        soc_rgb(soc, r, g, b);
        if (m.asleep) {                            // blend 50% toward the panel colour
            r = lerp8(r, kAsleepDimR, 0.5f);
            g = lerp8(g, kAsleepDimG, 0.5f);
            b = lerp8(b, kAsleepDimB, 0.5f);
        }
        m.fill_r = static_cast<uint8_t>(r);
        m.fill_g = static_cast<uint8_t>(g);
        m.fill_b = static_cast<uint8_t>(b);
        m.show_bolt = s.charging && soc < 100 && !m.asleep;
    }

    // Search/pairing heroes animate continuously; the battery view only needs fast refresh
    // while the SSID marquee is moving.
    m.animating = (m.hero != Hero::Battery) || m.ssid_scrolling;
    return m;
}

}  // namespace display
}  // namespace tk
