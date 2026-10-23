#pragma once
#include <cstdint>
struct XtExcFrame { uint32_t exit, pc, ps, a0, a1; uint8_t rest[92]; };
