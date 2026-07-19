#pragma once
// Last-resort escalation for a heap this device can no longer recover from. Pure, IDF-free,
// host-tested (test/test_logic.cpp).
//
// WHY THIS EXISTS. Every OOM guard in this firmware turns "out of memory" into "recover and
// continue" — the handle_all try/catch answers 503, the BLE parse guards reset the link, the
// /events broadcast drops a frame. That is right for a TRANSIENT shortage and must stay. But
// nothing ever asked the next question: what if it never recovers?
//
// On 2026-07-18 a non-reading WebSocket subscriber exhausted the heap (fixed separately by the
// /events send backpressure in ws_policy.hpp). The device then sat with free=14820 and
// largest_block=768 — against a healthy 31744 — for TEN HOURS. It never crashed and never
// rebooted: vehicle_->loop() threw std::bad_alloc, the handler reset the BLE link, the next
// 50 ms iteration threw again, ~20 times a second, all night. HTTP could not serve, MQTT could
// not reconnect, and even the WiFi watchdog was dead ("ping_sock: create ping task failed" — it
// could no longer allocate its own task). A hang is the worst failure shape available: a crash
// would have rebooted in seconds, but a wedge looks like a powered-off device, heals never, and
// reports nothing.
//
// So: when the heap has been unusable CONTINUOUSLY for long enough that no transient explains
// it, restart deliberately. Five minutes of dead device beats ten hours of dead device.
//
// WHY A RESTART AND NOT IN-PLACE RECOVERY. Rebooting is the crude answer, so it was adopted only
// after the alternatives were researched (full write-up with sources in docs/ARCHITECTURE.md,
// "Why a restart, and not in-place recovery"):
//   - Tearing down and re-initialising subsystems is the obvious smarter fix and has the worst
//     evidence: nimble_port_deinit leaked twice in a row (esp-idf#8136), esp_wifi_deinit leaked
//     per cycle on IDF 5.0/5.1 (#12014) and crashed in the 3.x era (#2050), and httpd_stop() leaked
//     ~16.7 KB per cycle on 5.3 — where the first upstream FIX for it crashed on an assert
//     (#14266). Worse, deinit paths allocate, and we run when allocation is failing: a throw there
//     unwinds into the net-less loop task and abort()s, i.e. an UNcontrolled restart with no
//     breadcrumb. Crash-only literature says the same thing structurally — a restart is
//     trustworthy only when it does not run the failing component's own code.
//   - A ballast block freed under pressure costs permanent internal DRAM out of a ~31 KB steady
//     state, and this path needs no headroom: it is allocation-free by construction.
//   - heap_caps_register_failed_alloc_callback() runs inside the allocator in an arbitrary task/ISR
//     context, cannot satisfy the allocation, and has no documented safety rules — a sensor, not an
//     actor. Espressif's own escalation for this condition (HEAP_ABORT_WHEN_ALLOCATION_FAILS) is a
//     bare esp_system_abort(), i.e. less ceremony than this.
//   - There is no defragmentation to call: ESP-IDF documents fragmentation failure as expected and
//     part of TLSF lives in ROM.
// What shipping firmware does (ESPHome safe mode, Tasmota) is exactly this shape: a bounded restart
// ladder with a persisted counter, not in-place repair.
//
// WHY IT CANNOT SIMPLY REBOOT ON bad_alloc. A reboot re-opens the active polling window
// (vehicle_ctrl seeds it at boot), so a device that reboots in a loop keeps a parked car awake
// and drains it. That is why the trigger is a LONG, UNBROKEN run below the threshold and not a
// single failed allocation: one bad_alloc is exactly the transient the existing guards handle.
// Note the objection is weaker than it first looks — the wedged state defeats car sleep too, and
// harder (it reset the BLE link at 20 Hz for ten hours).
//
// WHY largest_block AND NOT free. The binding limit on this chip is the largest CONTIGUOUS free
// block, not the total. During the incident `free` stayed at a plausible-looking ~16 KB while
// largest_block sat at 544–1536 B — total free would have looked survivable the whole time.
//
// WHY *INTERNAL* largest_block. The caller must sample MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL, not
// plain MALLOC_CAP_8BIT: the latter reports the max across every heap carrying the cap, and the
// esp32c5 registers 8 MB of PSRAM there (CONFIG_SPIRAM_USE_MALLOC). A C5 in the exact wedge —
// internal DRAM at 768 B — would read ~7.8 MB and never trigger, i.e. the watchdog would be a
// silent no-op on the one target that has the extra RAM. The thresholds below are internal-DRAM
// numbers and only mean anything against an internal-DRAM sample.
#include <cstddef>
#include <cstdint>

