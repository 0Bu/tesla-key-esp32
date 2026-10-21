// HTTP server core: the single wildcard dispatcher, the handle_all try/catch OOM guard
// every handler runs under, and server startup. The route handlers themselves live in
// http_api.cpp (evcc API), http_status.cpp (web UI + status/diag), http_ota.cpp (OTA),
// http_config.cpp (setup/config) and mcp_server.cpp (MCP endpoint); shared helpers in
// http_common.cpp. See http_handlers.hpp for the split map.

#include "http_handlers.hpp"
#include "logic/http_route.hpp"
#include "runtime_admission.hpp"
#include "net.hpp"
#include "stack_watch.hpp"
#include "logic/http_origin.hpp"
#include <esp_log.h>
#include <mdns.h>
#include <sdkconfig.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <cstring>
#include <exception>

#if defined(CONFIG_LWIP_IPV6) && CONFIG_LWIP_IPV6
#error "HTTP owned-address checks require an IPv6 socket policy before enabling IPv6"
#endif

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

static bool read_header_bounded(httpd_req_t* req, const char* name, char* out, size_t capacity) {
    out[0] = '\0';
    const size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0) return true;
    if (len >= capacity) return false;
    return httpd_req_get_hdr_value_str(req, name, out, capacity) == ESP_OK;
}

// Snapshot both the active lease and the socket-local address. The latter preserves a page on
// the still-owned WiFi address after Ethernet takes the default route. mDNS may rename on conflict;
// read its bounded live name instead of trusting the request's Host as its own authority.
__attribute__((noinline))
static bool owned_host_allowed(httpd_req_t* req, const char* host, const char* active_netif_ip) {
    char socket_ip[16] = {};
    const int fd = httpd_req_to_sockfd(req);
    if (fd >= 0) {
        struct sockaddr_in local{};
        socklen_t local_len = sizeof(local);
        if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&local), &local_len) == 0 &&
            local.sin_family == AF_INET) {
            inet_ntop(AF_INET, &local.sin_addr, socket_ip, sizeof(socket_ip));
        }
    }
    char mdns_hostname[MDNS_NAME_BUF_LEN] = {};
    if (mdns_hostname_get(mdns_hostname) != ESP_OK) mdns_hostname[0] = '\0';
    const std::string_view device_ips[] = {active_netif_ip, socket_ip};
    const size_t ip_count = sizeof(device_ips) / sizeof(device_ips[0]);
    return tk::device_host_allowed(host, device_ips, ip_count, mdns_hostname);
}

// Keep Referer's large fixed buffer separate from the owned-address scratch and the browser
// header buffers: each existing HTTP frame has its own reviewed stack maximum.
__attribute__((noinline))
static bool origin_allowed_for_owned_addresses(
    httpd_req_t* req, bool is_post, const char* host, const char* origin, const char* fetch_site,
    bool has_custom_header, const char* active_netif_ip) {
    char referer[192];
    if (!read_header_bounded(req, "Referer", referer, sizeof(referer))) return false;
    const bool host_owned = owned_host_allowed(req, host, active_netif_ip);
    return tk::mutation_request_allowed_for_owned_host(is_post, host_owned, host, origin,
                                                      fetch_site, referer, has_custom_header);
}

static void active_ipv4(char* out, size_t capacity) {
    out[0] = '\0';
    esp_netif_t* netif = tk::net_active_netif();
    esp_netif_ip_info_t ip{};
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        esp_ip4addr_ntoa(&ip.ip, out, capacity);
    }
}

__attribute__((noinline))
static bool diagnostic_host_allowed(httpd_req_t* req) {
    char host[128];
    if (!read_header_bounded(req, "Host", host, sizeof(host))) return false;
    char device_ip[16];
    active_ipv4(device_ip, sizeof(device_ip));
    return owned_host_allowed(req, host, device_ip);
}

// Preserve the documented headerless trusted-LAN API used by evcc/curl on POST, but do not let a
// foreign browser origin borrow the user's LAN reachability for a mutating request. Host is first
// bound to the device's own name/IP so a DNS-rebinding page cannot make attacker-controlled Host
// and Origin compare equal. Mutating GETs are guarded against headerless requests (Chromium plain-HTTP
// LAN behavior) and require a valid Referer, Origin, Sec-Fetch-Site, or custom X-Requested-With header.
static bool browser_mutation_allowed(httpd_req_t* req) {
    const bool is_post = req->method == HTTP_POST;
    char origin[192];
    char fetch_site[32];
    if (!read_header_bounded(req, "Origin", origin, sizeof(origin)) ||
        !read_header_bounded(req, "Sec-Fetch-Site", fetch_site, sizeof(fetch_site))) {
        return false;
    }
    const bool has_custom_header = (httpd_req_get_hdr_value_len(req, "X-Requested-With") > 0);

    char host[128];
    if (!read_header_bounded(req, "Host", host, sizeof(host))) return false;
    char device_ip[16];
    active_ipv4(device_ip, sizeof(device_ip));
    return origin_allowed_for_owned_addresses(req, is_post, host, origin, fetch_site,
                                             has_custom_header, device_ip);
}

