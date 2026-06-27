#pragma once

#include <string>

// Pure, hardware-free logic shared by the firmware and the host-side mock build
// (test/, built without ESP-IDF). Anything in this directory must stay free of IDF,
// FreeRTOS, NimBLE, NVS, cJSON and esp_http_server includes so it compiles with a
// plain host toolchain. See test/README.md and the project CLAUDE.md.
namespace tk {

// A plausible Tesla VIN: exactly 17 chars, uppercase alphanumeric excluding I/O/Q
// (reserved by the VIN standard). Mirrors index.html's client-side check and
// /set_vin's server validation; pairing is gated on it so the device never
// connects/enrols without a real VIN (the boot placeholder "UNKNOWN" is 7 chars,
// so it can never be plausible). Single source of truth — VehicleController::
// vin_is_plausible() delegates here.
inline bool vin_is_plausible(const std::string& vin) {
    if (vin.size() != 17) return false;
    for (char c : vin) {
        bool ok = (c >= '0' && c <= '9') ||
                  ((c >= 'A' && c <= 'Z') && c != 'I' && c != 'O' && c != 'Q');
        if (!ok) return false;
    }
    return true;
}

}  // namespace tk
