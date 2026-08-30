#pragma once

#include <cstdint>

using BaseType_t = int;
using UBaseType_t = unsigned;
using TickType_t = uint32_t;

inline constexpr BaseType_t pdFALSE = 0;
inline constexpr BaseType_t pdTRUE = 1;
inline constexpr BaseType_t pdFAIL = 0;
inline constexpr BaseType_t pdPASS = 1;
inline constexpr TickType_t portMAX_DELAY = 0xffffffffu;

#define pdMS_TO_TICKS(ms) static_cast<TickType_t>(ms)
