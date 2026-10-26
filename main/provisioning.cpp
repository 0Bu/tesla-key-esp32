#include "stack_watch.hpp"
#include "provisioning.hpp"
#include "ota_update.hpp"      // request-owned setup save/restart lease
#include "logic/http_body.hpp" // http_body_read() — reassemble a multi-segment POST body

#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "logic/vin.hpp"
#include "logic/effect_reply.hpp"
#include <type_traits>
#include "logic/captive.hpp"
#include "logic/setup_portal.hpp"
#include "logic/wifi_credentials.hpp"
#include "config_blob.hpp"
#include "net.hpp"
#include "boot_fatal.hpp"
#include "task_config.hpp"

static const char* TAG = "provisioning";
static const char* AP_SSID = "tesla-key-esp32-setup";

static NvsStorageAdapter* g_cfg = nullptr;

static void provisioning_boot_require(esp_err_t err, const char* component) {
    if (err == ESP_OK) return;
    ESP_LOGE(TAG, "%s failed: %s", component, esp_err_to_name(err));
    boot_fatal(component);
}

// ─── HTML form ──────────────────────────────────────────────────────────────

// Captive-portal setup page — embedded pre-gzipped (main/www/setup.html → setup.html.gz at
// build time, EMBED_FILES; see main/CMakeLists.txt). Served with Content-Encoding: gzip;
// length is end-start (a binary blob, not a NUL-terminated C string).
extern const uint8_t setup_html_gz_start[] asm("_binary_setup_html_gz_start");
extern const uint8_t setup_html_gz_end[]   asm("_binary_setup_html_gz_end");

// ─── x-www-form-urlencoded parsing ───────────────────────────────────────────

static bool url_decode(std::string_view in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%') {
            if (i + 2 >= in.size()) return false;
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            const int high = hex(in[i + 1]);
            const int low = hex(in[i + 2]);
            if (high < 0 || low < 0) return false;
            const char decoded = static_cast<char>(high * 16 + low);
            if (decoded == '\0') return false;
            out += decoded;
            i += 2;
        } else {
            if (c == '\0') return false;
            out += c;
        }
    }
    return true;
}

static bool form_field(std::string_view body, std::string_view key, std::string& out) {
    out.clear();
    size_t pos = body.find(key);
    while (pos != std::string_view::npos) {
        const size_t equals = pos + key.size();
        // Require an exact field boundary and '=' after the complete key; a substring such as
        // `old_ssid=` must never be accepted as the requested `ssid=` field.
        if ((pos == 0 || body[pos - 1] == '&') &&
            equals < body.size() && body[equals] == '=') {
            const size_t start = equals + 1;
            const size_t end = body.find('&', start);
            return url_decode(body.substr(start, end == std::string_view::npos
                                                     ? std::string_view::npos
                                                     : end - start),
                              out);
        }
        pos = body.find(key, pos + 1);
    }
    return true;  // An omitted field remains the empty value and is validated by its caller.
}

// ─── HTTP handlers ────────────────────────────────────────────────────────────

static esp_err_t form_get(httpd_req_t* req) {
    tk::StackWatchScope f135_callback_exit(tk::StackWatch::CaptiveHttp);

  try {
    // The portal only auto-pops if the joining OS's connectivity probe gets the answer THAT OS
    // keys on — iOS asks for captive.apple.com/hotspot-detect.html, Android for /generate_204,
    // Windows for /connecttest.txt. Answering all of them with 200 + the page (what this did
    // before) is a heuristic: iOS infers a portal from the unexpected body, Android may simply
    // leave the network "no internet" and never open anything. A 302 + Location is the one answer
    // all three understand, so the probe paths get that and the browser that follows it gets the
    // page. The decision is the host-tested logic/captive.hpp; the portal address is written in
    // exactly one place there, so the DNS answer, the DHCP option and this Location header cannot
    // drift apart.
    if (tk::captive_reply_for(req->uri, /*setup_mode=*/true) == tk::CaptiveReply::Redirect) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", tk::CAPTIVE_PORTAL_URI);
        // A redirect's body is empty, so no Content-Encoding here: these paths are walked by
        // minimal HTTP clients rather than browsers, and handing one a gzip header with nothing to
        // decode is how a probe ends up undecided instead of redirected.
        return httpd_resp_send(req, nullptr, 0);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    const size_t len = setup_html_gz_end - setup_html_gz_start;
    return httpd_resp_send(req, (const char*)setup_html_gz_start, len);
  } catch (...) {
    // This is an httpd C callback. A future throwing helper must degrade to a valid handler
    // return rather than unwinding through the server task.
    return ESP_ERR_NO_MEM;
  }
}

