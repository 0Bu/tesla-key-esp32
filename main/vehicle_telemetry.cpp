// Telemetry caches: the protobuf→struct parsers, the persistent cache callbacks
// (install_state_callbacks_), the background poll / sleep-gating loop (loop_task_fn_)
// and the data queries serving cached readings (get_charge_state, get_vehicle_status).
// Part of the VehicleController implementation split — see vehicle_ctrl_internal.hpp.

#include "vehicle_ctrl.hpp"
#include "runtime_admission.hpp"
#include "vehicle_ctrl_internal.hpp"
#include "logic/units.hpp"
#include "logic/active_window.hpp"
#include "logic/wake_poll.hpp"
#include "logic/heap_watchdog.hpp"
#include "ota_update.hpp"
#include "heap_trend.hpp"
#include "stack_watch.hpp"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <exception>
#include <type_traits>
#include <utility>
#include <vector>

// protobuf generated headers (from tesla-ble)
#include <vcsec.pb.h>
#include <car_server.pb.h>

static const char* TAG = "vehicle_ctrl";

namespace {

// A task that exits through the C++ exception boundary must unregister itself before FreeRTOS
// self-deletion. The destructor runs during that unwind; no external task ever owns this handle.
class TaskWatchdogSubscription {
public:
    bool subscribe() noexcept {
        active_ = esp_task_wdt_add(nullptr) == ESP_OK;
        return active_;
    }
    bool active() const noexcept { return active_; }
    ~TaskWatchdogSubscription() {
        if (active_ && esp_task_wdt_delete(nullptr) != ESP_OK) {
            ESP_LOGE(TAG, "vehicle loop could not unregister from the task watchdog");
        }
    }

private:
    bool active_{false};
};

}  // namespace

namespace {
// Translate a nanopb CarServer_ChargeState into our flat result struct. Each scalar is a
// proto3 optional → a single-member oneof in nanopb: present iff which_optional_<f> matches.
void parse_charge_state(const CarServer_ChargeState& cs, ChargeStateResult& out) {
    // A ChargeState response is one snapshot, not a delta. Clear every presence bit
    // before decoding it: otherwise an omitted field inherits the previous response's
    // value and a new generation can falsely "verify" stale charging_amps.
    out = {};
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
    out = {};
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
    if (cs.which_optional_is_climate_on == CarServer_ClimateState_is_climate_on_tag) {
        out.is_climate_on = cs.optional_is_climate_on.is_climate_on; out.has_climate_on = true;
    }
    if (cs.which_optional_is_preconditioning == CarServer_ClimateState_is_preconditioning_tag) {
        out.is_preconditioning = cs.optional_is_preconditioning.is_preconditioning;
        out.has_preconditioning = true;
    }

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
    out = {};
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
        // hundredths of a mile → km (logic/units.hpp, host-tested)
        out.odometer_km = (float)tk::odo_hundredths_mi_to_km(
            ds.optional_odometer_in_hundredths_of_a_mile.odometer_in_hundredths_of_a_mile);
        out.has_odometer = true;
    }
}

