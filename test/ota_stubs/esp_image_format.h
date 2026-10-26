#pragma once
#include <cstdint>
// Semantic SDK doubles only. They do not attest target ABI or real image/signature parsing.
struct esp_image_header_t { std::uint8_t magic; std::uint8_t other[23]; };
struct esp_image_segment_header_t { std::uint32_t load_addr, data_len; };
#define ESP_IMAGE_HEADER_MAGIC 0xe9
