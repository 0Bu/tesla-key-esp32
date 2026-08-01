#pragma once
// A 24-hour trend of this board's OWN memory — free heap and largest contiguous block, on one
// clock. Pure, IDF-free, host-testable; storage, the sampling task and the JSON serialisation stay
// at the call site.
//
// WHY THIS EXISTS. `/status` reports free_heap and largest_block as two spot numbers, and a spot
// number cannot answer the only question worth asking about a heap on this device: is it DRIFTING?
// A leak is a SLOPE — invisible in any single sample, obvious over hours. Fragmentation is the two
// lines SEPARATING — total free holding steady while the largest contiguous block sinks under it,
// which is the exact shape of the wedge documented in logic/heap_watchdog.hpp (free ~16 KB, i.e.
// entirely plausible, while largest_block sat at 544-1536 B). Reading those two numbers once tells
// you the level; reading them against each other over a day tells you what is happening, and this
// firmware's dominant failure mode is precisely the thing only the second reading can see.
//
// WHY IT BELONGS BESIDE THE WATCHDOG. logic/heap_watchdog.hpp fires on largest_block under 4 KB
// held unbroken for five minutes — it is the ESCALATION, and by the time it speaks the device has
// already been unusable for five minutes and is about to restart. This is the INSTRUMENT for the
// hours before that: the same quantity, sampled and kept, so the approach is diagnosable instead of
// being reconstructed from a `heap:<n>` breadcrumb after the fact. Sample the same INTERNAL
// largest_block the watchdog samples (heap_caps_get_largest_free_block(8BIT|INTERNAL)) — a plain
// 8BIT query on a board with PSRAM reports that too and would draw a flat, healthy, meaningless
// line on the one target with the extra RAM.
//
// WHY IT MUST BE A FIXED STRUCT IN STATIC STORAGE. The binding limit on this chip is the largest
// CONTIGUOUS free block, not total free heap — which is the exact resource this ring measures, so
// an instrument that allocated would be competing with its own subject. A `static HeapRing` does
// not compete for a contiguous block at all: it is carved out of the image's data segments before
// the allocator exists. Hence no heap, no std::vector, no growth: one 1152-byte pair of arrays,
// sized at compile time and asserted below. Every member zero-initialises deliberately (the
// "nothing folded yet" state is a flag, not a non-zero sentinel), so the whole object lands in
// .bss and costs nothing in the flash image either.
#include <cstddef>
#include <cstdint>