struct PortalSaveContext {
    OtaConfigRestartGuard restart{};
    tk::EffectReplyState effect{};
};
static_assert(std::is_nothrow_default_constructible<PortalSaveContext>::value &&
              std::is_nothrow_destructible<PortalSaveContext>::value,
              "portal context must not throw outside its callback catch");

static esp_err_t portal_save_send(httpd_req_t* req, PortalSaveContext& context,
                                  const char* status, const char* type, const char* body) noexcept {
    if (context.effect.terminal) return ESP_FAIL;
    context.effect.terminal = true;
    try {
        esp_err_t result = httpd_resp_set_status(req, status);
        if (result != ESP_OK) return result;
        result = httpd_resp_set_type(req, type);
        if (result != ESP_OK) return result;
        return httpd_resp_sendstr(req, body);
    } catch (...) { return ESP_FAIL; }
}

static esp_err_t portal_save_result(httpd_req_t* req, PortalSaveContext& context) noexcept {
    if (!context.effect.success())
        return portal_save_send(req, context, "500 Internal Server Error", "text/plain",
            "configuration write outcome unknown; no restart requested");
    if (context.effect.terminal) return ESP_FAIL;
    context.effect.terminal = true;
    try {
        esp_err_t result = httpd_resp_set_status(req, "200 OK");
        if (result != ESP_OK) return result;
        result = httpd_resp_set_type(req, "text/html");
        if (result != ESP_OK) return result;
        static constexpr char prefix[] =
            "<!doctype html><meta charset=utf-8><body style='font-family:system-ui'>"
            "<h2>Saved &#9989;</h2>";
        result = httpd_resp_send_chunk(req, prefix, sizeof(prefix) - 1);
        if (result != ESP_OK) return result;
        const bool warning = tk::config_image_warning(context.effect.image);
        result = httpd_resp_sendstr_chunk(req, warning ? "<p id=firmware-warning>" : "<p>");
        if (result != ESP_OK) return result;
        const char* message = warning
            ? tk::config_commit_reason(context.effect.image)
            : "Configuration saved for the next boot. Rebooting to connect to your WiFi.";
        result = httpd_resp_send_chunk(req, message, std::strlen(message));
        if (result != ESP_OK) return result;
        result = httpd_resp_sendstr_chunk(req, "</p>");
        if (result != ESP_OK) return result;
        return httpd_resp_send_chunk(req, nullptr, 0);
    } catch (...) { return ESP_FAIL; }
}

static void portal_save_continue(PortalSaveContext& context) noexcept {
    if (context.effect.take_continuation() != tk::ReplyContinuation::SuccessfulConfigRestart ||
        !context.restart.owns_gate()) return;
    try {
        vTaskDelay(pdMS_TO_TICKS(1000));
        tk::stack_watch_sample(tk::StackWatch::CaptiveHttp); // before terminal effect
        esp_restart();
    } catch (...) { /* never retry or unwind into the httpd C callback */ }
}

