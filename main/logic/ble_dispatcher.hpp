#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Pure, hardware-free BLE message dispatcher shared by firmware and the host mock build.
// Single source of truth for routing Tesla BLE responses according to normative reference:
// teslamotors/vehicle-command internal/dispatcher/dispatcher.go:245-315.
//
// Key Characteristics:
// - Routes incoming responses to registered outstanding requests by UUID and domain.
// - VCSEC UUID match exemption: domain DOMAIN_VEHICLE_SECURITY matches outstanding VCSEC
//   handlers regardless of response request UUID (modelled on dispatcher.go:259-261).
// - Per-request anti-replay window: monotonic / sliding window counter tracking per outstanding
//   request rather than a global shared slot.
// - Replay protection applies uniformly to authenticated and plaintext responses.
// - Bounded static array of outstanding requests (zero heap allocation, safe for ESP32 largest
//   contiguous free block).
namespace tk {

enum class BleDomain : uint8_t {
    None = 0,
    VehicleSecurity = 2,
    Infotainment = 3,
};

inline constexpr size_t kBleUuidSize = 16;
using BleUuid = std::array<uint8_t, kBleUuidSize>;

inline bool is_zero_uuid(const BleUuid& u) noexcept {
    for (uint8_t b : u) {
        if (b != 0) return false;
    }
    return true;
}

inline bool uuids_equal(const uint8_t* a, const uint8_t* b, size_t len) noexcept {
    if (len != kBleUuidSize) return false;
    return std::memcmp(a, b, kBleUuidSize) == 0;
}

// 64-bit sliding window anti-replay tracker (modelled on vehicle-command sliding window)
class AntiReplayWindow {
public:
    static constexpr uint32_t kWindowSize = 64;

    constexpr AntiReplayWindow() noexcept = default;

    [[nodiscard]] bool is_valid(uint32_t counter) const noexcept {
        if (!used_) return true;
        if (counter > highest_counter_) return true;
        if (highest_counter_ - counter >= kWindowSize) return false;
        const uint32_t offset = highest_counter_ - counter;
        return (window_ & (1ULL << offset)) == 0;
    }

    bool add(uint32_t counter) noexcept {
        if (!used_) {
            highest_counter_ = counter;
            window_ = 1ULL;
            used_ = true;
            return true;
        }

        if (counter > highest_counter_) {
            const uint32_t shift = counter - highest_counter_;
            if (shift >= kWindowSize) {
                window_ = 1ULL;
            } else {
                window_ = (window_ << shift) | 1ULL;
            }
            highest_counter_ = counter;
            return true;
        }

        if (highest_counter_ - counter >= kWindowSize) {
            return false;
        }

        const uint32_t offset = highest_counter_ - counter;
        const uint64_t mask = 1ULL << offset;
        if (window_ & mask) {
            return false; // Already seen: replay!
        }

        window_ |= mask;
        return true;
    }

    void reset() noexcept {
        window_ = 0;
        highest_counter_ = 0;
        used_ = false;
    }

    [[nodiscard]] bool is_initialized() const noexcept { return used_; }
    [[nodiscard]] uint32_t highest_counter() const noexcept { return highest_counter_; }

private:
    uint64_t window_{0};
    uint32_t highest_counter_{0};
    bool used_{false};
};

enum class DispatchDropReason : uint8_t {
    None = 0,
    MissingSource,
    InvalidUuidLength,
    Unmatched,
    Replay,
};

struct DispatchOutcome {
    bool routed{false};
    DispatchDropReason drop_reason{DispatchDropReason::None};
    uint32_t request_id{0};
    void* user_data{nullptr};
};

struct DispatcherStats {
    uint32_t routed_count{0};
    uint32_t unmatched_drops{0};
    uint32_t replay_drops{0};
    uint32_t invalid_uuid_drops{0};
    uint32_t missing_source_drops{0};
};

class BleDispatcher {
public:
    static constexpr size_t kMaxRequests = 8;

    struct RequestSlot {
        uint32_t id{0};
        BleDomain domain{BleDomain::None};
        BleUuid uuid{};
        void* user_data{nullptr};
        AntiReplayWindow replay_window{};
        uint32_t plaintext_count{0};
        bool active{false};
    };

    BleDispatcher() noexcept = default;

    void reset() noexcept {
        for (auto& slot : slots_) {
            slot = RequestSlot{};
        }
        active_count_ = 0;
    }

