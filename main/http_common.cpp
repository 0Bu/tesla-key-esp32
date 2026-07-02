// Shared helpers for the HTTP handler files (see http_handlers.hpp). JSON response
// plumbing, body/query parsing, and the browser-clock plausibility window.

#include "http_handlers.hpp"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static const char* TAG = "http_server";

// Largest POST body we accept, to bound the malloc in read_body() against a
// hostile/oversized Content-Length. All real requests here are tiny JSON objects.
static constexpr size_t MAX_BODY_LEN = 2048;

esp_err_t send_json(httpd_req_t* req, int status, cJSON* root) {
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    // On a fragmented heap cJSON_PrintUnformatted returns NULL rather than throwing, so this
    // path bypasses the handle_all try/catch. Guard it: httpd_resp_sendstr(NULL) would
    // strlen(NULL) and crash the C httpd task → reboot (which re-opens the poll window and
    // defeats car sleep). Degrade to a 503 like handle_all's own OOM fallback.
    if (!body) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"result\":false,\"reason\":\"out of memory\"}");
    }
    if (status != 200) {
        httpd_resp_set_status(req, status == 400 ? "400 Bad Request"
                                : status == 403 ? "403 Forbidden"
                                : status == 404 ? "404 Not Found"
                                : status == 409 ? "409 Conflict"
                                : "500 Internal Server Error");
    }
    esp_err_t ret = httpd_resp_sendstr(req, body);
    free(body);
    return ret;
}

cJSON* make_response(bool result, const char* command,
                     const char* vin, const char* reason) {
    cJSON* root     = cJSON_CreateObject();
    cJSON* response = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "response", response);
    cJSON_AddBoolToObject(response, "result", result);
    cJSON_AddStringToObject(response, "command", command);
    cJSON_AddStringToObject(response, "vin",     vin);
    cJSON_AddStringToObject(response, "reason",  reason);
    return root;
}

char* read_body(httpd_req_t* req) {
    if (req->content_len == 0) return nullptr;
    if (req->content_len > MAX_BODY_LEN) {
        ESP_LOGW(TAG, "rejecting oversized body: %u bytes", (unsigned)req->content_len);
        return nullptr;
    }
    size_t len = req->content_len;
    char* buf  = (char*)malloc(len + 1);
    if (!buf) return nullptr;
    int received = httpd_req_recv(req, buf, len);
    if (received <= 0) { free(buf); return nullptr; }
    buf[received] = '\0';
    return buf;
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
