#pragma once

// Internal header for the VehicleController implementation files ONLY
// (vehicle_ctrl.cpp — init/link_state glue, vehicle_commands.cpp — command dispatch,
// vehicle_telemetry.cpp — parsers/caches/background poll, vehicle_pairing.cpp —
// pairing lifecycle/keys; see .claude/CLAUDE.md "Architecture"). The public API
// stays vehicle_ctrl.hpp.

#include <atomic>

#include "rtos_guard.hpp"

namespace tk {
// RAII: marks a foreground command/query "in flight" (cmd_in_flight_) for as long as it
// is being sent + awaited, so loop_task pauses injecting background telemetry polls
// behind it. Clears on every exit path — early return or a throw from the library call.
// Users construct it only after taking command_mutex_, so at most one exists at a time.
struct InFlightGuard {
    std::atomic<bool>& flag;
    explicit InFlightGuard(std::atomic<bool>& f) : flag(f) { flag.store(true); }
    ~InFlightGuard() { flag.store(false); }
    InFlightGuard(const InFlightGuard&) = delete;
    InFlightGuard& operator=(const InFlightGuard&) = delete;
};
}  // namespace tk
