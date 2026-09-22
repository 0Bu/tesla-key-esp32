#pragma once
// One-shot charge-state poll on the VCSEC ASLEEP→AWAKE wake edge and stale-cache bootstrap (issues #264, #300, #301, #308).
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
// Further variants of this deadlock share one shape: the car is AWAKE and reachable, but the
// wake-edge detector never sees an ASLEEP→AWAKE transition, so nothing refreshes the cache.
//   • Device power-cycle / reboot: last_known_charge_ starts invalid, and if the car is already
//     awake (or wakes before a stable asleep run can accumulate) no edge ever arrives. evcc gets
//     HTTP 503 ("stale or unavailable"), coasts on its own cache, and never sends a command.
//   • The car drives away and comes back (the common daily case): the pre-drive wake already
//     consumed the arm, BLE drops on departure — which forces the sleep mirror to UNKNOWN — and the
//     reconnect on arrival reads AWAKE. That is an UNKNOWN→AWAKE transition, which never fires, and
//     the car stays awake while plugged in so no fresh stable-asleep run can re-arm it. The cache is
//     *valid*, so a bootstrap keyed on validity alone does not fire either: the pre-drive SOC
//     survives indefinitely (observed: 83% cached vs 18% actual, 8 h after the last contact).
// Validity is therefore the wrong bootstrap key — AGE is. A cache older than kChargeCacheFreshS
// cannot describe a car that has since been driven, so treat it like no cache at all.
//
// The fix: dual triggers for one shared one-shot poll.
//   1. Wake edge: on an ASLEEP→AWAKE transition after a debounced asleep run, fire exactly one
//      charge_state_poll(NO_WAKE_SKIP).
//   2. Stale-cache bootstrap: when paired and BLE-connected but last_known_charge_ is missing or
//      older than kChargeCacheFreshS (!cache_fresh), arm the same one-shot. When the car is AWAKE,
//      it fires once to refresh the cache and break the deadlock. If a poll fails/times out,
//      exponential backoff retries without hammering an unresponsive MCU (issue #301).
//   3. Awake-episode quiescence latch: once a fresh charge cache is acquired or held during an
//      awake episode, the episode latch (episode_fresh) engages and prevents subsequent periodic
//      re-arming while the car remains continuously connected and awake (issue #308). The system
//      remains completely quiescent so an idle parked car can reach sleep undisturbed.
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
// run, so it never arms, and an UNKNOWN→AWAKE while the cache is still fresh never fires either.
// One poll per wake episode — after firing we re-arm only on a fresh stable-asleep run.
#include <algorithm>
#include <cstdint>

#include "logic/link_state.hpp"   // kAwakeMaxAgeS — the shared fresh-infotainment threshold

namespace tk {

// How recent last_known_charge_ must be for the bootstrap trigger to consider it trustworthy.
// Deliberately the same threshold link_state() calls "Awake": below it we hold genuinely live
// data, above it the reading is a leftover that a drive could have invalidated. The bootstrap is
// armed at most once per BLE connection or wake episode, so a tight threshold costs at most one extra
// NO_WAKE_SKIP poll while making the post-drive case impossible to miss.
inline constexpr uint32_t kChargeCacheFreshS = kAwakeMaxAgeS;

// Backoff parameters for bootstrap retries when CarServer fails / times out (issue #301).
inline constexpr uint32_t kBootstrapInitialBackoffS = 30;
inline constexpr uint32_t kBootstrapMaxBackoffS = 300;

// The three things the VCSEC sleep flag can report to the sampler each cycle.
enum class WakeSample : uint8_t { Unknown, Asleep, Awake };

// State the vehicle loop carries across iterations. Zero-initialized is the correct
// "just booted, nothing observed yet, not armed" starting point.
struct WakePollState {
    bool armed = false;                 // debounced-asleep or bootstrap arm; next AWAKE fires one poll
    bool pending = false;               // latched fire request until connected & queue-idle
    bool bootstrap_dispatched = false;  // bootstrap one-shot has already armed for this connection
    bool episode_fresh = false;         // a fresh charge cache was acquired/held for this awake episode (issue #308)
    uint32_t bootstrap_backoff_s = 0;   // current backoff interval in seconds
    uint32_t next_bootstrap_retry_s = 0;// earliest now_s for next retry attempt

