#include "http_server.hpp"
#include "diag_log.hpp"
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_app_desc.h>
#include <esp_wifi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cJSON.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// Derived from PROJECT_VER (project root version.txt) so the reported version,
// the built firmware filename, and the web-installer manifest never drift apart.
static const char* fw_version() {
    static char buf[40] = {0};
    if (buf[0] == '\0') {
        snprintf(buf, sizeof(buf), "%s-esp32", esp_app_get_description()->version);
    }
    return buf;
}

static const char* TAG = "http_server";

// Global vehicle reference (set once at start)
static VehicleController* g_vehicle = nullptr;

// Largest POST body we accept, to bound the malloc in read_body() against a
// hostile/oversized Content-Length. All real requests here are tiny JSON objects.
static constexpr size_t MAX_BODY_LEN = 2048;

// ─── JSON helpers ─────────────────────────────────────────────────────────────

static esp_err_t send_json(httpd_req_t* req, int status, cJSON* root) {
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    if (status != 200) {
        httpd_resp_set_status(req, status == 400 ? "400 Bad Request"
                                : status == 403 ? "403 Forbidden"
                                : status == 404 ? "404 Not Found"
                                : status == 409 ? "409 Conflict"
                                : "500 Internal Server Error");
    }
    esp_err_t ret = httpd_resp_sendstr(req, body);
    free(body);
    return ret;
}

static cJSON* make_response(bool result, const char* command,
                             const char* vin, const char* reason) {
    cJSON* root     = cJSON_CreateObject();
    cJSON* response = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "response", response);
    cJSON_AddBoolToObject(response, "result", result);
    cJSON_AddStringToObject(response, "command", command);
    cJSON_AddStringToObject(response, "vin",     vin);
    cJSON_AddStringToObject(response, "reason",  reason);
    return root;
}

// Read POST body into a string (caller must free)
static char* read_body(httpd_req_t* req) {
    if (req->content_len == 0) return nullptr;
    if (req->content_len > MAX_BODY_LEN) {
        ESP_LOGW(TAG, "rejecting oversized body: %u bytes", (unsigned)req->content_len);
        return nullptr;
    }
    size_t len = req->content_len;
    char* buf  = (char*)malloc(len + 1);
    if (!buf) return nullptr;
    int received = httpd_req_recv(req, buf, len);
    if (received <= 0) { free(buf); return nullptr; }
    buf[received] = '\0';
    return buf;
}

// Extract last path segment from URI like /api/1/vehicles/VIN/command/CMD
static bool parse_uri(const char* uri, char* vin_out, size_t vin_sz,
                       char* cmd_out, size_t cmd_sz) {
    // URI pattern: /api/1/vehicles/{VIN}/command/{CMD}
    const char* vehicles = strstr(uri, "/vehicles/");
    if (!vehicles) return false;
    const char* vin_start = vehicles + strlen("/vehicles/");
    const char* command   = strstr(vin_start, "/command/");
    if (!command) return false;
    size_t vin_len = command - vin_start;
    if (vin_len == 0 || vin_len >= vin_sz) return false;
    strncpy(vin_out, vin_start, vin_len);
    vin_out[vin_len] = '\0';
    const char* cmd_start = command + strlen("/command/");
    // strip query string
    const char* q = strchr(cmd_start, '?');
    size_t cmd_len = q ? (size_t)(q - cmd_start) : strlen(cmd_start);
    if (cmd_len == 0 || cmd_len >= cmd_sz) return false;
    strncpy(cmd_out, cmd_start, cmd_len);
    cmd_out[cmd_len] = '\0';
    return true;
}

