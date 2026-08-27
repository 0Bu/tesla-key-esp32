#pragma once

#include <cstdint>

// Pure, hardware-free logic shared by the firmware and the host-side mock build
// (test/, built without ESP-IDF). Keep this file free of IDF/FreeRTOS/NimBLE/NVS/
// cJSON/esp_http_server includes so it compiles with a plain host toolchain.
// Single source of truth — VehicleController's deferred NimBLE event consumer delegates here.
namespace tk {

enum class BleDeferredEventKind : std::uint8_t {
    LinkUp,
    LinkDown,
    Rx,
};

// LinkDown events are ordered barriers in the fixed FIFO and must be applied even when the host
// has already published a later generation: the following LinkUp then rebuilds library state in
// order. LinkUp/Rx may only touch the Vehicle instance for the exact current stable generation.
inline constexpr bool ble_deferred_event_may_apply(BleDeferredEventKind kind,
                                                    std::uint32_t event_generation,
                                                    std::uint32_t current_generation) {
    if (kind == BleDeferredEventKind::LinkDown) return true;
    return (event_generation & 1U) == 0U && event_generation == current_generation;
}

}  // namespace tk
