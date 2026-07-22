#include "diag_log.hpp"
#include "syslog.hpp"

#include <esp_log.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rtos_guard.hpp"

// Ring buffer of console output. It WRAPS: once full, the oldest bytes are
// overwritten so the log always holds the MOST RECENT ~16 KB. This is what you
// want for live debugging (watching a pairing or key-deletion happen now) — the
// earlier non-wrapping design kept the boot prologue but then stopped capturing,
// so a long-running device showed stale output and missed the event of interest.
// reboot or /diag?clear=1 to reset.
//
// Sized at 16 KB, not 48 KB: this is a STATIC .bss buffer, so its size comes
// straight off the DRAM heap budget. At 48 KB it was the single largest static
// consumer in our code and pushed the heap start up, leaving the largest free block
// at only ~31 KB — too small for e.g. the OTA TLS record buffers. 16 KB still holds a
// solid burst of recent lifecycle output and frees 32 KB back to the heap.
static constexpr size_t DIAG_CAP = 16 * 1024;
static char              s_buf[DIAG_CAP];
static size_t            s_head    = 0;      // next write position
static bool              s_wrapped = false;  // buffer has wrapped at least once
static SemaphoreHandle_t s_mtx     = nullptr;
static vprintf_like_t    s_prev    = nullptr;
// atomic (not volatile): written by the /diag HTTP handler, read by the capture hook on
// whichever task logged the line. A cross-task scalar — atomic gives it a defined value.
static std::atomic<bool> s_verbose{false};

static void diag_append_(const char* data, size_t len) {
    if (!s_mtx || len == 0) return;
    tk::SemGuard g(s_mtx, pdMS_TO_TICKS(20));  // bounded: never stall the logging task
    if (!g) return;
    for (size_t i = 0; i < len; i++) {
        s_buf[s_head++] = data[i];
        if (s_head >= DIAG_CAP) { s_head = 0; s_wrapped = true; }
    }
}

// esp_log hook: capture the formatted line, then forward to the original sink so
// the USB serial console keeps working unchanged. Must not call ESP_LOG itself.
static int diag_vprintf_(const char* fmt, va_list ap) {
    char line[256];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof(line), fmt, ap2);
    va_end(ap2);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        diag_append_(line, len);
        // Same capture point feeds the Syslog forwarder (syslog.cpp), so every line
        // that reaches the serial console / /diag also reaches the configured
        // collector — no separate call site to keep in sync. Non-blocking and a
        // no-op before syslog_start() has run; syslog_send() itself filters out
        // this module's own "syslog:"-tagged lines to avoid a feedback loop.
        syslog_send(line, len);
    }
    return s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
}

void diag_log_init() {
    if (s_mtx) return;
    s_mtx  = xSemaphoreCreateMutex();
    s_prev = esp_log_set_vprintf(diag_vprintf_);
}

void diag_log_dump_chunks(const std::function<bool(const char*, size_t)>& sink) {
    if (!s_mtx) return;
    // Hold the mutex for the whole send so a concurrent writer can't shift s_head
    // mid-dump. The spans point straight into the static buffer — no large heap
    // allocation, which is the whole point (a 48 KB std::string can throw bad_alloc
    // on a fragmented heap, whose largest free block can fall to ~31 KB → crash).
    // RAII: `sink` writes to httpd and CAN throw (bad_alloc under heap pressure); the
    // guard releases s_mtx during unwinding so a throwing sink can't wedge the ring.
    tk::SemGuard g(s_mtx);
    if (s_wrapped) {
        if (sink(s_buf + s_head, DIAG_CAP - s_head)) sink(s_buf, s_head);
    } else {
        sink(s_buf, s_head);
    }
}

void diag_log_clear() {
    if (!s_mtx) return;
    tk::SemGuard g(s_mtx);
    s_head    = 0;
    s_wrapped = false;
}

void diag_set_verbose(bool on) { s_verbose.store(on); }
bool diag_verbose() { return s_verbose.load(); }