// Extract VIN from /api/1/vehicles/{VIN}/...
static bool parse_vin_only(const char* uri, char* vin_out, size_t vin_sz) {
    const char* vehicles = strstr(uri, "/vehicles/");
    if (!vehicles) return false;
    const char* vin_start = vehicles + strlen("/vehicles/");
    const char* slash = strchr(vin_start, '/');
    size_t vin_len = slash ? (size_t)(slash - vin_start) : strlen(vin_start);
    if (vin_len == 0 || vin_len >= vin_sz) return false;
    strncpy(vin_out, vin_start, vin_len);
    vin_out[vin_len] = '\0';
    return true;
}

// ─── POST /api/1/vehicles/{VIN}/command/{CMD} ─────────────────────────────────

static esp_err_t handle_command(httpd_req_t* req) {
    char vin[64], cmd[64];
    if (!parse_uri(req->uri, vin, sizeof(vin), cmd, sizeof(cmd))) {
        return send_json(req, 400, make_response(false, "unknown", "?", "invalid URI"));
    }
    ESP_LOGI(TAG, "CMD %s on VIN %s", cmd, vin);

    // Parse optional JSON body
    char* body = read_body(req);
    cJSON* json = body ? cJSON_Parse(body) : nullptr;
    free(body);

    bool ok = false;

    if      (strcmp(cmd, "wake_up")           == 0) ok = g_vehicle->wake_up();
    else if (strcmp(cmd, "charge_start")      == 0) ok = g_vehicle->charge_start();
    else if (strcmp(cmd, "charge_stop")       == 0) ok = g_vehicle->charge_stop();
    else if (strcmp(cmd, "charge_port_door_open")  == 0) ok = g_vehicle->charge_port_open();
    else if (strcmp(cmd, "charge_port_door_close") == 0) ok = g_vehicle->charge_port_close();
    else if (strcmp(cmd, "door_lock")         == 0) ok = g_vehicle->door_lock();
    else if (strcmp(cmd, "door_unlock")       == 0) ok = g_vehicle->door_unlock();
    else if (strcmp(cmd, "flash_lights")      == 0) ok = g_vehicle->flash_lights();
    else if (strcmp(cmd, "honk_horn")         == 0) ok = g_vehicle->honk_horn();
    else if (strcmp(cmd, "auto_conditioning_start") == 0) ok = g_vehicle->climate_start();
    else if (strcmp(cmd, "auto_conditioning_stop")  == 0) ok = g_vehicle->climate_stop();
    else if (strcmp(cmd, "set_charging_amps") == 0) {
        int amps = 0;
        if (json) {
            cJSON* j = cJSON_GetObjectItemCaseSensitive(json, "charging_amps");
            if (j) amps = (int)(cJSON_IsNumber(j) ? j->valuedouble
                                                   : atof(j->valuestring));
        }
        ok = g_vehicle->set_charging_amps(amps);
    }
    else if (strcmp(cmd, "set_charge_limit")  == 0) {
        int pct = 80;
        if (json) {
            cJSON* j = cJSON_GetObjectItemCaseSensitive(json, "percent");
            if (j) pct = (int)(cJSON_IsNumber(j) ? j->valuedouble
                                                  : atof(j->valuestring));
        }
        ok = g_vehicle->set_charge_limit(pct);
    }
    else if (strcmp(cmd, "set_sentry_mode")   == 0) {
        bool on = false;
        if (json) {
            cJSON* j = cJSON_GetObjectItemCaseSensitive(json, "on");
            if (j) on = cJSON_IsTrue(j);
        }
        ok = g_vehicle->set_sentry_mode(on);
    }
    else {
        cJSON_Delete(json);
        return send_json(req, 404, make_response(false, cmd, vin, "unknown command"));
    }

    cJSON_Delete(json);
    return send_json(req, 200,
        make_response(ok, cmd, vin, ok ? "command executed successfully"
                                       : "command failed or timed out"));
}

// ─── GET /api/1/vehicles/{VIN}/vehicle_data ───────────────────────────────────

