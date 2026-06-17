#include "provisioning.hpp"

#include <cstring>
#include <cstdlib>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"

static const char* TAG = "provisioning";
static const char* AP_SSID = "tesla-key-esp32-setup";

static NvsStorageAdapter* g_cfg = nullptr;

// ─── HTML form ──────────────────────────────────────────────────────────────

static const char FORM_HTML[] =
"<!doctype html><html lang=en><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<meta name=theme-color content=#e82127>"
"<title>Tesla Key Setup</title>"
"<style>"
":root{--bg:#f6f7f9;--card:#fff;--fg:#1a1d21;--muted:#5b626b;--border:#e6e8ec;--accent:#e82127;--radius:16px;--shadow:0 1px 2px rgba(0,0,0,.04),0 8px 24px rgba(0,0,0,.05)}"
"@media(prefers-color-scheme:dark){:root{--bg:#0f1115;--card:#171a20;--fg:#f2f3f5;--muted:#9aa1ab;--border:#262a31;--shadow:0 1px 2px rgba(0,0,0,.4),0 8px 24px rgba(0,0,0,.3)}}"
"*{box-sizing:border-box}body{font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--fg);line-height:1.55;margin:0;padding:1.5rem 1.1rem}"
"main{max-width:26rem;margin:0 auto}.hero{text-align:center;padding:1rem 0 .25rem}"
".badge{display:inline-flex;align-items:center;gap:.4rem;font-size:.8rem;font-weight:600;letter-spacing:.02em;color:var(--accent);background:color-mix(in srgb,var(--accent) 12%,transparent);padding:.35rem .7rem;border-radius:999px;margin-bottom:.9rem}"
"h1{font-size:1.5rem;line-height:1.2;margin:0 0 .5rem;letter-spacing:-.02em}.lead{color:var(--muted);margin:0 auto;max-width:22rem;font-size:.95rem}"
".card{background:var(--card);border:1px solid var(--border);border-radius:var(--radius);box-shadow:var(--shadow);padding:1.25rem;margin-top:1.25rem}"
"label{display:block;font-size:.85rem;color:var(--muted);margin:.85rem 0 .3rem}form label:first-of-type{margin-top:0}"
"input{width:100%;padding:.7rem .8rem;font:inherit;font-size:1rem;color:var(--fg);background:var(--bg);border:1px solid var(--border);border-radius:10px}"
"input:focus{outline:none;border-color:var(--accent)}"
"button{margin-top:1.4rem;width:100%;font:inherit;font-weight:650;font-size:1.05rem;background:var(--accent);color:#fff;border:0;border-radius:999px;padding:.9rem;cursor:pointer;box-shadow:var(--shadow)}"
"button:active{transform:translateY(1px)}"
".foot{color:var(--muted);font-size:.82rem;text-align:center;margin-top:1.25rem}b{font-weight:650}"
"</style>"
"<main>"
"<div class=hero><span class=badge>&#9889; device setup</span><h1>Connect to WiFi</h1>"
"<p class=lead>Enter your network and Tesla VIN to finish setup.</p></div>"
"<form class=card method=POST action=/save>"
"<label>WiFi network (SSID)</label><input name=ssid required autofocus>"
"<label>WiFi password</label><input name=pass type=password>"
"<label>Tesla VIN (17 characters)</label><input name=vin maxlength=17 minlength=17 placeholder='5YJ...'>"
"<button>Save &amp; connect</button>"
"</form>"
"<p class=foot>The device reboots, joins your WiFi and is then reachable at "
"<b>http://tesla-key-esp32.local</b>.</p>"
"</main></html>";

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
    return httpd_resp_send(req, FORM_HTML, HTTPD_RESP_USE_STRLEN);
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

    g_cfg->save_str("wifi_ssid", ssid);
    g_cfg->save_str("wifi_pass", pass);
    if (vin.size() == 17) g_cfg->save_str("vin", vin);
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
