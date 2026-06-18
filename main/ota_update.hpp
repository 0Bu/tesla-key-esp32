#pragma once

#include <string>

// Pull-based OTA self-update. The device fetches manifest.json from a fixed HTTPS
// URL (CONFIG_TESLA_OTA_MANIFEST_URL), compares its "version" to the running
// firmware, and — when asked — downloads the app image (CONFIG_TESLA_OTA_FIRMWARE_URL)
// straight into the inactive OTA slot via esp_https_ota, then reboots.

enum class OtaState { Idle, Checking, Downloading, Done, Error };

struct OtaStatus {
    OtaState    state;
    int         progress;   // 0–100 during download
    std::string message;
    std::string available;  // latest version seen by the last check (if any)
};

struct OtaCheckResult {
    bool        ok;                 // check completed (manifest fetched + parsed)
    bool        update_available;   // available > current
    std::string current;
    std::string available;
    std::string reason;
};

// Fetch the manifest and compare versions. Blocking (HTTPS GET, a few seconds).
OtaCheckResult ota_check();

// Kick off a background download+install task. Returns false if one is already
// running. On success the device reboots into the new image.
bool ota_start();

// Snapshot of the current OTA state (for GET /ota/status polling).
OtaStatus ota_get_status();