static esp_err_t handle_vehicle_data(httpd_req_t* req) {
    char vin[64];
    parse_vin_only(req->uri, vin, sizeof(vin));

    ChargeStateResult cs{};
    bool ok = g_vehicle->get_charge_state(cs);

    // Shape MUST match the Tesla Fleet API as proxied by TeslaBleHttpProxy:
    //   { "response": { "response": { "charge_state": { ... } } } }
    // evcc reads e.g. .response.response.charge_state.battery_level and
    // .response.response.charge_state.charge_amps — note the doubled "response"
    // and the field name "charge_amps" (not "charging_amps").
    cJSON* root  = cJSON_CreateObject();
    cJSON* outer = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "response", outer);
    cJSON_AddBoolToObject(outer, "result", ok);
    cJSON_AddStringToObject(outer, "vin", vin);

    // Always emit a fully-populated charge_state. evcc's tesla-ble template parses
    // .response.response.charge_state.battery_range etc. as floats; a missing field
    // would make it parse "<nil>" and fail. On failure cs is zero-initialised, which
    // still yields valid numbers (and get_charge_state already falls back to the cache).
    cJSON* inner = cJSON_CreateObject();
    cJSON_AddItemToObject(outer, "response", inner);
    cJSON* state = cJSON_CreateObject();
    cJSON_AddItemToObject(inner, "charge_state", state);
    cJSON_AddStringToObject(state, "charging_state",
                            cs.charging_state.empty() ? "Disconnected" : cs.charging_state.c_str());
    cJSON_AddNumberToObject(state, "battery_level",    cs.battery_level);
    cJSON_AddNumberToObject(state, "charge_limit_soc", cs.charge_limit_soc);
    cJSON_AddNumberToObject(state, "charger_power",    cs.charger_power);
    cJSON_AddNumberToObject(state, "charge_rate",      cs.charge_rate);
    cJSON_AddNumberToObject(state, "charge_amps",      cs.charging_amps);
    cJSON_AddNumberToObject(state, "battery_range",    cs.battery_range);
    cJSON_AddStringToObject(outer, "reason", ok ? "success" : "stale or unavailable");

    return send_json(req, 200, root);
}

// ─── GET /api/1/vehicles/{VIN}/body_controller_state ─────────────────────────

static esp_err_t handle_body_controller(httpd_req_t* req) {
    char vin[64];
    parse_vin_only(req->uri, vin, sizeof(vin));

    VehicleStatusResult vs{};
    bool ok = g_vehicle->get_vehicle_status(vs);

    cJSON* root     = cJSON_CreateObject();
    cJSON* response = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "response", response);
    cJSON_AddBoolToObject(response, "result", ok);
    cJSON_AddStringToObject(response, "vin", vin);

    if (ok) {
        cJSON* data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "vehicle_lock_state",   vs.lock_state.c_str());
        cJSON_AddStringToObject(data, "vehicle_sleep_status", vs.sleep_status.c_str());
        cJSON_AddStringToObject(data, "user_presence",        vs.user_presence.c_str());
        cJSON_AddItemToObject(response, "data", data);
        cJSON_AddStringToObject(response, "reason", "success");
    } else {
        cJSON_AddStringToObject(response, "reason", "failed to retrieve vehicle status");
    }

    return send_json(req, 200, root);
}

// ─── POST /gen_keys ───────────────────────────────────────────────────────────

static esp_err_t handle_gen_keys(httpd_req_t* req) {
    // Refuse to silently overwrite an existing key: regenerating un-pairs the device
    // from the vehicle (the old whitelisted key stops working) and breaks charging
    // until a physical re-pair. Require explicit ?force=1 to replace a present key.
    if (g_vehicle->has_key() && !strstr(req->uri, "force=1")) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "result", false);
        cJSON_AddStringToObject(root, "reason",
            "a key already exists — regenerating un-pairs the vehicle; "
            "call /gen_keys?force=1 to replace it");
        return send_json(req, 409, root);
    }

    bool ok = g_vehicle->generate_key();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "result", ok);
    cJSON_AddStringToObject(root, "reason", ok ? "key generated — use /send_key to pair with vehicle"
                                               : "key generation failed");
    return send_json(req, 200, root);
}

