#pragma once

#include <cstdint>

// Pure, hardware-free model of "which transport carries this device's IP, and when has that
// transport gone quiet without saying so". Two things live here, both previously implicit in
// main.cpp's WiFi code and therefore untestable:
//
//   1. NetLink — the transport identity. Until this existed the firmware had exactly one
//      network predicate, `wifi_is_connected()`, forward-declared by hand in five modules;
//      "the link is up" and "WiFi is up" were the same sentence. They are not: an Ethernet
//      board (M5Stack ATOMIC PoE Base, W5500 over SPI) holds an IP with no radio at all, and
//      the presenters have to say something other than "0 bars" about it.
//
//   2. watch_step() — the ghost-association state machine the connectivity watchdog runs.
//      Its subtlety is not the counting but the BASELINE rule: a gateway that has never
//      answered ICMP (a router that drops LAN echo) must never be read as "link dead", or a
//      perfectly healthy device re-associates every ~60 s forever. That rule was a two-line
//      `if` buried in a task loop; here it is a function with cases in test/test_logic.cpp.
//
// Pure data + pure functions — no IDF types, no heap — so both are host-tested without a
// board, the same contract logic/link_state.hpp and logic/ui_state.hpp hold.
namespace tk {

// Which transport currently carries the default route. `None` means "no lease" — the state
// the display renders as a search animation and the LED as a blue breathe; it is NOT an
// error, it is the normal first seconds of a boot.
//
// Deliberately not a bool: the presenters need to tell WiFi from Ethernet (RSSI and an SSID
// exist only for the former), and /status has to name the transport for a remote triage.
enum class NetLink : uint8_t { None = 0, Wifi = 1, Eth = 2 };

inline const char* net_link_str(NetLink k) {
    switch (k) {
        case NetLink::Wifi: return "wifi";
        case NetLink::Eth:  return "eth";
        default:            return "none";
    }
}

// Which transport carries the route when the two lease flags are known. BOTH can be held at
// once: a board whose W5500 finds no lease at boot falls back to WiFi with the Ethernet driver
// still running, so a cable plugged in later brings the wire up ALONGSIDE the radio.
//
// Ethernet wins whenever it has a lease — it is the transport that costs the BLE radio nothing,
// and it is what lwIP puts first, so reporting anything else would disagree with where the
// packets actually go.
//
// This is a function, not two lines at a call site, because the "last event wins" version it
// replaces had a real hole: unplugging that cable cleared the link for EVERYTHING above the
// transport seam — syslog stopped, the display showed "searching", MQTT dropped the RSSI —
// while a perfectly healthy WiFi lease was still in hand. A rule with two inputs and three
// outcomes belongs where it can be enumerated.
inline NetLink net_link_active(bool eth_lease, bool wifi_lease) {
    if (eth_lease)  return NetLink::Eth;
    if (wifi_lease) return NetLink::Wifi;
    return NetLink::None;
}

// ── Connectivity watchdog ────────────────────────────────────────────────────
// The watchdog exists for ONE failure mode the event-driven reconnect path cannot see: a
// missed deauth (WiFi) or a silently dead switch port / unplugged patch lead whose PHY still
// reports link (Ethernet). In both the stack believes it is connected, keeps the IP, keeps
// emitting TCP that times out — and NO disconnect event ever fires, so nothing reacts. The
// only evidence available is L3: does the default gateway still answer ICMP?
//
// It must never act on its own inability to measure. Two guards enforce that, and they are
// the whole reason this is a function rather than an `if`:
//   • the caller passes gw_ok = true whenever the probe could not even be set up, and
//   • gw_ever_ok gates recovery on a gateway that has answered at least once this boot.

enum class WatchAction : uint8_t {
    Idle,        // link down (the reconnect handler owns it) or the gateway answered — nothing to do
    Wait,        // a check failed, but not enough consecutive ones yet to act
    NoBaseline,  // enough failures, but this gateway has NEVER answered ICMP — refuse to act
    Recover,     // proven ghost link: force the transport to re-establish
};

// Consecutive-failure counter. One per watchdog task; reset on every healthy sample so only
// an UNBROKEN run of failures counts (a single dropped echo on a busy LAN is not evidence).
struct LinkWatch {
    int fails = 0;
};

// Consecutive failed checks before the watchdog acts (~2 × the 30 s check cadence = 60 s).
// Sized so a router reboot or a brief RF outage rides through untouched; the cost of acting
// too eagerly is a needless re-association, which drops every open connection.
inline constexpr int kWatchFailsToRecover = 2;

// One watchdog sample.
//   link_up    — the transport says it holds a lease (the ghost case is link_up == true by
//                definition; when it is false the reconnect handler already owns recovery).
//   gw_ok      — the gateway answered ≥1 echo, OR the probe could not be run at all.
//   gw_ever_ok — this gateway has answered at least once since boot.
inline WatchAction watch_step(LinkWatch& w, bool link_up, bool gw_ok, bool gw_ever_ok,
                              int fails_to_recover = kWatchFailsToRecover) {
    if (!link_up || gw_ok) {
        w.fails = 0;
        return WatchAction::Idle;
    }
    if (++w.fails < fails_to_recover)
        return WatchAction::Wait;

    // Threshold reached — the run is spent either way, so clear it before deciding. Without
    // this a device behind an ICMP-dropping router would report NoBaseline on every single
    // check after the second, instead of once per fails_to_recover window.
    w.fails = 0;
    return gw_ever_ok ? WatchAction::Recover : WatchAction::NoBaseline;
}

}  // namespace tk
