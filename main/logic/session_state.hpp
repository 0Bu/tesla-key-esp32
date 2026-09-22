#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Pure, hardware-free BLE session tracker mirror.
// Reflects authenticated session presence and counter progression for Tesla BLE sessions.
// Normative reference: teslamotors/vehicle-command pkg/protocol/signer.go (monotonic counter).
//
// Key Invariants:
// - Crypto, session derivation, and Nanopb protos stay upstream in yoziru/tesla-ble.
// - Monotonic counter alignment: max(local, reported) per signer.go.
//   Preserves higher local counter if vehicle reports lower counter without epoch change.
namespace tk {

enum class SessionState : uint8_t {
    Unauthenticated,
    Authenticating,
    Established,
    Invalid,
};

class SessionTracker {
public:
    SessionTracker() noexcept = default;

    void reset() noexcept {
        state_ = SessionState::Unauthenticated;
        epoch_.fill(0);
        counter_ = 0;
        clock_time_ = 0;
    }

    // Direct setter for session state, aligning counter monotonically per signer.go: max(local, reported)
    void set_established(const std::array<uint8_t, 16>& epoch, uint32_t counter, uint32_t clock_time) noexcept {
        epoch_ = epoch;
        counter_ = std::max(counter_, counter);
        clock_time_ = clock_time;
        state_ = SessionState::Established;
    }

    [[nodiscard]] bool is_authenticated() const noexcept { return state_ == SessionState::Established; }
    [[nodiscard]] SessionState state() const noexcept { return state_; }
    [[nodiscard]] uint32_t counter() const noexcept { return counter_; }
    [[nodiscard]] uint32_t clock_time() const noexcept { return clock_time_; }
    [[nodiscard]] const std::array<uint8_t, 16>& epoch() const noexcept { return epoch_; }

private:
    SessionState state_{SessionState::Unauthenticated};
    std::array<uint8_t, 16> epoch_{};
    uint32_t counter_{0};
    uint32_t clock_time_{0};
};

} // namespace tk
