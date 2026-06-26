#include "vehicle_ctrl.hpp"
#include <esp_log.h>
#include <esp_heap_caps.h>
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
    if (cs.which_optional_battery_level == CarServer_ChargeState_battery_level_tag) {
        out.battery_level = (float)cs.optional_battery_level.battery_level; out.has_battery_level = true;
    }
    if (cs.which_optional_charge_limit_soc == CarServer_ChargeState_charge_limit_soc_tag) {
        out.charge_limit_soc = (float)cs.optional_charge_limit_soc.charge_limit_soc; out.has_charge_limit_soc = true;
    }
    if (cs.which_optional_charger_power == CarServer_ChargeState_charger_power_tag) {
        out.charger_power = (float)cs.optional_charger_power.charger_power; out.has_charger_power = true;
    }
    if (cs.which_optional_charge_rate_mph_float == CarServer_ChargeState_charge_rate_mph_float_tag) {
        out.charge_rate = cs.optional_charge_rate_mph_float.charge_rate_mph_float; out.has_charge_rate = true;
    }
    if (cs.which_optional_charging_amps == CarServer_ChargeState_charging_amps_tag) {
        out.charging_amps = cs.optional_charging_amps.charging_amps; out.has_charging_amps = true;
    }
    if (cs.which_optional_battery_range == CarServer_ChargeState_battery_range_tag) {
        out.battery_range = cs.optional_battery_range.battery_range; out.has_battery_range = true;
    }

    // Extended read-only charge telemetry (HA bridge only). Same single-member-oneof pattern;
    // each costs nothing extra — it rides the charge_state poll the fields above already need.
    if (cs.which_optional_charger_actual_current == CarServer_ChargeState_charger_actual_current_tag) {
        out.charger_actual_current = cs.optional_charger_actual_current.charger_actual_current;
        out.has_actual_current = true;
    }
    if (cs.which_optional_charger_voltage == CarServer_ChargeState_charger_voltage_tag) {
        out.charger_voltage = cs.optional_charger_voltage.charger_voltage; out.has_voltage = true;
    }
    if (cs.which_optional_charge_current_request == CarServer_ChargeState_charge_current_request_tag) {
        out.charge_current_request = cs.optional_charge_current_request.charge_current_request;
        out.has_current_request = true;
    }
    if (cs.which_optional_charger_phases == CarServer_ChargeState_charger_phases_tag) {
        out.charger_phases = cs.optional_charger_phases.charger_phases; out.has_charger_phases = true;
    }
    if (cs.which_optional_charge_energy_added == CarServer_ChargeState_charge_energy_added_tag) {
        out.charge_energy_added = cs.optional_charge_energy_added.charge_energy_added;
        out.has_energy_added = true;
    }
    if (cs.which_optional_minutes_to_full_charge == CarServer_ChargeState_minutes_to_full_charge_tag) {
        out.minutes_to_full_charge = cs.optional_minutes_to_full_charge.minutes_to_full_charge;
        out.has_minutes_to_full = true;
    }
    if (cs.which_optional_charge_limit_reason == CarServer_ChargeState_charge_limit_reason_tag) {
        switch (cs.optional_charge_limit_reason.charge_limit_reason) {
            case CarServer_ChargeState_ChargeLimitReason_ChargeLimitReasonNone:        out.charge_limit_reason = "None";        break;
            case CarServer_ChargeState_ChargeLimitReason_ChargeLimitReasonEvse:        out.charge_limit_reason = "EVSE";        break;
            case CarServer_ChargeState_ChargeLimitReason_ChargeLimitReasonBattTempLow: out.charge_limit_reason = "BattTempLow"; break;
            case CarServer_ChargeState_ChargeLimitReason_ChargeLimitReasonHighSoc:     out.charge_limit_reason = "HighSoc";     break;
            case CarServer_ChargeState_ChargeLimitReason_ChargeLimitReasonCabin:       out.charge_limit_reason = "Cabin";       break;
            default:                                                                    out.charge_limit_reason = "";            break;  // Unknown → omit
        }
    }

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

// ─── Telemetry parsers (same proto3-optional → single-member-oneof pattern) ──────

