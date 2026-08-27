// HTTP server core: the single wildcard dispatcher, the handle_all try/catch OOM guard
// every handler runs under, and server startup. The route handlers themselves live in
// http_api.cpp (evcc API), http_status.cpp (web UI + status/diag), http_ota.cpp (OTA),
// http_config.cpp (setup/config) and mcp_server.cpp (MCP endpoint); shared helpers in
// http_common.cpp. See http_handlers.hpp for the split map.

#include "http_handlers.hpp"
#include "logic/http_route.hpp"
#include "runtime_admission.hpp"
#include "stack_watch.hpp"
#include <esp_log.h>
#include <cstring>
#include <exception>

static const char* TAG = "http_server";

// Global vehicle + config-store references (set once at start; declared in http_handlers.hpp)
VehicleController* g_vehicle = nullptr;
NvsStorageAdapter* g_config  = nullptr;

// Copy the PATH part of the request URI (everything before '?') into buf. Routing must look
// only at the path: matching against the raw req->uri (which includes the query string) lets a
// query value like "?next=/status" be mistaken for a route, and a substring like "force=1" be
// found anywhere. Truncation only changes a route into a 404, never the reverse, so it's safe.
static const char* uri_path(httpd_req_t* req, char* buf, size_t n) {
    if (!buf || n == 0) return buf;
    const std::string_view path = tk::http_path_only(req ? req->uri : "");
    const size_t copy = path.size() < n - 1 ? path.size() : n - 1;
    if (copy != 0) memcpy(buf, path.data(), copy);
    buf[copy] = '\0';
    return buf;
}

// ─── Wildcard handler dispatching ─────────────────────────────────────────────

// Single catch-all handler registered for /*
static esp_err_t handle_all_dispatch(httpd_req_t* req) {
    char path[128];
    uri_path(req, path, sizeof(path));
    const bool GET  = req->method == HTTP_GET;
    const bool POST = req->method == HTTP_POST;
    ESP_LOGI(TAG, "REQ: %s %s", GET ? "GET" : (POST ? "POST" : "OTHER"), req->uri);

    // Log all headers to see what evcc is sending
    char header_val[128];
    if (httpd_req_get_hdr_value_str(req, "User-Agent", header_val, sizeof(header_val)) == ESP_OK) {
        ESP_LOGI(TAG, "  User-Agent: %s", header_val);
    }
    if (httpd_req_get_hdr_value_str(req, "Accept", header_val, sizeof(header_val)) == ESP_OK) {
        ESP_LOGI(TAG, "  Accept: %s", header_val);
    }

    const tk::HttpVerb verb = GET ? tk::HttpVerb::Get
                                  : (POST ? tk::HttpVerb::Post : tk::HttpVerb::Other);
    const tk::HttpRoute route = tk::classify_http_route(verb, path);
    if (tk::http_route_requires_vehicle_runtime(route) &&
        !tk::runtime_admission_vehicle_ready()) {
        ESP_LOGW(TAG, "vehicle-active route refused while runtime is not ready: %s", path);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(
            req, "{\"error\":\"vehicle runtime unavailable\",\"retryable\":false}");
    }
    switch (route) {
        case tk::HttpRoute::Command:        return handle_command({req});
        case tk::HttpRoute::VehicleData:    return handle_vehicle_data({req});
        case tk::HttpRoute::BodyController: return handle_body_controller({req});
        case tk::HttpRoute::OtaCheck:       return handle_ota_check({req});
        case tk::HttpRoute::OtaUpdate:      return handle_ota_update({req});
        case tk::HttpRoute::OtaStatus:      return handle_ota_status({req});
        case tk::HttpRoute::GenKeys:        return handle_gen_keys({req});
        case tk::HttpRoute::SendKey:        return handle_send_key({req});
        case tk::HttpRoute::SetTime:        return handle_set_time({req});
        case tk::HttpRoute::SetVin:         return handle_set_vin({req});
        case tk::HttpRoute::SetMqtt:        return handle_set_mqtt({req});
        case tk::HttpRoute::SetSyslog:      return handle_set_syslog({req});
        case tk::HttpRoute::SetWifi:        return handle_set_wifi({req});
        case tk::HttpRoute::Scan:           return handle_scan({req});
        case tk::HttpRoute::Coredump:       return handle_coredump({req});
        case tk::HttpRoute::CrashDismiss:   return handle_crash_dismiss({req});
        case tk::HttpRoute::Heap:           return handle_heap({req});
        case tk::HttpRoute::McpPost:        return mcp_handle_post({req});
        case tk::HttpRoute::McpGet:         return mcp_handle_get({req});
        case tk::HttpRoute::Version:        return handle_version({req});
        case tk::HttpRoute::Status:         return handle_status({req});
        case tk::HttpRoute::Diag:           return handle_diag({req});
        case tk::HttpRoute::Index:          return handle_index({req});
        case tk::HttpRoute::NotFound:       break;
    }

    tk::JsonBuilder json;
    json.string(json.root(), "error", "not found");
    return send_json(req, 404, json.release());
}