// ─── POST /send_key ───────────────────────────────────────────────────────────

static esp_err_t handle_send_key(httpd_req_t* req) {
    // This firmware only enrolls a Charging Manager key (charging + wake), never an
    // owner key — its sole purpose is the evcc BLE integration. Reject an explicit
    // owner request rather than silently enrolling a different role than asked for.
    if (strstr(req->uri, "role=owner")) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "result", false);
        cJSON_AddStringToObject(root, "role",   "owner");
        cJSON_AddStringToObject(root, "reason",
            "owner role disabled — this device only enrolls Charging Manager keys");
        return send_json(req, 403, root);
    }

    bool ok = g_vehicle->pair();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "result", ok);
    cJSON_AddStringToObject(root, "role",   "charging_manager");
    cJSON_AddStringToObject(root, "reason",
        ok ? "key sent — confirm the pairing request on the car's screen"
           : "failed to send key (vehicle not reachable or timed out)");
    return send_json(req, 200, root);
}

// ─── GET /api/proxy/1/version ─────────────────────────────────────────────────

static esp_err_t handle_version(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", fw_version());
    cJSON_AddStringToObject(root, "platform", "ESP32-S3");
    return send_json(req, 200, root);
}

// ─── POST /scan — start a time-limited BLE discovery scan ─────────────────────

static esp_err_t handle_scan(httpd_req_t* req) {
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

static esp_err_t handle_status(httpd_req_t* req) {
    char ip[16];
    current_ip(ip, sizeof(ip));

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "vin",           g_vehicle->vin().c_str());
    cJSON_AddStringToObject(root, "ip",            ip);
    cJSON_AddStringToObject(root, "version",       esp_app_get_description()->version);
    cJSON_AddBoolToObject(root,   "key_present",   g_vehicle->has_key());
    cJSON_AddStringToObject(root, "key_fingerprint", g_vehicle->key_fingerprint().c_str());
    // Key creation date (epoch seconds), shown under the fingerprint in the UI.
    // Only emit a plausible wall-clock value (post-2020); a near-zero stamp means
    // the clock hadn't synced when the key was generated, so report it as unknown.
    time_t key_created = g_vehicle->key_created_at();
    if (key_created > 1600000000) cJSON_AddNumberToObject(root, "key_created", (double)key_created);
    cJSON_AddBoolToObject(root,   "paired",        g_vehicle->has_session());

    // WiFi: SSID + live signal strength (dBm) of the station link.
    cJSON* wifi = cJSON_CreateObject();
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddStringToObject(wifi, "ssid", (const char*)ap.ssid);
        cJSON_AddNumberToObject(wifi, "rssi", ap.rssi);
    }
    cJSON_AddItemToObject(root, "wifi", wifi);

    // BLE: when connected report the live signal strength; otherwise list nearby
    // Teslas seen while scanning, with their RSSI.
    cJSON* ble = cJSON_CreateObject();
    bool connected = g_vehicle->ble_connected();
    cJSON_AddBoolToObject(ble, "connected", connected);
    cJSON_AddBoolToObject(ble, "scanning",  g_vehicle->ble_scanning());
    if (connected) {
        int8_t rssi = 0;
        if (g_vehicle->ble_rssi(rssi)) cJSON_AddNumberToObject(ble, "rssi", rssi);
        cJSON_AddStringToObject(ble, "addr", g_vehicle->ble_peer().c_str());

        // Use cached data for the UI status to keep it responsive (non-blocking)
        ChargeStateResult cs = g_vehicle->get_cached_charge();
        if (cs.valid) {
            cJSON* veh = cJSON_CreateObject();
            cJSON_AddNumberToObject(veh, "soc", cs.battery_level);
            cJSON_AddStringToObject(veh, "status", cs.charging_state.c_str());
            cJSON_AddItemToObject(root, "vehicle", veh);
        }
    } else {
        cJSON* devices = cJSON_CreateArray();
        for (const auto& d : g_vehicle->ble_nearby()) {
            cJSON* dev = cJSON_CreateObject();
            cJSON_AddStringToObject(dev, "addr", d.addr.c_str());
            cJSON_AddStringToObject(dev, "name", d.name.c_str());
            cJSON_AddNumberToObject(dev, "rssi", d.rssi);
            cJSON_AddItemToArray(devices, dev);
        }
        cJSON_AddItemToObject(ble, "devices", devices);
    }
    cJSON_AddItemToObject(root, "ble", ble);
    return send_json(req, 200, root);
}

