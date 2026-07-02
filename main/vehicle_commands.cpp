// Command dispatch: the BLE connect helper, the shared result callback (incl. the
// revocation / desync-backstop heuristics), the generic VCSEC/Infotainment command
// runners, and every user-facing command (wake/charge/limit/port/lock/climate/…).
// Part of the VehicleController implementation split — see vehicle_ctrl_internal.hpp.

#include "vehicle_ctrl.hpp"
#include "vehicle_ctrl_internal.hpp"
#include <esp_log.h>

// protobuf generated headers (from tesla-ble)
#include <vcsec.pb.h>
#include <car_server.pb.h>

static const char* TAG = "vehicle_ctrl";

// ─── Connectivity ─────────────────────────────────────────────────────────────

bool VehicleController::ensure_connected_(int timeout_ms) {
    if (ble_ && ble_->is_connected()) return true;
    ble_->connect("");
    int waited = 0;
    while (!ble_->is_connected() && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    if (!ble_->is_connected()) {
        ble_->stop_connecting();  // drop the intent so the device returns to idle scanning
        ESP_LOGE(TAG, "connection timeout after %dms", timeout_ms);
        return false;
    }
    return true;
}

// ─── Callback factory ─────────────────────────────────────────────────────────

VehicleController::ResultCb VehicleController::make_result_cb_(bool auth_fail_is_revocation) {
    return [this, auth_fail_is_revocation](TeslaBLE::OperationResult result) {
        last_result_ = result.compatible_success();
        if (last_result_) {
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
            last_error_ = msg;   // surfaced to the HTTP layer / UI as the real reason
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
            //     Definitive, act immediately — honoured for EVERY command.
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
                pairing_lost_      = true;
                auth_fail_streak_  = 0;
            } else if (auth_fail_is_revocation &&
                       msg.find("authentication failed") != std::string::npos) {
                if (++auth_fail_streak_ >= 2) {
                    pairing_lost_     = true;
                    auth_fail_streak_ = 0;
                }
            }
        }
        xSemaphoreGive(cmd_sem_);
    };
}

// ─── Generic command runners ──────────────────────────────────────────────────

// RAII: marks a foreground command "in flight" (cmd_in_flight_) for as long as it is being
// sent + awaited, so loop_task pauses injecting background telemetry polls behind it. Clears
// on every exit path — early return or a throw from the library call.
namespace {
struct InFlightGuard {
    std::atomic<bool>& flag;
    explicit InFlightGuard(std::atomic<bool>& f) : flag(f) { flag.store(true); }
    ~InFlightGuard() { flag.store(false); }
};
}  // namespace

