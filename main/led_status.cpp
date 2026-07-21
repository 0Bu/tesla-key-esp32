#include "led_status.hpp"

#include "sdkconfig.h"

#if CONFIG_TESLA_LED_ENABLED

#include <cmath>
#include <cstdint>
#include <exception>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "logic/led_status.hpp"
#include "task_config.hpp"
#include "logic/soc_gradient.hpp"   // shared SoC colour ramp (same one the display uses)
#include "logic/ui_state.hpp"
#include "ota_update.hpp"
#include "vehicle_ctrl.hpp"

// Defined in main.cpp — true only while the STA holds an IP (see the note on the display's
// use of it: querying esp_wifi during association churn faults, so trust this flag instead).
bool wifi_is_connected();

namespace {

constexpr char TAG[] = "led";

constexpr gpio_num_t kDI = (gpio_num_t)CONFIG_TESLA_LED_DI_GPIO;   // APA102 data
constexpr gpio_num_t kCI = (gpio_num_t)CONFIG_TESLA_LED_CI_GPIO;   // APA102 clock
constexpr int        kGlobalBright = CONFIG_TESLA_LED_BRIGHTNESS;  // APA102 5-bit field (1..31)

// Resting states (parked SoC) render at a fraction of full so the dongle isn't a bright dot
// in a dark garage. Done by scaling the 8-bit colour (smooth) rather than the coarse 5-bit
// global-brightness field.
constexpr float kDimScale = 0.30f;

// "See the car, can't connect": this many consecutive recent connect failures raise the amber
// warning (car at its ~3-BLE-device limit, or another controller holds its single link).
constexpr uint32_t kConnFailWarn = 3;

// Hold a fault on the LED at least this long after its source clears, so a brief blip is seen.
constexpr int64_t kErrorLatchUs = 15'000'000;  // 15 s
constexpr int64_t kWarnLatchUs  = 10'000'000;  // 10 s

constexpr int kFrameMs           = 33;  // ~30 Hz render (smooth breathing on one pixel)
constexpr int kSampleEveryFrames = 30;  // resample cached state ~1 Hz (has_session() hits NVS)

constexpr float kTwoPi = 6.2831853f;

// ─── APA102 bit-bang (1 pixel) ────────────────────────────────────────────────────
// APA102/SK9822 has no reset timing — it's plain SPI clocked as fast as you drive it, so a
// no-delay GPIO bit-bang is fine and needs no bus/DMA. One pixel = 4-byte start frame +
// [0xE0|brightness][B][G][R] + 4-byte end frame (>= n/2 clocks). ~12 bytes, no allocation.
inline void apa_byte(uint8_t b) {
    for (int i = 0; i < 8; ++i) {
        gpio_set_level(kDI, (b & 0x80) ? 1 : 0);
        b <<= 1;
        gpio_set_level(kCI, 1);
        gpio_set_level(kCI, 0);
    }
}
void apa_show(uint8_t r, uint8_t g, uint8_t b) {
    int bright = kGlobalBright < 1 ? 1 : (kGlobalBright > 31 ? 31 : kGlobalBright);
    apa_byte(0x00); apa_byte(0x00); apa_byte(0x00); apa_byte(0x00);   // start frame
    apa_byte(0xE0 | (uint8_t)bright);
    apa_byte(b); apa_byte(g); apa_byte(r);                            // APA102 order: B, G, R
    apa_byte(0xFF); apa_byte(0xFF); apa_byte(0xFF); apa_byte(0xFF);   // end frame
}

// ─── colour + animation ───────────────────────────────────────────────────────────
void color_rgb(tk::LedColor c, int soc, uint8_t& r, uint8_t& g, uint8_t& b) {
    switch (c) {
        case tk::LedColor::Off:         r = 0;   g = 0;   b = 0;   break;
        case tk::LedColor::Blue:        r = 0;   g = 40;  b = 255; break;
        case tk::LedColor::Teal:        r = 0;   g = 200; b = 120; break;
        case tk::LedColor::Magenta:     r = 255; g = 0;   b = 110; break;
        case tk::LedColor::Green:       r = 0;   g = 220; b = 40;  break;
        case tk::LedColor::Amber:       r = 255; g = 110; b = 0;   break;
        case tk::LedColor::Red:         r = 255; g = 0;   b = 0;   break;
        case tk::LedColor::SocGradient: {
            int ri, gi, bi; tk::soc_rgb(soc, ri, gi, bi);   // shared logic/soc_gradient.hpp
            r = (uint8_t)ri; g = (uint8_t)gi; b = (uint8_t)bi;
            break;
        }
    }
}

// Brightness envelope 0..1 for the current animation at monotonic time t (seconds). A single
// pixel can't run a spatial wave, so "wave" is this temporal swell (Breathe).
float anim_factor(tk::LedAnim a, float t) {
    switch (a) {
        case tk::LedAnim::Off:   return 0.0f;
        case tk::LedAnim::Solid: return 1.0f;
        case tk::LedAnim::Breathe: {                       // ~2.6 s sine swell, floored at 0.15
            float s = sinf(t * (kTwoPi / 2.6f));
            return 0.575f + 0.425f * s;
        }
        case tk::LedAnim::Pulse: {                         // ~1.1 s: fast attack, linear decay
            float p = fmodf(t, 1.1f) / 1.1f;
            float f = (p < 0.12f) ? (p / 0.12f)
                    : (p < 0.60f) ? (1.0f - (p - 0.12f) / 0.48f)
                    : 0.0f;
            return 0.08f + 0.92f * f;
        }
        case tk::LedAnim::Blink:                            // ~0.9 s hard on/off (alarm)
            return (fmodf(t, 0.9f) < 0.45f) ? 1.0f : 0.0f;
    }
    return 1.0f;
}

void led_task_impl(void* arg) {
    auto& v = *static_cast<VehicleController*>(arg);

    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << kDI) | (1ULL << kCI);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(kCI, 0);
    gpio_set_level(kDI, 0);

