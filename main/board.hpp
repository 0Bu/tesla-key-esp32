#pragma once

// Runtime board identification for the ONE image each chip ships.
//
// This project builds one binary per target and lets it work out which board it is actually
// running on, rather than cutting a build (and an OTA channel, and a CI target, and a manifest
// entry) per variant. The esp32s3 image alone serves three: a LilyGo T-Dongle-S3 with an
// ST7735 panel, a bare ESP32-S3 with nothing attached, and an M5Stack AtomS3 Lite on an ATOMIC
// PoE Base with a W5500 over SPI.
//
// The detector lives here, not in display.cpp where it started, because it now has TWO callers
// that must agree: the display (may I drive the panel?) and the Ethernet backend (may I drive
// SPI on GPIO 5-8?). Those two questions overlap on the T-Dongle-S3, whose panel sits on
// MOSI 3 / SCK 5 / CS 4 — GPIO5 is the SAME pin the AtomPoE base uses for SPI clock. Two
// independent copies of "which board is this" would eventually answer differently, and the
// failure would be a bus fight on a shared pin.
namespace tk {

// True on the LilyGo T-Dongle-S3. Probed ONCE (the result is cached) and only on esp32s3;
// always false on every other target. Safe to call before anything else has touched a GPIO —
// that is in fact the requirement, see board.cpp.
bool board_is_t_dongle_s3();

}  // namespace tk
