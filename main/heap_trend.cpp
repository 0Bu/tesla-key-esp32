// Storage + mutex for the board's 24-hour memory trend (see heap_trend.hpp). Every mechanic is in
// the pure logic/heap_history.hpp; nothing here decides anything.
#include "heap_trend.hpp"

#include "rtos_guard.hpp"

#include <esp_attr.h>
#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace tk {
namespace {

const char* TAG = "heap_trend";

// .noinit — the one section the ESP-IDF startup code neither loads nor zeroes (it is NOBITS, and
// startup zeroes only .bss), so this storage holds whatever the previous run left in it across any
// reset that KEPT POWER: a deliberate restart, a panic, the task watchdog, an OTA reboot. That is
// the whole point — the heap watchdog's answer to exhaustion IS a restart, so a trend in .bss would
// be erased by the single event it exists to explain. Costs no flash writes and no extra RAM: the
// same ~1.2 KB, in a different section.
//
// WHY A RAW BUFFER AND NOT `__NOINIT_ATTR HeapPersist s_persist;`. That obvious spelling was
// written first and is WRONG in a way nothing at runtime would report. HeapRing has default member
// initialisers, so a namespace-scope HeapPersist is default-INITIALISED: GCC emits a real
// `HeapRing::HeapRing()` call into the TU's static-initialisation function, which runs on every
// boot and zeroes the ring before app_main. The section is retained, the CRC would then be
// recomputed over a blank ring, every boot would honestly report "starting empty", and the feature
// would be dead while looking present. Verified on the built image, not assumed:
//
//   riscv32-esp-elf-objdump -d --section=.text._Z41__static_initialization_and_destruction_0v
//       build/esp-idf/main/CMakeFiles/__idf_main.dir/heap_trend.cpp.obj
//
// must print NOTHING for this file. A plain byte array has no constructor to emit, which is what
// makes that true here; HeapPersist is standard-layout and trivially destructible (asserted in
// logic/heap_history.hpp) so reading the bytes back through it is well-defined in practice and the
// object is never destroyed. If this ever grows a constructor again, the syslog line in
// adopt_or_reset() below turns into a permanent "starting empty" — that is the symptom to look for.
__NOINIT_ATTR alignas(HeapPersist) uint8_t s_persist_raw[sizeof(HeapPersist)];

inline HeapPersist& persist() { return *reinterpret_cast<HeapPersist*>(s_persist_raw); }

// Derived at adopt time, never retained: it is a pure function of the adopted ring, so storing it
// would be a second copy of the same fact that could disagree with the first.
uint32_t s_carry_s = 0;
bool     s_ready   = false;

// Re-CRC the retained image so the NEXT boot can trust it. Runs on every record() — 1.2 KB of
// table-free CRC-32 once per sampling cycle (~30 s), which is nothing, and the alternative (sealing
// on a timer, or at shutdown) would leave the image unsealed at exactly the moments that matter:
// a panic and a watchdog reset do not run shutdown handlers.
void seal() {
    persist().magic       = kHeapPersistMagic;
    persist().fingerprint = heap_persist_fingerprint();
    persist().crc         = heap_persist_crc(persist());
}

// Adopt the retained ring, or start empty. Runs once, from the first record().
void adopt_or_reset() {
    if (heap_persist_valid(persist())) {
        s_carry_s = heap_persist_next_carry(persist().ring);
        ESP_LOGI(TAG, "adopted the retained memory trend: %u samples, continuing at bucket %u",
                 (unsigned) persist().ring.count(), (unsigned) (s_carry_s / kHeapHistoryDtS));
    } else {
        // A power-on (the section holds noise), a first boot, or an OTA that changed the ring's
        // geometry. All three are the same instruction: begin an empty, correctly-shaped trend
        // rather than draw a chart out of bytes we cannot vouch for.
        //
        // Logged, not silent: this line is the only external evidence that retention works at all.
        // Seeing it after a plain restart — rather than the "adopted" line above — is the symptom
        // of the retained storage having been zeroed before app_main ran.
        ESP_LOGI(TAG, "no usable retained memory trend (power-on, first boot, or a changed ring "
                      "layout) — starting empty");
        persist().ring.reset();
        s_carry_s = 0;
    }
    seal();
    s_ready = true;
}

// Created on first use rather than in an initializer, because this file has no init hook and the
// first caller (loop_task) runs long after the scheduler starts. The double-check is not a race
// worth guarding beyond this: heap_trend_record() has exactly one caller task, and it necessarily
// runs before any HTTP request can ask for a snapshot (loop_task starts inside
// VehicleController::init, the HTTP server after it).
SemaphoreHandle_t mutex() {
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}

}  // namespace

void heap_trend_record(uint32_t monotonic_s, uint32_t free_bytes, uint32_t largest_bytes) {
    SemaphoreHandle_t m = mutex();
    if (!m) return;   // no mutex, no trend — a diagnostic must never be the reason a boot fails
    SemGuard g(m);
    if (!s_ready) adopt_or_reset();
    // The caller passes plain uptime and knows nothing about retention. The carry turns that into
    // the ONE continuous timeline the retained ring is already on.
    persist().ring.record(monotonic_s + s_carry_s, free_bytes, largest_bytes);
    seal();
}

size_t heap_trend_snapshot(HeapTrendSample* free_out, HeapTrendSample* largest_out, size_t max,
                           uint32_t* out_bucket0, uint32_t* out_boot_bucket) {
    if (!free_out || !largest_out || max == 0) return 0;
    SemaphoreHandle_t m = mutex();
    if (!m) return 0;
    SemGuard g(m);
    // Adopt here too, not only in record(). Before this existed the ring was .bss and an early
    // reader simply saw an empty one; now an unadopted read would serve the RETAINED bytes, which
    // on a power-on boot are noise — a diagnostic reporting a fabricated 24 hours of memory. The
    // ordering that makes this unreachable (loop_task starts before the HTTP server) is a fact
    // about other files, and this is the file that pays if it changes.
    if (!s_ready) adopt_or_reset();
    // Both copies happen under ONE lock so the two series describe the same instant. Taking the
    // lock twice would let a record() land between them and shift one line by a bucket relative to
    // the other — which, on a chart whose entire purpose is the GAP between the two lines, would
    // manufacture exactly the fragmentation signal a reader is looking for.
    const size_t n = persist().ring.snapshot_free(free_out, max);
    (void)persist().ring.snapshot_largest(largest_out, max);
    if (out_bucket0)     *out_bucket0     = persist().ring.bucket0();
    if (out_boot_bucket) *out_boot_bucket = s_carry_s / kHeapHistoryDtS;
    return n;
}

}  // namespace tk
