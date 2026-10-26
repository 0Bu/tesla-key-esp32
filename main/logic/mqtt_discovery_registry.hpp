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

namespace discovery_detail {

// Human-readable source rows are evaluated only while constructing the compact catalog.
inline constexpr std::array<DiscoveryEntry, kDiscoveryEntryCount> kSourceEntries{{
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
    {StateDomain::Device, DiscoveryComponent::Sensor, "wifi_reconn", "Network reconnects",
     "wifi_reconnects", JsonValueKind::Number, "", "", "total_increasing",
     "diagnostic", false},
    {StateDomain::Device, DiscoveryComponent::Sensor, "mqtt_reconn", "MQTT reconnects",
     "mqtt_reconnects", JsonValueKind::Number, "", "", "total_increasing",
     "diagnostic", false},
}};

// All runtime storage is readonly. IDs, offsets and flags replace repeated pointer/length
// pairs; consumers still receive the original DiscoveryEntry value without heap allocation.
inline constexpr unsigned kStringFields = 7;
struct SourceCatalog {
    std::array<std::string_view, kDiscoveryEntryCount * kStringFields> strings{};
    std::array<std::array<uint8_t, kStringFields>, kDiscoveryEntryCount> ids{};
    size_t count{0};
    size_t bytes{0};
};
constexpr std::array<std::string_view, kStringFields> string_fields(
        const DiscoveryEntry& entry) noexcept {
    return {{entry.object_id, entry.name, entry.field, entry.device_class, entry.unit,
             entry.state_class, entry.entity_category}};
}
constexpr SourceCatalog source_catalog() noexcept {
    SourceCatalog catalog{};
    for (size_t row = 0; row < kSourceEntries.size(); ++row) {
        const auto fields = string_fields(kSourceEntries[row]);
        for (unsigned field = 0; field < fields.size(); ++field) {
            size_t id = 0;
            while (id < catalog.count && catalog.strings[id] != fields[field]) ++id;
            if (id == catalog.count) {
                catalog.strings[catalog.count++] = fields[field];
                catalog.bytes += fields[field].size() + 1;
            }
            catalog.ids[row][field] = static_cast<uint8_t>(id);
        }
    }
    return catalog;
}
inline constexpr auto kSourceCatalog = source_catalog();
static_assert(kSourceCatalog.count > 0 && kSourceCatalog.count <= 256);
static_assert(kSourceCatalog.bytes < 65536);
struct PackedEntry {
    std::array<uint8_t, kStringFields> strings{};
    uint8_t flags{0};
};
static_assert(sizeof(PackedEntry) == 8);

template <size_t Strings, size_t Bytes>
struct PackedCatalog {
    std::array<PackedEntry, kDiscoveryEntryCount> rows{};
    std::array<uint16_t, Strings + 1> offsets{};
    std::array<char, Bytes> text{};