void parse_tire_pressure(const CarServer_TirePressureState& t, TirePressureResult& out) {
    out = {};
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
    out = {};
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

// ─── Cache callbacks (installed once from init) ───────────────────────────────

void VehicleController::install_state_callbacks_() {
    // Persistent charge-state callback: refreshes the cache on *every* ChargeState
    // (the background refresh in loop_task). Installed once, never cleared; HTTP
    // reads serve last_known_charge_ from this cache without blocking.
    vehicle_->set_charge_state_callback([this](const CarServer_ChargeState& state) {
        on_charge_state_(state);
    });

    // Read-only telemetry callbacks. Fed by the rotating background poll in loop_task_fn_
    // (one telemetry domain per cycle). Each refreshes its own cache for the web UI; none
    // affect pairing or evcc. Installed once, never cleared.
    vehicle_->set_climate_state_callback([this](const CarServer_ClimateState& state) {
        on_climate_state_(state);
    });
    vehicle_->set_drive_state_callback([this](const CarServer_DriveState& state) {
        on_drive_state_(state);
    });
    vehicle_->set_tire_pressure_state_callback([this](const CarServer_TirePressureState& state) {
        on_tire_pressure_state_(state);
    });
    vehicle_->set_closures_state_callback([this](const CarServer_ClosuresState& state) {
        on_closures_state_(state);
    });
}

static_assert(std::is_trivially_copyable_v<CarServer_ChargeState>);
static_assert(std::is_trivially_copyable_v<CarServer_ClimateState>);
static_assert(std::is_trivially_copyable_v<CarServer_DriveState>);
static_assert(std::is_trivially_copyable_v<CarServer_TirePressureState>);
static_assert(std::is_trivially_copyable_v<CarServer_ClosuresState>);

void VehicleController::on_charge_state_(const CarServer_ChargeState& state) noexcept {
    portENTER_CRITICAL(&telemetry_pending_mux_);
    telemetry_pending_charge_ = state;
    telemetry_pending_mask_ |= PendingCharge;
    uint32_t generation = charging_amps_feedback_.generation + 1;
    charging_amps_feedback_ = {};
    charging_amps_feedback_.generation = generation ? generation : 1;
    if (state.which_optional_charging_amps == CarServer_ChargeState_charging_amps_tag) {
        charging_amps_feedback_.has_charging_amps = true;
        charging_amps_feedback_.charging_amps = state.optional_charging_amps.charging_amps;
    }
    if (state.which_optional_charge_current_request ==
        CarServer_ChargeState_charge_current_request_tag) {
        charging_amps_feedback_.has_current_request = true;
        charging_amps_feedback_.current_request =
            state.optional_charge_current_request.charge_current_request;
    }
    if (state.which_optional_charger_actual_current ==
        CarServer_ChargeState_charger_actual_current_tag) {
        charging_amps_feedback_.has_actual_current = true;
        charging_amps_feedback_.actual_current =
            state.optional_charger_actual_current.charger_actual_current;
    }
    portEXIT_CRITICAL(&telemetry_pending_mux_);
}

VehicleController::ChargingAmpsFeedback VehicleController::charging_amps_feedback_snapshot_() noexcept {
    portENTER_CRITICAL(&telemetry_pending_mux_);
    const ChargingAmpsFeedback snapshot = charging_amps_feedback_;
    portEXIT_CRITICAL(&telemetry_pending_mux_);
    return snapshot;
}

void VehicleController::on_climate_state_(const CarServer_ClimateState& state) noexcept {
    portENTER_CRITICAL(&telemetry_pending_mux_);
    telemetry_pending_climate_ = state;
    telemetry_pending_mask_ |= PendingClimate;
    portEXIT_CRITICAL(&telemetry_pending_mux_);
}

void VehicleController::on_drive_state_(const CarServer_DriveState& state) noexcept {
    portENTER_CRITICAL(&telemetry_pending_mux_);
    telemetry_pending_drive_ = state;
    telemetry_pending_mask_ |= PendingDrive;
    portEXIT_CRITICAL(&telemetry_pending_mux_);
}

void VehicleController::on_tire_pressure_state_(
    const CarServer_TirePressureState& state) noexcept {
    portENTER_CRITICAL(&telemetry_pending_mux_);
    telemetry_pending_tires_ = state;
    telemetry_pending_mask_ |= PendingTires;
    portEXIT_CRITICAL(&telemetry_pending_mux_);
}

void VehicleController::on_closures_state_(const CarServer_ClosuresState& state) noexcept {
    portENTER_CRITICAL(&telemetry_pending_mux_);
    telemetry_pending_closures_ = state;
    telemetry_pending_mask_ |= PendingClosures;
    portEXIT_CRITICAL(&telemetry_pending_mux_);
}

void VehicleController::process_pending_telemetry_() {
    CarServer_ChargeState charge{};
    CarServer_ClimateState climate{};
    CarServer_DriveState drive{};
    CarServer_TirePressureState tires{};
    CarServer_ClosuresState closures{};

    portENTER_CRITICAL(&telemetry_pending_mux_);
    const uint32_t pending = telemetry_pending_mask_;
    telemetry_pending_mask_ = 0;
    if (pending & PendingCharge) charge = telemetry_pending_charge_;
    if (pending & PendingClimate) climate = telemetry_pending_climate_;
    if (pending & PendingDrive) drive = telemetry_pending_drive_;
    if (pending & PendingTires) tires = telemetry_pending_tires_;
    if (pending & PendingClosures) closures = telemetry_pending_closures_;
    portEXIT_CRITICAL(&telemetry_pending_mux_);

    // Parse outside both vehicle_mutex_ and cache_mutex_. Only bounded result publication is
    // serialized; temporary strings are created and destroyed in normal task context.
    if (pending & PendingCharge) {
        ChargeStateResult parsed{};
        parse_charge_state(charge, parsed);
        {
            tk::MutexGuard g(cache_mutex_);
            last_known_charge_ = std::move(parsed);
        }
        const uint32_t now = xTaskGetTickCount();
        last_charge_ticks_.store(now);
        charge_state_generation_.fetch_add(1);
        charge_cache_stale_reported_.store(false);
        note_contact_();
    }
    if (pending & PendingClimate) {
        ClimateStateResult parsed{};
        parse_climate_state(climate, parsed);
        { tk::MutexGuard g(cache_mutex_); last_known_climate_ = std::move(parsed); }
        note_contact_();
    }
    if (pending & PendingDrive) {
        DriveStateResult parsed{};
        parse_drive_state(drive, parsed);
        { tk::MutexGuard g(cache_mutex_); last_known_drive_ = std::move(parsed); }
        note_contact_();
    }
    if (pending & PendingTires) {
        TirePressureResult parsed{};
        parse_tire_pressure(tires, parsed);
        { tk::MutexGuard g(cache_mutex_); last_known_tires_ = std::move(parsed); }
        note_contact_();
    }
    if (pending & PendingClosures) {
        ClosuresStateResult parsed{};
        parse_closures_state(closures, parsed);
        { tk::MutexGuard g(cache_mutex_); last_known_closures_ = std::move(parsed); }
        note_contact_();
    }
}

// ─── Background poll / sleep-gating loop ──────────────────────────────────────

bool VehicleController::apply_ble_link_state_(bool connected) {
    tk::SemGuard g(vehicle_mutex_);
    if (!g) return false;
    try {
        vehicle_->set_connected(connected);
        vcsec_sleep_state_.store(static_cast<int>(
            connected ? vehicle_->sleep_state() : TeslaBLE::SleepState::UNKNOWN));
        return true;
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "deferred set_connected(%d) threw (%s) — dropping link",
                 static_cast<int>(connected), e.what());
    } catch (...) {
        ESP_LOGE(TAG, "deferred set_connected(%d) threw (unknown) — dropping link",
                 static_cast<int>(connected));
    }
    vcsec_sleep_state_.store(static_cast<int>(TeslaBLE::SleepState::UNKNOWN));
    return false;
}

