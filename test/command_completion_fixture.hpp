#pragma once

// Actual native completion methods below; only RTOS, clock, logging and link sinks are fake.
#include "logic/connect_outcome.hpp"
#include "rtos_guard.hpp"

class CompletionController {
public:
#include "completion-types.inc"
    using ResultCb = std::function<void(tk::OperationResult)>;
    tk::CommandRunner command_runner_;
    std::atomic<uint32_t> command_generation_{1};
    std::atomic<int> auth_fail_streak_{0}, cmd_fail_streak_{0};
    std::atomic<bool> believed_paired_{true}, ble_fault_{false}, pairing_recovery_hint_{false}, repair_notice_{false};
    static constexpr int kCmdFailDropStreak = 3;
    tk::CompletionTimeoutState completion_timeout_{};
    SemaphoreHandle_t vehicle_mutex_{xSemaphoreCreateMutex()}, result_mutex_{xSemaphoreCreateMutex()};
    struct Link { unsigned disconnects{0}; void disconnect() { ++disconnects; } } link;
    Link* ble_{&link};
    std::array<std::function<void()>, tk::CommandRunner::kMaxQueueSize> command_builders_{};
    std::string last_error_;
    bool throw_contact{false};
    unsigned contacts{0}, timeouts{0}, retirements{0};
    ~CompletionController() { vSemaphoreDelete(vehicle_mutex_); vSemaphoreDelete(result_mutex_); }
    void note_reachable_() { if (throw_contact) throw std::bad_alloc(); ++contacts; }
    void retire_vcsec_contexts_() { ++retirements; }
    void request_framer_reset_() {}
    void mark_vcsec_unknown_() {}
    static TickType_t ticks_until_(uint32_t deadline) { return deadline > native_ticks ? deadline - native_ticks : 0; }
    void note_completion_timeout_(const char*, tk::CompletionTimeoutPolicy) { ++timeouts; }
    void publish_command_outcome_(const CommandOutcome&);
    void invalidate_and_flush_(uint32_t generation);
    CommandOutcome await_completion_(const std::shared_ptr<CommandCompletion>&, uint32_t, uint32_t, const char*, tk::CompletionTimeoutPolicy);
    ResultCb make_result_cb_(const std::shared_ptr<CommandCompletion>&, uint32_t, bool = false, bool = true);
};
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#include "completion-native.inc"
#undef ESP_LOGW
#undef ESP_LOGE