// ─── GET /diag — in-memory diagnostic log (for on-demand analysis) ────────────

static esp_err_t handle_diag(httpd_req_t* req) {
    if (strstr(req->uri, "clear=1"))        diag_log_clear();
    if (strstr(req->uri, "verbose=1"))      diag_set_verbose(true);
    else if (strstr(req->uri, "verbose=0")) diag_set_verbose(false);

    std::string body = diag_log_dump();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Diag-Verbose", diag_verbose() ? "1" : "0");
    return httpd_resp_send(req, body.c_str(), body.size());
}

// ─── POST /set_vin — persist VIN, then reboot ─────────────────────────────────

static esp_err_t handle_set_vin(httpd_req_t* req) {
    char* body = read_body(req);
    cJSON* json = body ? cJSON_Parse(body) : nullptr;
    free(body);

    std::string vin;
    if (json) {
        cJSON* j = cJSON_GetObjectItemCaseSensitive(json, "vin");
        if (cJSON_IsString(j) && j->valuestring) vin = j->valuestring;
    }
    cJSON_Delete(json);

    if (vin.size() != 17) {
        return send_json(req, 400, make_response(false, "set_vin", vin.c_str(),
                                                 "VIN must be 17 characters"));
    }
    bool ok = g_vehicle->save_config_vin(vin);
    esp_err_t r = send_json(req, ok ? 200 : 500,
        make_response(ok, "set_vin", vin.c_str(),
                      ok ? "VIN saved — rebooting" : "failed to save VIN"));
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    return r;
}

// ─── GET / — minimal web UI ────────────────────────────────────────────────────