void VehicleController::persist_discovered_mac_after_ready_() {
    if (!persist_discovered_mac_.load() || !config_store_ || !ble_) return;
    // Both the string materialization and NVS commit occur on vehicle_loop and outside every
    // shared mutex. A failed persistence only costs one scan on the next boot.
    const std::string addr = ble_->peer_addr_str();
    bool expected = true;
    if (addr.empty() ||
        !persist_discovered_mac_.compare_exchange_strong(expected, false)) return;
    if (!config_store_->save_str(tk::nvs_contract::kBleMac, addr)) {
        ESP_LOGW(TAG, "could not persist Tesla MAC %s — next boot rescans", addr.c_str());
    } else {
        ESP_LOGI(TAG, "Tesla MAC saved: %s", addr.c_str());
    }
}

void VehicleController::process_ble_host_events_() {
    if (!ble_event_queue_ || !ble_ || !vehicle_) return;

    if (ble_event_overflow_.exchange(false, std::memory_order_acq_rel)) {
        BleHostEvent discarded{};
        while (xQueueReceive(ble_event_queue_, &discarded, 0) == pdTRUE) {}
        // A lost byte or transition makes tesla-ble's framing/state unknowable. Reset the library
        // first, then terminate the physical link. No stale LinkUp may become ready afterward.
        (void)apply_ble_link_state_(false);
        ble_->disconnect();
        ESP_LOGW(TAG, "BLE deferred-event queue overflow — link reset fail-closed");
        return;
    }

    BleHostEvent event{};
    while (xQueueReceive(ble_event_queue_, &event, 0) == pdTRUE) {
        const uint32_t current_generation = ble_->lifecycle_generation();
        if (!tk::ble_deferred_event_may_apply(event.kind, event.generation,
                                               current_generation)) {
            continue;
        }

        if (event.kind == tk::BleDeferredEventKind::LinkDown) {
            if (!apply_ble_link_state_(false)) ble_fault_.store(true);
            continue;
        }

        if (event.kind == tk::BleDeferredEventKind::LinkUp) {
            const bool applied = apply_ble_link_state_(true);
            if (!applied || !ble_->complete_ready(event.conn_handle, event.generation)) {
                // set_connected(true) may have succeeded just before cancellation won the ready
                // CAS. Explicitly unwind the library state rather than waiting for a later event.
                if (applied) (void)apply_ble_link_state_(false);
                ble_->disconnect();
                continue;
            }
            persist_discovered_mac_after_ready_();
            ESP_LOGD(TAG, "BLE command-ready after deferred Vehicle ack (generation %lu)",
                     static_cast<unsigned long>(event.generation));
            continue;
        }

        // Materialize the tesla-ble API vector before taking vehicle_mutex_. If this allocation
        // fails, no shared lock is held and the only safe recovery is a clean reconnect.
        std::vector<uint8_t> data;
        try {
            data.assign(event.data.begin(), event.data.begin() + event.size);
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "deferred BLE RX allocation failed (%s) — dropping link", e.what());
            ble_fault_.store(true);
            break;
        } catch (...) {
            ESP_LOGE(TAG, "deferred BLE RX allocation failed (unknown) — dropping link");
            ble_fault_.store(true);
            break;
        }
        {
            tk::SemGuard g(vehicle_mutex_);
            if (!g) {
                ble_fault_.store(true);
                break;
            }
            try {
                vehicle_->on_rx_data(data);
                vcsec_sleep_state_.store(static_cast<int>(vehicle_->sleep_state()));
            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "deferred on_rx_data threw (%s) — dropping link", e.what());
                ble_fault_.store(true);
            } catch (...) {
                ESP_LOGE(TAG, "deferred on_rx_data threw (unknown) — dropping link");
                ble_fault_.store(true);
            }
        }
        if (ble_fault_.load()) break;
    }
}

