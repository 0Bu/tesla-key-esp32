#pragma once

#include <cstddef>
#include <cstdint>

// Pure, hardware-free model of supported SPI Ethernet (W5500) board candidates and pin
// validation. ESP32-S3 has no internal EMAC, so all Ethernet transports run W5500 over SPI.
// Because the GPIO matrix allows flexible routing, each commercial board uses a different pin-set.
// This table enables zero-configuration multi-candidate probing at boot.
namespace tk {

struct EthSpiCandidate {
    const char* name;
    int8_t sclk;
    int8_t cs;
    int8_t miso;
    int8_t mosi;
};

// Curated list of known commercial ESP32-S3 PoE / Ethernet boards with onboard W5500 over SPI.
// Ordered by priority:
// 1. M5Stack AtomS3 Lite + ATOMIC PoE Base (default / baseline)
// 2. Waveshare ESP32-S3-ETH / PoE
// 3. LilyGO T-ETH-Lite ESP32-S3
inline constexpr EthSpiCandidate kEthDefaultCandidates[] = {
    { "M5Stack ATOMIC PoE",       5,  6,  7,  8 },
    { "Waveshare ESP32-S3-ETH",  13, 14, 12, 11 },
    { "LilyGO T-ETH-Lite",        10,  9, 11, 12 },
};

inline constexpr size_t kEthDefaultCandidateCount =
    sizeof(kEthDefaultCandidates) / sizeof(kEthDefaultCandidates[0]);

// Validates whether the 4 SPI pins in a candidate are pairwise distinct.
inline constexpr bool eth_candidate_pins_unique(const EthSpiCandidate& cand) {
    return cand.sclk != cand.cs && cand.sclk != cand.miso && cand.sclk != cand.mosi &&
           cand.cs   != cand.miso && cand.cs   != cand.mosi &&
           cand.miso != cand.mosi;
}

// Checks whether a GPIO pin is forbidden on ESP32-S3 for peripheral SPI probing.
// Forbidden pins:
//   - Out of range (< 0 or > 48)
//   - Strapping pins: 0 (boot), 3 (JTAG/strapping), 45 (VDD_SPI), 46 (ROM log)
//   - Flash / Quad-SPI PSRAM internal pins: 26..32
//   - Octal Flash / Octal PSRAM pins: 33..37
//   - USB D+/D-: 19, 20
inline constexpr bool eth_gpio_is_forbidden(int8_t pin) {
    if (pin < 0 || pin > 48) return true;
    if (pin == 0 || pin == 3 || pin == 45 || pin == 46) return true;
    if (pin >= 26 && pin <= 37) return true;
    if (pin == 19 || pin == 20) return true;
    return false;
}

// Validates whether all 4 pins of a candidate are usable on ESP32-S3.
inline constexpr bool eth_candidate_pins_valid(const EthSpiCandidate& cand) {
    if (!eth_candidate_pins_unique(cand)) return false;
    if (eth_gpio_is_forbidden(cand.sclk)) return false;
    if (eth_gpio_is_forbidden(cand.cs))   return false;
    if (eth_gpio_is_forbidden(cand.miso)) return false;
    if (eth_gpio_is_forbidden(cand.mosi)) return false;
    return true;
}

}  // namespace tk
