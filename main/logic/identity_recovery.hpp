#pragma once
// Pure, hardware-free logic shared by firmware and the host-side mock build.
// Keep IDF/FreeRTOS/NimBLE/NVS/cJSON out of this header. HTTP/MQTT recovery fields and
// vehicle_pairing.cpp delegate here. Host owner: test/test_logic.cpp; see test/README.md.
// Platform ownership, raw metadata and journal probes stay in the integration shell.
#include "logic/runtime_admission.hpp"
#include <cstdint>
#include <cstring>

namespace tk {
enum class IdentityRecoveryState : std::uint8_t {
    Unknown, Ok, RebootRequired, CleanupPending, VinTransition, StorageUnavailable,
};
inline IdentityRecoveryState identity_recovery_state(const char* reason,
        RuntimeAdmissionState runtime) noexcept {
    if (!reason) return runtime == RuntimeAdmissionState::Ready
        ? IdentityRecoveryState::Ok : IdentityRecoveryState::Unknown;
    if (std::strcmp(reason, "key_reload_required") == 0 ||
        std::strcmp(reason, "key_runtime_mismatch") == 0) return IdentityRecoveryState::RebootRequired;
    if (std::strcmp(reason, "key_cleanup_pending") == 0) return IdentityRecoveryState::CleanupPending;
    if (std::strcmp(reason, "vin_transition_pending") == 0) return IdentityRecoveryState::VinTransition;
    if (std::strcmp(reason, "key_storage_unavailable") == 0) return IdentityRecoveryState::StorageUnavailable;
    return IdentityRecoveryState::Unknown;
}
constexpr const char* identity_recovery_state_name(IdentityRecoveryState state) noexcept {
    switch (state) {
        case IdentityRecoveryState::Ok: return "ok";
        case IdentityRecoveryState::RebootRequired: return "reboot_required";
        case IdentityRecoveryState::CleanupPending: return "cleanup_pending";
        case IdentityRecoveryState::VinTransition: return "vin_transition";
        case IdentityRecoveryState::StorageUnavailable: return "storage_unavailable";
        default: return "unknown";
    }
}
struct IdentityReloadAdmission {
    bool ready{};
    bool automatic_commit{};
    bool reload_required{};
    bool vin_pending{};
    bool cleanup_pending{};
    bool recovered_boot{};
    bool stable{};
    bool journal_readable{};
    bool journal_present{};
};
constexpr bool identity_reload_allowed(const IdentityReloadAdmission& in) noexcept {
    return in.ready && in.automatic_commit && in.reload_required && !in.vin_pending &&
           !in.cleanup_pending && !in.recovered_boot && in.stable && in.journal_readable &&
           in.journal_present;
}
class IdentityReloadOnce {
public:
    bool reserve(const IdentityReloadAdmission& in) noexcept {
        if (reserved_ || !identity_reload_allowed(in)) return false;
        reserved_ = true; // before any diagnostic, yield or restart effect
        return true;
    }
    bool reserved() const noexcept { return reserved_; }
private:
    bool reserved_{}; // task-local; the existing durable key_rotate journal bounds the next boot
};
static_assert(sizeof(IdentityReloadOnce) == 1, "one task-local byte, no global/NVS allowance");
} // namespace tk
