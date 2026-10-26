#include "stack_watch.hpp"

#include <atomic>
#include <cstddef>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace tk {
namespace {
// Packed presence and value are published in one atomic. Multiple callback/task contexts may
// share a word; strong CAS must recompute after failure rather than losing a neighboring lane.
std::atomic<uint32_t> s_samples[stack_sample::kWordCount]{};
static_assert(sizeof(s_samples) == 24);
#if defined(CONFIG_IDF_TARGET_ESP32C3) && CONFIG_IDF_TARGET_ESP32C3
// IDF implements the single-core C3's 32-bit atomics by briefly masking interrupts.
// Those bounded POD operations allocate nothing and do not wait on a task mutex.
static_assert(configNUM_CORES == 1);
#else
static_assert(std::atomic<uint32_t>::is_always_lock_free);
#endif
}

void stack_watch_sample(StackWatch which) noexcept {
    const auto slot = static_cast<unsigned>(which);
    if (slot >= kStackContextCount) return;
    const uint32_t free_bytes = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    auto& word = s_samples[slot / stack_sample::kLanesPerWord];
    stack_sample::publish_minimum(word, slot % stack_sample::kLanesPerWord, free_bytes);
}

std::optional<uint32_t> stack_watch_min_free_bytes(StackWatch which) noexcept {
    const auto slot = static_cast<unsigned>(which);
    if (slot >= kStackContextCount) return std::nullopt;
    const auto word = s_samples[slot / stack_sample::kLanesPerWord].load(std::memory_order_relaxed);
    const unsigned shift = (slot % stack_sample::kLanesPerWord) * stack_sample::kLaneBits;
    const auto sample = stack_sample::decode(static_cast<uint8_t>((word >> shift) & stack_sample::kLaneMask));
    return sample.sampled ? std::optional<uint32_t>(sample.lower_bound_bytes) : std::nullopt;
}

stack_sample::Snapshot stack_watch_snapshot() noexcept {
    stack_sample::Snapshot snapshot{};
    for (unsigned i = 0; i < stack_sample::kWordCount; ++i)
        snapshot[i] = s_samples[i].load(std::memory_order_relaxed);
    return snapshot;
}
}  // namespace tk
