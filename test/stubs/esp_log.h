#pragma once

#include <cstdarg>
#include <cstdio>

using vprintf_like_t = int (*)(const char*, va_list);

extern "C" vprintf_like_t esp_log_set_vprintf(vprintf_like_t replacement);

// Capture the real fallback call through an explicit host SDK facade. Interposing libc's
// vprintf symbol is unreliable with fortified stdio in Linux sanitizer builds.
extern "C" int host_test_vprintf(const char* format, va_list args);
#define vprintf host_test_vprintf

#define ESP_LOGE(tag, format, ...) do { (void)(tag); } while (0)
#define ESP_LOGW(tag, format, ...) do { (void)(tag); } while (0)
#define ESP_LOGI(tag, format, ...) do { (void)(tag); } while (0)
#define ESP_LOGD(tag, format, ...) do { (void)(tag); } while (0)
