#pragma once

#include <string>
#include <functional>

// In-memory diagnostic log. Installs an esp_log hook so everything the device
// prints to the serial console is also captured into a fixed buffer, retrievable
// over HTTP (GET /diag) for analysis without a USB cable attached. The buffer
// WRAPS: once full, the oldest bytes are overwritten, so it always holds the most
// recent output — best for watching an event happen live. Reboot or ?clear=1 resets it.
void        diag_log_init();          // install the esp_log capture hook (call early)

// Stream the captured log to a sink WITHOUT building one big std::string. Dumping the
// whole buffer as a single contiguous allocation could throw std::bad_alloc when it
// exceeds the largest free block on a fragmented heap, so /diag must not allocate the
// whole dump at once. `sink(ptr, len)` is called with one or two spans pointing straight
// into the static ring buffer; return false to stop early.
void        diag_log_dump_chunks(const std::function<bool(const char*, size_t)>& sink);
void        diag_log_clear();

// Verbose mode gates extra-noisy diagnostics (e.g. the raw BLE RX hex dump) that
// would otherwise spam the buffer during normal operation. Off by default.
void diag_set_verbose(bool on);
bool diag_verbose();
