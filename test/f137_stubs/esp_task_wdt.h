#pragma once
#include "freertos/task.h"
using esp_err_t = int;
inline constexpr esp_err_t ESP_OK=0, ESP_FAIL=-1;
inline constexpr esp_err_t ESP_ERR_NOT_FOUND=0x105, ESP_ERR_INVALID_STATE=0x103;
esp_err_t esp_task_wdt_add(TaskHandle_t);
esp_err_t esp_task_wdt_status(TaskHandle_t);
esp_err_t esp_task_wdt_reset();
esp_err_t esp_task_wdt_delete(TaskHandle_t);
