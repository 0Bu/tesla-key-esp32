#pragma once
#include "logic/ota_lifetime.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tk::ota {
inline constexpr std::size_t kBlockBytes = 1024;
inline constexpr std::size_t kUrlBytes = 1024;
enum class Phase : std::uint32_t { Idle, Requested, Reading, Full, Borrowed, Closed, Acked, CleanupFailed };
inline constexpr std::uint32_t kPhaseMask = 255, kRetired = 256, kClose = 512;
enum class NetResult : std::uint8_t { Data, Eof, NetworkError, NoMemory, Deadline, InvalidUrl };
struct Job {
    Job(std::uint32_t gen, std::uint64_t origin, std::uint64_t end) noexcept
        : generation(gen), started_ms(origin), deadline_ms(end) {}
    const std::uint32_t generation;
    const std::uint64_t started_ms, deadline_ms;
    std::atomic<std::uint32_t> control{static_cast<std::uint32_t>(Phase::Idle)};
    std::array<char, kUrlBytes> url{}; // Coordinator writes only in Idle/Closed.
    std::array<std::uint8_t, kBlockBytes> block{};
    std::size_t bytes{0};
    NetResult result{NetResult::NetworkError};
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    Phase phase() const noexcept { return static_cast<Phase>(control.load(std::memory_order_acquire) & kPhaseMask); }
    bool retired() const noexcept { return (control.load(std::memory_order_acquire) & kRetired) != 0; }
    void retire() noexcept { publish_control_bits(control, kRetired); }
    void close_connection() noexcept { publish_control_bits(control, kClose); }
    bool request(const char* text) noexcept {
        const auto old = control.load(std::memory_order_acquire);
        const auto p = static_cast<Phase>(old & kPhaseMask);
        if ((old & kRetired) || (p != Phase::Idle && p != Phase::Closed) || !text) return false;
        std::size_t n = 0;
        while (n < url.size() && text[n]) ++n;
        if (!n || n == url.size()) return false;
        std::memcpy(url.data(), text, n + 1);
        control.store(static_cast<std::uint32_t>(Phase::Requested), std::memory_order_release);
        return true;
    }
    bool borrow() noexcept {
        std::uint32_t expected = static_cast<std::uint32_t>(Phase::Full);
        return control.compare_exchange_strong(expected, static_cast<std::uint32_t>(Phase::Borrowed),
            std::memory_order_acq_rel, std::memory_order_acquire);
    }
    bool release() noexcept {
        auto previous = control.load(std::memory_order_acquire);
        for (;;) {
            if ((previous & kPhaseMask) != static_cast<std::uint32_t>(Phase::Borrowed)) return false;
            const auto next = (previous & ~kPhaseMask) | static_cast<std::uint32_t>(Phase::Reading);
            if (control.compare_exchange_strong(previous, next, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) return true;
        }
    }
};
// Fixed INFO checkpoints in the existing diagnostic ring. No retained measurement state.
// The four-argument overload reports the exact already-read guard sample; diagnostics never
// replace or repeat that policy read. All reporters contain sampling/logging failures.
void memory_checkpoint(const char* phase, std::uint32_t generation,
                       std::uint64_t started_ms) noexcept;
void memory_checkpoint(const char* phase, std::uint32_t generation,
                       std::uint64_t started_ms, std::size_t guard_largest) noexcept;
// No FreeRTOS call is hidden here. Later target task wrappers supply these bounded waits.
struct NetRuntime {
    void* context;
    std::uint64_t (*now_ms)(void*) noexcept;
    std::size_t (*largest_internal)(void*) noexcept;
    void (*pause_ms)(void*, unsigned) noexcept;
};
// Actual public ESP-TLS calls are in worker.cpp. No writer, NVS, status or restart capability.
void run_network_worker(Job&, const NetRuntime&) noexcept;
// Ended worker is not equivalent to freed RTOS TCB/stack; scheduler reclamation is separate.
inline bool worker_clean(const Job& j) noexcept { return j.phase() == Phase::Acked; }
// ota_update.cpp owns exactly one slot across every public check/update launch path.
// An acknowledged generation may retire it; a stalled/failed cleanup cannot free the slot.
class Slot {
public:
    bool claim(std::uint32_t generation) noexcept {
        if (!generation) return false;
        std::uint32_t empty = 0;
        return owner_.compare_exchange_strong(empty,generation,std::memory_order_acq_rel);
    }
    bool owns(const Job& j) const noexcept {
        return j.generation != 0 && owner_.load(std::memory_order_acquire) == j.generation;
    }
    bool release_after_ack(const Job& j, bool writer_quiescent) noexcept {
        if (!worker_clean(j) || !writer_quiescent) return false;
        auto expected = j.generation;
        return owner_.compare_exchange_strong(expected,0,std::memory_order_acq_rel);
    }
private:
    std::atomic<std::uint32_t> owner_{0};
};
} // namespace tk::ota

// RTOS wrapper gets only Job; after run_network_worker Ack it uses global own-HWM/selfdelete.
namespace tk::ota { void ota_network_task(void*); }
