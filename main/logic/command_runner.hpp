#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "ble_dispatcher.hpp"
#include "command_result.hpp"
#include "rx_framing.hpp"
#include "session_state.hpp"

// Pure, hardware-free Command FIFO & Request Runner shared by firmware and the host mock build.
// Single source of truth for BLE request orchestration according to normative reference:
// - teslamotors/vehicle-command internal/dispatcher/dispatcher.go
// - teslamotors/vehicle-command pkg/protocol/signer.go
// - docs/adr/0005-tesla-ble-seam.md
//
// Key Characteristics:
// - Integrates RxFramer, BleDispatcher, and SessionTracker into a deterministic command FIFO.
// - Strict FreeRTOS/heap friendliness: bounded static queue capacity (kMaxQueueDepth = 8), zero unbounded allocations.
// - Multi-phase prerequisite coordination:
//     VCSEC Auth -> Wake (if asleep and WakeIfNeeded) -> Infotainment Auth -> Command Transmission.
// - Wake-up policy enforcement:
//     NoWakeSkip: Skips if vehicle is asleep without sending wake or command.
//     NoWakeFail: Fails if vehicle is asleep without sending wake or command.
//     WakeIfNeeded: Automatically coordinates wake-up sequence before infotainment commands.
// - Outcome classification:
//     Uses is_nominal_already_set() to treat nominal already_set vehicle responses as idempotent success.
// - Retry and timeout arbitration:
//     Tracks per-command deadline, phase duration, and response timeout with bounded exponential backoff.
namespace tk {

enum class WakePolicy : uint8_t {
    NoWakeSkip = 0,
    NoWakeFail = 1,
    WakeIfNeeded = 2,
};

enum class SleepState : uint8_t {
    Unknown = 0,
    Asleep = 1,
    Awake = 2,
};

enum class CommandState : uint8_t {
    Idle = 0,
    EnsuringVcsec,
    WaitingVcsecAuth,
    EnsuringAwake,
    WaitingWake,
    EnsuringInfotainment,
    WaitingInfoAuth,
    Ready,
    AwaitingResponse,
    Completed,
    Failed,
    Skipped,
};

enum class CommandPhase : uint8_t {
    Queued = 0,
    EnsuringVcsecSession,
    EnsuringAwake,
    EnsuringInfotainmentSession,
    SendingRequest,
    AwaitingResponse,
    Terminal,
};

enum class TerminalReason : uint8_t {
    None = 0,
    Success,
    AlreadySet,
    VehicleAsleep,
    ResponseTimeout,
    StepTimeout,
    DeadlineExceeded,
    MaxRetriesExceeded,
    AuthenticationFailed,
    BleDisconnected,
    ReplayDetected,
    Cancelled,
};

enum class TxAction : uint8_t {
    None = 0,
    SendVcsecSessionInfoRequest,
    SendWake,
    SendInfoSessionInfoRequest,
    SendCommandPayload,
};

// Pure decision helpers for incoming telemetry and frame filtering

// Evaluates whether an incoming SessionInfo response UUID matches the outstanding request UUID.
// An empty/omitted response UUID (0 bytes) is valid and matches. A non-empty UUID must match
// the expected request UUID exactly in length and byte content.
inline bool is_session_info_uuid_matching(const uint8_t* msg_uuid, size_t msg_uuid_len,
                                          const uint8_t* expected_uuid, size_t expected_uuid_len) noexcept {
    if (msg_uuid_len == 0) return true;
    if (msg_uuid_len != expected_uuid_len) return false;
    if (msg_uuid == nullptr || expected_uuid == nullptr) return false;
    return std::memcmp(msg_uuid, expected_uuid, msg_uuid_len) == 0;
}

// Evaluates whether an active command is currently awaiting authentication for the given domain.
// Used to prevent unauthenticated/mismatched SessionInfo failure frames from failing commands
// that are not in the waiting-auth phase for that domain.
inline bool is_command_awaiting_session_auth(bool has_active_command, bool is_completed,
                                             CommandState cmd_state, BleDomain session_domain) noexcept {
    if (!has_active_command || is_completed) return false;
    if (session_domain == BleDomain::VehicleSecurity) {
        return cmd_state == CommandState::WaitingVcsecAuth;
    }
    if (session_domain == BleDomain::Infotainment) {
        return cmd_state == CommandState::WaitingInfoAuth;
    }
    return false;
}

// Evaluates whether a signed message fault domain matches an in-flight command.
// Signed message faults do not carry request UUIDs and are filtered by domain.
// Broadcast faults match any command; VehicleSecurity faults match VCSEC commands
// or commands awaiting VCSEC auth; Infotainment faults match Infotainment commands
// in their Infotainment execution phases (WaitingInfoAuth, Ready, AwaitingResponse)
// and must NOT match during prerequisite VCSEC auth or Wake phases.
inline bool is_fault_domain_matching(BleDomain fault_domain, BleDomain cmd_domain, CommandState cmd_state) noexcept {
    if (fault_domain == BleDomain::Broadcast) {
        return true;
    }
    if (fault_domain == BleDomain::VehicleSecurity) {
        return (cmd_domain == BleDomain::VehicleSecurity) ||
               (cmd_state == CommandState::WaitingVcsecAuth);
    }
    if (fault_domain == BleDomain::Infotainment) {
        return (cmd_domain == BleDomain::Infotainment) &&
               (cmd_state == CommandState::WaitingInfoAuth ||
                cmd_state == CommandState::Ready ||
                cmd_state == CommandState::AwaitingResponse);
    }
    return false;
}

enum class VcsecOpStatusAction : uint8_t {
    Wait,
    CompleteOk,
    Error,
};

struct VcsecOpStatusDecision {
    VcsecOpStatusAction action{VcsecOpStatusAction::Error};
    bool is_ok{false};
    const char* error_message{""};
};

// Maps VCSEC command status operationStatus (OK=0, WAIT=1, ERROR=2) to action and error text.
inline VcsecOpStatusDecision evaluate_vcsec_operation_status(int op_status_val) noexcept {
    if (op_status_val == 1) { // VCSEC_OperationStatus_E_OPERATIONSTATUS_WAIT
        return {VcsecOpStatusAction::Wait, false, ""};
    }
    if (op_status_val == 0) { // VCSEC_OperationStatus_E_OPERATIONSTATUS_OK
        return {VcsecOpStatusAction::CompleteOk, true, ""};
    }
    return {VcsecOpStatusAction::Error, false, "VCSEC command failed with error status"};
}


// Represents an outstanding command in the FIFO
struct CommandRequest {
    uint32_t id{0};
    std::string name{};
    BleDomain domain{BleDomain::VehicleSecurity};
    WakePolicy wake_policy{WakePolicy::WakeIfNeeded};
    BleUuid uuid{};
    uint32_t dispatcher_request_id{0};

