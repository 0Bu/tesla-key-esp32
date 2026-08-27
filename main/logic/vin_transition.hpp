#pragma once

#include "key_rotation.hpp"
#include "nvs_string_load.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace tk {

struct VinTransitionMarker {
    std::string previous_vin;
    std::string previous_key_id;
};

enum class VinTransitionJournalAction : uint8_t {
    Continue,
    Recover,
    Halt,
};

// A present but malformed marker is not disposable configuration garbage: it is evidence that a
// cross-namespace identity transaction may be between commits. Preserve it and halt so boot never
// guesses which VIN/key tuple is authoritative. Only genuine NVS absence permits normal startup.
constexpr VinTransitionJournalAction decide_vin_transition_journal(
        NvsStringLoadState load_state, bool marker_valid) {
    if (load_state == NvsStringLoadState::Missing)
        return VinTransitionJournalAction::Continue;
    if (load_state == NvsStringLoadState::Present && marker_valid)
        return VinTransitionJournalAction::Recover;
    return VinTransitionJournalAction::Halt;
}

// An empty fingerprint means "no previous key" only when NVS independently proves the private-key
// record is absent. A present but unloadable key, a probe error, or a fingerprint/probe mismatch is
// ambiguous durable identity and must leave the journal armed for operator recovery.
constexpr bool vin_transition_key_evidence_verified(std::string_view previous_key_id,
                                                      std::string_view current_key_id,
                                                      bool key_probe_ok,
                                                      bool key_present) {
    if (!key_probe_ok) return false;
    const bool fingerprint_present = !current_key_id.empty();
    if (key_present != fingerprint_present) return false;
    if (!fingerprint_present && !previous_key_id.empty()) return false;
    return true;
}

