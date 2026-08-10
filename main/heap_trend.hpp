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
// Static storage, never heap: the binding limit on this chip is the largest CONTIGUOUS free block,
// so a diagnostic that allocated would compete for the very resource it measures. ~1.2 KB, fixed.
//
// FLASH-FREE, BUT NOT REBOOT-FREE. Persisting the ring to NVS stays rejected — it would rewrite a
// blob every five minutes into the partition that holds the WiFi credentials and the private key,
// ~100k writes a year for an artifact whose whole value is the last day. But the ring must survive
// a RESTART, because the heap watchdog's answer to exhaustion is a restart: in .bss the trend was
// erased by the one event it exists to explain. It therefore lives in .noinit — DRAM the startup
// code neither loads nor zeroes — which survives every reset that kept power (deliberate restart,
// panic, task watchdog, OTA reboot) at zero flash cost and zero extra RAM. A power cut still clears
// it, and the retained image must pass a CRC and a layout fingerprint before it is believed;
// mechanics in logic/heap_history.hpp.

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
// every later reader. Returns the number of samples written to each.
//   out_bucket0     — the bucket index of sample zero, so a consumer can place the series on a clock
//   out_boot_bucket — the bucket THIS boot started in. Since the ring now spans restarts, the two
//                     together are what let a reader mark where the device rebooted; without it a
//                     retained trend reads as one uninterrupted run, and the clock is no longer
//                     uptime/dt so an age computed from uptime alone would silently be wrong.
size_t heap_trend_snapshot(HeapTrendSample* free_out, HeapTrendSample* largest_out, size_t max,
                           uint32_t* out_bucket0, uint32_t* out_boot_bucket);

// The ring's cadence, in seconds — served alongside the samples so a consumer never has to hardcode
// the spacing it is drawing.
constexpr uint32_t heap_trend_dt_s() { return kHeapHistoryDtS; }

}  // namespace tk