static esp_err_t save_post_impl(httpd_req_t* req, PortalSaveContext& context) {
    static constexpr size_t kSaveBodyMaxBytes = 1024;
    const int len = req->content_len;
    if (len <= 0) {
        (void)portal_save_send(req, context, "400 Bad Request", "text/plain", "empty form");
        return ESP_FAIL;
    }

    // Fixed stack storage: the full 1024-byte wire limit plus one mandatory terminator byte.
    // A 1025-byte Content-Length is rejected before recv, so the terminator never leaves the array.
    std::array<char, kSaveBodyMaxBytes + 1> body{};
    if (!tk::http_body_fits_buffer(static_cast<size_t>(len), body.size())) {
        (void)portal_save_send(req, context, "400 Bad Request", "text/plain", "oversized form");
        return ESP_FAIL;
    }
    // Reassemble the whole body: httpd_req_recv may return only the first TCP segment, so a single
    // recv could persist a truncated ssid/pass/vin. Loop until content_len (absolute deadline) —
    // logic/http_body.hpp. The reader writes the terminator only after all `len` bytes arrive.
    int got = tk::http_body_read(body.data(), body.size(), len,
        [req](char* dst, size_t want) -> tk::BodyChunk {
            int r = httpd_req_recv(req, dst, want);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) return { tk::BodyRecv::Timeout, 0 };
            if (r <= 0)                      return { tk::BodyRecv::Error,   0 };
            return { tk::BodyRecv::Data, static_cast<size_t>(r) };
        }, []() -> uint64_t { return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL; });
    if (got < 0 || got != len) {
        // Prevent IDF's request cleanup from draining a slow residual body.
        (void)shutdown(httpd_req_to_sockfd(req), SHUT_RD);
        (void)portal_save_send(req, context, "400 Bad Request", "text/plain", "recv failed");
        return ESP_FAIL;
    }
    if (tk::http_body_has_embedded_nul(body.data(), static_cast<size_t>(got))) {
        (void)portal_save_send(req, context, "400 Bad Request", "text/plain", "invalid NUL in form");
        return ESP_FAIL;
    }
    const std::string_view body_view(body.data(), static_cast<size_t>(got));

    std::string ssid;
    std::string pass;
    std::string vin;
    if (!form_field(body_view, "ssid", ssid) ||
        !form_field(body_view, "pass", pass) ||
        !form_field(body_view, "vin", vin)) {
        (void)portal_save_send(req, context, "400 Bad Request", "text/plain", "invalid form encoding");
        return ESP_FAIL;
    }

    const tk::WifiCredentialError wifi_error = tk::wifi_credentials_error(ssid, pass);
    if (wifi_error != tk::WifiCredentialError::None) {
        std::string message = "<p>";
        message += tk::wifi_credentials_reason(wifi_error);
        message += ". <a href=/>Back</a>.</p>";
        return portal_save_send(req, context, "200 OK", "text/html", message.c_str());
    }

    // Normalise + validate like /set_vin (trim, uppercase, tk::vin_is_plausible) — the
    // setup page's JS filters too, but a POST without it could otherwise persist a
    // 17-char-but-implausible VIN that silently disables pairing at boot.
    size_t vs = vin.find_first_not_of(" \t\r\n");
    size_t ve = vin.find_last_not_of(" \t\r\n");
    vin = (vs == std::string::npos) ? std::string{} : vin.substr(vs, ve - vs + 1);
    for (char& c : vin) c = (char)std::toupper((unsigned char)c);

    if (!vin.empty() && !tk::vin_is_plausible(vin)) {
        return portal_save_send(req, context, "200 OK", "text/html",
            "<p>VIN must be 17 valid characters. <a href=/>Back</a>.</p>");
    }

    tk::ConfigBlob cfg;
    if (!tk::cfg_load_for_update(*g_cfg, cfg)) {
        // The legacy mirrors cannot vouch for the stored VIN or service settings, and the
        // portal must not rewrite an identity from them. Leave the stored data alone.
        return portal_save_send(req, context, "503 Service Unavailable", "text/html",
            "<p>The stored configuration is unreadable, so nothing was changed. The device must "
            "be recovered over USB instead of being overwritten from stale data. "
            "<a href=/>Back</a>.</p>");
    }
    // The captive portal runs before VehicleController exists, so it cannot transactionally
    // rotate the private key and sessions. It may set the first VIN or retain the current one,
    // but changing an established vehicle identity here would bind the old key/session to a new
    // car. Use /set_vin on the running firmware for that destructive, confirmed transaction.
    if (!cfg.vin.empty() && !vin.empty() && vin != cfg.vin) {
        return portal_save_send(req, context, "409 Conflict", "text/html",
            "<p>An existing VIN cannot be changed from recovery setup because key rotation is "
            "not available yet. Reconnect using the saved VIN, then change it in the device UI. "
            "<a href=/>Back</a>.</p>");
    }

    if (!context.restart.try_begin())
        return portal_save_send(req, context, "409 Conflict", "text/plain", tk::kConfigRestartBusyReason);

    // Persist the whole form as ONE atomic entry, and reboot only if that write succeeds.
    //
    // This used to be three independent NVS writes (ssid, pass, vin). Checking each return value —
    // which it did — reports a tear but cannot undo the writes that already landed, and a power cut
    // between two of them reports nothing at all: the device then comes up with a new SSID beside
    // the old password, joins nothing, and can only be fixed over USB. One CRC-checked blob is
    // all-or-nothing across both failure modes (logic/config_store.hpp).
    // Commit the typed credentials AND retire any rollback state left over from an earlier
    // /set_wifi attempt: a stale armed backup would otherwise let a later failure of THESE fresh
    // credentials restore an unrelated older network (logic/config_store.hpp).
    tk::config_apply_setup_wifi(cfg, ssid, pass);
    if (!vin.empty()) cfg.vin = vin;
    // No rollback backup from HERE: the form replaces the stored pair outright. First-time setup
    // has nothing to fall back to, and arming one would mean a failed first attempt "restores" an
    // empty configuration — this same portal, one reboot later. In the retrying portal the stored
    // pair is still retried until the form is saved; saving is the explicit choice to replace it.

    context.effect.attempting(tk::RequestEffect::Configuration);
    if (!tk::cfg_save(*g_cfg, cfg)) return portal_save_result(req, context);
    context.effect.succeeded();
    context.effect.continuation = tk::ReplyContinuation::SuccessfulConfigRestart;
    // The setup portal never confirms an image. Preserve the queried state before any log/reply.
    context.effect.image = ota_config_image_state(context.restart);
    ESP_LOGI(TAG, "setup configuration saved; automatic restart requested");
    return portal_save_result(req, context);
}

