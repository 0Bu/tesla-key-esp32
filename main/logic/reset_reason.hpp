#pragma once
// ONE vocabulary for "why did this device last boot?". Pure, IDF-free, host-testable.
//
// WHY THIS EXISTS. The reset reason is the first fact any post-mortem needs and the cheapest
// one to get wrong. It reaches at least three readers — the boot line in the log stream, a
// crash record (logic/crashinfo.hpp) and whatever /status reports — and each of them will
// happily grow its own switch over esp_reset_reason(). Two switches drift: one gains
// ESP_RST_USB when USB-serial re-enumeration starts showing up, the other keeps calling it
// "unknown", and a week of collector data then contains two names for one event with nothing
// saying they are the same. Worse, a reader who has learned that "unknown" means "we could not
// tell" now sees it for a reset the chip named precisely. So the mapping lives here, once, and
// everything else spells the reason by calling it.
//
// WHY A PLAIN int. The device passes the raw esp_reset_reason() value straight through. This
// header must not include esp_system.h — the host mock build compiles it with the system
// toolchain and no ESP-IDF exists there — so it cannot name esp_reset_reason_t and instead
// MIRRORS that enum's numeric values below. That mirror is the load-bearing assumption: it is
// pinned to ESP-IDF 5.x, where the enumerators are
//   UNKNOWN=0 POWERON=1 EXT=2 SW=3 PANIC=4 INT_WDT=5 TASK_WDT=6 WDT=7
//   DEEPSLEEP=8 BROWNOUT=9 SDIO=10 USB=11 JTAG=12 EFUSE=13 PWR_GLITCH=14 CPU_LOCKUP=15
// and the .cpp that calls this should static_assert the handful it depends on against the IDF
// enum (POWERON/SW/PANIC/INT_WDT/TASK_WDT/BROWNOUT is enough), so a future renumber is a build
// error instead of a silently mislabelled crash. An unknown/newer code degrades to "unknown"
// rather than asserting a cause nobody established.
//
// The slugs are lowercase, stable and free of spaces/punctuation on purpose: they end up as
// values in key=value log records and in JSON, where a reader greps and groups by them. Renaming
// one splits a series — treat them as identifiers, not prose.
#include <cstdint>

namespace tk {

// Numeric mirror of ESP-IDF 5.x esp_reset_reason_t — see the pinning note above. Scoped, with an
// explicit int underlying type, so casting ANY int into it is well defined and the switch below
// can fall through to "unknown" for a value this table has never heard of.
enum class ResetCode : int {
    Unknown    = 0,
    PowerOn    = 1,
    Ext        = 2,
    Sw         = 3,
    Panic      = 4,
    IntWdt     = 5,
    TaskWdt    = 6,
    OtherWdt   = 7,
    DeepSleep  = 8,
    Brownout   = 9,
    Sdio       = 10,
    Usb        = 11,
    Jtag       = 12,
    Efuse      = 13,
    PwrGlitch  = 14,
    CpuLockup  = 15,
};

// Short, stable slug for a raw reset code.
inline const char* reset_reason_slug(int code) {
    switch (static_cast<ResetCode>(code)) {
        case ResetCode::PowerOn:   return "power_on";
        case ResetCode::Ext:       return "ext";
        case ResetCode::Sw:        return "sw";
        case ResetCode::Panic:     return "panic";
        case ResetCode::IntWdt:    return "int_wdt";
        case ResetCode::TaskWdt:   return "task_wdt";
        case ResetCode::OtherWdt:  return "wdt";
        case ResetCode::DeepSleep: return "deepsleep";
        case ResetCode::Brownout:  return "brownout";
        case ResetCode::Sdio:      return "sdio";
        case ResetCode::Usb:       return "usb";
        case ResetCode::Jtag:      return "jtag";
        case ResetCode::Efuse:     return "efuse";
        case ResetCode::PwrGlitch: return "pwr_glitch";
        case ResetCode::CpuLockup: return "cpu_lockup";
        case ResetCode::Unknown:
        default:                   return "unknown";
    }
}

// Did something go WRONG, or did this board simply start? The distinction is what decides
// whether a boot is worth reporting at all (crash_is_notable in logic/crashinfo.hpp) and what a
// UI is entitled to call a crash.
//
// NOT a fault: power_on (someone plugged it in), sw (our own esp_restart — an OTA reboot, a
// config save, the heap watchdog's deliberate restart), ext (the reset pin), deepsleep, and the
// debug/host-side wakes usb / jtag / sdio. Calling any of those a crash produces a banner on a
// perfectly healthy device, which trains everyone to ignore the banner.
//
// A fault: panic (abort()/an uncaught C++ exception — on this firmware most often an escaped
// std::bad_alloc), the three watchdogs (a task stopped feeding, i.e. wedged), brownout (the
// supply dipped — the classic symptom of a USB port that cannot carry the WiFi+BLE transmit
// peaks), power glitch and CPU lockup.
//
// efuse and unknown are deliberately NOT faults. Both mean "we cannot say", and the honest
// failure direction here is to under-report: a missed fault is one banner nobody saw, while a
// false fault sends someone hunting a crash that never happened and erodes trust in the ones
// that did.
inline bool reset_is_fault(int code) {
    switch (static_cast<ResetCode>(code)) {
        case ResetCode::Panic:
        case ResetCode::IntWdt:
        case ResetCode::TaskWdt:
        case ResetCode::OtherWdt:
        case ResetCode::Brownout:
        case ResetCode::PwrGlitch:
        case ResetCode::CpuLockup:
            return true;
        default:
            return false;
    }
}

}  // namespace tk
