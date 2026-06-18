#include "vehicle_ctrl.hpp"
#include <esp_log.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>

// protobuf generated headers (from tesla-ble)
#include <vcsec.pb.h>
#include <car_server.pb.h>

// mbedtls for deriving the public-key fingerprint from the stored PEM key
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha1.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

static const char* TAG = "vehicle_ctrl";

// ─── Custom no-op shared_ptr deleters ────────────────────────────────────────
// Vehicle needs shared_ptr<BleAdapter> and shared_ptr<StorageAdapter>.
// We own the objects externally, so we provide deleters that do nothing.
struct NoDelete {
    void operator()(TeslaBLE::BleAdapter*)    const {}
    void operator()(TeslaBLE::StorageAdapter*)const {}
};

// RAII guard that serializes a full command/query cycle.
namespace {
struct MutexGuard {
    SemaphoreHandle_t m;
    explicit MutexGuard(SemaphoreHandle_t mtx) : m(mtx) { xSemaphoreTake(m, portMAX_DELAY); }
    ~MutexGuard() { xSemaphoreGive(m); }
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;
};

// Translate a nanopb CarServer_ChargeState into our flat result struct. Each scalar is a
// proto3 optional → a single-member oneof in nanopb: present iff which_optional_<f> matches.
void parse_charge_state(const CarServer_ChargeState& cs, ChargeStateResult& out) {
    out.valid = true;
    if (cs.which_optional_battery_level == CarServer_ChargeState_battery_level_tag)
        out.battery_level = (float)cs.optional_battery_level.battery_level;
    if (cs.which_optional_charge_limit_soc == CarServer_ChargeState_charge_limit_soc_tag)
        out.charge_limit_soc = (float)cs.optional_charge_limit_soc.charge_limit_soc;
    if (cs.which_optional_charger_power == CarServer_ChargeState_charger_power_tag)
        out.charger_power = (float)cs.optional_charger_power.charger_power;
    if (cs.which_optional_charge_rate_mph_float == CarServer_ChargeState_charge_rate_mph_float_tag)
        out.charge_rate = cs.optional_charge_rate_mph_float.charge_rate_mph_float;
    if (cs.which_optional_charging_amps == CarServer_ChargeState_charging_amps_tag)
        out.charging_amps = cs.optional_charging_amps.charging_amps;
    if (cs.which_optional_battery_range == CarServer_ChargeState_battery_range_tag)
        out.battery_range = cs.optional_battery_range.battery_range;

    // charging_state is itself a oneof message (which_type holds the variant tag).
    if (cs.has_charging_state) {
        switch (cs.charging_state.which_type) {
            case CarServer_ChargeState_ChargingState_Charging_tag:     out.charging_state = "Charging";     break;
            case CarServer_ChargeState_ChargingState_Disconnected_tag: out.charging_state = "Disconnected"; break;
            case CarServer_ChargeState_ChargingState_Complete_tag:     out.charging_state = "Complete";     break;
            case CarServer_ChargeState_ChargingState_Stopped_tag:      out.charging_state = "Stopped";      break;
            case CarServer_ChargeState_ChargingState_NoPower_tag:      out.charging_state = "NoPower";      break;
            case CarServer_ChargeState_ChargingState_Starting_tag:     out.charging_state = "Starting";     break;
            default:                                                    out.charging_state = "Unknown";      break;
        }
    } else {
        out.charging_state = "Unknown";
    }
}
} // namespace

// ─── init ────────────────────────────────────────────────────────────────────

