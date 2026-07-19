// Web-UI-facing routes: the embedded page itself and the endpoints that drive it:
//   GET  /        (embedded, pre-gzipped web UI)
//   GET  /status  (device + pairing state — a request/response snapshot of the live state)
//   GET  /diag    (in-memory diagnostic log)
//   POST /scan    (time-limited BLE discovery scan)
// Dispatched from handle_all in http_server.cpp (inside its try/catch OOM guard).
//
// The live web UI no longer polls /status on a timer: it holds one WebSocket to /events and the
// device pushes this same JSON (built by build_status_object() below) on a fixed cadence — see
// http_events.cpp. /status stays as the request/response form for curl/diagnostics and the
// post-OTA reboot probe (app.js waitReboot()); build_status_object() is the ONE builder both paths
// share, so the pushed frame and a manual GET can never drift.

#include "http_handlers.hpp"
#include "diag_log.hpp"
#include "mqtt_ha.hpp"
#include "logic/status_model.hpp"
#include "syslog.hpp"
#include <esp_netif.h>
#include <esp_app_desc.h>
#include <esp_wifi.h>
#include <ctime>
#include <string>

// ─── POST /scan — start a time-limited BLE discovery scan ─────────────────────

esp_err_t handle_scan(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    g_vehicle->ble_scan();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "result", true);
    cJSON_AddStringToObject(root, "reason", "scanning for nearby Teslas (~12s)");
    return send_json(req, 200, root);
}

// ─── GET /status — device + pairing state (drives the web UI) ──────────────────

static void current_ip(char* out, size_t sz) {
    out[0] = '\0';
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        esp_ip4addr_ntoa(&ip.ip, out, sz);
    }
}

// Whether the car is "live" (awake) vs merely sleeping vs unreachable is decided centrally
// in VehicleController::link_state(), shared with the MQTT bridge so the two never drift.

// cJSON visitor for tk::status::emit_status() — builds the response tree one-to-one as
// the model walks it (no intermediate field list; the contract layer adds zero heap).
// Under heap pressure cJSON_Create* returns NULL: attaching NULL is a cJSON no-op and
// every later attach to a NULL parent is too, so the walk degrades to a partial-but-
// valid document exactly like the old inline builder did; send_json's NULL-print guard
// covers total exhaustion.
namespace {
struct CjsonEmitter {
    cJSON* stack[5];
    int    depth = 0;
    explicit CjsonEmitter(cJSON* root) { stack[0] = root; }
    cJSON* top() { return stack[depth]; }
    void attach(const char* key, cJSON* node) {
        if (cJSON_IsArray(top())) cJSON_AddItemToArray(top(), node);
        else                      cJSON_AddItemToObject(top(), key, node);
    }
    void obj_begin(const char* key) { cJSON* o = cJSON_CreateObject(); attach(key, o); stack[++depth] = o; }
    void obj_end() { --depth; }
    void arr_begin(const char* key) { cJSON* a = cJSON_CreateArray(); attach(key, a); stack[++depth] = a; }
    void arr_end() { --depth; }
    void str(const char* key, const char* v) { attach(key, cJSON_CreateString(v)); }
    void num(const char* key, double v)      { attach(key, cJSON_CreateNumber(v)); }
    void boolean(const char* key, bool v)    { attach(key, cJSON_CreateBool(v)); }
};
}  // namespace

