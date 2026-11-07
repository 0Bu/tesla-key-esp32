#pragma once

// Runtime board identification for the ONE image each chip ships.
//
// This project builds one binary per target and lets it work out which board it is actually
// running on, rather than cutting a build (and an OTA channel, and a CI target, and a manifest
// entry) per variant. The esp32s3 image alone serves several: a LilyGo T-Dongle-S3 with an
// ST7735 panel, a bare ESP32-S3 with nothing attached, and a range of W5500-over-SPI Ethernet
// boards (M5Stack AtomS3 Lite + ATOMIC PoE Base and Waveshare ESP32-S3-ETH),
// each detected at boot by net_eth_probe()'s candidate table (main/logic/eth_board.hpp).
//
// The Ethernet backend must recognize the T-Dongle-S3 before probing W5500 candidates.
// Its physical panel still occupies MOSI 3 / SCK 5 / CS 4 even though the firmware no longer
// drives it; GPIO5 is also the AtomPoE candidate clock. Keep this guard to avoid driving the
// dongle's hardware during Ethernet discovery.
namespace tk {

// True on the LilyGo T-Dongle-S3. Probed ONCE (the result is cached) and only on esp32s3;
// always false on every other target. Safe to call before anything else has touched a GPIO —
// that is in fact the requirement, see board.cpp.
bool board_is_t_dongle_s3();

}  // namespace tk
