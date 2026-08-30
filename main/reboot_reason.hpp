#pragma once

#include "logic/heap_watchdog.hpp"
#include "nvs_storage.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace tk {

enum class RebootReasonState : uint8_t {
    Missing,
    Present,
    Error,
};

struct RebootReasonRecord {
    RebootReasonState state{RebootReasonState::Missing};
    uint8_t           heap_restarts{0};
    char              text[24]{};
};

static_assert(std::is_trivially_copyable<RebootReasonRecord>::value,
              "boot error records must remain allocation-free");

[[nodiscard]] inline RebootReasonRecord make_reboot_reason_record(
    RebootReasonState state, const char* text, uint8_t heap_restarts) noexcept {
    RebootReasonRecord record{};
    record.state = state;
    record.heap_restarts = heap_restarts;
    if (text) {
        size_t i = 0;
        for (; i + 1 < sizeof(record.text) && text[i] != '\0'; ++i) record.text[i] = text[i];
        record.text[i] = '\0';
    }
    return record;
}

// The reboot itself is authorized only after its counter is durably persisted. Returning false is
// a hard stop for the caller: restarting without the breadcrumb would erase the loop cap.
[[nodiscard]] inline bool persist_reboot_reason(NvsStorageAdapter* cfg, const char* why) noexcept {
    if (!cfg || !why) return false;
    try {
        return cfg->save_str(nvs_contract::kRebootReason, why);
    } catch (...) {
        return false;
    }
}

// Read and consume the prior reboot breadcrumb. Only exact NVS NOT_FOUND opens an ordinary boot.
// Any read/clear ambiguity closes both the active vehicle window and the restart ladder by
// returning the cap as the effective count.
[[nodiscard]] inline RebootReasonRecord take_reboot_reason(NvsStorageAdapter& cfg) noexcept {
    try {
        std::string why;
        const NvsStringLoadState loaded =
            cfg.load_str_state(nvs_contract::kRebootReason, why);
        if (loaded == NvsStringLoadState::Missing) return {};
        if (loaded == NvsStringLoadState::Error) {
            return make_reboot_reason_record(RebootReasonState::Error,
                                             "heap:nvs-read-error",
                                             kHeapMaxConsecutiveRestarts);
        }
        const uint8_t heap_restarts = heap_reason_parse(why.c_str());
        const HeapReason canonical = heap_reason_format(heap_restarts);
        const bool valid = heap_restarts >= 1 &&
                           heap_restarts <= kHeapMaxConsecutiveRestarts &&
                           why == canonical.text;

        // Erase the consumed breadcrumb. An empty NVS string is deliberately classified as a
        // corrupt safety value by load_str_state(), so "clearing" it with save_str("") would
        // permanently turn every subsequent ordinary boot into Error instead of Missing.
        if (!cfg.remove(nvs_contract::kRebootReason)) {
            return make_reboot_reason_record(RebootReasonState::Error,
                                             "heap:nvs-clear-error",
                                             kHeapMaxConsecutiveRestarts);
        }
        // This key has exactly one writer. A non-canonical/out-of-range value is storage
        // ambiguity, not an ordinary boot; close the current boot's activity/restart window even
        // though the corrupt breadcrumb was successfully consumed for next boot recovery.
        if (!valid) {
            return make_reboot_reason_record(RebootReasonState::Error,
                                             "heap:nvs-invalid",
                                             kHeapMaxConsecutiveRestarts);
        }
        return make_reboot_reason_record(RebootReasonState::Present,
                                         canonical.text, heap_restarts);
    } catch (...) {
        return make_reboot_reason_record(RebootReasonState::Error,
                                         "heap:nvs-read-error",
                                         kHeapMaxConsecutiveRestarts);
    }
}

}  // namespace tk
