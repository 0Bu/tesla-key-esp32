#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

#include "logic/units.hpp"

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
    float       battery_level{0};        bool has_battery_level{false};
    float       usable_battery_level{0}; bool has_usable_battery_level{false};
    float       charge_limit_soc{0};     bool has_charge_limit_soc{false};
    std::string charging_state;
    float       charger_power{0};       bool has_charger_power{false};
    float       charge_rate{0};         bool has_charge_rate{false};
    int         charging_amps{0};       bool has_charging_amps{false};
    float       battery_range{0};       bool has_battery_range{false};
    // ── Extended read-only charge telemetry (HA/MQTT bridge + minutes_to_full_charge on
    // /api vehicle_data). Already decoded for free in the same CarServer_ChargeState the fields
    // above come from, so parsing them adds no BLE round-trip. Presence-flagged like the rest.
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

namespace tk {

// Serialize charge_state for GET /api/1/vehicles/{VIN}/vehicle_data
// Shape mirrors Tesla Fleet API / TeslaBleHttpProxy: always emits every field so evcc
// parsing floats/ints never hits a missing key. On failure/omission, cs is zero-initialised
// and minutes_to_full_charge emits 0 (where evcc's > 0 guard correctly yields "").
template <typename Emitter>
void emit_vehicle_charge_state(const ChargeStateResult& cs, Emitter& e) {
    e.str("charging_state", cs.charging_state.empty() ? "Disconnected" : cs.charging_state.c_str());
    e.num("battery_level",          cs.battery_level);
    e.num("usable_battery_level",   cs.has_usable_battery_level ? cs.usable_battery_level : cs.battery_level);
    e.num("charge_limit_soc",       cs.charge_limit_soc);
    e.num("charger_power",          cs.charger_power);
    e.num("charge_rate",            cs.charge_rate);
    e.num("charge_amps",            cs.charging_amps);
    e.num("charge_energy_added",    cs.charge_energy_added);
    e.num("battery_range",          cs.battery_range);
    e.num("minutes_to_full_charge", cs.minutes_to_full_charge);
}

// Fixed snapshot for the five evcc climate fields; the API never copies unrelated cache strings.
struct VehicleClimateData {
    bool valid{false};
    bool has_preconditioning{false};
    bool is_climate_on{false};
    bool is_preconditioning{false};
    float inside_temp{0};
    float outside_temp{0};
    float driver_setpoint{0};
};
static_assert(std::is_trivially_copyable_v<VehicleClimateData> && sizeof(VehicleClimateData) <= 32);

inline VehicleClimateData vehicle_climate_data(const ClimateStateResult& climate) noexcept {
    return {climate.valid, climate.has_preconditioning, climate.is_climate_on,
            climate.is_preconditioning, climate.inside_temp, climate.outside_temp,
            climate.driver_setpoint};
}

// Always emit typed climate fields; availability is checked separately before HTTP success.
// An unreported is_preconditioning is false, exactly like TeslaBleHttpProxy's
// GetIsPreconditioning(); only a present-AND-true field emits true.
template <typename Emitter>
void emit_vehicle_climate_state(const VehicleClimateData& cl, Emitter& e) {
    e.boolean("is_climate_on",      cl.is_climate_on);
    e.boolean("is_preconditioning", cl.has_preconditioning && cl.is_preconditioning);
    e.num("inside_temp",            cl.inside_temp);
    e.num("outside_temp",           cl.outside_temp);
    e.num("driver_temp_setting",    cl.driver_setpoint);
}

template <typename Emitter>
void emit_vehicle_climate_state(const ClimateStateResult& cl, Emitter& e) {
    emit_vehicle_climate_state(vehicle_climate_data(cl), e);
}

// Climate is one of four telemetry-rotation slots at 30 s (about 120 s per refresh), so 300 s
// tolerates one missed poll; the shell polls climate first when the active window opens. evcc's
// climater steers charging, so a stale in-window value must not pose as live.
inline constexpr uint32_t kActiveClimateStateMaxAgeS = 300;

// Same shape as charge_cache_usable(): outside the active window the last value stays usable
// (reads never wake the car); inside it, only a sample younger than the limit counts.
inline bool climate_cache_usable(bool valid, bool active_window,
                                 bool have_sample_age, uint32_t sample_age_s) noexcept {
    if (!valid) return false;
    if (!active_window) return true;
    return have_sample_age && sample_age_s <= kActiveClimateStateMaxAgeS;
}

// Fixed snapshot for evcc's odometer; never copies shift_state or other drive strings.
struct VehicleDriveData {
    bool valid{false};
    bool has_odometer{false};
    float odometer_km{0};
};
static_assert(std::is_trivially_copyable_v<VehicleDriveData> && sizeof(VehicleDriveData) <= 16);

inline VehicleDriveData vehicle_drive_data(const DriveStateResult& drive) noexcept {
    return {drive.valid, drive.has_odometer, drive.odometer_km};
}

// Emit exactly one typed number, "odometer", in MILES (TeslaBleHttpProxy shape): evcc's template
// scales by 1.60934 and rejects <= 0, so an absent odometer emits 0.
template <typename Emitter>
void emit_vehicle_drive_state(const VehicleDriveData& dr, Emitter& e) {
    e.num("odometer", dr.has_odometer ? static_cast<float>(km_to_mi(dr.odometer_km)) : 0.0f);
}

// Last-known semantics: the odometer only grows, so freshness comes from the in-window rotation
// and the wake/bootstrap one-shot rather than an age limit.
inline bool vehicle_drive_available(const VehicleDriveData& drive) noexcept {
    return drive.valid && drive.has_odometer;
}

// Domain selection bits for vehicle_data endpoints.
inline constexpr unsigned kVehicleDataCharge  = 1;
inline constexpr unsigned kVehicleDataClimate = 2;
inline constexpr unsigned kVehicleDataDrive   = 4;

struct VehicleDataEndpoints {
    // Internal selection flags stay in one byte; no binary/wire representation is exposed.
    bool charge_state : 1;
    bool climate_state : 1;
    bool drive_state : 1;
    bool has_unsupported : 1;