void parse_climate_state(const CarServer_ClimateState& cs, ClimateStateResult& out) {
    out.valid = true;
    if (cs.which_optional_inside_temp_celsius == CarServer_ClimateState_inside_temp_celsius_tag) {
        out.inside_temp = cs.optional_inside_temp_celsius.inside_temp_celsius; out.has_inside = true;
    }
    if (cs.which_optional_outside_temp_celsius == CarServer_ClimateState_outside_temp_celsius_tag) {
        out.outside_temp = cs.optional_outside_temp_celsius.outside_temp_celsius; out.has_outside = true;
    }
    if (cs.which_optional_driver_temp_setting == CarServer_ClimateState_driver_temp_setting_tag) {
        out.driver_setpoint = cs.optional_driver_temp_setting.driver_temp_setting; out.has_setpoint = true;
    }
    if (cs.which_optional_is_climate_on == CarServer_ClimateState_is_climate_on_tag)
        out.is_climate_on = cs.optional_is_climate_on.is_climate_on;
    if (cs.which_optional_is_preconditioning == CarServer_ClimateState_is_preconditioning_tag)
        out.is_preconditioning = cs.optional_is_preconditioning.is_preconditioning;

    // Cabin Overheat Protection — separate from the main HVAC (see ClimateStateResult).
    if (cs.which_optional_cabin_overheat_protection == CarServer_ClimateState_cabin_overheat_protection_tag) {
        out.has_cop = true;
        switch (cs.optional_cabin_overheat_protection.cabin_overheat_protection) {
            case CarServer_ClimateState_CabinOverheatProtection_E_CabinOverheatProtectionOff:     out.cop = "Off";     break;
            case CarServer_ClimateState_CabinOverheatProtection_E_CabinOverheatProtectionOn:      out.cop = "On";      break;
            case CarServer_ClimateState_CabinOverheatProtection_E_CabinOverheatProtectionFanOnly: out.cop = "FanOnly"; break;
            default:                                                                              out.cop = "Unknown"; break;
        }
    }
    if (cs.which_optional_cabin_overheat_protection_actively_cooling ==
        CarServer_ClimateState_cabin_overheat_protection_actively_cooling_tag) {
        out.has_cop_cooling = true;
        out.cop_cooling = cs.optional_cabin_overheat_protection_actively_cooling.cabin_overheat_protection_actively_cooling;
    }
    if (cs.which_optional_cop_activation_temperature == CarServer_ClimateState_cop_activation_temperature_tag) {
        out.has_cop_temp = true;
        switch (cs.optional_cop_activation_temperature.cop_activation_temperature) {
            case CarServer_ClimateState_CopActivationTemp_CopActivationTempLow:    out.cop_temp = "Low";         break;
            case CarServer_ClimateState_CopActivationTemp_CopActivationTempMedium: out.cop_temp = "Medium";      break;
            case CarServer_ClimateState_CopActivationTemp_CopActivationTempHigh:   out.cop_temp = "High";        break;
            default:                                                               out.cop_temp = "Unspecified"; break;
        }
    }
    if (cs.which_optional_cop_not_running_reason == CarServer_ClimateState_cop_not_running_reason_tag) {
        out.has_cop_reason = true;
        switch (cs.optional_cop_not_running_reason.cop_not_running_reason) {
            case CarServer_ClimateState_COPNotRunningReason_COPNotRunningReasonNoReason:                 out.cop_reason = "None";           break;
            case CarServer_ClimateState_COPNotRunningReason_COPNotRunningReasonUserInteraction:          out.cop_reason = "UserInteract";   break;
            case CarServer_ClimateState_COPNotRunningReason_COPNotRunningReasonEnergyConsumptionReached: out.cop_reason = "EnergyReached";  break;
            case CarServer_ClimateState_COPNotRunningReason_COPNotRunningReasonTimeout:                  out.cop_reason = "Timeout";        break;
            case CarServer_ClimateState_COPNotRunningReason_COPNotRunningReasonLowSolarLoad:             out.cop_reason = "LowSolarLoad";   break;
            case CarServer_ClimateState_COPNotRunningReason_COPNotRunningReasonFault:                    out.cop_reason = "Fault";          break;
            case CarServer_ClimateState_COPNotRunningReason_COPNotRunningReasonCabinBelowThreshold:      out.cop_reason = "BelowThreshold"; break;
            default:                                                                                     out.cop_reason = "Unknown";        break;
        }
    }

    // Defrost — front/rear defroster booleans + the Max-defrost mode (Off/Normal/Max).
    if (cs.which_optional_is_front_defroster_on == CarServer_ClimateState_is_front_defroster_on_tag) {
        out.has_front_defrost = true;
        out.front_defrost = cs.optional_is_front_defroster_on.is_front_defroster_on;
    }
    if (cs.which_optional_is_rear_defroster_on == CarServer_ClimateState_is_rear_defroster_on_tag) {
        out.has_rear_defrost = true;
        out.rear_defrost = cs.optional_is_rear_defroster_on.is_rear_defroster_on;
    }
    if (cs.has_defrost_mode) {
        out.has_defrost_mode = true;
        switch (cs.defrost_mode.which_type) {
            case CarServer_ClimateState_DefrostMode_Off_tag:    out.defrost_mode = "Off";    break;
            case CarServer_ClimateState_DefrostMode_Normal_tag: out.defrost_mode = "Normal"; break;
            case CarServer_ClimateState_DefrostMode_Max_tag:    out.defrost_mode = "Max";    break;
            default:                                            out.defrost_mode = "";       break;
        }
    }
}

void parse_drive_state(const CarServer_DriveState& ds, DriveStateResult& out) {
    out.valid = true;
    if (ds.has_shift_state) {
        switch (ds.shift_state.which_type) {
            case CarServer_ShiftState_P_tag: out.shift_state = "P"; break;
            case CarServer_ShiftState_R_tag: out.shift_state = "R"; break;
            case CarServer_ShiftState_N_tag: out.shift_state = "N"; break;
            case CarServer_ShiftState_D_tag: out.shift_state = "D"; break;
            default: out.shift_state = ""; break;
        }
    }
    if (ds.which_optional_odometer_in_hundredths_of_a_mile ==
        CarServer_DriveState_odometer_in_hundredths_of_a_mile_tag) {
        // hundredths of a mile → km
        out.odometer_km = ds.optional_odometer_in_hundredths_of_a_mile.odometer_in_hundredths_of_a_mile
                          * 0.01f * 1.609344f;
        out.has_odometer = true;
    }
}

