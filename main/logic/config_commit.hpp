#pragma once

#include <cstdint>
#include "logic/health_gate.hpp"

// Pure configuration-commit results. ota_update.cpp maps the raw SDK state under the
// request's held operation lease; HTTP/portal replies preserve that result after an effect.
// No IDF, RTOS, storage or transport dependencies. Host owner: test/test_logic.cpp.
namespace tk {

enum class ConfigImageResult : uint8_t {
    None, Valid, Undefined, Confirmed, Pending, New, Unsupported, Unknown,
    NotReady, HostUnsynced, HostReset, CriticalHeap, MarkUnknown, WrongOwner, WrongClass
};

// None means the local health sample permits an attempt, never that SDK confirmation succeeded.
constexpr ConfigImageResult config_health_failure(RuntimeHealthFailure failure) noexcept {
    switch (failure) {
        case RuntimeHealthFailure::None: return ConfigImageResult::None;
        case RuntimeHealthFailure::NotReady: return ConfigImageResult::NotReady;
        case RuntimeHealthFailure::HostUnsynced: return ConfigImageResult::HostUnsynced;
        case RuntimeHealthFailure::HostReset: return ConfigImageResult::HostReset;
        case RuntimeHealthFailure::CriticalHeap: return ConfigImageResult::CriticalHeap;
    }
    return ConfigImageResult::Unknown;
}

constexpr bool config_wifi_may_save(ConfigImageResult image) noexcept {
    return image == ConfigImageResult::Valid || image == ConfigImageResult::Undefined;
}

constexpr bool config_image_warning(ConfigImageResult image) noexcept {
    return image != ConfigImageResult::None && !config_wifi_may_save(image) &&
           image != ConfigImageResult::Confirmed;
}

inline const char* config_image_code(ConfigImageResult image) noexcept {
    switch (image) {
        case ConfigImageResult::None: return "not_attempted";
        case ConfigImageResult::Valid: return "already_valid";
        case ConfigImageResult::Undefined: return "not_in_probation";
        case ConfigImageResult::Confirmed: return "confirmed";
        case ConfigImageResult::Pending: return "pending";
        case ConfigImageResult::New: return "new";
        case ConfigImageResult::Unsupported: return "unsupported_state";
        case ConfigImageResult::NotReady: return "runtime_not_ready";
        case ConfigImageResult::HostUnsynced: return "host_unsynced";
        case ConfigImageResult::HostReset: return "host_reset";
        case ConfigImageResult::CriticalHeap: return "critical_heap";
        case ConfigImageResult::MarkUnknown: return "mark_outcome_unknown";
        case ConfigImageResult::WrongOwner: return "wrong_owner";
        case ConfigImageResult::WrongClass: return "wrong_reboot_class";
        case ConfigImageResult::Unknown: return "state_unknown";
    }
    return "state_unknown";
}

// Used only after a proven configuration save, including degraded replies. Never claim
// that an unconfirmed image is guaranteed to roll back or that a failed mark had no effect.
inline const char* config_commit_reason(ConfigImageResult image) noexcept {
    switch (image) {
        case ConfigImageResult::Pending:
        case ConfigImageResult::New:
            return "configuration saved for the next boot; firmware remains unconfirmed";
        case ConfigImageResult::NotReady:
            return "configuration saved for the next boot; firmware unconfirmed: runtime not ready";
        case ConfigImageResult::HostUnsynced:
            return "configuration saved for the next boot; firmware unconfirmed: BLE host not synced";
        case ConfigImageResult::HostReset:
            return "configuration saved for the next boot; firmware unconfirmed: BLE host reset";
        case ConfigImageResult::CriticalHeap:
            return "configuration saved for the next boot; firmware unconfirmed: insufficient OTA recovery headroom";
        default:
            return "configuration saved for the next boot; firmware verification is uncertain";
    }
}

inline constexpr const char* kConfigRestartBusyReason = "another operation is active; retry later";
inline constexpr const char* kConfigWifiVerificationReason =
    "firmware verification is pending or unavailable; nothing was changed";

}  // namespace tk
