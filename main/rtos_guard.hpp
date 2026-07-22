#pragma once

// Shared, exception-safe RAII ownership for a FreeRTOS semaphore / mutex.
//
// Manual xSemaphoreTake()/xSemaphoreGive() pairs are NOT unwind-safe: any throw, early
// return, or later maintenance edit inserted between the two calls leaks the lock, and a
// silently-held mutex wedges every subsequent user of it until power-cycle. C++ exceptions
// are enabled in this firmware (see .claude/CLAUDE.md "Memory is tight …"), and the
// tesla-ble builders/parsers, std::string/std::vector/std::function, and the cJSON/MQTT
// payload builders can all throw std::bad_alloc while the largest contiguous free block is
// exhausted — so a throw between a take and its give is a real, reachable failure, not a
// theoretical one.
//
// This guard takes on construction and gives on destruction, so release happens on the
// normal return path AND while a C++ exception unwinds the stack. It is the ONE reusable
// guard the exception-capable paths (vehicle command/query, the shared tesla-ble instance,
// BLE/scan state, the WebSocket registries, OTA status, Syslog status, the diag ring, the
// telemetry/pairing caches) share, so semaphore ownership is never split back into a manual
// take/give around code that can throw or return early.
//
// Lives in the project's tk:: namespace on purpose: a stock name like "LockGuard" or
// "MutexGuard" in the global namespace would collide, one-definition-rule, with an
// identically-named struct in any linked component — an IFNDR clash with no diagnostic.
//
// Two acquisition modes, so the SAME type serves blocking task contexts and non-blocking
// callback contexts (the issue's requirement: "support finite/zero waits for callback
// contexts that must not block and expose whether acquisition succeeded"):
//
//   * Blocking (default, portMAX_DELAY) — for task/HTTP contexts that may wait for the lock.
//         tk::SemGuard g(mtx);
//         // … guarded work …            // released on scope exit or unwind
//
//   * Bounded / zero wait — for callback contexts (the NimBLE host task, esp_event handlers,
//     the esp-mqtt event callback) that MUST NOT block indefinitely. Pass a timeout in ticks
//     (0 == try-lock). ALWAYS check acquired()/operator bool before touching the guarded
//     state; on a failed acquire the guard owns nothing and its destructor gives nothing back.
//         tk::SemGuard g(mtx, 0);        // try-lock, never blocks the callback
//         if (g) { … guarded work … }    // skip the work if the lock was contended
//
// A null handle acquires nothing (acquired() == false), so a guard over an optional or
// not-yet-created mutex degrades safely instead of dereferencing null — matching the existing
// `if (!cache_mutex_) return src;` style null-guards this code already uses.
//
// Deliberately NOT heap-heavy: one pointer + one bool, no allocation (the acceptance criteria
// forbid a new heap-heavy synchronization wrapper).

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace tk {

class SemGuard {
public:
    // Blocking acquire (portMAX_DELAY). acquired() is true unless `sem` was null.
    explicit SemGuard(SemaphoreHandle_t sem)
        : sem_(sem),
          held_(sem != nullptr && xSemaphoreTake(sem, portMAX_DELAY) == pdTRUE) {}

    // Bounded / zero-wait acquire for non-blocking (callback) contexts. `ticks` == 0 is a
    // try-lock. Check acquired() before using the guarded state.
    SemGuard(SemaphoreHandle_t sem, TickType_t ticks)
        : sem_(sem),
          held_(sem != nullptr && xSemaphoreTake(sem, ticks) == pdTRUE) {}

    ~SemGuard() { if (held_) xSemaphoreGive(sem_); }

    SemGuard(const SemGuard&)            = delete;
    SemGuard& operator=(const SemGuard&) = delete;
    SemGuard(SemGuard&&)                 = delete;
    SemGuard& operator=(SemGuard&&)      = delete;

    // True iff this guard currently owns the lock (took it and will release it on scope exit).
    bool acquired() const { return held_; }
    explicit operator bool() const { return held_; }

private:
    SemaphoreHandle_t sem_;
    bool              held_;
};

}  // namespace tk