    ESP_LOGI(TAG, "status LED on DI=%d CI=%d (brightness %d)", (int)kDI, (int)kCI, kGlobalBright);

    tk::UiSnapshot snap{};
    tk::LedAlerts  alerts{};
    int64_t error_until = 0, warn_until = 0;
    const int64_t t0 = esp_timer_get_time();

    for (int frame = 0;; ++frame) {
        const int64_t now = esp_timer_get_time();

        if (frame % kSampleEveryFrames == 0) {
            OtaStatus ota = ota_get_status();
            if (v.reauth_required() || ota.state == OtaState::Error) error_until = now + kErrorLatchUs;
            if (v.ble_connect_fail() >= kConnFailWarn)               warn_until  = now + kWarnLatchUs;

            // LED-only latched alerts (top of the ladder), held visible past the fault.
            alerts.error           = now < error_until;              // sticky reauth or latched OTA fail
            alerts.ota_downloading = (ota.state == OtaState::Downloading);
            alerts.warn            = now < warn_until;

            // Shared connectivity/charge facts — the SAME snapshot the display reads, so the LED
            // and the panel can never disagree (link_state has one source). ui_snapshot() fills
            // the vehicle-owned fields under the cache lock; the LED supplies wifi_on (bool only —
            // it shows no SSID) and paired (its own ≤1 Hz has_session() sample; hits NVS).
            snap = v.ui_snapshot();
            snap.wifi_on = wifi_is_connected();
            snap.paired  = v.has_session();
        }

        const tk::LedPattern p = tk::led_pattern(snap, alerts);
        const float t = (now - t0) / 1e6f;
        float f = anim_factor(p.anim, t);
        if (p.dim) f *= kDimScale;

        uint8_t r, g, b;
        color_rgb(p.color, snap.soc, r, g, b);
        apa_show((uint8_t)(r * f), (uint8_t)(g * f), (uint8_t)(b * f));

        vTaskDelay(pdMS_TO_TICKS(kFrameMs));
    }
}

void led_task(void* arg) {
    for (;;) {
        try {
            led_task_impl(arg);
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "LED task threw (%s); restarting indicator state", e.what());
        } catch (...) {
            ESP_LOGE(TAG, "LED task threw (unknown); restarting indicator state");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace

void led_status_start(VehicleController& vehicle) {
    // Low priority, small stack — the task only reads cached state and pushes ~12 bytes/frame.
    if (xTaskCreate(led_task, "led", 3072, &vehicle, tk::kPrioLed, nullptr) != pdPASS)
        ESP_LOGE(TAG, "led task create failed — status LED disabled");
}

#else  // !CONFIG_TESLA_LED_ENABLED — no-op stub so one source tree serves every board.

void led_status_start(VehicleController&) {}

#endif
