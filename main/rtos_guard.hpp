#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace tk {

// Exception-safe ownership of a FreeRTOS mutex/semaphore take. Any C++ operation in the
// protected scope may throw (notably std::string/vector and tesla-ble command enqueueing);
// the destructor still releases the semaphore while the stack unwinds. A finite wait is
// supported for NimBLE-host callbacks that must never block.
class MutexGuard {
public:
    explicit MutexGuard(SemaphoreHandle_t semaphore,
                        TickType_t wait = portMAX_DELAY) noexcept
        : semaphore_(semaphore), locked_(semaphore && xSemaphoreTake(semaphore, wait) == pdTRUE) {}

    ~MutexGuard() noexcept {
        if (locked_) xSemaphoreGive(semaphore_);
    }

    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

    bool locked() const noexcept { return locked_; }
    explicit operator bool() const noexcept { return locked_; }

    void unlock() noexcept {
        if (!locked_) return;
        xSemaphoreGive(semaphore_);
        locked_ = false;
    }

private:
    SemaphoreHandle_t semaphore_{nullptr};
    bool locked_{false};
};

}  // namespace tk
