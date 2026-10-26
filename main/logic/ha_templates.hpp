#pragma once
// Home-Assistant MQTT-discovery value_template builders (mqtt_ha.cpp). Pure, IDF-free, host-tested.
//
// Presence-awareness is the point. Every telemetry field is a proto3 optional, published only when
// the car reported it, so HA must show "unknown" for an unreported field — not a confident wrong
// value and not the previous one.
//
// What HA does with a template's output (MQTT sensor / binary_sensor contract):
//   * an EMPTY rendering is ignored — the entity KEEPS its previous state, so an absent field
//     rendered as "" leaves a stale temperature, RSSI or "Locked" in place;
//   * a template ERROR also keeps the previous state;
//   * the literal `None` sets the state to unknown.
// Every template therefore renders an absent (or JSON-null) field as the explicit token `None`.
// Jinja's `Undefined` is falsy, so an unguarded `{{ 'ON' if value_json.x else 'OFF' }}` would
// additionally invent a phantom OFF (and, for the inverted `lock` class, a phantom "Unlocked").
// An entirely empty state object `{}` therefore drives every entity of its domain to unknown, which
// is how mqtt_ha.cpp retires retained values once a domain's cache is no longer valid.
#include <string>
#include <cstring>
#include <stdexcept>

namespace tk {

namespace ha_detail {
// Reserve one result before appending; reject wrapped size arithmetic before it can allocate.
inline void reserve_fields(std::string& result, size_t fixed, size_t length,
                           size_t occurrences) {
    const size_t maximum = result.max_size();
    if (fixed > maximum || (occurrences != 0 && length > (maximum - fixed) / occurrences))
        throw std::length_error("HA template field too long");
    result.reserve(fixed + length * occurrences);
}

inline constexpr size_t kPresenceFixed = sizeof("value_json. is defined and value_json. is not none") - 1;

inline void append_presence(std::string& result, const char* field, size_t length) {
    result.append("value_json.");
    result.append(field, length);
    result.append(" is defined and value_json.");
    result.append(field, length);
    result.append(" is not none");
}
}  // namespace ha_detail

// Presence guard shared by both template kinds: true only when the field exists and is not null.
inline std::string ha_field_present_expr(const char* field) {
    const size_t length = std::strlen(field);
    std::string result;
    ha_detail::reserve_fields(result, ha_detail::kPresenceFixed, length, 2);
    ha_detail::append_presence(result, field, length);
    return result;
}

// Build the presence-aware value_template for a binary_sensor over optional field `field`.
// `invert` is for HA's "lock" device_class only, which renders ON as "Unlocked" / OFF as "Locked";
// our `locked` field is true=locked, so that one entity emits OFF-when-true to read "Locked".
// An ABSENT field renders `None` (HA → unknown) instead of a phantom OFF or an ignored empty string.
inline std::string ha_binary_value_template(const char* field, bool invert) {
    const size_t length = std::strlen(field);
    std::string result;
    constexpr size_t fixed = ha_detail::kPresenceFixed +
        sizeof("{% if  %}{{ '' if value_json. else '' }}{% else %}None{% endif %}") - 1 + 5;
    ha_detail::reserve_fields(result, fixed, length, 3);
    result.append("{% if ");
    ha_detail::append_presence(result, field, length);
    result.append(" %}{{ '");
    result.append(invert ? "OFF" : "ON");
    result.append("' if value_json.");
    result.append(field, length);
    result.append(" else '");
    result.append(invert ? "ON" : "OFF");
    result.append("' }}{% else %}None{% endif %}");
    return result;
}

// Build the presence-aware value_template for a numeric or text sensor over optional field
// `field`. A present value renders unchanged; an absent one renders `None` (HA → unknown) rather
// than an empty string, which HA would ignore, leaving the previous reading displayed.
inline std::string ha_value_template(const char* field) {
    const size_t length = std::strlen(field);
    std::string result;
    constexpr size_t fixed = ha_detail::kPresenceFixed +
        sizeof("{% if  %}{{ value_json. }}{% else %}None{% endif %}") - 1;
    ha_detail::reserve_fields(result, fixed, length, 3);
    result.append("{% if ");
    ha_detail::append_presence(result, field, length);
    result.append(" %}{{ value_json.");
    result.append(field, length);
    result.append(" }}{% else %}None{% endif %}");
    return result;
}

} // namespace tk
