#pragma once

#include <cstdint>
#include "nvs_contract.hpp"

namespace tk {

// Lives in the same tesla_ble namespace as the private key and peer sessions. NVS entry names
// are limited to 15 bytes, so keep this spelling short and pin its length in the host tests.
inline constexpr const char* kKeyRotationMarker = nvs_contract::kKeyRotation;

// NotCommitted proves the private key mutation was never attempted. CommitUnknown means the
// mutation was attempted but tesla-ble could not confirm the NVS commit: either the old or the
// new key may be durable, so only a reboot/reload may classify it. CleanupPending means the new
// key is durable but obsolete peer state or the journal itself is not yet durably erased. Only
// Complete permits signing/enrolment in this boot.
enum class KeyRotationResult : uint8_t {
    NotCommitted,
    CommitUnknown,
    CleanupPending,
    Complete,
};

constexpr bool key_rotation_committed(KeyRotationResult result) {
    return result == KeyRotationResult::CleanupPending ||
           result == KeyRotationResult::Complete;
}

constexpr bool key_rotation_runtime_safe(KeyRotationResult result) {
    return result == KeyRotationResult::Complete;
}

constexpr bool key_rotation_requires_reload(KeyRotationResult result) {
    return result == KeyRotationResult::CommitUnknown;
}

// The OTA health gate may be overridden only by a fully successful user operation. Recovery,
// ambiguous-commit and cleanup-incomplete reboots must leave PENDING_VERIFY armed so the
// bootloader can still roll back an image that has not completed its probation.
constexpr bool key_rotation_reboot_confirms_ota(KeyRotationResult result) {
    return result == KeyRotationResult::Complete;
}

enum class InitialKeyBootAction : uint8_t {
    UseExisting,
    Generate,
    HaltAfterEmptyRecovery,
};

// A CommitUnknown-triggered reboot gets exactly one chance to classify storage. If boot cleaned
// the rotation marker but still loaded no durable key, generating again immediately could repeat
// the same ambiguous write/reboot forever. Halt this recovery boot; an external reset may retry.
constexpr InitialKeyBootAction decide_initial_key_boot_action(bool key_exists,
                                                               bool recovered_rotation) {
    if (key_exists) return InitialKeyBootAction::UseExisting;
    return recovered_rotation ? InitialKeyBootAction::HaltAfterEmptyRecovery
                              : InitialKeyBootAction::Generate;
}

enum class VehicleTaskStartPhase : uint8_t {
    ControllerWired,
    IdentityResolved,
    BleReady,
    EssentialServicesReady,
};

// Vehicle loop/auto-pair are mutating tasks. They may start only after VIN/key recovery and every
// essential initializer that can still call boot_fatal have completed. Safe mode never starts
// them, even though its HTTP recovery surface reaches the terminal initialization phase.
constexpr bool vehicle_tasks_may_start(VehicleTaskStartPhase phase, bool safe_mode) {
    return !safe_mode && phase == VehicleTaskStartPhase::EssentialServicesReady;
}

enum class KeyRotationBootState : uint8_t {
    Ready,
    CleanupRequired,
    Blocked,
};

enum class KeyRotationMarkerProbe : uint8_t {
    Error,
    Missing,
    Present,
};

enum class KeyGenerationPreflight : uint8_t {
    Proceed,
    ExistingKeyRefused,
    ProbeFailed,
};

constexpr KeyGenerationPreflight decide_key_generation_preflight(bool allow_replace,
                                                                  bool probe_ok,
                                                                  bool key_exists) {
    if (allow_replace) return KeyGenerationPreflight::Proceed;
    if (!probe_ok) return KeyGenerationPreflight::ProbeFailed;
    return key_exists ? KeyGenerationPreflight::ExistingKeyRefused
                      : KeyGenerationPreflight::Proceed;
}

constexpr bool private_key_identity_verified(bool probe_ok,
                                             bool key_exists,
                                             bool fingerprint_available) {
    return probe_ok && (!key_exists || fingerprint_available);
}

enum class AutomaticKeyAction : uint8_t {
    Continue,
    Generate,
    StorageUnavailable,
    RebootRequired,
};

constexpr AutomaticKeyAction decide_automatic_key_action(bool probe_ok,
                                                          bool key_exists,
                                                          bool runtime_safe,
                                                          bool pairing_lost) {
    if (!probe_ok) return AutomaticKeyAction::StorageUnavailable;
    // An existing durable key with an unsafe in-memory identity must be reloaded before a
    // revocation flag can authorize replacement. Otherwise a CommitUnknown result followed by
    // pairing_lost=true would repeatedly mutate the key without ever classifying the first NVS
    // commit. Verified first-boot absence remains the sole unsafe-runtime generation case.
    if (key_exists && !runtime_safe) return AutomaticKeyAction::RebootRequired;
    if (pairing_lost) return AutomaticKeyAction::Generate;
    if (!key_exists) {
        return runtime_safe ? AutomaticKeyAction::RebootRequired
                            : AutomaticKeyAction::Generate;
    }
    return runtime_safe ? AutomaticKeyAction::Continue
                        : AutomaticKeyAction::RebootRequired;
}

constexpr KeyRotationMarkerProbe classify_key_rotation_marker_probe(bool probe_ok,
                                                                     bool marker_exists) {
    if (!probe_ok) return KeyRotationMarkerProbe::Error;
    return marker_exists ? KeyRotationMarkerProbe::Present
                         : KeyRotationMarkerProbe::Missing;
}

// Pure boot-recovery contract used by VehicleController::init(). A pending marker may only be
// retired after every persisted session record was erased successfully. A torn/failed cleanup
// therefore leaves init blocked; the Vehicle object is never constructed and cannot load/sign
// with a key/session combination whose transaction did not reach its durable terminal state.
constexpr KeyRotationBootState decide_key_rotation_boot(bool marker_present,
                                                         bool cleanup_attempted,
                                                         bool cleanup_succeeded,
                                                         bool marker_removed) {
    if (!marker_present) return KeyRotationBootState::Ready;
    if (!cleanup_attempted) return KeyRotationBootState::CleanupRequired;
    return cleanup_succeeded && marker_removed ? KeyRotationBootState::Ready
                                               : KeyRotationBootState::Blocked;
}

}  // namespace tk
