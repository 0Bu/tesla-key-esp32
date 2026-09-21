#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Pure, hardware-free BLE session tracker.
// Single source of truth for Tesla BLE session state mirror according to normative reference:
// teslamotors/vehicle-command pkg/protocol/signer.go (internal/authentication/signer.go:90-135).
//
// Key Invariants:
// - Crypto stays upstream in yoziru/tesla-ble (Client / Peer / Nanopb). No in-house crypto.
// - Monotonic counter alignment: max(local, reported) per signer.go.
//   Preserves higher local counter if vehicle reports lower counter without epoch change,
//   avoiding session stranding.
// - Session age validation (rejects sessions older than 3600 seconds on NVS load).
namespace tk {

enum class SessionState : uint8_t {
    Unauthenticated,
    Authenticating,
    Established,
    Invalid,
};

class SessionTracker {
public:
    static constexpr uint32_t kDefaultMaxSessionAgeS = 3600;

    SessionTracker() noexcept = default;

    void reset() noexcept {
        state_ = SessionState::Unauthenticated;
        epoch_.fill(0);
        counter_ = 0;
        clock_time_ = 0;
        handshake_uuid_.fill(0);
        has_handshake_ = false;
    }

    // Start handshake: records challenge UUID and transitions to Authenticating
    void start_handshake(const std::array<uint8_t, 16>& request_uuid) noexcept {
        handshake_uuid_ = request_uuid;
        has_handshake_ = true;
        state_ = SessionState::Authenticating;
    }

    [[nodiscard]] bool has_pending_handshake() const noexcept { return has_handshake_; }
    [[nodiscard]] const std::array<uint8_t, 16>& handshake_uuid() const noexcept { return handshake_uuid_; }

    // Direct setter for session state, aligning counter monotonically per signer.go: max(local, reported)
    void set_established(const std::array<uint8_t, 16>& epoch, uint32_t counter, uint32_t clock_time) noexcept {
        epoch_ = epoch;
        counter_ = std::max(counter_, counter);
        clock_time_ = clock_time;
        state_ = SessionState::Established;
        has_handshake_ = false;
    }

    // Allocate next TX counter for an outgoing command. Increments monotonically.
    bool next_tx_counter(uint32_t& out_counter) noexcept {
        if (state_ != SessionState::Established) return false;
        if (counter_ == 0xFFFFFFFF) return false; // Rollover rejection per signer.go:171
        counter_++;
        out_counter = counter_;
        return true;
    }

    // Session age validation (vehicle.cpp:1165: reject if > 3600s old)
    [[nodiscard]] bool validate_session_age(uint32_t current_time_s,
                                           uint32_t max_age_s = kDefaultMaxSessionAgeS) const noexcept {
        if (current_time_s > 0 && clock_time_ > 0) {
            int64_t age = static_cast<int64_t>(current_time_s) - static_cast<int64_t>(clock_time_);
            if (age < 0 || age > static_cast<int64_t>(max_age_s)) {
                return false;
            }
        }
        return state_ == SessionState::Established;
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
    std::array<uint8_t, 16> handshake_uuid_{};
    bool has_handshake_{false};
};

} // namespace tk
