#pragma once

// Pure, hardware-free per-target naming (see logic/vin.hpp header note). ONE source
// tree builds esp32 / esp32s3 / esp32c3 / esp32c6 / esp32c5; this maps the running chip
// to the strings the rest of the system depends on:
//   platform_name() — esp-web-tools chipFamily / HA device model / /api .../version.
//   image_suffix()  — per-target OTA image suffix (esp32 -> "", others -> "-s3"/...).
// platform.hpp selects TK_TARGET from CONFIG_IDF_TARGET_* and feeds these; ota_update.cpp
// static_asserts its compile-time suffix literal against image_suffix() so they can't
// drift. The host mock build (test/) exercises the mapping without a board.
namespace tk {

enum class Target { Esp32, Esp32S3, Esp32C3, Esp32C6, Esp32C5 };

inline constexpr const char* platform_name(Target t) {
    switch (t) {
        case Target::Esp32S3: return "ESP32-S3";
        case Target::Esp32C3: return "ESP32-C3";
        case Target::Esp32C6: return "ESP32-C6";
        case Target::Esp32C5: return "ESP32-C5";
        default:              return "ESP32";  // classic esp32
    }
}

// Must stay in lockstep with image_suffix() in scripts/ci-build-all.sh +
// scripts/build-pages.sh (CI builds the per-target filenames from the same rule).
inline constexpr const char* image_suffix(Target t) {
    switch (t) {
        case Target::Esp32S3: return "-s3";
        case Target::Esp32C3: return "-c3";
        case Target::Esp32C6: return "-c6";
        case Target::Esp32C5: return "-c5";
        default:              return "";  // classic esp32: tesla-key-esp32.bin
    }
}

}  // namespace tk
