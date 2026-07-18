#pragma once
// Home-Assistant MQTT-discovery value_template builders (mqtt_ha.cpp). Pure, IDF-free, host-tested.
//
// Presence-awareness is the point. Every telemetry field is a proto3 optional, published only when
// the car reported it, so HA must show "unknown" for an unreported field — not a confident wrong
// value. The NUMERIC path gets this for free: `{{ value_json.x }}` renders empty for an absent field
// and HA reads that as unknown. The BINARY path did NOT: `{{ 'ON' if value_json.x else 'OFF' }}`
// treats Jinja's `Undefined` (an absent field) as falsy, so it always took the `else` branch —
// rendering a phantom OFF (and, for the inverted `lock` class, a phantom "Unlocked" for a car whose
// lock state is actually unknown). Guarding on `is defined` makes the binary template render empty
// for an absent field, exactly like the numeric one, so both go to "unknown".
#include <string>

namespace tk {

// Build the presence-aware value_template for a binary_sensor over optional field `field`.
// `invert` is for HA's "lock" device_class only, which renders ON as "Unlocked" / OFF as "Locked";
// our `locked` field is true=locked, so that one entity emits OFF-when-true to read "Locked".
// An ABSENT field renders the empty string (HA → unknown) instead of a phantom OFF.
inline std::string ha_binary_value_template(const char* field, bool invert) {
    const char* on  = invert ? "OFF" : "ON";
    const char* off = invert ? "ON"  : "OFF";
    return std::string("{% if value_json.") + field + " is defined %}{{ '" + on +
           "' if value_json." + field + " else '" + off + "' }}{% endif %}";
}

} // namespace tk
