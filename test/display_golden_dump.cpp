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
        case dm::Hero::WifiSearch: return "wifi_search";
        case dm::Hero::Pairing:    return "pairing";
        case dm::Hero::BleSearch:  return "ble_search";
        default:                   return "battery";
    }
}

static tk::UiSnapshot mk(LinkState link, bool wifi_on, const char* ssid, bool ble, bool paired,
                         bool have_soc, int soc, bool charging) {
    tk::UiSnapshot s;
    s.link_state    = link;
    s.wifi_on       = wifi_on;
    s.ble_connected = ble;
    s.paired        = paired;
    s.have_soc      = have_soc;
    s.soc           = soc;
    s.charging      = charging;
    std::snprintf(s.ssid, sizeof(s.ssid), "%s", ssid);
    return s;
}

int main() {
    struct Case { tk::UiSnapshot s; unsigned tick; };
    const Case cases[] = {
        { mk(LinkState::Idle,        true,  "Home",                     true,  true,  true,  55,  false),  0 },  // battery, no bolt
        { mk(LinkState::Idle,        true,  "Home",                     true,  true,  true,  55,  true),   0 },  // charging → bolt
        { mk(LinkState::Asleep,      true,  "Home",                     true,  true,  true,  80,  true),   0 },  // asleep dims, bolt suppressed
        { mk(LinkState::Idle,        true,  "Home",                     true,  true,  true,  150, true),   0 },  // soc clamps to 100 → no bolt
        { mk(LinkState::Idle,        true,  "Home",                     true,  true,  true,  0,   false),  0 },  // empty battery colour
        { mk(LinkState::Awake,       true,  "Home",                     true,  true,  true,  55,  false),  0 },  // awake is also a live reading
        { mk(LinkState::Unknown,     false, "Home",                     false, false, false, 0,   false),  5 },  // WiFi search hero
        { mk(LinkState::Idle,        true,  "Home",                     true,  false, false, 0,   false),  7 },  // pairing (BLE up, unpaired)
        { mk(LinkState::Unknown,     true,  "Home",                     false, true,  false, 0,   false),  9 },  // BLE search hero
        { mk(LinkState::Unreachable, true,  "Home",                     false, true,  true,  50,  false),  0 },  // unreachable → BLE search, stale soc ignored
        { mk(LinkState::Idle,        true,  "AVeryLongNetworkNameXYZ",  true,  true,  true,  42,  false), 10 },  // long SSID at battery avail(98) → scrolls
        { mk(LinkState::Idle,        true,  "EightChr",                 true,  true,  true,  42,  false), 10 },  // 8 chars = 96px <= 98 → no scroll
        { mk(LinkState::Unknown,     true,  "TenCharSSD",               false, true,  false, 0,   false),  3 },  // BLE-search: wider avail(130), 120px → no scroll
        { mk(LinkState::Unknown,     true,  "AVeryLongNetworkNameXYZ",  false, true,  false, 0,   false),  4 },  // BLE-search: long SSID at avail(130) → scrolls
    };

    std::printf("link\twifi_on\tssid\tble_connected\thave_soc\tsoc\tcharging\tpaired\ttick\t"
                "hero\tshow_wifi\tshow_ble\tssid_avail\tssid_scrolling\tssid_off\tout_soc\t"
                "fill_r\tfill_g\tfill_b\tasleep\tshow_bolt\tanimating\n");

    for (const Case& c : cases) {
        const tk::UiSnapshot& s = c.s;
        const dm::Model m = dm::compose(s, c.tick);
        std::printf("%s\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%u\t",
                    link_str(s.link_state), s.wifi_on ? 1 : 0, s.ssid, s.ble_connected ? 1 : 0,
                    s.have_soc ? 1 : 0, s.soc, s.charging ? 1 : 0, s.paired ? 1 : 0, c.tick);
        std::printf("%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n",
                    hero_str(m.hero), m.show_wifi ? 1 : 0, m.show_ble ? 1 : 0, m.ssid_avail,
                    m.ssid_scrolling ? 1 : 0, m.ssid_scroll_off, m.soc,
                    m.fill_r, m.fill_g, m.fill_b, m.asleep ? 1 : 0, m.show_bolt ? 1 : 0,
                    m.animating ? 1 : 0);
    }
    return 0;
}
