#pragma once
// Active-window decision for the background infotainment poll (vehicle_telemetry.cpp loop_task_fn_).
// Pure, IDF-free, host-tested (test/test_logic.cpp).
//
// The window gates whether we open an INFOTAINMENT session (which keeps the car's main computer
// awake). It must be open only while the car has a reason to be awake, so a parked/idle car can
// reach sleep. Two independent reasons open it:
//   - a recent evcc/manual command (kActiveWindowMs), OR
//   - the car is CHARGING *and we can prove it right now* — i.e. we have fresh live contact.
//
// Why the charging arm needs a freshness gate. `charging_state` comes from a RAM cache that is only
// invalidated on a pairing reset — never on a link drop or on staleness. So a car that was
// "Charging" when it last answered keeps that cached state forever. Without a freshness gate, a car
// that unplugs and drives away (or suffers a sustained BLE dropout) while the cache still says
// "Charging" holds the window open indefinitely, and the warm-up-connect block scans/reconnects
// continuously — stealing WiFi/BT coexistence airtime and defeating the whole "drop the link so the
// MCU can sleep" design. A genuinely charging, reachable car answers the ~10 s charge poll, so its
// contact age stays well under kAwakeMaxAgeS (the same freshness link_state() calls "Awake"). Gating
// the charging arm on `contact_age_s < kAwakeMaxAgeS` therefore holds the window open for a car that
// is provably charging-and-awake, and lets it close ~kAwakeMaxAgeS after the car goes unreachable.
#include <cstdint>
#include "logic/link_state.hpp"   // kAwakeMaxAgeS — the shared fresh-infotainment threshold

namespace tk {

struct WindowInputs {
    bool     recent_cmd;      // a command was sent within kActiveWindowMs
    bool     charging_state;  // cached charging_state is "Charging" or "Starting"
    bool     have_contact;    // seconds_since_contact() returned true (we have live data at all)
    uint32_t contact_age_s;   // seconds since the last live infotainment contact (valid iff have_contact)
};

// True when the background infotainment poll window should be open.
inline constexpr bool active_window_open(const WindowInputs& in) {
    const bool fresh_charging =
        in.charging_state && in.have_contact && in.contact_age_s < kAwakeMaxAgeS;
    return in.recent_cmd || fresh_charging;
}

} // namespace tk
