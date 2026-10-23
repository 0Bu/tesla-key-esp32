#pragma once

#include <cstdint>

// Pure, hardware-free logic shared by the firmware and the host-side mock build
// (test/, built without ESP-IDF). Keep this file free of IDF/FreeRTOS/NimBLE/NVS/
// cJSON/esp_http_server includes so it compiles with a plain host toolchain.
// VehicleController::capped_deadline_ supplies one clock snapshot and delegates here.
namespace tk {

// Deadlines and caps are less than 2^31 ticks from now. Zero is the controller's
// unarmed sentinel; preserve deadline_in_'s one-tick nudge when the cap wraps to zero.
inline constexpr uint32_t capped_command_deadline(uint32_t deadline, uint32_t now,
                                                  uint32_t max_ticks) {
    const uint32_t candidate = now + max_ticks;
    const uint32_t cap = candidate ? candidate : 1;
    return static_cast<int32_t>(deadline - now) <= static_cast<int32_t>(cap - now)
        ? deadline : cap;
}

}  // namespace tk
