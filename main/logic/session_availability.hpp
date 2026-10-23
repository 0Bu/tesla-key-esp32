#pragma once

#include <cstdint>

// Pure, hardware-free session availability and persistence metadata. Native controller code
// publishes bounded cache snapshots; durable `paired` remains a separate record-presence fact.
namespace tk {

enum class SessionSaveState : uint8_t { Unknown, Unconfirmed, Confirmed, Error };
struct SessionRuntimeState {
    bool known{false};
    bool usable{false};
    SessionSaveState save{SessionSaveState::Unknown};
};
struct SessionAvailability {
    SessionRuntimeState vcsec;
    SessionRuntimeState infotainment;
};
inline constexpr SessionRuntimeState session_runtime_observation(bool private_key_loaded,
        bool valid, bool shared_secret, bool epoch_nonzero, bool runner_authenticated) noexcept {
    return {private_key_loaded,
            private_key_loaded && valid && shared_secret && epoch_nonzero && runner_authenticated,
            SessionSaveState::Unknown};
}
inline constexpr uint32_t encode_session_runtime(SessionRuntimeState state) noexcept {
    return (state.known ? 1U : 0U) | (state.known && state.usable ? 2U : 0U) |
           (static_cast<uint32_t>(state.save) << 2);
}
inline constexpr SessionRuntimeState decode_session_runtime(uint32_t bits) noexcept {
    return {(bits & 1U) != 0, (bits & 3U) == 3U,
            static_cast<SessionSaveState>((bits >> 2) & 3U)};
}
inline constexpr bool session_can_operate(bool identity_ready, bool stored,
                                          bool vcsec_available) noexcept {
    return identity_ready && (stored || vcsec_available);
}
inline constexpr const char* session_save_state_name(SessionSaveState state) noexcept {
    switch (state) {
        case SessionSaveState::Unknown: return "unknown";
        case SessionSaveState::Unconfirmed: return "unconfirmed";
        case SessionSaveState::Confirmed: return "confirmed";
        case SessionSaveState::Error: return "error";
    }
    return "unknown";
}
}  // namespace tk
