#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace tk {

struct VinTransitionMarker {
    std::string previous_vin;
    std::string previous_key_id;
};

inline std::string make_vin_transition_marker(std::string_view previous_vin,
                                              std::string_view previous_key_id) {
    std::string marker;
    marker.reserve(previous_vin.size() + previous_key_id.size() + 1);
    marker.append(previous_vin.data(), previous_vin.size());
    marker.push_back('|');
    marker.append(previous_key_id.data(), previous_key_id.size());
    return marker;
}

inline bool parse_vin_transition_marker(std::string_view encoded, VinTransitionMarker& out) {
    const size_t sep = encoded.find('|');
    if (sep == std::string_view::npos || encoded.find('|', sep + 1) != std::string_view::npos)
        return false;
    VinTransitionMarker parsed;
    parsed.previous_vin.assign(encoded.data(), sep);
    parsed.previous_key_id.assign(encoded.data() + sep + 1, encoded.size() - sep - 1);
    out = std::move(parsed);
    return true;
}

enum class VinTransitionRecovery {
    ClearMarker,          // journal committed, but ConfigBlob was never changed (or was restored)
    RollBackPreviousVin,  // ConfigBlob changed, private key did not
    CompleteNewIdentity,  // private key changed; finish session/MAC cleanup
};

inline VinTransitionRecovery decide_vin_transition_recovery(
        const VinTransitionMarker& marker,
        std::string_view configured_vin,
        std::string_view current_key_id) {
    if (current_key_id != marker.previous_key_id)
        return VinTransitionRecovery::CompleteNewIdentity;
    if (configured_vin != marker.previous_vin)
        return VinTransitionRecovery::RollBackPreviousVin;
    return VinTransitionRecovery::ClearMarker;
}

}  // namespace tk
