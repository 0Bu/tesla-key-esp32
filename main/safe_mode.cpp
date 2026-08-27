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
#include <new>
#include <string>

static const char* TAG = "safe_mode";

namespace tk {
namespace {

// NVS key in `tesla_cfg`, stored as a decimal string like every other value in that namespace
// (NvsStorageAdapter's config helpers are string-typed).
constexpr const char* kBootFailKey = tk::nvs_contract::kBootFailures;

std::atomic<bool> s_safe_mode{false};

bool load_count(NvsStorageAdapter& cfg, unsigned& count) {
    std::string raw;
    const NvsStringLoadState state = cfg.load_str_state(kBootFailKey, raw);
    if (state == NvsStringLoadState::Missing) {
        count = 0;
        return true;
    }
    if (state != NvsStringLoadState::Present) {
        ESP_LOGE(TAG, "could not read the crash-boot counter — entering safe mode");
        return false;
    }
    if (!boot_fail_parse(raw.c_str(), count)) {
        ESP_LOGE(TAG, "crash-boot counter is malformed — entering safe mode");
        return false;
    }
    return true;
}

bool store_count(NvsStorageAdapter& cfg, unsigned n) {
    if (!cfg.save_str(kBootFailKey, std::to_string(n))) {
        // Worth an error rather than a shrug: the counter IS the mechanism. A flash that can no
        // longer be written is itself a plausible cause of the crash loop this guards against, and
        // in that case safe mode can never latch — so the operator has to be told that the guard is
        // the thing that is broken, not merely that a write failed.
        ESP_LOGE(TAG, "could not persist the crash-boot counter (%u) — entering safe mode", n);
        return false;
    }
    return true;
}

void healthy_timer_task(void* arg) noexcept {
    try {
        auto* cfg = static_cast<NvsStorageAdapter*>(arg);
        vTaskDelay(pdMS_TO_TICKS(kBootHealthyS * 1000));
        // Reaching here means this boot RAN, under load, for the healthy window — the evidence the
        // counter was waiting for. Clear it so an unrelated crash weeks later starts from zero.
        if (cfg && store_count(*cfg, 0)) {
            ESP_LOGI(TAG, "boot healthy after %us — crash-boot counter cleared",
                     (unsigned)kBootHealthyS);
        } else {
            ESP_LOGE(TAG, "healthy window elapsed but the crash-boot counter remains armed");
        }
        vTaskDelete(nullptr);
    } catch (const std::bad_alloc&) {
        // This is a FreeRTOS C task entry: no C++ exception may unwind through its trampoline.
        // Leaving the counter uncleared is conservative; the next boot remains one step closer to
        // safe mode rather than aborting this otherwise healthy boot.
        ESP_LOGE(TAG, "out of memory while clearing the crash-boot counter; leaving it armed");
        vTaskDelete(nullptr);
    } catch (...) {
        ESP_LOGE(TAG, "unexpected exception while clearing the crash-boot counter; leaving it armed");
        vTaskDelete(nullptr);
    }
}

}  // namespace

bool safe_mode_begin(NvsStorageAdapter& config_store, bool was_fault) {
    try {
        unsigned prev = 0;
        if (!load_count(config_store, prev)) {
            s_safe_mode.store(true);
            return true;
        }
        const unsigned now  = boot_fail_next(prev, was_fault);
        if (!store_count(config_store, now)) {
            s_safe_mode.store(true);
            return true;
        }

        const bool safe = boot_safe_mode(now);
        s_safe_mode.store(safe);

        if (safe) {
            ESP_LOGE(TAG, "SAFE MODE — %u consecutive crash boots (threshold %u). Starting WiFi, the "
                          "web UI and OTA ONLY; the BLE/vehicle stack and the MQTT bridge stay DOWN so "
                          "the device is reachable and fixable in a browser. This also stops each boot "
                          "re-opening the vehicle polling window, which is what drains a parked car. "
                          "The latch remains until a non-fault reset or a successful "
                          "configuration/OTA/power reboot.",
                     now, (unsigned)kBootFailThreshold);
        } else if (now > 0) {
            ESP_LOGW(TAG, "crash boot %u of %u before safe mode latches (last reset was a fault)",
                     now, (unsigned)kBootFailThreshold);
        }
        return safe;
    } catch (const std::bad_alloc&) {
        // This runs on the boot boundary before the risky subsystems start. If the guard cannot
        // even allocate its tiny NVS string, starting BLE/MQTT anyway is the unsafe direction: a
        // reboot loop would reopen the vehicle poll window. Keep only the recovery surface up.
        s_safe_mode.store(true);
        ESP_LOGE(TAG, "out of memory while evaluating the boot guard — entering safe mode");
        return true;
    } catch (...) {
        s_safe_mode.store(true);
        ESP_LOGE(TAG, "unexpected exception while evaluating the boot guard — entering safe mode");
        return true;
    }
}

bool safe_mode_active() { return s_safe_mode.load(); }

void safe_mode_arm_healthy_timer(NvsStorageAdapter& config_store) {
    // NOT armed while safe mode is latched, and this is the difference between a latch and a
    // 4-crashes-then-one-quiet-boot cycle. Safe mode comes up with the BLE/vehicle stack and the
    // MQTT bridge DOWN — precisely the subsystems a start-up crash lives behind — so surviving
    // kBootHealthyS in that state is evidence about the recovery surface, not about the fault.
    // Clearing the counter on it would let the very next reboot start the full stack again, crash
    // again, and re-open the car's polling window on 4 boots out of every 5: the drain this guard
    // exists to stop, merely slowed down.
    //
    // Nothing strands the device here: ANY non-fault reset resets the counter to 0 in
    // safe_mode_begin (boot_fail_next(was_fault=false)), and every intentional exit is one — an OTA
    // install, a successfully persisted rebooting configuration/portal save, a power-cycle, or the
    // reset button. So safe mode ends the moment someone acts on it, and only then.
    if (s_safe_mode.load()) {
        ESP_LOGW(TAG, "SAFE MODE — the healthy-boot timer is NOT armed: with the vehicle stack and "
                      "MQTT down, staying up says nothing about the fault. Install a newer image or "
                      "fix the configuration; any clean reboot clears the counter.");
        return;
    }
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
