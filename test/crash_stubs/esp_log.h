#pragma once
void host_crash_log(char, const char*, const char*, const char*);
#define ESP_LOGE(tag, format, value) host_crash_log('E', tag, format, value)
#define ESP_LOGW(tag, format, value) host_crash_log('W', tag, format, value)
#define ESP_LOGI(tag, format, value) host_crash_log('I', tag, format, value)
