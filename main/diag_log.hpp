#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// In-memory diagnostic log. Installs an esp_log hook so everything the device
// prints to the serial console is also captured into a fixed buffer, retrievable
// over HTTP (GET /diag) for analysis without a USB cable attached. The buffer
// WRAPS: once full, the oldest bytes are overwritten, so it always holds the most
// recent output — best for watching an event happen live. Reboot or ?clear=1 resets it.
// Boot owner: call before MQTT/HTTP. Sequential retries never chain the hook to itself.
// Known MQTT URI parse errors are protected even when the optional ring mutex cannot allocate.
void        diag_log_init();
// Capture attempts lost to unavailable/contended ring, since boot (modulo 2^32). Clear retains it.
uint32_t diag_log_dropped_lines() noexcept;

// Stream the captured log to a sink WITHOUT building one big std::string. Dumping the
// whole buffer as a single contiguous allocation could throw std::bad_alloc when it
// exceeds the largest free block on a fragmented heap, so /diag must not allocate the
// whole dump at once. Fixed-size pieces are copied while the ring mutex is held, then
// `sink(ptr, len)` runs after the mutex is released: network I/O and throwing code never
// execute under the shared lock. Concurrent writers may overwrite bytes already delivered; if
// they reach an unread snapshot byte (or the log is cleared), the dump stops before mixing ring
// generations. Return false from the sink to stop early.
enum class DiagDumpStart {
    IncludeSnapshotStart,
    AfterWrappedLineBoundary,
};

enum class DiagDumpResult {
    Complete,
    SinkFailed,
    SnapshotInvalidated,
};

// A wrapped byte ring begins at the oldest retained BYTE, which may be the tail of a VIN/SSID/MAC
// after its line marker was overwritten. Redacted exports must request AfterWrappedLineBoundary:
// it suppresses bytes through the first retained '\n' before invoking the sink. Unwrapped rings
// start at the capture epoch and retain their first line.
using DiagDumpSinkFn = bool (*)(void* ctx, const char* data, size_t len);

DiagDumpResult diag_log_dump_chunks(
    DiagDumpSinkFn sink,
    void* ctx,
    DiagDumpStart start = DiagDumpStart::IncludeSnapshotStart);

template <typename Fn>
inline DiagDumpResult diag_log_dump_chunks(
    Fn&& sink,
    DiagDumpStart start = DiagDumpStart::IncludeSnapshotStart) {
    auto* fn = &sink;
    return diag_log_dump_chunks(
        [](void* ctx, const char* data, size_t len) -> bool {
            return (*static_cast<decltype(fn)>(ctx))(data, len);
        },
        const_cast<void*>(static_cast<const void*>(fn)), start);
}
void        diag_log_clear();

// Verbose mode gates extra-noisy diagnostics (e.g. the raw BLE RX hex dump) that
// would otherwise spam the buffer during normal operation. Off by default.
void diag_set_verbose(bool on);
bool diag_verbose();
