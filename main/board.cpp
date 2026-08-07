#include "board.hpp"

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"   // esp_rom_delay_us — the probe needs a settle delay, not a task delay
#endif

namespace tk {

#if CONFIG_IDF_TARGET_ESP32S3

static const char* TAG = "board";

// Detect the LilyGo T-Dongle-S3 by its TF-card socket's external SD pull-ups. The single esp32s3
// image also runs on a generic ESP32-S3 (no panel) and on an M5Stack AtomS3 Lite + ATOMIC PoE
// Base; the ST7735 itself can't be probed (its SDA is write-only — no MISO), but the dongle wires
// the S3's SDMMC bus (CMD=16, D0-D3=14/17/21/18, CLK=12) with EXTERNAL pull-ups, while the other
// two boards leave those GPIOs unrouted and floating. Read each with an internal pull-DOWN: still
// HIGH ⇒ an external pull-up holds it ⇒ the dongle. A majority vote (≥4/6) tolerates a stray.
// HW-verified: 6/6 HIGH on a T-Dongle-S3, 0/6 on a generic ESP32-S3. These GPIOs are otherwise
// unused; each is reset and left floating after the probe.
//
// MUST run before anything drives a GPIO, and in particular before the Ethernet backend claims
// SPI: none of the six pins overlaps the W5500's 5/6/7/8, but the ANSWER decides whether that
// claim is allowed at all (the T-Dongle's panel clock is GPIO5).
static bool probe_t_dongle_s3() {
    static const gpio_num_t sd_pins[] = { GPIO_NUM_16, GPIO_NUM_14, GPIO_NUM_17,
                                          GPIO_NUM_21, GPIO_NUM_18, GPIO_NUM_12 };
    int pulled = 0;
    for (gpio_num_t p : sd_pins) {
        gpio_reset_pin(p);
        gpio_set_direction(p, GPIO_MODE_INPUT);
        gpio_set_pull_mode(p, GPIO_PULLDOWN_ONLY);
        esp_rom_delay_us(1000);            // let the weak internal pull settle
        if (gpio_get_level(p)) ++pulled;
        gpio_set_pull_mode(p, GPIO_FLOATING);
    }
    ESP_LOGI(TAG, "T-Dongle-S3 SD pull-up probe: %d/6 held HIGH", pulled);
    return pulled >= 4;
}

bool board_is_t_dongle_s3() {
    // Cached: the probe drives pull modes on six GPIOs, and re-running it after the display has
    // claimed the bus would be both pointless and rude. A function-local static is initialised
    // exactly once, thread-safely, at the first call.
    static const bool s_is_dongle = probe_t_dongle_s3();
    return s_is_dongle;
}

#else

bool board_is_t_dongle_s3() { return false; }

#endif  // CONFIG_IDF_TARGET_ESP32S3

}  // namespace tk
