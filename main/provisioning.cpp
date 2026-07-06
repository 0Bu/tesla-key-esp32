#include "provisioning.hpp"

#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "logic/vin.hpp"

static const char* TAG = "provisioning";
static const char* AP_SSID = "tesla-key-esp32-setup";

static NvsStorageAdapter* g_cfg = nullptr;

// ─── HTML form ──────────────────────────────────────────────────────────────

// Captive-portal setup page — embedded pre-gzipped (main/www/setup.html → setup.html.gz at
// build time, EMBED_FILES; see main/CMakeLists.txt). Served with Content-Encoding: gzip;
// length is end-start (a binary blob, not a NUL-terminated C string).
extern const uint8_t setup_html_gz_start[] asm("_binary_setup_html_gz_start");
extern const uint8_t setup_html_gz_end[]   asm("_binary_setup_html_gz_end");

// ─── x-www-form-urlencoded parsing ───────────────────────────────────────────

static std::string url_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < in.size()) {
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return 0;
            };
            out += (char)(hex(in[i + 1]) * 16 + hex(in[i + 2]));
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

static std::string form_field(const std::string& body, const std::string& key) {
    std::string token = key + "=";
    size_t pos = body.find(token);
    while (pos != std::string::npos) {
        // ensure it's a field boundary (start or after '&')
        if (pos == 0 || body[pos - 1] == '&') break;
        pos = body.find(token, pos + 1);
    }
    if (pos == std::string::npos) return "";
    size_t start = pos + token.size();
    size_t end   = body.find('&', start);
    return url_decode(body.substr(start, end == std::string::npos ? std::string::npos
                                                                   : end - start));
}

// ─── HTTP handlers ────────────────────────────────────────────────────────────

static esp_err_t form_get(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    const size_t len = setup_html_gz_end - setup_html_gz_start;
    return httpd_resp_send(req, (const char*)setup_html_gz_start, len);
}

static esp_err_t save_post(httpd_req_t* req) {
    int len = req->content_len;
    if (len <= 0 || len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty/oversized form");
        return ESP_FAIL;
    }
    std::string body(len, '\0');
    int got = httpd_req_recv(req, body.data(), len);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    body.resize(got);

    std::string ssid = form_field(body, "ssid");
    std::string pass = form_field(body, "pass");
    std::string vin  = form_field(body, "vin");

    if (ssid.empty()) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req, "<p>SSID is required. <a href=/>Back</a>.</p>");
        return ESP_OK;
    }

    // Normalise + validate like /set_vin (trim, uppercase, tk::vin_is_plausible) — the
    // setup page's JS filters too, but a POST without it could otherwise persist a
    // 17-char-but-implausible VIN that silently disables pairing at boot.
    size_t vs = vin.find_first_not_of(" \t\r\n");
    size_t ve = vin.find_last_not_of(" \t\r\n");
    vin = (vs == std::string::npos) ? std::string{} : vin.substr(vs, ve - vs + 1);
    for (char& c : vin) c = (char)std::toupper((unsigned char)c);

    g_cfg->save_str("wifi_ssid", ssid);
    g_cfg->save_str("wifi_pass", pass);
    if (tk::vin_is_plausible(vin)) g_cfg->save_str("vin", vin);
    ESP_LOGI(TAG, "saved config: ssid='%s' vin='%s' — rebooting", ssid.c_str(), vin.c_str());

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!doctype html><meta charset=utf-8><body style='font-family:system-ui'>"
        "<h2>Saved &#9989;</h2><p>Rebooting and connecting to your WiFi. "
        "The device will be reachable at <b>http://tesla-key-esp32.local</b>.</p>");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// ─── Captive-portal DNS ───────────────────────────────────────────────────────
// Answers every A query with 192.168.4.1 so phones (incl. iOS Captive Network
// Assistant) detect a captive portal and auto-open the setup form.

static void dns_task(void*) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGW(TAG, "DNS socket failed"); vTaskDelete(nullptr); return; }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(53);
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(nullptr);
        return;
    }

    uint8_t buf[512];
    while (true) {
        sockaddr_in src{};
        socklen_t srclen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&src, &srclen);
        // Need at least a header (12) + one question; cap so the appended answer fits.
        if (n < 12 || n > (int)sizeof(buf) - 16) continue;

        buf[2] |= 0x80;   // QR = response
        buf[3] = 0x80;    // RA = 0, RCODE = 0 (AA bit optional)
        buf[6] = 0x00; buf[7] = 0x01;   // ANCOUNT = 1

        // Append answer: name pointer to the question (0xC00C), A/IN, TTL 60, 192.168.4.1
        static const uint8_t answer[] = {
            0xC0, 0x0C,             // name → offset 12
            0x00, 0x01,             // type A
            0x00, 0x01,             // class IN
            0x00, 0x00, 0x00, 0x3C, // TTL 60s
            0x00, 0x04,             // RDLENGTH 4
            192, 168, 4, 1          // RDATA
        };
        memcpy(buf + n, answer, sizeof(answer));
        sendto(sock, buf, n + sizeof(answer), 0, (sockaddr*)&src, srclen);
    }
}

// ─── Entry point ────────────────────────────────────────────────────────────

void provisioning_run(NvsStorageAdapter& config_store) {
    g_cfg = &config_store;

    // Defensive teardown in case a STA connection attempt ran first.
    esp_wifi_stop();
    esp_wifi_deinit();

    esp_netif_init();
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_err);
    }
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap{};
    strncpy((char*)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len       = strlen(AP_SSID);
    ap.ap.channel        = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode       = WIFI_AUTH_OPEN;   // open network for easy setup

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));  // AP only — portal is the sole WiFi setup path
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Setup AP up. Join WiFi '%s' and open http://192.168.4.1", AP_SSID);

    // Captive-portal DNS so the setup form pops up automatically on phones.
    xTaskCreate(dns_task, "captive_dns", 4096, nullptr, 5, nullptr);

    httpd_config_t hcfg   = HTTPD_DEFAULT_CONFIG();
    hcfg.uri_match_fn     = httpd_uri_match_wildcard;
    hcfg.max_uri_handlers = 4;
    httpd_handle_t server = nullptr;
    ESP_ERROR_CHECK(httpd_start(&server, &hcfg));

    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST,
                         .handler = save_post, .user_ctx = nullptr };
    httpd_register_uri_handler(server, &save);

    // Wildcard GET → form (also serves captive-portal probe requests)
    httpd_uri_t form = { .uri = "/*", .method = HTTP_GET,
                         .handler = form_get, .user_ctx = nullptr };
    httpd_register_uri_handler(server, &form);

    // Never returns — the device stays in setup mode until the form reboots it.
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
