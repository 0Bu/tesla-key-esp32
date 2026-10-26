#pragma once
#include "esp_err.h"
#include <stdint.h>
extern "C" esp_err_t esp_efuse_mac_get_default(uint8_t *mac);