namespace tk {

// Below this, the largest contiguous block can no longer satisfy the allocations normal
// operation makes. Healthy steady state is 31744 B and the boot-time low-water mark is ~40 KB;
// the wedge sat at 480–1536 B. 4 KB is far under anything healthy and far over the wedge, so it
// separates the two without needing a tight estimate of either.
inline constexpr size_t kHeapCriticalBytes = 4096;

// How long it must stay below that, without a single sample recovering, before we act. Long
// enough that no burst — an OTA, a /diag dump, a scan, several concurrent HTTP clients — can
// reach it, since all of those resolve in seconds.
inline constexpr uint32_t kHeapCriticalHoldMs = 300000;   // 5 minutes

// The action AND the transition that produced it. The transitions exist so the sample site can
// narrate the whole escalation to syslog — armed, counting down, recovered, fired — instead of
// only the last line. Syslog is the ONLY post-mortem source that survives the restart (the /diag
// ring is RAM), so a reader must be able to reconstruct why the device rebooted from it alone;
// during the 2026-07-18 incident there was nothing to read afterwards at all.
enum class HeapAction : uint8_t {
    Ok,         // healthy, and healthy on the previous sample too — nothing to say
    Recovered,  // a critical run just ENDED (the heap came back, or an OTA excused it)
    Armed,      // this sample OPENED a critical run — the countdown to a restart starts here
    Watching,   // still critical, the hold time has not elapsed
    Restart,    // sustained exhaustion: restart deliberately
};

struct HeapVerdict {
    HeapAction action;
    // How long the run THIS verdict describes has lasted: the elapsed time so far for
    // Watching/Restart, the total length of the run that just ended for Recovered, 0 otherwise.
    // Logged rather than the configured hold, so the line states what actually happened — the
    // sample cadence means a fired run has always run somewhat LONGER than kHeapCriticalHoldMs.
    uint32_t critical_ms;
    // Recovered because an OTA is in flight, not because the heap healed. A different sentence in
    // the log: one says the device is fine again, the other says we declined to judge it.
    bool ota_excused;
};

struct HeapWatchdog {
    bool     critical{false};   // currently inside an unbroken critical run
    uint32_t since_ms{0};       // when that run started (only meaningful while critical)
};

struct HeapSample {
    // heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) — INTERNAL matters,
    // see the header note: without it the esp32c5's PSRAM masks the exhaustion entirely.
    size_t   largest_block;
    uint32_t now_ms;          // monotonic; wrap is handled
    bool     ota_busy;        // an OTA check/download is in flight
};

// After this many CONSECUTIVE watchdog restarts, stop restarting. Five cycles are proof that a
// restart does not fix this one: the leak re-develops from a fresh ~78 KB in minutes, so we would
// otherwise reboot every ~10 minutes forever — and every boot re-seeds the active polling window,
// so a parked car next to it would never sleep again. Staying up in the degraded state is also
// useless, but it stops cycling the radios and still reports last_reboot, which is diagnosable.
// The count resets on any ordinary boot (power cycle, crash, OTA) because those leave no
// breadcrumb — so this only ever bounds a genuine restart LOOP, never an isolated self-heal.
inline constexpr uint8_t kHeapMaxConsecutiveRestarts = 5;

inline constexpr bool heap_may_restart(uint8_t consecutive) {
    return consecutive < kHeapMaxConsecutiveRestarts;
}

// The NVS breadcrumb is "heap:<n>" — the reason plus how many consecutive restarts it has now
// caused. Formatted into a fixed buffer rather than a std::string: this runs on a heap that is by
// definition failing, so the whole restart path stays allocation-free.
struct HeapReason {
    char text[16];
};

inline HeapReason heap_reason_format(uint8_t consecutive) {
    HeapReason r{};
    const char* p = "heap:";
    size_t i = 0;
    while (*p) r.text[i++] = *p++;
    if (consecutive >= 100) r.text[i++] = static_cast<char>('0' + (consecutive / 100) % 10);
    if (consecutive >= 10)  r.text[i++] = static_cast<char>('0' + (consecutive / 10) % 10);
    r.text[i++] = static_cast<char>('0' + consecutive % 10);
    r.text[i]   = '\0';
    return r;
}

// How many consecutive restarts the stored breadcrumb represents. 0 for anything that is not one
// of ours — an absent key, an empty string, or a value from a future/other writer — so an
// unparseable breadcrumb can only ever UNDER-count, never wrongly suppress a needed restart.
inline uint8_t heap_reason_parse(const char* s) {
    if (!s) return 0;
    const char* pre = "heap:";
    for (size_t i = 0; i < 5; i++)
        if (s[i] != pre[i]) return 0;
    unsigned n = 0;
    const char* d = s + 5;
    if (!*d) return 0;
    for (; *d; d++) {
        if (*d < '0' || *d > '9') return 0;
        n = n * 10 + static_cast<unsigned>(*d - '0');
        if (n > 255) return 255;
    }
    return static_cast<uint8_t>(n);
}

// How long until a restart, given how long the run has already lasted. Saturates at 0 rather than
// wrapping, so the countdown in the log can never print a nonsense ~49-day figure.
inline constexpr uint32_t heap_restart_in_ms(uint32_t critical_ms) {
    return critical_ms >= kHeapCriticalHoldMs ? 0 : kHeapCriticalHoldMs - critical_ms;
}

// Feed one sample. Call it on a fixed cadence; the decision uses elapsed time, not sample count,
// so the cadence can change without moving the threshold.
inline HeapVerdict heap_watch(HeapWatchdog& w, const HeapSample& s) {
    // An OTA legitimately holds the largest allocations this firmware ever makes (a TLS session
    // plus the image window), so a low reading during one proves nothing. Clear the run rather
    // than merely skipping the sample: skipping would let a critical run that started BEFORE the
    // download resume its clock afterwards and fire mid-install, which is the one reboot that
    // could leave a half-written slot.
    if (s.ota_busy || s.largest_block >= kHeapCriticalBytes) {
        if (!w.critical) return {HeapAction::Ok, 0, false};
        // Ending a run is worth a line: it is the difference between "the device healed itself"
        // and a silence the reader would have to interpret.
        const uint32_t ran = static_cast<uint32_t>(s.now_ms - w.since_ms);
        // Excused means the OTA is what saved it — the heap itself is still below the threshold.
        // An OTA running while the heap is healthy again is an ordinary recovery, not an excusal.
        const bool excused = s.ota_busy && s.largest_block < kHeapCriticalBytes;
        w.critical = false;
        return {HeapAction::Recovered, ran, excused};
    }
    if (!w.critical) {
        w.critical = true;
        w.since_ms = s.now_ms;
        return {HeapAction::Armed, 0, false};
    }
    // Unsigned subtraction, so a 32-bit millisecond wrap inside the hold window still measures
    // the true elapsed time instead of going hugely negative and firing instantly.
    const uint32_t held = static_cast<uint32_t>(s.now_ms - w.since_ms);
    if (held >= kHeapCriticalHoldMs) return {HeapAction::Restart, held, false};
    return {HeapAction::Watching, held, false};
}

}  // namespace tk
