#include "mqtt_json_publish.hpp"
#include "mqtt_payloads.hpp"
#include "mqtt_publish_sequence.hpp"
#include "logic/mqtt_discovery_registry.hpp"

#include <cJSON.h>

#include <array>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

size_t checks = 0;
size_t allocation_attempt = 0;
size_t fail_at = 0;
size_t live_allocations = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        ++checks;                                                                \
        if (!(condition)) {                                                      \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": "         \
                      << #condition << '\n';                                     \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

void* tracked_malloc(size_t size) {
    ++allocation_attempt;
    if (fail_at != 0 && allocation_attempt == fail_at) return nullptr;
    void* value = std::malloc(size);
    if (value) ++live_allocations;
    return value;
}

void tracked_free(void* value) {
    if (!value) return;
    CHECK(live_allocations > 0);
    --live_allocations;
    std::free(value);
}

void reset_allocator(size_t failure) {
    CHECK(live_allocations == 0);
    allocation_attempt = 0;
    fail_at = failure;
}

struct PublishSpy {
    bool fail{false};
    bool throw_failure{false};
    size_t calls{0};
    std::string topic;
    std::string payload;
    bool retain{false};
    std::string retained_value{"old-retained-value"};

    bool operator()(const char* new_topic, const char* new_payload, bool new_retain) {
        ++calls;
        if (throw_failure) throw std::runtime_error("publish spy failure");
        if (fail) return false;
        topic = new_topic;
        payload = new_payload;
        retain = new_retain;
        if (new_retain) retained_value = new_payload;
        return true;
    }
};

const tk::mqtt::DiscoveryEntry& discovery_entry(std::string_view object_id) {
    for (const auto& entry : tk::mqtt::kDiscoveryEntries) {
        if (entry.object_id == object_id) return entry;
    }
    CHECK(false);
    return tk::mqtt::kDiscoveryEntries.front();
}

tk::JsonOwner build_discovery_sensor_full() {
    const auto& entry = discovery_entry("soc");
    const std::string unique_id = tk::mqtt::discovery_unique_id("teslakey_fixture", entry);
    const std::string state_topic =
        tk::mqtt::discovery_state_topic("tesla-key/teslakey_fixture", entry.domain);
    const std::string value_template = tk::mqtt::discovery_value_template(entry);
    const tk::mqtt::DiscoveryPayload in{
        entry.name.data(), unique_id.c_str(), state_topic.c_str(),
        "tesla-key/teslakey_fixture/availability", value_template.c_str(),
        entry.device_class.data(), entry.unit.data(), entry.state_class.data(), "diagnostic",
        tk::mqtt::discovery_is_binary(entry), "teslakey_fixture",
        "Tesla Key", "esp32s3", "0.0.0-test", "http://192.0.2.1",
    };
    return tk::mqtt::build_discovery_payload(in);
}

tk::JsonOwner build_discovery_binary_minimal() {
    const auto& entry = discovery_entry("door_open");
    const std::string unique_id = tk::mqtt::discovery_unique_id("teslakey_fixture", entry);
    const std::string state_topic =
        tk::mqtt::discovery_state_topic("tesla-key/teslakey_fixture", entry.domain);
    const std::string value_template = tk::mqtt::discovery_value_template(entry);
    const tk::mqtt::DiscoveryPayload in{
        entry.name.data(), unique_id.c_str(), state_topic.c_str(),
        "tesla-key/teslakey_fixture/availability", value_template.c_str(),
        entry.device_class.data(), nullptr, nullptr, nullptr,
        tk::mqtt::discovery_is_binary(entry), "teslakey_fixture", "Tesla Key", "esp32",
        "0.0.0-test", nullptr,
    };
    return tk::mqtt::build_discovery_payload(in);
}

tk::mqtt::ChargePayload charge_full_input() {
    tk::mqtt::ChargePayload in;
    in.has_battery_level = true; in.battery_level = 62;
    in.has_charge_limit = true; in.charge_limit = 80;
    in.has_power = true; in.power = 11;
    in.has_amps = true; in.amps = 16;
    in.has_range = true; in.range_km = 321;
    in.has_rate = true; in.rate_kmh = 48;
    in.charging_state = "Charging";
    in.has_actual_current = true; in.actual_current = 15;
    in.has_voltage = true; in.voltage = 230;
    in.has_current_request = true; in.current_request = 16;
    in.has_phases = true; in.phases = 3;
    in.has_energy_added = true; in.energy_added = 12;
    in.has_minutes_to_full = true; in.minutes_to_full = 45;
    in.limit_reason = "Standard";
    return in;
}

tk::JsonOwner build_charge_full() {
    return tk::mqtt::build_charge_payload(charge_full_input());
}
tk::JsonOwner build_charge_minimal() {
    return tk::mqtt::build_charge_payload({});
}

tk::mqtt::ClimatePayload climate_full_input() {
    tk::mqtt::ClimatePayload in;
    in.has_inside = true; in.inside = 21;
    in.has_outside = true; in.outside = -3;
    in.has_setpoint = true; in.setpoint = 22;
    in.has_climate_on = true; in.climate_on = true;
    in.has_preconditioning = true; in.preconditioning = false;
    in.has_cop = true; in.cop = "On";
    in.has_cop_cooling = true; in.cop_cooling = true;
    in.has_cop_temp = true; in.cop_temp = "High";
    in.has_cop_reason = true; in.cop_reason = "Cabin hot";
    in.has_front_defrost = true; in.front_defrost = true;
    in.has_rear_defrost = true; in.rear_defrost = false;
    in.has_defrost_mode = true; in.defrost_mode = "Max";
    return in;
}

tk::JsonOwner build_climate_full() {
    return tk::mqtt::build_climate_payload(climate_full_input());
}
tk::JsonOwner build_climate_minimal() {
    return tk::mqtt::build_climate_payload({});
}

tk::JsonOwner build_drive_full() {
    return tk::mqtt::build_drive_payload({"D", true, 12345});
}
tk::JsonOwner build_drive_minimal() {
    return tk::mqtt::build_drive_payload({});
}

tk::JsonOwner build_tires_full() {
    return tk::mqtt::build_tires_payload(
        {true, 3, true, 4, true, 5, true, 6, true});
}
tk::JsonOwner build_tires_minimal() {
    return tk::mqtt::build_tires_payload({});
}

tk::JsonOwner build_closures_full() {
    return tk::mqtt::build_closures_payload(
        {true, true, true, false, true, false, true, true});
}
tk::JsonOwner build_closures_minimal() {
    return tk::mqtt::build_closures_payload({});
}

tk::JsonOwner build_vehicle_full() {
    return tk::mqtt::build_vehicle_payload("ASLEEP");
}

tk::mqtt::DevicePayload device_full_input() {
    tk::mqtt::DevicePayload in;
    in.has_wifi_rssi = true; in.wifi_rssi = -61;
    in.ble_connected = true;
    in.has_ble_rssi = true; in.ble_rssi = -72;
    in.paired = true;
    in.boot_time = "2026-01-02T03:04:05+00:00";
    in.free_heap = 100000;
    in.version = "0.0.0-test";
    in.largest_block = 50000;
    in.min_free_heap = 40000;
    in.reset_reason = "power_on";
    in.reset_reason_code = 1;
    in.crash_dump = true;
    in.safe_mode = false;
    in.wifi_reconnects = 2;
    in.mqtt_reconnects = 3;
    in.has_httpd_stack = true; in.httpd_stack = 4000;
    in.has_vehicle_stack = true; in.vehicle_stack = 5000;
    in.has_auto_pair_stack = true; in.auto_pair_stack = 6000;
    in.has_mqtt_stack = true; in.mqtt_stack = 7000;
    return in;
}

tk::JsonOwner build_device_full() {
    return tk::mqtt::build_device_payload(device_full_input());
}
tk::JsonOwner build_device_minimal() {
    tk::mqtt::DevicePayload in;
    in.ble_connected = false;
    in.paired = false;
    in.free_heap = 1;
    in.version = "v";
    in.largest_block = 2;
    in.min_free_heap = 3;
    in.reset_reason = "unknown";
    in.reset_reason_code = 0;
    in.crash_dump = false;
    in.safe_mode = true;
    in.wifi_reconnects = 4;
    in.mqtt_reconnects = 5;
    return tk::mqtt::build_device_payload(in);
}

using PayloadBuilder = tk::JsonOwner (*)();

struct PayloadFactoryCase {
    const char* production_name;
    PayloadBuilder build;
};

// Static runtime policy parses this exact inventory and requires a one-to-one match with every
// build_*_payload definition in mqtt_payloads.hpp and every production call in mqtt_ha.cpp. Each
// case is driven through a successful retained publish plus every cJSON build/print failpoint.
static const std::array<PayloadFactoryCase, 8> kProductionPayloadFactoryCases{{
    {"build_discovery_payload", build_discovery_sensor_full},
    {"build_charge_payload", build_charge_full},
    {"build_climate_payload", build_climate_full},
    {"build_drive_payload", build_drive_full},
    {"build_tires_payload", build_tires_full},
    {"build_closures_payload", build_closures_full},
    {"build_vehicle_payload", build_vehicle_full},
    {"build_device_payload", build_device_full},
}};

bool json_kind_matches(const cJSON* value, tk::mqtt::JsonValueKind kind) {
    switch (kind) {
        case tk::mqtt::JsonValueKind::Number: return cJSON_IsNumber(value);
        case tk::mqtt::JsonValueKind::String: return cJSON_IsString(value);
        case tk::mqtt::JsonValueKind::Boolean: return cJSON_IsBool(value);
    }
    return false;
}

template <size_t N>
bool registry_matches_full_payloads(
    const std::array<tk::mqtt::DiscoveryEntry, N>& entries,
    const std::array<tk::JsonOwner, tk::mqtt::kStateDomainCount>& payloads) {
    if (!tk::mqtt::discovery_registry_shape_valid(entries)) return false;
    for (const auto& entry : entries) {
        const size_t domain = tk::mqtt::state_domain_index(entry.domain);
        if (domain >= payloads.size() || !payloads[domain]) return false;
        const cJSON* value =
            cJSON_GetObjectItemCaseSensitive(payloads[domain].get(), entry.field.data());
        if (!value || !json_kind_matches(value, entry.value_kind)) return false;
    }
    return true;
}

uint64_t hash_registry_string(uint64_t hash, std::string_view value) {
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    hash ^= UINT64_C(0xff);
    return hash * UINT64_C(1099511628211);
}

template <size_t N>
uint64_t discovery_registry_fingerprint(
    const std::array<tk::mqtt::DiscoveryEntry, N>& entries) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const auto& entry : entries) {
        for (const uint8_t value : {
                 static_cast<uint8_t>(entry.domain),
                 static_cast<uint8_t>(entry.component),
                 static_cast<uint8_t>(entry.value_kind),
                 static_cast<uint8_t>(entry.invert),
             }) {
            hash ^= value;
            hash *= UINT64_C(1099511628211);
        }
        for (const std::string_view value : {
                 entry.object_id, entry.name, entry.field, entry.device_class, entry.unit,
                 entry.state_class, entry.entity_category,
             }) {
            hash = hash_registry_string(hash, value);
        }
    }
    return hash;
}

