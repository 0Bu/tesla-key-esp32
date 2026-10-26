#pragma once

#include <esp_task_wdt.h>

namespace tk {

// Current-task ownership only. Explicit release must precede FreeRTOS self-deletion;
// vTaskDelete(nullptr) does not unwind a live C++ scope.
class TaskWatchdogSubscription {
public:
    TaskWatchdogSubscription() = default;
    TaskWatchdogSubscription(const TaskWatchdogSubscription&) = delete;
    TaskWatchdogSubscription& operator=(const TaskWatchdogSubscription&) = delete;
    TaskWatchdogSubscription(TaskWatchdogSubscription&&) = delete;
    TaskWatchdogSubscription& operator=(TaskWatchdogSubscription&&) = delete;

    bool subscribe() noexcept {
        // This one consumer may subscribe once; never silently rearm a retired owner.
        if (started_ || released_) return false;
        started_ = true;
        try {
            active_ = esp_task_wdt_add(nullptr) == ESP_OK ||
                      esp_task_wdt_status(nullptr) == ESP_OK;
        } catch (...) {
            active_ = false;
        }
        return active_;
    }

    void feed(bool uncertain_subscription = false) noexcept {
        if (released_ || (!active_ && !uncertain_subscription)) return;
        try { (void)esp_task_wdt_reset(); } catch (...) {}
    }

    bool release() noexcept {
        if (released_) return true;
        try {
            const esp_err_t result = esp_task_wdt_delete(nullptr);
            // In the pinned SDK these mean confirmed removal, no current-task
            // entry, or an uninitialized TWDT with no entries, respectively.
            if (result != ESP_OK && result != ESP_ERR_NOT_FOUND &&
                result != ESP_ERR_INVALID_STATE) return false;
            active_ = false;
            released_ = true;
            return true;
        } catch (...) {
            return false;
        }
    }

    ~TaskWatchdogSubscription() { (void)release(); }

private:
    bool started_{false};
    bool active_{false};
    bool released_{false};
};

}  // namespace tk
