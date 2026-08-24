#include "stack_watch.hpp"

#include <atomic>
#include <cstddef>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace tk {
namespace {

// Static storage rather than heap: this diagnostic must remain usable while allocation is failing.
// Each slot has exactly one writer (the owning task) and any number of readers. A reader racing a
// sample sees either the previous or the new monotonically lower value; both are valid observations.
std::atomic<uint32_t> s_min_free_bytes[static_cast<size_t>(StackWatch::Count)] = {};
std::atomic<bool> s_sampled[static_cast<size_t>(StackWatch::Count)] = {};

}  // namespace

void stack_watch_sample(StackWatch which) noexcept {
    const size_t i = static_cast<size_t>(which);
    if (i >= static_cast<size_t>(StackWatch::Count)) return;

    const uint32_t free_bytes = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    if (!s_sampled[i].load(std::memory_order_relaxed)) {
        s_min_free_bytes[i].store(free_bytes, std::memory_order_relaxed);
        s_sampled[i].store(true, std::memory_order_release);
        return;
    }

    const uint32_t previous = s_min_free_bytes[i].load(std::memory_order_relaxed);
    if (free_bytes < previous) {
        s_min_free_bytes[i].store(free_bytes, std::memory_order_relaxed);
    }
}

std::optional<uint32_t> stack_watch_min_free_bytes(StackWatch which) noexcept {
    const size_t i = static_cast<size_t>(which);
    if (i >= static_cast<size_t>(StackWatch::Count) ||
        !s_sampled[i].load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    return s_min_free_bytes[i].load(std::memory_order_relaxed);
}

}  // namespace tk
