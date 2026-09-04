#pragma once
// One-shot charge-state poll on the VCSEC ASLEEP→AWAKE wake edge (issue #264).
// Pure, IDF-free, host-tested (test/test_logic.cpp).
//
// Problem this closes. While parked and asleep the firmware stops all infotainment polling so the
// car can sleep (see logic/active_window.hpp): the poll window opens only on a recent command or a
// cached Charging/Starting state. Plugging in the charge cable wakes the car's MCU but sends us no
// command and does not (yet) show as cached charging, so the ESP never re-reads the pack — evcc
// keeps serving a stale SOC and, when that stale value sits above minSoc, never sends the command
// that would have opened the window. The data that should trigger the action is itself the stale
// data. See the issue for the full circular-dependency writeup.
//
// The fix. The auto-pair VCSEC health probe keeps answering while parked (it never wakes the MCU)
// and each reply carries the car's sleep flag, which loop_task already mirrors and edge-detects.
// On a wake the car performed *itself* (cable plug-in, door, app access) we fire exactly one
// charge_state_poll(NO_WAKE_SKIP) — the same call the 10 s background refresh uses. NO_WAKE_SKIP
// means a car that already went back to sleep is left undisturbed, so the device still never
// causes a wake: we only ever piggyback on one the car did on its own. One poll refreshes the SOC
// cache; if it reports Charging/Starting the existing charging arm opens the window and normal
// session polling takes over, otherwise the car re-sleeps undisturbed.
//
// Why arming requires a *debounced* asleep run. A single ASLEEP reading is not proof of sleep: the
// VCSEC flag flaps AWAKE↔ASLEEP (~60 s) while Cabin-Overheat-Protection cycles the A/C, and it
// reads UNKNOWN at boot and across a BLE reconnect. Arming only after the flag has held ASLEEP for
// the shared kAsleepDebounceS (the same debounce link_state() trusts before it shows "asleep")
// filters both: a COP blip never accumulates a stable run, so it never arms, and an UNKNOWN→AWAKE
// at boot was never armed either. One poll per wake episode — after firing we re-arm only on a
// fresh stable-asleep run.
#include <cstdint>

namespace tk {

// The three things the VCSEC sleep flag can report to the sampler each cycle.
enum class WakeSample : uint8_t { Unknown, Asleep, Awake };

// State the vehicle loop carries across iterations. Zero-initialized is the correct
// "just booted, nothing observed yet, not armed" starting point.
struct WakePollState {
    bool armed = false;  // a debounced-asleep run has completed; the next AWAKE fires one poll
};

struct WakePollInputs {
    WakeSample sample;               // this cycle's VCSEC sleep reading
    bool       vcsec_stably_asleep;  // the current ASLEEP run has held >= kAsleepDebounceS
};

// Advance the one-shot state machine by one sample; return true iff the loop should fire a single
// charge_state_poll(NO_WAKE_SKIP) this cycle.
//   ASLEEP  — once the run is debounce-stable, arm the one-shot (a bare ASLEEP reading does not,
//             and a bare reading after arming never disarms).
//   AWAKE   — fire iff armed, then disarm (one poll per wake episode; re-arm needs a fresh run).
//   UNKNOWN — leave the arm untouched, so a transient link/boot gap neither arms nor fires and
//             cannot drop an arm that a real wake during the gap should still honour.
inline bool wake_edge_should_poll(WakePollState& st, const WakePollInputs& in) {
    switch (in.sample) {
        case WakeSample::Asleep:
            if (in.vcsec_stably_asleep) st.armed = true;
            return false;
        case WakeSample::Awake: {
            const bool fire = st.armed;
            st.armed = false;
            return fire;
        }
        case WakeSample::Unknown:
        default:
            return false;
    }
}

}  // namespace tk
