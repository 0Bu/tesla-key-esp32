// Emits golden decision vectors for the web UI's Bluetooth row presenter (tk::ble::decide in
// main/logic/ble_row.hpp) as TSV: each line is the input fields followed by the decided
// row/countdown/stateless. tools/ble_row_parity.js re-decides the same inputs with the
// JavaScript half that actually runs in the browser (the BLE_ROW region of main/www/app.js)
// and diffs the result — so "app.js mirrors the presenter" is checked by CI, not by hand.
// Host-only, no ESP-IDF; plain g++. Built + run by scripts/check-ble-row-parity.sh.
//
// BLE vectors cover the finite input classes below. --web-presenters additionally sweeps all
// integer SoC percentages with clamp cases, signed-byte RSSI and integer extrema.

#include "logic/ble_row.hpp"
#include "logic/link_state.hpp"

#include "logic/display_model.hpp"
#include "logic/soc_gradient.hpp"

#include <cstdio>
#include <cstring>
#include <limits>
#include <initializer_list>

namespace b = tk::ble;

static const char* phase_str(b::Phase p) {
    switch (p) {
        case b::Phase::Connecting: return "connecting";
        case b::Phase::Waiting:    return "waiting";
        default:                   return "none";
    }
}

int main(int argc, char** argv) {
    if (argc==2 && std::strcmp(argv[1],"--web-presenters")==0) {
        std::printf("kind\tinput\tr\tg\tb\n");
        for (int soc=-100;soc<=200;++soc) {
            int r,g,b;tk::soc_rgb(soc,r,g,b);
            std::printf("soc\t%d\t%d\t%d\t%d\n",soc,r,g,b);
        }
        for (int soc : {std::numeric_limits<int>::min(),std::numeric_limits<int>::max()}) {
            int r,g,b;tk::soc_rgb(soc,r,g,b);
            std::printf("soc\t%d\t%d\t%d\t%d\n",soc,r,g,b);
        }
        for (int rssi=-128;rssi<=127;++rssi)
            std::printf("rssi\t%d\t%d\t0\t0\n",rssi,tk::display::rssi_bars(rssi));
        for (int rssi : {std::numeric_limits<int>::min(),std::numeric_limits<int>::max()})
            std::printf("rssi\t%d\t%d\t0\t0\n",rssi,tk::display::rssi_bars(rssi));
        std::printf("warn\t0\t%u\t0\t0\n",static_cast<unsigned>(b::kConnectFailWarn));
        return 0;
    }
    if (argc!=1) return 2;
    const b::Phase phases[]  = { b::Phase::None, b::Phase::Connecting, b::Phase::Waiting };
    const uint32_t fails[]   = { 0, 1, 2, 9 };
    const int      devices[] = { 0, 4 };
    // Raw /status shapes, not pre-derived booleans: the VIN arrives as the string the firmware
    // reports ("UNKNOWN" when none is stored), and link as its lowercase web spelling. Sweeping
    // those means the harness also pins how they map to "has a VIN" and "link is known".
    const char*    vins[]    = { "UNKNOWN", "5YJ3E1EA7KF000316" };
    const tk::LinkState links[] = { tk::LinkState::Unknown, tk::LinkState::Unreachable,
                                    tk::LinkState::Awake,   tk::LinkState::Asleep,
                                    tk::LinkState::Idle };

    std::printf("vin\tpaired\tlink\tconnected\tdevices\tconnect_fail\tphase\trow\tcd\tstateless\n");
    for (const char* vin : vins)
    for (int paired = 0; paired < 2; paired++)
    for (tk::LinkState link : links)
    for (int conn = 0; conn < 2; conn++)
    for (int dev : devices)
    for (uint32_t cf : fails)
    for (b::Phase ph : phases) {
        b::RowStatus st;
        st.vin              = vin;
        st.paired           = paired;
        st.link             = link;
        st.ble_connected    = conn;
        st.ble_devices      = dev;
        st.ble_connect_fail = cf;
        st.phase            = ph;
        b::RowView v = b::decide(st);
        std::printf("%s\t%d\t%s\t%d\t%d\t%u\t%s\t%s\t%s\t%d\n",
                    vin, paired, tk::link_state_web_str(link), conn, dev, (unsigned)cf,
                    phase_str(ph), b::row_name(v.row), b::cd_name(v.cd), v.stateless ? 1 : 0);
    }
    return 0;
}
