// Telemetry caches: the protobuf→struct parsers, the persistent cache callbacks
// (install_state_callbacks_), the background poll / sleep-gating loop (loop_task_fn_)
// and the data queries serving cached readings (get_charge_state, get_vehicle_status).
// Part of the VehicleController implementation split — see vehicle_ctrl_internal.hpp.

#include "vehicle_ctrl.hpp"
#include "vehicle_ctrl_internal.hpp"
#include "logic/units.hpp"
#include "logic/active_window.hpp"
#include "logic/heap_watchdog.hpp"
#include "ota_update.hpp"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <exception>

// protobuf generated headers (from tesla-ble)
#include <vcsec.pb.h>
#include <car_server.pb.h>

static const char* TAG = "vehicle_ctrl";

namespace {
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

// ─── Cache callbacks (installed once from init) ───────────────────────────────

void VehicleController::install_state_callbacks_() {
    // Persistent charge-state callback: refreshes the cache on *every* ChargeState
    // (the background refresh in loop_task). Installed once, never cleared; HTTP
    // reads serve last_known_charge_ from this cache without blocking.
    vehicle_->set_charge_state_callback([this](const CarServer_ChargeState& cs) {
        tk::MutexGuard g(cache_mutex_);
        parse_charge_state(cs, last_known_charge_);
        note_contact_();
    });

    // Read-only telemetry callbacks. Fed by the rotating background poll in loop_task_fn_
    // (one telemetry domain per cycle). Each refreshes its own cache for the web UI; none
    // affect pairing or evcc. Installed once, never cleared.
    vehicle_->set_climate_state_callback([this](const CarServer_ClimateState& cs) {
        tk::MutexGuard g(cache_mutex_);
        parse_climate_state(cs, last_known_climate_);
        note_contact_();
    });
    vehicle_->set_drive_state_callback([this](const CarServer_DriveState& ds) {
        tk::MutexGuard g(cache_mutex_);
        parse_drive_state(ds, last_known_drive_);
        note_contact_();
    });
    vehicle_->set_tire_pressure_state_callback([this](const CarServer_TirePressureState& t) {
        tk::MutexGuard g(cache_mutex_);
        parse_tire_pressure(t, last_known_tires_);
        note_contact_();
    });
    vehicle_->set_closures_state_callback([this](const CarServer_ClosuresState& c) {
        tk::MutexGuard g(cache_mutex_);
        parse_closures_state(c, last_known_closures_);
        note_contact_();
    });
}

// ─── Background poll / sleep-gating loop ──────────────────────────────────────

void VehicleController::loop_task_fn_(void* arg) {
    auto* self = static_cast<VehicleController*>(arg);
    while (true) {
        try {
            loop_task_impl_(arg);
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "vehicle task escaped an exception (%s) — restarting task loop", e.what());
            self->ble_fault_.store(true);
        } catch (...) {
            ESP_LOGE(TAG, "vehicle task escaped an unknown exception — restarting task loop");
            self->ble_fault_.store(true);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void VehicleController::loop_task_impl_(void* arg) {
    auto* self = static_cast<VehicleController*>(arg);
    uint32_t last_poll_ticks    = 0;
    uint32_t last_connect_ticks = 0;
    uint32_t last_tele_ticks    = 0;
    int      tele_idx           = 0;  // rotates the telemetry domain polled each cycle
    bool     prev_window        = false;  // edge-detect the active window
    auto     prev_sleep         = TeslaBLE::SleepState::UNKNOWN;  // edge-detect VCSEC sleep flag
    while (true) {
        {
            tk::MutexGuard vehicle_guard(self->vehicle_mutex_);
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
        }

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
            // INTERNAL, not plain 8BIT: heap_caps_* reports the max across every heap with the
            // cap, and the esp32c5 registers 8 MB of PSRAM into 8BIT. Deciding on that number
            // would make the watchdog below a silent no-op on exactly the board that has PSRAM.
            // Logged alongside the historical 8BIT figures (identical on the four PSRAM-less
            // targets) so the trend stays comparable with older captures.
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
            ESP_LOGW(TAG, "HEAP free=%u largest_block=%u min_free=%u internal_largest=%u",
                     (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                     (unsigned) largest);

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
                    tk::HeapReason why = tk::heap_reason_format((uint8_t)(prior + 1));
                    self->persist_reboot_reason_(why.text);
                    // A restart inside the ~90 s OTA health gate would look like a failed boot and
                    // revert a good image. This is a deliberate restart, so confirm the image
                    // first — same reasoning as the config-save reboots.
                    ota_confirm_pending_image();
                    // Let the log actually LEAVE the device before we kill it. syslog_send only
                    // queues, and its task runs at priority 3 against this task's 5, so without a
                    // yield the final message dies in the queue on a single-core target — and the
                    // /diag ring does not survive the reboot either. Then the one thing explaining
                    // the restart would be gone, which is the whole point of logging it. This task
                    // is not registered with the task WDT, and 300 ms is nothing against a 5 min
                    // hold.
                    vTaskDelay(pdMS_TO_TICKS(300));
                    esp_restart();
                }
            }
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
            tk::MutexGuard vehicle_guard(self->vehicle_mutex_);
            self->vehicle_->charge_state_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP);
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
            tk::MutexGuard vehicle_guard(self->vehicle_mutex_);
            switch (tele_idx % 4) {
                case 0: self->vehicle_->climate_state_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP);  break;
                case 1: self->vehicle_->drive_state_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP);    break;
                case 2: self->vehicle_->tire_pressure_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP);  break;
                case 3: self->vehicle_->closures_state_poll(TeslaBLE::WakePolicy::NO_WAKE_SKIP); break;
            }
            tele_idx++;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
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
    tk::MutexGuard g(cache_mutex_);
    if (last_known_charge_.valid) {
        out = last_known_charge_;
        return true;
    }
    return false;
}

bool VehicleController::get_vehicle_status(VehicleStatusResult& out, int timeout_ms) {
    tk::MutexGuard cmd_guard(command_mutex_);
    // Foreground blocking query (HTTP /body_controller_state, auto-pair probes) — mark it
    // in-flight like the other runners so loop_task doesn't inject a slow background
    // telemetry poll ahead of it on the single BLE FIFO.
    tk::InFlightGuard inflight(cmd_in_flight_);
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

    {
        tk::MutexGuard vehicle_guard(vehicle_mutex_);
        vehicle_->vcsec_poll();
    }

    bool ok = xSemaphoreTake(cmd_sem_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    {
        tk::MutexGuard vehicle_guard(vehicle_mutex_);
        vehicle_->set_vehicle_status_callback(nullptr);
    }
    out = pending_status_;
    if (ok && out.valid) {
        note_reachable_();  // car answered a VCSEC status read ⇒ reachable over BLE right now
        cmd_fail_streak_.store(0);  // a clean round-trip ⇒ link healthy, reset desync backstop
        tk::MutexGuard cache_guard(cache_mutex_);
        last_known_status_ = out;
    }
    return ok && out.valid;
}
