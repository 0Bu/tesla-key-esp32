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
// log_adoption() below turns into a permanent "starting empty" — that is the symptom to look for.
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

enum class Adoption { None, Retained, Empty };

// Decide under the trend mutex; log only the fixed outcome after it is released.
Adoption adopt_or_reset() {
    if (s_ready) return Adoption::None;
    const bool retained = heap_persist_valid(persist());
    if (retained) {
        s_carry_s = heap_persist_next_carry(persist().ring);
    } else {
        persist().ring.reset();
        s_carry_s = 0;
    }
    seal();
    s_ready = true;
    return retained ? Adoption::Retained : Adoption::Empty;
}

void log_adoption(Adoption adopted) {
    if (adopted == Adoption::None) return;
    ESP_LOGI(TAG, "memory trend: %s", adopted == Adoption::Retained ? "retained" : "starting empty");
}

// Function-local static initialization serializes first use between the recorder and readers.
// Either may arrive first; both validate retained storage under the same mutex.
SemaphoreHandle_t mutex() {
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}

}  // namespace

void heap_trend_record(uint32_t monotonic_s, uint32_t free_bytes, uint32_t largest_bytes) {
    SemaphoreHandle_t m = mutex();
    if (!m) return;   // no mutex, no trend — a diagnostic must never be the reason a boot fails
    Adoption adopted{};
    {
        SemGuard g(m);
        adopted = adopt_or_reset();
        // The carry places this boot's uptime on the retained timeline.
        persist().ring.record(monotonic_s + s_carry_s, free_bytes, largest_bytes);
        seal();
    }
    log_adoption(adopted);
}

size_t heap_trend_snapshot(HeapTrendSample* free_out, HeapTrendSample* largest_out, size_t max,
                           uint32_t* out_bucket0, uint32_t* out_boot_bucket) {
    if (!free_out || !largest_out || max == 0) return 0;
    SemaphoreHandle_t m = mutex();
    if (!m) return 0;
    Adoption adopted{};
    size_t n = 0;
    {
        SemGuard g(m);
        // An early reader must never serve unvalidated retained bytes after power-on.
        adopted = adopt_or_reset();
        // Both series must describe one instant under the same lock.
        n = persist().ring.snapshot_free(free_out, max);
        (void)persist().ring.snapshot_largest(largest_out, max);
        if (out_bucket0)     *out_bucket0     = persist().ring.bucket0();
        if (out_boot_bucket) *out_boot_bucket = s_carry_s / kHeapHistoryDtS;
    }
    log_adoption(adopted);
    return n;
}

}  // namespace tk
