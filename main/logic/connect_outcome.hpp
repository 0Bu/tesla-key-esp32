#pragma once

#include <cstdint>

// Pure, hardware-free logic shared by the firmware and the host-side mock build
// (test/, built without ESP-IDF). Anything in this directory must stay free of IDF,
// FreeRTOS, NimBLE, NVS, cJSON and esp_http_server includes so it compiles with a
// plain host toolchain. See test/README.md and the project .claude/CLAUDE.md.
namespace tk {

// Why an ensure_connected_() window ended without a link, and how loudly to say so.
//
// The problem this solves, measured on the live device (syslog, 17.-24.07.2026): a parked
// car that is simply AWAY produced 7117 `E … connection timeout after 10000ms` lines in a
// week — one every 40 s, forever, at ERROR level, with no backoff. Two things were wrong
// with that, and they are separable:
//
//   * The CLASSIFICATION. "connection timeout" describes a connect that was attempted and
//     did not finish. When the scan never matched the car's advert at all, nothing was ever
//     attempted — the car is out of range. Those are different events with different fixes
//     (wait vs. investigate), and the log said the same thing for both.
//   * The VOLUME. A condition that is expected, unchanged and self-resolving does not need
//     re-stating every 40 s. But it must not go silent either, or a car that has been
//     unreachable for two days looks identical to one that is fine.
//
// So: derive the kind from what the scanner actually saw, log the first occurrence of each
// kind, then repeat only on a slow heartbeat until the kind CHANGES or a connect succeeds.
// A kind change is always logged immediately — "the car came back but now the connect
// fails" is exactly the transition worth waking up for, and suppressing it to save lines
// would defeat the whole point.
enum class ConnectFail : uint8_t {
    OutOfRange,     // no advert matched the target VIN in the window — car away/asleep
    AtBleLimit,     // advert seen but NON-connectable — car at its ~3-device BLE limit
    ConnectFailed,  // advert seen and connectable, yet the GATT connect never completed
};

// Map a BleClient connectability snapshot (-1 = not seen OR not determinable,
// 0 = non-connectable, 1 = connectable) onto the failure kind. Kept here rather than at the
// call site so the test
// pins the mapping — a silent off-by-one between "-1 unknown" and "0 at limit" would mislabel
// every line. Note -1 is deliberately the conservative bucket: the scanner returns it both for
// "no advert matched" and for "couldn't determine" (scan mutex busy, or the caller's own
// allocation failure), and OutOfRange is the reading that raises no false alarm.
inline ConnectFail connect_fail_from_connectable(int connectable) {
    if (connectable < 0) return ConnectFail::OutOfRange;
    if (connectable == 0) return ConnectFail::AtBleLimit;
    return ConnectFail::ConnectFailed;
}

// A connect failure must be classified from an advert observed during this attempt, not from
// the longer-lived 90 s snapshot kept for a stable UI. Equal timestamps count as current: the
// connect intent and first scan callback can share the same microsecond tick.
inline bool advert_seen_in_attempt(int64_t advert_us, int64_t attempt_start_us) {
    return advert_us >= attempt_start_us;
}

// A SCAN_RSP can refresh the target name/RSSI without carrying the primary advert's
// connectability bit. Treat a verdict as current only when both observations belong to this
// attempt; otherwise fall back to the conservative unknown bucket instead of attaching a stale
// connectable value to a fresh name.
inline int connectable_verdict_in_attempt(bool target_name_matches, bool connectable,
                                          int64_t name_seen_us, int64_t connectable_seen_us,
                                          int64_t attempt_start_us) {
    if (!target_name_matches ||
        !advert_seen_in_attempt(name_seen_us, attempt_start_us) ||
        !advert_seen_in_attempt(connectable_seen_us, attempt_start_us)) {
        return -1;
    }
    return connectable ? 1 : 0;
}

// Human-readable cause, used verbatim in the log line so the message names what happened
// instead of asserting a timeout that may not have occurred.
inline const char* connect_fail_text(ConnectFail kind) {
    switch (kind) {
        case ConnectFail::OutOfRange:    return "car not in BLE range (no advert seen)";
        case ConnectFail::AtBleLimit:    return "car advertising non-connectable (at its BLE "
                                                "connection limit — another phone/fob holds a slot)";
        case ConnectFail::ConnectFailed: return "car advertised but the connect did not complete";
    }
    return "unknown";
}

enum class ConnectLog : uint8_t {
    Suppress,   // same kind as last time and not a heartbeat tick — say nothing
    Warn,       // expected/benign condition (or its heartbeat)
    Error,      // a real fault, or any failure a caller is actively waiting on
};

// Why this connect attempt exists. Keep this strongly typed: cmd_in_flight_ is a FIFO-arbitration
// flag, not an origin signal — the unattended health probe also raises it while it owns the BLE
// command path. Passing that flag here used to misclassify every background failure as foreground
// and defeated the rate limiter entirely.
enum class ConnectOrigin : uint8_t {
    Background,
    Foreground,
};

// Restate an unchanged background condition once per monotonic hour. This must be time-based:
// the unpaired auto-enrolment supervisor can issue ten probes in one round, so an attempt-count
// threshold that is hourly for the paired health probe becomes only minutes in that burst path.
inline constexpr uint64_t kConnectFailRepeatMs = 60ULL * 60ULL * 1000ULL;

struct ConnectFailState {
    ConnectFail kind{ConnectFail::OutOfRange};
    uint32_t    streak{0};   // consecutive failures of `kind`; 0 = no failure outstanding
    uint64_t    last_emit_ms{0};
};

// Fold one failure into the state and decide what to emit.
//
// `origin == Foreground` = a caller is blocked on this attempt (an evcc/MCP command or a user
// action). Those are NEVER suppressed and always ERROR: someone asked for
// something and did not get it, which is a fault regardless of how ordinary the cause is,
// and a request that silently produced no log line is exactly the thing that makes a support
// question unanswerable. Only the unattended background polls are rate-limited.
inline ConnectLog connect_fail_note(ConnectFailState& st, ConnectFail kind,
                                    ConnectOrigin origin, uint64_t now_ms) {
    const bool first = st.streak == 0 || st.kind != kind;
    if (first) {
        st.kind   = kind;
        st.streak = 1;
    } else if (st.streak != UINT32_MAX) {
        st.streak++;
    }

    if (origin == ConnectOrigin::Foreground) {
        st.last_emit_ms = now_ms;
        return ConnectLog::Error;
    }

    const bool heartbeat = !first && now_ms - st.last_emit_ms >= kConnectFailRepeatMs;
    if (!first && !heartbeat) return ConnectLog::Suppress;
    st.last_emit_ms = now_ms;

    // OutOfRange is the car being elsewhere: the expected resting state of a device whose
    // whole job is to wait for it, so it is a warning, not an error. The other two mean the
    // car IS present and we still cannot talk to it — that is a fault worth an error even
    // unattended (it is the two-boards-on-one-car signature).
    return kind == ConnectFail::OutOfRange ? ConnectLog::Warn : ConnectLog::Error;
}

// A successful connect clears the run, so the next failure is reported as a first one.
inline void connect_ok_note(ConnectFailState& st) {
    st.streak = 0;
    st.last_emit_ms = 0;
}

// A completed BLE connection and a completed signed command are separate health signals.
// In particular, Whitelist Add Key normally has no completing commandStatus, while the
// authorised GET_STATUS health probe must answer. Do not infer that distinction from whether
// the caller is foreground/background; make each command choose its timeout contract.
enum class CompletionTimeoutPolicy : uint8_t {
    ForegroundWarn,    // a user/API caller is blocked: every timeout must leave a warning
    BackgroundHealth, // unattended but expected to answer: first + heartbeat warning
    ExpectedSilent,   // protocol is known not to complete (automatic Whitelist Add Key)
};

enum class CompletionTimeoutLog : uint8_t {
    Suppress,
    Warn,
};

inline constexpr uint64_t kCompletionTimeoutRepeatMs = 60ULL * 60ULL * 1000ULL;

struct CompletionTimeoutState {
    uint32_t streak{0};
    uint64_t last_emit_ms{0};
};

inline CompletionTimeoutLog completion_timeout_note(CompletionTimeoutState& st,
                                                     CompletionTimeoutPolicy policy,
                                                     uint64_t now_ms) {
    if (policy == CompletionTimeoutPolicy::ForegroundWarn) return CompletionTimeoutLog::Warn;
    if (policy == CompletionTimeoutPolicy::ExpectedSilent) return CompletionTimeoutLog::Suppress;

    if (st.streak != UINT32_MAX) st.streak++;
    const bool first = st.streak == 1;
    const bool heartbeat = !first && now_ms - st.last_emit_ms >= kCompletionTimeoutRepeatMs;
    if (!first && !heartbeat) return CompletionTimeoutLog::Suppress;
    st.last_emit_ms = now_ms;
    return CompletionTimeoutLog::Warn;
}

// Any completed signed command proves the command channel is answering and ends the health-
// timeout run, regardless of which policy that particular command used.
inline void completion_ok_note(CompletionTimeoutState& st) {
    st.streak = 0;
    st.last_emit_ms = 0;
}

// Repeated supervisor instructions are useful on a state transition and as a slow reminder,
// but become a log storm when emitted on every enrolment round. Keep this tiny generic clock
// here so the one-hour contract is host-tested rather than hidden in FreeRTOS task code.
inline constexpr uint64_t kAutoPairNoticeRepeatMs = 60ULL * 60ULL * 1000ULL;

struct PeriodicLogState {
    bool active{false};
    uint64_t last_emit_ms{0};
};

inline bool periodic_log_due(PeriodicLogState& st, uint64_t now_ms, uint64_t repeat_ms) {
    if (st.active && now_ms - st.last_emit_ms < repeat_ms) return false;
    st.active = true;
    st.last_emit_ms = now_ms;
    return true;
}

inline void periodic_log_reset(PeriodicLogState& st) {
    st.active = false;
    st.last_emit_ms = 0;
}

}  // namespace tk
