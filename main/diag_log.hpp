#pragma once

#include <string>

// In-memory diagnostic log. Installs an esp_log hook so everything the device
// prints to the serial console is also captured into a fixed buffer, retrievable
// over HTTP (GET /diag) for analysis without a USB cable attached. The buffer
// holds output from boot onward and does not wrap: once full it stops capturing
// (newest output dropped) rather than discarding the boot history.
void        diag_log_init();          // install the esp_log capture hook (call early)
std::string diag_log_dump();          // boot→newest captured output
void        diag_log_clear();

// Verbose mode gates extra-noisy diagnostics (e.g. the raw BLE RX hex dump) that
// would otherwise spam the buffer during normal operation. Off by default.
void diag_set_verbose(bool on);
bool diag_verbose();
