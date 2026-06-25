#pragma once

// Human-readable name of the running chip, matching the esp-web-tools chipFamily
// strings. Used for the /api/proxy/1/version "platform" field and the Home Assistant
// device model, so both report the actual target instead of a hardcoded "ESP32-S3".
// One of esp32 / esp32s3 / esp32c3 / esp32c6 (the targets yoziru/tesla-ble supports).
#if   defined(CONFIG_IDF_TARGET_ESP32S3)
#  define TK_PLATFORM "ESP32-S3"
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#  define TK_PLATFORM "ESP32-C3"
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
#  define TK_PLATFORM "ESP32-C6"
#else
#  define TK_PLATFORM "ESP32"
#endif
