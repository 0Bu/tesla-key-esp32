#pragma once

#include <cstdint>

// Pure, hardware-free model of VehicleController::link_state() — the single source
// of truth for the car's high-level connectivity, shared by the web UI hero and the
// MQTT/HA bridge so the two never drift. The firmware gathers the atomic member
// state into LinkInputs and calls compute_link_state(); the host mock build tests
// the decision (incl. the debounced-ASLEEP asymmetry) without a board. See the long
// rationale on VehicleController::link_state() in vehicle_ctrl.hpp / .cpp.
namespace tk {

enum class LinkState { Unknown, Awake, Asleep, Unreachable, Idle };

// Tuning constants (seconds). Kept here so the state machine and its tests agree.
//   kAwakeMaxAgeS     fresh-infotainment window: newer ⇒ Awake.
//   kReachableMaxAgeS how long a successful BLE round-trip keeps the car "reachable".
//   kAsleepDebounceS  how long VCSEC must hold ASLEEP before we trust it as sleep.
inline constexpr uint32_t kAwakeMaxAgeS     = 60;
inline constexpr uint32_t kReachableMaxAgeS = 150;
inline constexpr uint32_t kAsleepDebounceS  = 120;

// Snapshot of the controller's connectivity state, all derived from monotonic
// uptime ticks (wall-clock independent).
struct LinkInputs {
    bool     have_contact          = false;  // ever received live infotainment data
    uint32_t contact_age_s         = 0;      // seconds since that data (valid iff have_contact)
    bool     have_reachable        = false;  // ever completed a signed BLE round-trip
    uint32_t reachable_age_s       = 0;      // seconds since reachable (valid iff have_reachable)
    bool     vcsec_stably_asleep   = false;  // VCSEC sleep flag held ASLEEP >= kAsleepDebounceS
};

// The four-state decision. Asymmetry: a debounced VCSEC ASLEEP is trusted as proof
// of sleep, but VCSEC AWAKE is never trusted to assert Awake — that always requires
// fresh live telemetry, so a wrong VCSEC AWAKE can only ever leave us in Idle.
inline LinkState compute_link_state(const LinkInputs& in) {
    if (in.have_contact && in.contact_age_s < kAwakeMaxAgeS) return LinkState::Awake;
    if (in.have_reachable && in.reachable_age_s < kReachableMaxAgeS) {
        if (in.vcsec_stably_asleep) return LinkState::Asleep;
        return LinkState::Idle;  // reachable but not provably asleep — never claim sleep
    }
    // Heard something at some point but it's now stale ⇒ unreachable; never heard ⇒ unknown.
    if (in.have_reachable || in.have_contact) return LinkState::Unreachable;
    return LinkState::Unknown;
}

// Web-UI hero string (/status "link"). Drives the awake/asleep/parked card selection.
inline const char* link_state_web_str(LinkState s) {
    switch (s) {
        case LinkState::Awake:       return "awake";
        case LinkState::Asleep:      return "asleep";
        case LinkState::Idle:        return "idle";
        case LinkState::Unreachable: return "unreachable";
        default:                     return "unknown";
    }
}

// MQTT/HA sleep_status string. Unknown ⇒ nullptr (the bridge omits the field so HA
// shows "unknown" rather than publishing a phantom state).
inline const char* link_state_mqtt_str(LinkState s) {
    switch (s) {
        case LinkState::Awake:       return "AWAKE";
        case LinkState::Asleep:      return "ASLEEP";
        case LinkState::Idle:        return "IDLE";
        case LinkState::Unreachable: return "UNREACHABLE";
        default:                     return nullptr;  // Unknown ⇒ omit
    }
}

}  // namespace tk
