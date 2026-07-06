#pragma once

// Internal header for the VehicleController implementation files ONLY
// (vehicle_ctrl.cpp — init/link_state glue, vehicle_commands.cpp — command dispatch,
// vehicle_telemetry.cpp — parsers/caches/background poll, vehicle_pairing.cpp —
// pairing lifecycle/keys; see .claude/CLAUDE.md "Architecture"). The public API
// stays vehicle_ctrl.hpp.

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// RAII guard that serializes a full command/query cycle (command_mutex_) or a cache
// copy (cache_mutex_). Lives in the project's tk:: namespace: a stock utility name like
// MutexGuard in the global namespace would be one same-named struct in any linked
// component away from an IFNDR ODR clash with no diagnostic.
namespace tk {
struct MutexGuard {
    SemaphoreHandle_t m;
    explicit MutexGuard(SemaphoreHandle_t mtx) : m(mtx) { xSemaphoreTake(m, portMAX_DELAY); }
    ~MutexGuard() { xSemaphoreGive(m); }
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;
};

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
