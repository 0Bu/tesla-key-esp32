#pragma once

#include "json_builder.hpp"

#include <cstdint>

namespace tk::mqtt {

inline bool present(const char* value) noexcept { return value && value[0] != '\0'; }

struct DiscoveryPayload {
    const char* name{};
    const char* unique_id{};
    const char* state_topic{};
    const char* availability_topic{};
    const char* value_template{};
    const char* device_class{};
    const char* unit{};
    const char* state_class{};
    const char* entity_category{};
    bool binary{};
    const char* node{};
    const char* device_name{};
    const char* platform{};
    const char* version{};
    const char* configuration_url{};
};

inline JsonOwner build_discovery_payload(const DiscoveryPayload& in) noexcept {
    if (!in.name || !in.unique_id || !in.state_topic || !in.availability_topic ||
        !in.value_template || !in.node || !in.device_name || !in.platform || !in.version) {
        return {};
    }

    JsonBuilder json;
    cJSON* root = json.root();
    json.string(root, "name", in.name);
    json.string(root, "uniq_id", in.unique_id);
    json.string(root, "stat_t", in.state_topic);
    json.string(root, "avty_t", in.availability_topic);
    json.string(root, "val_tpl", in.value_template);
    if (in.binary) {
        json.string(root, "pl_on", "ON");
        json.string(root, "pl_off", "OFF");
    }
    if (in.device_class) json.string(root, "dev_cla", in.device_class);
    if (in.unit) json.string(root, "unit_of_meas", in.unit);
    if (in.state_class) json.string(root, "stat_cla", in.state_class);
    if (in.entity_category) json.string(root, "ent_cat", in.entity_category);

    cJSON* origin = json.object(root, "o");
    json.string(origin, "name", "tesla-key-esp32");
    json.string(origin, "sw", in.version);
    json.string(origin, "url", "https://github.com/0Bu/tesla-key-esp32");

    cJSON* device = json.object(root, "dev");
    cJSON* ids = json.array(device, "ids");
    json.string_element(ids, in.node);
    json.string(device, "name", in.device_name);
    json.string(device, "mf", "tesla-key-esp32");
    json.string(device, "mdl", in.platform);
    json.string(device, "sw", in.version);
    if (present(in.configuration_url)) json.string(device, "cu", in.configuration_url);
    return json.finish();
}

struct ChargePayload {
    bool has_battery_level{};
    double battery_level{};
    bool has_charge_limit{};
    double charge_limit{};
    bool has_power{};
    double power{};
    bool has_amps{};
    double amps{};
    bool has_range{};
    double range_km{};
    bool has_rate{};
    double rate_kmh{};
    const char* charging_state{};
    bool has_actual_current{};
    double actual_current{};
    bool has_voltage{};
    double voltage{};
    bool has_current_request{};
    double current_request{};
    bool has_phases{};
    double phases{};
    bool has_energy_added{};
    double energy_added{};
    bool has_minutes_to_full{};
    double minutes_to_full{};
    const char* limit_reason{};
};

inline JsonOwner build_charge_payload(const ChargePayload& in) noexcept {
    JsonBuilder json;
    cJSON* root = json.root();
    if (in.has_battery_level) json.number(root, "soc", in.battery_level);
    if (in.has_charge_limit) json.number(root, "charge_limit", in.charge_limit);
    if (in.has_power) json.number(root, "power", in.power);
    if (in.has_amps) json.number(root, "amps", in.amps);
    if (in.has_range) json.number(root, "range", in.range_km);
    if (in.has_rate) json.number(root, "rate", in.rate_kmh);
    if (present(in.charging_state)) json.string(root, "charging_state", in.charging_state);
    if (in.has_actual_current) json.number(root, "actual_current", in.actual_current);
    if (in.has_voltage) json.number(root, "volts", in.voltage);
    if (in.has_current_request) json.number(root, "current_request", in.current_request);
    if (in.has_phases) json.number(root, "phases", in.phases);
    if (in.has_energy_added) json.number(root, "energy_added", in.energy_added);
    if (in.has_minutes_to_full) json.number(root, "minutes_to_full", in.minutes_to_full);
    if (present(in.limit_reason)) json.string(root, "limit_reason", in.limit_reason);
    return json.finish();
}

struct ClimatePayload {
    bool has_inside{};
    double inside{};
    bool has_outside{};
    double outside{};
    bool has_setpoint{};
    double setpoint{};
    bool has_climate_on{};
    bool climate_on{};
    bool has_preconditioning{};
    bool preconditioning{};
    bool has_cop{};
    const char* cop{};
    bool has_cop_cooling{};
    bool cop_cooling{};
    bool has_cop_temp{};
    const char* cop_temp{};
    bool has_cop_reason{};
    const char* cop_reason{};
    bool has_front_defrost{};
    bool front_defrost{};
    bool has_rear_defrost{};
    bool rear_defrost{};
    bool has_defrost_mode{};
    const char* defrost_mode{};
};

inline JsonOwner build_climate_payload(const ClimatePayload& in) noexcept {
    if ((in.has_cop && !in.cop) || (in.has_cop_temp && !in.cop_temp) ||
        (in.has_cop_reason && !in.cop_reason) ||
        (in.has_defrost_mode && !in.defrost_mode)) {
        return {};
    }
    JsonBuilder json;
    cJSON* root = json.root();
    if (in.has_inside) json.number(root, "inside", in.inside);
    if (in.has_outside) json.number(root, "outside", in.outside);
    if (in.has_setpoint) json.number(root, "setpoint", in.setpoint);
    if (in.has_climate_on) json.boolean(root, "on", in.climate_on);
    if (in.has_preconditioning) json.boolean(root, "preconditioning", in.preconditioning);
    if (in.has_cop) json.string(root, "cop", in.cop);
    if (in.has_cop_cooling) json.boolean(root, "cop_cooling", in.cop_cooling);
    if (in.has_cop_temp) json.string(root, "cop_temp", in.cop_temp);
    if (in.has_cop_reason) json.string(root, "cop_reason", in.cop_reason);
    if (in.has_front_defrost) json.boolean(root, "front_defrost", in.front_defrost);
    if (in.has_rear_defrost) json.boolean(root, "rear_defrost", in.rear_defrost);
    if (in.has_defrost_mode) json.string(root, "defrost_mode", in.defrost_mode);
    return json.finish();
}

struct DrivePayload {
    const char* shift{};
    bool has_odometer{};
    double odometer{};
};

inline JsonOwner build_drive_payload(const DrivePayload& in) noexcept {
    JsonBuilder json;
    if (present(in.shift)) json.string(json.root(), "shift", in.shift);
    if (in.has_odometer) json.number(json.root(), "odometer", in.odometer);
    return json.finish();
}

struct TiresPayload {
    bool has_fl{};
    double fl{};
    bool has_fr{};
    double fr{};
    bool has_rl{};
    double rl{};
    bool has_rr{};
    double rr{};
    bool warn{};
};

inline JsonOwner build_tires_payload(const TiresPayload& in) noexcept {
    JsonBuilder json;
    if (in.has_fl) json.number(json.root(), "fl", in.fl);
    if (in.has_fr) json.number(json.root(), "fr", in.fr);
    if (in.has_rl) json.number(json.root(), "rl", in.rl);
    if (in.has_rr) json.number(json.root(), "rr", in.rr);
    json.boolean(json.root(), "warn", in.warn);
    return json.finish();
}

struct ClosuresPayload {
    bool has_locked{};
    bool locked{};
    bool door{};
    bool frunk{};
    bool trunk{};
    bool window{};
    bool has_user{};
    bool user{};
};

inline JsonOwner build_closures_payload(const ClosuresPayload& in) noexcept {
    JsonBuilder json;
    if (in.has_locked) json.boolean(json.root(), "locked", in.locked);
    json.boolean(json.root(), "door", in.door);
    json.boolean(json.root(), "frunk", in.frunk);
    json.boolean(json.root(), "trunk", in.trunk);
    json.boolean(json.root(), "window", in.window);
    if (in.has_user) json.boolean(json.root(), "user", in.user);
    return json.finish();
}

inline JsonOwner build_vehicle_payload(const char* sleep_status) noexcept {
    if (!sleep_status) return {};
    JsonBuilder json;
    json.string(json.root(), "sleep_status", sleep_status);
    return json.finish();
}

struct DevicePayload {
    bool has_wifi_rssi{};
    double wifi_rssi{};
    bool ble_connected{};
    bool has_ble_rssi{};
    double ble_rssi{};
    bool paired{};
    const char* boot_time{};
    double free_heap{};
    const char* version{};
    double largest_block{};
    double min_free_heap{};
    const char* reset_reason{};
    double reset_reason_code{};
    bool crash_dump{};
    bool safe_mode{};
    double wifi_reconnects{};
    double mqtt_reconnects{};
    bool has_httpd_stack{};
    double httpd_stack{};
    bool has_vehicle_stack{};
    double vehicle_stack{};
    bool has_auto_pair_stack{};
    double auto_pair_stack{};
    bool has_mqtt_stack{};
    double mqtt_stack{};
};

inline JsonOwner build_device_payload(const DevicePayload& in) noexcept {
    if (!in.version || !in.reset_reason) return {};
    JsonBuilder json;
    cJSON* root = json.root();
    if (in.has_wifi_rssi) json.number(root, "wifi_rssi", in.wifi_rssi);
    json.boolean(root, "ble_connected", in.ble_connected);
    if (in.has_ble_rssi) json.number(root, "ble_rssi", in.ble_rssi);
    json.boolean(root, "paired", in.paired);
    if (present(in.boot_time)) json.string(root, "boot_time", in.boot_time);
    json.number(root, "free_heap", in.free_heap);
    json.string(root, "version", in.version);
    json.number(root, "largest_block", in.largest_block);
    json.number(root, "min_free_heap", in.min_free_heap);
    json.string(root, "reset_reason", in.reset_reason);
    json.number(root, "reset_reason_code", in.reset_reason_code);
    json.boolean(root, "crash_dump", in.crash_dump);
    json.boolean(root, "safe_mode", in.safe_mode);
    json.number(root, "wifi_reconnects", in.wifi_reconnects);
    json.number(root, "mqtt_reconnects", in.mqtt_reconnects);
    if (in.has_httpd_stack)
        json.number(root, "httpd_stack_min_free_bytes", in.httpd_stack);
    if (in.has_vehicle_stack)
        json.number(root, "vehicle_stack_min_free_bytes", in.vehicle_stack);
    if (in.has_auto_pair_stack)
        json.number(root, "auto_pair_stack_min_free_bytes", in.auto_pair_stack);
    if (in.has_mqtt_stack)
        json.number(root, "mqtt_stack_min_free_bytes", in.mqtt_stack);
    return json.finish();
}

}  // namespace tk::mqtt
