#pragma once
// Pure, hardware-free crash-loop policy, shared with test/test_logic.cpp.
// BootGuard owns one canonical decimal unhealthy-restart ledger. Fault boots charge it;
// deliberate heap restarts reserve a unit before writing their separate diagnostic reason.
// Verified heap SW boots preserve the reservation. Unknown evidence grants no allowance.
// Safe Mode begins at four units; automatic heap recovery stops at five shared units.
// Only a confirmed known-clean reset or qualified 30-minute normal run replenishes credit.
#include "logic/heap_watchdog.hpp"
#include "logic/reset_reason.hpp"
#include <cstdint>

namespace tk {

// The fourth shared unhealthy unit enters recovery on the next boot before Vehicle work.
inline constexpr unsigned kBootFailThreshold = 4;

// Elapsed qualified normal-runtime evidence, not raw uptime or the OTA confirmation timer.
inline constexpr uint32_t kBootHealthyS = 1800;

enum class BootCounterCause : uint8_t { Clean, Fault, HeapRecovery, Uncertain };

// A real fault still increments beside corrupt reboot evidence. Keep the separate trust veto so
// that fact cannot accidentally authorize a time-based clear of an uncertain recovery boot.
inline bool boot_healthy_clear_allowed(BootCounterCause cause, RebootReasonState reason) noexcept {
    return cause != BootCounterCause::Uncertain && reason != RebootReasonState::Error;
}

inline BootCounterCause boot_counter_cause(int reset_code, RebootReasonState reason) noexcept {
    if (reset_is_fault(reset_code)) return BootCounterCause::Fault;
    if (reason == RebootReasonState::Error) return BootCounterCause::Uncertain;
    if (reason == RebootReasonState::Present) {
        return reset_code == static_cast<int>(ResetCode::Sw)
            ? BootCounterCause::HeapRecovery : BootCounterCause::Uncertain;
    }
    switch (static_cast<ResetCode>(reset_code)) {
        case ResetCode::PowerOn:
        case ResetCode::Ext:
        case ResetCode::Sw:
        case ResetCode::DeepSleep:
        case ResetCode::Sdio:
        case ResetCode::Usb:
        case ResetCode::Jtag:
            return BootCounterCause::Clean;
        default:
            return BootCounterCause::Uncertain;
    }
}

// An uncertain code is not called a fault, but it grants no automatic vehicle activity either.
// A later explicit foreground command keeps its existing independent admission.
inline bool boot_activity_seed_allowed(int reset_code, RebootReasonState reason) noexcept {
    return reason == RebootReasonState::Missing &&
           boot_counter_cause(reset_code, reason) == BootCounterCause::Clean;
}

// Where the stored counter saturates. It exists so a long loop cannot overflow the persisted value
// and wrap back under the threshold — the one arithmetic accident that would silently un-latch
// safe mode on the boot that needed it most.
inline constexpr unsigned kBootFailMax = 100;
inline constexpr unsigned kUnhealthyRestartLimit = 5;
static_assert(kUnhealthyRestartLimit == kHeapMaxConsecutiveRestarts);

// Conservative enrollment from legacy records, not reconstruction of their historical sum.
// The already-reserved unit on a verified heap SW boot must not be charged a second time.
inline constexpr unsigned boot_shared_count(unsigned stored, BootCounterCause cause,
                                            RebootReasonState reason,
                                            unsigned legacy_heap_streak) noexcept {
    const unsigned previous = stored > kBootFailMax ? kBootFailMax : stored;
    const bool legacy_verified = cause != BootCounterCause::Clean &&
        reason == RebootReasonState::Present && legacy_heap_streak <= kHeapMaxConsecutiveRestarts;
    const unsigned floor = legacy_verified && legacy_heap_streak > previous
        ? legacy_heap_streak : previous;
    if (cause == BootCounterCause::Clean) return 0;
    if (cause != BootCounterCause::Fault) return floor;
    return floor < kBootFailMax ? floor + 1 : kBootFailMax;
}

// Refusal leaves next untouched. Storage must independently confirm the resulting increment.
inline constexpr bool boot_heap_reservation_next(unsigned observed, unsigned confirmed_floor,
                                                 unsigned& next) noexcept {
    if (observed > kBootFailMax || confirmed_floor > kBootFailMax) return false;
    const unsigned current = observed > confirmed_floor ? observed : confirmed_floor;
    if (current >= kUnhealthyRestartLimit) return false;
    next = current + 1;
    return true;
}


// Parse only the canonical decimal strings store_count() can produce. A present-but-empty,
// signed, prefixed, overflowing or out-of-range value is storage ambiguity, not zero: the boot
// glue fails closed into safe mode rather than authorizing the vehicle/MQTT stack on a possible
// crash loop.
inline bool boot_fail_parse(const char* raw, unsigned& out) noexcept {
    if (!raw || *raw == '\0') return false;
    if (*raw == '0') {
        if (raw[1] != '\0') return false;
        out = 0;
        return true;
    }
    unsigned value = 0;
    for (const char* p = raw; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        const unsigned digit = static_cast<unsigned>(*p - '0');
        if (value > (kBootFailMax - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    out = value;
    return true;
}

// Only a known clean cause clears at boot. A fault increments; heap/uncertain causes preserve.
// The storage adapter must first validate the existing decimal record and confirm its commit.
inline constexpr unsigned boot_fail_next(unsigned stored, BootCounterCause cause) {
    const unsigned cur = (stored > kBootFailMax) ? kBootFailMax : stored;
    if (cause == BootCounterCause::Clean) return 0;
    if (cause != BootCounterCause::Fault) return cur;
    return (cur >= kBootFailMax) ? kBootFailMax : cur + 1u;
}

// Latched once the count reaches the threshold — and it stays latched on every subsequent boot,
// since only a qualified healthy run (or a known clean reset) ever writes the counter back down. No off-by-one:
// the glue charges FIRST and asks about the result, so with a threshold of 4 the 4th used unit
// comes up in safe mode.
inline constexpr bool boot_safe_mode(unsigned count) {
    return count >= kBootFailThreshold;
}

// Called on the elapsed qualified interval; raw boot uptime alone grants no counter clear.
inline constexpr bool boot_healthy_elapsed(uint32_t uptime_s) {
    return uptime_s >= kBootHealthyS;
}

}  // namespace tk