static esp_err_t save_post(httpd_req_t* req) {
    tk::StackWatchScope f135_callback_exit(tk::StackWatch::CaptiveHttp);

    PortalSaveContext context{};
    esp_err_t result = ESP_FAIL;
    try {
        result = save_post_impl(req, context);
    } catch (...) {
        // Keep the owner and committed outcome alive through outer catch and continuation.
        result = context.effect.needs_effect_reply()
            ? portal_save_result(req, context)
            : portal_save_send(req, context, "503 Service Unavailable", "text/plain", "out of memory");
    }
    portal_save_continue(context);
    return result;
}

// ─── Captive-portal DNS ───────────────────────────────────────────────────────
// Answers every A query with the portal address so phones (incl. iOS Captive Network
// Assistant) detect a captive portal and auto-open the setup form. The reply is built by the
// host-tested tk::build_captive_dns_reply(); malformed or unsupported queries are dropped.
// The task stops within about a second once s_dns_stop is set (the receive timeout re-checks it).

static std::atomic<bool> s_dns_stop{false};
static std::atomic<bool> s_dns_running{false};

static void dns_task(void*) {
    tk::stack_watch_sample(tk::StackWatch::CaptiveDns); // current task at entry
    tk::StackWatchScope f135_task_exit(tk::StackWatch::CaptiveDns);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "DNS socket failed");
        s_dns_running.store(false);
        tk::stack_watch_sample(tk::StackWatch::CaptiveDns); // no C++ unwind on delete
        vTaskDelete(nullptr);
        return;
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(53);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "DNS bind failed");
        close(sock);
        s_dns_running.store(false);
        tk::stack_watch_sample(tk::StackWatch::CaptiveDns); // no C++ unwind on delete
        vTaskDelete(nullptr);
        return;
    }
    timeval timeout{};
    timeout.tv_sec = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        // Without the timeout the task could not see the stop flag and would outlive the portal,
        // answering LAN DNS queries with the portal address. The form stays reachable by address.
        ESP_LOGW(TAG, "DNS receive timeout not set — captive DNS off");
        close(sock);
        s_dns_running.store(false);
        tk::stack_watch_sample(tk::StackWatch::CaptiveDns); // no C++ unwind on delete
        vTaskDelete(nullptr);
        return;
    }

    // Both buffers live on this task's stack (4096 B): the task exists only while the portal runs,
    // so a static reply would cost internal DRAM for the whole uptime.
    uint8_t buf[512];
    uint8_t reply[tk::kCaptiveDnsReplyMax];
    while (!s_dns_stop.load()) {
        tk::StackWatchScope f135_iteration_exit(tk::StackWatch::CaptiveDns);
        sockaddr_in src{};
        socklen_t srclen = sizeof(src);
        const int n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&src, &srclen);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            vTaskDelay(pdMS_TO_TICKS(100));  // a persistent socket error must not spin this task
            continue;
        }
        if (n <= 0) continue;  // receive timeout: re-check the stop flag
        const size_t len = tk::build_captive_dns_reply(buf, static_cast<size_t>(n), reply, sizeof(reply));
        if (len) sendto(sock, reply, len, 0, (sockaddr*)&src, srclen);
    }
    close(sock);
    s_dns_running.store(false);
    tk::stack_watch_sample(tk::StackWatch::CaptiveDns); // no C++ unwind on delete
    vTaskDelete(nullptr);
}

