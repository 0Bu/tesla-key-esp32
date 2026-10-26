#pragma once

// Pure stack-sample encoding shared by stack_watch.cpp and host tests. Values are conservative
// lower bounds, never exact byte minima. Zero/unknown remain distinct. No RTOS or atomics here.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace tk::stack_sample {

inline constexpr unsigned kLaneBits = 6;
inline constexpr unsigned kLanesPerWord = 5;
inline constexpr unsigned kWordCount = 6;
inline constexpr unsigned kCapacity = kLanesPerWord * kWordCount;
inline constexpr uint32_t kQuantumBytes = 256;
inline constexpr uint8_t kMaximumCode = 63;
inline constexpr uint32_t kSaturationBytes = (kMaximumCode - 1) * kQuantumBytes;
inline constexpr uint32_t kLaneMask = kMaximumCode;
inline constexpr uint32_t kUsedWordMask = UINT32_C(0x3fffffff);
using Snapshot = std::array<uint32_t, kWordCount>;

struct Sample {
    bool sampled{false};
    bool saturated{false};
    uint32_t lower_bound_bytes{0};
};

// Code zero is absent. Code one represents a measured value below one quantum,
// including an actual zero. Large measurements remain conservative lower bounds.
inline constexpr uint8_t encode(uint32_t free_bytes) noexcept {
    const uint32_t units = free_bytes / kQuantumBytes;
    return units >= kMaximumCode - 1 ? kMaximumCode : static_cast<uint8_t>(units + 1);
}

inline constexpr Sample decode(uint8_t code) noexcept {
    if (code == 0 || code > kMaximumCode) return {};
    return {true, code == kMaximumCode,
            static_cast<uint32_t>(code - 1) * kQuantumBytes};
}

// Merge only one lane, preserving all other lanes and reserved bits. The shell must
// apply this with strong compare-exchange and recompute after every failed exchange.
// A load/store pair would lose samples written concurrently into a different lane.
inline constexpr uint32_t merge(uint32_t previous, unsigned lane,
                                uint32_t free_bytes) noexcept {
    if (lane >= kLanesPerWord) return previous;
    const unsigned shift = lane * kLaneBits;
    const uint32_t old_code = (previous >> shift) & kLaneMask;
    const uint32_t new_code = encode(free_bytes);
    if (old_code != 0 && old_code <= new_code) return previous;
    const uint32_t mask = kLaneMask << shift;
    return (previous & ~mask) | (new_code << shift);
}

// Firmware instantiates this with std::atomic<uint32_t>. A passive atomic double can force
// competing updates in host tests; both use this single CAS/recompute program.
template<class AtomicWord>
inline void publish_minimum(AtomicWord& word, unsigned lane, uint32_t free_bytes) noexcept {
    uint32_t previous = word.load(std::memory_order_relaxed);
    for (;;) {
        const uint32_t next = merge(previous, lane, free_bytes);
        if (next == previous || word.compare_exchange_strong(previous, next,
                std::memory_order_relaxed, std::memory_order_relaxed)) return;
    }
}

inline constexpr Sample read(const Snapshot& snapshot, unsigned slot) noexcept {
    if (slot >= kCapacity) return {};
    const unsigned word = slot / kLanesPerWord;
    const unsigned shift = (slot % kLanesPerWord) * kLaneBits;
    return decode(static_cast<uint8_t>((snapshot[word] >> shift) & kLaneMask));
}

inline constexpr bool any_sample(const Snapshot& snapshot) noexcept {
    for (uint32_t word : snapshot) {
        if ((word & kUsedWordMask) != 0) return true;
    }
    return false;
}

static_assert(kLanesPerWord * kLaneBits <= 32);
static_assert(sizeof(Snapshot) == 24);
static_assert(kSaturationBytes == 15872);

}  // namespace tk::stack_sample
