#pragma once

#include <cstdint>

#include "logic/link_state.hpp"
#include "logic/ui_state.hpp"

// Pure, hardware-free model for the on-device status LED (the single APA102 RGB pixel on the
// underside of the LilyGo T-Dongle-C5 / T-Dongle-S3). It maps the firmware's cached state to
// ONE colour + animation. It reads the SAME shared UiSnapshot the ST7735 presenter consumes
// (logic/display_model.hpp) — so the LED, the panel, the web-UI hero and MQTT never disagree
// about the car's state — plus a small set of LED-only LATCHED alert flags. led_status.cpp
// gathers the live state and calls led_pattern(); the host mock build tests the priority
// ladder here without a board (see test/test_logic.cpp). Keep this a pure function of its
// inputs: the sampling layer (led_status.cpp) owns latching/timing, this owns the mapping.
//
// It is a SINGLE pixel, so a spatially "running" wave is impossible — the wave the design
// calls for is temporal: LedAnim::Breathe (a sine brightness swell) reads as a pulsing wave
// on one spot. Errors blink (a hard on/off is a clearer alarm than a swell).
namespace tk {

// One physical pixel can only ever show one thing, so the mapping is a priority ladder:
// the most important condition wins. Colours carry UI-convention meaning (blue=network,
// green=healthy/charging, amber=warning, red=error); the animation is a second, colour-
// blind-safe channel (breathe vs pulse vs blink vs solid). SocGradient renders via the
// shared logic/soc_gradient.hpp ramp (led_status.cpp resolves the colour).
enum class LedColor { Off, Blue, Teal, Magenta, Green, Amber, Red, SocGradient };
enum class LedAnim  { Off, Solid, Breathe, Pulse, Blink };

struct LedPattern {
    LedColor color = LedColor::Off;
    LedAnim  anim  = LedAnim::Off;
    // Resting states (parked, known SoC) render at reduced brightness so the dongle isn't a
    // bright dot in a dark garage; alerts/searches/charging use full brightness. Asleep is
    // Off entirely (LedColor::Off). The concrete dim/full levels live in led_status.cpp.
    bool     dim   = false;
};

inline bool operator==(const LedPattern& a, const LedPattern& b) {
    return a.color == b.color && a.anim == b.anim && a.dim == b.dim;
}
inline bool operator!=(const LedPattern& a, const LedPattern& b) { return !(a == b); }

// LED-only LATCHED alert signals, derived by the sampling layer (led_status.cpp), which owns
// the timing — a transient fault is held visible for N seconds so a brief blip is seen. These
// are deliberately kept OUT of UiSnapshot: unlike the momentary shared facts the snapshot
// carries, they are stateful and specific to the LED, so they travel in their own tiny struct.
struct LedAlerts {
    bool error           = false;  // needs attention: reauth_required() OR OTA failed (latched)
    bool ota_downloading = false;  // OtaState::Downloading — self-update in flight, reboot imminent
    bool warn            = false;  // self-recovering: repeated BLE connect failures (latched)
};

// The priority ladder. Highest condition wins; see the per-line rationale. The shared facts
// come from UiSnapshot; the top three tiers are the LED-only latched alerts. Mirrors the
// display's centre-priority (WiFi search > pairing > battery > BLE search), extended with the
// error/OTA/asleep tiers the LED adds on top.
inline LedPattern led_pattern(const UiSnapshot& s, const LedAlerts& a) {
    // 1. Attention required — a re-pair is pending or an OTA install failed. Red blink is the
    //    only "you must do something" signal, so it outranks everything.
    if (a.error)                            return {LedColor::Red,     LedAnim::Blink,   false};
    // 2. OTA download in progress — the device is about to reboot into new firmware; make it
    //    obvious so nobody pulls power mid-write.
    if (a.ota_downloading)                  return {LedColor::Blue,    LedAnim::Pulse,   false};
    // 3. Warning that recovers on its own — e.g. the car is visible but every connect times
    //    out (at its ~3-BLE-device limit, or another controller holds the link).
    if (a.warn)                             return {LedColor::Amber,   LedAnim::Blink,   false};
    // 4. No LAN yet — connecting to / reconnecting WiFi (in the running app WiFi is the only
    //    thing that can be "not connected"; the setup portal runs before this task exists).
    if (!s.wifi_on)                         return {LedColor::Blue,    LedAnim::Breathe, false};
    // 5. BLE link up but no session — waiting for the NFC-card pairing to complete.
    if (s.ble_connected && !s.paired)       return {LedColor::Magenta, LedAnim::Pulse,   false};
    // 6. Charging — the primary evcc event; a green swell that's visible across the room.
    if (s.charging)                         return {LedColor::Green,   LedAnim::Breathe, false};
    // 7. Provably asleep — go dark (user choice: off when asleep). Debounced upstream, so a
    //    single ASLEEP blip won't flick the LED off.
    if (s.link_state == LinkState::Asleep)  return {LedColor::Off,     LedAnim::Off,     false};
    // 8. Parked with a known state of charge — a calm, dimmed SoC colour (red→amber→green).
    //    Full charge reads as steady green. Only when the car is actually reachable/awake;
    //    a stale cache while Unreachable must not masquerade as a live reading.
    if (s.have_soc && (s.link_state == LinkState::Awake || s.link_state == LinkState::Idle)) {
        if (s.soc >= 100)                   return {LedColor::Green,       LedAnim::Solid, true};
        return {LedColor::SocGradient,      LedAnim::Solid, true};
    }
    // 9. Everything else — no live reading yet (Unreachable / Unknown / no SoC): searching for
    //    the car over BLE. Same teal swell whether pre-pair "connecting…" or a parked car out
    //    of range, matching the display's BLE-search bars.
    return {LedColor::Teal, LedAnim::Breathe, false};
}

}  // namespace tk