// ─── Portal lifecycle ─────────────────────────────────────────────────────────

struct SetupPortal {
    esp_netif_t*   ap_netif = nullptr;
    httpd_handle_t server   = nullptr;
};

// Bring up the setup AP, its DHCP option, the HTTP form and the captive DNS.
//
// keep_station: the WiFi driver already runs the station from net_start_wifi(), with its netif and
// event handlers. The portal then joins it as APSTA, so the stored credentials can be retried in
// the background, instead of tearing the station down for an AP-only portal.
static SetupPortal portal_open(NvsStorageAdapter& config_store, bool keep_station) {
    g_cfg = &config_store;
    SetupPortal portal;

    if (!keep_station) {
        // Defensive teardown in case a STA connection attempt ran first.
        esp_wifi_stop();
        esp_wifi_deinit();
    }

    const esp_err_t netif_err = esp_netif_init();
    if (netif_err != ESP_OK && netif_err != ESP_ERR_INVALID_STATE) {
        provisioning_boot_require(netif_err, "setup network interface substrate");
    }
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        provisioning_boot_require(loop_err, "setup network event loop");
    }
    esp_netif_t* ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) boot_fatal("setup AP network interface");
    portal.ap_netif = ap_netif;

    // RFC 8910 (DHCP option 114): hand the joining client the portal URI in its DHCP lease. Recent
    // iOS and Android prefer this over probing at all, so the portal opens without depending on the
    // probe-redirect heuristic below; a client that ignores the option still finds it that way, so
    // the two are belt and braces rather than alternatives.
    //
    // The buffer is STATIC on purpose: esp_netif stores the POINTER it is handed, not a copy, so a
    // stack buffer here would leave the DHCP server reading freed memory for the life of the AP.
    {
        // Copied at runtime rather than brace-initialised: CAPTIVE_PORTAL_URI is a `const char*`
        // (one definition shared with the DNS answer and the Location header), and a char array
        // cannot be initialised from a pointer. The COPY is what matters anyway — esp_netif wants a
        // mutable buffer and keeps the pointer it is given.
        static char portal_uri[64];
        std::snprintf(portal_uri, sizeof(portal_uri), "%s", tk::CAPTIVE_PORTAL_URI);
        esp_netif_dhcps_stop(ap_netif);
        esp_err_t opt = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                                               ESP_NETIF_CAPTIVEPORTAL_URI,
                                               portal_uri, (int)strlen(portal_uri));
        if (opt != ESP_OK) {
            // Named rather than discarded: "the portal doesn't pop" is the report this whole path
            // exists to prevent, and a silently-dropped option is exactly the evidence that was
            // missing when it did.
            ESP_LOGW(TAG, "captive-portal DHCP option not set (%s) — the portal will rely on the "
                          "probe redirect only", esp_err_to_name(opt));
        }
        esp_netif_dhcps_start(ap_netif);
    }

    if (!keep_station) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        provisioning_boot_require(esp_wifi_init(&cfg), "setup WiFi driver initialization");
    }

    wifi_config_t ap{};
    strncpy((char*)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len       = strlen(AP_SSID);
    ap.ap.channel        = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode       = WIFI_AUTH_OPEN;   // open network for easy setup

    // The portal is the sole WiFi setup path; with stored credentials the station stays beside it.
    provisioning_boot_require(esp_wifi_set_mode(keep_station ? WIFI_MODE_APSTA : WIFI_MODE_AP),
                              "setup AP mode");
    provisioning_boot_require(esp_wifi_set_config(WIFI_IF_AP, &ap), "setup AP configuration");
    if (!keep_station) provisioning_boot_require(esp_wifi_start(), "setup AP start");

    ESP_LOGI(TAG, "Setup AP up. Join WiFi '%s' and open %s", AP_SSID, tk::CAPTIVE_PORTAL_URI);

    httpd_config_t hcfg   = HTTPD_DEFAULT_CONFIG();
    hcfg.recv_wait_timeout = tk::BODY_RECV_TIMEOUT_SECONDS;
    hcfg.uri_match_fn     = httpd_uri_match_wildcard;
    hcfg.max_uri_handlers = 4;
    if (httpd_start(&portal.server, &hcfg) != ESP_OK) {
        boot_fatal("setup HTTP server");
    }

    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST,
                         .handler = save_post, .user_ctx = nullptr };
    // Wildcard GET → form (also serves captive-portal probe requests)
    httpd_uri_t form = { .uri = "/*", .method = HTTP_GET,
                         .handler = form_get, .user_ctx = nullptr };
    if (httpd_register_uri_handler(portal.server, &save) != ESP_OK ||
        httpd_register_uri_handler(portal.server, &form) != ESP_OK) {
        ESP_LOGE(TAG, "setup portal handler registration failed; halting setup mode");
        httpd_stop(portal.server);
        boot_fatal("setup HTTP handlers");
    }

    // Redirect phones only after the entire HTTP surface is ready. Otherwise an orphaned DNS
    // task could keep pointing clients at a portal that failed to start.
    s_dns_stop.store(false);
    s_dns_running.store(true);
    if (xTaskCreate(dns_task, "captive_dns", 4096, nullptr, tk::kPrioCaptiveDns, nullptr) != pdPASS) {
        s_dns_running.store(false);
        ESP_LOGW(TAG, "captive DNS task not started — portal still reachable at %s", tk::CAPTIVE_PORTAL_IP);
    }
    return portal;
}

