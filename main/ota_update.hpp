#pragma once

#include "logic/health_gate.hpp"
#include "logic/config_commit.hpp"
#include "logic/ota_contract.hpp"

#include <string>

// Pull-based signed OTA. One coordinator owns the shared device-operation lease and raw
// inactive-slot writer. One separately quarantined ESP-TLS worker receives only a bounded Job;
// all metadata/image/redirect legs share one nonrenewable admission deadline. Public SDK image
// verification and the existing RSA update policy remain mandatory before boot selection.

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

// Fetch the manifest and compare versions. Blocking background coordinator API; the separate
// worker owns network calls. HTTP uses ota_check_start. This is not a hard SDK wall-time guarantee.
// pr_number == 0 checks main channel; pr_number > 0 checks PR preview channel.
OtaCheckResult ota_check(unsigned pr_number = 0);

// Kick off a background version check (HTTPS manifest fetch). Typed refusal separates busy,
// unavailable service and task/startup failure. Poll ota_get_status(): state goes Checking →
// Idle (then read update_available/available/current) or Error. Keeps the slow TLS
// fetch off the HTTP server task so the UI and evcc stay responsive.
tk::OtaStartResult ota_check_start(unsigned pr_number = 0);

// Kick off a background download+install task. Refuse pending/unknown image verification before
// creating a worker or starting HTTPS; inspect the SDK state under the operation gate.
tk::OtaStartResult ota_start(unsigned pr_number = 0);

// State-only snapshot for cosmetic indicators; no OtaStatus/string materialization.
// The shared status mutex may be created on first use. Unavailable locking reports Error.
OtaState ota_get_state();

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

// Allocation-free request only. The coordinator aborts its writer and releases authority after
// actual quiescence; a network-only quarantine never grants writer/restart authority.
void ota_request_heap_abort() noexcept;

// Safety gate for VIN/private-key mutations. A rollback-capable image may not write recovery
// journals that the previous slot might interpret differently after an automatic rollback, and
// an OTA worker must not be allowed to reboot during a half-committed identity transaction.
// Unknown partition state fails closed just like PENDING_VERIFY. The snapshot helper is suitable
// for UI/status decisions only; mutation paths must hold OtaIdentityMutationGuard for their full
// transaction so OTA startup is excluded atomically in the opposite direction too.
tk::OtaVerificationState ota_verification_state();
// Status reads the cache only. Refresh once before HTTP/tasks start and after a verification
// transition while the caller still owns the OTA/HealthCommit/ConfigRestart operation gate.
// This is diagnostic evidence; mutation admission always queries fresh SDK state.
tk::OtaVerificationState ota_cached_verification_state() noexcept;
tk::OtaVerificationState ota_refresh_verification_cache() noexcept;
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

// Fault/rollback admission. Before a confirmed effect, scope exit releases this owner only.
// seal() retains it for shutdown even if subsequent logging throws or a restart returns.
class OtaFaultRestartGuard {
public:
    OtaFaultRestartGuard() noexcept;
    ~OtaFaultRestartGuard() noexcept;
    OtaFaultRestartGuard(const OtaFaultRestartGuard&) = delete;
    OtaFaultRestartGuard& operator=(const OtaFaultRestartGuard&) = delete;
    OtaFaultRestartGuard(OtaFaultRestartGuard&&) = delete;
    OtaFaultRestartGuard& operator=(OtaFaultRestartGuard&&) = delete;
    bool held() const noexcept { return acquisition_.acquired; }
    tk::OtaIdentityGateState observed_owner() const noexcept { return acquisition_.observed; }
    void seal() noexcept { if (held()) sealed_ = true; }
private:
    tk::OperationAcquisition acquisition_;
    bool sealed_{false};
};

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
    tk::OtaIdentityGateState observed_owner() const noexcept { return observed_; }

private:
    bool held_ = false;
    tk::OtaIdentityGateState observed_ = tk::OtaIdentityGateState::Idle;
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
    // Bootstrap recovery uses this lease without confirming a pending image. After a committed
    // repair it retains ownership through restart, including a throw or an unexpected return.
    void seal() noexcept { if (owns_gate()) sealed_ = true; }
private:
    bool held_{};
    bool sealed_{};
};

// Query raw image state under this request's lease. VALID and UNDEFINED are distinct;
// errors are unknown, never an inferred blank/Stable state. This does not confirm anything.
tk::ConfigImageResult ota_config_image_state(const OtaConfigRestartGuard& owner) noexcept;

// Local Ready/sync/sticky-reset/INTERNAL-largest-block sample, ordered and allocation-free.
// Only a fresh call under the caller's proper operation owner can precede an SDK mark attempt.
tk::RuntimeHealthSample ota_runtime_health();
// Read-only ordinary-reference query. Only this actually held owner may borrow the old
// Operation; an unowned advisory health sample never reads its pointer or coordinator.
bool ota_recovery_reference_ready(const OtaHealthCommitGuard& owner) noexcept;

// Only a raw PENDING_VERIFY image after a successful user configuration save may be
// confirmed, subject to Ready, current NimBLE sync/reset and fresh INTERNAL-largest-block
// checks. Setup, identity, recovery and WiFi no-ops never confirm a candidate. A failed SDK
// mark is uncertain: the metadata erase/write may already have changed durable state.
tk::ConfigImageResult ota_confirm_pending_image(
    const OtaConfigRestartGuard& owner, tk::OtaRebootClass reboot_class) noexcept;