    CommandState state{CommandState::Idle};
    CommandPhase phase{CommandPhase::Queued};

    uint32_t enqueued_at_ms{0};
    uint32_t phase_started_at_ms{0};
    uint32_t last_tx_ms{0};
    uint32_t timeout_ms{20000};

    uint32_t retry_count{0};
    uint32_t max_retries{3};
    uint32_t next_retry_delay_ms{0};

    bool is_completed{false};
    bool is_success{false};
    bool is_already_set{false};
    // Set once this command's wake was confirmed while actively waiting for a wake (VCSEC reported awake,
    // or a VehicleStatus with closureStatuses answered). Cleared on retry if the vehicle is asleep,
    // so an asleep vehicle is re-evaluated and properly woken with SendWake.
    bool wake_confirmed{false};
    std::string error_message{};
    TerminalReason terminal_reason{TerminalReason::None};
    // True for commands whose transmission over BLE constitutes completion (e.g. Wake).
    // Prerequisite transmissions (like SessionInfoRequest) do NOT complete the command.
    bool completes_on_transmit{false};

    // User completion callback
    std::function<void(bool success, const std::string& err)> on_complete{nullptr};
};

class CommandRunner {
public:
    static constexpr size_t kMaxQueueSize = 8;
    static constexpr uint32_t kDefaultStepTimeoutMs = 5000;
    static constexpr uint32_t kDefaultResponseTimeoutMs = 7000;
    static constexpr uint32_t kDefaultCommandTimeoutMs = 20000;
    static constexpr uint32_t kInitialRetryDelayMs = 500;
    static constexpr uint32_t kMaxRetryDelayMs = 5000;
    static constexpr uint32_t kDefaultRxInterChunkTimeoutMs = 3000;