static const char INDEX_HTML[] =
"<!doctype html><html lang=en><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>tesla-key-esp32</title>"
"<meta name=theme-color content=#e82127>"
"<style>"
":root{--bg:#f6f7f9;--card:#fff;--fg:#1a1d21;--muted:#5b626b;--border:#e6e8ec;--accent:#e82127;--code-bg:#f0f1f3;--ok:#16a34a;--warn:#d97706;--radius:16px;--shadow:0 1px 2px rgba(0,0,0,.04),0 8px 24px rgba(0,0,0,.05)}"
"@media(prefers-color-scheme:dark){:root{--bg:#0f1115;--card:#171a20;--fg:#f2f3f5;--muted:#9aa1ab;--border:#262a31;--code-bg:#22262e;--ok:#34d399;--warn:#fbbf24;--shadow:0 1px 2px rgba(0,0,0,.4),0 8px 24px rgba(0,0,0,.3)}}"
"*{box-sizing:border-box}body{font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--fg);line-height:1.55;margin:0;padding:1.25rem 1.1rem}"
"main{max-width:34rem;margin:0 auto}"
"header{display:flex;align-items:center;gap:.7rem;margin-bottom:.25rem}"
".logo{width:2.4rem;height:2.4rem;border-radius:.7rem;background:var(--accent);color:#fff;display:grid;place-items:center;font-size:1.3rem;box-shadow:var(--shadow)}"
".ttl{font-size:1.15rem;font-weight:650;letter-spacing:-.01em}.meta{font-size:.78rem;color:var(--muted)}"
".pill{margin-left:auto;font-size:.75rem;padding:.25rem .65rem;border-radius:999px;background:var(--border);color:var(--muted)}.pill.good{background:color-mix(in srgb,var(--ok) 16%,transparent);color:var(--ok)}"
".card{background:var(--card);border:1px solid var(--border);border-radius:var(--radius);box-shadow:var(--shadow);padding:1.25rem;margin-top:1.25rem}"
".card h2{font-size:.78rem;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin:0 0 1.1rem}"
"ol.steps{list-style:none;counter-reset:s;margin:0;padding:0}"
"ol.steps li{counter-increment:s;position:relative;padding:0 0 1.4rem 2.6rem}ol.steps li:last-child{padding-bottom:0}"
"ol.steps li::before{content:counter(s);position:absolute;left:0;top:-.1rem;width:1.8rem;height:1.8rem;border-radius:50%;display:grid;place-items:center;background:color-mix(in srgb,var(--accent) 14%,transparent);color:var(--accent);font-weight:700;font-size:.9rem}"
"ol.steps li:not(:last-child)::after{content:'';position:absolute;left:.875rem;top:1.9rem;bottom:.4rem;width:2px;background:var(--border)}"
/* When step 4 (Vehicle control) is hidden (not paired), step 3 is visually last — drop its dangling connector. */
"ol.steps.s3end li:nth-child(3)::after{display:none}"
".h{font-size:1rem;font-weight:600;display:flex;align-items:center;gap:.5rem}.sub{color:var(--muted);font-size:.85rem;margin:.1rem 0 .55rem}"
".mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:.95rem}"
"code{background:var(--code-bg);padding:.12rem .4rem;border-radius:6px;font-size:.92rem;letter-spacing:.5px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}"
".tag{font-size:.72rem;padding:.15rem .55rem;border-radius:999px;background:var(--border);color:var(--muted)}.tag.good{background:color-mix(in srgb,var(--ok) 16%,transparent);color:var(--ok)}"
".clk{cursor:pointer;border-bottom:1px dashed var(--muted)}.clk:hover{color:var(--accent);border-color:var(--accent)}"
".dev{display:flex;align-items:center;gap:.5rem;width:100%;margin:.2rem 0}.dbm{margin-left:auto;font-weight:600;font-size:.85rem}.muted{color:var(--muted)}"
".bars{display:inline-flex;align-items:flex-end;gap:2px;height:16px}.b{width:4px;background:var(--border);border-radius:1px}.b.g{background:var(--ok)}.b.a{background:var(--warn)}.b.b{background:var(--accent)}"
"button{font:inherit;font-weight:600;font-size:.95rem;padding:.7rem 1.1rem;border:1px solid var(--border);border-radius:999px;background:var(--card);color:var(--fg);cursor:pointer}button:active{transform:translateY(1px)}button:disabled{opacity:.55}"
".primary{background:var(--accent);color:#fff;border:0;width:100%;padding:.85rem;font-size:1rem;box-shadow:var(--shadow)}"
".scan{padding:.5rem 1.1rem;font-size:.85rem}"
".grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:.5rem;margin-top:.6rem}.grid button{padding:.6rem .3rem;font-size:.85rem}"
"#bleslot{margin-top:.6rem}"
"#log{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:.82rem;color:var(--muted);white-space:pre-wrap;word-break:break-word;margin:0;max-height:55vh;overflow:auto}"
"</style>"
"<main>"
"<header><div class=logo>&#9889;</div><div style='flex:1'><div class=ttl>tesla-key-esp32</div><div class=meta id=meta>&#8230;</div></div><span class=pill id=net>&#8230;</span></header>"
"<section class=card><h2>Setup &amp; control</h2><ol class=steps id=flow>"
"<li><div class=h>Vehicle <span class=tag id=pair>&#8230;</span></div><div class=sub>Tap the VIN to change it</div><div id=vinslot></div></li>"
"<li><div class=h>Security key</div><div class=sub>Tap the fingerprint to regenerate the key</div><div id=keyslot></div></li>"
"<li><div class=h>Connection <span class=tag id=findtag></span></div><div class=sub id=pairmsg></div><div id=bleslot></div></li>"
"<li id=step4 style='display:none'><div class=h>Vehicle control</div><div id=ctrlslot style='margin-bottom:.8rem;margin-top:.4rem'></div><div class=grid id=acts><button onclick=cmd('wake_up')>Wake</button><button onclick=cmd('charge_start')>Charge</button><button onclick=cmd('charge_stop')>Stop</button></div></li>"
"</ol></section>"
"<section class=card><h2>LOGS</h2>"
"<div id=log>Ready.</div></section>"
"</main>"
"<script>"
"let V='';"
"function log(t){document.getElementById('log').textContent=t}"
"function esc(s){return (s||'').replace(/[<&>]/g,c=>({'<':'&lt;','&':'&amp;','>':'&gt;'}[c]))}"
"function bleBody(e){"
"if(e.connected){let r=e.rssi;"
"return `<div class=dev><span class=mono>${esc(e.addr||'connected')} ${r!=null?r+'dBm':''}</span></div>`}"
"let d=e.devices||[];"
"if(e.scanning&&!d.length)return `<div class=sub>scanning\\u2026</div>`;"
"if(!d.length)return `<div class=sub>no Teslas found yet</div>`;"
"return d.map(x=>`<div class=dev><span class=mono>${esc(x.addr)} ${x.rssi}dBm ${x.name?`\\u00b7 ${esc(x.name)}`:''}</span></div>`).join('')}"
"function ctrlBody(v){"
"if(!v) return `<div class=sub>vehicle asleep</div>`;"
"return `<span class=mono style='font-size:.9rem'>SOC ${Math.round(v.soc)}% \\u00b7 ${esc(v.status)}</span>`;"
"}"
"async function st(){let j;try{j=await (await fetch('/status')).json()}catch(e){return}V=j.vin;"
"let w=j.wifi||{};let parts=[];if(w.ssid!=null)parts.push(w.ssid+' '+w.rssi+' dBm');if(j.ip)parts.push(j.ip);if(j.version)parts.push(j.version);"
"document.getElementById('meta').textContent=parts.join(' \\u00b7 ');"
"let e=j.ble||{};let net=document.getElementById('net');net.textContent=e.connected?'linked':'online';net.className='pill '+(e.connected?'good':'');"
"let pg=document.getElementById('pair');pg.textContent=j.paired?'paired':'not paired';pg.className='tag'+(j.paired?' good':'');"
"document.getElementById('vinslot').innerHTML=`<span class='mono clk' title='Tap to change VIN' onclick=ev()>${esc(j.vin)}</span>`;"
"let fp=j.key_fingerprint||'\\u2014';"
"let kc=(j.key_present&&j.key_created)?`<div class=sub style='margin-top:.35rem'>Created ${new Date(j.key_created*1000).toLocaleString()}</div>`:'';"
"document.getElementById('keyslot').innerHTML=(j.key_present?`<code class=clk title='Tap to regenerate key' onclick=rk()>${fp}</code>`:`<code>${fp}</code>`)+kc;"
"let ft=document.getElementById('findtag'),pm=document.getElementById('pairmsg');"
"if(j.paired){ft.textContent='paired';ft.className='tag good';pm.textContent='Vehicle is linked and authorized.'}"
"else if(e.connected){ft.textContent='connecting\\u2026';ft.className='tag';pm.textContent='Confirm the pairing request on your Tesla\\u2019s screen.'}"
"else{ft.textContent='searching\\u2026';ft.className='tag';pm.textContent='Bring the device near your Tesla.'}"
"document.getElementById('bleslot').innerHTML=bleBody(e);"
"let s4=document.getElementById('step4');"
"document.getElementById('flow').classList.toggle('s3end',!j.paired);"
"if(j.paired){s4.style.display='list-item';document.getElementById('ctrlslot').innerHTML=ctrlBody(j.vehicle);}"
"else{s4.style.display='none';}}"
"async function ev(){let v=prompt('New VIN (17 characters):',V&&V!='UNKNOWN'?V:'');if(v===null)return;v=v.trim();"
"if(v.length!=17){log('VIN must be 17 characters');return}"
"log('Saving VIN & rebooting...');"
"try{let r=await fetch('/set_vin',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({vin:v})});"
"let j=await r.json();log((j.response?j.response.reason:j.reason))}catch(e){log('Saved \\u2014 device rebooting')}}"
"async function rk(){if(!confirm('Regenerate key?\\n\\nThe existing key will be DELETED and the car will be UN-PAIRED \\u2014 you must pair again at the vehicle.'))return;"
"log('Regenerating key...');let r=await fetch('/gen_keys?force=1',{method:'POST'});log((await r.json()).reason);st()}"
"async function cmd(c){log(c+'...');let r=await fetch('/api/1/vehicles/'+V+'/command/'+c,{method:'POST'});"
"let j=await r.json();log(c+': '+(j.response?j.response.reason:JSON.stringify(j)));st()}"
"st();setInterval(st,4000);"
"</script></html>";

