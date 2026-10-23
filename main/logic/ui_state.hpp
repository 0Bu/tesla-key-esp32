#pragma once

#include <cstdint>
#include "logic/link_state.hpp"
#include "logic/net_link.hpp"

// Immutable, hardware-free snapshot of everything the on-device status indicators need
// to decide what to show — the ST7735 status display (logic/display_model.hpp) and
// the APA102 status LED (logic/led_status.hpp). Each presenter receives one coherent
// vehicle snapshot rather than separately sampling controller accessors. Tasks can sample at
// different times; sharing this input structure does not synchronize their rendered frames.
// Pure data —
// no IDF types, no std::string, no heap — so every presenter that consumes it is host-tested
// in test/ without a board (the same reason logic/link_state.hpp exists).
//
// WHO FILLS WHAT — the snapshot is ASSEMBLED at the presentation seam, from more than one
// source, so the vehicle core stays free of WiFi/NVS concerns:
//   • VehicleController::ui_snapshot() fills the vehicle-owned cache fields (link_state /
//     ble_* / soc / charging) in ONE read under its cache lock — instead of a presenter
//     calling five separate accessors across a frame and mixing state from different instants.
//   • The caller (the display / LED task) fills the `paired` presentation input from its own ≤1 Hz durable-record
//     or usable-RAM sample (public `paired` keeps its durable meaning), and the wifi_* fields
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
    bool      charging       = false;   // fresh ChargeState says "Charging" while link is Awake

    // ── pairing presentation (caller, ≤1 Hz): durable presence OR usable VCSEC RAM ──
    bool      paired         = false;

    // ── network (caller, from tk::net_* at the seam) ──
    // `net` is the TRANSPORT, not a boolean: an Ethernet board (M5Stack ATOMIC PoE Base,
    // W5500 over SPI) holds an IP with no radio, so "up" and "WiFi" stopped being the same
    // sentence. RSSI and an SSID exist only for NetLink::Wifi and are meaningless otherwise —
    // presenters must branch on `net`, never read the two fields unconditionally.
    NetLink   net            = NetLink::None;  // which transport holds the lease
    int       wifi_rssi      = 0;              // dBm,             iff net == NetLink::Wifi
    char      ssid[33]       = {0};            // NUL-terminated,  iff net == NetLink::Wifi

    // Does this board HAVE an Ethernet interface at all? Decided once at boot, not per frame.
    // It is what lets the SEARCHING state name the right thing: with no link yet there is no
    // transport to read, and a PoE board that displays "WiFi …" while it waits for DHCP is
    // simply lying. False on every board without the Ethernet backend, which keeps the
    // rendering byte-identical there.
    bool      eth_present    = false;

    // True while any transport carries the lease. The successor to the old `wifi_on`.
    bool net_up() const { return net != NetLink::None; }
};

}  // namespace tk