    CommandRunner() noexcept = default;

    // Reset all internal state, dispatcher, sessions, and active commands.
    // framer is optionally reset (defaults to true for standalone use/tests; pass false when
    // reset from non-loop tasks so RxFramer remains strictly single-owner on vehicle_loop).
    void reset(bool reset_framer = true) noexcept {
        if (reset_framer) {
            rx_framer_.reset();
        }
        dispatcher_.reset();
        vcsec_session_.reset();
        info_session_.reset();
        clear();
    }

    // Queue management
    [[nodiscard]] size_t queue_size() const noexcept { return queue_count_; }
    [[nodiscard]] bool is_queue_full() const noexcept { return queue_count_ >= kMaxQueueSize; }
    [[nodiscard]] bool has_active_command() const noexcept { return queue_count_ > 0; }

    CommandRequest* current_command() noexcept {
        if (queue_count_ == 0) return nullptr;
        return &queue_[queue_head_];
    }

    const CommandRequest* current_command() const noexcept {
        if (queue_count_ == 0) return nullptr;
        return &queue_[queue_head_];
    }

    // Enqueue a new command. Returns command ID > 0 on success, or 0 if queue is full.
    uint32_t enqueue(std::string name, BleDomain domain, WakePolicy wake_policy,
                     uint32_t timeout_ms = kDefaultCommandTimeoutMs, uint32_t now_ms = 0,
                     BleUuid uuid = {},
                     std::function<void(bool, const std::string&)> on_complete = nullptr) {
        if (queue_count_ >= kMaxQueueSize) {
            return 0; // Queue full
        }

        const size_t tail = (queue_head_ + queue_count_) % kMaxQueueSize;
        CommandRequest& cmd = queue_[tail];
        cmd = CommandRequest{};
        cmd.id = next_cmd_id_++;
        if (next_cmd_id_ == 0) next_cmd_id_ = 1;
        cmd.name = std::move(name);
        cmd.domain = domain;
        cmd.wake_policy = wake_policy;
        cmd.uuid = uuid;
        cmd.enqueued_at_ms = now_ms;
        cmd.phase_started_at_ms = now_ms;
        cmd.timeout_ms = timeout_ms;
        cmd.on_complete = std::move(on_complete);
        cmd.state = CommandState::Idle;
        cmd.phase = CommandPhase::Queued;
        cmd.completes_on_transmit = (cmd.name == "Wake");

        queue_count_++;
        return cmd.id;
    }

    // Pop the front command from the FIFO
    void pop_current() noexcept {
        if (queue_count_ == 0) return;
        CommandRequest& cmd = queue_[queue_head_];
        if (cmd.dispatcher_request_id != 0) {
            dispatcher_.unregister_request(cmd.dispatcher_request_id);
            cmd.dispatcher_request_id = 0;
        }
        cmd = CommandRequest{};
        queue_head_ = (queue_head_ + 1) % kMaxQueueSize;
        queue_count_--;
    }

    // Cancel all commands in the queue
    void clear() noexcept {
        while (queue_count_ > 0) {
            cancel_current(TerminalReason::Cancelled);
            pop_current();
        }
        queue_head_ = 0;
        queue_count_ = 0;
    }

    // Cancel currently active command
    void cancel_current(TerminalReason reason = TerminalReason::Cancelled) {
        CommandRequest* cmd = current_command();
        if (!cmd) return;
        if (!cmd->is_completed) {
            cmd->is_completed = true;
            cmd->is_success = false;
            cmd->terminal_reason = reason;
            if (cmd->error_message.empty()) {
                cmd->error_message = "command cancelled";
            }
            if (cmd->dispatcher_request_id != 0) {
                dispatcher_.unregister_request(cmd->dispatcher_request_id);
                cmd->dispatcher_request_id = 0;
            }
            if (cmd->on_complete) {
                cmd->on_complete(false, cmd->error_message);
            }
        }
    }

