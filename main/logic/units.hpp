#pragma once

// Pure, hardware-free unit conversions (see logic/vin.hpp header note).
// Tesla reports distance/speed in imperial; the MQTT/HA bridge and the web-UI
// telemetry convert to metric (only the /api evcc path keeps miles). Centralising
// the factor here keeps every conversion in lockstep and host-testable.
namespace tk {

// Exact international mile -> kilometre factor.
inline constexpr double kMilesToKm = 1.609344;

// Distance miles -> km (e.g. battery range).
inline constexpr double mi_to_km(double miles) { return miles * kMilesToKm; }

// Speed mph -> km/h (e.g. charge rate). Same factor; named for call-site clarity.
inline constexpr double mph_to_kmh(double mph) { return mph * kMilesToKm; }

// Odometer arrives from the car in hundredths of a mile -> km.
inline constexpr double odo_hundredths_mi_to_km(double hundredths) {
    return hundredths * 0.01 * kMilesToKm;
}

}  // namespace tk