size_t discovery_entry_index(std::string_view object_id) {
    for (size_t i = 0; i < tk::mqtt::kDiscoveryEntries.size(); ++i) {
        if (tk::mqtt::kDiscoveryEntries[i].object_id == object_id) return i;
    }
    return tk::mqtt::kDiscoveryEntries.size();
}

void test_discovery_registry() {
    using tk::mqtt::DiscoveryComponent;
    using tk::mqtt::JsonValueKind;
    using tk::mqtt::StateDomain;

    reset_allocator(0);
    std::array<tk::JsonOwner, tk::mqtt::kStateDomainCount> payloads{{
        build_charge_full(),
        build_climate_full(),
        build_drive_full(),
        build_tires_full(),
        build_closures_full(),
        build_vehicle_full(),
        build_device_full(),
    }};
    for (const auto& payload : payloads) CHECK(payload != nullptr);

    CHECK(tk::mqtt::kDiscoveryEntries.size() == 55);
    CHECK(tk::mqtt::discovery_registry_shape_valid(tk::mqtt::kDiscoveryEntries));
    CHECK(registry_matches_full_payloads(tk::mqtt::kDiscoveryEntries, payloads));

    const std::array<size_t, tk::mqtt::kStateDomainCount> expected_domain_counts{{
        14, 12, 2, 5, 6, 1, 15,
    }};
    std::array<size_t, tk::mqtt::kStateDomainCount> domain_counts{};
    size_t sensors = 0;
    size_t binary_sensors = 0;
    size_t numbers = 0;
    size_t strings = 0;
    size_t booleans = 0;
    size_t inverted = 0;
    std::set<std::string> object_ids;
    std::set<std::string> unique_ids;
    std::set<std::string> config_topics;
    for (const auto& entry : tk::mqtt::kDiscoveryEntries) {
        ++domain_counts[tk::mqtt::state_domain_index(entry.domain)];
        if (entry.component == DiscoveryComponent::BinarySensor) ++binary_sensors;
        else ++sensors;
        if (entry.value_kind == JsonValueKind::Number) ++numbers;
        else if (entry.value_kind == JsonValueKind::String) ++strings;
        else ++booleans;
        if (entry.invert) ++inverted;

        CHECK(object_ids.insert(std::string(entry.object_id)).second);
        CHECK(unique_ids.insert(
                  tk::mqtt::discovery_unique_id("teslakey_fixture", entry)).second);
        CHECK(config_topics.insert(tk::mqtt::discovery_config_topic(
                  "homeassistant", "teslakey_fixture", entry)).second);

        const std::string value_template = tk::mqtt::discovery_value_template(entry);
        if (entry.component == DiscoveryComponent::BinarySensor) {
            CHECK(value_template.find("value_json." + std::string(entry.field)) !=
                  std::string::npos);
            CHECK(value_template.find(" is defined ") != std::string::npos);
        } else {
            CHECK(value_template == "{{ value_json." + std::string(entry.field) + " }}");
        }

        const std::string unique_id =
            tk::mqtt::discovery_unique_id("teslakey_fixture", entry);
        const std::string state_topic = tk::mqtt::discovery_state_topic(
            "tesla-key/teslakey_fixture", entry.domain);
        const tk::mqtt::DiscoveryPayload input{
            entry.name.data(),
            unique_id.c_str(),
            state_topic.c_str(),
            "tesla-key/teslakey_fixture/availability",
            value_template.c_str(),
            entry.device_class.empty() ? nullptr : entry.device_class.data(),
            entry.unit.empty() ? nullptr : entry.unit.data(),
            entry.state_class.empty() ? nullptr : entry.state_class.data(),
            entry.entity_category.empty() ? nullptr : entry.entity_category.data(),
            tk::mqtt::discovery_is_binary(entry),
            "teslakey_fixture",
            "Tesla Key",
            "esp32",
            "0.0.0-test",
            nullptr,
        };
        tk::JsonOwner discovery = tk::mqtt::build_discovery_payload(input);
        CHECK(discovery != nullptr);
        const auto string_field_equals = [&](const char* key, std::string_view expected) {
            const cJSON* value = cJSON_GetObjectItemCaseSensitive(discovery.get(), key);
            return cJSON_IsString(value) && value->valuestring &&
                   std::string_view(value->valuestring) == expected;
        };
        CHECK(string_field_equals("name", entry.name));
        CHECK(string_field_equals("uniq_id", unique_id));
        CHECK(string_field_equals("stat_t", state_topic));
        CHECK(string_field_equals("val_tpl", value_template));
        for (const auto& metadata : {
                 std::pair<const char*, std::string_view>{"dev_cla", entry.device_class},
                 {"unit_of_meas", entry.unit},
                 {"stat_cla", entry.state_class},
                 {"ent_cat", entry.entity_category},
             }) {
            const cJSON* value =
                cJSON_GetObjectItemCaseSensitive(discovery.get(), metadata.first);
            if (metadata.second.empty()) CHECK(value == nullptr);
            else CHECK(string_field_equals(metadata.first, metadata.second));
        }
        const cJSON* payload_on = cJSON_GetObjectItemCaseSensitive(discovery.get(), "pl_on");
        const cJSON* payload_off = cJSON_GetObjectItemCaseSensitive(discovery.get(), "pl_off");
        if (tk::mqtt::discovery_is_binary(entry)) {
            CHECK(cJSON_IsString(payload_on) && std::string_view(payload_on->valuestring) == "ON");
            CHECK(cJSON_IsString(payload_off) && std::string_view(payload_off->valuestring) == "OFF");
        } else {
            CHECK(payload_on == nullptr && payload_off == nullptr);
        }
    }
    CHECK(domain_counts == expected_domain_counts);
    CHECK(sensors == 39 && binary_sensors == 16);
    CHECK(numbers == 28 && strings == 11 && booleans == 16);
    CHECK(inverted == 1);

    const auto& locked = discovery_entry("locked");
    CHECK(locked.invert && locked.device_class == "lock");
    CHECK(tk::mqtt::discovery_value_template(locked) ==
          "{% if value_json.locked is defined %}{{ 'OFF' if value_json.locked else 'ON' }}"
          "{% endif %}");

    static constexpr std::array<std::string_view, tk::mqtt::kStateDomainCount>
        expected_topic_suffixes{{
            "charge", "climate", "drive", "tires", "closures", "vehicle", "device",
        }};
    CHECK(tk::mqtt::kStateTopicSuffixes == expected_topic_suffixes);
    for (size_t i = 0; i < expected_topic_suffixes.size(); ++i) {
        const auto domain = static_cast<StateDomain>(i);
        CHECK(tk::mqtt::discovery_state_topic("tesla-key/teslakey_fixture", domain) ==
              "tesla-key/teslakey_fixture/" + std::string(expected_topic_suffixes[i]));
    }

    // Full metadata fingerprint: component, JSON kind, inversion, names, fields, classes, units,
    // state classes and entity categories for all 55 production rows.
    const uint64_t fingerprint = discovery_registry_fingerprint(tk::mqtt::kDiscoveryEntries);
    std::cout << "  MQTT discovery registry fingerprint: " << fingerprint << '\n';
    CHECK(fingerprint == UINT64_C(17320820650760433559));

    // Mutation canaries: an add/remove, duplicate identifier, domain miswire, field drift,
    // domain-topic swap, boolean/string mismatch or metadata change must make the gate red.
    std::array<tk::mqtt::DiscoveryEntry, 54> removed{};
    for (size_t i = 0; i < removed.size(); ++i) removed[i] = tk::mqtt::kDiscoveryEntries[i];
    CHECK(!tk::mqtt::discovery_registry_shape_valid(removed));

    std::array<tk::mqtt::DiscoveryEntry, 56> added{};
    for (size_t i = 0; i < tk::mqtt::kDiscoveryEntries.size(); ++i) {
        added[i] = tk::mqtt::kDiscoveryEntries[i];
    }
    added.back() = tk::mqtt::kDiscoveryEntries.front();
    added.back().object_id = "fixture_added";
    CHECK(!tk::mqtt::discovery_registry_shape_valid(added));

    auto duplicate = tk::mqtt::kDiscoveryEntries;
    duplicate[1].object_id = duplicate[0].object_id;
    CHECK(!tk::mqtt::discovery_registry_shape_valid(duplicate));

    auto miswired = tk::mqtt::kDiscoveryEntries;
    miswired[0].domain = StateDomain::Climate;
    CHECK(!registry_matches_full_payloads(miswired, payloads));

    auto field_drift = tk::mqtt::kDiscoveryEntries;
    field_drift[0].field = "soc_drift";
    CHECK(!registry_matches_full_payloads(field_drift, payloads));

    auto topic_drift = tk::mqtt::kStateTopicSuffixes;
    std::swap(topic_drift[0], topic_drift[1]);
    CHECK(topic_drift != expected_topic_suffixes);

    auto boolean_drift = tk::mqtt::kDiscoveryEntries;
    const size_t safe_mode = discovery_entry_index("safe_mode");
    CHECK(safe_mode < boolean_drift.size());
    boolean_drift[safe_mode].value_kind = JsonValueKind::String;
    CHECK(!tk::mqtt::discovery_registry_shape_valid(boolean_drift));
    CHECK(!registry_matches_full_payloads(boolean_drift, payloads));

    auto metadata_drift = tk::mqtt::kDiscoveryEntries;
    metadata_drift[0].unit = "kWh";
    CHECK(discovery_registry_fingerprint(metadata_drift) != fingerprint);

    const cJSON* full_device = payloads[tk::mqtt::state_domain_index(StateDomain::Device)].get();
    const cJSON* crash_dump = cJSON_GetObjectItemCaseSensitive(full_device, "crash_dump");
    const cJSON* safe_mode_value = cJSON_GetObjectItemCaseSensitive(full_device, "safe_mode");
    CHECK(cJSON_IsBool(crash_dump) && cJSON_IsTrue(crash_dump));
    CHECK(cJSON_IsBool(safe_mode_value) && cJSON_IsFalse(safe_mode_value));

    payloads = {};
    CHECK(live_allocations == 0);
}

