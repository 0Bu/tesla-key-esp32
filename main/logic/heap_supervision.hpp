#pragma once

#include "logic/boot_guard.hpp"
#include "logic/heap_watchdog.hpp"

#include <atomic>
#include <cstdint>

// Pure, hardware-free recovery policy. No IDF/FreeRTOS/NimBLE/NVS/cJSON dependency.
// The independent heap supervisor delegates here; test/test_logic.cpp owns its host cases.
namespace tk {

inline constexpr std::uint64_t kRecoverySampleMs = 30000;
inline constexpr std::uint64_t kRecoveryFreshMs = 60000;
inline constexpr std::uint64_t kRecoveryHealthyMs = std::uint64_t{kBootHealthyS} * 1000;
inline constexpr std::uint64_t kRecoveryRetryMs = 300000;

inline bool vehicle_iteration_successful(bool runner_failed, bool parse_fault) noexcept {
    return !runner_failed && !parse_fault;
}

// One writer publishes successful Vehicle iterations. Readers acquire no Vehicle/application
// lock; this does not promise a lock-free implementation of 64-bit atomics on every target.
// An unseen, future or stale observation cannot qualify a healthy recovery interval.
class VehicleProgress {
public:
    void publish(std::uint64_t now_ms) noexcept {
        last_ms_.store(now_ms, std::memory_order_release);
        present_.store(true, std::memory_order_release);
    }

    bool fresh(std::uint64_t now_ms) const noexcept {
        if (!present_.load(std::memory_order_acquire)) return false;
        const auto observed = last_ms_.load(std::memory_order_acquire);
        return now_ms >= observed && now_ms - observed <= kRecoveryFreshMs;
    }

private:
    std::atomic<std::uint64_t> last_ms_{0};
    std::atomic<bool> present_{false};
};

// This qualifies decay and crash-counter maintenance only. It does not replace the existing
// critical-heap hold, which deliberately uses elapsed time even when its caller is late.
struct RecoveryHealthyRun {
    std::uint64_t since_ms{0};
    std::uint64_t last_ms{0};
    bool observed{false};
    bool active{false};

    bool sample(std::uint64_t now_ms, bool eligible) noexcept {
        if (!eligible || (observed && (now_ms < last_ms || now_ms - last_ms > kRecoveryFreshMs))) {
            active = false;
        }
        last_ms = now_ms;
        observed = true;
        if (!eligible) return false;
        if (!active) {
            active = true;
            since_ms = now_ms;
        }
        return now_ms - since_ms >= kRecoveryHealthyMs;
    }
};

struct RecoveryHealth {
    bool ready;
    bool host_synced;
    bool host_reset_free;
    bool vehicle_progress_fresh;
    bool network_up;
    bool ota_busy;
    std::size_t largest_internal;
};

inline bool recovery_health_eligible(const RecoveryHealth& health) noexcept {
    return health.ready && health.host_synced && health.host_reset_free &&
           health.vehicle_progress_fresh && health.network_up && !health.ota_busy &&
           health.largest_internal >= kHeapCriticalBytes;
}

// The boot record stays immutable. This task-local heap streak is diagnostic only: the
// BootGuard ledger must independently confirm each shared charge. Its conservative guard can
// refuse a malformed/exhausted streak but never grants credit. Decay follows durable shared
// zero; an error seed never becomes trusted because time passes.
struct HeapRecoveryBudget {
    std::uint8_t effective;
    bool trusted;

    static HeapRecoveryBudget seed(std::uint8_t count, bool trusted_seed) noexcept {
        if (!trusted_seed || count > kHeapMaxConsecutiveRestarts) {
            return {kHeapMaxConsecutiveRestarts, false};
        }
        return {count, true};
    }

    bool decay_after_healthy_run(bool qualified) noexcept {
        if (!qualified || !trusted || effective == 0) return false;
        effective = 0;
        return true;
    }

    bool may_restart() const noexcept { return trusted && heap_may_restart(effective); }
};

// Keep retries independent of diagnostic throttling. Advance before the fallible storage call;
// a false/throw/uncertain commit still consumes this interval. A busy lease consumes no write.
struct RecoveryWriteCadence {
    std::uint64_t last_ms{0};
    bool attempted{false};

    bool due(std::uint64_t now_ms) const noexcept {
        return !attempted || (now_ms >= last_ms && now_ms - last_ms >= kRecoveryRetryMs);
    }
    void attempt(std::uint64_t now_ms) noexcept { last_ms = now_ms; attempted = true; }
};

struct RecoveryDiagnostic {
    std::uint64_t last_ms{0};
    std::uint32_t suppressed{0};
    bool emitted{false};

    bool emit(std::uint64_t now_ms, std::uint32_t& suppressed_out) noexcept {
        if (emitted && (now_ms < last_ms || now_ms - last_ms < kRecoveryRetryMs)) {
            if (suppressed != UINT32_MAX) ++suppressed;
            suppressed_out = 0;
            return false;
        }
        suppressed_out = suppressed;
        suppressed = 0;
        emitted = true;
        last_ms = now_ms;
        return true;
    }
};

}  // namespace tk
