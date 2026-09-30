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

namespace tk {

// Presence guard shared by both template kinds: true only when the field exists and is not null.
inline std::string ha_field_present_expr(const char* field) {
    return std::string("value_json.") + field + " is defined and value_json." + field +
           " is not none";
}

// Build the presence-aware value_template for a binary_sensor over optional field `field`.
// `invert` is for HA's "lock" device_class only, which renders ON as "Unlocked" / OFF as "Locked";
// our `locked` field is true=locked, so that one entity emits OFF-when-true to read "Locked".
// An ABSENT field renders `None` (HA → unknown) instead of a phantom OFF or an ignored empty string.
inline std::string ha_binary_value_template(const char* field, bool invert) {
    const char* on  = invert ? "OFF" : "ON";
    const char* off = invert ? "ON"  : "OFF";
    return "{% if " + ha_field_present_expr(field) + " %}{{ '" + on + "' if value_json." + field +
           " else '" + off + "' }}{% else %}None{% endif %}";
}

// Build the presence-aware value_template for a numeric or text sensor over optional field
// `field`. A present value renders unchanged; an absent one renders `None` (HA → unknown) rather
// than an empty string, which HA would ignore, leaving the previous reading displayed.
inline std::string ha_value_template(const char* field) {
    return "{% if " + ha_field_present_expr(field) + " %}{{ value_json." + field +
           " }}{% else %}None{% endif %}";
}

} // namespace tk
