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
constexpr uint16_t kCountMask = 0x7f;
constexpr uint16_t kLatched = 0x80;
constexpr uint16_t kTrusted = 0x100;
constexpr uint16_t kClearPending = 0x200;
// One startup/supervisor writer. Readers obtain a consistent bounded snapshot; this replaces
// the two old bool atomics rather than adding another persistent/global counter.
std::atomic<uint16_t> s_recovery{kLatched};
static_assert(sizeof(s_recovery) <= 2);

bool load_count(NvsStorageAdapter& cfg, unsigned& count, bool require_present = false) {
    std::string raw;
    const auto state = cfg.load_str_state(kBootFailKey, raw);
    if (state == NvsStringLoadState::Missing && !require_present) { count = 0; return true; }
    if (state != NvsStringLoadState::Present || !boot_fail_parse(raw.c_str(), count)) {
        ESP_LOGE(TAG, "shared restart counter is unreadable or malformed; preserving recovery");
        return false;
    }
    return true;
}

bool store_count(NvsStorageAdapter& cfg, unsigned count) {
    if (!cfg.save_str(kBootFailKey, std::to_string(count))) {
        ESP_LOGE(TAG, "shared restart counter commit was not confirmed (%u)", count);
        return false;
    }
    return true;
}
}  // namespace

bool safe_mode_begin(NvsStorageAdapter& cfg, BootCounterCause cause, RebootReasonState reason,
                     unsigned legacy_heap_streak) {
    s_recovery.store(kLatched, std::memory_order_release);
    try {
        unsigned previous = 0;
        if (!load_count(cfg, previous)) return true;
        const unsigned next = boot_shared_count(previous, cause, reason, legacy_heap_streak);
        if (!store_count(cfg, next)) return true;
        const bool latched = boot_safe_mode(next);
        const bool trusted = boot_healthy_clear_allowed(cause, reason);
        s_recovery.store(static_cast<uint16_t>(next) | (latched ? kLatched : 0) |
            (trusted ? kTrusted : 0) | (!latched && next != 0 && trusted ? kClearPending : 0),
            std::memory_order_release);
        if (latched) {
            ESP_LOGE(TAG, "SAFE MODE: shared restart counter %u/%u; vehicle work disabled. "
                          "Exit needs a known clean boot and a confirmed counter reset.", next,
                     static_cast<unsigned>(kBootFailThreshold));
        } else if (next != 0) {
            ESP_LOGW(TAG, "shared restart counter %u/%u; qualified healthy run required before clear", next,
                     static_cast<unsigned>(kBootFailThreshold));
        }
        return latched;
    } catch (...) {
        s_recovery.store(kLatched, std::memory_order_release);
        try { ESP_LOGE(TAG, "boot guard failed; entering safe mode; "
                           "a clean boot exits only if counter storage is healthy."); } catch (...) {}
        return true;
    }
}

bool safe_mode_active() { return (s_recovery.load(std::memory_order_acquire) & kLatched) != 0; }

bool safe_mode_healthy_clear_pending() noexcept {
    const auto state = s_recovery.load(std::memory_order_acquire);
    return (state & (kLatched | kTrusted | kClearPending)) == (kTrusted | kClearPending);
}

bool safe_mode_clear_after_healthy_run(NvsStorageAdapter& cfg,
                                      const OtaHealthCommitGuard& owner,
                                      bool qualified_now) noexcept {
    if (!owner || !qualified_now || !safe_mode_healthy_clear_pending()) return false;
    try {
        unsigned observed = 0;
        if (!load_count(cfg, observed, true) || !store_count(cfg, 0)) return false;
        // Durable zero precedes any RAM/diagnostic refill. A false-visible write leaves this
        // confirmed floor and pending maintenance intact until a later successful commit.
        s_recovery.store(kTrusted, std::memory_order_release);
        return true;
    } catch (...) {
        return false;
    }
}

SharedRestartReservation safe_mode_prepare_heap_restart(NvsStorageAdapter& cfg,
                                                       const OtaFaultRestartGuard& owner) noexcept {
    if (!owner.held()) return {};
    try {
        const auto state = s_recovery.load(std::memory_order_acquire);
        if (!(state & kTrusted) || ota_verification_state() != OtaVerificationState::Stable) return {};
        unsigned observed = 0, next = 0;
        // Unlike fresh enrollment, a missing live record cannot erase a confirmed floor.
        if (!load_count(cfg, observed, true) ||
            !boot_heap_reservation_next(observed, state & kCountMask, next) ||
            !store_count(cfg, next)) return {};
        s_recovery.store(static_cast<uint16_t>(next) | (state & (kLatched | kTrusted)) |
            (!(state & kLatched) ? kClearPending : 0), std::memory_order_release);
        return {static_cast<uint8_t>(next)};
    } catch (...) {
        return {};
    }
}

}  // namespace tk
