#include "diag_log.hpp"
#include "syslog.hpp"
#include "logic/redact.hpp"

#ifdef ESP_PLATFORM
#include <sdkconfig.h>
static_assert(CONFIG_LOG_VERSION == 1, "diagnostic capture requires combined V1 log callbacks");
static_assert(CONFIG_LOG_TIMESTAMP_SOURCE_RTOS, "diagnostic credential guard requires RTOS log timestamps");
#if defined(CONFIG_LOG_COLORS) && CONFIG_LOG_COLORS
#error "diagnostic credential guard requires uncolored log records"
#endif
#endif

#include <esp_log.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <algorithm>
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
static uint64_t          s_written = 0;      // monotonic byte sequence within one clear epoch
static uint64_t          s_epoch   = 0;      // detects clear while a dump is in flight
static std::atomic<SemaphoreHandle_t> s_mtx{nullptr};
static std::atomic<vprintf_like_t> s_prev{nullptr};
static bool             s_hook_installed = false;  // single boot-owner initialization; sequential retries allowed
static const char*       TAG       = "diag_log";
// atomic (not volatile): written by the /diag HTTP handler, read by the capture hook on
// whichever task logged the line. A cross-task scalar — atomic gives it a defined value.
static std::atomic<bool> s_verbose{false};
static std::atomic<uint32_t> s_dropped_lines{0};  // boot counter, wraps modulo 2^32

static void diag_append_(const char* data, size_t len) {
    const auto mtx = s_mtx.load(std::memory_order_acquire);
    if (len == 0) return;
    if (!mtx) { s_dropped_lines.fetch_add(1, std::memory_order_relaxed); return; }
    const size_t library_end = tk::diag_library_capture_content_end({data, len});
    tk::SemGuard g(mtx, 0);  // capture is best-effort; never wait in a producer's log callback
    if (!g) { s_dropped_lines.fetch_add(1, std::memory_order_relaxed); return; }
    for (size_t i = 0; i < len; i++) {
        s_buf[s_head++] = tk::diag_capture_byte(data[i], i, library_end);
        ++s_written;
        if (s_head >= DIAG_CAP) { s_head = 0; s_wrapped = true; }
    }
}

// Supply a fresh va_list to the existing Serial sink for a protected fixed replacement.
static int diag_forward_fixed_(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const auto previous = s_prev.load(std::memory_order_acquire);
    const int result = previous ? previous(fmt, ap) : vprintf(fmt, ap);
    va_end(ap);
    return result;
}

// Keep classifier scratch outside the callback's existing fixed-buffer stack frame.
__attribute__((noinline))
static bool diag_mqtt_uri_error_(const char* line, size_t len) {
    return tk::diag_mqtt_uri_error({line, len});
}

// esp_log hook: capture the formatted line, then forward ordinary records to the original
// Serial sink. Known credential-bearing MQTT parser errors are replaced before every sink.
// Must not call ESP_LOG itself.
static int diag_vprintf_(const char* fmt, va_list ap) {
    char line[256];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof(line), fmt, ap2);
    va_end(ap2);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        if (diag_mqtt_uri_error_(line, len)) {
            diag_append_(tk::kDiagMqttUriErrorLine, sizeof(tk::kDiagMqttUriErrorLine) - 1);
            syslog_send(tk::kDiagMqttUriErrorLine, sizeof(tk::kDiagMqttUriErrorLine) - 1);
            return diag_forward_fixed_("%s", tk::kDiagMqttUriErrorLine);
        }
        diag_append_(line, len);
        // Same capture point feeds the Syslog forwarder (syslog.cpp), so every line
        // that reaches the serial console / /diag also reaches the configured
        // collector — no separate call site to keep in sync. Non-blocking and a
        // no-op before syslog_start() has run; syslog_send() itself filters out
        // this module's own "syslog:"-tagged lines to avoid a feedback loop.
        syslog_send(line, len);
    }
    const auto previous = s_prev.load(std::memory_order_acquire);
    return previous ? previous(fmt, ap) : vprintf(fmt, ap);
}

uint32_t diag_log_dropped_lines() noexcept {
    return s_dropped_lines.load(std::memory_order_relaxed);
}