static void check_native_command_completion() {
    using Controller = CompletionController;
    using Severity = tk::CommandError::Severity;
    using Outcome = tk::CommandError::Outcome;
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        Controller c;
        auto completion = std::make_shared<Controller::CommandCompletion>();
        auto cb = c.make_result_cb_(completion, 1);
#include "completion-on-done.inc"
        completion->command_id = c.command_runner_.enqueue("mutation", tk::BleDomain::VehicleSecurity,
            tk::WakePolicy::WakeIfNeeded, 60000, native_ticks, {}, on_done, scenario == 3);
        auto* command = c.command_runner_.current_command();
        if (scenario != 2) c.command_runner_.notify_tx_started(tk::TxAction::SendCommandPayload);
        bool threw = false;
        if (scenario == 0) c.command_runner_.notify_link_lost();
        if (scenario == 1) c.vehicle_mutex_->fail = true;
        if (scenario == 3 || scenario == 4) {
            c.throw_contact = scenario == 3;
            fail_next_cpp_allocation = scenario == 4;
            try {
                if (scenario == 3) c.command_runner_.notify_tx_complete(tk::TxAction::SendCommandPayload, native_ticks);
                else c.command_runner_.complete_current_command(true, "", true);
            } catch (const std::bad_alloc&) { threw = true; }
            fail_next_cpp_allocation = false;
            expect((scenario == 3 ? completion->callback_fault : threw) && command->is_completed && command->is_success,
                   "TX-only callback/AlreadySet text failure retains committed transmission",
                   "success OOM control did not reach committed completion");
        }
        if (scenario == 5) {
            // Exercise catch-all inside the actual bounded callback, with no error allocation.
            c.throw_contact = true; cb(tk::OperationResult::success());
        }
        if (scenario == 6) {
            // The old generation's late callback cannot mutate or signal the waiting object.
            c.command_generation_.store(2);
            cb(tk::OperationResult::failure(std::make_unique<tk::CommandError>("late", Severity::Permanent, Outcome::DefinitelyFailed)));
            expect(!completion->completed && completion->sem->gives == 0,
                   "stale generation cannot signal or replace completion", "late callback changed result");
        }
        auto outcome = c.await_completion_(completion, 1, native_ticks + 100, "test", tk::CompletionTimeoutPolicy::ForegroundWarn);
        c.publish_command_outcome_(outcome);
        const bool uncertain = scenario != 2;
        expect(!outcome.success && tk::execution_uncertain(outcome.failure) == uncertain &&
               (uncertain ? c.last_error_.find(tk::uncertain_command_reason()) == 0
                          : c.last_error_ == "command completion deadline exhausted"),
               "native callback, timeout and public reason preserve uncertain versus unsubmitted failures",
               "native public completion lost execution uncertainty");
        expect(tk::command_result_text(false, c.last_error_) == c.last_error_.c_str(),
               "HTTP and MCP shared reason exposes native outcome", "public reason discarded native outcome");
        expect(c.vehicle_mutex_->available || c.vehicle_mutex_->fail,
               "completion leaves Vehicle mutex released", "completion leaked Vehicle mutex");
    }
    {
        Controller c; auto completion = std::make_shared<Controller::CommandCompletion>();
        auto cb = c.make_result_cb_(completion, 1);
        cb(tk::OperationResult::failure(std::make_unique<tk::CommandError>(
            "connection lost", Severity::Temporary, Outcome::MayHaveSucceeded)));
        expect(std::strcmp(completion->error.data(), tk::uncertain_command_reason()) == 0 &&
               tk::execution_uncertain(completion->failure),
               "typed callback exposes uncertainty without parsing original diagnostic text",
               "bounded callback lost typed uncertainty reason");
    }
    for (const char* reason : {"vehicle fault: vehicle busy", "vehicle fault: vehicle permission denied"}) {
        Controller c;
        for (unsigned i = 0; i < 4; ++i) {
            auto completion = std::make_shared<Controller::CommandCompletion>();
            auto cb = c.make_result_cb_(completion, 1);
            cb(tk::OperationResult::failure(std::make_unique<tk::CommandError>(reason, Severity::Permanent, Outcome::DefinitelyFailed)));
            expect(completion->completed && !completion->success && !completion->drop_link &&
                   !tk::execution_uncertain(completion->failure) && c.cmd_fail_streak_.load() == 0 && !c.ble_fault_.load(),
                   "explicit vehicle faults never count as transport loss", "valid negative response dropped healthy link");
        }
    }
}