namespace tk {

// ── Ring geometry ───────────────────────────────────────────────────────────────────────────────
// One sample per bucket, 288 buckets deep: 288 x 300 s = 24 h exactly. Five minutes is chosen
// against the SIGNAL rather than the memory — a leak worth calling one moves over hours, and the
// watchdog's own trigger is a five-minute unbroken hold, so a finer raster would buy noise and a
// coarser one could hide a whole trigger window inside a single sample.
inline constexpr uint32_t kHeapHistoryDtS     = 300;
inline constexpr size_t   kHeapHistorySamples = 288;

// Samples are TENTHS OF A KiB, as int16. Tenths because that keeps the wire value exact without
// floats anywhere in the firmware or the payload (a consumer divides by 10) at ~102-byte
// resolution, which is finer than anything readable off a 24-hour chart; int16 because two 288-entry
// arrays of it are 1152 bytes total, small enough that the whole instrument sits in .bss and needs
// no budget discussion.
//
// The range is 0 .. 3276.7 KiB. That covers every INTERNAL-DRAM figure any target here can report
// and is therefore not a real ceiling — but a sample is CLAMPED, never wrapped: a clamped reading
// draws a flat line at the top and reads as one, while a wrapped uint32 would draw a plausible
// small number, i.e. it would invent exactly the exhaustion this trend exists to detect. If a
// caller ever feeds this a PSRAM-inclusive figure, change the unit rather than the clamp.
using HeapTrendSample = int16_t;

// "No sample in this bucket." A sentinel rather than a parallel validity array: the array would be
// a second structure to keep in step with the ring's own wrap-around, and the two going out of step
// is invisible (a gap becomes a value, or a value becomes a gap). It is collision-proof by
// construction here in a way a general trend ring cannot be — the input is a byte COUNT, so every
// real sample is >= 0 and no measurement can ever land on INT16_MIN.
inline constexpr HeapTrendSample kHeapTrendNoSample = INT16_MIN;

inline constexpr bool heap_trend_absent(HeapTrendSample s) { return s == kHeapTrendNoSample; }

// A byte count as the ring stores it. 64-bit intermediate on purpose: `bytes * 10` overflows a
// uint32 at ~429 MB, and the wrap would be a small, entirely believable number.
inline constexpr HeapTrendSample heap_tenths_kib(uint32_t bytes) {
    const uint64_t tenths = (static_cast<uint64_t>(bytes) * 10u + 512u) / 1024u;
    return tenths > static_cast<uint64_t>(INT16_MAX) ? static_cast<HeapTrendSample>(INT16_MAX)
                                                     : static_cast<HeapTrendSample>(tenths);
}

// ── The ring ────────────────────────────────────────────────────────────────────────────────────
// Bucket folding, wrap-around ordering and skipped-bucket filling live here, in a header the host
// build can run, because they are exactly the off-by-one that has no symptom until someone reads a
// chart three weeks later and it is quietly wrong about WHEN. A rule of this shape buried in a .cpp
// can only be verified on the device, which is to say not verified.
//
// BOTH SERIES SHARE ONE RING AND ONE BUCKET CLOCK. That is the whole design: the diagnosis is a
// comparison ("the two lines separate"), so sample i of one series must describe the same five
// minutes as sample i of the other. Two independent rings could drift by a bucket after a skipped
// cycle and the separation would then be an artefact of the instrument.
struct HeapRing {
    // Fold one measurement into the currently open bucket, advancing the ring when the monotonic
    // clock has moved into a later one. Call it on a fixed cadence; the bucket is derived from the
    // timestamp rather than from a sample count, so the cadence can change without moving the
    // 5-minute raster.
    void record(uint32_t monotonic_s, uint32_t free_bytes, uint32_t largest_bytes) {
        const uint32_t b = monotonic_s / kHeapHistoryDtS;
        if (!open_) {
            open_        = true;
            open_bucket_ = b;
        } else if (b > open_bucket_) {
            // How many whole buckets went by with no record() at all — a stall, an OTA holding the
            // CPU, a task that stopped sampling. Never negative: a `b` that is not ahead of the
            // open bucket folds into it instead, so a clock that fails to advance can no longer
            // produce a huge unsigned wrap that blanks the entire ring on one cycle.
            commit(b - open_bucket_ - 1u);
            open_bucket_ = b;
        }
        fold(heap_tenths_kib(free_bytes), heap_tenths_kib(largest_bytes));
    }

    // Samples held, oldest-first, saturating at kHeapHistorySamples. The OPEN bucket is not counted
    // — it is still being folded, so publishing it would show a partial five minutes beside 287
    // complete ones.
    size_t count() const { return count_; }

    // The monotonic bucket index of sample 0, so a consumer can put the series on a clock (a wall
    // clock at serve time, or simply an age). It is DERIVED, never stored: every bucket between the
    // oldest sample and the open one occupies exactly one slot — which is only true because skipped
    // buckets are filled rather than compressed (see commit) — so `open_bucket_ - count_` is the
    // answer by construction and cannot drift out of step with the ring the way a second stored
    // field would. With no samples yet it names the bucket sample 0 will land in.
    uint32_t bucket0() const { return open_bucket_ - static_cast<uint32_t>(count_); }

    // Copy the free-heap series out, oldest sample first, absences as kHeapTrendNoSample. Returns how
    // many were written (min of count() and `max`).
    size_t snapshot_free(HeapTrendSample* out, size_t max) const { return copy_(free_, out, max); }

    // The same for the largest contiguous block. Same length, same instants, same absences.
    size_t snapshot_largest(HeapTrendSample* out, size_t max) const { return copy_(largest_, out, max); }

    void reset() {
        count_       = 0;
        head_        = 0;
        open_        = false;
        open_bucket_ = 0;
        pend_seen_   = false;
        pend_free_   = 0;
        pend_large_  = 0;
    }

