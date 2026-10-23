#pragma once
// Synthetic host facade only. All four real target ABIs/configurations are checked
// separately by the pinned firmware compilation; these declarations prove no SDK ABI.
#define CONFIG_APP_RETRIEVE_LEN_ELF_SHA 64
#define CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH 1
#if defined(HOST_CRASH_RISCV)
#define CONFIG_IDF_FIRMWARE_CHIP_ID 5
#else
#define CONFIG_IDF_TARGET_ARCH_XTENSA 1
#define CONFIG_IDF_FIRMWARE_CHIP_ID 9
#endif