template <typename Build>
bool build_and_publish(Build build, PublishSpy& spy) {
    return tk::mqtt_publish_json(
        "homeassistant/sensor/teslakey_fixture/config", build(), true,
        [&spy](const char* topic, const char* payload, bool retain) {
            return spy(topic, payload, retain);
        });
}

template <typename Build>
void exhaust_build_and_print_allocations(const char* name, Build build) {
    reset_allocator(0);
    size_t build_allocations = 0;
    {
        tk::JsonOwner root = build();
        CHECK(root != nullptr);
        build_allocations = allocation_attempt;
    }
    CHECK(live_allocations == 0);

    reset_allocator(0);
    PublishSpy success;
    CHECK(build_and_publish(build, success));
    const size_t all_allocations = allocation_attempt;
    CHECK(all_allocations > build_allocations);
    CHECK(success.calls == 1);
    CHECK(success.retain);
    CHECK(live_allocations == 0);

    for (size_t nth = 1; nth <= all_allocations; ++nth) {
        reset_allocator(nth);
        PublishSpy failed;
        CHECK(!build_and_publish(build, failed));
        CHECK(failed.calls == 0);
        CHECK(failed.retained_value == "old-retained-value");
        CHECK(live_allocations == 0);
    }

    std::cout << "  " << name << ": " << build_allocations << " build + "
              << (all_allocations - build_allocations) << " print allocation stages\n";
}

