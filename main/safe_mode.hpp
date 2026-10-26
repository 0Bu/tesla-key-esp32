#pragma once

#include "logic/boot_guard.hpp"

class NvsStorageAdapter;
class OtaFaultRestartGuard;
class OtaHealthCommitGuard;

namespace tk {

// app_main calls once before the supervisor/services exist. Unknown storage fails closed.
bool safe_mode_begin(NvsStorageAdapter& config_store, BootCounterCause cause,
                     RebootReasonState reason, unsigned legacy_heap_streak = 0);
bool safe_mode_active();

// The supervisor is the only later caller. A pending clear represents a confirmed boot-time
// count, never a failed write or an uncertain reset cause. Safe Mode cannot earn this clear.
bool safe_mode_healthy_clear_pending() noexcept;
// Caller must freshly qualify the long normal run after acquiring this HealthCommit lease.
// This maintenance lease grants no image confirmation or configuration-restart authority.
bool safe_mode_clear_after_healthy_run(NvsStorageAdapter& config_store,
                                      const OtaHealthCommitGuard& owner,
                                      bool qualified_now) noexcept;

// A typed, confirmed charge in the shared ledger; zero means no authority.
struct SharedRestartReservation {
    uint8_t count{0};
    explicit operator bool() const noexcept { return count != 0; }
};

// Under FaultRestart, freshly require Stable and commit a shared unit in every runtime mode
// before reboot_why. A readback after a failed commit never authorizes the reset.
SharedRestartReservation safe_mode_prepare_heap_restart(NvsStorageAdapter& config_store,
                                                       const OtaFaultRestartGuard& owner) noexcept;

}  // namespace tk