// Stop advertising the setup AP: captive DNS, the form's HTTP server, then the AP interface. The
// station keeps running, so background retries continue and the normal web server can start.
// Returns false while the AP is still up; the caller reports it with tk::portal_close_failed() and
// retries after the backoff (idempotent).
static bool portal_close_ap(SetupPortal& portal) {
    s_dns_stop.store(true);
    for (int i = 0; i < 30 && s_dns_running.load(); ++i) vTaskDelay(pdMS_TO_TICKS(100));
    if (portal.server) {
        const esp_err_t stop_err = httpd_stop(portal.server);
        if (stop_err != ESP_OK) {
            // A failed stop can leave the server running. Retain its handle and retry through
            // the existing close backoff before allowing the normal port-80 server to start.
            ESP_LOGW(TAG, "setup HTTP server not stopped: %s", esp_err_to_name(stop_err));
            return false;
        }
        portal.server = nullptr;
    }
    const esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (mode_err != ESP_OK) {
        // The AP still runs on this netif; destroying it under the driver is worse than keeping it.
        ESP_LOGW(TAG, "setup AP not switched off: %s", esp_err_to_name(mode_err));
        return false;
    }
    // The mode switch posts WIFI_EVENT_AP_STOP; its default handler stops the DHCP server through
    // this netif. Give the event task (higher priority) the chance to run it before the netif is
    // destroyed, so the DHCP server is not left running on a freed interface.
    vTaskDelay(pdMS_TO_TICKS(200));
    if (portal.ap_netif) {
        esp_netif_destroy_default_wifi(portal.ap_netif);
        portal.ap_netif = nullptr;
    }
    return true;
}

