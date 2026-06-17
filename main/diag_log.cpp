#include "diag_log.hpp"

#include <esp_log.h>
#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Linear byte buffer of console output captured since boot. It does NOT wrap:
// once full it stops capturing, so the log always begins at boot rather than
// silently dropping the oldest lines. Sized to hold a full boot + pairing
// session; reboot or /diag?clear=1 to reset. (The device has ~250 KB free heap.)
static constexpr size_t DIAG_CAP = 48 * 1024;
static char              s_buf[DIAG_CAP];
static size_t            s_head    = 0;      // next write position / used length
static bool              s_full    = false;  // capacity reached, further output dropped
static SemaphoreHandle_t s_mtx     = nullptr;
static vprintf_like_t    s_prev    = nullptr;
static volatile bool     s_verbose = false;

// Note stamped at the end when the buffer fills, so a truncated log is obvious.
static const char DIAG_TRUNC[] =
    "\n[diag buffer full \xE2\x80\x94 output since boot retained, newer output dropped; "
    "reboot or GET /diag?clear=1 to reset]\n";

static void diag_append_(const char* data, size_t len) {
    if (!s_mtx || len == 0) return;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;
    if (!s_full) {
        for (size_t i = 0; i < len; i++) {
            // Keep room for the truncation note so it always fits.
            if (s_head >= DIAG_CAP - sizeof(DIAG_TRUNC)) {
                memcpy(s_buf + s_head, DIAG_TRUNC, sizeof(DIAG_TRUNC) - 1);
                s_head += sizeof(DIAG_TRUNC) - 1;
                s_full = true;
                break;
            }
            s_buf[s_head++] = data[i];
        }
    }
    xSemaphoreGive(s_mtx);
}

// esp_log hook: capture the formatted line, then forward to the original sink so
// the USB serial console keeps working unchanged. Must not call ESP_LOG itself.
static int diag_vprintf_(const char* fmt, va_list ap) {
    char line[256];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof(line), fmt, ap2);
    va_end(ap2);
    if (n > 0) diag_append_(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    return s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
}

void diag_log_init() {
    if (s_mtx) return;
    s_mtx  = xSemaphoreCreateMutex();
    s_prev = esp_log_set_vprintf(diag_vprintf_);
}

std::string diag_log_dump() {
    std::string out;
    if (!s_mtx) return out;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    out.append(s_buf, s_head);
    xSemaphoreGive(s_mtx);
    return out;
}

void diag_log_clear() {
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_head = 0;
    s_full = false;
    xSemaphoreGive(s_mtx);
}

void diag_set_verbose(bool on) { s_verbose = on; }
bool diag_verbose() { return s_verbose; }
