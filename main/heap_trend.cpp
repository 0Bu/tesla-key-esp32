// Storage + mutex for the board's 24-hour memory trend (see heap_trend.hpp). Every mechanic is in
// the pure logic/heap_history.hpp; nothing here decides anything.
#include "heap_trend.hpp"

#include "rtos_guard.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace tk {
namespace {

// .bss — see the header. Zero-initialised by construction, so it costs nothing in the image either.
HeapRing s_ring;

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
    s_ring.record(monotonic_s, free_bytes, largest_bytes);
}

size_t heap_trend_snapshot(HeapTrendSample* free_out, HeapTrendSample* largest_out, size_t max,
                           uint32_t* out_bucket0) {
    if (!free_out || !largest_out || max == 0) return 0;
    SemaphoreHandle_t m = mutex();
    if (!m) return 0;
    SemGuard g(m);
    // Both copies happen under ONE lock so the two series describe the same instant. Taking the
    // lock twice would let a record() land between them and shift one line by a bucket relative to
    // the other — which, on a chart whose entire purpose is the GAP between the two lines, would
    // manufacture exactly the fragmentation signal a reader is looking for.
    const size_t n = s_ring.snapshot_free(free_out, max);
    (void)s_ring.snapshot_largest(largest_out, max);
    if (out_bucket0) *out_bucket0 = s_ring.bucket0();
    return n;
}

}  // namespace tk
