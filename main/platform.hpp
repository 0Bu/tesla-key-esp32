#pragma once

#include "logic/target.hpp"

// Select the compile-time target enum from the IDF target macro. This is the ONE
// place CONFIG_IDF_TARGET_* is mapped to tk::Target; everything chip-specific
// (platform name, OTA image suffix) flows from tk::target.hpp via TK_TARGET so the
// host mock build can exercise the same mapping without ESP-IDF.
#if   defined(CONFIG_IDF_TARGET_ESP32S3)
#  define TK_TARGET (::tk::Target::Esp32S3)
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#  define TK_TARGET (::tk::Target::Esp32C3)
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
#  define TK_TARGET (::tk::Target::Esp32C6)
#else
#  define TK_TARGET (::tk::Target::Esp32)
#endif

// Human-readable name of the running chip, matching the esp-web-tools chipFamily
// strings. Used for the /api/proxy/1/version "platform" field and the Home Assistant
// device model, so both report the actual target instead of a hardcoded "ESP32-S3".
// One of esp32 / esp32s3 / esp32c3 / esp32c6 (the targets yoziru/tesla-ble supports).
#define TK_PLATFORM (::tk::platform_name(TK_TARGET))