// Actual wake/generic wrapper methods and clock helpers; only the scheduler, cached link
// inputs and blocking-send handoff are controlled. The latter also runs the real native BLE
// driver below, so post-submission uncertainty comes from production code, not a test rule.
#include "vehicle_ctrl_internal.hpp"
#include "logic/link_state.hpp"
#include "logic/command_deadline.hpp"
static std::function<void()> native_wake_delay_hook;
static std::function<void()> native_wake_tick_hook;
static TickType_t native_wake_tick() {
    if (native_wake_tick_hook) native_wake_tick_hook();
    return native_ticks;
}
static void native_wake_delay(TickType_t ticks) {
    native_ticks += ticks;
    if (native_wake_delay_hook) native_wake_delay_hook();
}
#define xTaskGetTickCount() native_wake_tick()
#define portTICK_PERIOD_MS 1
class WakeController : public CompletionController {
public:
    using Builder = std::function<int(TeslaBLE::Client*, uint8_t*, size_t*)>;
    using WakePolicy = tk::WakePolicy;
    using LinkState = tk::LinkState;
    SemaphoreHandle_t command_mutex_{xSemaphoreCreateMutex()};
    std::atomic<bool> cmd_in_flight_{false};
    std::atomic<uint32_t> last_cmd_ticks_{0};
    tk::LinkInputs link_inputs{};
    CommandOutcome observed;
    unsigned sends{0};
    uint32_t observed_send_deadline{0};
    std::function<CommandOutcome(Builder)> locked_send;
    ~WakeController() { vSemaphoreDelete(command_mutex_); }
#include "wake-clock.inc"
    LinkState link_state() const { return tk::compute_link_state(link_inputs); }
    void publish_command_outcome_(const CommandOutcome& out) {
        observed = out;
        CompletionController::publish_command_outcome_(out);
    }
    CommandOutcome send_vcsec_locked_(const std::string&, Builder builder, WakePolicy policy,
            uint32_t deadline, tk::ConnectOrigin origin, bool, tk::CompletionTimeoutPolicy,
            bool tx_only, uint32_t retries, tk::CommandEffect effect, tk::VcsecReplyKind kind) {
        // The production locked sender rejects an expired deadline before BLE admission.
        if (remaining_ms_(deadline) <= 0) {
            CommandOutcome out; out.error = "command deadline exhausted"; return out;
        }
        ++sends;
        observed_send_deadline = deadline;
        expect(cmd_in_flight_.load() && !command_mutex_->available && ticks_until_(deadline) > 0 &&
               policy == WakePolicy::NoWakeFail && origin == tk::ConnectOrigin::Foreground &&
               tx_only && retries == 3 && effect == tk::CommandEffect::Mutating && kind == tk::VcsecReplyKind::Action,
               "native wake retains serialized foreground deadline and command admission",
               "wake wrapper changed send admission");
        return locked_send(std::move(builder));
    }
    bool send_vcsec_(const std::string&, Builder, WakePolicy, int, tk::ConnectOrigin, bool,
                     tk::CompletionTimeoutPolicy, bool, tk::CommandEffect, tk::VcsecReplyKind);
    CommandOutcome send_vcsec_outcome_(const std::string&, Builder, WakePolicy, uint32_t,
        tk::ConnectOrigin, bool, tk::CompletionTimeoutPolicy, bool, tk::CommandEffect, tk::VcsecReplyKind);
    bool wake_up(int);
};
#define vTaskDelay native_wake_delay
#include "wake-native.inc"
#undef vTaskDelay
#undef portTICK_PERIOD_MS
#undef xTaskGetTickCount

