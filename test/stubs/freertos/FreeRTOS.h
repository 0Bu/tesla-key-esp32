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

// Host clock uses one millisecond per tick; hardware conversion remains an IDF boundary.
#define pdTICKS_TO_MS(ticks) static_cast<uint32_t>(ticks)

// Public ESP-IDF v6.1 port signatures, with deterministic runtime-test implementations.
extern "C" BaseType_t xPortInIsrContext();
extern "C" bool xPortCanYield();