void parse_tire_pressure(const CarServer_TirePressureState& t, TirePressureResult& out) {
    out.valid = true;
    if (t.which_optional_tpms_pressure_fl == CarServer_TirePressureState_tpms_pressure_fl_tag) {
        out.fl = t.optional_tpms_pressure_fl.tpms_pressure_fl; out.has_fl = true;
    }
    if (t.which_optional_tpms_pressure_fr == CarServer_TirePressureState_tpms_pressure_fr_tag) {
        out.fr = t.optional_tpms_pressure_fr.tpms_pressure_fr; out.has_fr = true;
    }
    if (t.which_optional_tpms_pressure_rl == CarServer_TirePressureState_tpms_pressure_rl_tag) {
        out.rl = t.optional_tpms_pressure_rl.tpms_pressure_rl; out.has_rl = true;
    }
    if (t.which_optional_tpms_pressure_rr == CarServer_TirePressureState_tpms_pressure_rr_tag) {
        out.rr = t.optional_tpms_pressure_rr.tpms_pressure_rr; out.has_rr = true;
    }
    auto warn = [](pb_size_t w, pb_size_t tag, bool v) { return w == tag && v; };
    out.warn =
        warn(t.which_optional_tpms_soft_warning_fl, CarServer_TirePressureState_tpms_soft_warning_fl_tag,
             t.optional_tpms_soft_warning_fl.tpms_soft_warning_fl) ||
        warn(t.which_optional_tpms_soft_warning_fr, CarServer_TirePressureState_tpms_soft_warning_fr_tag,
             t.optional_tpms_soft_warning_fr.tpms_soft_warning_fr) ||
        warn(t.which_optional_tpms_soft_warning_rl, CarServer_TirePressureState_tpms_soft_warning_rl_tag,
             t.optional_tpms_soft_warning_rl.tpms_soft_warning_rl) ||
        warn(t.which_optional_tpms_soft_warning_rr, CarServer_TirePressureState_tpms_soft_warning_rr_tag,
             t.optional_tpms_soft_warning_rr.tpms_soft_warning_rr) ||
        warn(t.which_optional_tpms_hard_warning_fl, CarServer_TirePressureState_tpms_hard_warning_fl_tag,
             t.optional_tpms_hard_warning_fl.tpms_hard_warning_fl) ||
        warn(t.which_optional_tpms_hard_warning_fr, CarServer_TirePressureState_tpms_hard_warning_fr_tag,
             t.optional_tpms_hard_warning_fr.tpms_hard_warning_fr) ||
        warn(t.which_optional_tpms_hard_warning_rl, CarServer_TirePressureState_tpms_hard_warning_rl_tag,
             t.optional_tpms_hard_warning_rl.tpms_hard_warning_rl) ||
        warn(t.which_optional_tpms_hard_warning_rr, CarServer_TirePressureState_tpms_hard_warning_rr_tag,
             t.optional_tpms_hard_warning_rr.tpms_hard_warning_rr);
}

void parse_closures_state(const CarServer_ClosuresState& c, ClosuresStateResult& out) {
    out.valid = true;
    auto on = [](pb_size_t w, pb_size_t tag, bool v) { return w == tag && v; };
    if (c.which_optional_locked == CarServer_ClosuresState_locked_tag) {
        out.locked = c.optional_locked.locked; out.has_locked = true;
    }
    out.any_door_open =
        on(c.which_optional_door_open_driver_front, CarServer_ClosuresState_door_open_driver_front_tag,
           c.optional_door_open_driver_front.door_open_driver_front) ||
        on(c.which_optional_door_open_driver_rear, CarServer_ClosuresState_door_open_driver_rear_tag,
           c.optional_door_open_driver_rear.door_open_driver_rear) ||
        on(c.which_optional_door_open_passenger_front, CarServer_ClosuresState_door_open_passenger_front_tag,
           c.optional_door_open_passenger_front.door_open_passenger_front) ||
        on(c.which_optional_door_open_passenger_rear, CarServer_ClosuresState_door_open_passenger_rear_tag,
           c.optional_door_open_passenger_rear.door_open_passenger_rear);
    out.frunk_open = on(c.which_optional_door_open_trunk_front, CarServer_ClosuresState_door_open_trunk_front_tag,
                        c.optional_door_open_trunk_front.door_open_trunk_front);
    out.trunk_open = on(c.which_optional_door_open_trunk_rear, CarServer_ClosuresState_door_open_trunk_rear_tag,
                        c.optional_door_open_trunk_rear.door_open_trunk_rear);
    out.any_window_open =
        on(c.which_optional_window_open_driver_front, CarServer_ClosuresState_window_open_driver_front_tag,
           c.optional_window_open_driver_front.window_open_driver_front) ||
        on(c.which_optional_window_open_passenger_front, CarServer_ClosuresState_window_open_passenger_front_tag,
           c.optional_window_open_passenger_front.window_open_passenger_front) ||
        on(c.which_optional_window_open_driver_rear, CarServer_ClosuresState_window_open_driver_rear_tag,
           c.optional_window_open_driver_rear.window_open_driver_rear) ||
        on(c.which_optional_window_open_passenger_rear, CarServer_ClosuresState_window_open_passenger_rear_tag,
           c.optional_window_open_passenger_rear.window_open_passenger_rear);
    if (c.which_optional_is_user_present == CarServer_ClosuresState_is_user_present_tag) {
        out.user_present = c.optional_is_user_present.is_user_present; out.has_user_present = true;
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
    cache_mutex_   = xSemaphoreCreateMutex();

    auto ble_sp     = std::shared_ptr<TeslaBLE::BleAdapter>(&ble, NoDelete{});
    auto storage_sp = std::shared_ptr<TeslaBLE::StorageAdapter>(&storage, NoDelete{});
    vehicle_ = std::make_unique<TeslaBLE::Vehicle>(ble_sp, storage_sp);

    vehicle_->set_vin(vin);

    // Wire BLE → Vehicle callbacks
    ble_->set_connected_cb([this](bool connected) {
        xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
        vehicle_->set_connected(connected);
        xSemaphoreGive(vehicle_mutex_);

        if (!connected) {
            // The BLE link just dropped. The "auth response authentication failed" →
            // pairing_lost_ heuristic in make_result_cb_ (now fed only by the signed health
            // probe) requires TWO such replies in a row,
            // on the premise that a genuinely de-whitelisted key keeps failing on a healthy,
            // continuously-connected link. A lossy/recovering link, by contrast, emits the
            // same message as transient corruption and then drops — so two failures that
            // straddle a disconnect are NOT evidence of a deleted key. Reset the streak here
            // so a reconnect starts clean and a flaky link can't be mistaken for a revocation
            // (which would clear the session and wrongly prompt "approve on the touchscreen"
            // on an already-paired car). The definitive signals — a "whitelist" message and
            // the ERROR_UNKNOWN_KEY_ID/INACTIVE_KEY/INVALID_KEY_HANDLE faults — are immediate
            // and unaffected, so a real key deletion is still caught.
            auth_fail_streak_.store(0);
        }

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
        // on_rx_data parses Tesla's length-prefixed frames out of these bytes synchronously.
        // A weak/lossy BLE link desyncs the framing ("Invalid message length …") and some
        // corrupt inputs make the parser throw (out_of_range / bad_alloc). This callback runs
        // in NimBLE's host task, so an escaping throw unwinds through C dispatch frames →
        // std::terminate → abort() → reboot. Catch it at this nearest C++ boundary and flag a
        // link reset (handled in loop_task). The give still runs (catch never rethrows), so
        // the mutex can't be left locked.
        try {
            vehicle_->on_rx_data(data);
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "on_rx_data threw (%s) — corrupt BLE RX; resetting link", e.what());
            ble_fault_.store(true);
        } catch (...) {
            ESP_LOGE(TAG, "on_rx_data threw (unknown) — corrupt BLE RX; resetting link");
            ble_fault_.store(true);
        }
        xSemaphoreGive(vehicle_mutex_);
    });

    // Persistent charge-state callback: refreshes the cache on *every* ChargeState
    // (the background refresh in loop_task). Installed once, never cleared; HTTP
    // reads serve last_known_charge_ from this cache without blocking.
    vehicle_->set_charge_state_callback([this](const CarServer_ChargeState& cs) {
        MutexGuard g(cache_mutex_);
        parse_charge_state(cs, last_known_charge_);
        note_contact_();
    });

    // Read-only telemetry callbacks. Fed by the rotating background poll in loop_task_fn_
    // (one telemetry domain per cycle). Each refreshes its own cache for the web UI; none
    // affect pairing or evcc. Installed once, never cleared.
    vehicle_->set_climate_state_callback([this](const CarServer_ClimateState& cs) {
        MutexGuard g(cache_mutex_);
        parse_climate_state(cs, last_known_climate_);
        note_contact_();
    });
    vehicle_->set_drive_state_callback([this](const CarServer_DriveState& ds) {
        MutexGuard g(cache_mutex_);
        parse_drive_state(ds, last_known_drive_);
        note_contact_();
    });
    vehicle_->set_tire_pressure_state_callback([this](const CarServer_TirePressureState& t) {
        MutexGuard g(cache_mutex_);
        parse_tire_pressure(t, last_known_tires_);
        note_contact_();
    });
    vehicle_->set_closures_state_callback([this](const CarServer_ClosuresState& c) {
        MutexGuard g(cache_mutex_);
        parse_closures_state(c, last_known_closures_);
        note_contact_();
    });

    // Reliable key-revocation detector. When the key is deleted on the car side, the
    // VCSEC health poll keeps succeeding from its cached session (the whitelist is not
    // re-checked per command), so it can miss the deletion entirely. But the car rejects
    // every signed command on the *infotainment* domain immediately with a signed-message
    // fault naming the key (ERROR_UNKNOWN_KEY_ID) — the background charge poll triggers
    // exactly that. Observe every incoming message and, while we believe we're paired,
    // treat such a fault as a lost pairing. Runs in the BLE RX task; only cheap atomic
    // ops here. Gated on believed_paired_ so enrolment-time rejections are ignored.
    vehicle_->set_message_callback([this](const UniversalMessage_RoutableMessage& msg) {
        if (!believed_paired_ || !msg.has_signedMessageStatus) return;
        switch (msg.signedMessageStatus.signed_message_fault) {
            case UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_UNKNOWN_KEY_ID:
            case UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INACTIVE_KEY:
            case UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INVALID_KEY_HANDLE:
                if (!pairing_lost_.exchange(true)) {
                    ESP_LOGW(TAG, "auto-pair: car rejected our key (fault %d) — key deleted on the car side, pairing lost",
                             (int)msg.signedMessageStatus.signed_message_fault);
                }
                break;
            default:
                break;
        }
    });

    // Seed the active window open at boot so evcc gets a warm cache for the first few
    // minutes after start; it then backs off if the car stays idle (no command, not charging).
    last_cmd_ticks_.store(xTaskGetTickCount());

    xTaskCreate(loop_task_fn_, "vehicle_loop", 8192, this, 5, &loop_task_);
    xTaskCreate(auto_pair_task_fn_, "auto_pair", 8192, this, 4, &auto_pair_task_);
    ESP_LOGI(TAG, "VehicleController ready for VIN %s", vin.c_str());
    return true;
}

