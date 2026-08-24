#pragma once

// Allocation-free reporting for the second memory budget: FreeRTOS task stacks.
//
// The heap watchdog and /heap trend already expose heap exhaustion before and after a restart, but
// a growing task frame otherwise stays invisible until the stack watchpoint panics. Each watched
// task samples its OWN high-water mark; readers only consume the cached minimum and never inspect a
// foreign task handle. Sample presence is tracked separately so a measured zero remains visible;
// safe mode deliberately leaves the vehicle, auto-pair and MQTT tasks unsampled.

#include <cstdint>
#include <optional>

namespace tk {

enum class StackWatch : uint8_t {
    Httpd,
    Vehicle,
    AutoPair,
    Mqtt,
    Count,
};

// Must be called from the task named by `which`. ESP-IDF reports the calling task's historical
// minimum free stack in bytes (unlike upstream FreeRTOS, whose corresponding API uses words).
// Lock-free, allocation-free and safe on OOM/error paths.
void stack_watch_sample(StackWatch which) noexcept;

// Worst sampled headroom since boot, in bytes. nullopt means never sampled; an engaged zero is a
// valid and critical measurement.
std::optional<uint32_t> stack_watch_min_free_bytes(StackWatch which) noexcept;

}  // namespace tk
