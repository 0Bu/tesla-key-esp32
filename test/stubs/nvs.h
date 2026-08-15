#pragma once

#include <cstddef>
#include <cstdint>

using esp_err_t = int32_t;
using nvs_handle_t = uint32_t;

inline constexpr esp_err_t ESP_OK = 0;
inline constexpr esp_err_t ESP_FAIL = -1;
inline constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
inline constexpr esp_err_t ESP_ERR_NVS_TYPE_MISMATCH = 0x1103;
inline constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH = 0x110c;

enum nvs_open_mode_t : uint8_t {
    NVS_READONLY,
    NVS_READWRITE,
};

extern "C" {
esp_err_t nvs_open(const char* namespace_name, nvs_open_mode_t mode,
                   nvs_handle_t* out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value,
                       size_t* length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value,
                       size_t length);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key);
esp_err_t nvs_commit(nvs_handle_t handle);
esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* out_value,
                      size_t* length);
esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value);
const char* esp_err_to_name(esp_err_t err);
}
