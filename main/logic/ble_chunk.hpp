#pragma once

#include <cstddef>
#include <cstdint>

// Pure, hardware-free logic shared by the firmware and the host-side mock build
// (test/, built without ESP-IDF). Keep this file free of IDF/FreeRTOS/NimBLE includes.
// Single source of truth — BleClient's MTU callback delegates the write payload size here.
namespace tk {

inline constexpr size_t kBleDefaultWritePayload = 20;
inline constexpr size_t kBleMaxWritePayload = 244;  // preferred ATT MTU 247 minus 3-byte header

inline constexpr size_t ble_write_payload_for_mtu(uint16_t mtu) {
    if (mtu <= 3) return kBleDefaultWritePayload;
    const size_t payload = static_cast<size_t>(mtu - 3);
    if (payload < kBleDefaultWritePayload) return kBleDefaultWritePayload;
    return payload > kBleMaxWritePayload ? kBleMaxWritePayload : payload;
}

}  // namespace tk
