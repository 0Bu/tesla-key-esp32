// Command dispatch: the BLE connect helper, the shared result callback (incl. the
// revocation / desync-backstop heuristics), the generic VCSEC/Infotainment command
// runners, and every user-facing command (wake/charge/limit/port/lock/climate/…).
// Part of the VehicleController implementation split — see vehicle_ctrl_internal.hpp.

#include "vehicle_ctrl.hpp"
#include "vehicle_ctrl_internal.hpp"
#include <esp_log.h>
#include <esp_timer.h>
#include <cstdio>
#include <exception>
#include <utility>

// protobuf generated headers (from tesla-ble)
#include <vcsec.pb.h>
#include <car_server.pb.h>

static const char* TAG = "vehicle_ctrl";

// ─── Connectivity ─────────────────────────────────────────────────────────────

bool VehicleController::ensure_connected_until_(uint32_t deadline, tk::ConnectOrigin origin) {
    if (!ble_) return false;
    if (ble_->is_connected()) {
        // The telemetry loop may have established this link outside this helper. It is still
        // proof that the previous failure run ended; otherwise a later independent outage can
        // inherit the old streak and have its first occurrence suppressed.
        tk::connect_ok_note(connect_fail_);
        return true;
    }
    const int timeout_ms = remaining_ms_(deadline);
    if (timeout_ms <= 0) return false;
    // This is the ONE place a connect attempt is started and bounded, so it is also where
    // the UI's "Searching…" countdown is armed: the attempt gives up exactly at this
    // deadline. Armed for the whole attempt and cleared on every exit path, so the Bluetooth
    // row counts this phase down instead of showing an unbounded animation.
    //
    // The loop exits on that SAME deadline rather than on an accumulated sleep count. Summing
    // nominal 200 ms sleeps under-counts real time — each iteration costs at least its sleep
    // plus tick rounding and preemption (WiFi/BLE coexistence readily adds more), and
    // connect() itself was never counted at all — so the attempt used to outlive the deadline
    // the UI was showing and park the row on "timing out…" for the difference. One clock for
    // the wait and the countdown, the same rule idle_until_next_health_poll_ follows.
    connect_deadline_.store(deadline);
    const int64_t attempt_start_us = esp_timer_get_time();
    ble_->connect("");
    while (!ble_->is_connected() &&
           (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        TickType_t left = ticks_until_(deadline);
        TickType_t step = pdMS_TO_TICKS(200);
        vTaskDelay(left < step ? left : step);
    }
    if (!ble_->is_connected()) {
        ble_->stop_connecting();  // drop the intent so the device returns to idle scanning
        connect_deadline_.store(0);
        // Say what actually happened, and only as often as it is worth saying. The old line
        // ("connection timeout after 10000ms", ERROR, every attempt) asserted a timed-out
        // connect even when the scan never matched the car at all, and a car parked elsewhere
        // therefore emitted 7117 ERROR lines a week — see logic/connect_outcome.hpp for the
        // measurement and the rate-limit rule. Use only adverts from THIS attempt here; the
        // public target_connectable() intentionally retains 90 s of history for a stable UI
        // and would otherwise mislabel a car that has just driven away.
        // target_connectable_since() copies each advert name into a std::string (tesla-ble's
        // matches_vin overload), so it ALLOCATES and can throw — and this path runs when the
        // radio is unhappy, which correlates with the heap being unhappy. Letting that escape
        // would lose the line explaining the failure to the very condition it is reporting on,
        // and would abort the enclosing command besides. -1 is the scanner's own "not known"
        // value (what it returns when it can't take the scan mutex either), so the fallback
        // classifies conservatively: warn, rate-limited, no false at-BLE-limit alarm.
        int connectable = -1;
        try {
            connectable = ble_->target_connectable_since(attempt_start_us);
        } catch (...) {
        }
        const tk::ConnectFail kind = tk::connect_fail_from_connectable(connectable);
        // Origin is explicit rather than inferred from cmd_in_flight_: the background health
        // probe raises that flag too so loop_task cannot inject telemetry into the same FIFO.
        // Foreground requests remain unthrottled; unattended probes use the slow heartbeat.
        const uint64_t now_ms = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
        switch (tk::connect_fail_note(connect_fail_, kind, origin, now_ms)) {
            case tk::ConnectLog::Error:
                ESP_LOGE(TAG, "BLE connect gave up after %dms: %s (attempt %u of this run)",
                         timeout_ms, tk::connect_fail_text(kind), (unsigned) connect_fail_.streak);
                break;
            case tk::ConnectLog::Warn:
                ESP_LOGW(TAG, "BLE connect gave up after %dms: %s (attempt %u of this run; "
                              "repeating every %llu min while unchanged)",
                         timeout_ms, tk::connect_fail_text(kind), (unsigned) connect_fail_.streak,
                         (unsigned long long)(tk::kConnectFailRepeatMs / 60000ULL));
                break;
            case tk::ConnectLog::Suppress:
                // Same cause as the last attempt and not a heartbeat tick. Production builds
                // compile at maximum INFO and therefore emit no line here; only a diagnostic
                // build compiled with maximum DEBUG can expose individual attempts. /status
                // still carries the BLE scan verdict in either build.
                ESP_LOGD(TAG, "BLE connect gave up after %dms: %s (attempt %u)",
                         timeout_ms, tk::connect_fail_text(kind), (unsigned) connect_fail_.streak);
                break;
        }
        return false;
    }
    connect_deadline_.store(0);
    tk::connect_ok_note(connect_fail_);   // a link closes the run; the next failure reports as new
    return true;
}

// ─── Callback factory ─────────────────────────────────────────────────────────

std::string VehicleController::last_command_error() const {
    if (!result_mutex_) return last_error_;
    tk::SemGuard g(result_mutex_);
    return last_error_;
}

void VehicleController::publish_command_outcome_(const CommandOutcome& outcome) {
    if (!result_mutex_) {
        last_error_ = outcome.success ? std::string{} : outcome.error;
        return;
    }
    tk::SemGuard g(result_mutex_);
    last_error_ = outcome.success ? std::string{} : outcome.error;
}

std::shared_ptr<VehicleController::CommandCompletion>
VehicleController::begin_completion_(uint32_t& generation) {
    auto completion = std::make_shared<CommandCompletion>();
    generation = command_generation_.fetch_add(1) + 1;
    if (generation == 0) {
        generation = 1;
        command_generation_.store(generation);
    }
    return completion;
}

void VehicleController::invalidate_and_flush_(uint32_t generation) {
    // tesla-ble has no targeted cancel API. Invalidate first because set_connected(false)
    // synchronously finalises every queued callback; callbacks from the expired request must
    // already see themselves as stale when that flush begins.
    if (command_generation_.load() == generation) {
        uint32_t next = generation + 1;
        command_generation_.store(next ? next : 1);
    }
    {
        tk::SemGuard g(vehicle_mutex_);
        try {
            vehicle_->set_connected(false);
            vcsec_sleep_state_.store(static_cast<int>(TeslaBLE::SleepState::UNKNOWN));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "command FIFO flush threw (%s) — forcing BLE reset", e.what());
            ble_fault_.store(true);
        } catch (...) {
            ESP_LOGE(TAG, "command FIFO flush threw (unknown) — forcing BLE reset");
            ble_fault_.store(true);
        }
    }
    if (ble_) ble_->disconnect();
}

