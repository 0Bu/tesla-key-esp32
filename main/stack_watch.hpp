#pragma once

#include "logic/stack_context.hpp"
#include <cstdint>
#include <optional>

namespace tk {
using StackWatch = StackContext;
// Call only from a task context, including SDK callbacks known to have a current task.
// Never inspect a foreign task handle. Missing/unsafe/unexercised contexts remain unknown.
void stack_watch_sample(StackWatch which) noexcept;

// A conservative256-byte lower bound on successfully sampled historical free stack.
// An engaged zero is measured; nullopt is unknown. Saturation is at least15872B.
std::optional<uint32_t> stack_watch_min_free_bytes(StackWatch which) noexcept;
// Each word is an atomic observation; the six-word snapshot is not a global instant.
stack_sample::Snapshot stack_watch_snapshot() noexcept;

// Use at task-backed callback exits, declared before local lock guards so they release first.
// Infinite task bodies require explicit iteration/terminal samples; no-unwind deletion skips it.
class StackWatchScope {
public:
    explicit StackWatchScope(StackWatch which) noexcept : which_(which) {}
    ~StackWatchScope() noexcept { stack_watch_sample(which_); }
    StackWatchScope(const StackWatchScope&) = delete;
    StackWatchScope& operator=(const StackWatchScope&) = delete;
private:
    StackWatch which_;
};
}  // namespace tk