static void check_native_wake_confirmation() {
    for (bool response_expected : {false, true}) {
        CompletionController c;
        c.auth_fail_streak_ = 2; c.cmd_fail_streak_ = 2;
        c.throw_contact = !response_expected;
        auto completion = std::make_shared<CompletionController::CommandCompletion>();
        auto callback = c.make_result_cb_(completion, 1, false, response_expected);
        callback(tk::OperationResult::success());
        expect(completion->completed && completion->success && !completion->callback_fault &&
               c.contacts == unsigned(response_expected) &&
               c.auth_fail_streak_.load() == (response_expected ? 0 : 2) &&
               c.cmd_fail_streak_.load() == (response_expected ? 0 : 2),
               "only response-bearing native success stamps reachability and clears response failures",
               "TX-only callback claimed a vehicle response or response success lost contact");
    }
    {
        WakeController c;
        c.locked_send = [](WakeController::Builder) {
            WakeController::CommandOutcome out; out.error = "injected pre-transmission failure"; return out;
        };
        const auto started = native_ticks;
        expect(!c.wake_up(2500) && native_ticks == started && c.sends == 1 &&
               c.last_error_ == "injected pre-transmission failure" &&
               !tk::execution_uncertain(c.observed.failure) && !c.cmd_in_flight_.load() && c.command_mutex_->available,
               "actual wake returns definite send failure without confirmation wait",
               "definite send error was delayed, replaced or retained a command lock");
    }
    {
        WakeController c; c.command_mutex_->fail = true;
        expect(!c.wake_up(2500) && c.sends == 0 && !tk::execution_uncertain(c.observed.failure) &&
               c.last_error_ == "command deadline exhausted waiting for another request",
               "actual generic handoff preserves an unsubmitted mutex/deadline error",
               "wake converted mutex refusal into uncertain transmission");
    }
    for (bool wraps : {false, true}) for (unsigned preempt_at : {3u, 4u}) {
        WakeController c;
        native_ticks = wraps ? 0xfffffff0u : 1000u;
        const uint32_t budget = wraps ? 40 : 2500;
        const uint32_t expected_deadline = native_ticks + budget;
        unsigned clock_reads = 0;
        native_wake_tick_hook = [&] {
            if (++clock_reads == preempt_at) native_ticks += wraps ? 20 : 1000;
        };
        c.locked_send = [](WakeController::Builder) {
            WakeController::CommandOutcome out; out.error = "injected pre-transmission failure"; return out;
        };
        const bool success = c.wake_up(budget);
        native_wake_tick_hook = {};
        expect(!success && c.sends == 1 && c.observed_send_deadline == expected_deadline,
               "actual wake keeps its original absolute send deadline across preemption and tick wrap",
               "wake renewed its send budget after preemption or lost wrap-safe deadline");
    }
    {
        WakeController c;
        native_ticks = 1000;
        unsigned clock_reads = 0;
        native_wake_tick_hook = [&] { if (++clock_reads == 3) native_ticks += 1000; };
        const bool success = c.wake_up(500);
        native_wake_tick_hook = {};
        expect(!success && c.sends == 0 && c.last_error_ == "command deadline exhausted" &&
               !tk::execution_uncertain(c.observed.failure),
               "preemption past the absolute wake deadline rejects the native send handoff",
               "wake dispatched after its outer deadline expired during preemption");
    }
    {
        WakeController c;
        native_ticks = 1000;
        unsigned clock_reads = 0;
        // In the former cap helper, read5 lies between the two remaining-time comparisons:
        // outer3500 has expired at9000, but cap10000 still has1000 ticks left.
        native_wake_tick_hook = [&] { if (++clock_reads == 5) native_ticks += 8000; };
        c.locked_send = [](WakeController::Builder) {
            WakeController::CommandOutcome out; out.success = true; return out;
        };
        const bool success = c.wake_up(2500);
        native_wake_tick_hook = {};
        expect(!success && c.sends == 0 && c.last_error_ == "command deadline exhausted" &&
               !tk::execution_uncertain(c.observed.failure),
               "one-snapshot cap never admits a wake after preemption between remaining-time reads",
               "two-clock cap extended the outer deadline and admitted an expired wake");
    }
    for (bool write_ok : {false, true}) for (bool confirms : {false, true}) {
        vcsec_native::Fixture f; WakeController c;
        c.link_inputs.have_reachable = true;  // VCSEC reachability alone is only Idle.
        c.locked_send = [&](WakeController::Builder builder) {
            const auto id = f.enqueue(vcsec_native::Kind::Action, "Wake", true);
            f.controller.command_builders_[id % tk::CommandRunner::kMaxQueueSize] = std::move(builder);
            f.controller.ble.write_ok = write_ok; f.controller.ble.submit_then_fail = !write_ok;
            f.controller.drive_command_runner_();
            return WakeController::CommandOutcome{f.completions != 0, f.success, f.last_failure, f.last_reason};
        };
        const auto started = native_ticks;
        native_wake_delay_hook = [&] {
            c.last_error_ = "another request changed the shared error";
            if (confirms && native_ticks - started >= 1000) c.link_inputs.have_contact = true;
        };
        const bool success = c.wake_up(2500);
        native_wake_delay_hook = {};
        expect(success == confirms && c.sends == 1 && f.controller.ble.writes == 1 &&
               f.controller.ble.frames.size() == 1 && (success ? c.observed.success && c.last_error_.empty()
               : !c.observed.success && tk::execution_uncertain(c.observed.failure) &&
                 c.observed.error == (write_ok ? "wake confirmation timed out" : f.last_reason) &&
                 c.last_error_.find("another request") == std::string::npos),
               "actual wake confirms only fresh Info cache input; timeout retains call-owned uncertain send result",
               "wake confused TX/VCSEC with Awake, lost send uncertainty or read shared error");
        for (unsigned i = 0; i < 600; ++i) { native_ticks += 1000; f.controller.drive_command_runner_(); }
        expect(f.controller.ble.writes == 1 && c.sends == 1,
               "real post-submission false write and TX success never resend wake during confirmation",
               "wake confirmation or later runner progress repeated a mutation");
    }
    {
        WakeController c; c.link_inputs.have_contact = true;
        expect(c.wake_up(2500) && c.sends == 0, "fresh Info evidence keeps already-Awake fast path", "awake fast path sent a wake");
        c.link_inputs = {};
        expect(!c.wake_up(0) && c.sends == 0 && !tk::execution_uncertain(c.observed.failure),
               "exhausted outer wake deadline cannot dispatch", "expired wake dispatched or claimed uncertainty");
    }
}