// A plausible Tesla VIN: exactly 17 chars, uppercase alphanumeric excluding I/O/Q (reserved by
// the VIN standard). Mirrors index.html's client-side check and /set_vin's server validation.
// Used to gate pairing so the device never connects/enrols without a real VIN — the boot
// placeholder "UNKNOWN" (7 chars) is not plausible, so it can never reach the matching path.
bool VehicleController::vin_is_plausible(const std::string& vin) {
    if (vin.size() != 17) return false;
    for (char c : vin) {
        bool ok = (c >= '0' && c <= '9') ||
                  ((c >= 'A' && c <= 'Z') && c != 'I' && c != 'O' && c != 'Q');
        if (!ok) return false;
    }
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
//   2. Send the whitelist-add. The car whitelists the key when the user confirms on
//      screen but sends NO completing commandStatus, so this command never finishes
//      cleanly — it just exhausts ~180 s of library retries while sitting at the head
//      of the single FIFO queue, starving everything behind it. So after pair() we
//      drop the link to flush that stuck command from the queue.
//   3. Probe once more on a clean link — now authorised, this establishes the session.
// On any failure the BLE link is dropped, which flushes the library's command
// queue and RX buffer (set_connected(false)) so the next round starts clean.
void VehicleController::auto_pair_task_fn_(void* arg) {
    auto* self = static_cast<VehicleController*>(arg);
    vTaskDelay(pdMS_TO_TICKS(4000));  // let WiFi/BLE come up first
    bool warned_no_vin = false;
    while (true) {
        // No vehicle to target: without a plausible 17-char VIN we must not connect or enrol —
        // that risks whitelisting our key onto an arbitrary nearby Tesla. Idle quietly instead
        // of spinning a connect→10 s-timeout loop; /scan still lists nearby cars. Logged once.
        // Re-checked each cycle so enrolment starts automatically once a VIN is saved (the web
        // UI's POST /set_vin reboots into a configured state, but this stays robust regardless).
        if (!self->has_plausible_vin()) {
            if (!warned_no_vin) {
                ESP_LOGW(TAG, "auto-pair: no VIN configured — pairing disabled. Set a VIN via the "
                              "setup AP or POST /set_vin, then enrolment starts automatically.");
                warned_no_vin = true;
            }
            // Keep a fresh, LISTING-ONLY view of nearby Teslas for the web UI (nearby() sorts
            // by RSSI). start_discovery never connects/enrols — want_connect_ stays false — so
            // this only populates /status ble.devices and can't whitelist our key onto an
            // arbitrary car. Re-armed each cycle once the ~12 s scan window lapses.
            if (self->ble_ && !self->ble_->is_scanning()) self->ble_scan(12000);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        warned_no_vin = false;  // a VIN is present again — re-arm the one-shot log

        // The car deleted our key (detected via a KEY_NOT_ON_WHITELIST response to any
        // signed command or the health poll below). The stored key is now useless, so
        // re-key — which also clears the session + cache — and fall through to re-pair.
        if (self->pairing_lost_) {
            self->believed_paired_ = false;  // stop the observer acting during re-enrol
            ESP_LOGW(TAG, "auto-pair: KEY DELETED on the car — clearing pairing, generating a new key, restarting enrolment");
            self->repair_notice_ = true;  // tell the UI why it's asking to pair again
            self->generate_key();   // clears pairing_lost_, session and cached data
            ESP_LOGI(TAG, "auto-pair: new key generated (%s) — re-enrol it on the car", self->key_fingerprint().c_str());
            continue;
        }

        if (self->has_session()) {
            // A live session means (re-)pairing succeeded; drop the re-auth notice.
            self->repair_notice_ = false;
            // We're paired: arm the message observer so a key-rejection fault on any
            // signed command (e.g. the background charge poll → ERROR_UNKNOWN_KEY_ID)
            // trips pairing_lost_ even while the cached VCSEC session keeps succeeding.
            self->believed_paired_ = true;
            // Paired — periodically run a signed VCSEC health poll (~30 s) so a key deleted
            // on the car side is noticed even with no evcc traffic. The poll hits the always-
            // on body controller (VCSEC), which does NOT wake the car's main computer (wake
            // sequences are infotainment-only), so it never keeps a parked car awake. Three
            // outcomes, distinguished so /diag clearly says what happened:
            //   • success            → key still valid
            //   • auth rejection     → car refused our key (likely deleted) — confirm now
            //   • neither (no reply) → car unreachable (asleep / out of range / weak link)
            int  streak_before = self->auth_fail_streak_;
            bool ok            = self->health_probe_();
            if (self->pairing_lost_) continue;  // 2nd strike already → revoked (top of loop)

            if (ok) {
                ESP_LOGD(TAG, "auto-pair: health check OK — key still valid");
                // Idle ~30 s, but bail out fast if the message observer flags a deletion
                // (a faulting charge poll mid-wait) so we re-key promptly, not 30 s later.
                for (int w = 0; w < 60 && !self->pairing_lost_; w++) vTaskDelay(pdMS_TO_TICKS(500));
            } else if (self->auth_fail_streak_ > streak_before) {
                // The car answered but REFUSED our key → almost certainly deleted on the
                // car side. Confirm immediately (don't wait a whole cycle) so we react in
                // ~1-2 s; a second auth rejection trips pairing_lost_ in make_result_cb_.
                ESP_LOGW(TAG, "auto-pair: car refused our key (auth fail %d/2) — re-checking to confirm…",
                         (int)self->auth_fail_streak_);
                self->health_probe_();
                if (self->pairing_lost_) continue;
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                // No auth answer at all → could not reach/talk to the car. NOT a revocation;
                // keep the pairing and retry. Logged so it's clear the key simply could not
                // be verified (connectivity), rather than a deletion being silently missed.
                ESP_LOGW(TAG, "auto-pair: car not reachable over BLE — can't verify key right now, will retry");
                for (int w = 0; w < 60 && !self->pairing_lost_; w++) vTaskDelay(pdMS_TO_TICKS(500));
            }
            continue;
        }

        // Not paired (enrolling): disarm the observer — key-rejection faults are expected
        // here and must not be mistaken for a revocation.
        self->believed_paired_ = false;

        // 1. Probe for an existing whitelist entry. Once the key is enrolled — by resting a
        //    Tesla NFC keycard on the center-console reader and confirming the "Add key"
        //    dialog the car then shows on its touchscreen (the dialog only appears while a
        //    card is present) — it shows up here as a usable session.
        VehicleStatusResult st;
        self->get_vehicle_status(st, 6000);
        if (self->has_session()) {
            ESP_LOGI(TAG, "auto-pair: session established");
            continue;
        }

        // 2. Send the whitelist-add ONCE, then flush the queue. This is what makes the car
        //    show the "Add key" dialog on its touchscreen — but the car only shows it while
        //    a Tesla NFC keycard is resting on the center-console reader. We do NOT block
        //    waiting on it: the "Whitelist Add Key" never completes cleanly on this car (no
        //    completing commandStatus) — success is detected by probing (step 3), not by
        //    pair()'s return. The short wait just lets the message reach the car; flushing
        //    (set_connected(false)) then clears the lingering whitelist-add from the single
        //    FIFO queue so the probes below run clean. Sending it only once per round
        //    (instead of every ~45 s block) also stops the car re-prompting after the key
        //    is already registered.
        ESP_LOGI(TAG, "auto-pair: not paired — requesting key enrolment from the car…");
        self->pair(5000);
        if (self->ble_ && self->ble_->is_connected()) self->ble_->disconnect();
        xSemaphoreTake(self->vehicle_mutex_, portMAX_DELAY);
        self->vehicle_->set_connected(false);
        xSemaphoreGive(self->vehicle_mutex_);
        ESP_LOGI(TAG, "auto-pair: enrolment request sent — place a Tesla NFC keycard on the center-console reader, then confirm 'Add key' on the touchscreen; waiting for the key to register…");

        // 3. Poll for the resulting session at a short cadence so an enrolment that lands
        //    mid-round — the instant a keycard is tapped — is noticed within a few seconds
        //    instead of after a full slow round. A failed probe (not yet enrolled) returns
        //    on its timeout; a successful one (now enrolled) returns in ~1 s and persists
        //    the session, so has_session() flips and we stop here.
        bool established = false;
        for (int i = 0; i < 8; i++) {
            self->get_vehicle_status(st, 3000);
            if (self->has_session()) { established = true; break; }
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        if (established) {
            ESP_LOGI(TAG, "auto-pair: key registered on the car — session established, now PAIRED");
            continue;
        }
        ESP_LOGI(TAG, "auto-pair: not registered yet — place a Tesla NFC keycard on the console reader and confirm 'Add key' on screen (or move closer if the car is out of BLE range)");
    }
}

void VehicleController::loop_task_fn_(void* arg) {
    auto* self = static_cast<VehicleController*>(arg);
    uint32_t last_poll_ticks    = 0;
    uint32_t last_connect_ticks = 0;
    uint32_t last_tele_ticks    = 0;
    int      tele_idx           = 0;  // rotates the telemetry domain polled each cycle
    bool     prev_window        = false;  // edge-detect the active window
    auto     prev_sleep         = TeslaBLE::SleepState::UNKNOWN;  // edge-detect VCSEC sleep flag
    while (true) {
        xSemaphoreTake(self->vehicle_mutex_, portMAX_DELAY);
        // loop() pumps the command/wake state machine and processes inbound messages, so it
        // can throw on corrupt RX the same way on_rx_data does. Same containment: catch here,
        // flag a link reset below.
        try {
            self->vehicle_->loop();
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "vehicle loop() threw (%s) — resetting BLE link", e.what());
            self->ble_fault_.store(true);
        } catch (...) {
            ESP_LOGE(TAG, "vehicle loop() threw (unknown) — resetting BLE link");
            self->ble_fault_.store(true);
        }
        xSemaphoreGive(self->vehicle_mutex_);

        // A parse fault flagged from loop() or the BLE rx callback: drop the link once,
        // outside the vehicle mutex. Disconnect drives set_connected(false), which clears the
        // library's rx_buffer and resets sessions so the next connect re-syncs cleanly —
        // turning a would-be abort()/reboot into a brief reconnect.
        if (self->ble_fault_.exchange(false)) {
            ESP_LOGW(TAG, "BLE parse fault — dropping link to clear corrupt RX state");
            if (self->ble_connected()) self->ble_->disconnect();
        }

        // Heap watch: log free heap + LARGEST contiguous free block every ~30 s. The largest
        // block (not total free) is what bounds big allocations; it can fall to a few tens of KB
        // under BLE rx-buffer churn, so a large contiguous alloc would throw std::bad_alloc →
        // uncaught → abort(). This keeps the trend visible. (/diag itself streams and no longer
        // allocates the whole log; the HTTP handler guard catches anything else.)
        static uint32_t last_heap_log = 0;
        uint32_t hb_now = xTaskGetTickCount();
        if (hb_now - last_heap_log > pdMS_TO_TICKS(30000)) {
            last_heap_log = hb_now;
            ESP_LOGW(TAG, "HEAP free=%u largest_block=%u min_free=%u",
                     (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
        }

        // While NOT paired we stay completely out of the way: the auto-pair task owns the
        // connection and the single command queue, and a stray charge poll injected into
        // that queue mid-handshake corrupts the pairing exchange (overlapping responses →
        // RX reassembly errors → pairing never completes).
        bool paired = self->has_session();

        // ── VCSEC sleep-flag sampler (feeds link_state()'s asleep debounce) ───────────────
        // The library updates Vehicle::sleep_state() from the car's vehicleSleepStatus on
        // every VCSEC poll, including auto_pair_task's idle health probe — the only BLE
        // traffic while parked. Sample it here (cheap word-sized read; the RX task writes it,
        // a benign race) and fold it into the debounce clock so link_state() can require a
        // STABLE ASLEEP run before showing "Vehicle asleep". UNKNOWN leaves the clock alone.
        // Log only on a transition so the serial console reveals what the car actually reports
        // (e.g. whether VCSEC ever asserts ASLEEP, or just flaps for COP) without spamming.
        if (paired) {
            TeslaBLE::SleepState st = self->vehicle_->sleep_state();
            if (st != prev_sleep) {
                ESP_LOGI(TAG, "VCSEC sleep flag: %s",
                         st == TeslaBLE::SleepState::ASLEEP ? "ASLEEP"
                       : st == TeslaBLE::SleepState::AWAKE  ? "AWAKE" : "UNKNOWN");
                prev_sleep = st;
            }
            if (st == TeslaBLE::SleepState::ASLEEP)     self->note_vcsec_sleep_(true);
            else if (st == TeslaBLE::SleepState::AWAKE)  self->note_vcsec_sleep_(false);
            // UNKNOWN: leave the run untouched.
        }

        // ── Active-window gate ──────────────────────────────────────────────────────────
        // The three background blocks below open an INFOTAINMENT session, which keeps the
        // car's main computer awake. Run them ONLY while the car has a reason to be awake,
        // so a parked, idle car can actually reach sleep (no vampire drain). The window is
        // open when EITHER holds:
        //   • an evcc/manual command in the last kActiveWindowMs (last_cmd_ticks_), OR
        //   • the car is charging (cached charging_state — a charging car is awake anyway).
        // We deliberately do NOT open the window merely because the car is observed awake:
        // that is self-perpetuating — our own infotainment polling keeps the MCU awake, which
        // would re-open the window, so the car could never finish its idle→sleep transition.
        // evcc starts charging via a command (→ window opens → charging_state then holds it
        // open for the session), so signals 1+2 cover the cases evcc cares about. When the
        // window closes we stop polling and drop the link once so the MCU idles into sleep.
        // The auto-pair VCSEC health poll keeps running (it never wakes the MCU) as the
        // revocation canary, and evcc reads stay served from cache (stale by design).
        uint32_t now_ticks = xTaskGetTickCount();
        bool charging;
        {
            ChargeStateResult cs = self->copy_locked_(self->last_known_charge_);
            charging = cs.valid && (cs.charging_state == "Charging" ||
                                    cs.charging_state == "Starting");
        }
        uint32_t lc = self->last_cmd_ticks_.load();
        bool recent_cmd = (lc != 0) && ((now_ticks - lc) < pdMS_TO_TICKS(kActiveWindowMs));
        bool window = recent_cmd || charging;

        // Falling edge: window just closed → drop the link once so the car can sleep.
        if (paired && prev_window && !window && self->ble_connected()) {
            ESP_LOGI(TAG, "idle: no command and not charging — dropping BLE link so the car can sleep");
            self->ble_->disconnect();
        }
        // Rising edge: window just opened → refresh the cache promptly (reset throttles).
        if (paired && !prev_window && window) {
            last_poll_ticks = last_tele_ticks = last_connect_ticks = 0;
        }
        prev_window = window;

        // Warm-up connect (paired + window): non-blocking, idempotent, throttled.
        if (paired && window && !self->ble_connected()
            && (now_ticks - last_connect_ticks > pdMS_TO_TICKS(15000))) {
            last_connect_ticks = now_ticks;
            self->ble_->connect("");
        }

        // Background charge-state refresh (paired + window + connected), every 10 s. This
        // infotainment poll doubles as the reliable key-revocation canary: a deleted key
        // makes it fault with ERROR_UNKNOWN_KEY_ID, which the message observer turns into
        // pairing_lost_. Gated on the active window so an idle car is left to sleep; the
        // VCSEC health poll still catches a deletion while idle.
        if (paired && window && self->ble_connected() && !self->cmd_in_flight_.load()
            && (now_ticks - last_poll_ticks > pdMS_TO_TICKS(10000))) {
            last_poll_ticks = now_ticks;
            ESP_LOGD(TAG, "background charge-state refresh…");
            // Fire-and-forget poll. We must NOT block here: this task also pumps
            // vehicle_->loop(), which drives the command's transmission/retries. The
            // persistent charge-state callback updates last_known_charge_ when the
            // response arrives. NO_WAKE_SKIP so a sleeping car is left undisturbed.
            xSemaphoreTake(self->vehicle_mutex_, portMAX_DELAY);
            self->vehicle_->charge_state_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP);
            xSemaphoreGive(self->vehicle_mutex_);
        }

        // Background telemetry refresh (paired + window + connected): one domain per cycle,
        // rotating climate → drive → tires → closures so the full set refreshes every ~120 s
        // without flooding the single FIFO command queue. These feed only the web UI / MQTT
        // (slow-changing: cabin temp, tyre pressure, odometer), so a relaxed 30 s cadence
        // costs nothing visible while cutting how often the BLE radio is active — each poll
        // on a weak link can desync into a multi-second retry burst that, via WiFi/BT radio
        // coexistence, steals airtime from the HTTP server. The evcc-critical charge poll
        // above stays at 10 s. All NO_WAKE_SKIP; web-UI caches only; evcc and pairing are
        // unaffected.
        if (paired && window && self->ble_connected() && !self->cmd_in_flight_.load()
            && (now_ticks - last_tele_ticks > pdMS_TO_TICKS(30000))) {
            last_tele_ticks = now_ticks;
            xSemaphoreTake(self->vehicle_mutex_, portMAX_DELAY);
            switch (tele_idx % 4) {
                case 0: self->vehicle_->climate_state_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP);  break;
                case 1: self->vehicle_->drive_state_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP);    break;
                case 2: self->vehicle_->tire_pressure_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP);  break;
                case 3: self->vehicle_->closures_state_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP); break;
            }
            xSemaphoreGive(self->vehicle_mutex_);
            tele_idx++;
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
    MutexGuard cmd_guard(command_mutex_);
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
    MutexGuard cmd_guard(command_mutex_);
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
    // onboard charger accepts, deliberately ABOVE the docs' conservative "0–32" typical range
    // so a legitimate high-current request (e.g. a 48 A-capable Model 3/Y) is never capped.
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

// ─── Data queries ─────────────────────────────────────────────────────────────

bool VehicleController::get_charge_state(ChargeStateResult& out, int /*timeout_ms*/) {
    // Serve the cached reading instantly and never block. evcc polls vehicle_data
    // frequently and times out quickly, so an on-demand connect + poll here would risk a
    // gateway timeout (HTTP 502). Freshness is maintained out of band by loop_task, but ONLY
    // while the active window is open (a recent command OR the car charging) — see
    // loop_task_fn_. We deliberately do NOT open the window merely because the car is observed
    // awake: that is self-perpetuating (our polling would keep the MCU awake), so a parked,
    // idle car is left to sleep and this serves the last value (which heals within seconds of
    // any evcc command). If we have never gotten a reading yet, the caller emits a zeroed
    // charge_state.
    MutexGuard g(cache_mutex_);
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
    if (ok && out.valid) {
        note_reachable_();  // car answered a VCSEC status read ⇒ reachable over BLE right now
        cmd_fail_streak_.store(0);  // a clean round-trip ⇒ link healthy, reset desync backstop
        MutexGuard cache_guard(cache_mutex_);
        last_known_status_ = out;
    }
    return ok && out.valid;
}

// ─── Key management ───────────────────────────────────────────────────────────

bool VehicleController::generate_key() {
    MutexGuard cmd_guard(command_mutex_);
    xSemaphoreTake(vehicle_mutex_, portMAX_DELAY);
    vehicle_->regenerate_key();
    xSemaphoreGive(vehicle_mutex_);
    // Record when the key was generated so the UI can show the key's creation
    // date next to its fingerprint. Wall-clock comes from the browser (POST
    // /set_time) or the NVS-cached time; if neither is set yet this stamps a
    // near-zero value, which the UI ignores.
    if (storage_) {
        time_t now = time(nullptr);
        storage_->save_str("key_created", std::to_string((long long)now));
    }
    // A new key invalidates any existing pairing: the stored session belonged to the
    // previous key/whitelist entry, so a fresh enrolment + handshake is required.
    // Wipe the session and cached data so has_session() flips to false (the UI shows
    // "not paired" and hides the controls/SOC) and the auto-pair loop re-enrolls.
    clear_session_and_cache_();
    pairing_lost_     = false;  // re-keying is the resolution; clear any pending flag
    auth_fail_streak_ = 0;      // and the streak that may have led here
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
        storage_->remove("paired_at");   // re-pair re-stamps the pairing date
    }

    // Drop cached readings so /status and vehicle_data never serve old SOC/charge data
    // (or stale telemetry) from a defunct pairing. Under cache_mutex_ since the HTTP task
    // may be copying these concurrently.
    {
        MutexGuard cache_guard(cache_mutex_);
        last_known_charge_   = {};
        last_known_status_   = {};
        last_known_climate_  = {};
        last_known_drive_    = {};
        last_known_tires_    = {};
        last_known_closures_ = {};
    }
    last_contact_ticks_.store(0);    // no live data anymore → "asleep" card has nothing to show
    last_reachable_ticks_.store(0);  // and no proven reachability → link_state() back to Unknown
    vcsec_asleep_since_ticks_.store(0);  // forget any debounced sleep run from the old pairing
    ESP_LOGI(TAG, "pairing/session cleared");
}

// Derived connectivity state — see the enum doc in vehicle_ctrl.hpp. Centralised here so
// the web UI (/status) and the MQTT/HA bridge consume one consistent answer.
//   kAwakeMaxAgeS     mirrors the old per-file thresholds (charge polls refresh contact
//                     every ~10 s while the window is open, so 60 s won't flap).
//   kReachableMaxAgeS must span TWO full idle health-probe cycles incl. one missed probe so a
//                     transient miss never flaps a sleeping-NEARBY car to Unreachable (which
//                     would wrongly hide the web-UI hero / publish a phantom "UNREACHABLE").
//                     The idle reachability stamp comes only from auto_pair_task's health
//                     probe, whose cycle is its 30 s post-probe wait + a VCSEC scan/connect
//                     (≤10 s, ensure_connected_) + round-trip (≤8 s, health_probe_) ≈ 40-48 s;
//                     a failed probe on the flaky link to a sleeping car adds another ~30 s
//                     wait + timeout. 150 s clears two such cycles with margin while a
//                     genuinely-gone car still flips to Unreachable in ~2.5 min.
//   kAsleepDebounceS  must outlast the COP-driven VCSEC AWAKE↔ASLEEP flap (~60 s observed) so
//                     a momentary ASLEEP blip can't flip the UI to "Vehicle asleep"; 120 s
//                     needs the flag to stay ASLEEP across at least two idle health probes.
VehicleController::LinkState VehicleController::link_state() const {
    static constexpr uint32_t kAwakeMaxAgeS     = 60;
    static constexpr uint32_t kReachableMaxAgeS = 150;
    static constexpr uint32_t kAsleepDebounceS  = 120;  // VCSEC must hold ASLEEP this long
    uint32_t age = 0;
    if (seconds_since_contact(age) && age < kAwakeMaxAgeS) return LinkState::Awake;
    if (seconds_since_reachable(age) && age < kReachableMaxAgeS) {
        // Reachable over VCSEC but no fresh infotainment data. Do NOT assert "asleep" the
        // instant we stop polling — that mislabelled an awake-but-idle car as sleeping (the
        // very bug this fixes: the car answers the VCSEC health poll the whole time, so the
        // old `reachable && !awake ⇒ Asleep` flipped to "Vehicle asleep" ~60 s after the
        // last command even while the car was wide awake). Require positive, debounced proof
        // instead: the car's own VCSEC sleep flag must have held ASLEEP for kAsleepDebounceS
        // (so a Cabin-Overheat-Protection AWAKE↔ASLEEP flap, ~60 s, never flips the UI).
        // Without that proof we honestly do not know ⇒ Idle (neutral standby, never "asleep").
        if (vcsec_stably_asleep_(kAsleepDebounceS)) return LinkState::Asleep;
        return LinkState::Idle;
    }
    // Heard something at some point but it's now stale ⇒ unreachable; never heard ⇒ unknown.
    if (last_reachable_ticks_.load() != 0 || last_contact_ticks_.load() != 0)
        return LinkState::Unreachable;
    return LinkState::Unknown;
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
    // A signed VCSEC GET_STATUS — the one signed command a Charging-Manager key is ALWAYS
    // authorised for, so its outcome unambiguously reflects whitelist state: success ⇒ key
    // still valid; KEY_NOT_ON_WHITELIST or a tagless session-info ("authentication failed")
    // ⇒ key deleted. Because role refusal cannot masquerade as revocation here (there is no
    // role that can't read status), this is the ONE caller that passes auth_fail_is_revocation
    // so make_result_cb_ lets an "authentication failed" feed the two-strike pairing_lost_.
    return send_vcsec_("VCSEC Health Poll", [](TeslaBLE::Client* c, uint8_t* b, size_t* l) {
        return c->build_vcsec_information_request_message(
            VCSEC_InformationRequestType_INFORMATION_REQUEST_TYPE_GET_STATUS, b, l);
    }, TeslaBLE::WakePolicy::WAKE_IF_NEEDED, timeout_ms, /*count_as_activity=*/false,
       /*auth_fail_is_revocation=*/true);
}

time_t VehicleController::key_created_at() {
    if (!storage_) return 0;
    std::string s;
    if (!storage_->load_str("key_created", s)) return 0;
    return (time_t)atoll(s.c_str());
}

time_t VehicleController::paired_at() {
    if (!storage_ || !has_session()) return 0;
    std::string s;
    if (storage_->load_str("paired_at", s)) {
        time_t t = (time_t)atoll(s.c_str());
        if (t > 1600000000) return t;
    }
    // First time we observe a session with a valid wall clock: stamp it now. For a
    // fresh handshake this is within seconds of pairing; a pairing that predates this
    // tracking (or whose clock was unsynced) gets stamped at first sync instead.
    time_t now = time(nullptr);
    if (now > 1600000000) {
        storage_->save_str("paired_at", std::to_string((long long)now));
        return now;
    }
    return 0;
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
    last_error_.clear();

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
