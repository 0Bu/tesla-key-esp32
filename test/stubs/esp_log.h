#pragma once

#include <cstdarg>

using vprintf_like_t = int (*)(const char*, va_list);

extern "C" vprintf_like_t esp_log_set_vprintf(vprintf_like_t replacement);

#define ESP_LOGE(tag, format, ...) do { (void)(tag); } while (0)
#define ESP_LOGW(tag, format, ...) do { (void)(tag); } while (0)
#define ESP_LOGI(tag, format, ...) do { (void)(tag); } while (0)
#define ESP_LOGD(tag, format, ...) do { (void)(tag); } while (0)
