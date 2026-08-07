// Emits golden decision vectors for the ST7735 display presenter (tk::display::compose in
// main/logic/display_model.hpp) as TSV: each row is the input UiSnapshot fields + the decided
// Model. tools/display_sim.py's `parity` mode re-decides the same inputs and diffs the result,
// so the pixel sim can be checked against the firmware presenter automatically (killing the
// "the sim mirrors display.cpp 1:1 by hand" drift risk). Host-only, no ESP-IDF; plain g++.
// Built + run by scripts/check-display-sim-parity.sh. Bars (rssi→level) are excluded — the sim
// is fed levels directly and does not re-derive them (rssi_bars is host-tested in test_logic).

#include "logic/ui_state.hpp"
#include "logic/display_model.hpp"

#include <cstdio>

using tk::LinkState;
namespace dm = tk::display;

static const char* link_str(LinkState s) {
    switch (s) {
        case LinkState::Awake:       return "awake";
        case LinkState::Asleep:      return "asleep";
        case LinkState::Idle:        return "idle";
        case LinkState::Unreachable: return "unreachable";
        default:                     return "unknown";
    }
}
static const char* hero_str(dm::Hero h) {
    switch (h) {
        case dm::Hero::NetSearch: return "net_search";
        case dm::Hero::Pairing:   return "pairing";
        case dm::Hero::BleSearch: return "ble_search";
        default:                  return "battery";
    }
}
static const char* net_str(tk::NetLink n) { return tk::net_link_str(n); }

static tk::UiSnapshot mk(LinkState link, tk::NetLink net, const char* ssid, bool ble, bool paired,
                         bool have_soc, int soc, bool charging, bool eth_present = false) {
    tk::UiSnapshot s;
    s.link_state    = link;
    s.net           = net;
    s.eth_present   = eth_present;
    s.ble_connected = ble;
    s.paired        = paired;
    s.have_soc      = have_soc;
    s.soc           = soc;
    s.charging      = charging;
    std::snprintf(s.ssid, sizeof(s.ssid), "%s", ssid);
    return s;
}

// Shorthands so the case table stays readable at one row per case.
static constexpr tk::NetLink NONE = tk::NetLink::None;
static constexpr tk::NetLink WIFI = tk::NetLink::Wifi;
static constexpr tk::NetLink ETH  = tk::NetLink::Eth;

