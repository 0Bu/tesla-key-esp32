#include "stack_watch.hpp"
#include "heap_supervisor.hpp"

#include "ble_client.hpp"
#include "heap_trend.hpp"
#include "logic/heap_supervision.hpp"
#include "logic/ota_lifetime.hpp"
#include "logic/uptime.hpp"
#include "net.hpp"
#include "ota_update.hpp"
#include "runtime_admission.hpp"
#include "safe_mode.hpp"
#include "task_config.hpp"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace tk {
namespace {
const char* TAG = "heap_supervisor";
constexpr unsigned kInternal = MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL;

struct SupervisorStartup {
    NvsStorageAdapter* cfg{nullptr};
    HeapRecoveryBudget budget{};
};
static_assert(sizeof(SupervisorStartup) <= 2 * sizeof(void*));

struct SupervisorState {
    NvsStorageAdapter* cfg{nullptr};
    HeapRecoveryBudget budget{HeapRecoveryBudget::seed(0, false)};
    HeapWatchdog watchdog;
    ota::CriticalEpisode writer_grace;
    RecoveryHealthyRun healthy_run;
    RecoveryWriteCadence clear_write;
    RecoveryWriteCadence restart_write;
    RecoveryDiagnostic diagnostic;
    bool shutdown_committed{false};
    uint32_t min_stack{UINT32_MAX};

