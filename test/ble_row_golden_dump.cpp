// Emits golden decision vectors for the web UI's Bluetooth row presenter (tk::ble::decide in
// main/logic/ble_row.hpp) as TSV: each line is the input fields followed by the decided
// row/countdown/stateless. tools/ble_row_parity.js re-decides the same inputs with the
// JavaScript half that actually runs in the browser (the BLE_ROW region of main/www/app.js)
// and diffs the result — so "app.js mirrors the presenter" is checked by CI, not by hand.
// Host-only, no ESP-IDF; plain g++. Built + run by scripts/check-ble-row-parity.sh.
//
// The sweep is exhaustive over the representable input space rather than a hand-picked list:
// the whole point of this file is to leave no combination where the two implementations could
// quietly differ.

#include "logic/ble_row.hpp"
#include "logic/link_state.hpp"

#include <cstdio>

namespace b = tk::ble;

static const char* phase_str(b::Phase p) {
    switch (p) {
        case b::Phase::Connecting: return "connecting";
        case b::Phase::Waiting:    return "waiting";
        default:                   return "none";
    }
}

int main() {
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