void VehicleController::note_completion_timeout_(
        const char* name, tk::CompletionTimeoutPolicy timeout_policy) {
    // Callers hold command_mutex_, so the health-timeout state is serialized with every signed
    // command path. The logging consequence is identical for the generic command runner and the
    // direct vehicle-status callback path; keeping it here prevents either from bypassing policy.
    const uint64_t now_ms = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
    switch (tk::completion_timeout_note(completion_timeout_, timeout_policy, now_ms)) {
        case tk::CompletionTimeoutLog::Warn:
            if (timeout_policy == tk::CompletionTimeoutPolicy::ForegroundWarn) {
                ESP_LOGW(TAG, "'%s' timed out — invalidating request and flushing command FIFO",
                         name);
            } else {
                ESP_LOGW(TAG, "background health '%s' timed out — invalidating request and "
                              "flushing command FIFO (attempt %u of this run; repeating every %u min)",
                         name, (unsigned)completion_timeout_.streak,
                         (unsigned)(tk::kCompletionTimeoutRepeatMs / 60000ULL));
            }
            break;
        case tk::CompletionTimeoutLog::Suppress:
            ESP_LOGD(TAG, "background '%s' timed out — invalidating request and flushing command FIFO",
                     name);
            break;
    }
}

VehicleController::CommandOutcome VehicleController::await_completion_(
        const std::shared_ptr<CommandCompletion>& completion,
        uint32_t generation, uint32_t deadline, const char* name,
        tk::CompletionTimeoutPolicy timeout_policy) {
    CommandOutcome out;
    if (!completion || !completion->sem) {
        out.error = "command completion unavailable";
        return out;
    }
    const bool signalled =
        xSemaphoreTake(completion->sem, ticks_until_(deadline)) == pdTRUE;
    if (signalled && command_generation_.load() == generation) {
        tk::completion_ok_note(completion_timeout_);
        out.completed = completion->completed;
        out.success   = completion->success;
        out.error     = completion->error;
        return out;
    }

    note_completion_timeout_(name, timeout_policy);
    invalidate_and_flush_(generation);
    return out;
}

