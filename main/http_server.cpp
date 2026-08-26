// HTTP server core: the single wildcard dispatcher, the handle_all try/catch OOM guard
// every handler runs under, and server startup. The route handlers themselves live in
// http_api.cpp (evcc API), http_status.cpp (web UI + status/diag), http_ota.cpp (OTA),
// http_config.cpp (setup/config) and mcp_server.cpp (MCP endpoint); shared helpers in
// http_common.cpp. See http_handlers.hpp for the split map.

#include "http_handlers.hpp"
#include "net.hpp"
#include "stack_watch.hpp"
#include "logic/http_origin.hpp"
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

static bool read_header_bounded(httpd_req_t* req, const char* name, char* out, size_t capacity) {
    out[0] = '\0';
    const size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0) return true;
    if (len >= capacity) return false;
    return httpd_req_get_hdr_value_str(req, name, out, capacity) == ESP_OK;
}

// Preserve the documented headerless trusted-LAN API used by evcc/curl, but do not let a foreign
// browser origin borrow the user's LAN reachability for a mutating request. Host is first bound to
// the device's own name/IP so a DNS-rebinding page cannot make attacker-controlled Host and Origin
// compare equal. This is deliberately not authentication: a raw LAN peer can still call every
// endpoint described in docs/SECURITY.md.
static bool browser_mutation_allowed(httpd_req_t* req) {
    char origin[192];
    char fetch_site[32];
    if (!read_header_bounded(req, "Origin", origin, sizeof(origin)) ||
        !read_header_bounded(req, "Sec-Fetch-Site", fetch_site, sizeof(fetch_site))) {
        return false;
    }
    if (origin[0] == '\0' && fetch_site[0] == '\0') return true;

    char host[128];
    if (!read_header_bounded(req, "Host", host, sizeof(host))) return false;
    char device_ip[16] = {};
    esp_netif_t* netif = tk::net_active_netif();
    esp_netif_ip_info_t ip{};
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        esp_ip4addr_ntoa(&ip.ip, device_ip, sizeof(device_ip));
    }
    return tk::mutation_origin_allowed(host, origin, fetch_site, device_ip);
}

static esp_err_t reject_cross_origin_mutation(httpd_req_t* req) {
    ESP_LOGW(TAG, "rejected cross-origin mutation");
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "cross-origin mutation rejected");
}

// ─── Wildcard handler dispatching ─────────────────────────────────────────────

// Single catch-all handler registered for /*
static esp_err_t handle_all_dispatch(httpd_req_t* req) {
    char path[128];
    uri_path(req, path, sizeof(path));
    const bool GET  = req->method == HTTP_GET;
    const bool POST = req->method == HTTP_POST;
    ESP_LOGI(TAG, "REQ: %s %s", GET ? "GET" : (POST ? "POST" : "OTHER"), req->uri);

    if (tk::mutation_origin_required(POST, req->uri) && !browser_mutation_allowed(req)) {
        return reject_cross_origin_mutation(req);
    }

    // Log all headers to see what evcc is sending
    char header_val[128];
    if (httpd_req_get_hdr_value_str(req, "User-Agent", header_val, sizeof(header_val)) == ESP_OK) {
        ESP_LOGI(TAG, "  User-Agent: %s", header_val);
    }
    if (httpd_req_get_hdr_value_str(req, "Accept", header_val, sizeof(header_val)) == ESP_OK) {
        ESP_LOGI(TAG, "  Accept: %s", header_val);
    }

    // Parameterized API routes (the VIN is embedded in the path). The command route delegates its
    // exact shape to the handler parser; an unrelated path containing "/command/" stays a 404.
    if (POST && is_command_route(req->uri))                     return handle_command({req});
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
    if (POST && strcmp(path, "/set_syslog") == 0)               return handle_set_syslog({req});
    if (POST && strcmp(path, "/set_wifi")   == 0)               return handle_set_wifi({req});
    if (POST && strcmp(path, "/scan")       == 0)               return handle_scan({req});
    if (GET  && strcmp(path, "/coredump")   == 0)               return handle_coredump({req});
    if (POST && strcmp(path, "/crash/dismiss") == 0)            return handle_crash_dismiss({req});
    if (GET  && strcmp(path, "/heap")       == 0)               return handle_heap({req});
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
    // Sample on every exit, including the exception fallbacks. The request that came closest to
    // the limit is exactly the one most likely to throw; a destructor also covers every early
    // return without duplicating the measurement at each route.
    struct SampleHttpdStackOnExit {
        ~SampleHttpdStackOnExit() noexcept { tk::stack_watch_sample(tk::StackWatch::Httpd); }
    } sample_httpd_stack;
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