    constexpr bool valid() const noexcept {
        if (offsets.front() != 0 || offsets.back() != Bytes) return false;
        for (size_t id = 0; id < Strings; ++id) {
            if (offsets[id] >= offsets[id + 1] || offsets[id + 1] > Bytes ||
                text[offsets[id + 1] - 1] != '\0') return false;
        }
        for (const auto& row : rows) {
            if ((row.flags & 0x80u) || (row.flags & 7u) >= kStateDomainCount ||
                ((row.flags >> 4) & 3u) > static_cast<unsigned>(JsonValueKind::Boolean))
                return false;
            for (const auto id : row.strings) if (id >= Strings) return false;
        }
        return true;
    }
    constexpr std::string_view string(unsigned id) const noexcept {
        if (id >= Strings) return {};
        const size_t begin = offsets[id], end = offsets[id + 1];
        if (begin >= end || end > Bytes || text[end - 1] != '\0') return {};
        return {text.data() + begin, end - begin - 1};
    }
    constexpr DiscoveryEntry entry(size_t index) const noexcept {
        if (index >= rows.size()) return {};
        const auto& row = rows[index];
        const auto flags = row.flags;
        if ((flags & 0x80u) || (flags & 7u) >= kStateDomainCount ||
            ((flags >> 4) & 3u) > static_cast<unsigned>(JsonValueKind::Boolean)) return {};
        return {static_cast<StateDomain>(flags & 7u),
                static_cast<DiscoveryComponent>((flags >> 3) & 1u),
                string(row.strings[0]), string(row.strings[1]), string(row.strings[2]),
                static_cast<JsonValueKind>((flags >> 4) & 3u),
                string(row.strings[3]), string(row.strings[4]), string(row.strings[5]),
                string(row.strings[6]), (flags & 0x40u) != 0};
    }
};
template <size_t Strings, size_t Bytes>
constexpr PackedCatalog<Strings, Bytes> pack_catalog() noexcept {
    PackedCatalog<Strings, Bytes> packed{};
    size_t offset = 0;
    for (size_t id = 0; id < Strings; ++id) {
        packed.offsets[id] = static_cast<uint16_t>(offset);
        for (const char byte : kSourceCatalog.strings[id]) packed.text[offset++] = byte;
        packed.text[offset++] = '\0';
    }
    packed.offsets[Strings] = static_cast<uint16_t>(offset);
    for (size_t i = 0; i < kSourceEntries.size(); ++i) {
        const auto& entry = kSourceEntries[i];
        packed.rows[i].strings = kSourceCatalog.ids[i];
        packed.rows[i].flags = static_cast<uint8_t>(
            static_cast<unsigned>(entry.domain) |
            (static_cast<unsigned>(entry.component) << 3) |
            (static_cast<unsigned>(entry.value_kind) << 4) |
            (entry.invert ? 0x40u : 0u));
    }
    return packed;
}
inline constexpr auto kPackedCatalog =
    pack_catalog<kSourceCatalog.count, kSourceCatalog.bytes>();
static_assert(kPackedCatalog.valid());
static_assert(sizeof(kPackedCatalog) < kDiscoveryEntryCount * sizeof(DiscoveryEntry));
}  // namespace discovery_detail

// Immutable view: each dereference returns one decoded value, never a reference to a temporary
// or a second expanded global registry. Callers keeping an entry must keep that value.
struct DiscoveryRegistry {
    constexpr size_t size() const noexcept { return kDiscoveryEntryCount; }
    constexpr const auto& packed_rows() const noexcept {
        return discovery_detail::kPackedCatalog.rows;
    }
    constexpr DiscoveryEntry operator[](size_t index) const noexcept {
        return discovery_detail::kPackedCatalog.entry(index);
    }
    constexpr DiscoveryEntry front() const noexcept { return (*this)[0]; }
    struct Iterator {
        size_t index;
        constexpr DiscoveryEntry operator*() const noexcept {
            return discovery_detail::kPackedCatalog.entry(index);
        }
        constexpr Iterator& operator++() noexcept { ++index; return *this; }
        constexpr bool operator!=(Iterator other) const noexcept { return index != other.index; }
    };
    constexpr Iterator begin() const noexcept { return {0}; }
    constexpr Iterator end() const noexcept { return {size()}; }
};
inline constexpr DiscoveryRegistry kDiscoveryEntries{};

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