VehicleController::ResultCb VehicleController::make_result_cb_(
        const std::shared_ptr<CommandCompletion>& completion,
        uint32_t generation, bool auth_fail_is_revocation) {
    return [this, completion, generation, auth_fail_is_revocation](TeslaBLE::OperationResult result) {
      // A timeout flush synchronously invokes queued callbacks. Generation is deliberately
      // checked before any state mutation, so such callbacks cannot complete a later request.
      if (command_generation_.load() != generation) return;
      try {
        completion->completed = true;
        completion->success = result.compatible_success();
        if (completion->success) {
            // A good response proves the car still trusts our key — clear any pending
            // "key might be gone" streak so a later one-off glitch starts from zero.
            auth_fail_streak_ = 0;
            cmd_fail_streak_.store(0);  // link is answering cleanly → reset the desync backstop
            // It also proves the car is reachable over BLE right now (this fires for the
            // idle VCSEC health poll too), which keeps link_state() out of "Unreachable"
            // while the car merely sleeps nearby. NO_WAKE polls don't update note_contact_.
            note_reachable_();
        }
        if (result.is_failure() && result.error()) {
            const std::string& msg = result.error()->message();
            completion->error = msg;
            ESP_LOGW(TAG, "command failed: %s", msg.c_str());
            // Soft-desync backstop: when the link is churning (buffer-recovery storm) the
            // library reports failures here but recovers internally without throwing, so
            // ble_fault_ never fires. After kCmdFailDropStreak failures in a row, drop the
            // link once (only while paired) to force the same clean rx-buffer/session resync.
            if (cmd_fail_streak_.fetch_add(1) + 1 >= kCmdFailDropStreak) {
                cmd_fail_streak_.store(0);
                if (believed_paired_.load() && !ble_fault_.exchange(true)) {
                    ESP_LOGW(TAG, "telemetry desync: %d consecutive BLE failures — dropping link to resync",
                             kCmdFailDropStreak);
                }
            }
            // Two distinct ways a Tesla signals "your key is no longer whitelisted"
            // (it was deleted on the car side); both must invalidate the pairing so the
            // supervisor re-keys + re-pairs and the UI/evcc stop showing a dead pairing:
            //
            //  a) KEY_NOT_ON_WHITELIST → "… key not on whitelist - pairing required".
            //     Definitive, act immediately — honoured for EVERY command, but only
            //     while we believe we're paired: during enrolment an incoming foreground
            //     command (e.g. evcc traffic) legitimately fails its handshake with
            //     KEY_NOT_ON_WHITELIST, and treating that as a revocation would rotate
            //     the key under the user mid-enrolment (the key just confirmed on the
            //     car would then belong to a stale keypair). Same gate as the primary
            //     set_message_callback observer.
            //  b) The car answers a signed command with a session-info reply that has no
            //     HMAC tag (it can't authenticate a key it no longer holds) → the library
            //     reports "auth response authentication failed". Observed in the field as
            //     the actual response to key deletion. BUT the car returns the *same*
            //     message when it authenticates the key fine yet REFUSES the operation for
            //     the key's role — a Charging-Manager key sending door_lock/door_unlock/
            //     flash_lights/honk_horn/climate/sentry/etc. gets "authentication failed"
            //     too. A role refusal is therefore indistinguishable from a revocation at
            //     this layer, so counting (b) on arbitrary user commands would let two
            //     role-denied calls in a row destroy a perfectly good pairing (forcing a
            //     physical NFC re-enrol). Hence (b) is honoured ONLY for the dedicated
            //     health probe (auth_fail_is_revocation), which sends a GET_STATUS the
            //     Charging-Manager key is always authorised for — there an auth failure
            //     genuinely means revocation. The supervisor runs that probe ~30 s, so a
            //     real deletion is still caught even with no evcc traffic. Two in a row are
            //     required (one-off glitch guard); the counter resets on any success above
            //     and on a BLE disconnect.
            if (msg.find("whitelist") != std::string::npos) {
                if (believed_paired_.load()) {
                    pairing_lost_      = true;
                    auth_fail_streak_  = 0;
                }
            } else if (auth_fail_is_revocation &&
                       msg.find("authentication failed") != std::string::npos) {
                if (++auth_fail_streak_ >= 2) {
                    pairing_lost_     = true;
                    auth_fail_streak_ = 0;
                }
            }
        }
      } catch (const std::exception& e) {
          ESP_LOGE(TAG, "result callback threw (%s) — command result may be partial", e.what());
          completion->completed = true;
          completion->success = false;
      } catch (...) {
          ESP_LOGE(TAG, "result callback threw (unknown) — command result may be partial");
          completion->completed = true;
          completion->success = false;
      }
      if (command_generation_.load() == generation && completion->sem) {
          xSemaphoreGive(completion->sem);
      }
    };
}

