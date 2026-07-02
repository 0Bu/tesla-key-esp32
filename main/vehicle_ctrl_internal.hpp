#pragma once

// Internal header for the VehicleController implementation files ONLY
// (vehicle_ctrl.cpp — init/link_state glue, vehicle_commands.cpp — command dispatch,
// vehicle_telemetry.cpp — parsers/caches/background poll, vehicle_pairing.cpp —
// pairing lifecycle/keys; see .claude/CLAUDE.md "Architecture"). The public API
// stays vehicle_ctrl.hpp.

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
}  // namespace tk
