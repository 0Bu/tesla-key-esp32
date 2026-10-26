#pragma once
#include <cstdint>
struct esp_app_desc_t { std::uint32_t magic_word; char version[32]; std::uint8_t padding[220]; };
#define ESP_APP_DESC_MAGIC_WORD 0xabcd5432

const esp_app_desc_t* esp_app_get_description();