int main() {
    struct Case { tk::UiSnapshot s; unsigned tick; };
    const Case cases[] = {
        { mk(LinkState::Idle,        WIFI,  "Home",                     true,  true,  true,  55,  false),  0 },  // battery, no bolt
        { mk(LinkState::Idle,        WIFI,  "Home",                     true,  true,  true,  55,  true),   0 },  // charging → bolt
        { mk(LinkState::Asleep,      WIFI,  "Home",                     true,  true,  true,  80,  true),   0 },  // asleep dims, bolt suppressed
        { mk(LinkState::Idle,        WIFI,  "Home",                     true,  true,  true,  150, true),   0 },  // soc clamps to 100 → no bolt
        { mk(LinkState::Idle,        WIFI,  "Home",                     true,  true,  true,  0,   false),  0 },  // empty battery colour
        { mk(LinkState::Awake,       WIFI,  "Home",                     true,  true,  true,  55,  false),  0 },  // awake is also a live reading
        { mk(LinkState::Unknown,     NONE,  "Home",                     false, false, false, 0,   false),  5 },  // network search hero (no eth) → "WiFi"
        { mk(LinkState::Idle,        WIFI,  "Home",                     true,  false, false, 0,   false),  7 },  // pairing (BLE up, unpaired)
        { mk(LinkState::Unknown,     WIFI,  "Home",                     false, true,  false, 0,   false),  9 },  // BLE search hero
        { mk(LinkState::Unreachable, WIFI,  "Home",                     false, true,  true,  50,  false),  0 },  // unreachable → BLE search, stale soc ignored
        { mk(LinkState::Idle,        WIFI,  "AVeryLongNetworkNameXYZ",  true,  true,  true,  42,  false), 10 },  // long SSID at battery avail(98) → scrolls
        { mk(LinkState::Idle,        WIFI,  "EightChr",                 true,  true,  true,  42,  false), 10 },  // 8 chars = 96px <= 98 → no scroll
        { mk(LinkState::Unknown,     WIFI,  "TenCharSSD",               false, true,  false, 0,   false),  3 },  // BLE-search: wider avail(130), 120px → no scroll
        { mk(LinkState::Unknown,     WIFI,  "AVeryLongNetworkNameXYZ",  false, true,  false, 0,   false),  4 },  // BLE-search: long SSID at avail(130) → scrolls
        { mk(LinkState::Idle,        WIFI,  "TwelveCharSSD",            true,  true,  true,  42,  false), 10 },  // portrait: 78px > avail(72) → scrolls (fits landscape)
        { mk(LinkState::Idle,        WIFI,  "TenCharSSD",               true,  true,  true,  42,  false), 10 },  // portrait: 60px <= 72 → no scroll
        // ── Ethernet (W5500 / PoE base): the wire takes the WiFi slot but draws a label, so
        // show_wifi is false while show_lan is true and every ssid_* stays at its default —
        // an Ethernet link has neither a signal level nor a network name to render.
        { mk(LinkState::Idle,        ETH,   "ignored",                  true,  true,  true,  42,  false, true), 10 },  // LAN label, battery hero
        { mk(LinkState::Unknown,     ETH,   "ignored",                  false, true,  false, 0,   false, true),  3 },  // LAN label, BLE-search hero
        // Searching on a PoE board: no transport yet, so the hero must label itself "LAN",
        // not "WiFi" — eth_present is the only input that can say so.
        { mk(LinkState::Unknown,     NONE,  "ignored",                  false, false, false, 0,   false, true),  5 },  // search hero says LAN
    };

    // Each case is decided for BOTH orientations (landscape + portrait) so the sim's
    // orientation-aware decide() is checked against the C++ presenter on every layout.
    struct Or { dm::Orient o; const char* name; };
    const Or orients[] = { { dm::Orient::Landscape, "landscape" }, { dm::Orient::Portrait, "portrait" } };

    std::printf("link\tnet\teth_present\tssid\tble_connected\thave_soc\tsoc\tcharging\tpaired\t"
                "tick\torient\t"
                "hero\tshow_wifi\tshow_lan\tsearch_is_lan\tshow_ble\tssid_avail\tssid_scrolling\t"
                "ssid_off\tout_soc\tfill_r\tfill_g\tfill_b\tasleep\tshow_bolt\tanimating\n");

    for (const Case& c : cases) {
        const tk::UiSnapshot& s = c.s;
        for (const Or& orr : orients) {
            const dm::Model m = dm::compose(s, c.tick, orr.o);
            std::printf("%s\t%s\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%u\t%s\t",
                        link_str(s.link_state), net_str(s.net), s.eth_present ? 1 : 0, s.ssid,
                        s.ble_connected ? 1 : 0, s.have_soc ? 1 : 0, s.soc, s.charging ? 1 : 0,
                        s.paired ? 1 : 0, c.tick, orr.name);
            std::printf("%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n",
                        hero_str(m.hero), m.show_wifi ? 1 : 0, m.show_lan ? 1 : 0,
                        m.search_is_lan ? 1 : 0, m.show_ble ? 1 : 0, m.ssid_avail,
                        m.ssid_scrolling ? 1 : 0, m.ssid_scroll_off, m.soc,
                        m.fill_r, m.fill_g, m.fill_b, m.asleep ? 1 : 0, m.show_bolt ? 1 : 0,
                        m.animating ? 1 : 0);
        }
    }
    return 0;
}
