#include "stack_watch.hpp"
// Shared helpers for the HTTP handler files (see http_handlers.hpp). JSON response
// plumbing, body/query parsing, and the browser-clock plausibility window.

#include "http_handlers.hpp"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_timer.h>
#include <cstring>
#include <cstdlib>
#include <sys/time.h>
#include <lwip/sockets.h>
#include "ota_update.hpp"
#include <esp_system.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "http_server";

// Largest POST body we accept, to bound the malloc in read_body_result() against a
// hostile/oversized Content-Length. All real requests here are tiny JSON objects.
static constexpr size_t MAX_BODY_LEN = 2048;

static const char* http_status_text_(int status) noexcept {
    return status == 200 ? "200 OK"
         : status == 400 ? "400 Bad Request"
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

// Checked adapter used only for effect-aware responses. Ordinary pre-effect paths retain
// their existing envelopes. Every header/chunk error is terminal for this logical reply.
struct EffectJsonTransport {
    httpd_req_t* req;
    esp_err_t set_json_type() const noexcept {
        return httpd_resp_set_type(req, "application/json");
    }
    esp_err_t set_status(int status) const noexcept {
        return httpd_resp_set_status(req, http_status_text_(status));
    }
    esp_err_t send(const char* body) const noexcept { return httpd_resp_sendstr(req, body); }
    esp_err_t chunk(const char* body, size_t size) const noexcept {
        return httpd_resp_send_chunk(req, body, size);
    }
};

esp_err_t send_effect_json(GuardedReq rq, int status, cJSON* root) {
    if (!rq.reply()->state.needs_effect_reply()) return send_json(rq.req(), status, root);
    EffectJsonTransport transport{rq.req()};
    return tk::effect_json_reply(transport, *rq.reply(), status, tk::json_owner(root));
}

esp_err_t send_effect_exception(GuardedReq rq) noexcept {
    if (rq.reply()->state.terminal) return ESP_FAIL;
    try {
        EffectJsonTransport transport{rq.req()};
        if (rq.reply()->state.needs_effect_reply())
            return tk::degraded_effect_reply(transport, *rq.reply());
        rq.reply()->state.terminal = true;
        esp_err_t result = transport.set_status(503);
        if (result != ESP_OK) return result;
        result = httpd_resp_set_type(rq.req(), "text/plain");
        if (result != ESP_OK) return result;
        return transport.send("out of memory");
    } catch (...) { return ESP_FAIL; } // never retry a partial response or unwind into httpd C
}

void run_effect_continuation(GuardedReq rq) noexcept {
    const auto continuation = rq.reply()->state.take_continuation();
    if (continuation == tk::ReplyContinuation::None) return;
    try {
        if (continuation == tk::ReplyContinuation::SuccessfulConfigRestart) {
            if (!rq.context->config_restart.owns_gate()) return;
        }
        // Identity recovery/failure reboots never confirm a pending OTA image.
        vTaskDelay(pdMS_TO_TICKS(800));
        tk::stack_watch_sample(tk::StackWatch::Httpd); // before terminal effect
        esp_restart();
    } catch (...) {
        // Never retry a consumed continuation or let diagnostics unwind into httpd.
        try { ESP_LOGE(TAG, "saved configuration restart interrupted; no continuation retry"); }
        catch (...) {}
    }
}

void finish_config_save(GuardedReq rq) noexcept {
    rq.reply()->state.succeeded();
    rq.reply()->state.continuation = tk::ReplyContinuation::SuccessfulConfigRestart;
    rq.reply()->state.image = ota_confirm_pending_image(
        rq.context->config_restart, tk::OtaRebootClass::SuccessfulUserConfigCommit);
}

esp_err_t send_config_reply(GuardedReq rq, int status, bool ok,
                            const char* command, const char* reason) {
    tk::JsonBuilder json;
    cJSON* response = json.object(json.root(), "response");
    json.boolean(response, "result", ok);
    json.string(response, "command", command);
    json.string(response, "vin", "");
    const auto image = rq.reply()->state.image;
    json.string(response, "reason", ok && tk::config_image_warning(image)
        ? tk::config_commit_reason(image) : reason);
    if (image != tk::ConfigImageResult::None)
        json.string(response, "firmware_verification", tk::config_image_code(image));
    return send_effect_json(rq, status, json.release());
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
        }, []() -> uint64_t { return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL; });
    if (result.status == tk::BodyReadStatus::TooLarge ||
        result.status == tk::BodyReadStatus::NoMemory ||
        result.status == tk::BodyReadStatus::ReceiveFailed) {
        // Also safe outside handle_all: unread-body cleanup must not restart RX.
        (void)shutdown(httpd_req_to_sockfd(req), SHUT_RD);
    }
    if (result.status == tk::BodyReadStatus::TooLarge) {
        ESP_LOGW(TAG, "rejecting oversized body: %u bytes", (unsigned)req->content_len);
    } else if (result.status == tk::BodyReadStatus::NoMemory) {
        ESP_LOGW(TAG, "could not allocate %u-byte request body", (unsigned)req->content_len);
    } else if (result.status == tk::BodyReadStatus::ReceiveFailed) {
        ESP_LOGW(TAG, "request body receive failed after bounded retries");
    }
    return result;
}

esp_err_t validate_query_string(httpd_req_t* req) {
    if (!req) return ESP_ERR_INVALID_ARG;
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0) return ESP_OK;
    if (qlen >= kQueryBufBytes) {
        ESP_LOGW(TAG, "query string length %u exceeds supported buffer (%u)",
                 (unsigned)qlen, (unsigned)kQueryBufBytes);
        return ESP_ERR_HTTPD_RESULT_TRUNC;
    }
    return ESP_OK;
}

bool query_param_is(httpd_req_t* req, const char* key, const char* want) {
    if (!req || !key || !want) return false;
    char q[kQueryBufBytes];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    char val[32];
    if (httpd_query_key_value(q, key, val, sizeof(val)) != ESP_OK) return false;
    return strcmp(val, want) == 0;
}
