#pragma once

#include <cstdint>

// Pure, hardware-free decisions shared by firmware and host tests. No IDF/FreeRTOS/NimBLE/NVS/
// cJSON dependencies. runtime_fault_log.hpp delegates here for vehicle/display/LED catch paths.
namespace tk {

struct IterationFaultLog {
    std::uint64_t last_emit_ms{0};
    std::uint32_t suppressed{0};
    bool emitted{false};
};

struct IterationFaultReport {
    bool emit;
    std::uint32_t suppressed;
};

// State belongs to one task and survives successful iterations. A success/fault alternation must
// not bypass the limit. Advance before calling any sink; a failing sink consumes this interval too.
inline IterationFaultReport iteration_fault_note(IterationFaultLog& state,
                                                 std::uint64_t now_ms) noexcept {
    if (state.emitted && now_ms - state.last_emit_ms < 30000) {
        if (state.suppressed != UINT32_MAX) ++state.suppressed;
        return {false, 0};
    }
    const auto suppressed = state.suppressed;
    state.emitted = true;
    state.last_emit_ms = now_ms;
    state.suppressed = 0;
    return {true, suppressed};
}

}  // namespace tk
