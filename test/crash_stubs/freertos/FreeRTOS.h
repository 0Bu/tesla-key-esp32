#pragma once
#include <cstdint>
#define configMAX_TASK_NAME_LEN 16
struct StaticTask_t {
    uint8_t before[52];
    uint8_t ucDummy7[16];
#if defined(HOST_CRASH_RISCV)
    uint8_t after[28];
#else
    uint8_t after[32];
#endif
};
