#pragma once

#include <string>

// Vehicle-state result structs — the cached shapes VehicleController hands to every
// consumer (/status + web UI, /api evcc routes, MQTT/HA bridge, MCP get_vehicle_state,
// display/LED via UiSnapshot). IDF-free ON PURPOSE: logic/status_model.hpp shapes the
// /status contract from these on the host (test/test_logic.cpp golden CHECKs), and the
// device serializes the same structs — so a field-contract regression fails the mock
// build instead of surfacing on hardware. Moved verbatim from vehicle_ctrl.hpp; the
// presence-flag (has_*) conventions documented per-struct below are the load-bearing
// part (proto3 optional: an unreported field must render as absent, never a phantom 0).

struct ChargeStateResult {
    bool valid{false};
    // Numeric fields carry presence flags like the telemetry structs below: the car omits
    // values it has no reading for (proto3 optional). The display paths (MQTT/HA, /status)
    // emit a field only when present so it renders "unknown"/omitted, not a phantom 0. The
    // evcc-facing /api path is the deliberate exception — it always emits every field.
    float       battery_level{0};       bool has_battery_level{false};
    float       charge_limit_soc{0};    bool has_charge_limit_soc{false};
    std::string charging_state;
    float       charger_power{0};       bool has_charger_power{false};
    float       charge_rate{0};         bool has_charge_rate{false};
    int         charging_amps{0};       bool has_charging_amps{false};
    float       battery_range{0};       bool has_battery_range{false};
    // ── Extended read-only charge telemetry (HA/MQTT bridge only; never on the /api evcc
    // path). Already decoded for free in the same CarServer_ChargeState the fields above
    // come from, so parsing them adds no BLE round-trip. Presence-flagged like the rest.
    int         charger_actual_current{0}; bool has_actual_current{false};   // A delivered now
    int         charger_voltage{0};        bool has_voltage{false};          // V at the charger
    int         charge_current_request{0}; bool has_current_request{false};  // A the car asked for
    int         charger_phases{0};         bool has_charger_phases{false};   // 1 / 2 / 3
    float       charge_energy_added{0};    bool has_energy_added{false};     // kWh this session
    int         minutes_to_full_charge{0}; bool has_minutes_to_full{false};  // min
    std::string charge_limit_reason;       // "" if the car reported none
};

struct VehicleStatusResult {
    bool valid{false};
    std::string lock_state;
    std::string sleep_status;
    std::string user_presence;
};

// ─── Read-only telemetry (refreshed in the background, shown in the web UI) ──────
// Each carries presence flags for the numeric fields because the car omits values
// it has no reading for (proto3 optional); a missing field must render as "—",
// not as 0.
struct ClimateStateResult {
    bool valid{false};
    bool has_climate_on{false};      bool is_climate_on{false};
    bool has_preconditioning{false}; bool is_preconditioning{false};
    bool  has_inside{false};   float inside_temp{0};      // °C
    bool  has_outside{false};  float outside_temp{0};     // °C
    bool  has_setpoint{false}; float driver_setpoint{0};  // °C
    // Cabin Overheat Protection — a parked anti-overheat subsystem separate from
    // the main HVAC, so is_climate_on does NOT reflect it. Short (≤15 char) label
    // strings keep SSO so the per-poll struct copy never heap-allocs.
    bool has_cop{false};         std::string cop;          // "Off"/"On"/"FanOnly"
    bool has_cop_cooling{false}; bool        cop_cooling{false}; // actively cooling now
    bool has_cop_temp{false};    std::string cop_temp;     // "Low"/"Medium"/"High"
    bool has_cop_reason{false};  std::string cop_reason;   // why COP isn't cooling
    // Defrost — front/rear defroster + Max-defrost mode (part of the HVAC, not COP).
    bool has_front_defrost{false}; bool front_defrost{false};
    bool has_rear_defrost{false};  bool rear_defrost{false};
    bool has_defrost_mode{false};  std::string defrost_mode;  // "Off"/"Normal"/"Max"
};

struct DriveStateResult {
    bool valid{false};
    std::string shift_state;          // "P"/"R"/"N"/"D" or "" if unknown
    bool  has_odometer{false}; float odometer_km{0};
};

struct TirePressureResult {
    bool valid{false};
    bool  has_fl{false}; float fl{0};   // bar
    bool  has_fr{false}; float fr{0};
    bool  has_rl{false}; float rl{0};
    bool  has_rr{false}; float rr{0};
    // Aggregate over all eight soft/hard per-wheel warnings with present-AND-true
    // semantics — an unreported wheel counts as "no warning" BY DESIGN (no presence
    // flag; the alternative would alarm on every partial report).
    bool  warn{false};
};

struct ClosuresStateResult {
    bool valid{false};
    bool has_locked{false};       bool locked{false};
    // The four *_open fields aggregate per-opening booleans with present-AND-true
    // semantics — an unreported opening counts as "closed" BY DESIGN (no presence
    // flags; Tesla sends closures as a full set, and "open" is the actionable state).
    bool any_door_open{false};
    bool frunk_open{false};
    bool trunk_open{false};
    bool any_window_open{false};
    bool has_user_present{false}; bool user_present{false};
};
