#pragma once

#include <cstdint>
#include "logic/link_state.hpp"

// Immutable, hardware-free snapshot of everything the on-device status indicators need
// to decide what to show — the ST7735 status display (logic/display_model.hpp) and, in
// future, the APA102 status LED (logic/led_status.hpp). It is the single INPUT CONTRACT
// shared by every presenter, so the display and the LED can never disagree about the car's
// state: they read ONE struct instead of each racing the controller's accessors
// independently (which was the "4th/5th sink of link_state()" drift hazard). Pure data —
// no IDF types, no std::string, no heap — so every presenter that consumes it is host-tested
// in test/ without a board (the same reason logic/link_state.hpp exists).
//
// WHO FILLS WHAT — the snapshot is ASSEMBLED at the presentation seam, from more than one
// source, so the vehicle core stays free of WiFi/NVS concerns:
//   • VehicleController::ui_snapshot() fills the vehicle-owned cache fields (link_state /
//     ble_* / soc / charging) in ONE read under its cache lock — instead of a presenter
//     calling five separate accessors across a frame and mixing state from different instants.
//   • The caller (the display / LED task) fills `paired` from its own ≤1 Hz has_session()
//     sample — that call hits NVS, so it must NOT run every frame — and the wifi_* fields
//     from esp_wifi. Neither is the controller's concern.
namespace tk {

struct UiSnapshot {
    // ── vehicle-owned (VehicleController::ui_snapshot(), under cache_mutex_) ──
    LinkState link_state     = LinkState::Unknown;
    bool      ble_connected  = false;
    bool      ble_rssi_valid = false;   // a live-link RSSI reading is available
    int       ble_rssi       = 0;       // dBm, valid iff ble_rssi_valid
    bool      have_soc       = false;   // charge cache valid AND battery level present
    int       soc            = 0;       // battery %, RAW (rounded, NOT yet clamped 0..100)
    bool      charging       = false;   // charging_state == "Charging"

    // ── pairing (caller, sampled at ≤1 Hz — has_session() hits NVS) ──
    bool      paired         = false;

    // ── WiFi (caller, from esp_wifi at the seam) ──
    bool      wifi_on        = false;   // STA holds an IP AND the ap-info read succeeded
    int       wifi_rssi      = 0;       // dBm
    char      ssid[33]       = {0};     // NUL-terminated (max 32 + NUL, per 802.11)
};

}  // namespace tk
