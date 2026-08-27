// Shared helpers for the HTTP handler files (see http_handlers.hpp). JSON response
// plumbing, body/query parsing, and the browser-clock plausibility window.

#include "http_handlers.hpp"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <cstring>
#include <cstdlib>
#include <sys/time.h>

static const char* TAG = "http_server";

// Largest POST body we accept, to bound the malloc in read_body_result() against a
// hostile/oversized Content-Length. All real requests here are tiny JSON objects.
static constexpr size_t MAX_BODY_LEN = 2048;

static const char* http_status_text_(int status) noexcept {
    return status == 400 ? "400 Bad Request"
         : status == 403 ? "403 Forbidden"
         : status == 404 ? "404 Not Found"
         : status == 409 ? "409 Conflict"
         : status == 413 ? "413 Payload Too Large"
         : status == 502 ? "502 Bad Gateway"
         : status == 503 ? "503 Service Unavailable"
                         : "500 Internal Server Error";
}

struct RestJsonTransport {
    httpd_req_t* req;

    void set_json_type() const noexcept {
        httpd_resp_set_type(req, "application/json");
    }
    void set_status(int status) const noexcept {
        httpd_resp_set_status(req, http_status_text_(status));
    }
    esp_err_t send(const char* body) const noexcept {
        return httpd_resp_sendstr(req, body);
    }
};

esp_err_t send_json(httpd_req_t* req, int status, cJSON* root) {
    RestJsonTransport transport{req};
    return tk::json_http_reply(
        transport,
        tk::json_owner(root),
        status,
        "{\"result\":false,\"reason\":\"out of memory\"}");
}

cJSON* make_response(bool result, const char* command,
                     const char* vin, const char* reason) {
    tk::JsonBuilder json;
    cJSON* response = json.object(json.root(), "response");
    json.boolean(response, "result", result);
    json.string(response, "command", command);
    json.string(response, "vin", vin);
    json.string(response, "reason", reason);
    return json.release();
}

tk::BodyReadResult read_body_result(httpd_req_t* req) {
    const tk::BodyReadResult result = tk::http_body_receive(
        req->content_len, MAX_BODY_LEN,
        [](size_t n) -> void* { return malloc(n); },
        [](void* p) { free(p); },
        [req](char* dst, size_t want) -> tk::BodyChunk {
            int r = httpd_req_recv(req, dst, want);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) return { tk::BodyRecv::Timeout, 0 };
            if (r <= 0)                      return { tk::BodyRecv::Error,   0 };
            return { tk::BodyRecv::Data, static_cast<size_t>(r) };
        });
    if (result.status == tk::BodyReadStatus::TooLarge) {
        ESP_LOGW(TAG, "rejecting oversized body: %u bytes", (unsigned)req->content_len);
    } else if (result.status == tk::BodyReadStatus::NoMemory) {
        ESP_LOGW(TAG, "could not allocate %u-byte request body", (unsigned)req->content_len);
    } else if (result.status == tk::BodyReadStatus::ReceiveFailed) {
        ESP_LOGW(TAG, "request body receive failed after bounded retries");
    }
    return result;
}

bool query_param_is(httpd_req_t* req, const char* key, const char* want) {
    char q[96];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    char val[24];
    if (httpd_query_key_value(q, key, val, sizeof(val)) != ESP_OK) return false;
    return strcmp(val, want) == 0;
}

// ─── Browser-clock plausibility window ────────────────────────────────────────
// For a browser-supplied wall clock (the NTP fallback: POST /set_time and /ota/check?ms=).
// The clock gates OTA TLS certificate validation, so an unauthenticated LAN client must not
// be able to push it arbitrarily far in EITHER direction: too far back makes valid certs
// look not-yet-valid; too far forward makes them look expired (and not-yet-valid certs look
// valid). The lower floor is fixed at ~2023-11; the upper bound is BUILD-RELATIVE (build
// year + 10) so it never goes stale as real time advances — a fixed ceiling would eventually
// reject a correct clock on a long-lived device.
static constexpr double kClockFloorMs = 1700000000000.0;   // ~2023-11-14

// Epoch ms at 00:00 UTC on Jan 1 of `year` (proleptic Gregorian, year ≥ 1970). Pure integer
// arithmetic — the leap-day terms count Feb 29s strictly before Jan 1 of `year`.
static double jan1_epoch_ms(int year) {
    long long days = 365LL * (year - 1970)
                   + (year - 1969) / 4
                   - (year - 1901) / 100
                   + (year - 1601) / 400;
    return (double)days * 86400.0 * 1000.0;
}

// Upper plausibility bound = (build year + 10) as epoch ms. esp_app_get_description()->date is
// the compiler __DATE__ string "Mmm dd yyyy", so the 4-digit year is its last token.
static double clock_ceiling_ms() {
    const char* d = esp_app_get_description()->date;
    size_t n = d ? strlen(d) : 0;
    int year = (n >= 4) ? atoi(d + n - 4) : 0;
    if (year < 2023) year = 2023;   // unparseable → conservative base
    return jan1_epoch_ms(year + 10);
}

bool browser_time_plausible(double epoch_ms) {
    return epoch_ms >= kClockFloorMs && epoch_ms < clock_ceiling_ms();
}

long long apply_browser_clock(double epoch_ms) {
    long long sec = (long long)(epoch_ms / 1000.0);
    struct timeval tv = {};
    tv.tv_sec  = (time_t)sec;
    tv.tv_usec = (suseconds_t)((long long)epoch_ms % 1000) * 1000;
    settimeofday(&tv, nullptr);
    // Persist the applied wall clock ("last_time", tesla_cfg): the device has no battery-backed
    // RTC, so main.cpp restores this on boot — a headless reboot (evcc only, NTP blocked, no
    // browser visit) still comes up with a plausible clock for OTA TLS cert validation and the
    // key_created/paired_at timestamps. NOT needed for tesla-ble signed-command freshness
    // (expires_at derives from the vehicle's SessionInfo.ClockTime + a monotonic delta).
    if (!g_config->save_str(tk::nvs_contract::kLastTime, std::to_string(sec))) {
        // The clock is already applied in RAM, so this request still succeeded; what is lost is
        // only the restore across the NEXT reboot. Naming it here is what separates "NVS is
        // failing" from "the browser never set the time" when a headless boot comes up at 1970.
        ESP_LOGW(TAG, "wall clock applied but not persisted — a reboot will come up unset");
    }
    return sec;
}
