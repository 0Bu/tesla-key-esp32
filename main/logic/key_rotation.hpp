#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
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
// therefore leaves init blocked; the controller cannot complete initialization and cannot load/sign
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

// Outcome of the boot-time erase of an interrupted rotation's persisted state.
struct KeyRotationBootCleanup {
    bool vcsec_removed = false;
    bool info_removed = false;
    bool paired_removed = false;
    bool date_removed = false;
    bool marker_removed = false;  // attempted only after every erase above succeeded

    // Every record that could pair the new key with the old key's identity is gone.
    constexpr bool peers_erased() const {
        return vcsec_removed && info_removed && paired_removed && date_removed;
    }
    constexpr KeyRotationBootState state() const {
        return decide_key_rotation_boot(true, true, peers_erased(), marker_removed);
    }
};

// The production sequence behind VehicleController::recover_pending_key_rotation_at_boot_(),
// templated on the storage so the host tests drive the real NvsStorageAdapter fault injection
// through exactly this code rather than a copy of it. Every erase is attempted and commits on its
// own, so power loss at any boundary re-enters this idempotent sequence on the next boot. The
// journal is the retry authority and goes LAST: it is cleared only when every erase succeeded.
// `key_created` is erased unconditionally: an interrupted rotation cannot prove whether the stored
// date names the old key or an already-stamped new one, and a date that outlives its key is shown
// beside the wrong fingerprint.
template <typename Storage>
KeyRotationBootCleanup run_key_rotation_boot_cleanup(Storage& storage) {
    KeyRotationBootCleanup cleanup;
    cleanup.vcsec_removed = storage.remove(nvs_contract::kSessionVcsec);
    cleanup.info_removed = storage.remove(nvs_contract::kSessionInfotainment);
    cleanup.paired_removed = storage.remove(nvs_contract::kPairedAt);
    cleanup.date_removed = storage.remove(nvs_contract::kKeyCreated);
    if (cleanup.peers_erased()) cleanup.marker_removed = storage.remove(kKeyRotationMarker);
    return cleanup;
}

enum class KeyRotationJournalRetire : uint8_t {
    Retired,        // date (if untrusted) and journal are both durably gone
    DatePending,    // an untrusted key_created could not be erased; the journal stays armed
    JournalPending, // the date is fine but the journal erase failed; the journal stays armed
};

// Last step of a runtime rotation, after the peer sessions were cleared. A key_created that could
// not be retired or stamped earlier (`date_untrusted`) still holds the PREVIOUS key's date and
// would reappear beside the new fingerprint after a reboot, so it is erased first and the journal
// only after it. `date_untrusted` is cleared only once that erase committed.
template <typename Storage>
KeyRotationJournalRetire retire_key_rotation_journal(Storage& storage, bool& date_untrusted) {
    if (date_untrusted) {
        if (!storage.remove(nvs_contract::kKeyCreated)) return KeyRotationJournalRetire::DatePending;
        date_untrusted = false;
    }
    return storage.remove(kKeyRotationMarker) ? KeyRotationJournalRetire::Retired
                                              : KeyRotationJournalRetire::JournalPending;
}

// tesla-ble's Client::get_private_key() exports PEM (mbedtls_pk_write_key_pem): about 228 B for
// a P-256 SEC1 key including the terminating NUL. Upstream persist_private_key_() uses 2048 B;
// a smaller export buffer makes every export fail (#314 review, B2).
inline constexpr size_t kPrivateKeyPemCapacity = 2048;

enum class KeyRegenerationResult : uint8_t {
    Committed,             // new key created, exported and persisted: RAM matches storage
    ExportExistingFailed,  // existing key not exportable: runtime identity left untouched
    CreateFailed,          // creation failed: previous in-memory key restored (if any)
    ExportNewFailed,       // new key not exportable: previous in-memory key restored (if any)
    PersistFailed,         // persistence failed: previous in-memory key restored (if any)
};

// Fail-closed private-key regeneration over the tesla-ble Client contract (has_private_key,
// get_private_key, create_private_key, load_private_key). `persist` receives the exported PEM
// (including its NUL, as tesla-ble stores it) and returns whether it was saved. The runtime
// identity is never replaced unless the previous key could be exported for a rollback. This is
// the production transaction behind VehicleController::regenerate_key_native_(); the
// real-library harness (test/test_tesla_ble_harness.cpp) runs exactly this template.
template <typename Client, typename Persist>
KeyRegenerationResult regenerate_private_key(Client& client, Persist&& persist,
                                             size_t capacity = kPrivateKeyPemCapacity) {
    struct SecureWipe {
        std::vector<uint8_t>& buf;
        ~SecureWipe() {
            if (!buf.empty()) {
                volatile uint8_t* p = buf.data();
                for (size_t i = 0; i < buf.size(); ++i) p[i] = 0;
            }
        }
    };
    std::vector<uint8_t> old_key;
    SecureWipe wipe_old{old_key};
    bool had_old = false;
    if (client.has_private_key()) {
        old_key.resize(capacity);
        size_t old_len = old_key.size();
        if (client.get_private_key(old_key.data(), old_key.size(), &old_len) != 0 ||
            old_len == 0 || old_len > old_key.size()) {
            return KeyRegenerationResult::ExportExistingFailed;
        }
        old_key.resize(old_len);
        had_old = true;
    }
    auto restore_old = [&]() {
        if (had_old) (void)client.load_private_key(old_key.data(), old_key.size());
    };
    if (client.create_private_key() != 0) {
        restore_old();
        return KeyRegenerationResult::CreateFailed;
    }
    std::vector<uint8_t> new_key(capacity);
    SecureWipe wipe_new{new_key};
    size_t new_len = new_key.size();
    if (client.get_private_key(new_key.data(), new_key.size(), &new_len) != 0 ||
        new_len == 0 || new_len > new_key.size()) {
        restore_old();
        return KeyRegenerationResult::ExportNewFailed;
    }
    new_key.resize(new_len);
    if (!persist(static_cast<const std::vector<uint8_t>&>(new_key))) {
        restore_old();
        return KeyRegenerationResult::PersistFailed;
    }
    return KeyRegenerationResult::Committed;
}

}  // namespace tk
