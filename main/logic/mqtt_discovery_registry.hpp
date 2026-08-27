#pragma once

#include "ha_templates.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Pure, hardware-free Home Assistant discovery registry shared by mqtt_ha.cpp and the
// pinned-cJSON host gate. Keep this file free of IDF/FreeRTOS/NimBLE/NVS/cJSON includes.
// Single source of truth: production derives component/config/unique/state topics and value
// templates from these rows; tests bind every row back to the corresponding state-payload field.
namespace tk::mqtt {

enum class StateDomain : uint8_t {
    Charge,
    Climate,
    Drive,
    Tires,
    Closures,
    Vehicle,
    Device,
    Count,
};

enum class DiscoveryComponent : uint8_t { Sensor, BinarySensor };
enum class JsonValueKind : uint8_t { Number, String, Boolean };

inline constexpr size_t kStateDomainCount = static_cast<size_t>(StateDomain::Count);
inline constexpr size_t kDiscoveryEntryCount = 55;

struct DiscoveryEntry {
    StateDomain domain;
    DiscoveryComponent component;
    std::string_view object_id;
    std::string_view name;
    std::string_view field;
    JsonValueKind value_kind;
    std::string_view device_class;
    std::string_view unit;
    std::string_view state_class;
    std::string_view entity_category;
    bool invert;
};

inline constexpr std::array<std::string_view, kStateDomainCount> kStateTopicSuffixes{{
    "charge", "climate", "drive", "tires", "closures", "vehicle", "device",
}};

inline constexpr std::array<DiscoveryEntry, kDiscoveryEntryCount> kDiscoveryEntries{{
    // Charge
    {StateDomain::Charge, DiscoveryComponent::Sensor, "soc", "Battery", "soc",
     JsonValueKind::Number, "battery", "%", "measurement", "", false},
    {StateDomain::Charge, DiscoveryComponent::Sensor, "charge_limit", "Charge limit",
     "charge_limit", JsonValueKind::Number, "battery", "%", "measurement", "", false},
    {StateDomain::Charge, DiscoveryComponent::Sensor, "charger_power", "Charger power",
     "power", JsonValueKind::Number, "power", "kW", "measurement", "", false},
    {StateDomain::Charge, DiscoveryComponent::Sensor, "charging_amps", "Charging current",
     "amps", JsonValueKind::Number, "current", "A", "measurement", "", false},
    {StateDomain::Charge, DiscoveryComponent::Sensor, "range", "Range", "range",
     JsonValueKind::Number, "distance", "km", "measurement", "", false},
    {StateDomain::Charge, DiscoveryComponent::Sensor, "charge_rate", "Charge rate", "rate",
     JsonValueKind::Number, "", "km/h", "measurement", "", false},
    {StateDomain::Charge, DiscoveryComponent::Sensor, "charging_state", "Charging state",
     "charging_state", JsonValueKind::String, "", "", "", "", false},
    {StateDomain::Charge, DiscoveryComponent::Sensor, "actual_current",
     "Charging current (actual)", "actual_current", JsonValueKind::Number, "current", "A",
     "measurement", "", false},
    {StateDomain::Charge, DiscoveryComponent::Sensor, "current_request",
     "Charging current (requested)", "current_request", JsonValueKind::Number, "current", "A",
     "measurement", "", false},
    {StateDomain::Charge, DiscoveryComponent::Sensor, "charger_voltage", "Charger voltage",
     "volts", JsonValueKind::Number, "voltage", "V", "measurement", "", false},
    {StateDomain::Charge, DiscoveryComponent::Sensor, "phases", "Charger phases", "phases",
     JsonValueKind::Number, "", "", "measurement", "", false},
    {StateDomain::Charge, DiscoveryComponent::Sensor, "energy_added", "Energy added",
     "energy_added", JsonValueKind::Number, "energy", "kWh", "total_increasing", "", false},
    {StateDomain::Charge, DiscoveryComponent::Sensor, "minutes_to_full", "Time to full",
     "minutes_to_full", JsonValueKind::Number, "duration", "min", "measurement", "", false},
    {StateDomain::Charge, DiscoveryComponent::Sensor, "limit_reason", "Charge limit reason",
     "limit_reason", JsonValueKind::String, "", "", "", "diagnostic", false},

    // Climate
    {StateDomain::Climate, DiscoveryComponent::Sensor, "inside_temp", "Inside temperature",
     "inside", JsonValueKind::Number, "temperature", "°C", "measurement", "", false},
    {StateDomain::Climate, DiscoveryComponent::Sensor, "outside_temp", "Outside temperature",
     "outside", JsonValueKind::Number, "temperature", "°C", "measurement", "", false},
    {StateDomain::Climate, DiscoveryComponent::Sensor, "setpoint", "Climate setpoint", "setpoint",
     JsonValueKind::Number, "temperature", "°C", "measurement", "", false},
    {StateDomain::Climate, DiscoveryComponent::BinarySensor, "climate_on", "Climate", "on",
     JsonValueKind::Boolean, "running", "", "", "", false},
    {StateDomain::Climate, DiscoveryComponent::BinarySensor, "preconditioning", "Preconditioning",
     "preconditioning", JsonValueKind::Boolean, "running", "", "", "", false},
    {StateDomain::Climate, DiscoveryComponent::BinarySensor, "cop_cooling",
     "Overheat protection cooling", "cop_cooling", JsonValueKind::Boolean, "running", "", "",
     "", false},
    {StateDomain::Climate, DiscoveryComponent::Sensor, "cop", "Overheat protection", "cop",
     JsonValueKind::String, "", "", "", "diagnostic", false},
    {StateDomain::Climate, DiscoveryComponent::Sensor, "cop_temp", "Overheat threshold",
     "cop_temp", JsonValueKind::String, "", "", "", "diagnostic", false},
    {StateDomain::Climate, DiscoveryComponent::Sensor, "cop_reason",
     "Overheat protection reason", "cop_reason", JsonValueKind::String, "", "", "",
     "diagnostic", false},
    {StateDomain::Climate, DiscoveryComponent::BinarySensor, "front_defrost", "Front defroster",
     "front_defrost", JsonValueKind::Boolean, "running", "", "", "", false},
    {StateDomain::Climate, DiscoveryComponent::BinarySensor, "rear_defrost", "Rear defroster",
     "rear_defrost", JsonValueKind::Boolean, "running", "", "", "", false},
    {StateDomain::Climate, DiscoveryComponent::Sensor, "defrost_mode", "Defrost mode",
     "defrost_mode", JsonValueKind::String, "", "", "", "diagnostic", false},

    // Drive
    {StateDomain::Drive, DiscoveryComponent::Sensor, "shift_state", "Shift state", "shift",
     JsonValueKind::String, "", "", "", "", false},
    {StateDomain::Drive, DiscoveryComponent::Sensor, "odometer", "Odometer", "odometer",
     JsonValueKind::Number, "distance", "km", "total_increasing", "", false},

    // Tires
    {StateDomain::Tires, DiscoveryComponent::Sensor, "tire_fl", "Tire front left", "fl",
     JsonValueKind::Number, "pressure", "bar", "measurement", "", false},
    {StateDomain::Tires, DiscoveryComponent::Sensor, "tire_fr", "Tire front right", "fr",
     JsonValueKind::Number, "pressure", "bar", "measurement", "", false},
    {StateDomain::Tires, DiscoveryComponent::Sensor, "tire_rl", "Tire rear left", "rl",
     JsonValueKind::Number, "pressure", "bar", "measurement", "", false},
    {StateDomain::Tires, DiscoveryComponent::Sensor, "tire_rr", "Tire rear right", "rr",
     JsonValueKind::Number, "pressure", "bar", "measurement", "", false},
    {StateDomain::Tires, DiscoveryComponent::BinarySensor, "tire_warn", "Tire pressure warning",
     "warn", JsonValueKind::Boolean, "problem", "", "", "", false},

    // Closures
    {StateDomain::Closures, DiscoveryComponent::BinarySensor, "locked", "Locked", "locked",
     JsonValueKind::Boolean, "lock", "", "", "", true},
    {StateDomain::Closures, DiscoveryComponent::BinarySensor, "door_open", "Doors", "door",
     JsonValueKind::Boolean, "door", "", "", "", false},
    {StateDomain::Closures, DiscoveryComponent::BinarySensor, "frunk_open", "Frunk", "frunk",
     JsonValueKind::Boolean, "opening", "", "", "", false},
    {StateDomain::Closures, DiscoveryComponent::BinarySensor, "trunk_open", "Trunk", "trunk",
     JsonValueKind::Boolean, "opening", "", "", "", false},
    {StateDomain::Closures, DiscoveryComponent::BinarySensor, "window_open", "Windows", "window",
     JsonValueKind::Boolean, "window", "", "", "", false},
    {StateDomain::Closures, DiscoveryComponent::BinarySensor, "occupant", "Occupant", "user",
     JsonValueKind::Boolean, "occupancy", "", "", "", false},

    // Vehicle
    {StateDomain::Vehicle, DiscoveryComponent::Sensor, "sleep_state", "Sleep state",
     "sleep_status", JsonValueKind::String, "", "", "", "diagnostic", false},

    // Device diagnostics
    {StateDomain::Device, DiscoveryComponent::Sensor, "wifi_rssi", "WiFi signal", "wifi_rssi",
     JsonValueKind::Number, "signal_strength", "dBm", "measurement", "diagnostic", false},
    {StateDomain::Device, DiscoveryComponent::Sensor, "ble_rssi", "BLE signal", "ble_rssi",
     JsonValueKind::Number, "signal_strength", "dBm", "measurement", "diagnostic", false},
    {StateDomain::Device, DiscoveryComponent::BinarySensor, "ble_link", "BLE link",
     "ble_connected", JsonValueKind::Boolean, "connectivity", "", "", "diagnostic", false},
    {StateDomain::Device, DiscoveryComponent::BinarySensor, "paired", "Paired", "paired",
     JsonValueKind::Boolean, "connectivity", "", "", "diagnostic", false},
    {StateDomain::Device, DiscoveryComponent::Sensor, "uptime", "Last boot", "boot_time",
     JsonValueKind::String, "timestamp", "", "", "diagnostic", false},
    {StateDomain::Device, DiscoveryComponent::Sensor, "free_heap", "Free heap", "free_heap",
     JsonValueKind::Number, "data_size", "B", "measurement", "diagnostic", false},
    {StateDomain::Device, DiscoveryComponent::Sensor, "firmware", "Firmware", "version",
     JsonValueKind::String, "", "", "", "diagnostic", false},
    {StateDomain::Device, DiscoveryComponent::Sensor, "reset_reason", "Reset reason",
     "reset_reason", JsonValueKind::String, "", "", "", "diagnostic", false},
    {StateDomain::Device, DiscoveryComponent::Sensor, "reset_code", "Reset reason code",
     "reset_reason_code", JsonValueKind::Number, "", "", "measurement", "diagnostic", false},
    {StateDomain::Device, DiscoveryComponent::BinarySensor, "safe_mode", "Safe mode", "safe_mode",
     JsonValueKind::Boolean, "problem", "", "", "diagnostic", false},
    {StateDomain::Device, DiscoveryComponent::BinarySensor, "crash_dump", "Crash dump waiting",
     "crash_dump", JsonValueKind::Boolean, "problem", "", "", "diagnostic", false},
    {StateDomain::Device, DiscoveryComponent::Sensor, "largest_block", "Largest free block",
     "largest_block", JsonValueKind::Number, "data_size", "B", "measurement", "diagnostic",
     false},
    {StateDomain::Device, DiscoveryComponent::Sensor, "min_free_heap", "Min free heap",
     "min_free_heap", JsonValueKind::Number, "data_size", "B", "measurement", "diagnostic",
     false},
    {StateDomain::Device, DiscoveryComponent::Sensor, "wifi_reconn", "WiFi reconnects",
     "wifi_reconnects", JsonValueKind::Number, "data_size", "", "total_increasing",
     "diagnostic", false},
    {StateDomain::Device, DiscoveryComponent::Sensor, "mqtt_reconn", "MQTT reconnects",
     "mqtt_reconnects", JsonValueKind::Number, "data_size", "", "total_increasing",
     "diagnostic", false},
}};

static_assert(kDiscoveryEntries.size() == kDiscoveryEntryCount,
              "Home Assistant discovery registry must contain exactly 55 entities");

inline constexpr size_t state_domain_index(StateDomain domain) noexcept {
    return static_cast<size_t>(domain);
}

inline constexpr std::string_view state_topic_suffix(StateDomain domain) noexcept {
    const size_t index = state_domain_index(domain);
    return index < kStateTopicSuffixes.size() ? kStateTopicSuffixes[index] : std::string_view{};
}

inline constexpr std::string_view discovery_component_name(
    DiscoveryComponent component) noexcept {
    return component == DiscoveryComponent::BinarySensor ? "binary_sensor" : "sensor";
}

inline constexpr bool discovery_is_binary(const DiscoveryEntry& entry) noexcept {
    return entry.component == DiscoveryComponent::BinarySensor;
}

template <size_t N>
inline constexpr bool discovery_registry_size_valid(
    const std::array<DiscoveryEntry, N>&) noexcept {
    return N == kDiscoveryEntryCount;
}

template <size_t N>
inline constexpr bool discovery_registry_object_ids_unique(
    const std::array<DiscoveryEntry, N>& entries) noexcept {
    for (size_t i = 0; i < N; ++i) {
        if (entries[i].object_id.empty()) return false;
        for (size_t j = i + 1; j < N; ++j) {
            if (entries[i].object_id == entries[j].object_id) return false;
        }
    }
    return true;
}

template <size_t N>
inline constexpr bool discovery_registry_shape_valid(
    const std::array<DiscoveryEntry, N>& entries) noexcept {
    if (!discovery_registry_size_valid(entries) ||
        !discovery_registry_object_ids_unique(entries)) {
        return false;
    }
    for (const DiscoveryEntry& entry : entries) {
        if (entry.name.empty() || entry.field.empty() ||
            state_domain_index(entry.domain) >= kStateDomainCount) {
            return false;
        }
        const bool boolean = entry.value_kind == JsonValueKind::Boolean;
        if (discovery_is_binary(entry) != boolean) return false;
        if (entry.invert && (!boolean || entry.device_class != "lock")) return false;
    }
    return true;
}

inline std::string discovery_unique_id(std::string_view node,
                                       const DiscoveryEntry& entry) {
    std::string result;
    result.reserve(node.size() + 1 + entry.object_id.size());
    result.append(node.data(), node.size());
    result.push_back('_');
    result.append(entry.object_id.data(), entry.object_id.size());
    return result;
}

inline std::string discovery_config_topic(std::string_view prefix, std::string_view node,
                                          const DiscoveryEntry& entry) {
    const std::string_view component = discovery_component_name(entry.component);
    std::string result;
    result.reserve(prefix.size() + component.size() + node.size() + entry.object_id.size() + 10);
    result.append(prefix.data(), prefix.size());
    result.push_back('/');
    result.append(component.data(), component.size());
    result.push_back('/');
    result.append(node.data(), node.size());
    result.push_back('/');
    result.append(entry.object_id.data(), entry.object_id.size());
    result.append("/config");
    return result;
}

inline std::string discovery_state_topic(std::string_view base, StateDomain domain) {
    const std::string_view suffix = state_topic_suffix(domain);
    if (suffix.empty()) return {};
    std::string result;
    result.reserve(base.size() + 1 + suffix.size());
    result.append(base.data(), base.size());
    result.push_back('/');
    result.append(suffix.data(), suffix.size());
    return result;
}

inline std::string discovery_value_template(const DiscoveryEntry& entry) {
    if (discovery_is_binary(entry)) {
        return ha_binary_value_template(entry.field.data(), entry.invert);
    }
    std::string result = "{{ value_json.";
    result.append(entry.field.data(), entry.field.size());
    result.append(" }}");
    return result;
}

}  // namespace tk::mqtt