// Typed field access lets the publisher use a const packed-row reference, avoiding a
// full decoded DiscoveryEntry temporary in its already bounded serializer frame.
enum class DiscoveryTextField : uint8_t {
    ObjectId, Name, Field, DeviceClass, Unit, StateClass, EntityCategory,
};
inline constexpr std::string_view discovery_text(const DiscoveryEntry& entry,
        DiscoveryTextField field) noexcept {
    switch (field) {
        case DiscoveryTextField::ObjectId: return entry.object_id;
        case DiscoveryTextField::Name: return entry.name;
        case DiscoveryTextField::Field: return entry.field;
        case DiscoveryTextField::DeviceClass: return entry.device_class;
        case DiscoveryTextField::Unit: return entry.unit;
        case DiscoveryTextField::StateClass: return entry.state_class;
        case DiscoveryTextField::EntityCategory: return entry.entity_category;
    }
    return {};
}
inline constexpr std::string_view discovery_text(const discovery_detail::PackedEntry& entry,
        DiscoveryTextField field) noexcept {
    const auto column = static_cast<unsigned>(field);
    return column < discovery_detail::kStringFields
        ? discovery_detail::kPackedCatalog.string(entry.strings[column]) : std::string_view{};
}
inline constexpr DiscoveryComponent discovery_component(const DiscoveryEntry& entry) noexcept {
    return entry.component;
}
inline constexpr DiscoveryComponent discovery_component(
        const discovery_detail::PackedEntry& entry) noexcept {
    return static_cast<DiscoveryComponent>((entry.flags >> 3) & 1u);
}
inline constexpr StateDomain discovery_domain(const DiscoveryEntry& entry) noexcept {
    return entry.domain;
}
inline constexpr StateDomain discovery_domain(const discovery_detail::PackedEntry& entry) noexcept {
    return static_cast<StateDomain>(entry.flags & 7u);
}
inline constexpr bool discovery_inverted(const DiscoveryEntry& entry) noexcept {
    return entry.invert;
}
inline constexpr bool discovery_inverted(const discovery_detail::PackedEntry& entry) noexcept {
    return (entry.flags & 0x40u) != 0;
}
template <class Entry>
inline constexpr bool discovery_is_binary(const Entry& entry) noexcept {
    return discovery_component(entry) == DiscoveryComponent::BinarySensor;
}
template <class Entry>
inline const char* discovery_optional_text(const Entry& entry, DiscoveryTextField field) noexcept {
    const auto text = discovery_text(entry, field);
    return text.empty() ? nullptr : text.data();
}

template <class Registry>
inline constexpr bool discovery_registry_size_valid(
    const Registry& entries) noexcept {
    return entries.size() == kDiscoveryEntryCount;
}

template <class Registry>
inline constexpr bool discovery_registry_object_ids_unique(
    const Registry& entries) noexcept {
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].object_id.empty()) return false;
        for (size_t j = i + 1; j < entries.size(); ++j) {
            if (entries[i].object_id == entries[j].object_id) return false;
        }
    }
    return true;
}

template <class Registry>
inline constexpr bool discovery_registry_shape_valid(
    const Registry& entries) noexcept {
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

template <class Entry>
inline std::string discovery_unique_id(std::string_view node,
                                       const Entry& entry) {
    const auto object_id = discovery_text(entry, DiscoveryTextField::ObjectId);
    std::string result;
    result.reserve(node.size() + 1 + object_id.size());
    result.append(node.data(), node.size());
    result.push_back('_');
    result.append(object_id.data(), object_id.size());
    return result;
}

template <class Entry>
inline std::string discovery_config_topic(std::string_view prefix, std::string_view node,
                                          const Entry& entry) {
    const std::string_view component = discovery_component_name(discovery_component(entry));
    const auto object_id = discovery_text(entry, DiscoveryTextField::ObjectId);
    std::string result;
    result.reserve(prefix.size() + component.size() + node.size() + object_id.size() + 10);
    result.append(prefix.data(), prefix.size());
    result.push_back('/');
    result.append(component.data(), component.size());
    result.push_back('/');
    result.append(node.data(), node.size());
    result.push_back('/');
    result.append(object_id.data(), object_id.size());
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

template <class Entry>
inline std::string discovery_value_template(const Entry& entry) {
    const auto field = discovery_text(entry, DiscoveryTextField::Field);
    if (discovery_is_binary(entry)) {
        return ha_binary_value_template(field.data(), discovery_inverted(entry));
    }
    return ha_value_template(std::string(field).c_str());
}

}  // namespace tk::mqtt