    // Complete currently active command with outcome
    void complete_current_command(bool success, const std::string& err = "", bool already_set = false) {
        CommandRequest* cmd = current_command();
        if (!cmd) return;
        if (already_set || is_nominal_already_set(err)) {
            cmd->is_already_set = true;
            finish_command_(cmd, true, "command executed successfully", TerminalReason::AlreadySet);
        } else if (success) {
            finish_command_(cmd, true, "", TerminalReason::Success);
        } else {
            finish_command_(cmd, false, err.empty() ? "command rejected by vehicle" : err.c_str(),
                            TerminalReason::AuthenticationFailed);
        }
    }

    // Subcomponent accessors
    RxFramer& rx_framer() noexcept { return rx_framer_; }
    const RxFramer& rx_framer() const noexcept { return rx_framer_; }

    BleDispatcher& dispatcher() noexcept { return dispatcher_; }
    const BleDispatcher& dispatcher() const noexcept { return dispatcher_; }

    SessionTracker& vcsec_session() noexcept { return vcsec_session_; }
    const SessionTracker& vcsec_session() const noexcept { return vcsec_session_; }

    SessionTracker& info_session() noexcept { return info_session_; }
    const SessionTracker& info_session() const noexcept { return info_session_; }

    // Authentication and fault routing queries
    [[nodiscard]] bool is_awaiting_session_auth(BleDomain session_domain) const noexcept {
        const CommandRequest* cmd = current_command();
        return cmd && is_command_awaiting_session_auth(true, cmd->is_completed, cmd->state, session_domain);
    }

    [[nodiscard]] bool should_notify_signed_message_fault(BleDomain fault_domain) const noexcept {
        const CommandRequest* cmd = current_command();
        return cmd && !cmd->is_completed && is_fault_domain_matching(fault_domain, cmd->domain, cmd->state);
    }

