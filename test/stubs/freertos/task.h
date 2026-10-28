#pragma once

#include "FreeRTOS.h"

using TaskFunction_t = void (*)(void*);
using TaskHandle_t = void*;

extern "C" {
BaseType_t xTaskCreate(TaskFunction_t task, const char* name, uint32_t stack_depth,
                       void* arg, UBaseType_t priority, TaskHandle_t* out_handle);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
TickType_t xTaskGetTickCount();
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
}

// Exact public task.h values retained from the pinned SDK.
inline constexpr BaseType_t taskSCHEDULER_SUSPENDED = 0;
inline constexpr BaseType_t taskSCHEDULER_NOT_STARTED = 1;
inline constexpr BaseType_t taskSCHEDULER_RUNNING = 2;
extern "C" BaseType_t xTaskGetSchedulerState();
extern "C" TaskHandle_t xTaskGetCurrentTaskHandle();
