#pragma once

#include <cstdint>
#include <limits>

namespace tk::ble {

// Stable connection generations are even, so this odd sentinel can never identify a usable
// snapshot. Keeping readiness as a generation token (rather than a bool) prevents a delayed
// callback for an old link from marking a replacement link ready after NimBLE reuses its
// 16-bit connection handle.
inline constexpr uint32_t kNoReadyGeneration = std::numeric_limits<uint32_t>::max();

struct ConnectLifecycle {
    bool want_connect;
    bool connecting;
};

// Host procedure results are converted at the NimBLE boundary so the publication contract stays
// host-testable. `AlreadyRunning` is positive evidence that a discovery procedure exists;
// `AlreadyStopped` is positive evidence that there is no procedure to cancel.
enum class ScanStartResult : uint8_t {
    Started,
    AlreadyRunning,
    Failed,
};

enum class ScanCancelResult : uint8_t {
    Canceled,
    AlreadyStopped,
    Failed,
};

enum class ConnectCancelResult : uint8_t {
    Canceled,
    AlreadyStopped,
    Failed,
};

constexpr bool scan_running_after_start(bool was_running, ScanStartResult result) {
    return was_running || result == ScanStartResult::Started ||
           result == ScanStartResult::AlreadyRunning;
}

constexpr bool scan_running_after_cancel(bool was_running, ScanCancelResult result) {
    return result == ScanCancelResult::Failed ? was_running : false;
}

constexpr bool connecting_after_cancel(bool was_connecting, ConnectCancelResult result) {
    return result == ConnectCancelResult::Failed ? was_connecting : false;
}

// 15 s > 10 s ble_gap_connect timeout so any orphaned/wedged GAP connect procedure is
// canceled and scanning can resume even if NimBLE drops BLE_GAP_EVENT_CONNECT.
inline constexpr int64_t kStuckConnectingTimeoutUs = 15LL * 1000 * 1000;

constexpr bool connecting_timed_out(bool connecting, int64_t started_us, int64_t now_us) {
    return connecting && started_us > 0 && (now_us - started_us) >= kStuckConnectingTimeoutUs;
}

// ~1 s of 50 ms service_pending() polls returning BLE_HS_ENOTCONN before retiring a stale handle.
inline constexpr uint32_t kZombieTerminateThreshold = 20u;

constexpr bool zombie_terminate_should_retire(uint32_t enotconn_streak) {
    return enotconn_streak >= kZombieTerminateThreshold;
}

inline constexpr int64_t kScanErrLogIntervalUs = 60LL * 1000 * 1000;

constexpr bool scan_error_log_due(int rc, int last_rc, int64_t last_log_us, int64_t now_us) {
    return rc != 0 &&
           (rc != last_rc || last_log_us <= 0 || (now_us - last_log_us) >= kScanErrLogIntervalUs);
}

constexpr bool disc_complete_should_restart(bool want_connect,
                                            bool disconnecting,
                                            bool has_gap_link,
                                            bool connecting) {
    return want_connect && !disconnecting && !has_gap_link && !connecting;
}

// A host reset cancels NimBLE's pending GAP procedure. want_connect stays asserted from the
// original command through GAP + GATT discovery, so preserve that explicit intent and always
// clear the host-owned in-flight latch before on_sync decides whether to restart scanning.
constexpr ConnectLifecycle connect_lifecycle_after_host_reset(bool want_connect) {
    return {want_connect, false};
}

// Failures never manufacture an intent: stop_connecting() may have canceled concurrently.
constexpr ConnectLifecycle connect_lifecycle_after_start_failure(bool want_connect) {
    return {want_connect, false};
}

constexpr ConnectLifecycle connect_lifecycle_during_gap_start(bool want_connect) {
    return {want_connect, want_connect};
}

constexpr ConnectLifecycle connect_lifecycle_after_gap_connected(bool want_connect) {
    return {want_connect, false};
}

constexpr ConnectLifecycle connect_lifecycle_after_command_ready() {
    return {false, false};
}

struct DisconnectFollowup {
    bool restart_scan_now;
    bool defer_reconcile;
};

// When BLE_GAP_EVENT_DISCONNECT cannot acquire intent_mutex_ without blocking, preserve
// want_connect_ (a task may be asserting a fresh connect() intent) and defer scan
// reconciliation to service_pending(). If stop_connecting()/disconnect() owns the lock,
// it clears want_connect_ under the lock so the deferred reconcile stays idle.
constexpr DisconnectFollowup disconnect_followup(bool intent_locked,
                                                 bool want_connect) noexcept {
    if (!intent_locked) return {false, true};
    return {want_connect, false};
}

constexpr bool connect_scan_should_start(bool want_connect,
                                         bool disconnecting,
                                         bool has_gap_link,
                                         bool connecting,
                                         bool scanning) {
    return want_connect && !disconnecting && !has_gap_link && !connecting && !scanning;
}

constexpr bool manual_discovery_may_start(bool disconnecting,
                                           bool want_connect,
                                           bool has_gap_link,
                                           bool connecting) {
    return !disconnecting && !want_connect && !has_gap_link && !connecting;
}

constexpr bool manual_discovery_timeout_may_cancel(bool scanning,
                                                    bool want_connect,
                                                    bool connecting,
                                                    bool has_gap_link) {
    return scanning && !want_connect && !connecting && !has_gap_link;
}

// GAP/GATT callbacks are continuations of the bounded command-owned connect attempt. They may
// advance only while that intent is still live and the connection snapshot still belongs to the
// same link. stop_connecting()/disconnect() serialize cancellation against this decision.
constexpr bool connect_attempt_may_advance(bool want_connect, bool snapshot_matches) {
    return want_connect && snapshot_matches;
}

// A stable GAP link is useful to the adapter's own discovery state machine, but is deliberately
// weaker than command readiness. `generation_before` and `generation_after` are the two seqlock
// reads surrounding the handle snapshot.
constexpr bool gap_link_available(bool disconnecting,
                                  uint32_t generation_before,
                                  uint32_t generation_after,
                                  bool has_conn_handle) {
    return !disconnecting && !(generation_before & 1U) &&
           generation_before == generation_after && has_conn_handle;
}

// Commands may use the link only after the Tesla write characteristic exists and the CCCD
// subscription plus the adapter's connected callback have completed for this exact generation.
constexpr bool command_ready(bool disconnecting,
                             uint32_t generation_before,
                             uint32_t generation_after,
                             bool has_conn_handle,
                             bool has_write_handle,
                             uint32_t ready_generation) {
    return gap_link_available(disconnecting, generation_before, generation_after,
                              has_conn_handle) &&
           has_write_handle && ready_generation == generation_before;
}

}  // namespace tk::ble
