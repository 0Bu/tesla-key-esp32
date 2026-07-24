#pragma once

#include <cstdint>

// Pure, hardware-free logic shared by the firmware and the host-side mock build
// (test/, built without ESP-IDF). Anything in this directory must stay free of IDF,
// FreeRTOS, NimBLE, NVS, cJSON and esp_http_server includes so it compiles with a
// plain host toolchain. See test/README.md and the project CLAUDE.md.
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

// Map BleClient::target_connectable() (-1 = not seen, 0 = non-connectable, 1 = connectable)
// onto the failure kind. Kept here rather than at the call site so the test pins the mapping
// — a silent off-by-one between "-1 unknown" and "0 at limit" would mislabel every line.
inline ConnectFail connect_fail_from_connectable(int connectable) {
    if (connectable < 0) return ConnectFail::OutOfRange;
    if (connectable == 0) return ConnectFail::AtBleLimit;
    return ConnectFail::ConnectFailed;
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

// How many consecutive failures of the SAME kind pass before the condition is restated.
// The background health poll retries about every 40 s, so 90 is roughly hourly: frequent
// enough that "still unreachable" is visible in a log someone opens the next morning, rare
// enough that a week away costs ~170 lines instead of ~15000.
inline constexpr uint32_t kConnectFailRepeatEvery = 90;

struct ConnectFailState {
    ConnectFail kind{ConnectFail::OutOfRange};
    uint32_t    streak{0};   // consecutive failures of `kind`; 0 = no failure outstanding
};

// Fold one failure into the state and decide what to emit.
//
// `foreground` = a caller is blocked on this attempt (an evcc/MCP command or a user action,
// marked by cmd_in_flight_). Those are NEVER suppressed and always ERROR: someone asked for
// something and did not get it, which is a fault regardless of how ordinary the cause is,
// and a request that silently produced no log line is exactly the thing that makes a support
// question unanswerable. Only the unattended background polls are rate-limited.
inline ConnectLog connect_fail_note(ConnectFailState& st, ConnectFail kind, bool foreground) {
    if (st.streak == 0 || st.kind != kind) {
        st.kind   = kind;
        st.streak = 1;
    } else if (st.streak != UINT32_MAX) {
        st.streak++;
    }

    if (foreground) return ConnectLog::Error;

    const bool first     = st.streak == 1;
    const bool heartbeat = st.streak % kConnectFailRepeatEvery == 0;
    if (!first && !heartbeat) return ConnectLog::Suppress;

    // OutOfRange is the car being elsewhere: the expected resting state of a device whose
    // whole job is to wait for it, so it is a warning, not an error. The other two mean the
    // car IS present and we still cannot talk to it — that is a fault worth an error even
    // unattended (it is the two-boards-on-one-car signature).
    return kind == ConnectFail::OutOfRange ? ConnectLog::Warn : ConnectLog::Error;
}

// A successful connect clears the run, so the next failure is reported as a first one.
inline void connect_ok_note(ConnectFailState& st) { st.streak = 0; }

}  // namespace tk