// ─── Generic command runners ──────────────────────────────────────────────────

bool VehicleController::send_vcsec_(const std::string& name, Builder builder,
                                     TeslaBLE::WakePolicy wp, int timeout_ms,
                                     tk::ConnectOrigin origin, bool auth_fail_is_revocation,
                                     tk::CompletionTimeoutPolicy timeout_policy) {
    const bool foreground = origin == tk::ConnectOrigin::Foreground;
    CommandOutcome out;
    if (timeout_ms <= 0) {
        out.error = "command deadline exhausted";
        if (foreground) publish_command_outcome_(out);
        return false;
    }
    const uint32_t deadline = deadline_in_(static_cast<uint32_t>(timeout_ms));
    tk::SemGuard cmd_guard(command_mutex_, ticks_until_(deadline));
    if (!cmd_guard) {
        out.error = "command deadline exhausted waiting for another request";
        if (foreground) publish_command_outcome_(out);
        return false;
    }
    tk::InFlightGuard inflight(cmd_in_flight_);
    // Real commands open the active window so loop_task resumes polling; the background
    // health poll passes Background (else the window never expires and the
    // car never gets to idle/sleep).
    if (foreground) last_cmd_ticks_.store(xTaskGetTickCount());
    out = send_vcsec_locked_(name, std::move(builder), wp, deadline,
                             origin, auth_fail_is_revocation, timeout_policy);
    if (foreground) publish_command_outcome_(out);
    return out.success;
}

VehicleController::CommandOutcome VehicleController::send_vcsec_locked_(
        const std::string& name, Builder builder, TeslaBLE::WakePolicy wp,
        uint32_t deadline, tk::ConnectOrigin origin, bool auth_fail_is_revocation,
        tk::CompletionTimeoutPolicy timeout_policy) {
    CommandOutcome out;
    if (!command_identity_ready_()) {
        out.error = "runtime key is not verified; reboot or regenerate required";
        return out;
    }
    if (remaining_ms_(deadline) <= 0) {
        out.error = "command deadline exhausted";
        return out;
    }
    if (!ensure_connected_until_(capped_deadline_(deadline, 10000), origin)) return out;
    if (remaining_ms_(deadline) <= 0) {
        out.error = "command deadline exhausted";
        return out;
    }

    uint32_t generation = 0;
    std::shared_ptr<CommandCompletion> completion = begin_completion_(generation);
    if (!completion->sem) {
        out.error = "command completion unavailable";
        return out;
    }

    try {
        // RAII give — send_command_result runs the protobuf builder, which can throw bad_alloc;
        // a hand-rolled give would then be skipped and vehicle_mutex_ wedged forever (Scenario B).
        tk::SemGuard g(vehicle_mutex_);
        vehicle_->send_command_result(
            UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY,
            name, builder, make_result_cb_(completion, generation, auth_fail_is_revocation), wp);
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "'%s' enqueue threw (%s) — invalidating command FIFO", name.c_str(), e.what());
        // The builder/library may have queued work before throwing. Treat it exactly like a
        // timeout: invalidate first, then synchronously flush, so no partial request survives.
        invalidate_and_flush_(generation);
        out.error = "command enqueue failed";
        return out;
    } catch (...) {
        ESP_LOGE(TAG, "'%s' enqueue threw (unknown) — invalidating command FIFO", name.c_str());
        invalidate_and_flush_(generation);
        out.error = "command enqueue failed";
        return out;
    }
    return await_completion_(completion, generation, deadline, name.c_str(), timeout_policy);
}

