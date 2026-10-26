#pragma once
#include <cstddef>

// Pure, hardware-free logic shared by firmware and host tests. No IDF/RTOS/NVS.
// mqtt_uri.hpp delegates its existing MQTT admission here. OTA ordinary-entry recovery
// and its distinct record-capacity screen are owned by ota_recovery_memory.hpp.
namespace tk {
// Existing MQTT TLS-attempt policy. Provisional: this is neither
// an allocation reservation nor a measured minimum that guarantees a TLS session.
inline constexpr std::size_t kTlsAttemptPolicyFloorB = 48u * 1024u;
inline constexpr bool tls_attempt_affordable(std::size_t internal_largest_bytes) noexcept {
    return internal_largest_bytes >= kTlsAttemptPolicyFloorB;
}
}  // namespace tk
