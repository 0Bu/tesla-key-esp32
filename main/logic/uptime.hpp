#pragma once

#include <cstdint>

// Pure, hardware-free logic shared by firmware and host tests. Keep this header free of
// ESP-IDF/FreeRTOS/NimBLE/NVS/cJSON includes. vehicle_telemetry.cpp delegates its long-lived
// diagnostic and bootstrap clocks here using the 64-bit ESP timer, rather than RTOS ticks.
namespace tk {

inline constexpr uint32_t uptime_seconds(uint64_t microseconds) noexcept {
    const uint64_t seconds = microseconds / 1000000u;
    return seconds > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(seconds);
}

}  // namespace tk
