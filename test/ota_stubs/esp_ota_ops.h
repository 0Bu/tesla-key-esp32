#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_image_format.h"
#include "esp_app_desc.h"
using esp_err_t = int;
using esp_ota_handle_t = std::uint32_t;
using esp_partition_type_t = unsigned;
struct esp_partition_t { unsigned type, subtype; std::uint32_t address, size; };
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_OTA_VALIDATE_FAILED 0x1503
#define ESP_PARTITION_TYPE_APP 0
#define OTA_WITH_SEQUENTIAL_WRITES 0xfffffffeu
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*);
const esp_partition_t* esp_ota_get_running_partition();
esp_err_t esp_ota_check_image_validity(esp_partition_type_t,const esp_image_header_t*,const esp_app_desc_t*);
esp_err_t esp_ota_begin(const esp_partition_t*,std::size_t,esp_ota_handle_t*);
esp_err_t esp_ota_write(esp_ota_handle_t,const void*,std::size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*);

enum esp_ota_img_states_t : unsigned { ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY,
    ESP_OTA_IMG_VALID, ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED=~0u };
esp_err_t esp_ota_get_state_partition(const esp_partition_t*,esp_ota_img_states_t*);