    // Standalone tick: evaluates timeouts, retries, prerequisite progression, and next TX action.
    // sleep status model: is_awake indicates confirmed awake; is_asleep indicates confirmed asleep.
    // If both are false, the vehicle sleep state is Unknown (e.g. at boot/reconnect).
    TxAction tick(uint32_t now_ms, bool is_connected, bool is_awake, bool is_asleep = false) {
        CommandRequest* cmd = current_command();
        if (!cmd) return TxAction::None;

        // If completed/failed/skipped, awaiting caller to pop or read outcome
        if (cmd->is_completed) {
            return TxAction::None;
        }

        // 1. Overall deadline check
        if (cmd->timeout_ms > 0 && (now_ms - cmd->enqueued_at_ms) >= cmd->timeout_ms) {
            finish_command_(cmd, false,
                            !is_connected ? "connection lost; command deadline exhausted"
                                          : "command deadline exhausted",
                            TerminalReason::DeadlineExceeded);
            return TxAction::None;
        }

        // 2. Connection check
        if (!is_connected) {
            return TxAction::None;
        }

        // 3. Response timeout and retry handling for AwaitingResponse
        if (cmd->state == CommandState::AwaitingResponse) {
            const uint32_t elapsed = now_ms - cmd->last_tx_ms;
            if (elapsed >= kDefaultResponseTimeoutMs) {
                if (cmd->retry_count < cmd->max_retries) {
                    cmd->retry_count++;
                    const uint32_t shift = std::min<uint32_t>(cmd->retry_count - 1, 10);
                    cmd->next_retry_delay_ms = std::min(kMaxRetryDelayMs,
                        kInitialRetryDelayMs * (1U << shift));
                    cmd->state = CommandState::Ready;
                    cmd->phase = CommandPhase::SendingRequest;
                    cmd->phase_started_at_ms = now_ms;
                    if (cmd->dispatcher_request_id != 0) {
                        dispatcher_.unregister_request(cmd->dispatcher_request_id);
                        cmd->dispatcher_request_id = 0;
                    }
                    return TxAction::None;
                } else {
                    finish_command_(cmd, false, "command response timeout; max retries exceeded",
                                    TerminalReason::MaxRetriesExceeded);
                    return TxAction::None;
                }
            }
            return TxAction::None; // Still waiting for response
        }

        // 4. If currently waiting for wake and the vehicle is confirmed awake, advance immediately
        if (is_awake) {
            if (cmd->state == CommandState::WaitingWake || cmd->phase == CommandPhase::EnsuringAwake) {
                cmd->wake_confirmed = true;
                cmd->state = CommandState::Idle;
                cmd->phase_started_at_ms = now_ms;
            }
        }

        // 5. Step timeouts for authentication / wake phases
        if (cmd->state == CommandState::WaitingVcsecAuth ||
            cmd->state == CommandState::WaitingWake ||
            cmd->state == CommandState::WaitingInfoAuth) {
            const uint32_t elapsed = now_ms - cmd->phase_started_at_ms;
            if (elapsed >= kDefaultStepTimeoutMs) {
                if (cmd->retry_count < cmd->max_retries) {
                    cmd->retry_count++;
                    cmd->next_retry_delay_ms = 0;
                    cmd->wake_confirmed = false;
                    cmd->state = CommandState::Idle; // Reset to idle to retry prerequisites
                    cmd->phase = CommandPhase::Queued;
                    cmd->phase_started_at_ms = now_ms;
                    return TxAction::None;
                } else {
                    finish_command_(cmd, false, "authentication / wake timeout; max retries exceeded",
                                    TerminalReason::StepTimeout);
                    return TxAction::None;
                }
            } else {
                return TxAction::None; // Still waiting for step
            }
        }

        auto emit_payload = [&](CommandRequest* req) -> TxAction {
            if (req->next_retry_delay_ms > 0) {
                if (now_ms - req->phase_started_at_ms < req->next_retry_delay_ms) {
                    return TxAction::None;
                }
                req->next_retry_delay_ms = 0;
            }
            req->state = CommandState::Ready;
            req->phase = CommandPhase::SendingRequest;
            req->phase_started_at_ms = now_ms;
            return TxAction::SendCommandPayload;
        };

        // 6. Prerequisite Progression & Wake-up Policy Coordination
        if (cmd->domain == BleDomain::VehicleSecurity) {
            // Pairing "Whitelist Add Key" starts with untrusted key, so session auth is bypassed
            if (cmd->name == "Whitelist Add Key") {
                return emit_payload(cmd);
            }

            // VCSEC commands require an authenticated VCSEC session (including Wake,
            // which is an encrypted RKE action).
            if (!vcsec_session_.is_authenticated()) {
                cmd->state = CommandState::WaitingVcsecAuth;
                cmd->phase = CommandPhase::EnsuringVcsecSession;
                cmd->phase_started_at_ms = now_ms;
                return TxAction::SendVcsecSessionInfoRequest;
            }

            // VCSEC body controller is always reachable while connected; commands do not
            // require vehicle infotainment to be awake, and health poll runs while vehicle sleeps.
            return emit_payload(cmd);
        }

        if (cmd->domain == BleDomain::Infotainment) {
            // Prerequisite 1: VCSEC session (required to encrypt SendWake and for general vehicle security)
            if (!vcsec_session_.is_authenticated()) {
                cmd->state = CommandState::WaitingVcsecAuth;
                cmd->phase = CommandPhase::EnsuringVcsecSession;
                cmd->phase_started_at_ms = now_ms;
                return TxAction::SendVcsecSessionInfoRequest;
            }

            // Prerequisite 2: Wake policy evaluation (aligned with vehicle-command & upstream)
            // Once VCSEC session is established, send SendWake if vehicle is asleep / unknown.
            // A confirmed wake is remembered for this attempt, but re-evaluated if vehicle is asleep on retry.
            if (!cmd->wake_confirmed && is_asleep) {
                switch (cmd->wake_policy) {
                    case WakePolicy::NoWakeSkip:
                        finish_command_(cmd, false, "vehicle asleep", TerminalReason::VehicleAsleep);
                        cmd->state = CommandState::Skipped;
                        return TxAction::None;
                    case WakePolicy::NoWakeFail:
                        finish_command_(cmd, false, "vehicle asleep", TerminalReason::VehicleAsleep);
                        cmd->state = CommandState::Failed;
                        return TxAction::None;
                    case WakePolicy::WakeIfNeeded:
                        cmd->state = CommandState::WaitingWake;
                        cmd->phase = CommandPhase::EnsuringAwake;
                        cmd->phase_started_at_ms = now_ms;
                        return TxAction::SendWake;
                }
            } else if (!cmd->wake_confirmed && !is_awake && cmd->wake_policy == WakePolicy::WakeIfNeeded) {
                // Sleep status is Unknown; WakeIfNeeded policy coordinates wake sequence
                cmd->state = CommandState::WaitingWake;
                cmd->phase = CommandPhase::EnsuringAwake;
                cmd->phase_started_at_ms = now_ms;
                return TxAction::SendWake;
            }

            // Prerequisite 3: Infotainment session (only once vehicle is awake)
            if (!info_session_.is_authenticated()) {
                cmd->state = CommandState::WaitingInfoAuth;
                cmd->phase = CommandPhase::EnsuringInfotainmentSession;
                cmd->phase_started_at_ms = now_ms;
                return TxAction::SendInfoSessionInfoRequest;
            }

            // All prerequisites met -> Ready to send infotainment payload
            return emit_payload(cmd);
        }

        return TxAction::None;
    }

