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
}
