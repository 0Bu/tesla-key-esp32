#pragma once
// One-shot charge-state poll on the VCSEC ASLEEP→AWAKE wake edge and cache-invalid bootstrap (issue #264).
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
// A second variant of this deadlock occurs on device power-cycle / reboot: last_known_charge_ starts
// invalid, but if the car was already awake (or wakes before a stable asleep run can accumulate),
// the wake-edge detector never sees an ASLEEP→AWAKE transition. evcc receives HTTP 503 ("stale or
// unavailable"), coasts on its own internal cache, and never sends a command.
//
// The fix: dual triggers for one shared one-shot poll.
//   1. Wake edge: on an ASLEEP→AWAKE transition after a debounced asleep run, fire exactly one
//      charge_state_poll(NO_WAKE_SKIP).
//   2. Cache-invalid bootstrap: when paired and BLE-connected but last_known_charge_ is invalid
//      (!cache_valid), arm the same one-shot. When the car is AWAKE, it fires once to populate the
//      initial cache and break the deadlock.
//
// Both use the same NO_WAKE_SKIP poll: a car already back asleep is skipped, so the device still
// never causes a wake: we only ever piggyback on one the car did on its own. One poll refreshes the
// SOC cache; if it reports Charging/Starting the existing charging arm opens the window and normal
// session polling takes over, otherwise the car re-sleeps undisturbed.
//
// Why arming requires a *debounced* asleep run for the wake-edge trigger:
// A single ASLEEP reading is not proof of sleep: the VCSEC flag flaps AWAKE↔ASLEEP (~60 s) while
// Cabin-Overheat-Protection cycles the A/C, and it reads UNKNOWN at boot and across a BLE reconnect.
// Arming only after the flag has held ASLEEP for the shared kAsleepDebounceS (the same debounce
// link_state() trusts before it shows "asleep") filters both: a COP blip never accumulates a stable
// run, so it never arms, and an UNKNOWN→AWAKE when cache is already valid never fires either.
// One poll per wake episode — after firing we re-arm only on a fresh stable-asleep run.
#include <cstdint>

namespace tk {

// The three things the VCSEC sleep flag can report to the sampler each cycle.
enum class WakeSample : uint8_t { Unknown, Asleep, Awake };

// State the vehicle loop carries across iterations. Zero-initialized is the correct
// "just booted, nothing observed yet, not armed" starting point.
struct WakePollState {
    bool armed = false;                 // debounced-asleep or bootstrap arm; next AWAKE fires one poll
    bool pending = false;               // latched fire request until connected & queue-idle
    bool bootstrap_dispatched = false;  // bootstrap one-shot has already armed for this connection

    inline void note_asleep(bool stably_asleep) {
        if (stably_asleep) {
            armed = true;
            pending = false;
        }
    }

    inline void note_awake() {
        if (armed) {
            armed = false;
            pending = true;
        }
    }

    inline void note_bootstrap(bool ble_connected, bool cache_valid) {
        if (!bootstrap_dispatched && ble_connected && !cache_valid) {
            bootstrap_dispatched = true;
            armed = true;
        }
    }

    inline void note_disconnected() {
        bootstrap_dispatched = false;
    }

    inline bool take_pending() {
        const bool p = pending;
        pending = false;
        return p;
    }
};

struct WakePollInputs {
    WakeSample sample;                    // this cycle's VCSEC sleep reading
    bool       vcsec_stably_asleep;       // the current ASLEEP run has held >= kAsleepDebounceS
    bool       ble_connected = true;      // whether BLE link is connected
    bool       charge_cache_valid = true; // whether charge-state cache is already populated
};

// Update the WakePollState from this cycle's telemetry sample.
inline void wake_poll_update(WakePollState& st, const WakePollInputs& in) {
    if (!in.ble_connected) {
        st.note_disconnected();
        return;
    }
    st.note_bootstrap(in.ble_connected, in.charge_cache_valid);
    switch (in.sample) {
        case WakeSample::Asleep:
            st.note_asleep(in.vcsec_stably_asleep);
            break;
        case WakeSample::Awake:
            st.note_awake();
            break;
        case WakeSample::Unknown:
        default:
            break;
    }
}

struct ChargePollGateInputs {
    bool paired = false;
    bool poll_cadence = false;
    bool ble_connected = false;
    bool cmd_in_flight = false;
};

// Decide whether the vehicle loop should fire a background charge-state poll this iteration.
// The poll fires if the device is paired, connected, queue-idle (!cmd_in_flight), and EITHER:
//   • poll_cadence is true (10 s periodic refresh within the active window), OR
//   • a wake-edge or bootstrap poll request is latched in st.pending (issue #264).
//
// Crucially, st.take_pending() is ONLY called when the command channel is actually ready to
// dispatch (ble_connected && !cmd_in_flight). If the channel is blocked — which is always the case
// on the iteration an ASLEEP→AWAKE edge is discovered via the health probe, because the probe holds
// cmd_in_flight_ — st.pending remains latched so the poll fires on the subsequent iteration once
// the probe completes and cmd_in_flight_ drops.
inline bool charge_poll_should_fire(const ChargePollGateInputs& in, WakePollState& st) {
    if (in.paired && (in.poll_cadence || st.pending) && in.ble_connected && !in.cmd_in_flight) {
        st.take_pending();
        return true;
    }
    return false;
}

// Advance the one-shot state machine by one sample; return true iff a charge poll should fire
// immediately, assuming channel is idle and connected.
// Composes wake_poll_update and charge_poll_should_fire so test coverage aligns with the
// production loop call-site.
inline bool wake_edge_should_poll(WakePollState& st, const WakePollInputs& in) {
    wake_poll_update(st, in);
    return charge_poll_should_fire({true, false, in.ble_connected, false}, st);
}

}  // namespace tk
