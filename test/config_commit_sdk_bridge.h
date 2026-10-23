#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Passive host bridge, not an SDK ABI substitute. Raw OTA enum/entry declarations below
 * are verbatim from the pinned public esp_flash_partitions.h. Partition/error/I/O handles
 * model only members used by the unmodified query; firmware uses the actual SDK headers. */
typedef enum {
    ESP_OTA_IMG_NEW             = 0x0U,         /*!< Monitor the first boot. In bootloader this state is changed to ESP_OTA_IMG_PENDING_VERIFY. */
    ESP_OTA_IMG_PENDING_VERIFY  = 0x1U,         /*!< First boot for this app was. If while the second boot this state is then it will be changed to ABORTED. */
    ESP_OTA_IMG_VALID           = 0x2U,         /*!< App was confirmed as workable. App can boot and work without limits. */
    ESP_OTA_IMG_INVALID         = 0x3U,         /*!< App was confirmed as non-workable. This app will not selected to boot at all. */
    ESP_OTA_IMG_ABORTED         = 0x4U,         /*!< App could not confirm the workable or non-workable. In bootloader IMG_PENDING_VERIFY state will be changed to IMG_ABORTED. This app will not selected to boot at all. */
    ESP_OTA_IMG_UNDEFINED       = 0xFFFFFFFFU,  /*!< Undefined. App can boot and work without limits. */
} esp_ota_img_states_t;

/* OTA selection structure (two copies in the OTA data partition.)
   Size of 32 bytes is friendly to flash encryption */
typedef struct {
    uint32_t ota_seq;
    uint8_t  seq_label[20];
    uint32_t ota_state;
    uint32_t crc; /* CRC32 of ota_seq field only */
} esp_ota_select_entry_t;

typedef int esp_err_t;
typedef struct { int type; int subtype; } esp_partition_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_ARG (-2)
#define ESP_ERR_NOT_SUPPORTED (-3)
#define ESP_ERR_NOT_FOUND (-4)
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_SUBTYPE_APP_OTA_MIN 0x10
#ifdef __cplusplus
extern "C" {
#endif
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t* buf, uint32_t len);
uint32_t bootloader_common_ota_select_crc(const esp_ota_select_entry_t* entry);
esp_err_t esp_ota_get_state_partition(const esp_partition_t* partition, esp_ota_img_states_t* state);
const esp_partition_t* read_otadata(esp_ota_select_entry_t* out);
int esp_ota_get_app_partition_count(void);
bool is_ota_partition(const esp_partition_t* partition);
#ifdef __cplusplus
}
#endif
