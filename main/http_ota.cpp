// OTA self-update endpoints:
//   GET  /ota/check[?ms=<epoch>]  (start a background manifest check)
//   POST /ota/update              (start the background download+install)
//   GET  /ota/status              (poll progress)
// Dispatched from handle_all in http_server.cpp (inside its try/catch OOM guard).

#include "http_handlers.hpp"
#include "ota_update.hpp"
#include "logic/ota_contract.hpp"

static const char* ota_state_str(OtaState s) {
    switch (s) {
        case OtaState::Checking:    return "checking";
        case OtaState::Downloading: return "downloading";
        case OtaState::Done:        return "done";
        case OtaState::Error:       return "error";
        default:                    return "idle";
    }
}

// No query timestamp or an already committed NTP clock needs no fallback write.
// Required fallback values must be canonical bounded integer milliseconds.
static BrowserClockStatus apply_browser_time_query_(GuardedReq rq) {
    char q[kQueryBufBytes];
    const esp_err_t query = httpd_req_get_url_query_str(rq.req(), q, sizeof(q));
    if (query == ESP_ERR_NOT_FOUND) return BrowserClockStatus::AlreadyNtpSynced;
    if (query != ESP_OK) return BrowserClockStatus::Invalid;
    const auto ms = tk::ota_query_parameter(q, "ms");
    if (!ms.present) return BrowserClockStatus::AlreadyNtpSynced;
    tk::ClockSample sample;
    if (!ms.valid || !tk::clock_parse_query_ms(ms.value, clock_plausibility_ceiling(), sample))
        return BrowserClockStatus::Invalid;
    if (clock_synced_via_ntp()) return BrowserClockStatus::AlreadyNtpSynced;
    rq.reply()->state.attempting(tk::RequestEffect::Clock);
    const BrowserClockResult result = apply_browser_clock_sample(sample);
    if (result.status != BrowserClockStatus::Applied &&
        result.status != BrowserClockStatus::SetFailed)
        rq.reply()->state.refused(tk::RequestEffect::Clock);
    return result.status;
}

static bool parse_pr_param_(httpd_req_t* req, unsigned& pr) {
    pr = 0;
    char q[kQueryBufBytes];
    const auto read = httpd_req_get_url_query_str(req, q, sizeof(q));
    if (read == ESP_ERR_NOT_FOUND) return true;
    if (read != ESP_OK) return false;
    const auto value = tk::ota_query_parameter(q, "pr");
    if (!value.present) return true;
    if (!value.valid) return false;
    pr = tk::parse_pr_query(value.value);
    return pr != 0;
}

// GET /ota/check[?ms=<epoch>][&pr=<N>] — start a background version check, return at once.
// Accept one reserved check; memory admission runs after this request's SDK scratch is freed.
// The slow HTTPS fetch runs in its own task; the UI polls /ota/status for admission/result.
esp_err_t handle_ota_check(GuardedReq rq) {
    httpd_req_t* req = rq.req();
    if (validate_query_string(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid query string");
    }
    rq.reply()->state.shape = tk::ReplyShape::OtaCheck;
    unsigned pr = 0;
    if (!parse_pr_param_(req, pr)) {
        tk::JsonBuilder json;
        json.boolean(json.root(), "started", false);
        json.string(json.root(), "reason", "invalid PR parameter");
        return send_json(req, 400, json.release());
    }
    // Entry snapshot: an update in progress forbids both fallback clock mutation and a new
    // check. This is not a global exclusion against an update starting after this snapshot.
    if (ota_is_updating()) {
        tk::JsonBuilder json;
        json.boolean(json.root(), "started", false);
        json.string(json.root(), "reason", "an update is already in progress");
        return send_json(req, 409, json.release());
    }
    const BrowserClockStatus clock = apply_browser_time_query_(rq);
    if (clock == BrowserClockStatus::Invalid || clock == BrowserClockStatus::Busy ||
        clock == BrowserClockStatus::SetFailed) {
        tk::JsonBuilder json;
        json.boolean(json.root(), "started", false);
        json.string(json.root(), "reason", clock == BrowserClockStatus::Invalid
                    ? "implausible timestamp" : clock == BrowserClockStatus::Busy
                    ? "clock is busy" : "clock set failed");
        return send_effect_json(rq, clock == BrowserClockStatus::Invalid ? 400
                                    : clock == BrowserClockStatus::Busy ? 503 : 500, json.release());
    }
    rq.reply()->state.attempting(tk::RequestEffect::OtaStart);
    const auto result = ota_check_start_after_http(pr, req->handle);
    const bool started = result == tk::OtaStartResult::Started;
    if (started) rq.reply()->state.succeeded();
    else rq.reply()->state.refused(tk::RequestEffect::OtaStart);
    tk::JsonBuilder json;
    json.boolean(json.root(), "started", started);
    if (!started)
        json.string(json.root(), "reason", tk::ota_start_reason(result, true));
    return send_effect_json(rq, tk::ota_start_http_status(result), json.release());
}

// POST /ota/update[?pr=<N>] — start the background download+install, return immediately.
esp_err_t handle_ota_update(GuardedReq rq) {
    httpd_req_t* req = rq.req();
    if (validate_query_string(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid query string");
    }
    rq.reply()->state.shape = tk::ReplyShape::FlatResult;
    unsigned pr = 0;
    if (!parse_pr_param_(req, pr)) {
        tk::JsonBuilder json;
        json.boolean(json.root(), "result", false);
        json.string(json.root(), "reason", "invalid PR parameter");
        return send_json(req, 400, json.release());
    }
    rq.reply()->state.attempting(tk::RequestEffect::OtaStart);
    const auto result = ota_start(pr);
    const bool started = result == tk::OtaStartResult::Started;
    if (started) rq.reply()->state.succeeded();
    else rq.reply()->state.refused(tk::RequestEffect::OtaStart);
    tk::JsonBuilder json;
    json.boolean(json.root(), "result", started);
    json.string(json.root(), "reason", tk::ota_start_reason(result, false));
    return send_effect_json(rq, tk::ota_start_http_status(result), json.release());
}

// GET /ota/status — poll download progress.
esp_err_t handle_ota_status(GuardedReq rq) {
    httpd_req_t* req = rq.req();
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
    httpd_req_t* req = rq.req();
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
