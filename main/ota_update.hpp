#pragma once

#include "logic/health_gate.hpp"

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

// Safety gate for VIN/private-key mutations. A rollback-capable image may not write recovery
// journals that the previous slot might interpret differently after an automatic rollback, and
// an OTA worker must not be allowed to reboot during a half-committed identity transaction.
// Unknown partition state fails closed just like PENDING_VERIFY. The snapshot helper is suitable
// for UI/status decisions only; mutation paths must hold OtaIdentityMutationGuard for their full
// transaction so OTA startup is excluded atomically in the opposite direction too.
tk::OtaVerificationState ota_verification_state();
bool ota_identity_mutation_allowed(tk::IdentityMutationEntry entry);

class OtaIdentityMutationGuard {
public:
    explicit OtaIdentityMutationGuard(tk::IdentityMutationEntry entry);
    ~OtaIdentityMutationGuard();

    OtaIdentityMutationGuard(const OtaIdentityMutationGuard&) = delete;
    OtaIdentityMutationGuard& operator=(const OtaIdentityMutationGuard&) = delete;
    OtaIdentityMutationGuard(OtaIdentityMutationGuard&&) = delete;
    OtaIdentityMutationGuard& operator=(OtaIdentityMutationGuard&&) = delete;

    explicit operator bool() const { return held_; }

private:
    bool held_ = false;
};

// Acquire the same process-wide operation gate for a deliberate fault restart. The caller must
// do this BEFORE persisting reboot_why: an OTA or identity transaction already in flight postpones
// the restart without changing durable state. A failed persistence must call
// ota_fault_restart_cancel(); after a successful persistence the owner is deliberately held until
// esp_restart() so no OTA/identity operation can enter the shutdown window.
bool ota_fault_restart_begin();
void ota_fault_restart_cancel();

// Serialize the irreversible PENDING_VERIFY -> VALID transition against OTA workers, identity
// journals and a deliberate fault restart. The health task must re-check the heap only after this
// guard is held, then keep it alive through esp_ota_mark_app_valid_cancel_rollback().
class OtaHealthCommitGuard {
public:
    OtaHealthCommitGuard();
    ~OtaHealthCommitGuard();

    OtaHealthCommitGuard(const OtaHealthCommitGuard&) = delete;
    OtaHealthCommitGuard& operator=(const OtaHealthCommitGuard&) = delete;
    OtaHealthCommitGuard(OtaHealthCommitGuard&&) = delete;
    OtaHealthCommitGuard& operator=(OtaHealthCommitGuard&&) = delete;

    explicit operator bool() const { return held_; }

private:
    bool held_ = false;
};

// If the running image is still ESP_OTA_IMG_PENDING_VERIFY (a fresh OTA the ~90 s health gate in
// main.cpp hasn't confirmed yet), mark it valid NOW so it can't be rolled back. Call this before
// a SuccessfulUserConfigCommit reboot after /set_wifi, /set_mqtt, /set_syslog or a setup-portal
// save: the successful durable commit plus a healthy internal contiguous-heap sample proves the
// image runs, so that intentional restart inside the health window normally must not look like a
// failed boot and revert the update. Identity mutations (/set_vin and /gen_keys) never call this
// path and remain Stable-only. If the shared owner is busy or heap is already critical,
// confirmation fails closed and leaves rollback armed. No-op on a normal boot or an already-valid
// image.
void ota_confirm_pending_image(tk::OtaRebootClass reboot_class);
