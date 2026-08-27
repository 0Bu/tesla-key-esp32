#pragma once

// Hardware-free sequencing shared by mqtt_ha.cpp and the host fault-injection gate. A false result
// or an exception re-arms discovery exactly once; later stages are never called after a failure.
namespace tk {

template <typename Discovery, typename Availability, typename State, typename Rearm>
bool mqtt_run_discovery_round(Discovery&& discovery,
                              Availability&& availability,
                              State&& state,
                              Rearm&& rearm) {
    bool success = false;
    try {
        success = discovery() && availability() && state();
    } catch (...) {
        rearm();
        throw;
    }
    if (!success) rearm();
    return success;
}

template <typename State, typename Rearm>
bool mqtt_run_state_round(State&& state, Rearm&& rearm) {
    bool success = false;
    try {
        success = state();
    } catch (...) {
        rearm();
        throw;
    }
    if (!success) rearm();
    return success;
}

}  // namespace tk
