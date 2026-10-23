#pragma once

#include "logic/iteration_fault.hpp"

namespace tk {

// The task supplies its monotonic clock and bounded logger. No sink exception may escape a catch
// path, and no retry of a failed sink may evade the task-owned reporting deadline.
template <typename Sink>
void report_iteration_fault(IterationFaultLog& state, std::uint64_t now_ms, Sink&& sink) noexcept {
    const auto report = iteration_fault_note(state, now_ms);
    if (!report.emit) return;
    try {
        sink(report.suppressed);
    } catch (...) {
        // Preserve the already-advanced deadline without recursively logging the sink failure.
    }
}

}  // namespace tk
