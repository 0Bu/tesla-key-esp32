#pragma once
#include <cstddef>
#include <cstdint>
#define MALLOC_CAP_INTERNAL 1u
#define MALLOC_CAP_8BIT 2u
std::size_t heap_caps_get_largest_free_block(unsigned);
std::size_t esp_get_free_heap_size();
// Passive facade exposes only the fields read by the actual diagnostic reporter.
// Target compilation still uses the complete pinned SDK type and implementation.
struct multi_heap_info_t {std::size_t total_free_bytes,largest_free_block;};
void heap_caps_get_info(multi_heap_info_t*,std::uint32_t);
