#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include "logic/led_status.hpp"
#include "logic/soc_gradient.hpp"

// Allocation-free decisions used by the optional APA102 driver. The task owns time,
// cached inputs and GPIO writes; these helpers own latching, animation and wire bytes.
namespace tk {

inline constexpr uint64_t kLedErrorLatchUs = 15'000'000;
inline constexpr uint64_t kLedWarnLatchUs = 10'000'000;
inline constexpr uint32_t kLedConnectFailWarn = 3;

struct LedLatches {
    uint64_t error_until = 0;
    uint64_t warn_until = 0;
};

inline uint64_t led_latch_deadline(uint64_t now, uint64_t hold) {
    return now > std::numeric_limits<uint64_t>::max() - hold
        ? std::numeric_limits<uint64_t>::max() : now + hold;
}

inline LedAlerts led_sample_alerts(LedLatches& state, uint64_t now,
                                  bool reauth, bool ota_error, bool ota_downloading,
                                  uint32_t connect_failures, bool safe_mode = false) {
    const bool error = safe_mode || reauth || ota_error;
    const bool warn = connect_failures >= kLedConnectFailWarn;
    if (error) state.error_until = led_latch_deadline(now, kLedErrorLatchUs);
    if (warn) state.warn_until = led_latch_deadline(now, kLedWarnLatchUs);
    return {error || now < state.error_until, ota_downloading,
            warn || now < state.warn_until};
}

struct LedRgb { uint8_t r, g, b; };

inline LedRgb led_color_rgb(LedColor color, int soc) {
    switch (color) {
        case LedColor::Off: return {0, 0, 0};
        case LedColor::Blue: return {0, 40, 255};
        case LedColor::Teal: return {0, 200, 120};
        case LedColor::Magenta: return {255, 0, 110};
        case LedColor::Green: return {0, 220, 40};
        case LedColor::Amber: return {255, 110, 0};
        case LedColor::Red: return {255, 0, 0};
        case LedColor::SocGradient: {
            int r, g, b;
            soc_rgb(soc, r, g, b);
            return {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
        }
    }
    return {0, 0, 0};
}

// The task supplies finite, nonnegative monotonic elapsed seconds.
inline float led_animation_factor(LedAnim animation, float seconds) {
    constexpr float two_pi = 6.2831853f;
    switch (animation) {
        case LedAnim::Off: return 0.0f;
        case LedAnim::Solid: return 1.0f;
        case LedAnim::Breathe:
            return 0.575f + 0.425f * std::sin(seconds * (two_pi / 2.6f));
        case LedAnim::Pulse: {
            const float phase = std::fmod(seconds, 1.1f) / 1.1f;
            const float value = phase < 0.12f ? phase / 0.12f
                : phase < 0.60f ? 1.0f - (phase - 0.12f) / 0.48f : 0.0f;
            return 0.08f + 0.92f * value;
        }
        case LedAnim::Blink: return std::fmod(seconds, 0.9f) < 0.45f ? 1.0f : 0.0f;
    }
    return 1.0f;
}

inline LedRgb led_frame_rgb(const LedPattern& pattern, int soc, float seconds) {
    const LedRgb color = led_color_rgb(pattern.color, soc);
    float factor = led_animation_factor(pattern.anim, seconds);
    if (pattern.dim) factor *= 0.30f;
    return {static_cast<uint8_t>(color.r * factor), static_cast<uint8_t>(color.g * factor),
            static_cast<uint8_t>(color.b * factor)};
}

inline std::array<uint8_t, 12> led_apa102_frame(LedRgb color, int brightness) {
    const int clamped = brightness < 1 ? 1 : brightness > 31 ? 31 : brightness;
    return {0, 0, 0, 0, static_cast<uint8_t>(0xe0 | clamped), color.b, color.g, color.r,
            0xff, 0xff, 0xff, 0xff};
}

}  // namespace tk