void diag_log_init() {
    // The application calls this from its boot owner before MQTT/HTTP start. The credential
    // guard must remain installed even if optional ring allocation fails; never chain to self
    // when a sequential initialization retry attempts that allocation again.
    if (!s_hook_installed) {
        const auto previous = esp_log_set_vprintf(diag_vprintf_);
        s_prev.store(previous, std::memory_order_release);
        s_hook_installed = true;
    }
    if (s_mtx.load(std::memory_order_acquire)) return;
    const auto mtx = xSemaphoreCreateMutex();
    if (!mtx) {
        ESP_LOGE(TAG, "failed to allocate diagnostic-log mutex; capture disabled");
        return;
    }
    s_mtx.store(mtx, std::memory_order_release);
}

DiagDumpResult diag_log_dump_chunks(const std::function<bool(const char*, size_t)>& sink,
                                    DiagDumpStart start_mode) {
    const auto mtx = s_mtx.load(std::memory_order_acquire);
    if (!mtx) return DiagDumpResult::SnapshotInvalidated;
    // Snapshot the logical spans, then copy each bounded piece under the mutex and invoke the sink
    // only AFTER releasing it.  The sink is HTTP/network I/O and may block or throw; doing either
    // while holding the shared log mutex stalls every producer and violates the firmware lock
    // contract. A whole-ring snapshot would need another 16 KB allocation/static buffer. A writer
    // may replace bytes already delivered, but if it reaches any unread snapshot byte the
    // dump aborts before copying it. Mixing generations would be worse than a truncated response:
    // in redact mode a new prefix plus an old sensitive tail could evade a line-based marker.
    size_t head = 0;
    bool wrapped = false;
    uint64_t snapshot_written = 0;
    uint64_t snapshot_epoch = 0;
    {
        tk::SemGuard g(mtx);
        if (!g) return DiagDumpResult::SnapshotInvalidated;
        head = s_head;
        wrapped = s_wrapped;
        snapshot_written = s_written;
        snapshot_epoch = s_epoch;
    }

    static constexpr size_t kDumpChunk = 256;
    char chunk[kDumpChunk];
    const size_t snapshot_len = wrapped ? DIAG_CAP : head;
    const uint64_t snapshot_start = snapshot_written - snapshot_len;
    bool discard_partial_line =
        wrapped && start_mode == DiagDumpStart::AfterWrappedLineBoundary;
    auto dump_span = [&](size_t start, size_t len,
                         uint64_t absolute_start) -> DiagDumpResult {
        for (size_t offset = 0; offset < len;) {
            const size_t n = (len - offset < sizeof(chunk)) ? len - offset : sizeof(chunk);
            {
                tk::SemGuard g(mtx);
                if (!g) return DiagDumpResult::SnapshotInvalidated;
                const uint64_t unread = absolute_start + offset;
                if (s_epoch != snapshot_epoch || s_written < unread ||
                    s_written - unread > DIAG_CAP) {
                    return DiagDumpResult::SnapshotInvalidated;
                }
                memcpy(chunk, s_buf + start + offset, n);
            }
            const char* delivered = chunk;
            size_t delivered_size = n;
            if (discard_partial_line) {
                const void* boundary = std::memchr(chunk, '\n', n);
                if (!boundary) {
                    offset += n;
                    continue;
                }
                const size_t skipped =
                    static_cast<const char*>(boundary) - chunk + 1;
                discard_partial_line = false;
                delivered += skipped;
                delivered_size -= skipped;
            }
            if (delivered_size != 0 && !sink(delivered, delivered_size)) {
                return DiagDumpResult::SinkFailed;
            }
            offset += n;
        }
        return DiagDumpResult::Complete;
    };

    if (wrapped) {
        const size_t first_len = DIAG_CAP - head;
        const DiagDumpResult first = dump_span(head, first_len, snapshot_start);
        if (first != DiagDumpResult::Complete) return first;
        return dump_span(0, head, snapshot_start + first_len);
    }
    return dump_span(0, head, snapshot_start);
}

void diag_log_clear() {
    const auto mtx = s_mtx.load(std::memory_order_acquire);
    if (!mtx) return;
    tk::SemGuard g(mtx);
    s_head    = 0;
    s_wrapped = false;
    s_written = 0;
    ++s_epoch;
}

void diag_set_verbose(bool on) { s_verbose.store(on); }
bool diag_verbose() { return s_verbose.load(); }
