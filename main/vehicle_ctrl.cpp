#include "vehicle_ctrl.hpp"
#include <esp_log.h>

// protobuf generated headers (from tesla-ble)
#include <vcsec.pb.h>
#include <car_server.pb.h>

static const char* TAG = "vehicle_ctrl";

// ─── Custom no-op shared_ptr deleters ────────────────────────────────────────
// Vehicle needs shared_ptr<BleAdapter> and shared_ptr<StorageAdapter>.
// We own the objects externally, so we provide deleters that do nothing.
struct NoDelete {
    void operator()(TeslaBLE::BleAdapter*)    const {}
    void operator()(TeslaBLE::StorageAdapter*)const {}
};

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

    xTaskCreate(loop_task_fn_, "vehicle_loop", 8192, this, 5, &loop_task_);
    ESP_LOGI(TAG, "VehicleController ready for VIN %s", vin.c_str());
    return true;
}

void VehicleController::loop_task_fn_(void* arg) {
    auto* self = static_cast<VehicleController*>(arg);
    while (true) {
        xSemaphoreTake(self->vehicle_mutex_, portMAX_DELAY);
        self->vehicle_->loop();
        xSemaphoreGive(self->vehicle_mutex_);
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
            ESP_LOGW(TAG, "command failed: %s", result.error()->message().c_str());
        }
        xSemaphoreGive(cmd_sem_);
    };
}

// ─── Generic command runners ──────────────────────────────────────────────────

bool VehicleController::send_vcsec_(const std::string& name, Builder builder,
                                     TeslaBLE::WakePolicy wp, int timeout_ms) {
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
                                            int timeout_ms) {
    if (!ensure_connected_()) return false;
    xSemaphoreTake(cmd_sem_, 0);
    last_result_ = false;

    xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
    vehicle_->send_command_result(
        UniversalMessage_Domain_DOMAIN_INFOTAINMENT,
        name, builder, make_result_cb_());
    xSemaphoreGive(vehicle_mutex_);

    bool ok = xSemaphoreTake(cmd_sem_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (!ok) ESP_LOGW(TAG, "'%s' timed out", name.c_str());
    return ok && last_result_;
}

// ─── Commands ─────────────────────────────────────────────────────────────────

bool VehicleController::wake_up(int timeout_ms) {
    return send_vcsec_("Wake", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        return c->build_vcsec_action_message(VCSEC_RKEAction_E_RKE_ACTION_WAKE_VEHICLE, b, l);
    }, TeslaBLE::WakePolicy::NO_WAKE_FAIL, timeout_ms);
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

bool VehicleController::get_charge_state(ChargeStateResult& out, int timeout_ms) {
    if (!ensure_connected_()) return false;
    xSemaphoreTake(cmd_sem_, 0);
    pending_charge_ = {};

    // Store result in member (not stack) so there's no use-after-free if timeout fires
    vehicle_->set_charge_state_callback([this](const CarServer_ChargeState& cs) {
        pending_charge_.valid            = true;
        pending_charge_.battery_level    = (float)cs.battery_level;
        pending_charge_.charge_limit_soc = (float)cs.charge_limit_soc;
        pending_charge_.charger_power    = (float)cs.charger_power;
        pending_charge_.charge_rate      = cs.charge_rate;
        pending_charge_.charging_amps    = cs.charging_amps;
        pending_charge_.battery_range    = cs.battery_range;
        switch (cs.charging_state) {
            case CarServer_ChargingState_Charging:     pending_charge_.charging_state = "Charging";     break;
            case CarServer_ChargingState_Disconnected: pending_charge_.charging_state = "Disconnected"; break;
            case CarServer_ChargingState_Complete:     pending_charge_.charging_state = "Complete";     break;
            case CarServer_ChargingState_Stopped:      pending_charge_.charging_state = "Stopped";      break;
            default:                                   pending_charge_.charging_state = "Unknown";      break;
        }
        xSemaphoreGive(cmd_sem_);
    });

    xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
    vehicle_->charge_state_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP);
    xSemaphoreGive(vehicle_mutex_);

    bool ok = xSemaphoreTake(cmd_sem_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
    vehicle_->set_charge_state_callback(nullptr);
    xSemaphoreGive(vehicle_mutex_);
    out = pending_charge_;
    return ok && out.valid;
}

bool VehicleController::get_vehicle_status(VehicleStatusResult& out, int timeout_ms) {
    if (!ensure_connected_()) return false;
    xSemaphoreTake(cmd_sem_, 0);
    pending_status_ = {};

    vehicle_->set_vehicle_status_callback([this](const VCSEC_VehicleStatus& vs) {
        pending_status_.valid = true;
        switch (vs.vehicleLockState) {
            case VCSEC_VehicleLockState_VEHICLELOCKSTATE_LOCKED:   pending_status_.lock_state = "LOCKED";   break;
            case VCSEC_VehicleLockState_VEHICLELOCKSTATE_UNLOCKED: pending_status_.lock_state = "UNLOCKED"; break;
            default:                                               pending_status_.lock_state = "UNKNOWN";  break;
        }
        switch (vs.vehicleSleepStatus) {
            case VCSEC_VehicleSleepStatus_VEHICLE_SLEEP_STATUS_AWAKE:  pending_status_.sleep_status = "AWAKE";   break;
            case VCSEC_VehicleSleepStatus_VEHICLE_SLEEP_STATUS_ASLEEP: pending_status_.sleep_status = "ASLEEP";  break;
            default:                                                    pending_status_.sleep_status = "UNKNOWN"; break;
        }
        switch (vs.userPresence) {
            case VCSEC_UserPresence_VEHICLE_USER_PRESENCE_PRESENT:     pending_status_.user_presence = "PRESENT";     break;
            case VCSEC_UserPresence_VEHICLE_USER_PRESENCE_NOT_PRESENT: pending_status_.user_presence = "NOT_PRESENT"; break;
            default:                                                    pending_status_.user_presence = "UNKNOWN";     break;
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
    return ok && out.valid;
}

// ─── Key management ───────────────────────────────────────────────────────────

bool VehicleController::generate_key() {
    xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
    vehicle_->regenerate_key();
    xSemaphoreGive(vehicle_mutex_);
    ESP_LOGI(TAG, "new key generated");
    return true;
}

bool VehicleController::pair(bool owner_role, int timeout_ms) {
    if (!ensure_connected_()) return false;
    xSemaphoreTake(cmd_sem_, 0);
    last_result_ = false;

    Keys_Role role = owner_role ? Keys_Role_ROLE_OWNER : Keys_Role_ROLE_CHARGING_MANAGER;

    xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
    // Use send_command_result to get a callback when the whitelist message is delivered.
    // The user still needs to confirm by tapping an NFC card on the center console.
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
    if (!ok) ESP_LOGW(TAG, "pair timed out — tap NFC card on Tesla center console");
    ESP_LOGI(TAG, "pair %s — confirm on vehicle touchscreen", ok ? "sent" : "failed");
    return ok;
}