    inline void note_asleep(bool stably_asleep) {
        if (stably_asleep) {
            armed = true;
            pending = false;
            episode_fresh = false;
            bootstrap_dispatched = false;
            bootstrap_backoff_s = 0;
            next_bootstrap_retry_s = 0;
        }
    }

    inline void note_awake() {
        if (armed) {
            armed = false;
            pending = true;
        }
    }

    inline void note_bootstrap(bool ble_connected, bool cache_fresh, uint32_t now_s = 0) {
        if (!ble_connected) {
            return;
        }
        if (cache_fresh) {
            episode_fresh = true;
            bootstrap_backoff_s = 0;
            next_bootstrap_retry_s = 0;
            return;
        }
        if (episode_fresh) {
            // Once a fresh cache is acquired for this awake episode, stay quiescent
            // while continuously connected and awake (issue #308).
            return;
        }
        if (!bootstrap_dispatched) {
            bootstrap_dispatched = true;
            armed = true;
            bootstrap_backoff_s = kBootstrapInitialBackoffS;
            next_bootstrap_retry_s = now_s + bootstrap_backoff_s;
            return;
        }
        if (armed || pending) {
            return;
        }
        if (now_s >= next_bootstrap_retry_s) {
            armed = true;
            bootstrap_backoff_s = (bootstrap_backoff_s == 0)
                ? kBootstrapInitialBackoffS
                : std::min(bootstrap_backoff_s * 2, kBootstrapMaxBackoffS);
            next_bootstrap_retry_s = now_s + bootstrap_backoff_s;
        }
    }

    inline void note_disconnected() {
        pending = false;
        armed = false;
        bootstrap_dispatched = false;
        bootstrap_backoff_s = 0;
        next_bootstrap_retry_s = 0;
        episode_fresh = false;
    }

    inline bool take_pending(uint32_t now_s = 0) {
        const bool p = pending;
        pending = false;
        if (p) {
            if (bootstrap_backoff_s == 0) {
                bootstrap_backoff_s = kBootstrapInitialBackoffS;
            }
            next_bootstrap_retry_s = now_s + bootstrap_backoff_s;
        }
        return p;
    }
};

struct WakePollInputs {
    WakeSample sample;                     // this cycle's VCSEC sleep reading
    bool       vcsec_stably_asleep;        // the current ASLEEP run has held >= kAsleepDebounceS
    bool       ble_connected = true;       // whether BLE link is connected
    // Raw charge-cache facts, threshold applied here rather than by the caller — the same split
    // WindowInputs uses for have_contact/contact_age_s, so the decision stays host-testable.
    bool       have_charge_cache = true;   // a ChargeState has been received since boot / re-pair
    uint32_t   charge_cache_age_s = 0;     // seconds since that ChargeState (valid iff have_charge_cache)
    uint32_t   now_s = 0;                  // monotonic uptime seconds for retry backoff (#301)
};

// A cached ChargeState we are still willing to trust: present, and young enough that the car
// cannot have been driven since. Anything else is treated exactly like no cache at all.
inline constexpr bool charge_cache_fresh(const WakePollInputs& in) {
    return in.have_charge_cache && in.charge_cache_age_s < kChargeCacheFreshS;
}

// Update the WakePollState from this cycle's telemetry sample.
inline void wake_poll_update(WakePollState& st, const WakePollInputs& in) {
    if (!in.ble_connected) {
        st.note_disconnected();
        return;
    }
    st.note_bootstrap(in.ble_connected, charge_cache_fresh(in), in.now_s);
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
    uint32_t now_s = 0;
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
        st.take_pending(in.now_s);
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
    return charge_poll_should_fire({true, false, in.ble_connected, false, in.now_s}, st);
}

}  // namespace tk