void VehicleController::loop_task_fn_(void* arg) {
  try {
    auto* self = static_cast<VehicleController*>(arg);
    if (!self || !self->await_task_start_()) {
        // Cancellation is acknowledged inside await_task_start_(). No watchdog, mutex, BLE object
        // or vehicle state has been touched, so a second-create OOM unwinds without cross-core kill.
        vTaskDelete(nullptr);
        return;
    }

    // Subscribe to the Task Watchdog. The heap watchdog above already covers the wedge caused by
    // memory exhaustion; this covers the one it structurally cannot see — a task blocked forever on
    // a semaphore, the BLE stack or a socket, with the heap looking perfectly healthy the whole
    // time. This task is the right subscriber because it is the one that must keep ticking for the
    // device to be doing anything at all.
    //
    // The budget (CONFIG_ESP_TASK_WDT_TIMEOUT_S=60) is sized against this task's LONGEST legitimate
    // block, which is not its 50 ms cadence but the vehicle mutex: a foreground command holds it
    // for up to 20 s, pair() for up to 30 s. Feeding once per iteration is therefore both necessary
    // and sufficient — a slow-but-progressing command never trips it, a genuinely stuck one does.
    // A failed subscription is logged and the loop runs on unwatched: losing the watchdog is worse
    // than not having it, but far better than refusing to poll the car.
    TaskWatchdogSubscription watchdog;
    if (!watchdog.subscribe()) {
        ESP_LOGW(TAG, "vehicle_loop could not subscribe to the task watchdog — a wedged poll will "
                      "no longer reboot the device automatically");
    }
    uint32_t last_poll_ticks    = 0;
    uint32_t last_connect_ticks = 0;
    uint32_t last_tele_ticks    = 0;
    int      tele_idx           = 0;  // rotates the telemetry domain polled each cycle
    bool     prev_window        = false;  // edge-detect the active window
    auto     prev_sleep         = TeslaBLE::SleepState::UNKNOWN;  // edge-detect VCSEC sleep flag
    tk::WakePollState wake_poll{};         // one-shot charge poll on the VCSEC wake edge (#264)
    bool     wake_poll_pending  = false;   // latched fire request until connected & queue-idle
    while (true) {
      // Feed the task watchdog FIRST and UNCONDITIONALLY, before anything that can block or throw.
      // Gating it on the work below would make a long-but-legitimate command look like a hang; put
      // after the work, a throw would skip it and turn an already-contained OOM into a reboot.
      if (watchdog.active()) esp_task_wdt_reset();
      // Retrospective: FreeRTOS retains the lowest free stack ever observed for this task, so a
      // top-of-loop sample records the deepest path of the previous cycle and no branch can skip it.
      tk::stack_watch_sample(tk::StackWatch::Vehicle);

      // Iteration-boundary containment (issue #204): the poll injections + bookkeeping below
      // call tesla-ble builders and touch std::string caches that can throw std::bad_alloc.
      // An escape would unwind into the FreeRTOS C task trampoline → std::terminate → reboot,
      // and a reboot loop also re-opens the car-poll window (defeating sleep). Contain it, then
      // fall through to the tail delay so we never spin a tight error loop.
      try {
        // NimBLE callbacks only enqueue fixed POD records. Drain them here, in a normal task with
        // the task-level exception boundary, before pumping tesla-ble's state machine.
        self->process_ble_host_events_();
        if (self->ble_fault_.exchange(false)) {
            ESP_LOGW(TAG, "BLE deferred processing fault — dropping link to reset library state");
            if (self->ble_connected()) self->ble_->disconnect();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        {
            // RAII give — vehicle_->loop() can throw on corrupt RX the same way on_rx_data does;
            // the guard releases vehicle_mutex_ on unwind so it can't wedge every later command.
            tk::SemGuard g(self->vehicle_mutex_);
            try {
                if (self->command_identity_ready_()) self->vehicle_->loop();
                // on_rx_data() only queues decoded routable messages; loop() is what invokes
                // handle_vcsec_message_ and mutates Vehicle::sleep_state(). Publish the mirror
                // after that mutation while the same lock is still held, otherwise the atomic
                // remains one processing cycle behind (or UNKNOWN forever on quiet links).
                self->vcsec_sleep_state_.store(static_cast<int>(
                    self->command_identity_ready_() ? self->vehicle_->sleep_state()
                                                    : TeslaBLE::SleepState::UNKNOWN));
            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "vehicle loop() threw (%s) — resetting BLE link", e.what());
                self->vcsec_sleep_state_.store(
                    static_cast<int>(TeslaBLE::SleepState::UNKNOWN));
                self->ble_fault_.store(true);
            } catch (...) {
                ESP_LOGE(TAG, "vehicle loop() threw (unknown) — resetting BLE link");
                self->vcsec_sleep_state_.store(
                    static_cast<int>(TeslaBLE::SleepState::UNKNOWN));
                self->ble_fault_.store(true);
            }
        }

        // tesla-ble state callbacks above only copied nanopb POD into the latest-value mailbox.
        // The Vehicle lock is now released, so parsers may materialize strings and publish caches
        // without creating a nested vehicle_mutex_ -> cache_mutex_ / heap dependency.
        self->process_pending_telemetry_();

        // A parse fault flagged from loop(): drop the link once,
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
            // INTERNAL, not plain 8BIT: heap_caps_* reports the max across every heap with the
            // cap, and a board with PSRAM registers it into 8BIT. Deciding on that number
            // would make the watchdog below a silent no-op on exactly the board that has PSRAM.
            // Logged alongside the historical 8BIT figures (identical on the four PSRAM-less
            // targets) so the trend stays comparable with older captures.
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
            ESP_LOGI(TAG, "HEAP free=%u largest_block=%u min_free=%u internal_largest=%u",
                     (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                     (unsigned) largest);

            // Feed the 24-hour memory trend (GET /heap) from the SAME two samples the watchdog
            // below judges — so the chart a human reads and the threshold the firmware acts on can
            // never tell different stories. INTERNAL on both, for the PSRAM reason above: a trend
            // drawn from plain 8BIT would show any PSRAM and hide the only heap that matters.
            // Fixed .bss ring, no allocation — a diagnostic must not compete for the contiguous
            // block it exists to measure.
            tk::heap_trend_record((uint32_t) pdTICKS_TO_MS(hb_now) / 1000,
                                  (uint32_t) heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                                  (uint32_t) largest);

            // Last-resort escalation. Every OOM guard in this firmware turns OOM into "recover
            // and continue", which is right for a transient and left the device WEDGED for ten
            // hours on 2026-07-18 when the shortage was permanent. Decision logic (threshold,
            // hold time, the OTA excusal, tick-wrap safety) is the host-tested
            // logic/heap_watchdog.hpp; this site only samples and acts.
            //
            // Nothing here may allocate: we are deciding precisely because allocation is failing.
            // ota_is_busy() reads one atomic (ota_get_status() would copy std::strings and could
            // throw), and the persist below hands NVS a short literal.
            //
            // Every transition is logged, not just the restart. Syslog is the ONLY post-mortem
            // source that outlives the reboot (the /diag ring is RAM), so someone reading it must
            // be able to answer "why did this device restart?" without any other evidence: the
            // arming line states the trigger, the countdown lines prove the shortage was sustained
            // rather than a spike, and a recovery line closes a run that did not fire.
            static tk::HeapWatchdog heap_wd;
            tk::HeapVerdict v = tk::heap_watch(
                heap_wd, {largest, (uint32_t) pdTICKS_TO_MS(hb_now), ota_is_busy()});
            const unsigned held_s    = (unsigned) (v.critical_ms / 1000);
            const unsigned left_s    = (unsigned) (tk::heap_restart_in_ms(v.critical_ms) / 1000);
            const unsigned threshold = (unsigned) tk::kHeapCriticalBytes;

            if (v.action == tk::HeapAction::Armed) {
                ESP_LOGE(TAG, "HEAP CRITICAL: internal largest_block %u B < %u B — watchdog ARMED, "
                              "restarting in %u s unless it recovers",
                         (unsigned) largest, threshold, left_s);
            } else if (v.action == tk::HeapAction::Watching) {
                ESP_LOGE(TAG, "HEAP CRITICAL for %u s (internal largest_block %u B < %u B) — "
                              "restarting in %u s unless it recovers",
                         held_s, (unsigned) largest, threshold, left_s);
            } else if (v.action == tk::HeapAction::Recovered) {
                if (v.ota_excused) {
                    ESP_LOGW(TAG, "HEAP critical run (%u s) cleared: an OTA is in flight and holds "
                                  "the largest allocations we make — not judging the heap during "
                                  "an install", held_s);
                } else {
                    ESP_LOGW(TAG, "HEAP recovered after %u s critical (internal largest_block now "
                                  "%u B) — watchdog disarmed, no restart needed",
                             held_s, (unsigned) largest);
                }
            }

            if (v.action == tk::HeapAction::Restart) {
                // How many watchdog restarts this run of exhaustion has already caused (0 unless
                // the last boot was one of ours). Five is proof that restarting does not fix it,
                // and continuing would cycle the radios every ~10 min forever — see
                // kHeapMaxConsecutiveRestarts. Latch the run so we say this once, not every 30 s.
                uint8_t prior = VehicleController::boot_heap_restarts();
                if (!tk::heap_may_restart(prior)) {
                    static bool said = false;
                    if (!said) {
                        said = true;
                        ESP_LOGE(TAG, "HEAP EXHAUSTED for %u s but %u consecutive watchdog restarts "
                                      "have not fixed it — NOT restarting again, staying up "
                                      "degraded so it can be diagnosed",
                                 held_s, (unsigned) prior);
                    }
                } else {
                    tk::HeapReason why = tk::heap_reason_format((uint8_t)(prior + 1));
                    // Join the same CAS gate used by OTA and identity journals BEFORE making the
                    // restart durable. If either operation is in flight, postpone this watchdog
                    // sample without writing reboot_why; the next sample retries after the owner
                    // exits. Once the breadcrumb is durable we intentionally hold FaultRestart
                    // until esp_restart(), closing the inverse "persisted, then OTA started" race.
                    if (!ota_fault_restart_begin()) {
                        static bool said_gate = false;
                        if (!said_gate) {
                            said_gate = true;
                            ESP_LOGW(TAG, "HEAP EXHAUSTED for %u s but OTA/identity work is in "
                                          "flight — postponing deliberate restart",
                                     held_s);
                        }
                        continue;
                    }
                    if (!self->persist_reboot_reason_(why.text)) {
                        // Persistence authorizes the reboot: without the next counter a reboot
                        // would forget this attempt and could cycle BLE/radios forever. Stay up
                        // degraded and keep retrying the durable write on later watchdog samples.
                        // Release FaultRestart so the failed attempt does not wedge OTA/identity.
                        ota_fault_restart_cancel();
                        static bool said_persist = false;
                        if (!said_persist) {
                            said_persist = true;
                            ESP_LOGE(TAG, "HEAP EXHAUSTED for %u s but reboot_why could not be "
                                          "persisted — NOT restarting, staying up degraded",
                                     held_s);
                        }
                        continue;
                    }
                    // The one line that has to survive the reboot and explain it on its own —
                    // state, threshold, how long, which restart, and where the reasoning lives.
                    // Keep every line here well under ~230 chars: diag_log.cpp's capture hook
                    // formats into a 256-byte stack buffer, so a longer line reaches syslog cut
                    // off mid-sentence — and this is the one that must not be.
                    ESP_LOGE(TAG, "HEAP EXHAUSTED for %u s (internal largest_block %u B < %u B, "
                                  "free %u B) — RESTARTING DELIBERATELY (watchdog restart %u/%u, "
                                  "reboot_why=heap:%u; no in-place recovery exists, see "
                                  "docs/ARCHITECTURE.md)",
                             held_s, (unsigned) largest, threshold,
                             (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                             (unsigned) (prior + 1), (unsigned) tk::kHeapMaxConsecutiveRestarts,
                             (unsigned) (prior + 1));
                    // Heap exhaustion is an automatic fault reboot, not a user health signal.
                    // Leaving PENDING_VERIFY armed is exactly what lets the bootloader escape a
                    // new image whose heap regression triggers this watchdog.
                    static_assert(!tk::ota_reboot_confirms_pending_image(
                        tk::OtaRebootClass::AutomaticFaultRecovery));
                    // Let the log actually LEAVE the device before we kill it. syslog_send only
                    // queues, and its task runs at priority 3 against this task's 5, so without a
                    // yield the final message dies in the queue on a single-core target — and the
                    // /diag ring does not survive the reboot either. Then the one thing explaining
                    // the restart would be gone, which is the whole point of logging it. 300 ms is
                    // nothing against a 5 min hold, and nothing against the 60 s task-watchdog
                    // budget this task is now subscribed to either — but feed it first anyway, so a
                    // deliberate restart is never misreported as a task_wdt panic on the way out.
                    if (watchdog.active()) esp_task_wdt_reset();
                    vTaskDelay(pdMS_TO_TICKS(300));
                    esp_restart();
                }
            }
        }

        // While NOT paired we stay completely out of the way: the auto-pair task owns the
        // connection and the single command queue, and a stray charge poll injected into
        // that queue mid-handshake corrupts the pairing exchange (overlapping responses →
        // RX reassembly errors → pairing never completes).
        const bool identity_ready = self->command_identity_ready_();
        bool paired = identity_ready && self->has_session();

        // ── VCSEC sleep-flag sampler (feeds link_state()'s asleep debounce) ───────────────
        // The library updates Vehicle::sleep_state() from the car's vehicleSleepStatus on
        // every VCSEC poll, including auto_pair_task's idle health probe — the only BLE
        // traffic while parked. The RX task publishes an atomic mirror while holding
        // vehicle_mutex_; sample that mirror here and fold it into the debounce clock so link_state() can require a
        // STABLE ASLEEP run before showing "Vehicle asleep". UNKNOWN leaves the clock alone.
        // Log only on a transition so the serial console reveals what the car actually reports
        // (e.g. whether VCSEC ever asserts ASLEEP, or just flaps for COP) without spamming.
        if (paired) {
            TeslaBLE::SleepState st = static_cast<TeslaBLE::SleepState>(
                self->vcsec_sleep_state_.load());
            if (st != prev_sleep) {
                ESP_LOGI(TAG, "VCSEC sleep flag: %s",
                         st == TeslaBLE::SleepState::ASLEEP ? "ASLEEP"
                       : st == TeslaBLE::SleepState::AWAKE  ? "AWAKE" : "UNKNOWN");
                prev_sleep = st;
            }
            if (st == TeslaBLE::SleepState::ASLEEP)     self->note_vcsec_sleep_(true);
            else if (st == TeslaBLE::SleepState::AWAKE)  self->note_vcsec_sleep_(false);
            // UNKNOWN: leave the run untouched.

            // One-shot charge poll on the VCSEC wake edge (issue #264). Arm only after a DEBOUNCED
            // ASLEEP run (reusing kAsleepDebounceS, the same debounce link_state() trusts), so the
            // ~60 s COP AWAKE↔ASLEEP flap and UNKNOWN→AWAKE at boot can't fire it; then request
            // exactly one poll the next time the car wakes itself (cable plug-in, door, app). The
            // decision is host-tested in logic/wake_poll.hpp; this site only samples and latches.
            const tk::WakeSample wake_sample =
                st == TeslaBLE::SleepState::ASLEEP ? tk::WakeSample::Asleep
              : st == TeslaBLE::SleepState::AWAKE  ? tk::WakeSample::Awake
                                                   : tk::WakeSample::Unknown;
            if (tk::wake_edge_should_poll(
                    wake_poll, {wake_sample, self->vcsec_stably_asleep_(tk::kAsleepDebounceS)})) {
                wake_poll_pending = true;
            }
        } else {
            // Unpaired: the sampler above does not run, so retire any armed edge and pending
            // request rather than carry them across a pairing reset.
            wake_poll = {};
            wake_poll_pending = false;
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
        // revocation canary. Idle evcc reads may use the last cache value; during this
        // active window get_charge_state requires a recent ChargeState instead.
        uint32_t now_ticks = xTaskGetTickCount();
        bool charging_state;
        {
            ChargeStateResult cs = self->copy_locked_(self->last_known_charge_);
            charging_state = cs.valid && (cs.charging_state == "Charging" ||
                                          cs.charging_state == "Starting");
        }
        uint32_t lc = self->last_cmd_ticks_.load();
        bool recent_cmd = (lc != 0) && ((now_ticks - lc) < pdMS_TO_TICKS(kActiveWindowMs));
        // Gate the charging arm on FRESH contact: charging_state is a RAM cache never invalidated on
        // a link drop, so a car that unplugged and left (or dropped BLE) while cached "Charging"
        // would otherwise hold the window open forever → perpetual scanning. A charging, reachable
        // car answers the ~10 s charge poll, so its contact stays fresh (< kAwakeMaxAgeS). Decision +
        // boundary are host-tested in logic/active_window.hpp.
        uint32_t contact_age = 0;
        bool have_contact = self->seconds_since_contact(contact_age);
        bool window = tk::active_window_open({recent_cmd, charging_state, have_contact, contact_age});

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
            tk::SemGuard g(self->vehicle_mutex_);   // RAII: charge_state_poll can throw
            self->vehicle_->charge_state_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP);
        }

        // One-shot wake-edge charge poll (issue #264). Fires OUTSIDE the active window — that is
        // the whole point: a self-woken car (cable plug-in) that sent no command and shows no
        // cached charging would otherwise never refresh its SOC, so evcc keeps acting on a stale
        // reading. NO_WAKE_SKIP preserves the anti-vampire-drain guarantee (a car already back
        // asleep is skipped, so we only ride a wake the car performed itself); exactly one poll
        // per wake episode. If it reports Charging/Starting the charging arm opens the window and
        // normal session polling takes over. When the window is already open the 10 s refresh
        // above covers it, so just consume the request without a redundant poll.
        if (wake_poll_pending && paired) {
            if (window) {
                wake_poll_pending = false;
            } else if (self->ble_connected() && !self->cmd_in_flight_.load()) {
                wake_poll_pending = false;
                ESP_LOGI(TAG, "VCSEC wake edge: one-shot charge poll to refresh cached SOC");
                tk::SemGuard g(self->vehicle_mutex_);   // RAII: charge_state_poll can throw
                self->vehicle_->charge_state_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP);
            }
            // else: not connected yet or a command is in flight — keep the request, retry next cycle.
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
            {
                tk::SemGuard g(self->vehicle_mutex_);   // RAII: the *_poll builders can throw
                switch (tele_idx % 4) {
                    case 0: self->vehicle_->climate_state_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP);  break;
                    case 1: self->vehicle_->drive_state_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP);    break;
                    case 2: self->vehicle_->tire_pressure_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP);  break;
                    case 3: self->vehicle_->closures_state_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP); break;
                }
            }
            tele_idx++;
        }
      } catch (const std::exception& e) {
          ESP_LOGE(TAG, "vehicle loop iteration threw (%s) — continuing", e.what());
      } catch (...) {
          ESP_LOGE(TAG, "vehicle loop iteration threw (unknown) — continuing");
      }

      vTaskDelay(pdMS_TO_TICKS(50));
    }
  } catch (...) {
      ESP_LOGE(TAG, "vehicle loop boundary threw outside an iteration — stopping task");
      // TaskWatchdogSubscription has already unwound and removed this task from TWDT.
      vTaskDelete(nullptr);
  }
}