    // Call after TX action was successfully transmitted over BLE
    void notify_tx_complete(TxAction action, uint32_t now_ms) {
        CommandRequest* cmd = current_command();
        if (!cmd) return;

        cmd->last_tx_ms = now_ms;
        if (cmd->completes_on_transmit) {
            // Wake action has no commandStatus acknowledgement from Tesla; its payload transmission completes it.
            // A prerequisite transmission (e.g. SessionInfoRequest) must NOT complete the Wake command prematurely.
            if (action == TxAction::SendCommandPayload) {
                finish_command_(cmd, true, "wake transmitted", TerminalReason::Success);
            }
            return;
        }
        if (cmd->state == CommandState::Ready) {
            cmd->state = CommandState::AwaitingResponse;
            cmd->phase = CommandPhase::AwaitingResponse;
            cmd->phase_started_at_ms = now_ms;

            // Register request with dispatcher for response matching and anti-replay
            if (cmd->dispatcher_request_id == 0) {
                cmd->dispatcher_request_id = dispatcher_.register_request(
                    cmd->domain, cmd->uuid, cmd);
            }
        }
    }

    void notify_tx_complete(uint32_t now_ms) {
        CommandRequest* cmd = current_command();
        TxAction action = (cmd && cmd->state == CommandState::Ready)
                              ? TxAction::SendCommandPayload
                              : TxAction::None;
        notify_tx_complete(action, now_ms);
    }

    // Call if TX action transmission failed
    void notify_tx_failed(const char* reason = "BLE write failed") {
        CommandRequest* cmd = current_command();
        if (!cmd) return;
        if (cmd->retry_count < cmd->max_retries) {
            cmd->retry_count++;
            cmd->next_retry_delay_ms = 0;
            cmd->wake_confirmed = false;
            cmd->state = CommandState::Idle;
        } else {
            finish_command_(cmd, false, reason, TerminalReason::BleDisconnected);
        }
    }

    // Notify that vehicle wake status was confirmed
    void notify_vehicle_awake(bool awake) {
        CommandRequest* cmd = current_command();
        if (!cmd) return;
        if (awake) {
            if (cmd->state == CommandState::WaitingWake || cmd->phase == CommandPhase::EnsuringAwake) {
                cmd->wake_confirmed = true;
                // Advance to infotainment session or ready; the wake is not re-sent afterwards
                cmd->state = CommandState::Idle;
            }
        }
    }

