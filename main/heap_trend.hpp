#pragma once

#include "logic/heap_history.hpp"

#include <cstddef>
#include <cstdint>

// Storage + locking for the board's own 24-hour memory trend. The ring mechanics and the sample
// encoding are the pure, host-tested logic/heap_history.hpp; this file owns the one instance, the
// mutex around it, and the sampling entry point.
//
// WHY A TREND AND NOT A NUMBER. `/status` reports free heap, low-water and largest-block as spot
// values, and a spot value cannot answer the question that actually matters on this device: is the
// heap DRIFTING? A leak is a slope. Fragmentation is the two lines separating — total free holding
// steady while the largest contiguous block sinks toward the 4 KB floor at which the heap watchdog
// fires. Both are invisible in any single sample, and both are exactly what preceded the ten-hour
// wedge that made the watchdog necessary. This is the instrument that shows them coming.
//
// .bss, never heap: the binding limit on this chip is the largest CONTIGUOUS free block, so a
// diagnostic that allocated would compete for the very resource it measures. ~1.2 KB, fixed.
//
// RAM-only on purpose. Persisting it would mean rewriting a blob every five minutes in the same
// flash partition that holds the WiFi credentials and the private key — ~100k writes a year against
// an artifact whose whole value is the last day. A reboot empties it, and the UI draws the span it
// actually has rather than padding a 24-hour axis with absence.

namespace tk {

// Sample both heap figures into the current bucket. Called from loop_task, which already samples
// the same two numbers for the heap watchdog — so the trend costs no extra heap_caps call on the
// hot path and, more importantly, cannot disagree with what the watchdog saw.
//   monotonic_s   — seconds since boot (the ring's clock; deliberately NOT the wall clock, which
//                   jumps when SNTP lands mid-boot and would fold two buckets into one).
//   free_bytes    — total free, MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL
//   largest_bytes — largest contiguous free block, same caps (the number the watchdog gates on)
void heap_trend_record(uint32_t monotonic_s, uint32_t free_bytes, uint32_t largest_bytes);

// Copy the two series out, oldest sample first, into CALLER-OWNED buffers — no allocation happens
// under the lock, which is the rule that keeps an OOM here from stranding the mutex and wedging
// every later reader. Returns the number of samples written to each; `out_bucket0` receives the
// monotonic bucket index of sample zero so a consumer can place the series on a clock.
size_t heap_trend_snapshot(HeapTrendSample* free_out, HeapTrendSample* largest_out, size_t max,
                           uint32_t* out_bucket0);

// The ring's cadence, in seconds — served alongside the samples so a consumer never has to hardcode
// the spacing it is drawing.
constexpr uint32_t heap_trend_dt_s() { return kHeapHistoryDtS; }

}  // namespace tk