    // ── internals (public so the struct stays a plain aggregate in .bss; treat as private) ──
    HeapTrendSample free_[kHeapHistorySamples]    = {};
    HeapTrendSample largest_[kHeapHistorySamples] = {};
    uint32_t   open_bucket_ = 0;   // monotonic bucket index currently being folded
    uint16_t   count_       = 0;   // committed samples held
    uint16_t   head_        = 0;   // next write slot
    HeapTrendSample pend_free_   = 0;   // the open bucket's running value…
    HeapTrendSample pend_large_  = 0;
    bool       open_        = false;      // has record() ever run (bucket 0 is a real bucket)
    bool       pend_seen_   = false;      // …and whether anything has been folded into it yet

    // MINIMUM wins the bucket, not the last reading. This is a deliberate departure from the usual
    // last-value fold, and the reason is what the series is for: on a resource ceiling the
    // interesting event is the DIP, and a dip shorter than the raster is precisely the early warning
    // that precedes the five-minute unbroken hold the watchdog restarts on. A last-value fold would
    // drop it silently — the curve would look calm right up to the moment the device restarted.
    // The two minima are taken independently and that stays coherent: largest <= free holds at
    // every instant, so it holds between their minima too, and no bucket can claim a largest block
    // bigger than the free heap containing it.
    void fold(HeapTrendSample f, HeapTrendSample l) {
        if (!pend_seen_) {
            pend_seen_  = true;
            pend_free_  = f;
            pend_large_ = l;
            return;
        }
        if (f < pend_free_)  pend_free_  = f;
        if (l < pend_large_) pend_large_ = l;
    }

    // Close the open bucket and open the next. `skipped` buckets went by with no sample at all and
    // are FILLED with the sentinel, never compressed: compressing them would slide every earlier
    // sample forward in time, so a 24-hour chart would be wrong about WHEN with nothing on it to
    // say so — and bucket0() above, which reads the ring's time base straight off the sample count,
    // would silently start lying too. The fill is capped at one full ring, which stays honest: past
    // that every slot is a sentinel anyway, and bucket0() then names them as the most recent 288
    // empty buckets, which is exactly what they are.
    void commit(uint32_t skipped) {
        push(pend_seen_ ? pend_free_ : kHeapTrendNoSample, pend_seen_ ? pend_large_ : kHeapTrendNoSample);
        for (uint32_t k = 0; k < skipped && k < kHeapHistorySamples; k++)
            push(kHeapTrendNoSample, kHeapTrendNoSample);
        pend_seen_  = false;
        pend_free_  = 0;
        pend_large_ = 0;
    }

    void push(HeapTrendSample f, HeapTrendSample l) {
        free_[head_]    = f;
        largest_[head_] = l;
        head_ = static_cast<uint16_t>((head_ + 1u) % kHeapHistorySamples);
        if (count_ < kHeapHistorySamples) count_++;
    }

    // Oldest-first copy. Until the ring wraps the oldest sample is slot 0; after that it is `head_`
    // — the slot about to be overwritten is also the one holding the oldest sample.
    size_t copy_(const HeapTrendSample* src, HeapTrendSample* out, size_t max) const {
        if (!out) return 0;
        const size_t n      = (count_ < max) ? count_ : max;
        const size_t oldest = (count_ < kHeapHistorySamples) ? 0u : head_;
        for (size_t i = 0; i < n; i++) out[i] = src[(oldest + i) % kHeapHistorySamples];
        return n;
    }
};

// The memory budget, asserted where it is stated rather than trusted to a comment. Two series x 288
// samples x 2 bytes is 1152 B of ring; the ceiling leaves room for the handful of counters around
// it and for nothing else. A future change that adds a third series or a wider sample meets this
// here, on a device whose binding limit is the very quantity being buffered — and the answer is
// then a deliberate raise with the arithmetic in hand, not a silent one.
static_assert(sizeof(HeapRing) <= 1536,
              "the heap trend is static storage on a heap-tight board — justify the growth "
              "before raising this");
static_assert(2 * kHeapHistorySamples * sizeof(HeapTrendSample) == 1152, "ring cost changed");
static_assert(kHeapHistorySamples * kHeapHistoryDtS == 86400, "the trend is meant to span 24 h");

}  // namespace tk