    SupervisorState() = default;
    explicit SupervisorState(const SupervisorStartup& startup) noexcept
        : cfg(startup.cfg), budget(startup.budget) {}
};

// Only app_main writes this small startup record before task creation. The supervisor owns
// its full policy on the allocated task stack; diagnostic readers cannot change its budget.
SupervisorStartup s_startup;
bool s_started = false;

uint64_t monotonic_ms() noexcept {
    return static_cast<uint64_t>(esp_timer_get_time()) / 1000;
}

bool healthy_now(uint64_t now_ms) {
    // Ready release/acquire publishes BleClient construction. Before it, the early supervisor
    // must not touch the SDK shell's non-atomic singleton through the host-state getters.
    if (!runtime_admission_vehicle_ready()) return false;
    return recovery_health_eligible({
        true, ble_host_synced(), ble_host_reset_count() == 0,
        runtime_vehicle_progress_fresh(now_ms), net_is_up(), ota_is_busy(),
        heap_caps_get_largest_free_block(kInternal)});
}

// One fixed task-local cadence covers every degraded cause, including owner changes. A failed
// diagnostic cannot escape to a task boundary or turn its suppression count into write authority.
void report_degraded(SupervisorState& s, uint64_t now_ms, const char* reason,
                     OtaIdentityGateState owner = OtaIdentityGateState::Idle) noexcept {
    uint32_t suppressed = 0;
    if (!s.diagnostic.emit(now_ms, suppressed)) return;
    try {
        ESP_LOGE(TAG, "heap recovery deferred: %s; owner=%s; %u suppressed",
                 reason, operation_owner_name(owner), static_cast<unsigned>(suppressed));
    } catch (...) {}
}

[[noreturn]] void park_recovery() {
    tk::stack_watch_sample(tk::StackWatch::HeapSupervisor); // before nonreturning park

    // Returning from restart or throwing after a confirmed effect grants no second attempt and
    // must not release the retained owner to an OTA/configuration operation.
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}

void maintain_healthy_run(SupervisorState& s, uint64_t now_ms) {
    const bool qualified = s.healthy_run.sample(now_ms, healthy_now(now_ms));
    if (!qualified) return;
    // The common ledger owns all credit. Do not refill even the diagnostic streak before a
    // confirmed durable clear. Immutable boot evidence and activity admission never reopen.
    if (!safe_mode_healthy_clear_pending() || !s.clear_write.due(now_ms)) return;

    OtaHealthCommitGuard owner;
    if (!owner) {
        report_degraded(s, now_ms, "healthy counter maintenance busy", owner.observed_owner());
        return;
    }
    // The first sample preceded the CAS. Re-read every condition, progress and elapsed/gap state
    // under the actual owner before the fallible NVS commit. Safe Mode never reaches this write.
    const uint64_t fresh_ms = monotonic_ms();
    const bool still_qualified = s.healthy_run.sample(fresh_ms, healthy_now(fresh_ms));
    if (!still_qualified) return;
    s.clear_write.attempt(fresh_ms);
    if (!safe_mode_clear_after_healthy_run(*s.cfg, owner, true)) {
        report_degraded(s, fresh_ms, "healthy counter commit unconfirmed");
        return;
    }
    (void)s.budget.decay_after_healthy_run(true);
}

void attempt_heap_restart(SupervisorState& s, uint64_t now_ms, const HeapVerdict& verdict) {
    if (!s.budget.may_restart()) {
        report_degraded(s, now_ms, "untrusted or exhausted restart budget");
        return;
    }
    if (!s.restart_write.due(now_ms)) return;
    OtaFaultRestartGuard owner;
    if (!owner.held()) {
        report_degraded(s, now_ms, "operation in flight", owner.observed_owner());
        return;
    }

    // Recheck after the CAS, without a Vehicle lock. An OTA owner cannot start in this window.
    const size_t largest = heap_caps_get_largest_free_block(kInternal);
    if (largest >= kHeapCriticalBytes || ota_is_busy()) return;
    const uint64_t fresh_ms = monotonic_ms();
    s.restart_write.attempt(fresh_ms);
    // Reserve from the one durable ledger in every runtime mode. Stable, current ownership
    // and a confirmed shared charge precede the separate consecutive-heap breadcrumb.
    const auto reservation = safe_mode_prepare_heap_restart(*s.cfg, owner);
    if (!reservation) {
        report_degraded(s, fresh_ms, "shared restart reservation unconfirmed or exhausted");
        return;
    }
    const uint8_t next = static_cast<uint8_t>(s.budget.effective + 1);
    const HeapReason why = heap_reason_format(next);
    if (!persist_reboot_reason(s.cfg, why.text)) {
        report_degraded(s, fresh_ms, "reboot reason commit unconfirmed");
        return;
    }

    // Last confirmed durable effect. Retain this owner across logging, yield, throw and even an
    // unexpected returning restart; pre-commit exits above release only this guard's own lease.
    owner.seal();
    s.shutdown_committed = true;
    try {
        ESP_LOGE(TAG, "HEAP EXHAUSTED for %u s (internal largest %u B < %u B) — "
                      "RESTARTING (heap streak %u, shared budget %u/%u, reboot_why=heap:%u)",
                 static_cast<unsigned>(verdict.critical_ms / 1000),
                 static_cast<unsigned>(largest), static_cast<unsigned>(kHeapCriticalBytes),
                 static_cast<unsigned>(next), static_cast<unsigned>(reservation.count),
                 static_cast<unsigned>(kUnhealthyRestartLimit), static_cast<unsigned>(next));
    } catch (...) {}
    static_assert(!ota_reboot_confirms_pending_image(OtaRebootClass::AutomaticFaultRecovery));
    vTaskDelay(pdMS_TO_TICKS(300));
    tk::stack_watch_sample(tk::StackWatch::HeapSupervisor); // before terminal effect
    esp_restart();
    park_recovery();
}

void supervisor_iteration(SupervisorState& s) {
    if (s.shutdown_committed) park_recovery();
    const uint64_t now_ms = monotonic_ms();
    const size_t largest = heap_caps_get_largest_free_block(kInternal);
    const uint32_t free_bytes = static_cast<uint32_t>(heap_caps_get_free_size(kInternal));
    const auto verdict = heap_watch(s.watchdog,
        {largest, static_cast<uint32_t>(now_ms), ota_is_busy()});

    // Critical evaluation precedes optional trend locking and logging. This sole writer keeps
    // sampling during Booting, portal, Safe Mode, Fatal and a dead/stalled Vehicle task.
    const bool writer_busy = ota_is_busy();
    const bool excused = s.writer_grace.excused(now_ms, largest, writer_busy,
                                               ota::kCriticalWriterGraceMs);
    if (verdict.action == HeapAction::Restart) {
        if (writer_busy) {
            if (!excused) ota_request_heap_abort();
            report_degraded(s, now_ms, excused ? "OTA writer grace active" :
                                              "awaiting OTA writer abort acknowledgement");
        } else {
            // ota_is_busy becomes false only after the actual writer is quiescent and its
            // owner was released. A network-only quarantine cannot postpone safe recovery.
            attempt_heap_restart(s, now_ms, verdict);
        }
    }
    maintain_healthy_run(s, now_ms);

    try {
        (void)heap_trend_record(uptime_seconds(static_cast<uint64_t>(esp_timer_get_time())),
                                free_bytes, static_cast<uint32_t>(largest));
    } catch (...) { report_degraded(s, now_ms, "optional heap trend failed"); }

    // A supervisor samples its own stack only; never relabel it as a Vehicle measurement. This
    // is a runtime observation, not certification that 3072 bytes covers every failure path.
    const uint32_t stack = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    if (stack < s.min_stack) s.min_stack = stack;
    try {
        // Preserve the historical 8-bit diagnostic fields; only the explicit INTERNAL sample
        // above controls recovery and the retained trend, including boards with PSRAM.
        ESP_LOGI(TAG, "HEAP free=%u largest_block=%u min_free=%u internal_largest=%u supervisor_min_stack=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(largest),
                 static_cast<unsigned>(s.min_stack));
        if (verdict.action == HeapAction::Armed || verdict.action == HeapAction::Watching)
            ESP_LOGE(TAG, "HEAP CRITICAL for %u s; restart in %u s unless recovered",
                     static_cast<unsigned>(verdict.critical_ms / 1000),
                     static_cast<unsigned>(heap_restart_in_ms(verdict.critical_ms) / 1000));
        else if (verdict.action == HeapAction::Recovered)
            ESP_LOGW(TAG, "HEAP critical run ended after %u s; ota_excused=%u",
                     static_cast<unsigned>(verdict.critical_ms / 1000), verdict.ota_excused ? 1u : 0u);
    } catch (...) { report_degraded(s, now_ms, "optional heap diagnostic failed"); }
}

void heap_supervisor_task(void* arg) {
    tk::stack_watch_sample(tk::StackWatch::HeapSupervisor); // current task at entry
    tk::StackWatchScope f135_task_exit(tk::StackWatch::HeapSupervisor);

  try {
    SupervisorState s(*static_cast<const SupervisorStartup*>(arg));
    for (;;) {
        tk::StackWatchScope f135_iteration_exit(tk::StackWatch::HeapSupervisor);
        try {
            supervisor_iteration(s);
        } catch (...) {
            if (s.shutdown_committed) park_recovery();
            // Do not certify an unobserved interval as healthy after a shell failure.
            (void)s.healthy_run.sample(monotonic_ms(), false);
            report_degraded(s, monotonic_ms(), "supervisor iteration failed");
        }
        vTaskDelay(pdMS_TO_TICKS(kRecoverySampleMs));
    }
  } catch (...) {
    // Iteration failures above resume sampling. An unexpected failure outside that boundary
    // closes admission and parks without granting an unrecorded or repeated restart effect.
    runtime_admission_mark_fatal();
    park_recovery();
  }
}
}  // namespace

bool heap_supervisor_start(NvsStorageAdapter& cfg, const RebootReasonRecord& boot_reason) noexcept {
    if (s_started) return false;
    s_startup.cfg = &cfg;
    s_startup.budget = HeapRecoveryBudget::seed(
        boot_reason.heap_restarts, boot_reason.state != RebootReasonState::Error);
    // Optional diagnostic allocation is complete before the essential sampler starts. Allocation
    // failure leaves record/snapshot disabled; the critical decision has no dependency on it.
    (void)heap_trend_prepare();
    if (xTaskCreate(heap_supervisor_task, "heapsup", 3072, &s_startup,
                    kPrioHeapSupervisor, nullptr) != pdPASS) return false;
    s_started = true;
    return true;
}

}  // namespace tk