template <typename Build>
void check_exact_retained(Build build, const char* expected) {
    reset_allocator(0);
    PublishSpy spy;
    CHECK(build_and_publish(build, spy));
    CHECK(spy.calls == 1);
    CHECK(spy.topic == "homeassistant/sensor/teslakey_fixture/config");
    CHECK(spy.payload == expected);
    CHECK(spy.retain);
    CHECK(spy.retained_value == expected);
    CHECK(live_allocations == 0);
}

void test_exact_payload_branches() {
    check_exact_retained(
        build_discovery_sensor_full,
        "{\"name\":\"Battery\",\"uniq_id\":\"teslakey_fixture_soc\","
        "\"stat_t\":\"tesla-key/teslakey_fixture/charge\","
        "\"avty_t\":\"tesla-key/teslakey_fixture/availability\","
        "\"val_tpl\":\"{{ value_json.soc }}\",\"dev_cla\":\"battery\","
        "\"unit_of_meas\":\"%\",\"stat_cla\":\"measurement\","
        "\"ent_cat\":\"diagnostic\","
        "\"o\":{\"name\":\"tesla-key-esp32\",\"sw\":\"0.0.0-test\","
        "\"url\":\"https://github.com/0Bu/tesla-key-esp32\"},"
        "\"dev\":{\"ids\":[\"teslakey_fixture\"],\"name\":\"Tesla Key\","
        "\"mf\":\"tesla-key-esp32\",\"mdl\":\"esp32s3\",\"sw\":\"0.0.0-test\","
        "\"cu\":\"http://192.0.2.1\"}}");
    check_exact_retained(
        build_discovery_binary_minimal,
        "{\"name\":\"Doors\",\"uniq_id\":\"teslakey_fixture_door_open\","
        "\"stat_t\":\"tesla-key/teslakey_fixture/closures\","
        "\"avty_t\":\"tesla-key/teslakey_fixture/availability\","
        "\"val_tpl\":\"{% if value_json.door is defined %}{{ 'ON' if value_json.door "
        "else 'OFF' }}{% endif %}\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
        "\"dev_cla\":\"door\","
        "\"o\":{\"name\":\"tesla-key-esp32\",\"sw\":\"0.0.0-test\","
        "\"url\":\"https://github.com/0Bu/tesla-key-esp32\"},"
        "\"dev\":{\"ids\":[\"teslakey_fixture\"],\"name\":\"Tesla Key\","
        "\"mf\":\"tesla-key-esp32\",\"mdl\":\"esp32\",\"sw\":\"0.0.0-test\"}}");
    check_exact_retained(
        build_charge_full,
        "{\"soc\":62,\"charge_limit\":80,\"power\":11,\"amps\":16,\"range\":321,"
        "\"rate\":48,\"charging_state\":\"Charging\",\"actual_current\":15,"
        "\"volts\":230,\"current_request\":16,\"phases\":3,\"energy_added\":12,"
        "\"minutes_to_full\":45,\"limit_reason\":\"Standard\"}");
    check_exact_retained(build_charge_minimal, "{}");
    check_exact_retained(
        build_climate_full,
        "{\"inside\":21,\"outside\":-3,\"setpoint\":22,\"on\":true,"
        "\"preconditioning\":false,\"cop\":\"On\",\"cop_cooling\":true,"
        "\"cop_temp\":\"High\",\"cop_reason\":\"Cabin hot\",\"front_defrost\":true,"
        "\"rear_defrost\":false,\"defrost_mode\":\"Max\"}");
    check_exact_retained(build_climate_minimal, "{}");
    check_exact_retained(build_drive_full, "{\"shift\":\"D\",\"odometer\":12345}");
    check_exact_retained(build_drive_minimal, "{}");
    check_exact_retained(
        build_tires_full, "{\"fl\":3,\"fr\":4,\"rl\":5,\"rr\":6,\"warn\":true}");
    check_exact_retained(build_tires_minimal, "{\"warn\":false}");
    check_exact_retained(
        build_closures_full,
        "{\"locked\":true,\"door\":true,\"frunk\":false,\"trunk\":true,"
        "\"window\":false,\"user\":true}");
    check_exact_retained(
        build_closures_minimal,
        "{\"door\":false,\"frunk\":false,\"trunk\":false,\"window\":false}");
    check_exact_retained(build_vehicle_full, "{\"sleep_status\":\"ASLEEP\"}");
    check_exact_retained(
        build_device_full,
        "{\"wifi_rssi\":-61,\"ble_connected\":true,\"ble_rssi\":-72,\"paired\":true,"
        "\"boot_time\":\"2026-01-02T03:04:05+00:00\",\"free_heap\":100000,"
        "\"version\":\"0.0.0-test\",\"largest_block\":50000,\"min_free_heap\":40000,"
        "\"reset_reason\":\"power_on\",\"reset_reason_code\":1,\"crash_dump\":true,"
        "\"safe_mode\":false,\"wifi_reconnects\":2,\"mqtt_reconnects\":3,"
        "\"httpd_stack_min_free_bytes\":4000,\"vehicle_stack_min_free_bytes\":5000,"
        "\"auto_pair_stack_min_free_bytes\":6000,\"mqtt_stack_min_free_bytes\":7000}");
    check_exact_retained(
        build_device_minimal,
        "{\"ble_connected\":false,\"paired\":false,\"free_heap\":1,\"version\":\"v\","
        "\"largest_block\":2,\"min_free_heap\":3,\"reset_reason\":\"unknown\","
        "\"reset_reason_code\":0,\"crash_dump\":false,\"safe_mode\":true,"
        "\"wifi_reconnects\":4,\"mqtt_reconnects\":5}");

    reset_allocator(0);
    CHECK(tk::mqtt::build_vehicle_payload(nullptr) == nullptr);
    CHECK(tk::mqtt::build_discovery_payload({}) == nullptr);
    tk::mqtt::ClimatePayload invalid_climate;
    invalid_climate.has_cop = true;
    CHECK(tk::mqtt::build_climate_payload(invalid_climate) == nullptr);
    CHECK(live_allocations == 0);
}

