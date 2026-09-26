// OTA self-update endpoints:
//   GET  /ota/check[?ms=<epoch>]  (start a background manifest check)
//   POST /ota/update              (start the background download+install)
//   GET  /ota/status              (poll progress)
// Dispatched from handle_all in http_server.cpp (inside its try/catch OOM guard).

#include "http_handlers.hpp"
#include "ota_update.hpp"
#include "logic/ota_contract.hpp"
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
    char q[kQueryBufBytes];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return;
    char ms[24];
    if (httpd_query_key_value(q, "ms", ms, sizeof(ms)) != ESP_OK) return;
    double epoch_ms = atof(ms);
    if (!browser_time_plausible(epoch_ms)) return;
    long long sec = apply_browser_clock(epoch_ms);
    ESP_LOGI(TAG, "clock set from browser (ota query): %lld", sec);
}

// GET /ota/check[?ms=<epoch>][&pr=<N>] — start a background version check, return at once.
// The slow HTTPS manifest fetch runs in its own task (see ota_check_start) so it
// never ties up the HTTP server; the UI polls /ota/status for the result.
esp_err_t handle_ota_check(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    if (validate_query_string(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid query string");
    }
    apply_browser_time_query_(req);
    unsigned pr = 0;
    char q[kQueryBufBytes];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char pr_str[16];
        if (httpd_query_key_value(q, "pr", pr_str, sizeof(pr_str)) == ESP_OK) {
            pr = tk::parse_pr_query(pr_str);
        }
    }
    bool started = ota_check_start(pr);
    tk::JsonBuilder json;
    json.boolean(json.root(), "started", started);
    if (!started)
        json.string(json.root(), "reason", "a check or update is already in progress");
    return send_json(req, started ? 200 : 409, json.release());
}

// POST /ota/update[?pr=<N>] — start the background download+install, return immediately.
esp_err_t handle_ota_update(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    if (validate_query_string(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid query string");
    }
    unsigned pr = 0;
    char q[kQueryBufBytes];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char pr_str[16];
        if (httpd_query_key_value(q, "pr", pr_str, sizeof(pr_str)) == ESP_OK) {
            pr = tk::parse_pr_query(pr_str);
        }
    }
    bool started = ota_start(pr);
    tk::JsonBuilder json;
    json.boolean(json.root(), "result", started);
    json.string(json.root(), "reason",
        started ? "update started — the device will reboot when done"
                : "an update is already in progress");
    return send_json(req, started ? 200 : 409, json.release());
}

// GET /ota/status — poll download progress.
esp_err_t handle_ota_status(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    OtaStatus s = ota_get_status();
    tk::JsonBuilder json;
    json.string(json.root(), "state", ota_state_str(s.state));
    json.number(json.root(), "progress", s.progress);
    json.string(json.root(), "message", s.message.c_str());
    json.string(json.root(), "available", s.available.c_str());
    json.boolean(json.root(), "update_available", s.update_available);
    json.string(json.root(), "current", s.current.c_str());
    json.string(json.root(), "channel", s.channel.c_str());
    if (s.target_pr > 0) {
        json.number(json.root(), "pr", s.target_pr);
    }
    return send_json(req, 200, json.release());
}

// GET /ota/changelog — fetch notes for the currently offered update.
// Returns text/plain with line-by-line changelog points (or 204 No Content if none).
esp_err_t handle_ota_changelog(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    char notes[1025];
    size_t len = 0;
    if (!ota_get_changelog(notes, sizeof(notes), len) || len == 0) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, nullptr, 0);
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, notes, len);
}