// ─── Data queries ─────────────────────────────────────────────────────────────

bool VehicleController::get_charge_state(ChargeStateResult& out, int /*timeout_ms*/) {
    // Serve the cached reading instantly and never block. evcc polls vehicle_data
    // frequently and times out quickly, so an on-demand connect + poll here would risk a
    // gateway timeout. Freshness is maintained out of band by loop_task while the active
    // window is open (a recent command OR the car charging). In that window, a ChargeState
    // older than kActiveChargeStateMaxAgeS is a broken feedback path and must fail instead
    // of masquerading as live data. Outside it, serving the last value is deliberate: a
    // parked idle car must be allowed to sleep without read-only evcc traffic waking it.
    ChargeStateResult cached;
    uint32_t sample = 0;
    uint32_t generation = 0;
    {
        // Keep the cache and its freshness stamp coherent. The callback updates all
        // three under this same lock; reading them in separate critical sections could
        // pair an old value with the timestamp from a newer response.
        tk::MutexGuard g(cache_mutex_);
        cached = last_known_charge_;
        sample = last_charge_ticks_.load();
        generation = charge_state_generation_.load();
    }
    if (!cached.valid) return false;

    uint32_t now = xTaskGetTickCount();
    uint32_t cmd = last_cmd_ticks_.load();
    bool recent_cmd = cmd != 0 && (now - cmd) < pdMS_TO_TICKS(kActiveWindowMs);
    bool charging = cached.charging_state == "Charging" ||
                    cached.charging_state == "Starting";
    bool active_window = recent_cmd || charging;

    // generation is the "have sample" bit so a legitimate callback at FreeRTOS tick 0
    // is not mistaken for "never received".
    bool have_sample_age = generation != 0;
    uint32_t sample_age_s = have_sample_age
        ? (now - sample) / configTICK_RATE_HZ
        : 0;
    if (!tk::charge_cache_usable(cached.valid, active_window,
                                 have_sample_age, sample_age_s)) {
        if (!charge_cache_stale_reported_.exchange(true)) {
            ESP_LOGW(TAG,
                     "charge-state cache stale during active window (%us old) — refusing live response",
                     (unsigned)sample_age_s);
        }
        return false;
    }

    out = std::move(cached);
    return true;
}

