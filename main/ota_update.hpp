#pragma once

#include <string>

// Pull-based OTA self-update. The device fetches manifest.json from a fixed HTTPS
// URL (CONFIG_TESLA_OTA_MANIFEST_URL), compares its "version" to the running
// firmware, and — when asked — downloads its per-target app image
// (CONFIG_TESLA_OTA_FIRMWARE_BASE_URL + "tesla-key-esp32-<target>.bin") straight into
// the inactive OTA slot via esp_https_ota, then reboots. esp_https_ota verifies the
// image chip-id, so a wrong-target image is refused rather than flashed.

enum class OtaState { Idle, Checking, Downloading, Done, Error };

struct OtaStatus {
    OtaState    state;
    int         progress;          // 0–100 during download
    std::string message;
    std::string available;         // latest version seen by the last check (if any)
    bool        update_available;  // last check found a newer version
    std::string current;           // running version at the last check
};

struct OtaCheckResult {
    bool        ok;                 // check completed (manifest fetched + parsed)
    bool        update_available;   // available > current
    std::string current;
    std::string available;
    std::string reason;
};

// Fetch the manifest and compare versions. Blocking (HTTPS GET, a few seconds) —
// runs inside the background task spawned by ota_check_start(), not on the HTTP task.
OtaCheckResult ota_check();

// Kick off a background version check (HTTPS manifest fetch). Returns false if a
// check or update is already running. Poll ota_get_status(): state goes Checking →
// Idle (then read update_available/available/current) or Error. Keeps the slow TLS
// fetch off the HTTP server task so the UI and evcc stay responsive.
bool ota_check_start();

// Kick off a background download+install task. Returns false if one is already
// running. On success the device reboots into the new image.
bool ota_start();

// Snapshot of the current OTA state (for GET /ota/status polling).
OtaStatus ota_get_status();

// Is a check or download task running right now? Deliberately separate from ota_get_status():
// that one copies std::strings, so on an exhausted heap it can THROW — unusable for the heap
// watchdog, whose whole job is to run when allocation is failing. This reads one atomic and
// allocates nothing, so it is safe from any task at any heap level.
bool ota_is_busy();

// If the running image is still ESP_OTA_IMG_PENDING_VERIFY (a fresh OTA the ~90 s health gate in
// main.cpp hasn't confirmed yet), mark it valid NOW so it can't be rolled back. Call this before
// any DELIBERATE, user-initiated reboot (a config save that reboots, the setup-portal save): the
// user actively interacting is proof the image runs, so an intentional restart inside the health
// window must not look like a failed boot and revert the update. No-op on a normal boot or an
// already-valid image, so it is always safe to call before esp_restart().
void ota_confirm_pending_image();
