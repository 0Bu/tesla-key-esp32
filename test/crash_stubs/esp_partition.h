#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
inline constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NOT_FOUND = 0x105,
    ESP_ERR_NO_MEM = 0x101;
inline constexpr int ESP_PARTITION_TYPE_ANY = 0xff, ESP_PARTITION_SUBTYPE_ANY = 0xff,
    ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_SUBTYPE_DATA_COREDUMP = 3;
struct esp_partition_t { uint32_t address, size; int type{1}, subtype{3}; };
struct host_partition_iterator { unsigned index; };
using esp_partition_iterator_t = host_partition_iterator*;
esp_err_t esp_partition_find_err(int, int, const char*, esp_partition_iterator_t*);
const esp_partition_t* esp_partition_get(esp_partition_iterator_t);
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t);
void esp_partition_iterator_release(esp_partition_iterator_t);
esp_err_t esp_partition_read(const esp_partition_t*, size_t, void*, size_t);
