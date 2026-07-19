#pragma once
// Which BLE phase the web UI's Bluetooth row counts down, and how long is left in it.
// Pure, IDF-free, host-tested (test/test_logic.cpp). Fed by VehicleController::ble_phase()
// from two deadlines, and surfaced as /status ble.phase + ble.phase_s.
//
// The row has two states worth a countdown, and they are owned by different tasks:
//   Connecting — a scan/connect attempt is running and GIVES UP at connect_deadline
//                (ensure_connected_'s timeout — every command, poll and health probe).
//   Waiting    — no attempt is running; the next one STARTS at retry_deadline
//                (auto_pair_task_fn_'s idle wait between VCSEC health polls).
//
// Two rules make the countdown trustworthy, and both exist because breaking them is what
// made the first attempt at this feature misreport:
//
//   1. Connecting outranks Waiting. The two overlap routinely — a command, or loop_task's
//      warm-up connect, starts an attempt in the middle of auto-pair's idle wait. The
//      attempt is the more specific truth about what the radio is doing right now, and
//      because the deadlines are stored separately (never cleared by each other), the idle
//      wait's countdown simply reappears when the attempt ends instead of the row going bare.
//
//   2. Seconds round UP, and 0 is a real answer, not "no countdown". A phase reads its full
//      length the moment it starts and only reaches 0 when the deadline has actually
//      arrived. Truncating instead would drop the last second, and treating 0 as "nothing to
//      show" would blank the countdown for the whole gap between one phase ending and the
//      next becoming visible — which is precisely how a "Disconnected (in ~Ns)" row ended up
//      flashing a bare "Disconnected" every cycle.
//
// Deadlines are FreeRTOS tick counts with 0 as the "not armed" sentinel. The remaining time
// is computed on the SIGNED difference so it stays correct across a tick-counter wrap
// (~49 days at 1 kHz), where an unsigned `deadline - now` would read as an enormous countdown.
#include <cstdint>

namespace tk {
namespace ble {

enum class Phase : uint8_t { None = 0, Connecting, Waiting };

struct PhaseView {
    Phase    kind{Phase::None};
    uint32_t secs{0};   // meaningful only when kind != None; 0 means "right now"
};

// Whole seconds from `now` until `deadline`, rounded up; 0 once the deadline has passed.
inline constexpr uint32_t secs_left(uint32_t deadline, uint32_t now, uint32_t tick_hz) {
    const int32_t left = (int32_t)(deadline - now);
    if (left <= 0) return 0;
    return ((uint32_t)left + tick_hz - 1) / tick_hz;
}

// The one phase to report, from the two independently-armed deadlines.
inline constexpr PhaseView phase(uint32_t connect_deadline, uint32_t retry_deadline,
                                 uint32_t now, uint32_t tick_hz) {
    if (connect_deadline) return { Phase::Connecting, secs_left(connect_deadline, now, tick_hz) };
    if (retry_deadline)   return { Phase::Waiting,    secs_left(retry_deadline,   now, tick_hz) };
    return {};
}

// The /status spelling of a phase. Empty string ⇒ omit ble.phase AND ble.phase_s.
inline constexpr const char* phase_name(Phase p) {
    return p == Phase::Connecting ? "connecting"
         : p == Phase::Waiting    ? "waiting"
                                  : "";
}

} // namespace ble
} // namespace tk
