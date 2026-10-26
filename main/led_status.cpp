#include "stack_watch.hpp"
#include "led_status.hpp"
#include "runtime_fault_log.hpp"

#include "sdkconfig.h"

#if CONFIG_TESLA_LED_ENABLED

#include <cmath>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "logic/led_status.hpp"
#include "task_config.hpp"
#include "logic/led_driver.hpp"
#include "logic/ui_state.hpp"
#include "net.hpp"
#include "ota_update.hpp"
#include "safe_mode.hpp"
#include "vehicle_ctrl.hpp"

// The network seam (net.hpp) — see the note in display.cpp on why this task must not read
// esp_wifi itself. The LED shows only WHETHER a link exists, never which transport.

namespace {

constexpr char TAG[] = "led";

constexpr gpio_num_t kDI = (gpio_num_t)CONFIG_TESLA_LED_DI_GPIO;   // APA102 data
constexpr gpio_num_t kCI = (gpio_num_t)CONFIG_TESLA_LED_CI_GPIO;   // APA102 clock
constexpr int        kGlobalBright = CONFIG_TESLA_LED_BRIGHTNESS;  // APA102 5-bit field (1..31)

constexpr int kFrameMs           = 33;  // ~30 Hz render (smooth breathing on one pixel)
constexpr int kSampleEveryFrames = 30;  // resample cached state ~1 Hz (has_session() can hit NVS)

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
    const auto bytes = tk::led_apa102_frame({r, g, b}, kGlobalBright);
    for (uint8_t byte : bytes) apa_byte(byte);
}

// Keep the once-per-second snapshot temporary out of the persistent frame loop. This
// separates compiler frames; composed call-chain headroom still needs runtime measurement.
[[gnu::noinline]] void led_sample(VehicleController& v, uint64_t now, tk::UiSnapshot& snap,
                                 tk::LedAlerts& alerts, tk::LedLatches& latches) {
    const bool safe_mode = tk::safe_mode_active();
    // Recovery needs only its latched fault indication. Avoid vehicle/session probes and
    // the OTA status mutex when those subsystems are intentionally unavailable.
    const OtaState ota = safe_mode ? OtaState::Idle : ota_get_state();
    alerts = tk::led_sample_alerts(latches, now, safe_mode ? false : v.reauth_required(),
        ota == OtaState::Error, ota == OtaState::Downloading,
        safe_mode ? 0 : v.ble_connect_fail(), safe_mode);
    if (safe_mode) {
        snap = {};
        return;
    }

    // Shared connectivity/charge facts use the same snapshot type as the display;
    // independent sampling and LED alert latches can differ. ui_snapshot() fills
    // the vehicle-owned fields under the cache lock; transport/pairing are sampled here.
    // Use the display's same WiFi signal-read downgrade during reassociation.
    snap = v.ui_snapshot();
    snap.net = tk::net_kind();
    if (snap.net == tk::NetLink::Wifi && !tk::net_wifi_signal(&snap.wifi_rssi, snap.ssid))
        snap.net = tk::NetLink::None;
    snap.paired = v.has_session() || v.session_available();
}

void led_task(void* arg) {
    tk::stack_watch_sample(tk::StackWatch::Led); // current task at entry
    tk::StackWatchScope f135_task_exit(tk::StackWatch::Led);

  try {
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
    tk::LedLatches latches{};
    const int64_t t0 = esp_timer_get_time();
    tk::IterationFaultLog fault_log{};

    for (int frame = 0;; ++frame) {
        tk::StackWatchScope f135_iteration_exit(tk::StackWatch::Led);
      // ui_snapshot() and durable-session probes can fail under heap pressure. The OTA read
      // copies only its state enum; it does not materialize status strings. This is a cosmetic
      // task with no lock held across the frame, so contain any throw rather than let it unwind
      // into the FreeRTOS C task trampoline → std::terminate → reboot.
      try {
        const int64_t now = esp_timer_get_time();

        if (frame % kSampleEveryFrames == 0) {
            led_sample(v, static_cast<uint64_t>(now), snap, alerts, latches);
        }

        const tk::LedPattern p = tk::led_pattern(snap, alerts);
        const float t = (now - t0) / 1e6f;
        const tk::LedRgb color = tk::led_frame_rgb(p, snap.soc, t);
        apa_show(color.r, color.g, color.b);

        vTaskDelay(pdMS_TO_TICKS(kFrameMs));
      } catch (...) {
          tk::report_iteration_fault(fault_log, static_cast<uint64_t>(esp_timer_get_time()) / 1000,
              [](uint32_t suppressed) { ESP_LOGE(TAG, "iteration fault; %u suppressed", (unsigned)suppressed); });
          vTaskDelay(pdMS_TO_TICKS(200));
      }
    }
  } catch (...) {
      ESP_LOGE(TAG, "LED task boundary threw outside a frame — stopping task");
      tk::stack_watch_sample(tk::StackWatch::Led); // no C++ unwind on delete
      vTaskDelete(nullptr);
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