// Build the device + pairing + vehicle status object (caller owns the returned cJSON). The ONE
// source for BOTH the GET /status response and the /events WebSocket push (http_events.cpp), so the
// two can never drift. GATHER ONLY here: every which-field/when/what-value decision lives in the
// host-tested model (logic/status_model.hpp, golden CHECKs in test/test_logic.cpp); this collects
// the inputs under the existing locks, then emits. The throwing by-value getters (vin(), broker,
// syslog host, cached structs) all run in the GATHER below — BEFORE any cJSON is allocated — so a
// std::bad_alloc can't leak a partial tree (the /events broadcast task calls this every ~2 s with
// nothing to catch above it); the emit that follows only does cJSON allocs, which return NULL under
// pressure rather than throw. May return nullptr under total OOM; every caller guards for it.
cJSON* build_status_object() {
    tk::status::Inputs in;

    char ip[16];
    current_ip(ip, sizeof(ip));
    in.ip              = ip;
    in.vin             = g_vehicle->vin();
    in.version         = esp_app_get_description()->version;
    in.key_present     = g_vehicle->has_key();
    in.key_fingerprint = g_vehicle->key_fingerprint();
    in.key_created     = (long long)g_vehicle->key_created_at();
    in.paired          = g_vehicle->has_session();
    in.paired_at       = (long long)g_vehicle->paired_at();
    in.reauth          = g_vehicle->reauth_required();

    // WiFi: SSID + live signal strength (dBm) of the station link. The friendly name is
    // the highest 802.11 generation the AP advertises — the ESP32 radio itself may top
    // out lower, but the flags reflect the AP, so a Wi-Fi 6 router reads "Wi-Fi 6".
    wifi_ap_record_t ap{};
    if (wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        in.wifi_connected = true;
        in.wifi_ssid      = (const char*)ap.ssid;
        in.wifi_rssi      = ap.rssi;
        const char* std_ = ap.phy_11ax ? "Wi-Fi 6"
                         : ap.phy_11ac ? "Wi-Fi 5"
                         : ap.phy_11n  ? "Wi-Fi 4"
                         : ap.phy_11g  ? "802.11g"
                         : ap.phy_11b  ? "802.11b" : nullptr;
        if (std_) in.wifi_std = std_;
    }

    in.mqtt_configured = mqtt_ha_configured();
    in.mqtt_connected  = mqtt_ha_connected();
    in.mqtt_tls        = mqtt_ha_tls();
    in.mqtt_broker     = mqtt_ha_broker();
    in.mqtt_error      = mqtt_ha_last_error();

    // Syslog: configured / DNS-resolved (the delivery gate) / advisory ARP-ICMP
    // reachability hint. Gathered into the model like every other field.
    {
        SyslogStatus sy = syslog_status();
        in.syslog_configured = sy.configured;
        in.syslog_resolved   = sy.resolved;
        in.syslog_reachable  = sy.reachable;
        in.syslog_host       = sy.host;
        in.syslog_port       = sy.port;
        in.syslog_error      = sy.error;
    }

    in.ble_connected = g_vehicle->ble_connected();
    in.ble_scanning  = g_vehicle->ble_scanning();
    if (in.ble_connected) {
        int8_t rssi = 0;
        if (g_vehicle->ble_rssi(rssi)) { in.have_ble_rssi = true; in.ble_rssi = rssi; }
        in.ble_addr = g_vehicle->ble_peer();
        // Telemetry caches ride along only while the link is up (model rule).
        in.climate  = g_vehicle->get_cached_climate();
        in.drive    = g_vehicle->get_cached_drive();
        in.tires    = g_vehicle->get_cached_tires();
        in.closures = g_vehicle->get_cached_closures();
    } else {
        for (const auto& d : g_vehicle->ble_nearby())
            in.devices.push_back({ d.addr, d.name, d.rssi, d.connectable });
        in.connect_fail = g_vehicle->ble_connect_fail();
        int8_t srssi;
        if (g_vehicle->ble_seen_rssi(srssi)) { in.have_seen_rssi = true; in.seen_rssi = srssi; }
        in.target_connectable = g_vehicle->ble_target_connectable();
    }

    {
        tk::ble::PhaseView ph = g_vehicle->ble_phase();
        in.ble_phase   = tk::ble::phase_name(ph.kind);   // "" ⇒ model omits the block
        in.ble_phase_s = ph.secs;
    }
    in.link        = g_vehicle->link_state();
    in.vcsec_sleep = g_vehicle->vcsec_sleep_raw();
    in.charge      = g_vehicle->get_cached_charge();

    uint32_t ago = 0;
    if (g_vehicle->seconds_since_contact(ago)) { in.have_last_seen = true; in.last_seen_s = ago; }

    // The last-known charge snapshot ("last" / "last_seen_s", shown on the asleep card
    // regardless of link state) is emitted by the model from in.charge + in.last_seen.
    cJSON* root = cJSON_CreateObject();
    CjsonEmitter e(root);
    tk::status::emit_status(in, e);
    return root;
}

// GET /status — the request/response form of the live snapshot. The web UI's live feed uses
// /events (the WS push of this same object); this path still serves curl/diagnostics and the
// post-OTA reboot probe (app.js waitReboot()). Never cache: a stale copy sticks the hero on a
// transient state until a manual reload (matches "/" and "/diag").
esp_err_t handle_status(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return send_json(req, 200, build_status_object());   // send_json degrades a nullptr to 503
}

// ─── GET /diag — in-memory diagnostic log (for on-demand analysis) ────────────

esp_err_t handle_diag(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    if (query_param_is(req, "clear", "1"))        diag_log_clear();
    if (query_param_is(req, "verbose", "1"))      diag_set_verbose(true);
    else if (query_param_is(req, "verbose", "0")) diag_set_verbose(false);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Diag-Verbose", diag_verbose() ? "1" : "0");
    // Stream the log straight from its static buffer in (at most) two chunks. Building one
    // big std::string here used to throw std::bad_alloc when the whole buffer exceeded the
    // largest contiguous free block on a fragmented heap → uncaught in the httpd task →
    // abort() → reboot. Chunked send needs no large contiguous allocation, so /diag is safe
    // regardless of buffer size or fragmentation.
    diag_log_dump_chunks([req](const char* p, size_t n) {
        return n == 0 || httpd_resp_send_chunk(req, p, n) == ESP_OK;
    });
    return httpd_resp_send_chunk(req, nullptr, 0);  // terminate the chunked response
}

// ─── GET / — web UI (embedded from main/www/, inlined + gzipped at build time) ──

// The web UI is embedded pre-gzipped (see main/CMakeLists.txt: www/index.html +
// www/style.css + www/app.js are spliced into one page, then gzipped — ~13 KB vs 41 KB
// raw), the biggest first-paint win over a high-latency WiFi link. Browsers always accept
// gzip; the only consumer of "/" is a browser (evcc/curl hit /api and /status), so the
// encoding is sent unconditionally. Length is end-start (binary blob, not a C string).
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

esp_err_t handle_index(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    httpd_resp_set_type(req, "text/html");
    // The UI is embedded in the firmware and changes with every flash/OTA. Without
    // this, browsers cache index.html and keep rendering the OLD layout (with live
    // /status data) after an update — so tell them never to cache the page.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    const size_t len = index_html_gz_end - index_html_gz_start;
    return httpd_resp_send(req, (const char*)index_html_gz_start, len);
}