static uint32_t portal_ap_clients() {
    wifi_sta_list_t stations{};
    return esp_wifi_ap_get_sta_list(&stations) == ESP_OK ? static_cast<uint32_t>(stations.num) : 0;
}

// ─── Entry points ───────────────────────────────────────────────────────────

void provisioning_run(NvsStorageAdapter& config_store) {
    portal_open(config_store, /*keep_station=*/false);
    // Never returns — the device stays in setup mode until the form reboots it.
    while (true) {
        tk::StackWatchScope f135_portal_wait_exit(tk::StackWatch::Main);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void provisioning_run_with_retry(NvsStorageAdapter& config_store) {
    if (!tk::net_wifi_station_started()) {
        // net_start_wifi() rejected the stored credentials before the driver existed (legacy or
        // build-time values that skipped validation). Retrying them is pointless and APSTA needs
        // a running station, so this is the terminal first-time portal.
        ESP_LOGW(TAG, "stored WiFi credentials unusable — setup portal without retry");
        provisioning_run(config_store);
    }
    SetupPortal portal = portal_open(config_store, /*keep_station=*/true);
    ESP_LOGW(TAG, "setup portal open; retrying the stored WiFi network in the background");
    // The host-tested logic/setup_portal.hpp defers its attempts under an AP client before the cap;
    // station-event retries remain independent. Close requests at idle/open-time limits use backoff
    // on failure. This loop samples inputs once a second using the 64-bit timer.
    tk::PortalState state{};
    bool ap_open = true;
    const int64_t opened_us = esp_timer_get_time();
    for (;;) {
        tk::StackWatchScope f135_portal_retry_exit(tk::StackWatch::Main);
        vTaskDelay(pdMS_TO_TICKS(1000));
        const uint32_t now_s = static_cast<uint32_t>((esp_timer_get_time() - opened_us) / 1000000);
        const uint32_t clients = ap_open ? portal_ap_clients() : 0;
        switch (tk::portal_step(state, {tk::net_is_up(), ap_open, clients, now_s})) {
            case tk::PortalAction::AttemptStation:
                ESP_LOGI(TAG, "retrying the stored WiFi network (attempt %u)", (unsigned)state.attempts);
                tk::net_wifi_retry_station();
                break;
            case tk::PortalAction::CloseAp:
                ESP_LOGW(TAG, "closing the open setup AP (open %u s, idle %u s); retries continue, "
                              "and a power cycle reopens it", (unsigned)now_s,
                         (unsigned)(now_s - state.idle_since_s));
                if (portal_close_ap(portal)) ap_open = false;
                else tk::portal_close_failed(state, now_s);
                break;
            case tk::PortalAction::Leave:
                // Fail closed: the boot continues (normal web server, BLE) only once the open AP
                // is down; a failed close is retried with backoff (logic/setup_portal.hpp).
                if (ap_open && !portal_close_ap(portal)) {
                    tk::portal_close_failed(state, now_s);
                    break;
                }
                ESP_LOGI(TAG, "network up via %s — leaving the setup portal", tk::net_link_str(tk::net_kind()));
                return;
            case tk::PortalAction::None:
                break;
        }
    }
}