inline bool vin_transition_key_id_valid(std::string_view key_id) {
    if (key_id.empty()) return true;  // a first VIN may legitimately have no previous key
    if (key_id.size() != 11 || key_id[2] != ':' || key_id[5] != ':' || key_id[8] != ':')
        return false;
    for (size_t i = 0; i < key_id.size(); ++i) {
        if (i == 2 || i == 5 || i == 8) continue;
        const char c = key_id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

// Versioned textual envelope stored with nvs_set_str:
//   VT1:<vin byte length>:<key-id byte length>:<VIN bytes><key-id bytes>
// Both lengths are explicit, so an arbitrary legacy VIN (including '|', ':', empty or otherwise
// invalid) round-trips without becoming framing. The key length also makes appended bytes a hard
// error instead of silently interpreting them as a key id.
inline constexpr std::string_view kVinTransitionMarkerPrefix = "VT1:";

inline std::string make_vin_transition_marker(std::string_view previous_vin,
                                              std::string_view previous_key_id) {
    if (!vin_transition_key_id_valid(previous_key_id)) return {};
    const std::string vin_len = std::to_string(previous_vin.size());
    const std::string key_len = std::to_string(previous_key_id.size());
    std::string marker;
    marker.reserve(kVinTransitionMarkerPrefix.size() + vin_len.size() + key_len.size() + 2 +
                   previous_vin.size() + previous_key_id.size());
    marker.append(kVinTransitionMarkerPrefix.data(), kVinTransitionMarkerPrefix.size());
    marker.append(vin_len);
    marker.push_back(':');
    marker.append(key_len);
    marker.push_back(':');
    marker.append(previous_vin.data(), previous_vin.size());
    marker.append(previous_key_id.data(), previous_key_id.size());
    return marker;
}

inline bool parse_vin_transition_length(std::string_view encoded, size_t& pos, size_t& value) {
    if (pos >= encoded.size()) return false;
    size_t parsed = 0;
    bool have_digit = false;
    while (pos < encoded.size() && encoded[pos] != ':') {
        const char c = encoded[pos++];
        if (c < '0' || c > '9') return false;
        have_digit = true;
        const size_t digit = static_cast<size_t>(c - '0');
        if (parsed > (std::numeric_limits<size_t>::max() - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    if (!have_digit || pos >= encoded.size() || encoded[pos] != ':') return false;
    ++pos;
    value = parsed;
    return true;
}

inline bool parse_vin_transition_marker(std::string_view encoded, VinTransitionMarker& out) {
    if (encoded.size() < kVinTransitionMarkerPrefix.size() ||
        encoded.substr(0, kVinTransitionMarkerPrefix.size()) != kVinTransitionMarkerPrefix)
        return false;
    size_t pos = kVinTransitionMarkerPrefix.size();
    size_t vin_len = 0;
    size_t key_len = 0;
    if (!parse_vin_transition_length(encoded, pos, vin_len) ||
        !parse_vin_transition_length(encoded, pos, key_len)) return false;
    const size_t remaining = encoded.size() - pos;
    if (vin_len > remaining || key_len != remaining - vin_len) return false;
    const std::string_view previous_vin = encoded.substr(pos, vin_len);
    const std::string_view previous_key_id = encoded.substr(pos + vin_len, key_len);
    // The writer emits either empty or exactly four uppercase hex bytes. Rejecting any other
    // logical value prevents an arbitrary-but-well-typed NVS string from becoming fingerprint
    // authority. The previous VIN is deliberately not plausibility-validated: this transaction is
    // also the supported migration path away from an empty or invalid legacy VIN.
    if (!vin_transition_key_id_valid(previous_key_id)) return false;
    VinTransitionMarker parsed;
    parsed.previous_vin.assign(previous_vin.data(), previous_vin.size());
    parsed.previous_key_id.assign(previous_key_id.data(), previous_key_id.size());
    out = std::move(parsed);
    return true;
}

enum class VinTransitionRecovery {
    ClearMarker,          // journal committed, but ConfigBlob was never changed (or was restored)
    RollBackPreviousVin,  // ConfigBlob changed, private key did not
    CompleteNewIdentity,  // private key changed; finish session/MAC cleanup
    HaltInconsistent,     // private key changed but ConfigBlob still names the previous VIN
};

// Result of the live /set_vin transaction while its staging callback and key rotation are held
// under the same command mutex. Unlike a fingerprint sampled after unlocking, this value belongs
// to exactly this request and cannot be reclassified by an interleaving auto-rekey.
enum class VinTransitionApply : uint8_t {
    IdentityUnverified,
    IdentityRecoveryPending,
    StageFailed,
    RollBackPreviousIdentity,
    RecoverAmbiguousIdentity,
    RecoverCommittedIdentity,
    Complete,
};

constexpr bool vin_transition_recovery_blocks_staging(bool key_reload_required,
                                                       bool pairing_cleanup_pending) {
    return key_reload_required || pairing_cleanup_pending;
}

constexpr VinTransitionApply decide_vin_transition_apply(bool identity_verified,
                                                          bool staged,
                                                          KeyRotationResult rotation) {
    if (!identity_verified) return VinTransitionApply::IdentityUnverified;
    if (!staged) return VinTransitionApply::StageFailed;
    switch (rotation) {
        case KeyRotationResult::NotCommitted:
            return VinTransitionApply::RollBackPreviousIdentity;
        case KeyRotationResult::CommitUnknown:
            // Keep the staged VIN and vin_txn. Boot reloads the durable key and uses its
            // fingerprint to choose rollback-old versus complete-new without guessing here.
            return VinTransitionApply::RecoverAmbiguousIdentity;
        case KeyRotationResult::CleanupPending:
            return VinTransitionApply::RecoverCommittedIdentity;
        case KeyRotationResult::Complete:
            return VinTransitionApply::Complete;
    }
    return VinTransitionApply::IdentityUnverified;
}

inline VinTransitionRecovery decide_vin_transition_recovery(
        const VinTransitionMarker& marker,
        std::string_view configured_vin,
        std::string_view current_key_id) {
    const bool key_changed = current_key_id != marker.previous_key_id;
    const bool vin_changed = configured_vin != marker.previous_vin;
    if (!key_changed && !vin_changed) return VinTransitionRecovery::ClearMarker;
    if (!key_changed && vin_changed)  return VinTransitionRecovery::RollBackPreviousVin;
    if (key_changed && vin_changed)   return VinTransitionRecovery::CompleteNewIdentity;
    return VinTransitionRecovery::HaltInconsistent;
}

}  // namespace tk