bool VehicleController::get_vehicle_status(VehicleStatusResult& out, tk::ConnectOrigin origin,
                                           int timeout_ms) {
    out = {};
    if (!tk::runtime_admission_vehicle_ready()) return false;
    if (timeout_ms <= 0) return false;
    const uint32_t deadline = deadline_in_(static_cast<uint32_t>(timeout_ms));
    tk::SemGuard cmd_guard(command_mutex_, ticks_until_(deadline));
    if (!cmd_guard) {
        if (origin == tk::ConnectOrigin::Foreground) {
            ESP_LOGW(TAG, "vehicle-status deadline exhausted waiting for another request");
        } else {
            ESP_LOGD(TAG, "background vehicle-status deadline exhausted waiting for another request");
        }
        return false;
    }
    if (!command_identity_ready_()) {
        ESP_LOGE(TAG, "vehicle-status poll refused — runtime key is not verified");
        return false;
    }
    // Mark both HTTP and auto-pair queries in-flight so loop_task doesn't inject a slow
    // telemetry poll ahead of either one on the single BLE FIFO. This arbitration flag is
    // independent of `origin`: only the HTTP request is foreground provenance.
    tk::InFlightGuard inflight(cmd_in_flight_);
    // A VCSEC status poll is the auto-pair / wake probe as well as an HTTP read, so it
    // must be able to bring the BLE link up. With a NO-wake policy it reads status
    // (including ASLEEP) without actually waking the car.
    if (!ensure_connected_until_(capped_deadline_(deadline, 10000), origin)) return false;
    if (remaining_ms_(deadline) <= 0) {
        if (origin == tk::ConnectOrigin::Foreground) {
            ESP_LOGW(TAG, "vehicle-status deadline exhausted after BLE connect");
        } else {
            ESP_LOGD(TAG, "background vehicle-status deadline exhausted after BLE connect");
        }
        return false;
    }

    struct StatusCompletion {
        SemaphoreHandle_t sem{xSemaphoreCreateBinary()};
        int32_t lock_state{0};
        int32_t sleep_status{0};
        int32_t user_presence{0};
        ~StatusCompletion() { if (sem) vSemaphoreDelete(sem); }
    };
    auto completion = std::make_shared<StatusCompletion>();
    if (!completion->sem) {
        ESP_LOGE(TAG, "vehicle-status poll refused — command completion unavailable");
        return false;
    }
    uint32_t generation = command_generation_.fetch_add(1) + 1;
    if (generation == 0) {
        generation = 1;
        command_generation_.store(generation);
    }

    auto callback = [this, completion, generation](const VCSEC_VehicleStatus& vs) {
        if (command_generation_.load() != generation) return;
        completion->lock_state = static_cast<int32_t>(vs.vehicleLockState);
        completion->sleep_status = static_cast<int32_t>(vs.vehicleSleepStatus);
        completion->user_presence = static_cast<int32_t>(vs.userPresence);
        if (command_generation_.load() == generation) xSemaphoreGive(completion->sem);
    };

    try {
        // Both setter and poll are tesla-ble calls and the setter mutates a std::function read
        // from the RX task. Serialize them under the same mutex as on_rx_data/loop.
        tk::SemGuard g(vehicle_mutex_);
        vehicle_->set_vehicle_status_callback(std::move(callback));
        vehicle_->vcsec_poll();
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "vehicle-status poll threw (%s) — invalidating command FIFO", e.what());
        invalidate_and_flush_(generation);
        tk::SemGuard g(vehicle_mutex_);
        vehicle_->set_vehicle_status_callback(nullptr);
        return false;
    } catch (...) {
        ESP_LOGE(TAG, "vehicle-status poll threw (unknown) — invalidating command FIFO");
        invalidate_and_flush_(generation);
        tk::SemGuard g(vehicle_mutex_);
        vehicle_->set_vehicle_status_callback(nullptr);
        return false;
    }

    bool ok = xSemaphoreTake(completion->sem, ticks_until_(deadline)) == pdTRUE &&
              command_generation_.load() == generation;
    if (!ok) {
        note_completion_timeout_(
            "VCSEC Status Poll",
            origin == tk::ConnectOrigin::Foreground
                ? tk::CompletionTimeoutPolicy::ForegroundWarn
                : tk::CompletionTimeoutPolicy::ExpectedSilent);
        invalidate_and_flush_(generation);
    }
    {
        tk::SemGuard g(vehicle_mutex_);
        vehicle_->set_vehicle_status_callback(nullptr);
    }
    if (ok) {
        out.valid = true;
        switch (completion->lock_state) {
            case VCSEC_VehicleLockState_E_VEHICLELOCKSTATE_LOCKED: out.lock_state = "LOCKED"; break;
            case VCSEC_VehicleLockState_E_VEHICLELOCKSTATE_UNLOCKED: out.lock_state = "UNLOCKED"; break;
            default: out.lock_state = "UNKNOWN"; break;
        }
        switch (completion->sleep_status) {
            case VCSEC_VehicleSleepStatus_E_VEHICLE_SLEEP_STATUS_AWAKE: out.sleep_status = "AWAKE"; break;
            case VCSEC_VehicleSleepStatus_E_VEHICLE_SLEEP_STATUS_ASLEEP: out.sleep_status = "ASLEEP"; break;
            default: out.sleep_status = "UNKNOWN"; break;
        }
        switch (completion->user_presence) {
            case VCSEC_UserPresence_E_VEHICLE_USER_PRESENCE_PRESENT: out.user_presence = "PRESENT"; break;
            case VCSEC_UserPresence_E_VEHICLE_USER_PRESENCE_NOT_PRESENT: out.user_presence = "NOT_PRESENT"; break;
            default: out.user_presence = "UNKNOWN"; break;
        }
    }
    if (ok && out.valid) {
        tk::completion_ok_note(completion_timeout_);
        note_reachable_();  // car answered a VCSEC status read ⇒ reachable over BLE right now
        cmd_fail_streak_.store(0);  // a clean round-trip ⇒ link healthy, reset desync backstop
        tk::MutexGuard cache_guard(cache_mutex_);
        last_known_status_ = out;
    }
    return ok && out.valid;
}