bool VehicleController::send_infotainment_(const std::string& name, Builder builder,
                                            int timeout_ms, TeslaBLE::WakePolicy wp) {
    CommandOutcome out;
    if (timeout_ms <= 0) {
        out.error = "command deadline exhausted";
        publish_command_outcome_(out);
        return false;
    }
    const uint32_t deadline = deadline_in_(static_cast<uint32_t>(timeout_ms));
    tk::SemGuard cmd_guard(command_mutex_, ticks_until_(deadline));
    if (!cmd_guard) {
        out.error = "command deadline exhausted waiting for another request";
        publish_command_outcome_(out);
        return false;
    }
    tk::InFlightGuard inflight(cmd_in_flight_);
    last_cmd_ticks_.store(xTaskGetTickCount());
    out = send_infotainment_locked_(name, std::move(builder), deadline, wp);
    publish_command_outcome_(out);
    return out.success;
}

VehicleController::CommandOutcome VehicleController::send_infotainment_locked_(
        const std::string& name, Builder builder, uint32_t deadline,
        TeslaBLE::WakePolicy wp) {
    CommandOutcome out;
    if (!command_identity_ready_()) {
        out.error = "runtime key is not verified; reboot or regenerate required";
        return out;
    }
    if (remaining_ms_(deadline) <= 0) {
        out.error = "command deadline exhausted";
        return out;
    }
    if (!ensure_connected_until_(capped_deadline_(deadline, 10000),
                                 tk::ConnectOrigin::Foreground)) return out;
    if (remaining_ms_(deadline) <= 0) {
        out.error = "command deadline exhausted";
        return out;
    }

    uint32_t generation = 0;
    std::shared_ptr<CommandCompletion> completion = begin_completion_(generation);
    if (!completion->sem) {
        out.error = "command completion unavailable";
        return out;
    }

    try {
        // RAII give — the builder inside send_command_result can throw (Scenario B).
        tk::SemGuard g(vehicle_mutex_);
        // WAKE_IF_NEEDED so charge commands also work when the car is asleep
        // (matches TeslaBleHttpProxy, which auto-wakes the vehicle).
        vehicle_->send_command_result(
            UniversalMessage_Domain_DOMAIN_INFOTAINMENT,
            name, builder, make_result_cb_(completion, generation), wp);
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "'%s' enqueue threw (%s) — invalidating command FIFO", name.c_str(), e.what());
        invalidate_and_flush_(generation);
        out.error = "command enqueue failed";
        return out;
    } catch (...) {
        ESP_LOGE(TAG, "'%s' enqueue threw (unknown) — invalidating command FIFO", name.c_str());
        invalidate_and_flush_(generation);
        out.error = "command enqueue failed";
        return out;
    }
    return await_completion_(completion, generation, deadline, name.c_str(),
                             tk::CompletionTimeoutPolicy::ForegroundWarn);
}

// ─── Commands ─────────────────────────────────────────────────────────────────

