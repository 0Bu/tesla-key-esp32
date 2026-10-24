#include "safe_mode.hpp"

#include "nvs_storage.hpp"
#include "ota_update.hpp"

#include <esp_log.h>
#include <atomic>
#include <string>

static const char* TAG = "safe_mode";

namespace tk {
namespace {
constexpr const char* kBootFailKey = nvs_contract::kBootFailures;
std::atomic<bool> s_safe_mode{false};
std::atomic<bool> s_clear_pending{false};

bool load_count(NvsStorageAdapter& cfg, unsigned& count) {
    std::string raw;
    const auto state = cfg.load_str_state(kBootFailKey, raw);
    if (state == NvsStringLoadState::Missing) { count = 0; return true; }
    if (state != NvsStringLoadState::Present || !boot_fail_parse(raw.c_str(), count)) {
        ESP_LOGE(TAG, "crash-boot counter is unreadable or malformed; preserving recovery");
        return false;
    }
    return true;
}

bool store_count(NvsStorageAdapter& cfg, unsigned count) {
    if (!cfg.save_str(kBootFailKey, std::to_string(count))) {
        ESP_LOGE(TAG, "crash-boot counter commit was not confirmed (%u)", count);
        return false;
    }
    return true;
}
}  // namespace

bool safe_mode_begin(NvsStorageAdapter& cfg, BootCounterCause cause, RebootReasonState reason) {
    s_clear_pending.store(false, std::memory_order_release);
    try {
        unsigned previous = 0;
        if (!load_count(cfg, previous)) {
            s_safe_mode.store(true, std::memory_order_release);
            return true;
        }
        const unsigned next = boot_fail_next(previous, cause);
        if (!store_count(cfg, next)) {
            s_safe_mode.store(true, std::memory_order_release);
            return true;
        }
        const bool latched = boot_safe_mode(next);
        s_safe_mode.store(latched, std::memory_order_release);
        s_clear_pending.store(!latched && next != 0 && boot_healthy_clear_allowed(cause, reason),
                              std::memory_order_release);
        if (latched) {
            ESP_LOGE(TAG, "SAFE MODE: crash counter %u/%u; vehicle work disabled. "
                          "Exit needs a known clean boot and a successful counter read/reset.", next,
                     static_cast<unsigned>(kBootFailThreshold));
        } else if (next != 0) {
            ESP_LOGW(TAG, "crash counter %u/%u; qualified healthy run required before clear", next,
                     static_cast<unsigned>(kBootFailThreshold));
        }
        return latched;
    } catch (...) {
        s_clear_pending.store(false, std::memory_order_release);
        s_safe_mode.store(true, std::memory_order_release);
        try { ESP_LOGE(TAG, "boot guard failed; entering safe mode; "
                           "a clean boot exits only if counter storage is healthy."); } catch (...) {}
        return true;
    }
}

bool safe_mode_active() { return s_safe_mode.load(std::memory_order_acquire); }

bool safe_mode_healthy_clear_pending() noexcept {
    return !safe_mode_active() && s_clear_pending.load(std::memory_order_acquire);
}

bool safe_mode_clear_after_healthy_run(NvsStorageAdapter& cfg,
                                      const OtaHealthCommitGuard& owner,
                                      bool qualified_now) noexcept {
    if (!owner || !qualified_now || !safe_mode_healthy_clear_pending()) return false;
    try {
        if (!store_count(cfg, 0)) return false;
        s_clear_pending.store(false, std::memory_order_release);
        return true;
    } catch (...) {
        return false;
    }
}

bool safe_mode_prepare_heap_restart(NvsStorageAdapter& cfg,
                                    const OtaFaultRestartGuard& owner) noexcept {
    if (!owner.held()) return false;
    if (!safe_mode_active()) return true;
    try {
        if (ota_verification_state() != OtaVerificationState::Stable) return false;
        unsigned count = 0;
        if (!load_count(cfg, count)) return false;
        if (count < kBootFailThreshold) count = kBootFailThreshold;
        // A prior failed commit can still be visible on this handle. Confirm a real commit,
        // including when load_count() reports an already-high value, before authorizing the reason.
        return store_count(cfg, count);
    } catch (...) {
        return false;
    }
}

}  // namespace tk