    // Register an outstanding request. Returns request_id > 0 on success, or 0 if full or duplicate.
    uint32_t register_request(BleDomain domain, const BleUuid& uuid, void* user_data = nullptr) noexcept {
        if (domain == BleDomain::None) return 0;

        // Reject duplicate active request for same domain with non-zero UUID
        if (!is_zero_uuid(uuid)) {
            for (const auto& slot : slots_) {
                if (slot.active && slot.domain == domain && uuids_equal(slot.uuid.data(), uuid.data(), kBleUuidSize)) {
                    return 0; // Duplicate active UUID
                }
            }
        }

        for (auto& slot : slots_) {
            if (!slot.active) {
                slot.id = next_request_id_++;
                if (next_request_id_ == 0) next_request_id_ = 1;
                slot.domain = domain;
                slot.uuid = uuid;
                slot.user_data = user_data;
                slot.replay_window.reset();
                slot.plaintext_count = 0;
                slot.active = true;
                active_count_++;
                return slot.id;
            }
        }
        return 0; // Capacity exhausted
    }

    bool unregister_request(uint32_t request_id) noexcept {
        if (request_id == 0) return false;
        for (auto& slot : slots_) {
            if (slot.active && slot.id == request_id) {
                slot = RequestSlot{};
                active_count_--;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool has_request(uint32_t request_id) const noexcept {
        if (request_id == 0) return false;
        for (const auto& slot : slots_) {
            if (slot.active && slot.id == request_id) return true;
        }
        return false;
    }

    [[nodiscard]] size_t active_count() const noexcept { return active_count_; }
    [[nodiscard]] const DispatcherStats& stats() const noexcept { return stats_; }

    // Route an incoming response to a registered outstanding request.
    // - from_domain: source domain of response.
    // - uuid_bytes / uuid_len: request UUID from response (can be empty / 0 bytes).
    // - has_counter: whether response carries an authenticated response counter.
    // - counter: response counter value (if has_counter).
    DispatchOutcome dispatch(BleDomain from_domain,
                             const uint8_t* uuid_bytes, size_t uuid_len,
                             bool has_counter, uint32_t counter) noexcept {
        // 1. Missing source check (dispatcher.go:248-251)
        if (from_domain != BleDomain::VehicleSecurity && from_domain != BleDomain::Infotainment) {
            stats_.missing_source_drops++;
            return {false, DispatchDropReason::MissingSource, 0, nullptr};
        }

        // 2. Request UUID length check (dispatcher.go:254-258: must be 0 or 16 bytes)
        if (uuid_len != 0 && uuid_len != kBleUuidSize) {
            stats_.invalid_uuid_drops++;
            return {false, DispatchDropReason::InvalidUuidLength, 0, nullptr};
        }

        // 3. Match outstanding request
        RequestSlot* matched_slot = nullptr;

        // Pass 1: exact UUID match across active slots in the same domain
        if (uuid_len == kBleUuidSize && uuid_bytes != nullptr) {
            for (auto& slot : slots_) {
                if (!slot.active || slot.domain != from_domain) continue;
                if (uuids_equal(slot.uuid.data(), uuid_bytes, kBleUuidSize)) {
                    matched_slot = &slot;
                    break;
                }
            }
        }

        // Pass 2: VCSEC UUID match exemption (dispatcher.go:259-261)
        // Only if response has no request UUID (empty UUID, 0 bytes), route to the first active VCSEC request handler.
        if (!matched_slot && uuid_len == 0 && from_domain == BleDomain::VehicleSecurity) {
            for (auto& slot : slots_) {
                if (slot.active && slot.domain == BleDomain::VehicleSecurity) {
                    matched_slot = &slot;
                    break;
                }
            }
        }

        if (!matched_slot) {
            stats_.unmatched_drops++;
            return {false, DispatchDropReason::Unmatched, 0, nullptr};
        }

        // 4. Per-request Anti-Replay Validation (dispatcher.go:302-308 & ADR-0005)
        // Uniform replay protection for authenticated and plaintext responses.
        if (has_counter) {
            if (!matched_slot->replay_window.add(counter)) {
                stats_.replay_drops++;
                return {false, DispatchDropReason::Replay, matched_slot->id, matched_slot->user_data};
            }
        } else {
            // Plaintext / unauthenticated response without counter:
            // At most one plaintext response per request. Repeated responses are rejected as replay.
            if (matched_slot->plaintext_count > 0) {
                stats_.replay_drops++;
                return {false, DispatchDropReason::Replay, matched_slot->id, matched_slot->user_data};
            }
            matched_slot->plaintext_count++;
        }

        stats_.routed_count++;
        return {true, DispatchDropReason::None, matched_slot->id, matched_slot->user_data};
    }

private:
    std::array<RequestSlot, kMaxRequests> slots_{};
    size_t active_count_{0};
    uint32_t next_request_id_{1};
    DispatcherStats stats_{};
};

}  // namespace tk