    // Nothing selected, nothing unsupported. Use of()/unsupported(): positional bools were
    // ambiguous once a third domain existed.
    constexpr VehicleDataEndpoints() noexcept
        : charge_state(false), climate_state(false), drive_state(false), has_unsupported(false) {}

    // Selection from a mask of kVehicleData* bits.
    static constexpr VehicleDataEndpoints of(unsigned mask) noexcept {
        VehicleDataEndpoints ep;
        ep.charge_state  = (mask & kVehicleDataCharge) != 0;
        ep.climate_state = (mask & kVehicleDataClimate) != 0;
        ep.drive_state   = (mask & kVehicleDataDrive) != 0;
        return ep;
    }

    // Fail-closed marker: only has_unsupported is set.
    static constexpr VehicleDataEndpoints unsupported() noexcept {
        VehicleDataEndpoints ep;
        ep.has_unsupported = true;
        return ep;
    }

    constexpr bool empty() const noexcept {
        return !charge_state && !climate_state && !drive_state;
    }
};

// Decode one bounded selector list. Raw ';'/',' and URL-encoded delimiters are accepted.
// Unsupported, duplicate or incomplete selectors fail closed; this does not parse a URI.
inline VehicleDataEndpoints parse_vehicle_data_endpoint_list(std::string_view value) noexcept {
    if (value.size() >= 128) return VehicleDataEndpoints::unsupported();
    // Avoid pointer arithmetic on a default string_view's null data pointer.
    if (value.empty()) return VehicleDataEndpoints::unsupported();
    const char* value_end = value.data() + value.size();
    const char* cursor = value.data();
    unsigned selected = 0;
    while (true) {
        while (cursor < value_end && (*cursor == ' ' || *cursor == '\t')) ++cursor;
        const char* end = cursor;
        unsigned delimiter = 0;
        while (end < value_end) {
            if (*end == ';' || *end == ',') { delimiter = 1; break; }
            if (*end == '%' && value_end - end >= 3 &&
                ((end[1] == '3' && (end[2] | 0x20) == 'b') ||
                 (end[1] == '2' && (end[2] | 0x20) == 'c'))) {
                delimiter = 3;
                break;
            }
            ++end;
        }
        const char* trimmed = end;
        while (trimmed > cursor && (trimmed[-1] == ' ' || trimmed[-1] == '\t')) --trimmed;
        const size_t length = trimmed - cursor;
        unsigned domain = 0;
        if (length == 12 && memcmp(cursor, "charge_state", 12) == 0) domain = kVehicleDataCharge;
        else if (length == 13 && memcmp(cursor, "climate_state", 13) == 0) domain = kVehicleDataClimate;
        else if (length == 11 && memcmp(cursor, "drive_state", 11) == 0) domain = kVehicleDataDrive;
        if (!domain || (selected & domain)) return VehicleDataEndpoints::unsupported();
        selected |= domain;
        if (!delimiter) return VehicleDataEndpoints::of(selected);
        cursor = end + delimiter;
    }
}

// Parse the query string only, with exact key boundaries and no allocation. Omission selects
// charge_state + climate_state (TeslaBleHttpProxy default); drive_state is opt-in only.
inline VehicleDataEndpoints parse_vehicle_data_endpoints(std::string_view query) noexcept {
    constexpr unsigned kDefault = kVehicleDataCharge | kVehicleDataClimate;
    if (query.size() >= 128) return VehicleDataEndpoints::unsupported();
    if (query.empty()) return VehicleDataEndpoints::of(kDefault);
    const char* value = nullptr;
    const char* value_end = nullptr;
    const char* query_end = query.data() + query.size();
    for (const char* item = query.data(); item < query_end;) {
        const char* end = item;
        while (end < query_end && *end != '&') ++end;
        const char* eq = item;
        while (eq < end && *eq != '=') ++eq;
        if (eq - item == 9 && memcmp(item, "endpoints", 9) == 0) {
            if (value || eq == end) return VehicleDataEndpoints::unsupported();
            value = eq + 1;
            value_end = end;
        }
        item = end < query_end ? end + 1 : query_end;
    }
    return value ? parse_vehicle_data_endpoint_list({value, static_cast<size_t>(value_end - value)})
                 : VehicleDataEndpoints::of(kDefault);
}

// Success requires every selected domain; one valid cache must not mask another's failure.
inline bool vehicle_data_available(const VehicleDataEndpoints& endpoints,
                                   bool charge_ok, bool climate_ok, bool drive_ok) noexcept {
    return !endpoints.empty() && !endpoints.has_unsupported &&
           (!endpoints.charge_state || charge_ok) && (!endpoints.climate_state || climate_ok) &&
           (!endpoints.drive_state || drive_ok);
}

// Usable battery level with fallback to nominal battery level when tag 115 is omitted.
inline float effective_usable_soc(const ChargeStateResult& cs) {
    return cs.has_usable_battery_level ? cs.usable_battery_level : cs.battery_level;
}

// Pure decision logic for publishing pending telemetry against an identity epoch.
// A telemetry snapshot taken before pairing cleanup must not outlive the cleanup and revive
// stale readings into active caches.
inline bool telemetry_epoch_matches(uint32_t captured_epoch, uint32_t current_epoch) noexcept {
    return captured_epoch == current_epoch;
}

}  // namespace tk
