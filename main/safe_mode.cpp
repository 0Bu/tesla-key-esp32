// Boot-loop safe mode (see safe_mode.hpp). Storage + latch only; every decision is the pure,
// host-tested logic/boot_guard.hpp.
#include "safe_mode.hpp"

#include "logic/boot_guard.hpp"
#include "nvs_storage.hpp"
#include "task_config.hpp"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdlib>
#include <string>

static const char* TAG = "safe_mode";

namespace tk {
namespace {

// NVS key in `tesla_cfg`, stored as a decimal string like every other value in that namespace
// (NvsStorageAdapter's config helpers are string-typed).
constexpr const char* kBootFailKey = "boot_fails";

std::atomic<bool> s_safe_mode{false};

unsigned load_count(NvsStorageAdapter& cfg) {
    std::string raw;
    if (!cfg.load_str(kBootFailKey, raw) || raw.empty()) return 0;
    // strtoul, not stoi: a garbled value must not throw on the boot path. boot_fail_next() then
    // resolves an implausibly large read back to 0 rather than to the maximum — under-counting only
    // delays safe mode, while over-counting would cripple a device that never crashed.
    return static_cast<unsigned>(strtoul(raw.c_str(), nullptr, 10));
}

void store_count(NvsStorageAdapter& cfg, unsigned n) {
    if (!cfg.save_str(kBootFailKey, std::to_string(n))) {
        // Worth an error rather than a shrug: the counter IS the mechanism. A flash that can no
        // longer be written is itself a plausible cause of the crash loop this guards against, and
        // in that case safe mode can never latch — so the operator has to be told that the guard is
        // the thing that is broken, not merely that a write failed.
        ESP_LOGE(TAG, "could not persist the crash-boot counter (%u) — safe mode cannot latch "
                      "while NVS writes are failing", n);
    }
}

void healthy_timer_task(void* arg) {
    auto* cfg = static_cast<NvsStorageAdapter*>(arg);
    vTaskDelay(pdMS_TO_TICKS(kBootHealthyS * 1000));
    // Reaching here means this boot RAN, under load, for the healthy window — the evidence the
    // counter was waiting for. Clear it so an unrelated crash weeks later starts from zero.
    if (cfg) store_count(*cfg, 0);
    ESP_LOGI(TAG, "boot healthy after %us — crash-boot counter cleared", (unsigned)kBootHealthyS);
    vTaskDelete(nullptr);
}

}  // namespace

bool safe_mode_begin(NvsStorageAdapter& config_store, bool was_fault) {
    const unsigned prev = load_count(config_store);
    const unsigned now  = boot_fail_next(prev, was_fault);
    store_count(config_store, now);

    const bool safe = boot_safe_mode(now);
    s_safe_mode.store(safe);

    if (safe) {
        ESP_LOGE(TAG, "SAFE MODE — %u consecutive crash boots (threshold %u). Starting WiFi, the "
                      "web UI and OTA ONLY; the BLE/vehicle stack and the MQTT bridge stay DOWN so "
                      "the device is reachable and fixable in a browser. This also stops each boot "
                      "re-opening the vehicle polling window, which is what drains a parked car. "
                      "Clears itself after a boot that stays up %us.",
                 now, (unsigned)kBootFailThreshold, (unsigned)kBootHealthyS);
    } else if (now > 0) {
        ESP_LOGW(TAG, "crash boot %u of %u before safe mode latches (last reset was a fault)",
                 now, (unsigned)kBootFailThreshold);
    }
    return safe;
}

bool safe_mode_active() { return s_safe_mode.load(); }

void safe_mode_arm_healthy_timer(NvsStorageAdapter& config_store) {
    // 2560 B: the task sleeps, then does one NVS string write and one log line. Deliberately small —
    // it exists for a single write and this device counts stack in hundreds of bytes.
    if (xTaskCreate(healthy_timer_task, "safe_gate", 2560, &config_store,
                    kPrioOtaGate, nullptr) != pdPASS) {
        // Not fatal, but it does mean the counter will not self-clear this boot: the next crash
        // then starts one closer to the threshold. Say so rather than failing silently — a device
        // that latches safe mode "too early" would otherwise be inexplicable.
        ESP_LOGW(TAG, "could not start the healthy-boot timer — the crash-boot counter will not "
                      "clear on its own this boot");
    }
}

}  // namespace tk
