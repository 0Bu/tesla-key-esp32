#pragma once

#include <cstdint>
#include <limits>

// Pure, hardware-free logic shared by the firmware and the host-side mock build
// (test/, built without ESP-IDF). Keep this file free of IDF/FreeRTOS/NimBLE/NVS/
// cJSON/esp_http_server includes. The pairing supervisor delegates its enrollment cadence here.
namespace tk {

inline constexpr uint32_t kEnrollmentRetryFirstMs = 60000;
inline constexpr uint32_t kEnrollmentRetryMaxMs = 300000;

struct AutoEnrollmentState {
    uint64_t next_attempt_ms = 0;
    uint32_t retry_ms = kEnrollmentRetryFirstMs;
};

struct AutoEnrollmentSession {
    bool storage_readable = false;
    bool stored = false;
    bool runtime_readable = false;
    bool runtime_ready = false;

    bool observed() const noexcept { return storage_readable && runtime_readable; }
    bool present() const noexcept { return stored || runtime_ready; }
};

// A recovery hint does not prove that the vehicle removed a key. It only requests a bounded
// retry with the same stored key; it never authorizes local key generation or session deletion.
inline bool auto_enrollment_needed(const AutoEnrollmentSession& session,
                                   bool recovery_hint) noexcept {
    return session.observed() && (!session.present() || recovery_hint);
}

// Storage must be readable for both first pairing and recovery. The supervisor probes status
// before each attempt and the sender rechecks identity, session state and OTA ownership.
inline bool auto_enrollment_due(const AutoEnrollmentState& state, uint64_t now_ms,
                                const AutoEnrollmentSession& session,
                                bool recovery_hint,
                                bool identity_ready, bool operation_allowed) noexcept {
    return state.next_attempt_ms != std::numeric_limits<uint64_t>::max() &&
           auto_enrollment_needed(session, recovery_hint) &&
           identity_ready && operation_allowed &&
           now_ms >= state.next_attempt_ms;
}

// Reserve before I/O: a thrown allocation, timeout or unconfirmed transmission also observes
// backoff. Each later attempt first probes for a session again; no attempt retries BLE writes.
inline void auto_enrollment_started(AutoEnrollmentState& state, uint64_t now_ms) noexcept {
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    state.next_attempt_ms = now_ms > maximum - state.retry_ms
        ? maximum : now_ms + state.retry_ms;
    state.retry_ms = state.retry_ms >= kEnrollmentRetryMaxMs / 2
        ? kEnrollmentRetryMaxMs : state.retry_ms * 2;
}

}  // namespace tk