bool VehicleController::init(const std::string& vin,
                              BleClient& ble,
                              NvsStorageAdapter& storage,
                              NvsStorageAdapter& config_store,
                              std::string& known_mac) {
    ble_          = &ble;
    storage_      = &storage;
    config_store_ = &config_store;
    known_mac_    = &known_mac;
    vin_          = vin;

    cmd_sem_       = xSemaphoreCreateBinary();
    vehicle_mutex_ = xSemaphoreCreateMutex();
    command_mutex_ = xSemaphoreCreateMutex();

    auto ble_sp     = std::shared_ptr<TeslaBLE::BleAdapter>(&ble, NoDelete{});
    auto storage_sp = std::shared_ptr<TeslaBLE::StorageAdapter>(&storage, NoDelete{});
    vehicle_ = std::make_unique<TeslaBLE::Vehicle>(ble_sp, storage_sp);

    vehicle_->set_vin(vin);

    // Wire BLE → Vehicle callbacks
    ble_->set_connected_cb([this](bool connected) {
        xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
        vehicle_->set_connected(connected);
        xSemaphoreGive(vehicle_mutex_);

        // Persist discovered MAC on first connection so we skip scanning next boot
        if (connected && known_mac_ && known_mac_->empty() && config_store_) {
            std::string addr = ble_client_instance()
                               ? ble_client_instance()->peer_addr_str() : "";
            if (!addr.empty()) {
                *known_mac_ = addr;
                config_store_->save_str("ble_mac", addr);
                ESP_LOGI(TAG, "Tesla MAC saved: %s", addr.c_str());
            }
        }
    });
    ble_->set_rx_data_cb([this](const std::vector<uint8_t>& data) {
        xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
        vehicle_->on_rx_data(data);
        xSemaphoreGive(vehicle_mutex_);
    });

    // Persistent charge-state callback: refreshes the cache on *every* ChargeState
    // (the 30 s background refresh in loop_task). Installed once, never cleared; HTTP
    // reads serve last_known_charge_ from this cache without blocking.
    vehicle_->set_charge_state_callback([this](const CarServer_ChargeState& cs) {
        parse_charge_state(cs, last_known_charge_);
    });

    xTaskCreate(loop_task_fn_, "vehicle_loop", 8192, this, 5, &loop_task_);
    xTaskCreate(auto_pair_task_fn_, "auto_pair", 8192, this, 4, &auto_pair_task_);
    ESP_LOGI(TAG, "VehicleController ready for VIN %s", vin.c_str());
    return true;
}