static esp_err_t reject_cross_origin_mutation(httpd_req_t* req) {
    ESP_LOGW(TAG, "rejected cross-origin mutation");
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "cross-origin mutation rejected");
}
// ─── Wildcard handler dispatching ─────────────────────────────────────────────

// Single catch-all handler registered for /*
static esp_err_t handle_all_dispatch(GuardedReq rq) {
    httpd_req_t* req = rq.req();
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

    const tk::HttpVerb verb = GET ? tk::HttpVerb::Get
                                  : (POST ? tk::HttpVerb::Post : tk::HttpVerb::Other);
    const tk::HttpRoute route = tk::classify_http_route(verb, path);
    if (tk::http_route_requires_owned_host(verb, route) && !diagnostic_host_allowed(req)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "device-owned Host required");
    }
    if (tk::http_route_requires_vehicle_runtime(route) &&
        !tk::runtime_admission_vehicle_ready()) {
        ESP_LOGW(TAG, "vehicle-active route refused while runtime is not ready: %s", path);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(
            req, "{\"error\":\"vehicle runtime unavailable\",\"retryable\":false}");
    }
    switch (route) {
        case tk::HttpRoute::Command:        return handle_command(rq);
        case tk::HttpRoute::VehicleData:    return handle_vehicle_data(rq);
        case tk::HttpRoute::BodyController: return handle_body_controller(rq);
        case tk::HttpRoute::OtaCheck:       return handle_ota_check(rq);
        case tk::HttpRoute::OtaUpdate:      return handle_ota_update(rq);
        case tk::HttpRoute::OtaStatus:      return handle_ota_status(rq);
        case tk::HttpRoute::OtaChangelog:   return handle_ota_changelog(rq);
        case tk::HttpRoute::GenKeys:        return handle_gen_keys(rq);
        case tk::HttpRoute::SetTime:        return handle_set_time(rq);
        case tk::HttpRoute::SetVin:         return handle_set_vin(rq);
        case tk::HttpRoute::SetMqtt:        return handle_set_mqtt(rq);
        case tk::HttpRoute::SetSyslog:      return handle_set_syslog(rq);
        case tk::HttpRoute::SetWifi:        return handle_set_wifi(rq);
        case tk::HttpRoute::SetOta:         return handle_set_ota(rq);
        case tk::HttpRoute::Scan:           return handle_scan(rq);
        case tk::HttpRoute::Coredump:       return handle_coredump(rq);
        case tk::HttpRoute::CrashDismiss:   return handle_crash_dismiss(rq);
        case tk::HttpRoute::Heap:           return handle_heap(rq);
        case tk::HttpRoute::McpPost:        return mcp_handle_post(rq);
        case tk::HttpRoute::McpGet:         return mcp_handle_get(rq);
        case tk::HttpRoute::Version:        return handle_version(rq);
        case tk::HttpRoute::Status:         return handle_status(rq);
        case tk::HttpRoute::Diag:           return handle_diag(rq);
        case tk::HttpRoute::Index:          return handle_index(rq);
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
    HttpGuardContext context{req, {}};
    const GuardedReq rq{&context};
    esp_err_t result = ESP_FAIL;
    try {
        // IDF otherwise purges every unread declared body after an ESP_OK reply.
        // This also covers origin/route/admission rejections before the reader.
        // Retire body-bearing connections explicitly; TX remains available for
        // the response, while cleanup cannot restart a slow peer's RX budget.
        struct RetireBodyConnection {
            httpd_req_t* req;
            ~RetireBodyConnection() noexcept {
                if (req->content_len > 0) (void)shutdown(httpd_req_to_sockfd(req), SHUT_RD);
            }
        } retire_body_connection{req};
        if (req->content_len > 0) (void)httpd_resp_set_hdr(req, "Connection", "close");
        // Sample on every exit, including the exception fallbacks. The request that came closest to
        // the limit is exactly the one most likely to throw; a destructor also covers every early
        // return without duplicating the measurement at each route. Construct it inside the outer
        // boundary so even a future throwing constructor cannot escape into the C httpd frame.
        struct SampleHttpdStackOnExit {
            ~SampleHttpdStackOnExit() noexcept { tk::stack_watch_sample(tk::StackWatch::Httpd); }
        } sample_httpd_stack;
        result = handle_all_dispatch(rq);
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "handler for %s threw (%s) — containing reply failure", req->uri, e.what());
        result = send_effect_exception(rq);
    } catch (...) {
        ESP_LOGE(TAG, "handler for %s threw (unknown) — containing reply failure", req->uri);
        result = send_effect_exception(rq);
    }
    run_effect_continuation(rq);
    return result;
}

// ─── Start ────────────────────────────────────────────────────────────────────

bool http_server_start(VehicleController& vehicle, NvsStorageAdapter& config_store) {
    g_vehicle = &vehicle;
    g_config  = &config_store;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.recv_wait_timeout = tk::BODY_RECV_TIMEOUT_SECONDS;
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