// Catch-all wrapper: a handler that runs out of memory throws std::bad_alloc (e.g. a
// large response built on a fragmented heap). This task is invoked from the C httpd
// loop, so an escaping C++ exception unwinds into C frames → std::terminate → abort()
// → reboot. Contain it here and return 503 instead, keeping the device alive. (Root
// cause is the low largest-free-block; this is the safety net so no request can crash
// the box.)
static esp_err_t handle_all(httpd_req_t* req) {
    try {
        // Sample on every exit, including the exception fallbacks. The request that came closest to
        // the limit is exactly the one most likely to throw; a destructor also covers every early
        // return without duplicating the measurement at each route. Construct it inside the outer
        // boundary so even a future throwing constructor cannot escape into the C httpd frame.
        struct SampleHttpdStackOnExit {
            ~SampleHttpdStackOnExit() noexcept { tk::stack_watch_sample(tk::StackWatch::Httpd); }
        } sample_httpd_stack;
        return handle_all_dispatch(req);
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "handler for %s threw (%s) — likely OOM; returning 503", req->uri, e.what());
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "out of memory");
    } catch (...) {
        ESP_LOGE(TAG, "handler for %s threw (unknown) — returning 503", req->uri);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "out of memory");
    }
}

// ─── Start ────────────────────────────────────────────────────────────────────

bool http_server_start(VehicleController& vehicle, NvsStorageAdapter& config_store) {
    g_vehicle = &vehicle;
    g_config  = &config_store;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn     = httpd_uri_match_wildcard;
    // Every route goes through the two /* wildcards (GET, POST) — see handle_all above.
    config.max_uri_handlers = 2;
    config.stack_size       = 8192;
    config.lru_purge_enable = true;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HTTP server");
        return false;
    }

    // Wildcard GET handler
    httpd_uri_t get_handler = {
        .uri      = "/*",
        .method   = HTTP_GET,
        .handler  = handle_all,
        .user_ctx = nullptr,
    };
    // Wildcard POST handler
    httpd_uri_t post_handler = {
        .uri      = "/*",
        .method   = HTTP_POST,
        .handler  = handle_all,
        .user_ctx = nullptr,
    };
    // A partial registration is worse than none — a wildcard dispatcher that answers some methods
    // and silently 404s the others — so ANY registration failure unwinds the whole HTTP start: stop
    // the server and report failure to app_main (issue #204, Scenario D).
    if (httpd_register_uri_handler(server, &get_handler)  != ESP_OK ||
        httpd_register_uri_handler(server, &post_handler) != ESP_OK) {
        ESP_LOGE(TAG, "wildcard handler registration failed — aborting HTTP start");
        httpd_stop(server);
        return false;
    }

    ESP_LOGI(TAG, "HTTP server started on :80");
    return true;
}
