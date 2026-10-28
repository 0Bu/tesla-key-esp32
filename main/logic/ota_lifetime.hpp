#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace tk::ota {
// Preserve the full-word acq_rel RMW, including when every requested bit is already set.
// Non-lock-free targets reuse the CAS primitive already needed by the transport state machine.
// Failed strong CAS refreshes observed; recompute desired to preserve concurrent phase/flags.
template <class AtomicWord>
inline void publish_control_bits(AtomicWord& control, std::uint32_t bits) noexcept {
    if constexpr (AtomicWord::is_always_lock_free) {
        control.fetch_or(bits, std::memory_order_acq_rel);
    } else {
        auto observed = control.load(std::memory_order_acquire);
        while (!control.compare_exchange_strong(observed, observed | bits,
                std::memory_order_acq_rel, std::memory_order_acquire)) {}
    }
}
// Pure, hardware-free logic shared by firmware and host tests; no IDF/RTOS/NVS/cJSON.
// ota_update.cpp and heap_supervisor.cpp delegate here. Ownership: test/test_logic.cpp.
// Policy parameters below are admission bounds, not measured throughput/allocation guarantees.
inline constexpr std::uint64_t kCriticalWriterGraceMs = 300000;
inline constexpr std::uint64_t kMetadataBudgetBytes = 2u * 8192u;
struct TimePolicy {
    std::uint64_t setup_ms;
    std::uint64_t finalize_ms;
    std::uint32_t minimum_bytes_per_second;
};
inline constexpr TimePolicy kOperationTimePolicy{15000,10000,16u*1024u};
class Budget {
public:
    bool begin(std::uint64_t now, std::uint64_t maximum_bytes, TimePolicy p) noexcept {
        if (initialized_) return false; // No caller can renew this operation's origin.
        initialized_ = true;
        valid_ = false;
        started_ = now;
        last_ = now;
        maximum_ = maximum_bytes;
        policy_ = p;
        if (!p.minimum_bytes_per_second || !maximum_bytes) return false;
        std::uint64_t duration = 0;
        if (!duration_for(maximum_bytes, duration) || duration > UINT64_MAX - now) return false;
        deadline_ = now + duration;
        valid_ = true;
        return true;
    }
    bool live(std::uint64_t now) noexcept {
        if (!valid_ || now < last_ || now < started_ || now >= deadline_) {
            valid_ = false;
            return false;
        }
        last_ = now;
        return true;
    }
    bool tighten(std::uint64_t size) noexcept {
        if (!valid_ || size == 0 || size > maximum_) return false;
        std::uint64_t duration = 0;
        if (!duration_for(size, duration) || duration > UINT64_MAX - started_) return false;
        const auto candidate = started_ + duration;
        if (candidate < deadline_) deadline_ = candidate;
        return true;
    }
    std::uint64_t deadline() const noexcept { return deadline_; }
    std::uint64_t started() const noexcept { return started_; }
private:
    bool duration_for(std::uint64_t bytes, std::uint64_t& result) const noexcept {
        if (bytes > UINT64_MAX / 1000) return false;
        const auto scaled = bytes * 1000;
        const auto rate = policy_.minimum_bytes_per_second;
        const auto transfer = scaled / rate + (scaled % rate != 0);
        if (policy_.setup_ms > UINT64_MAX - transfer) return false;
        const auto first = policy_.setup_ms + transfer;
        if (policy_.finalize_ms > UINT64_MAX - first) return false;
        result = first + policy_.finalize_ms;
        return result != 0;
    }
    TimePolicy policy_{};
    std::uint64_t started_{0}, last_{0}, deadline_{0}, maximum_{0};
    bool valid_{false}, initialized_{false};
};

// One continuous critical episode. A new operation/generation never resets it.
class CriticalEpisode {
public:
    bool excused(std::uint64_t now, std::size_t internal_largest,
                 bool active_writer, std::uint64_t grace_ms) noexcept {
        // Only an actual healthy INTERNAL observation ends this continuous episode.
        if (internal_largest >= 4096) {
            active_ = invalid_clock_ = expired_ = false;
            since_ = last_ = grace_ = 0;
            return false;
        }
        if (!active_) {
            active_ = true;
            since_ = last_ = now;
            grace_ = grace_ms;
        } else {
            if (now < last_) invalid_clock_ = true;
            if (grace_ms < grace_) grace_ = grace_ms; // A caller/generation cannot renew grace.
        }
        last_ = now;
        if (now < since_) invalid_clock_ = true;
        if (!invalid_clock_ && now - since_ >= grace_) expired_ = true;
        if (invalid_clock_ || expired_) return false;
        return active_writer;
    }
    std::uint64_t elapsed(std::uint64_t now) const noexcept {
        return active_ && now >= since_ ? now - since_ : 0;
    }
private:
    std::uint64_t since_{0}, last_{0}, grace_{0};
    bool active_{false}, invalid_clock_{false}, expired_{false};
};

} // namespace tk::ota