    // Notify link loss: fail in-flight command immediately
    void notify_link_lost(const char* reason = "connection lost") {
        CommandRequest* cmd = current_command();
        if (!cmd) return;
        finish_command_(cmd, false, reason, TerminalReason::BleDisconnected);
    }

    // Notify signed message fault: retry if session error, otherwise fail immediately
    void notify_signed_message_fault(bool is_session_error,
                                     const char* reason = "signed message authentication failed") {
        CommandRequest* cmd = current_command();
        if (!cmd) return;
        if (is_session_error) {
            if (cmd->retry_count < cmd->max_retries) {
                cmd->retry_count++;
                cmd->next_retry_delay_ms = 0;
                cmd->wake_confirmed = false;
                cmd->state = CommandState::Idle;
                if (cmd->dispatcher_request_id != 0) {
                    dispatcher_.unregister_request(cmd->dispatcher_request_id);
                    cmd->dispatcher_request_id = 0;
                }
            } else {
                finish_command_(cmd, false, "session error; max retries exceeded",
                                TerminalReason::AuthenticationFailed);
            }
        } else {
            finish_command_(cmd, false, reason, TerminalReason::AuthenticationFailed);
        }
    }

    // Handle generic incoming command response
    DispatchOutcome handle_response(BleDomain from_domain,
                                    const uint8_t* uuid_bytes, size_t uuid_len,
                                    bool has_counter, uint32_t counter,
                                    bool is_success, const std::string& error_string = "") {
        DispatchOutcome outcome = dispatcher_.dispatch(from_domain, uuid_bytes, uuid_len, has_counter, counter);
        if (!outcome.routed) {
            return outcome;
        }

        CommandRequest* cmd = current_command();
        if (!cmd || cmd->dispatcher_request_id != outcome.request_id) {
            // Response belonged to another request or an older invalidated slot
            return outcome;
        }

        // Evaluate outcome with normative already_set mapping
        if (is_nominal_already_set(error_string)) {
            cmd->is_already_set = true;
            finish_command_(cmd, true, "command executed successfully", TerminalReason::AlreadySet);
        } else if (is_success) {
            finish_command_(cmd, true, "", TerminalReason::Success);
        } else {
            finish_command_(cmd, false, error_string.empty() ? "command rejected by vehicle" : error_string.c_str(),
                            TerminalReason::AuthenticationFailed);
        }

        return outcome;
    }

    // Stream chunk ingestion convenience helper
    template <typename FrameCb, typename DropCb>
    size_t push_rx_chunk(const uint8_t* data, size_t len, uint32_t now_ms,
                         FrameCb&& frame_cb, DropCb&& drop_cb) {
        return rx_framer_.push_chunk(data, len, now_ms,
                                     std::forward<FrameCb>(frame_cb),
                                     std::forward<DropCb>(drop_cb));
    }

private:
    [[gnu::noinline]] void finish_command_(CommandRequest* cmd, bool success, const char* err,
                                          TerminalReason reason) {
        if (!cmd || cmd->is_completed) return;
        cmd->is_completed = true;
        cmd->is_success = success;
        cmd->error_message = (err != nullptr) ? err : "";
        cmd->terminal_reason = reason;
        cmd->state = success ? CommandState::Completed : CommandState::Failed;
        cmd->phase = CommandPhase::Terminal;

        if (cmd->dispatcher_request_id != 0) {
            dispatcher_.unregister_request(cmd->dispatcher_request_id);
            cmd->dispatcher_request_id = 0;
        }

        if (cmd->on_complete) {
            cmd->on_complete(success, cmd->error_message);
        }
    }

    RxFramer rx_framer_{kDefaultRxInterChunkTimeoutMs};
    BleDispatcher dispatcher_;
    SessionTracker vcsec_session_;
    SessionTracker info_session_;

    std::array<CommandRequest, kMaxQueueSize> queue_{};
    size_t queue_head_{0};
    size_t queue_count_{0};
    uint32_t next_cmd_id_{1};
};

}  // namespace tk
