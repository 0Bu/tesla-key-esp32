#pragma once

#include "logic/health_gate.hpp"
#include "logic/config_commit.hpp"
#include "logic/ota_contract.hpp"

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
    std::string channel;           // active update channel ("release" or "dev")
    unsigned    target_pr{0};      // 0 for main channel, >0 if PR preview channel
};

struct OtaCheckResult {
    bool        ok;                 // check completed (manifest fetched + parsed)
    bool        update_available;   // eligible update available
    std::string current;
    std::string available;
    std::string reason;
    unsigned    target_pr{0};
};

// Fetch the manifest and compare versions. Blocking (HTTPS GET, a few seconds) —
// runs inside the background task spawned by ota_check_start(), not on the HTTP task.
// pr_number == 0 checks main channel; pr_number > 0 checks PR preview channel.
OtaCheckResult ota_check(unsigned pr_number = 0);

// Kick off a background version check (HTTPS manifest fetch). Returns false if a
// check or update is already running. Poll ota_get_status(): state goes Checking →
// Idle (then read update_available/available/current) or Error. Keeps the slow TLS
// fetch off the HTTP server task so the UI and evcc stay responsive.
bool ota_check_start(unsigned pr_number = 0);

// Kick off a background download+install task. Returns false if one is already
// running. On success the device reboots into the new image.
bool ota_start(unsigned pr_number = 0);

// Snapshot of the current OTA state (for GET /ota/status polling).
OtaStatus ota_get_status();

// Active OTA feed channel ("release" or "dev").
tk::OtaChannel ota_get_channel();
void ota_set_channel(tk::OtaChannel channel);

// Retrieve latest changelog text for offered update (returns true if available).
bool ota_get_changelog(char* out, size_t max_len, size_t& out_len);

// Is a check or download task running right now? Deliberately separate from ota_get_status():
// that one copies std::strings, so on an exhausted heap it can THROW — unusable for the heap
// watchdog, whose whole job is to run when allocation is failing. This reads one atomic and
// allocates nothing, so it is safe from any task at any heap level.
bool ota_is_busy();

// Is an active OTA download or flash-write installation running right now?
// Returns true strictly during an active firmware download/installation task, not during idle checks.
bool ota_is_updating();

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

// A request owns this lease from before cfg_save through reply/catch/restart. Construction
// does not acquire; an unsuccessful request releases on all exits. Confirmation borrows this
// actual noncopyable owner rather than recursively acquiring HealthCommit.
class OtaConfigRestartGuard {
public:
    OtaConfigRestartGuard() noexcept = default;
    ~OtaConfigRestartGuard() noexcept;
    OtaConfigRestartGuard(const OtaConfigRestartGuard&) = delete;
    OtaConfigRestartGuard& operator=(const OtaConfigRestartGuard&) = delete;
    OtaConfigRestartGuard(OtaConfigRestartGuard&&) = delete;
    OtaConfigRestartGuard& operator=(OtaConfigRestartGuard&&) = delete;
    [[nodiscard]] bool try_begin() noexcept;
    bool owns_gate() const noexcept;
private:
    bool held_{};
};

// Query raw image state under this request's lease. VALID and UNDEFINED are distinct;
// errors are unknown, never an inferred blank/Stable state. This does not confirm anything.
tk::ConfigImageResult ota_config_image_state(const OtaConfigRestartGuard& owner) noexcept;

// Only a raw PENDING_VERIFY image after a successful user configuration save may be
// confirmed, subject to Ready, current NimBLE sync/reset and fresh INTERNAL-largest-block
// checks. Setup, identity, recovery and WiFi no-ops never confirm a candidate. A failed SDK
// mark is uncertain: the metadata erase/write may already have changed durable state.
tk::ConfigImageResult ota_confirm_pending_image(
    const OtaConfigRestartGuard& owner, tk::OtaRebootClass reboot_class) noexcept;