bool VehicleController::wake_up(int timeout_ms) {
    CommandOutcome final_outcome;
    if (timeout_ms <= 0) {
        final_outcome.error = "command deadline exhausted";
        publish_command_outcome_(final_outcome);
        return false;
    }
    const uint32_t deadline = deadline_in_(static_cast<uint32_t>(timeout_ms));
    // "Awake" that matters here is the INFOTAINMENT computer — it serves SOC/charge/climate —
    // NOT the always-on VCSEC body controller. A parked, reachable car answers a VCSEC status
    // poll with sleep_status="AWAKE" even while its infotainment sleeps; that is exactly why
    // link_state()==Awake never trusts the VCSEC AWAKE flag (only the debounced ASLEEP one —
    // see its doc / AGENTS.md) and requires live infotainment telemetry. The previous code
    // used that VCSEC "AWAKE" BOTH to short-circuit ("already awake") AND to confirm the wake,
    // so on a nearby-sleeping car it returned success in ~0.4 s WITHOUT ever sending the wake:
    // the car never woke and the web-UI spinner just timed out. Trust live telemetry instead.
    if (link_state() == LinkState::Awake) {
        final_outcome.completed = true;
        final_outcome.success = true;
        publish_command_outcome_(final_outcome);
        return true;  // fresh infotainment data (<60 s) ⇒ awake
    }

    // Fire the wake. The car wakes on the first message; the library retries ~7 s then reports
    // failure even on success (Tesla acks a wake with an authenticated-but-empty response that
    // carries no commandStatus for the library to complete on), so we ignore send_vcsec_'s
    // result and confirm out-of-band below. Sending it also opens the active window
    // (last_cmd_ticks_), so loop_task starts refreshing the charge cache as soon as the car is up.
    int wake_budget_ms = remaining_ms_(deadline);
    if (wake_budget_ms > 9000) wake_budget_ms = 9000;
    if (wake_budget_ms > 0) {
        (void)send_vcsec_("Wake", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
            return c->build_vcsec_action_message(VCSEC_RKEAction_E_RKE_ACTION_WAKE_VEHICLE, b, l);
        }, TeslaBLE::WakePolicy::NO_WAKE_FAIL, wake_budget_ms);
    }

    // Confirm the infotainment actually woke by waiting for live charge telemetry: loop_task
    // polls the now-open window (NO_WAKE_SKIP) and the first response stamps note_contact_,
    // flipping link_state() to Awake. That — not VCSEC — is the honest signal, and it is the
    // very state the web UI's wake spinner waits on, so the two agree. timeout_ms budgets a
    // cold infotainment boot; even a false "not yet" self-heals (the window stays open, so the
    // browser's /status poll picks up Awake moments later).
    while (ticks_until_(deadline) > 0) {
        TickType_t left = ticks_until_(deadline);
        TickType_t step = pdMS_TO_TICKS(500);
        vTaskDelay(left < step ? left : step);
        if (link_state() == LinkState::Awake) {
            final_outcome.completed = true;
            final_outcome.success = true;
            publish_command_outcome_(final_outcome);
            return true;
        }
    }
    final_outcome.error = "wake confirmation timed out";
    publish_command_outcome_(final_outcome);
    return false;
}

bool VehicleController::charge_start(int timeout_ms) {
    return send_infotainment_("Start Charging", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        bool enable = true;
        return c->build_car_server_vehicle_action_message(
            b, l, CarServer_VehicleAction_chargingStartStopAction_tag, &enable);
    }, timeout_ms);
}

bool VehicleController::charge_stop(int timeout_ms) {
    return send_infotainment_("Stop Charging", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        bool enable = false;
        return c->build_car_server_vehicle_action_message(
            b, l, CarServer_VehicleAction_chargingStartStopAction_tag, &enable);
    }, timeout_ms);
}

