// HTTP server core: the single wildcard dispatcher, the handle_all try/catch OOM guard
// every handler runs under, and server startup. The route handlers themselves live in
// http_api.cpp (evcc API), http_status.cpp (web UI + status/diag), http_ota.cpp (OTA),
// http_config.cpp (setup/config) and mcp_server.cpp (MCP endpoint); shared helpers in
// http_common.cpp. See http_handlers.hpp for the split map.

#include "http_handlers.hpp"
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
    const char* uri = req->uri;
    size_t i = 0;
    for (; uri[i] && uri[i] != '?' && i + 1 < n; ++i) buf[i] = uri[i];
    buf[i] = '\0';
    return buf;
}

// True if `path` ends with `suffix` (exact tail match) — used for the parameterized API routes
// whose path carries the VIN, e.g. ".../{VIN}/vehicle_data".
static bool path_ends_with(const char* path, const char* suffix) {
    size_t lp = strlen(path), ls = strlen(suffix);
    return lp >= ls && strcmp(path + lp - ls, suffix) == 0;
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

    // Parameterized API routes (the VIN is embedded in the path): match the trailing segment of
    // the query-stripped path. /command/ is matched as an interior segment since {CMD} follows.
    if (POST && strstr(path, "/command/"))                      return handle_command({req});
    if (GET  && path_ends_with(path, "/vehicle_data"))          return handle_vehicle_data({req});
    if (GET  && path_ends_with(path, "/body_controller_state")) return handle_body_controller({req});

    // Fixed routes — exact path match. "/ota/status" can no longer fall through to "/status".
    if (GET  && strcmp(path, "/ota/check")  == 0)               return handle_ota_check({req});
    if (POST && strcmp(path, "/ota/update") == 0)               return handle_ota_update({req});
    if (GET  && strcmp(path, "/ota/status") == 0)               return handle_ota_status({req});
    if (POST && strcmp(path, "/gen_keys")   == 0)               return handle_gen_keys({req});
    if (POST && strcmp(path, "/send_key")   == 0)               return handle_send_key({req});
    if (POST && strcmp(path, "/set_time")   == 0)               return handle_set_time({req});
    if (POST && strcmp(path, "/set_vin")    == 0)               return handle_set_vin({req});
    if (POST && strcmp(path, "/set_mqtt")   == 0)               return handle_set_mqtt({req});
    if (POST && strcmp(path, "/scan")       == 0)               return handle_scan({req});
    if (POST && strcmp(path, "/mcp")        == 0)               return mcp_handle_post({req});
    if (GET  && strcmp(path, "/mcp")        == 0)               return mcp_handle_get({req});
    if (GET  && strcmp(path, "/api/proxy/1/version") == 0)      return handle_version({req});
    if (GET  && strcmp(path, "/status")     == 0)               return handle_status({req});
    if (GET  && strcmp(path, "/diag")       == 0)               return handle_diag({req});
    if (GET  && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) return handle_index({req});

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", "not found");
    return send_json(req, 404, root);
}

// Catch-all wrapper: a handler that runs out of memory throws std::bad_alloc (e.g. a
// large response built on a fragmented heap). This task is invoked from the C httpd
// loop, so an escaping C++ exception unwinds into C frames → std::terminate → abort()
// → reboot. Contain it here and return 503 instead, keeping the device alive. (Root
// cause is the low largest-free-block; this is the safety net so no request can crash
// the box.)
static esp_err_t handle_all(httpd_req_t* req) {
    try {
        return handle_all_dispatch(req);
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "handler for %s threw (%s) — likely OOM; returning 503", req->uri, e.what());
    } catch (...) {
        ESP_LOGE(TAG, "handler for %s threw (unknown) — returning 503", req->uri);
    }
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "out of memory");
}

// ─── Start ────────────────────────────────────────────────────────────────────

bool http_server_start(VehicleController& vehicle, NvsStorageAdapter& config_store) {
    g_vehicle = &vehicle;
    g_config  = &config_store;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn     = httpd_uri_match_wildcard;
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
    httpd_register_uri_handler(server, &get_handler);

    // Wildcard POST handler
    httpd_uri_t post_handler = {
        .uri      = "/*",
        .method   = HTTP_POST,
        .handler  = handle_all,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &post_handler);

    ESP_LOGI(TAG, "HTTP server started on :80");
    return true;
}
