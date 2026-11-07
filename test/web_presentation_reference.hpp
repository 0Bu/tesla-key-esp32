#pragma once

#include <cstdint>
#include <cmath>

// C++ reference for the web UI battery colour and RSSI glyph parity vectors.
namespace web_reference {

struct SocGradStop { float p; uint8_t r, g, b; };
inline constexpr SocGradStop kSocGrad[] = {
    {0.00f, 231,  76,  60}, {0.18f, 240, 190,  40}, {0.45f, 120, 200,  90},
    {0.80f,  60, 175,  80}, {1.00f,  30, 140,  60},
};

inline int soc_lerp8(int a, int b, float t) { return a + static_cast<int>(std::lroundf((b - a) * t)); }

inline void soc_rgb(int soc, int& r, int& g, int& b) {
    float p = soc <= 0 ? 0.0f : (soc >= 100 ? 1.0f : soc / 100.0f);
    const int n = static_cast<int>(sizeof(kSocGrad) / sizeof(kSocGrad[0]));
    for (int i = 0; i < n - 1; ++i) {
        if (p <= kSocGrad[i + 1].p) {
            float span = kSocGrad[i + 1].p - kSocGrad[i].p;
            float t = span <= 0 ? 0.0f : (p - kSocGrad[i].p) / span;
            r = soc_lerp8(kSocGrad[i].r, kSocGrad[i + 1].r, t);
            g = soc_lerp8(kSocGrad[i].g, kSocGrad[i + 1].g, t);
            b = soc_lerp8(kSocGrad[i].b, kSocGrad[i + 1].b, t);
            return;
        }
    }
    r = kSocGrad[n - 1].r; g = kSocGrad[n - 1].g; b = kSocGrad[n - 1].b;
}

inline int rssi_bars(int rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

}  // namespace web_reference