bool VehicleController::set_charging_amps(int amps, int timeout_ms) {
    CommandOutcome outcome;
    if (timeout_ms <= 0) {
        outcome.error = "command deadline exhausted";
        publish_command_outcome_(outcome);
        return false;
    }
    // One deadline covers waiting for command_mutex_, connecting, the action ACK, both
    // readbacks and the retry gap. No sub-step receives a fresh timeout budget.
    const uint32_t deadline = deadline_in_(static_cast<uint32_t>(timeout_ms));
    // Guard against garbage input. Lower bound 0; upper bound 48 A — the maximum any Tesla
    // onboard charger accepts (docs/README.md documents the same 0–48 range), so a legitimate
    // high-current request (e.g. a 48 A-capable Model 3/Y) is never capped.
    // The car still enforces its own per-model maximum.
    if (amps < 0)  amps = 0;
    if (amps > 48) amps = 48;
    int32_t amps32 = (int32_t)amps;

    ESP_LOGI(TAG, "set charging amps requested: %d A", amps);

    // Keep the action ACK and the independent ChargeState readback in one serialized
    // transaction. cmd_in_flight_ prevents the background task from adding a telemetry
    // poll to tesla-ble's single FIFO while we verify the safety-critical current limit.
    tk::SemGuard cmd_guard(command_mutex_, ticks_until_(deadline));
    if (!cmd_guard) {
        outcome.error = "command deadline exhausted waiting for another request";
        publish_command_outcome_(outcome);
        return false;
    }
    tk::InFlightGuard inflight(cmd_in_flight_);
    last_cmd_ticks_.store(xTaskGetTickCount());

    outcome = send_infotainment_locked_(
            "Set Charging Amps",
            [amps32](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
                return c->build_car_server_vehicle_action_message(
                    b, l, CarServer_VehicleAction_setChargingAmpsAction_tag, &amps32);
            },
            deadline, TeslaBLE::WakePolicy::WAKE_IF_NEEDED);
    if (!outcome.success) {
        publish_command_outcome_(outcome);
        return false;
    }

    tk::ChargingAmpsReadback readback = tk::ChargingAmpsReadback::Missing;
    int observed_amps = 0;
    CommandOutcome poll_outcome;
    for (int attempt = 1; attempt <= 2; ++attempt) {
        int left_ms = remaining_ms_(deadline);
        if (left_ms <= 0) break;

        const uint32_t generation_before = charge_state_generation_.load();
        poll_outcome = send_infotainment_locked_(
            "Verify Charging Amps",
            [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
                return c->build_car_server_get_vehicle_data_message(
                    b, l, CarServer_GetVehicleData_getChargeState_tag);
            },
            deadline, TeslaBLE::WakePolicy::WAKE_IF_NEEDED);
        bool poll_ok = poll_outcome.success;

        const uint32_t generation_after = charge_state_generation_.load();
        ChargeStateResult state = copy_locked_(last_known_charge_);
        if (poll_ok && generation_after != generation_before) {
            observed_amps = state.charging_amps;
            readback = tk::verify_charging_amps(
                amps, state.valid, state.has_charging_amps, state.charging_amps);
            if (readback == tk::ChargingAmpsReadback::Verified) {
                ESP_LOGI(TAG,
                         "set charging amps verified: requested=%d A applied=%d A request=%d A actual=%d A",
                         amps, state.charging_amps,
                         state.has_current_request ? state.charge_current_request : -1,
                         state.has_actual_current ? state.charger_actual_current : -1);
                CommandOutcome verified;
                verified.completed = true;
                verified.success = true;
                publish_command_outcome_(verified);
                return true;
            }
            if (readback == tk::ChargingAmpsReadback::Mismatch) {
                ESP_LOGW(TAG,
                         "set charging amps readback mismatch (attempt %d/2): "
                         "requested=%d A applied=%d A request=%d A actual=%d A",
                         attempt, amps, state.charging_amps,
                         state.has_current_request ? state.charge_current_request : -1,
                         state.has_actual_current ? state.charger_actual_current : -1);
            } else {
                ESP_LOGW(TAG,
                         "set charging amps readback missing charging_amps (attempt %d/2)",
                         attempt);
            }
        } else if (poll_ok) {
            ESP_LOGW(TAG, "set charging amps verification returned no fresh ChargeState (attempt %d/2)",
                     attempt);
        }

        if (attempt < 2 && remaining_ms_(deadline) > 250) vTaskDelay(pdMS_TO_TICKS(250));
    }

    char reason[112];
    if (readback == tk::ChargingAmpsReadback::Mismatch) {
        snprintf(reason, sizeof(reason),
                 "charging amps not applied: requested %d A, vehicle reports %d A",
                 amps, observed_amps);
    } else if (poll_outcome.error.empty()) {
        snprintf(reason, sizeof(reason),
                 "charging amps not verified: no fresh vehicle readback for %d A", amps);
    } else {
        snprintf(reason, sizeof(reason),
                 "charging amps not verified: %s", poll_outcome.error.c_str());
    }
    outcome.completed = poll_outcome.completed;
    outcome.success = false;
    outcome.error = reason;
    publish_command_outcome_(outcome);
    ESP_LOGE(TAG, "%s", reason);
    return false;
}

