#pragma once

#include <string>
#include <functional>

// In-memory diagnostic log. Installs an esp_log hook so everything the device
// prints to the serial console is also captured into a fixed buffer, retrievable
// over HTTP (GET /diag) for analysis without a USB cable attached. The buffer
// holds output from boot onward and does not wrap: once full it stops capturing
// (newest output dropped) rather than discarding the boot history.
void        diag_log_init();          // install the esp_log capture hook (call early)
std::string diag_log_dump();          // boot→newest captured output (allocates the whole log)

// Stream the captured log to a sink WITHOUT building one big std::string. The full
// buffer is ~48 KB; on a fragmented heap a single contiguous 48 KB allocation throws
// std::bad_alloc (the device's largest free block can fall to ~31 KB), so /diag must
// not allocate the whole dump at once. `sink(ptr, len)` is called with one or two
// spans pointing straight into the static ring buffer; return false to stop early.
void        diag_log_dump_chunks(const std::function<bool(const char*, size_t)>& sink);
void        diag_log_clear();

// Verbose mode gates extra-noisy diagnostics (e.g. the raw BLE RX hex dump) that
// would otherwise spam the buffer during normal operation. Off by default.
void diag_set_verbose(bool on);
bool diag_verbose();
