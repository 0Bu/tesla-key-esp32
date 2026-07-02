// OTA self-update endpoints:
//   GET  /ota/check[?ms=<epoch>]  (start a background manifest check)
//   POST /ota/update              (start the background download+install)
//   GET  /ota/status              (poll progress)
// Dispatched from handle_all in http_server.cpp (inside its try/catch OOM guard).

#include "http_handlers.hpp"
#include "ota_update.hpp"
#include <esp_log.h>
#include <cstdlib>

static const char* TAG = "http_server";

static const char* ota_state_str(OtaState s) {
    switch (s) {
        case OtaState::Checking:    return "checking";
        case OtaState::Downloading: return "downloading";
        case OtaState::Done:        return "done";
        case OtaState::Error:       return "error";
        default:                    return "idle";
    }
}

// Apply ?ms=<epoch> as the wall clock (the NTP fallback, see /set_time) when NTP
// hasn't synced. Lets a single /ota/check request both set the browser time and
// start the check — no extra blocking round-trip on the (serialized) HTTP server.
static void apply_browser_time_query_(httpd_req_t* req) {
    if (clock_synced_via_ntp()) return;
    char q[48];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return;
    char ms[24];
    if (httpd_query_key_value(q, "ms", ms, sizeof(ms)) != ESP_OK) return;
    double epoch_ms = atof(ms);
    if (!browser_time_plausible(epoch_ms)) return;
    long long sec = apply_browser_clock(epoch_ms);
    ESP_LOGI(TAG, "clock set from browser (ota query): %lld", sec);
}

// GET /ota/check[?ms=<epoch>] — start a background version check, return at once.
// The slow HTTPS manifest fetch runs in its own task (see ota_check_start) so it
// never ties up the HTTP server; the UI polls /ota/status for the result.
esp_err_t handle_ota_check(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    apply_browser_time_query_(req);
    bool started = ota_check_start();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "started", started);
    if (!started)
        cJSON_AddStringToObject(root, "reason", "a check or update is already in progress");
    return send_json(req, started ? 200 : 409, root);
}

// POST /ota/update — start the background download+install, return immediately.
esp_err_t handle_ota_update(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    bool started = ota_start();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root,   "result", started);
    cJSON_AddStringToObject(root, "reason",
        started ? "update started — the device will reboot when done"
                : "an update is already in progress");
    return send_json(req, started ? 200 : 409, root);
}

// GET /ota/status — poll download progress.
esp_err_t handle_ota_status(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    OtaStatus s = ota_get_status();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state",            ota_state_str(s.state));
    cJSON_AddNumberToObject(root, "progress",         s.progress);
    cJSON_AddStringToObject(root, "message",          s.message.c_str());
    cJSON_AddStringToObject(root, "available",        s.available.c_str());
    cJSON_AddBoolToObject(root,   "update_available", s.update_available);
    cJSON_AddStringToObject(root, "current",          s.current.c_str());
    return send_json(req, 200, root);
}