bool VehicleController::set_charge_limit(int percent, int timeout_ms) {
    // Clamp to the documented 50–100 % range (below 50 the car refuses; above 100 is invalid).
    if (percent < 50)  percent = 50;
    if (percent > 100) percent = 100;
    int32_t pct32 = (int32_t)percent;
    return send_infotainment_("Set Charge Limit", [pct32](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        return c->build_car_server_vehicle_action_message(
            b, l, CarServer_VehicleAction_chargingSetLimitAction_tag, &pct32);
    }, timeout_ms);
}

bool VehicleController::charge_port_open(int timeout_ms) {
    return send_vcsec_("Open Charge Port", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        VCSEC_ClosureMoveRequest req = VCSEC_ClosureMoveRequest_init_zero;
        req.chargePort = VCSEC_ClosureMoveType_E_CLOSURE_MOVE_TYPE_OPEN;
        return c->build_vcsec_closure_message(&req, b, l);
    }, TeslaBLE::WakePolicy::WAKE_IF_NEEDED, timeout_ms);
}

bool VehicleController::charge_port_close(int timeout_ms) {
    return send_vcsec_("Close Charge Port", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        VCSEC_ClosureMoveRequest req = VCSEC_ClosureMoveRequest_init_zero;
        req.chargePort = VCSEC_ClosureMoveType_E_CLOSURE_MOVE_TYPE_CLOSE;
        return c->build_vcsec_closure_message(&req, b, l);
    }, TeslaBLE::WakePolicy::WAKE_IF_NEEDED, timeout_ms);
}

bool VehicleController::door_lock(int timeout_ms) {
    return send_vcsec_("Lock", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        return c->build_vcsec_action_message(VCSEC_RKEAction_E_RKE_ACTION_LOCK, b, l);
    }, TeslaBLE::WakePolicy::WAKE_IF_NEEDED, timeout_ms);
}

bool VehicleController::door_unlock(int timeout_ms) {
    return send_vcsec_("Unlock", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        return c->build_vcsec_action_message(VCSEC_RKEAction_E_RKE_ACTION_UNLOCK, b, l);
    }, TeslaBLE::WakePolicy::WAKE_IF_NEEDED, timeout_ms);
}

bool VehicleController::flash_lights(int timeout_ms) {
    return send_infotainment_("Flash Lights", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        return c->build_car_server_vehicle_action_message(
            b, l, CarServer_VehicleAction_vehicleControlFlashLightsAction_tag, nullptr);
    }, timeout_ms);
}

bool VehicleController::honk_horn(int timeout_ms) {
    return send_infotainment_("Honk Horn", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        return c->build_car_server_vehicle_action_message(
            b, l, CarServer_VehicleAction_vehicleControlHonkHornAction_tag, nullptr);
    }, timeout_ms);
}

bool VehicleController::set_sentry_mode(bool enable, int timeout_ms) {
    return send_infotainment_(enable ? "Sentry On" : "Sentry Off",
        [enable](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
            return c->build_car_server_vehicle_action_message(
                b, l, CarServer_VehicleAction_vehicleControlSetSentryModeAction_tag, &enable);
        }, timeout_ms);
}

bool VehicleController::climate_start(int timeout_ms) {
    return send_infotainment_("Climate On", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        bool enable = true;
        return c->build_car_server_vehicle_action_message(
            b, l, CarServer_VehicleAction_hvacAutoAction_tag, &enable);
    }, timeout_ms);
}

bool VehicleController::climate_stop(int timeout_ms) {
    return send_infotainment_("Climate Off", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        bool enable = false;
        return c->build_car_server_vehicle_action_message(
            b, l, CarServer_VehicleAction_hvacAutoAction_tag, &enable);
    }, timeout_ms);
}

bool VehicleController::set_scheduled_charging(bool enable, int start_minutes, int timeout_ms) {
    if (start_minutes < 0)    start_minutes = 0;
    if (start_minutes > 1439) start_minutes = 1439;
    return send_infotainment_(enable ? "Scheduled Charging On" : "Scheduled Charging Off",
        [enable, start_minutes](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
            CarServer_ScheduledChargingAction act = CarServer_ScheduledChargingAction_init_zero;
            act.enabled       = enable;
            act.charging_time = start_minutes;  // minutes after local midnight
            return c->build_car_server_vehicle_action_message(
                b, l, CarServer_VehicleAction_scheduledChargingAction_tag, &act);
        }, timeout_ms);
}