// Automatic pairing supervisor. The hard constraint is the tesla-ble library's
// single FIFO command queue: an unsigned "Whitelist Add Key" lingers in that
// queue until the car confirms it (or ~180 s pass), so anything queued behind it
// is blocked. The earlier design queued a session probe *behind* the whitelist-add,
// so the probe never ran, commands piled up, and overlapping responses corrupted
// the RX buffer ("Invalid message length …"). The car accepted the key while the
// firmware never established a session.
//
// This version keeps the queue clean and runs ONE command at a time per round:
//   1. Probe with a signed VCSEC poll. If the key is already authorised this
//      establishes + persists the session (done). If not, it fails *cleanly* with
//      KEY_NOT_ON_WHITELIST and is popped — no clog.
//   2. Send the whitelist-add and wait for the user to confirm the pairing request
//      on the car's screen. The confirmation completes this command.
//   3. Probe once more — now authorised, this establishes the session.
// On any failure the BLE link is dropped, which flushes the library's command
// queue and RX buffer (set_connected(false)) so the next round starts clean.
void VehicleController::auto_pair_task_fn_(void* arg) {
    auto* self = static_cast<VehicleController*>(arg);
    vTaskDelay(pdMS_TO_TICKS(4000));  // let WiFi/BLE come up first
    while (true) {
        // The car deleted our key (detected via a KEY_NOT_ON_WHITELIST response to any
        // signed command or the health poll below). The stored key is now useless, so
        // re-key — which also clears the session + cache — and fall through to re-pair.
        if (self->pairing_lost_) {
            ESP_LOGW(TAG, "auto-pair: key no longer on the vehicle — regenerating key and re-pairing");
            self->repair_notice_ = true;  // tell the UI why it's asking to pair again
            self->generate_key();   // clears pairing_lost_, session and cached data
            continue;
        }

        if (self->has_session()) {
            // A live session means (re-)pairing succeeded; drop the re-auth notice.
            self->repair_notice_ = false;
            // Paired — on-demand connect serves commands. Periodically run a signed
            // health poll so a key deleted on the car side is noticed even when no
            // commands are flowing (otherwise the UI would keep showing "paired").
            self->health_probe_();
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        // 1. Probe for an existing whitelist entry (clean-fails if not authorised).
        VehicleStatusResult st;
        self->get_vehicle_status(st, 8000);
        if (self->has_session()) {
            ESP_LOGI(TAG, "auto-pair: session established");
            continue;
        }

        // 2. Request enrolment; the car shows a pairing dialog to confirm on screen.
        //    The link is held open while we wait so the car's response can complete
        //    the command without the queue churn of disconnect/reconnect.
        ESP_LOGI(TAG, "auto-pair: requesting key enrolment — confirm on the car's screen");
        self->pair(45000);

        // 3. After confirmation a fresh probe establishes the session.
        self->get_vehicle_status(st, 10000);
        if (self->has_session()) {
            ESP_LOGI(TAG, "auto-pair: pairing complete — session established");
            continue;
        }
        ESP_LOGI(TAG, "auto-pair: not confirmed yet — confirm the request on the car's screen");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void VehicleController::loop_task_fn_(void* arg) {
    auto* self = static_cast<VehicleController*>(arg);
    uint32_t last_poll_ticks    = 0;
    uint32_t last_connect_ticks = 0;
    while (true) {
        xSemaphoreTake(self->vehicle_mutex_, portMAX_DELAY);
        self->vehicle_->loop();
        xSemaphoreGive(self->vehicle_mutex_);

        // Once paired, keep the link warm and refresh the cache so HTTP reads (evcc) are
        // served instantly from cache instead of blocking on an on-demand connect (which
        // would risk an HTTP 502 timeout). While NOT paired we stay completely out of the
        // way: the auto-pair task owns the connection and the single command queue, and a
        // stray charge poll injected into that queue mid-handshake corrupts the pairing
        // exchange (overlapping responses → RX reassembly errors → pairing never completes).
        bool paired = self->has_session();

        // Warm-up connect (paired only): non-blocking, idempotent, throttled.
        if (paired && !self->ble_connected()
            && (xTaskGetTickCount() - last_connect_ticks > pdMS_TO_TICKS(15000))) {
            last_connect_ticks = xTaskGetTickCount();
            self->ble_->connect("");
        }

        // Background charge-state refresh (paired + connected only), every 30 seconds.
        if (paired && self->ble_connected() && (xTaskGetTickCount() - last_poll_ticks > pdMS_TO_TICKS(30000))) {
            last_poll_ticks = xTaskGetTickCount();
            ESP_LOGD(TAG, "background charge-state refresh…");
            // Fire-and-forget poll. We must NOT block here: this task also pumps
            // vehicle_->loop(), which drives the command's transmission/retries. The
            // persistent charge-state callback updates last_known_charge_ when the
            // response arrives. NO_WAKE_SKIP so a sleeping car is left undisturbed.
            xSemaphoreTake(self->vehicle_mutex_, portMAX_DELAY);
            self->vehicle_->charge_state_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP);
            xSemaphoreGive(self->vehicle_mutex_);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

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

VehicleController::ResultCb VehicleController::make_result_cb_() {
    return [this](TeslaBLE::OperationResult result) {
        last_result_ = result.compatible_success();
        if (result.is_failure() && result.error()) {
            const std::string& msg = result.error()->message();
            ESP_LOGW(TAG, "command failed: %s", msg.c_str());
            // KEY_NOT_ON_WHITELIST ("… key not on whitelist - pairing required") means
            // the vehicle no longer trusts our key (it was deleted on the car side).
            // Flag it so the auto-pair supervisor re-keys and restarts pairing, and so
            // the UI/evcc stop reporting a pairing that is actually gone.
            if (msg.find("whitelist") != std::string::npos) {
                pairing_lost_ = true;
            }
        }
        xSemaphoreGive(cmd_sem_);
    };
}

// ─── Generic command runners ──────────────────────────────────────────────────

bool VehicleController::send_vcsec_(const std::string& name, Builder builder,
                                     TeslaBLE::WakePolicy wp, int timeout_ms) {
    MutexGuard cmd_guard(command_mutex_);
    if (!ensure_connected_()) return false;
    xSemaphoreTake(cmd_sem_, 0); // drain in case leftover signal
    last_result_ = false;

    xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
    vehicle_->send_command_result(
        UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY,
        name, builder, make_result_cb_(), wp);
    xSemaphoreGive(vehicle_mutex_);

    bool ok = xSemaphoreTake(cmd_sem_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (!ok) ESP_LOGW(TAG, "'%s' timed out", name.c_str());
    return ok && last_result_;
}

bool VehicleController::send_infotainment_(const std::string& name, Builder builder,
                                            int timeout_ms, TeslaBLE::WakePolicy wp) {
    MutexGuard cmd_guard(command_mutex_);
    if (!ensure_connected_()) return false;
    xSemaphoreTake(cmd_sem_, 0);
    last_result_ = false;

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
    // Tesla acknowledges a VCSEC wake with an *authenticated but empty* response
    // (valid AES-GCM tag, no commandStatus sub-message). The tesla-ble library only
    // completes a "Wake" command on a commandStatus, so the queued Wake exhausts its
    // retries and reports failure even though the car actually woke. Work around it:
    // confirm success with a VCSEC status poll instead of the wake command's result.
    VehicleStatusResult st;
    if (get_vehicle_status(st, 6000) && st.sleep_status == "AWAKE") {
        return true;  // already awake
    }

    // Fire the wake. The car wakes on the first message, but the library keeps
    // retrying (~7s) until it gives up; we wait that out so the command queue clears
    // before polling (a poll queued behind the retrying Wake would never run).
    send_vcsec_("Wake", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        return c->build_vcsec_action_message(VCSEC_RKEAction_E_RKE_ACTION_WAKE_VEHICLE, b, l);
    }, TeslaBLE::WakePolicy::NO_WAKE_FAIL, 9000);

    // Confirm the vehicle is now awake (this also updates the library's observed
    // sleep-state so subsequent charge commands proceed without re-waking).
    for (int i = 0; i < 4; i++) {
        if (get_vehicle_status(st, 6000) && st.sleep_status == "AWAKE") return true;
        vTaskDelay(pdMS_TO_TICKS(800));
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
    int32_t amps32 = (int32_t)amps;
    return send_infotainment_("Set Charging Amps", [amps32](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        return c->build_car_server_vehicle_action_message(
            b, l, CarServer_VehicleAction_setChargingAmpsAction_tag, &amps32);
    }, timeout_ms);
}

bool VehicleController::set_charge_limit(int percent, int timeout_ms) {
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

// ─── Data queries ─────────────────────────────────────────────────────────────

bool VehicleController::get_charge_state(ChargeStateResult& out, int /*timeout_ms*/) {
    // Serve the cached reading instantly and never block. evcc polls vehicle_data
    // frequently and times out quickly, so an on-demand connect + poll here would risk a
    // gateway timeout (HTTP 502). Freshness is maintained out of band: loop_task keeps the
    // BLE link warm while paired and refreshes the cache every 30 s (and immediately after
    // a fresh connect). A sleeping car simply leaves the last value in place. If we have
    // never gotten a reading yet, the caller emits a zeroed-but-well-formed charge_state.
    if (last_known_charge_.valid) {
        out = last_known_charge_;
        return true;
    }
    return false;
}

bool VehicleController::get_vehicle_status(VehicleStatusResult& out, int timeout_ms) {
    MutexGuard cmd_guard(command_mutex_);
    // A VCSEC status poll is the auto-pair / wake probe as well as an HTTP read, so it
    // must be able to bring the BLE link up. With a NO-wake policy it reads status
    // (including ASLEEP) without actually waking the car.
    if (!ensure_connected_()) return false;
    xSemaphoreTake(cmd_sem_, 0);
    pending_status_ = {};

    vehicle_->set_vehicle_status_callback([this](const VCSEC_VehicleStatus& vs) {
        pending_status_.valid = true;
        switch (vs.vehicleLockState) {
            case VCSEC_VehicleLockState_E_VEHICLELOCKSTATE_LOCKED:   pending_status_.lock_state = "LOCKED";   break;
            case VCSEC_VehicleLockState_E_VEHICLELOCKSTATE_UNLOCKED: pending_status_.lock_state = "UNLOCKED"; break;
            default:                                                 pending_status_.lock_state = "UNKNOWN";  break;
        }
        switch (vs.vehicleSleepStatus) {
            case VCSEC_VehicleSleepStatus_E_VEHICLE_SLEEP_STATUS_AWAKE:  pending_status_.sleep_status = "AWAKE";   break;
            case VCSEC_VehicleSleepStatus_E_VEHICLE_SLEEP_STATUS_ASLEEP: pending_status_.sleep_status = "ASLEEP";  break;
            default:                                                      pending_status_.sleep_status = "UNKNOWN"; break;
        }
        switch (vs.userPresence) {
            case VCSEC_UserPresence_E_VEHICLE_USER_PRESENCE_PRESENT:     pending_status_.user_presence = "PRESENT";     break;
            case VCSEC_UserPresence_E_VEHICLE_USER_PRESENCE_NOT_PRESENT: pending_status_.user_presence = "NOT_PRESENT"; break;
            default:                                                      pending_status_.user_presence = "UNKNOWN";     break;
        }
        xSemaphoreGive(cmd_sem_);
    });

    xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
    vehicle_->vcsec_poll();
    xSemaphoreGive(vehicle_mutex_);

    bool ok = xSemaphoreTake(cmd_sem_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
    vehicle_->set_vehicle_status_callback(nullptr);
    xSemaphoreGive(vehicle_mutex_);
    out = pending_status_;
    if (ok && out.valid) last_known_status_ = out;
    return ok && out.valid;
}

// ─── Key management ───────────────────────────────────────────────────────────

bool VehicleController::generate_key() {
    MutexGuard cmd_guard(command_mutex_);
    xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
    vehicle_->regenerate_key();
    xSemaphoreGive(vehicle_mutex_);
    // Record when the key was generated so the UI can show the key's creation
    // date next to its fingerprint. Wall-clock comes from SNTP; if the clock
    // hasn't synced yet this stamps a near-zero value, which the UI ignores.
    if (storage_) {
        time_t now = time(nullptr);
        storage_->save_str("key_created", std::to_string((long long)now));
    }
    // A new key invalidates any existing pairing: the stored session belonged to the
    // previous key/whitelist entry, so a fresh enrolment + handshake is required.
    // Wipe the session and cached data so has_session() flips to false (the UI shows
    // "not paired" and hides the controls/SOC) and the auto-pair loop re-enrolls.
    clear_session_and_cache_();
    pairing_lost_ = false;  // re-keying is the resolution; clear any pending flag
    ESP_LOGI(TAG, "new key generated");
    return true;
}

// Tear down the current pairing without touching the private key. Used by
// generate_key() (re-key) and reset_for_new_vehicle() (VIN change). Must NOT be
// called while holding vehicle_mutex_ (it takes it to reset the in-memory peers).
void VehicleController::clear_session_and_cache_() {
    // Reset the library's in-memory peer sessions (and flush its command queue / RX
    // buffer) so a stale session key cannot be reused. set_connected(false) does this;
    // only bother when something is actually established to avoid a spurious log on a
    // first-boot key generation.
    bool had_link    = ble_ && ble_->is_connected();
    bool had_session = has_session();
    if (had_link) ble_->disconnect();
    if ((had_link || had_session) && vehicle_) {
        xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
        vehicle_->set_connected(false);
        xSemaphoreGive(vehicle_mutex_);
    }

    // Erase the persisted sessions so has_session() is false until a fresh handshake.
    if (storage_) {
        storage_->remove("session_vcsec");
        storage_->remove("session_infotainment");
    }

    // Drop cached readings so /status and vehicle_data never serve old SOC/charge data.
    last_known_charge_ = {};
    last_known_status_ = {};
    ESP_LOGI(TAG, "pairing/session cleared");
}

bool VehicleController::reset_for_new_vehicle() {
    // Regenerating the key already clears the session + cache (see generate_key()).
    generate_key();
    // The discovered BLE MAC belongs to the previous car; drop it so the next boot
    // rediscovers the new vehicle by its VIN-derived advertising name.
    if (config_store_) config_store_->remove("ble_mac");
    ESP_LOGI(TAG, "reset for new vehicle complete");
    return true;
}

bool VehicleController::health_probe_(int timeout_ms) {
    // A signed VCSEC status request. If our key is still whitelisted the car answers
    // with a status (success); if it was deleted the session-info exchange returns
    // KEY_NOT_ON_WHITELIST, which make_result_cb_ turns into pairing_lost_.
    return send_vcsec_("VCSEC Health Poll", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        return c->build_vcsec_information_request_message(
            VCSEC_InformationRequestType_INFORMATION_REQUEST_TYPE_GET_STATUS, b, l);
    }, TeslaBLE::WakePolicy::WAKE_IF_NEEDED, timeout_ms);
}

time_t VehicleController::key_created_at() {
    if (!storage_) return 0;
    std::string s;
    if (!storage_->load_str("key_created", s)) return 0;
    return (time_t)atoll(s.c_str());
}

bool VehicleController::has_key() {
    if (!storage_) return false;
    std::vector<uint8_t> buf;
    return storage_->load("private_key", buf);
}

bool VehicleController::has_session() {
    if (!storage_) return false;
    std::vector<uint8_t> buf;
    return storage_->load("session_vcsec", buf);
}

std::string VehicleController::key_fingerprint() {
    if (!storage_) return "";
    std::vector<uint8_t> pem;
    if (!storage_->load("private_key", pem) || pem.empty()) return "";
    // mbedtls expects the PEM buffer to be NUL-terminated and the length to include it.
    if (pem.back() != '\0') pem.push_back('\0');

    mbedtls_pk_context     pk;   mbedtls_pk_init(&pk);
    mbedtls_entropy_context ent; mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_context drbg; mbedtls_ctr_drbg_init(&drbg);
    std::string fp;

    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, nullptr, 0) == 0 &&
        mbedtls_pk_parse_key(&pk, pem.data(), pem.size(), nullptr, 0,
                             mbedtls_ctr_drbg_random, &drbg) == 0 &&
        mbedtls_pk_get_type(&pk) == MBEDTLS_PK_ECKEY) {
        mbedtls_ecp_keypair* kp = mbedtls_pk_ec(pk);
        mbedtls_ecp_group grp;  mbedtls_ecp_group_init(&grp);
        mbedtls_ecp_point Q;    mbedtls_ecp_point_init(&Q);
        uint8_t pub[65];
        size_t  publen = 0;
        if (mbedtls_ecp_export(kp, &grp, nullptr, &Q) == 0 &&
            mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                           &publen, pub, sizeof(pub)) == 0) {
            // Tesla key id = first 4 bytes of SHA-1 over the uncompressed public point.
            uint8_t sha[20];
            if (mbedtls_sha1(pub, publen, sha) == 0) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X",
                         sha[0], sha[1], sha[2], sha[3]);
                fp = buf;
            }
        }
        mbedtls_ecp_point_free(&Q);
        mbedtls_ecp_group_free(&grp);
    }

    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return fp;
}

bool VehicleController::pair(int timeout_ms) {
    MutexGuard cmd_guard(command_mutex_);
    if (!ensure_connected_()) return false;
    xSemaphoreTake(cmd_sem_, 0);
    last_result_ = false;

    // This firmware only ever enrolls a Charging Manager key (charging + wake),
    // never an owner key — its sole purpose is the evcc BLE integration. Limiting
    // the role keeps the device's stored key from granting full vehicle access.
    const Keys_Role role = Keys_Role_ROLE_CHARGING_MANAGER;

    xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
    // Use send_command_result to get a callback when the whitelist message is delivered.
    // The user still needs to confirm the pairing request shown on the car's screen.
    vehicle_->send_command_result(
        UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY,
        "Whitelist Add Key",
        [role](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
            return c->build_white_list_message(role, VCSEC_KeyFormFactor_KEY_FORM_FACTOR_CLOUD_KEY, b, l);
        },
        make_result_cb_(),
        TeslaBLE::WakePolicy::NO_WAKE_FAIL);
    xSemaphoreGive(vehicle_mutex_);

    bool ok = xSemaphoreTake(cmd_sem_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (!ok) ESP_LOGW(TAG, "pair not confirmed — confirm the pairing request on the car's screen");
    else     ESP_LOGI(TAG, "pair confirmed on the car's screen");
    return ok;
}