void test_transport_failures() {
    reset_allocator(0);
    PublishSpy rejected;
    rejected.fail = true;
    CHECK(!build_and_publish(build_device_full, rejected));
    CHECK(rejected.calls == 1);
    CHECK(rejected.retained_value == "old-retained-value");
    CHECK(live_allocations == 0);

    reset_allocator(0);
    PublishSpy throwing;
    throwing.throw_failure = true;
    CHECK(!build_and_publish(build_discovery_sensor_full, throwing));
    CHECK(throwing.calls == 1);
    CHECK(throwing.retained_value == "old-retained-value");
    CHECK(live_allocations == 0);
}

void test_retry_sequence() {
    for (int failed_stage = 0; failed_stage < 3; ++failed_stage) {
        int calls[3] = {0, 0, 0};
        int rearms = 0;
        const bool ok = tk::mqtt_run_discovery_round(
            [&] { ++calls[0]; return failed_stage != 0; },
            [&] { ++calls[1]; return failed_stage != 1; },
            [&] { ++calls[2]; return failed_stage != 2; },
            [&] { ++rearms; });
        CHECK(!ok);
        CHECK(rearms == 1);
        CHECK(calls[0] == 1);
        CHECK(calls[1] == (failed_stage >= 1 ? 1 : 0));
        CHECK(calls[2] == (failed_stage >= 2 ? 1 : 0));
    }

    int calls[3] = {0, 0, 0};
    int rearms = 0;
    CHECK(tk::mqtt_run_discovery_round(
        [&] { ++calls[0]; return true; },
        [&] { ++calls[1]; return true; },
        [&] { ++calls[2]; return true; },
        [&] { ++rearms; }));
    CHECK(calls[0] == 1 && calls[1] == 1 && calls[2] == 1);
    CHECK(rearms == 0);

    for (int thrown_stage = 0; thrown_stage < 3; ++thrown_stage) {
        int throw_calls[3] = {0, 0, 0};
        int throw_rearms = 0;
        bool caught = false;
        try {
            tk::mqtt_run_discovery_round(
                [&] {
                    ++throw_calls[0];
                    if (thrown_stage == 0) throw std::runtime_error("discovery");
                    return true;
                },
                [&] {
                    ++throw_calls[1];
                    if (thrown_stage == 1) throw std::runtime_error("availability");
                    return true;
                },
                [&] {
                    ++throw_calls[2];
                    if (thrown_stage == 2) throw std::runtime_error("state");
                    return true;
                },
                [&] { ++throw_rearms; });
        } catch (const std::runtime_error&) {
            caught = true;
        }
        CHECK(caught);
        CHECK(throw_rearms == 1);
        CHECK(throw_calls[0] == 1);
        CHECK(throw_calls[1] == (thrown_stage >= 1 ? 1 : 0));
        CHECK(throw_calls[2] == (thrown_stage >= 2 ? 1 : 0));
    }

    int state_calls = 0;
    int state_rearms = 0;
    CHECK(!tk::mqtt_run_state_round(
        [&] { ++state_calls; return false; },
        [&] { ++state_rearms; }));
    CHECK(state_calls == 1 && state_rearms == 1);
    CHECK(tk::mqtt_run_state_round(
        [&] { ++state_calls; return true; },
        [&] { ++state_rearms; }));
    CHECK(state_calls == 2 && state_rearms == 1);

    bool state_throw_caught = false;
    try {
        tk::mqtt_run_state_round(
            []() -> bool { throw std::runtime_error("periodic state"); },
            [&] { ++state_rearms; });
    } catch (const std::runtime_error&) {
        state_throw_caught = true;
    }
    CHECK(state_throw_caught);
    CHECK(state_rearms == 2);

    // Bind an actual production builder OOM to the production retry seam.
    reset_allocator(1);
    PublishSpy no_publish;
    int oom_rearms = 0;
    CHECK(!tk::mqtt_run_state_round(
        [&] { return build_and_publish(build_charge_full, no_publish); },
        [&] { ++oom_rearms; }));
    CHECK(oom_rearms == 1);
    CHECK(no_publish.calls == 0);
    CHECK(no_publish.retained_value == "old-retained-value");
    CHECK(live_allocations == 0);
}

}  // namespace

int main() {
    cJSON_Hooks hooks{tracked_malloc, tracked_free};
    cJSON_InitHooks(&hooks);

    for (const PayloadFactoryCase& factory : kProductionPayloadFactoryCases) {
        exhaust_build_and_print_allocations(factory.production_name, factory.build);
    }
    // Additional optional/minimal branches for factories whose production input can omit fields.
    exhaust_build_and_print_allocations("discovery binary/minimal", build_discovery_binary_minimal);
    exhaust_build_and_print_allocations("charge/minimal", build_charge_minimal);
    exhaust_build_and_print_allocations("climate/minimal", build_climate_minimal);
    exhaust_build_and_print_allocations("drive/minimal", build_drive_minimal);
    exhaust_build_and_print_allocations("tires/minimal", build_tires_minimal);
    exhaust_build_and_print_allocations("closures/minimal", build_closures_minimal);
    exhaust_build_and_print_allocations("device/minimal", build_device_minimal);
    test_discovery_registry();
    test_exact_payload_branches();
    test_transport_failures();
    test_retry_sequence();

    reset_allocator(0);
    cJSON_InitHooks(nullptr);
    std::cout << "OK " << checks << " MQTT production-payload checks passed\n";
    return 0;
}