bool VehicleController::send_vcsec_(const std::string& name, Builder builder,
                                     TeslaBLE::WakePolicy wp, int timeout_ms,
                                     bool count_as_activity, bool auth_fail_is_revocation) {
    tk::MutexGuard cmd_guard(command_mutex_);
    InFlightGuard inflight(cmd_in_flight_);
    // Real commands open the active window so loop_task resumes polling; the background
    // health poll passes count_as_activity=false (else the window never expires and the
    // car never gets to idle/sleep).
    if (count_as_activity) last_cmd_ticks_.store(xTaskGetTickCount());
    if (!ensure_connected_()) return false;
    xSemaphoreTake(cmd_sem_, 0); // drain in case leftover signal
    last_result_ = false;
    last_error_.clear();

    xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
    vehicle_->send_command_result(
        UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY,
        name, builder, make_result_cb_(auth_fail_is_revocation), wp);
    xSemaphoreGive(vehicle_mutex_);

    bool ok = xSemaphoreTake(cmd_sem_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (!ok) ESP_LOGW(TAG, "'%s' timed out", name.c_str());
    return ok && last_result_;
}

bool VehicleController::send_infotainment_(const std::string& name, Builder builder,
                                            int timeout_ms, TeslaBLE::WakePolicy wp) {
    tk::MutexGuard cmd_guard(command_mutex_);
    InFlightGuard inflight(cmd_in_flight_);
    // Every infotainment command is a real evcc/manual action → open the active window.
    last_cmd_ticks_.store(xTaskGetTickCount());
    if (!ensure_connected_()) return false;
    xSemaphoreTake(cmd_sem_, 0);
    last_result_ = false;
    last_error_.clear();

    xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
    // WAKE_IF_NEEDED so charge commands also work when the car is asleep
    // (matches TeslaBleHttpProxy, which auto-wakes the vehicle).
    vehicle_->send_command_result(
        UniversalMessage_Domain_DOMAIN_INFOTAINMENT,
        name, builder, make_result_cb_(), wp);
    xSemaphoreGive(vehicle_mutex_);

    bool ok = xSemaphoreTake(cmd_sem_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (!ok) ESP_LOGW(TAG, "'%s' timed out", name.c_str());
    return ok && last_result_;
}

// ─── Commands ─────────────────────────────────────────────────────────────────

bool VehicleController::wake_up(int timeout_ms) {
    // "Awake" that matters here is the INFOTAINMENT computer — it serves SOC/charge/climate —
    // NOT the always-on VCSEC body controller. A parked, reachable car answers a VCSEC status
    // poll with sleep_status="AWAKE" even while its infotainment sleeps; that is exactly why
    // link_state()==Awake never trusts the VCSEC AWAKE flag (only the debounced ASLEEP one —
    // see its doc / CLAUDE.md) and requires live infotainment telemetry. The previous code
    // used that VCSEC "AWAKE" BOTH to short-circuit ("already awake") AND to confirm the wake,
    // so on a nearby-sleeping car it returned success in ~0.4 s WITHOUT ever sending the wake:
    // the car never woke and the web-UI spinner just timed out. Trust live telemetry instead.
    if (link_state() == LinkState::Awake) return true;  // fresh infotainment data (<60 s) ⇒ awake

    // Fire the wake. The car wakes on the first message; the library retries ~7 s then reports
    // failure even on success (Tesla acks a wake with an authenticated-but-empty response that
    // carries no commandStatus for the library to complete on), so we ignore send_vcsec_'s
    // result and confirm out-of-band below. Sending it also opens the active window
    // (last_cmd_ticks_), so loop_task starts refreshing the charge cache as soon as the car is up.
    send_vcsec_("Wake", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        return c->build_vcsec_action_message(VCSEC_RKEAction_E_RKE_ACTION_WAKE_VEHICLE, b, l);
    }, TeslaBLE::WakePolicy::NO_WAKE_FAIL, 9000);

    // Confirm the infotainment actually woke by waiting for live charge telemetry: loop_task
    // polls the now-open window (NO_WAKE_SKIP) and the first response stamps note_contact_,
    // flipping link_state() to Awake. That — not VCSEC — is the honest signal, and it is the
    // very state the web UI's wake spinner waits on, so the two agree. timeout_ms budgets a
    // cold infotainment boot; even a false "not yet" self-heals (the window stays open, so the
    // browser's /status poll picks up Awake moments later).
    const TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (link_state() == LinkState::Awake) return true;
    }
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
    // Guard against garbage input. Lower bound 0; upper bound 48 A — the maximum any Tesla
    // onboard charger accepts (docs/README.md documents the same 0–48 range), so a legitimate
    // high-current request (e.g. a 48 A-capable Model 3/Y) is never capped.
    // The car still enforces its own per-model maximum.
    if (amps < 0)  amps = 0;
    if (amps > 48) amps = 48;
    int32_t amps32 = (int32_t)amps;
    return send_infotainment_("Set Charging Amps", [amps32](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        return c->build_car_server_vehicle_action_message(
            b, l, CarServer_VehicleAction_setChargingAmpsAction_tag, &amps32);
    }, timeout_ms);
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
