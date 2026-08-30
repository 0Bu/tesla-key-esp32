#pragma once

#include <atomic>
#include <cstdint>

// Generation ownership for asynchronous esp_ping completion. Hardware-free so late and
// out-of-order callback cases are fault-injected by the host mock suite.
namespace tk {

class PingProbeGeneration {
public:
    // Returns zero while another generation is still live.
    std::uint32_t begin() {
        if (active_.load(std::memory_order_acquire) != 0) return 0;
        std::uint32_t generation = next_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (generation == 0) {
            generation = next_.fetch_add(1, std::memory_order_relaxed) + 1;
        }
        replies_.store(0, std::memory_order_relaxed);
        measurement_valid_.store(false, std::memory_order_relaxed);
        ended_.store(0, std::memory_order_relaxed);
        active_.store(generation, std::memory_order_release);
        return generation;
    }

    // Called only by on_ping_end. A callback from an older/retired generation is ignored and may
    // not wake the current waiter.
    bool complete(std::uint32_t generation, std::uint32_t replies,
                  bool measurement_valid = true) {
        if (generation == 0 || active_.load(std::memory_order_acquire) != generation) return false;
        replies_.store(replies, std::memory_order_relaxed);
        measurement_valid_.store(measurement_valid, std::memory_order_relaxed);
        ended_.store(generation, std::memory_order_release);
        return true;
    }

    bool ended(std::uint32_t generation) const {
        return generation != 0 && ended_.load(std::memory_order_acquire) == generation &&
               active_.load(std::memory_order_acquire) == generation;
    }

    bool result(std::uint32_t generation, std::uint32_t& replies) const {
        bool measurement_valid = false;
        return result(generation, replies, measurement_valid) && measurement_valid;
    }

    bool result(std::uint32_t generation, std::uint32_t& replies,
                bool& measurement_valid) const {
        if (!ended(generation)) return false;
        replies = replies_.load(std::memory_order_acquire);
        measurement_valid = measurement_valid_.load(std::memory_order_acquire);
        return true;
    }

    bool retire(std::uint32_t generation) {
        if (!ended(generation)) return false;
        std::uint32_t expected = generation;
        return active_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }

    // Only for a session that never started and therefore cannot call on_ping_end.
    bool abandon_unstarted(std::uint32_t generation) {
        if (generation == 0 || ended_.load(std::memory_order_acquire) == generation) return false;
        std::uint32_t expected = generation;
        return active_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }

    std::uint32_t active() const { return active_.load(std::memory_order_acquire); }

private:
    std::atomic<std::uint32_t> next_{0};
    std::atomic<std::uint32_t> active_{0};
    std::atomic<std::uint32_t> ended_{0};
    std::atomic<std::uint32_t> replies_{0};
    std::atomic<bool> measurement_valid_{false};
};

}  // namespace tk