static esp_err_t handle_index(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// ─── Wildcard handler dispatching ─────────────────────────────────────────────

// Single catch-all handler registered for /*
static esp_err_t handle_all(httpd_req_t* req) {
    const char* uri = req->uri;
    const char* method = (req->method == HTTP_GET ? "GET" : (req->method == HTTP_POST ? "POST" : "OTHER"));
    ESP_LOGI(TAG, "REQ: %s %s", method, uri);

    // Log all headers to see what evcc is sending
    char header_val[128];
    if (httpd_req_get_hdr_value_str(req, "User-Agent", header_val, sizeof(header_val)) == ESP_OK) {
        ESP_LOGI(TAG, "  User-Agent: %s", header_val);
    }
    if (httpd_req_get_hdr_value_str(req, "Accept", header_val, sizeof(header_val)) == ESP_OK) {
        ESP_LOGI(TAG, "  Accept: %s", header_val);
    }

    if (req->method == HTTP_POST && strstr(uri, "/command/")) {
        return handle_command(req);
    }
    if (req->method == HTTP_GET && strstr(uri, "/vehicle_data")) {
        return handle_vehicle_data(req);
    }
    if (req->method == HTTP_GET && strstr(uri, "/body_controller_state")) {
        return handle_body_controller(req);
    }
    if (req->method == HTTP_POST && strstr(uri, "/gen_keys")) {
        return handle_gen_keys(req);
    }
    if (req->method == HTTP_POST && strstr(uri, "/send_key")) {
        return handle_send_key(req);
    }
    if (req->method == HTTP_POST && strstr(uri, "/set_vin")) {
        return handle_set_vin(req);
    }
    if (req->method == HTTP_POST && strstr(uri, "/scan")) {
        return handle_scan(req);
    }
    if (req->method == HTTP_GET && strstr(uri, "/version")) {
        return handle_version(req);
    }
    if (req->method == HTTP_GET && strstr(uri, "/status")) {
        return handle_status(req);
    }
    if (req->method == HTTP_GET && strstr(uri, "/diag")) {
        return handle_diag(req);
    }
    if (req->method == HTTP_GET && (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0)) {
        return handle_index(req);
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", "not found");
    return send_json(req, 404, root);
}

// ─── Start ────────────────────────────────────────────────────────────────────

bool http_server_start(VehicleController& vehicle) {
    g_vehicle = &vehicle;

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
