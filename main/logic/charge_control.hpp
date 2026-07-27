#pragma once

#include <cstdint>

namespace tk {

// A charging car, or one that has just received a command, is expected to answer the
// 10-second background ChargeState poll. Older data must not be presented to evcc as a
// live reading: doing so hides a broken BLE feedback loop behind a successful HTTP 200.
// Outside the active window the last value remains intentionally usable so an idle car
// can sleep without being woken by read-only evcc polling.
inline constexpr uint32_t kActiveChargeStateMaxAgeS = 30;

inline bool charge_cache_usable(bool valid, bool active_window,
                                bool have_sample_age, uint32_t sample_age_s) {
    if (!valid) return false;
    if (!active_window) return true;
    return have_sample_age && sample_age_s <= kActiveChargeStateMaxAgeS;
}

enum class ChargingAmpsReadback {
    Verified,
    Missing,
    Mismatch,
};

// A Tesla actionStatus=OK only acknowledges that the command was accepted. The
// ChargeState returned by a subsequent, independently completed request is the proof
// that the requested limit became the vehicle's effective charging-amps setting.
inline ChargingAmpsReadback verify_charging_amps(int requested_amps,
                                                 bool charge_state_valid,
                                                 bool has_charging_amps,
                                                 int applied_amps) {
    if (!charge_state_valid || !has_charging_amps) return ChargingAmpsReadback::Missing;
    return applied_amps == requested_amps
        ? ChargingAmpsReadback::Verified
        : ChargingAmpsReadback::Mismatch;
}

}  // namespace tk
