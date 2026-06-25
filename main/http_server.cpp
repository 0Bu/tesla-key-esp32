#include "http_server.hpp"
#include "diag_log.hpp"
#include "ota_update.hpp"
#include "mqtt_ha.hpp"
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
#include <cctype>
#include <string>
#include <sys/time.h>
#include <exception>

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

// Defined in main.cpp: true once SNTP has synced this boot. The browser /set_time
// fallback only applies the client clock while this is false (NTP is authoritative).
bool clock_synced_via_ntp();

// Defined in main.cpp: true only while the STA holds an IP. Gate esp_wifi_sta_get_ap_info()
// so it's never read during association churn (concurrent read of the half-built AP
// record faults — LoadProhibited/EXCVADDR=0x1).
bool wifi_is_connected();

// Largest POST body we accept, to bound the malloc in read_body() against a
// hostile/oversized Content-Length. All real requests here are tiny JSON objects.
static constexpr size_t MAX_BODY_LEN = 2048;

// ─── JSON helpers ─────────────────────────────────────────────────────────────

static esp_err_t send_json(httpd_req_t* req, int status, cJSON* root) {
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    // On a fragmented heap cJSON_PrintUnformatted returns NULL rather than throwing, so this
    // path bypasses the handle_all try/catch. Guard it: httpd_resp_sendstr(NULL) would
    // strlen(NULL) and crash the C httpd task → reboot (which re-opens the poll window and
    // defeats car sleep). Degrade to a 503 like handle_all's own OOM fallback.
    if (!body) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"result\":false,\"reason\":\"out of memory\"}");
    }
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
            if (cJSON_IsNumber(j))                        amps = (int)j->valuedouble;
            else if (cJSON_IsString(j) && j->valuestring) amps = atoi(j->valuestring);
        }
        ok = g_vehicle->set_charging_amps(amps);
    }
    else if (strcmp(cmd, "set_charge_limit")  == 0) {
        int pct = 80;
        if (json) {
            cJSON* j = cJSON_GetObjectItemCaseSensitive(json, "percent");
            if (cJSON_IsNumber(j))                        pct = (int)j->valuedouble;
            else if (cJSON_IsString(j) && j->valuestring) pct = atoi(j->valuestring);
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
    else if (strcmp(cmd, "set_scheduled_charging") == 0) {
        // Body: {"enable":bool, "start_minutes":int}  (minutes after local midnight)
        bool enable = false;
        int  start  = 0;
        if (json) {
            cJSON* je = cJSON_GetObjectItemCaseSensitive(json, "enable");
            if (je) enable = cJSON_IsTrue(je);
            cJSON* jm = cJSON_GetObjectItemCaseSensitive(json, "start_minutes");
            if (cJSON_IsNumber(jm))                         start = (int)jm->valuedouble;
            else if (cJSON_IsString(jm) && jm->valuestring) start = atoi(jm->valuestring);
        }
        ok = g_vehicle->set_scheduled_charging(enable, start);
    }
    else {
        cJSON_Delete(json);
        return send_json(req, 404, make_response(false, cmd, vin, "unknown command"));
    }

    cJSON_Delete(json);
    // On failure, distinguish "the car rejected it" (we got an error reply, e.g.
    // "complete") from "the car was unreachable" (no reply / timed out). The former
    // carries the real Tesla reason; only the latter is an in-range problem.
    std::string err = g_vehicle->last_command_error();
    const char* reason = ok ? "command executed successfully"
                            : (!err.empty() ? err.c_str() : "vehicle not reachable");
    return send_json(req, 200, make_response(ok, cmd, vin, reason));
}

// ─── GET /api/1/vehicles/{VIN}/vehicle_data ───────────────────────────────────

static esp_err_t handle_vehicle_data(httpd_req_t* req) {
    char vin[64] = {0};
    if (!parse_vin_only(req->uri, vin, sizeof(vin)))
        return send_json(req, 400, make_response(false, "vehicle_data", "?", "invalid URI"));

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
    char vin[64] = {0};
    if (!parse_vin_only(req->uri, vin, sizeof(vin)))
        return send_json(req, 400, make_response(false, "body_controller_state", "?", "invalid URI"));

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

// ─── OTA self-update ──────────────────────────────────────────────────────────

static const char* ota_state_str(OtaState s) {
    switch (s) {
        case OtaState::Checking:    return "checking";
        case OtaState::Downloading: return "downloading";
        case OtaState::Done:        return "done";
        case OtaState::Error:       return "error";
        default:                    return "idle";
    }
}

// Apply ?ms=<epoch> as the wall clock (the NTP fallback, see /set_time) when NTP
// hasn't synced. Lets a single /ota/check request both set the browser time and
// start the check — no extra blocking round-trip on the (serialized) HTTP server.
static void apply_browser_time_query_(httpd_req_t* req) {
    if (clock_synced_via_ntp()) return;
    char q[48];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return;
    char ms[24];
    if (httpd_query_key_value(q, "ms", ms, sizeof(ms)) != ESP_OK) return;
    double epoch_ms = atof(ms);
    if (epoch_ms < 1700000000000.0) return;
    long long sec = (long long)(epoch_ms / 1000.0);
    struct timeval tv = {};
    tv.tv_sec = (time_t)sec;
    settimeofday(&tv, nullptr);
    g_vehicle->save_config_time(sec);
    ESP_LOGI(TAG, "clock set from browser (ota query): %lld", sec);
}

// GET /ota/check[?ms=<epoch>] — start a background version check, return at once.
// The slow HTTPS manifest fetch runs in its own task (see ota_check_start) so it
// never ties up the HTTP server; the UI polls /ota/status for the result.
static esp_err_t handle_ota_check(httpd_req_t* req) {
    apply_browser_time_query_(req);
    bool started = ota_check_start();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "started", started);
    if (!started)
        cJSON_AddStringToObject(root, "reason", "a check or update is already in progress");
    return send_json(req, started ? 200 : 409, root);
}

// POST /ota/update — start the background download+install, return immediately.
static esp_err_t handle_ota_update(httpd_req_t* req) {
    bool started = ota_start();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root,   "result", started);
    cJSON_AddStringToObject(root, "reason",
        started ? "update started — the device will reboot when done"
                : "an update is already in progress");
    return send_json(req, started ? 200 : 409, root);
}

// GET /ota/status — poll download progress.
static esp_err_t handle_ota_status(httpd_req_t* req) {
    OtaStatus s = ota_get_status();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state",            ota_state_str(s.state));
    cJSON_AddNumberToObject(root, "progress",         s.progress);
    cJSON_AddStringToObject(root, "message",          s.message.c_str());
    cJSON_AddStringToObject(root, "available",        s.available.c_str());
    cJSON_AddBoolToObject(root,   "update_available", s.update_available);
    cJSON_AddStringToObject(root, "current",          s.current.c_str());
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

// Whether the car is "live" (awake) vs merely sleeping vs unreachable is decided centrally
// in VehicleController::link_state(), shared with the MQTT bridge so the two never drift.

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
    // Pairing date (epoch seconds), shown under "Paired" in the UI. Same post-2020
    // plausibility guard as key_created.
    time_t paired_at = g_vehicle->paired_at();
    if (paired_at > 1600000000) cJSON_AddNumberToObject(root, "paired_at", (double)paired_at);
    // True when the previous pairing was lost (key removed on the car side) and a
    // re-pair is pending — lets the UI explain why it's prompting to pair again.
    cJSON_AddBoolToObject(root,   "reauth",        g_vehicle->reauth_required());

    // WiFi: SSID + live signal strength (dBm) of the station link.
    cJSON* wifi = cJSON_CreateObject();
    wifi_ap_record_t ap{};
    if (wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddStringToObject(wifi, "ssid", (const char*)ap.ssid);
        cJSON_AddNumberToObject(wifi, "rssi", ap.rssi);
        // Highest 802.11 generation the AP advertises → friendly Wi-Fi name. The
        // ESP32-S3 radio itself tops out at 802.11n, but the flags reflect the AP's
        // capability, so a Wi-Fi 6 router still reads "Wi-Fi 6".
        const char* std_ = ap.phy_11ax ? "Wi-Fi 6"
                         : ap.phy_11ac ? "Wi-Fi 5"
                         : ap.phy_11n  ? "Wi-Fi 4"
                         : ap.phy_11g  ? "802.11g"
                         : ap.phy_11b  ? "802.11b" : nullptr;
        if (std_) cJSON_AddStringToObject(wifi, "std", std_);
    }
    cJSON_AddItemToObject(root, "wifi", wifi);

    // MQTT (Home Assistant bridge): whether a broker is configured, whether the
    // session is live, and the host:port. Drives the web-UI Connection block.
    cJSON* mqtt = cJSON_CreateObject();
    cJSON_AddBoolToObject(mqtt, "configured", mqtt_ha_configured());
    cJSON_AddBoolToObject(mqtt, "connected",  mqtt_ha_connected());
    std::string broker = mqtt_ha_broker();
    if (!broker.empty()) cJSON_AddStringToObject(mqtt, "broker", broker.c_str());
    cJSON_AddItemToObject(root, "mqtt", mqtt);

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

        // Read-only telemetry caches, refreshed by the rotating background poll. Each
        // numeric field is emitted only when the car actually reported it (presence
        // flag) so the UI can show "—" for anything not yet seen. Grouped under "tele".
        cJSON* tele = cJSON_CreateObject();
        ClimateStateResult cl = g_vehicle->get_cached_climate();
        if (cl.valid) {
            cJSON* o = cJSON_CreateObject();
            if (cl.has_inside)   cJSON_AddNumberToObject(o, "inside",   cl.inside_temp);
            if (cl.has_outside)  cJSON_AddNumberToObject(o, "outside",  cl.outside_temp);
            if (cl.has_setpoint) cJSON_AddNumberToObject(o, "setpoint", cl.driver_setpoint);
            cJSON_AddBoolToObject(o, "on",            cl.is_climate_on);
            cJSON_AddBoolToObject(o, "preconditioning", cl.is_preconditioning);
            // Cabin Overheat Protection (separate subsystem; emitted only when the
            // car actually reported each field, so an absent one means "not sent").
            if (cl.has_cop)         cJSON_AddStringToObject(o, "cop",         cl.cop.c_str());
            if (cl.has_cop_cooling) cJSON_AddBoolToObject(o,   "cop_cooling", cl.cop_cooling);
            if (cl.has_cop_temp)    cJSON_AddStringToObject(o, "cop_temp",    cl.cop_temp.c_str());
            if (cl.has_cop_reason)  cJSON_AddStringToObject(o, "cop_reason",  cl.cop_reason.c_str());
            // Defrost (front/rear defroster + Max-defrost mode), emitted only when reported.
            if (cl.has_front_defrost) cJSON_AddBoolToObject(o,   "front_defrost", cl.front_defrost);
            if (cl.has_rear_defrost)  cJSON_AddBoolToObject(o,   "rear_defrost",  cl.rear_defrost);
            if (cl.has_defrost_mode)  cJSON_AddStringToObject(o, "defrost_mode",  cl.defrost_mode.c_str());
            cJSON_AddItemToObject(tele, "climate", o);
        }
        DriveStateResult dr = g_vehicle->get_cached_drive();
        if (dr.valid) {
            cJSON* o = cJSON_CreateObject();
            if (!dr.shift_state.empty()) cJSON_AddStringToObject(o, "shift", dr.shift_state.c_str());
            if (dr.has_odometer)         cJSON_AddNumberToObject(o, "odometer_km", dr.odometer_km);
            cJSON_AddItemToObject(tele, "drive", o);
        }
        TirePressureResult tp = g_vehicle->get_cached_tires();
        if (tp.valid) {
            cJSON* o = cJSON_CreateObject();
            if (tp.has_fl) cJSON_AddNumberToObject(o, "fl", tp.fl);
            if (tp.has_fr) cJSON_AddNumberToObject(o, "fr", tp.fr);
            if (tp.has_rl) cJSON_AddNumberToObject(o, "rl", tp.rl);
            if (tp.has_rr) cJSON_AddNumberToObject(o, "rr", tp.rr);
            cJSON_AddBoolToObject(o, "warn", tp.warn);
            cJSON_AddItemToObject(tele, "tires", o);
        }
        ClosuresStateResult cz = g_vehicle->get_cached_closures();
        if (cz.valid) {
            cJSON* o = cJSON_CreateObject();
            if (cz.has_locked)       cJSON_AddBoolToObject(o, "locked", cz.locked);
            cJSON_AddBoolToObject(o, "door",   cz.any_door_open);
            cJSON_AddBoolToObject(o, "frunk",  cz.frunk_open);
            cJSON_AddBoolToObject(o, "trunk",  cz.trunk_open);
            cJSON_AddBoolToObject(o, "window", cz.any_window_open);
            if (cz.has_user_present) cJSON_AddBoolToObject(o, "user", cz.user_present);
            cJSON_AddItemToObject(tele, "closures", o);
        }
        cJSON_AddItemToObject(root, "tele", tele);
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

    // Overall connectivity, the single source of truth the UI keys the hero off (and the
    // same enum the MQTT bridge publishes) — so "asleep" and "unreachable" can never be
    // confused. awake ⇒ live SOC card; asleep ⇒ "Vehicle asleep" card; unreachable ⇒ the
    // car drove off / is out of range and the UI hides the hero entirely; unknown ⇒ nothing
    // heard yet. Decoupled from the momentary BLE link (dropped between polls by design).
    const char* link;
    switch (g_vehicle->link_state()) {
        case VehicleController::LinkState::Awake:       link = "awake";       break;
        case VehicleController::LinkState::Asleep:      link = "asleep";      break;
        case VehicleController::LinkState::Unreachable: link = "unreachable"; break;
        default:                                        link = "unknown";     break;
    }
    cJSON_AddStringToObject(root, "link", link);

    // Live "vehicle" object — drives the UI's awake/SOC view. Emitted only when the car is
    // AWAKE (fresh infotainment telemetry per link_state()), independent of the momentary
    // BLE link: the link is dropped between polls so the car can sleep, but data inside the
    // freshness window is still live. When not awake the UI falls through to the asleep card
    // (reachable) or hides the hero (unreachable) using `link` above.
    if (g_vehicle->link_state() == VehicleController::LinkState::Awake) {
        ChargeStateResult cs = g_vehicle->get_cached_charge();
        if (cs.valid) {
            cJSON* veh = cJSON_CreateObject();
            cJSON_AddNumberToObject(veh, "soc",    cs.battery_level);
            cJSON_AddStringToObject(veh, "status", cs.charging_state.c_str());
            // Charge target — lets the UI mark "charge complete" (SOC >= limit) and gate the
            // start-charge tap, which the car would only reject as "complete" anyway. Emitted
            // only when the car reported it (proto3 optional); a missing limit means the UI
            // keeps the button live rather than blocking on unknown data.
            if (cs.has_charge_limit_soc)
                cJSON_AddNumberToObject(veh, "charge_limit", cs.charge_limit_soc);
            // Charging detail for the web UI: live power (kW) and current (A). Emitted only
            // when the car actually reported them (proto3 optional) so the UI omits the chip
            // rather than rendering a phantom 0. Power is a whole number (no decimals).
            if (cs.has_charger_power)
                cJSON_AddNumberToObject(veh, "power",  (int)(cs.charger_power + 0.5f));
            if (cs.has_charging_amps)
                cJSON_AddNumberToObject(veh, "amps",   cs.charging_amps);
            // Actual AC draw (current × voltage) — distinct from charger_power (DC to the
            // battery, 0 when "Complete"). Lets us see what the car pulls from the wall while
            // e.g. cabin-overheat-protection runs with a full battery. Emitted only if reported.
            if (cs.has_actual_current)
                cJSON_AddNumberToObject(veh, "actual_amps", cs.charger_actual_current);
            if (cs.has_voltage)
                cJSON_AddNumberToObject(veh, "volts", cs.charger_voltage);
            cJSON_AddItemToObject(root, "vehicle", veh);
        }
    }

    // Last-known vehicle snapshot for the "Vehicle asleep" card. The charge cache is
    // retained in RAM across BLE disconnects, so the UI can show the last battery level
    // (a sleeping car barely drains, so it stays a good estimate) and — via last_seen_s —
    // how long the car has been asleep, all without waking it. Emitted regardless of the
    // BLE link state; when awake the UI uses the live "vehicle" object instead.
    {
        ChargeStateResult last = g_vehicle->get_cached_charge();
        if (last.valid) {
            cJSON* lo = cJSON_CreateObject();
            cJSON_AddNumberToObject(lo, "soc",    last.battery_level);
            cJSON_AddStringToObject(lo, "status", last.charging_state.c_str());
            cJSON_AddItemToObject(root, "last", lo);
        }
        uint32_t ago = 0;
        if (g_vehicle->seconds_since_contact(ago))
            cJSON_AddNumberToObject(root, "last_seen_s", (double)ago);
    }
    return send_json(req, 200, root);
}

// ─── GET /diag — in-memory diagnostic log (for on-demand analysis) ────────────

static esp_err_t handle_diag(httpd_req_t* req) {
    if (strstr(req->uri, "clear=1"))        diag_log_clear();
    if (strstr(req->uri, "verbose=1"))      diag_set_verbose(true);
    else if (strstr(req->uri, "verbose=0")) diag_set_verbose(false);

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

// ─── POST /set_time — set the wall clock from the browser (NTP fallback) ───────
// TLS certificate validation (OTA) and the tesla-ble session-freshness checks need a
// real UTC clock. NTP is the primary source; this endpoint is the fallback for
// networks that block NTP. The web UI posts the browser's clock ({"ms": <epoch ms>})
// on load and before an OTA check, but we only apply it while SNTP has not synced —
// otherwise NTP (which is more trustworthy than a possibly-skewed browser) wins. The
// applied fallback time is persisted so a later offline reboot starts plausibly.
static esp_err_t handle_set_time(httpd_req_t* req) {
    // NTP already synced → it's authoritative; drain the body and accept as a no-op.
    if (clock_synced_via_ntp()) {
        free(read_body(req));
        return send_json(req, 200, make_response(true, "set_time", "", "clock set via NTP"));
    }

    char* body = read_body(req);
    cJSON* json = body ? cJSON_Parse(body) : nullptr;
    free(body);

    double epoch_ms = 0;
    if (json) {
        cJSON* j = cJSON_GetObjectItemCaseSensitive(json, "ms");
        if (cJSON_IsNumber(j)) epoch_ms = j->valuedouble;
    }
    cJSON_Delete(json);

    // Floor at ~2023-11 (1.7e12 ms): reject a missing/implausible browser clock so
    // we never set the device clock backwards into the cert-invalid range.
    if (epoch_ms < 1700000000000.0) {
        return send_json(req, 400, make_response(false, "set_time", "",
                                                 "implausible timestamp"));
    }

    long long sec = (long long)(epoch_ms / 1000.0);
    struct timeval tv = {};
    tv.tv_sec  = (time_t)sec;
    tv.tv_usec = (suseconds_t)((long long)epoch_ms % 1000) * 1000;
    settimeofday(&tv, nullptr);
    g_vehicle->save_config_time(sec);
    ESP_LOGI(TAG, "clock set from browser: %lld", sec);

    return send_json(req, 200, make_response(true, "set_time", "", "clock set"));
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

    // Normalise to the canonical stored form (trim, uppercase) before validating or
    // comparing, so "unchanged" is judged on the stored representation, not on casing.
    size_t s = vin.find_first_not_of(" \t\r\n");
    size_t e = vin.find_last_not_of(" \t\r\n");
    vin = (s == std::string::npos) ? std::string{} : vin.substr(s, e - s + 1);
    for (char& c : vin) c = (char)std::toupper((unsigned char)c);

    // Unchanged → nothing to apply: skip the NVS write and the reboot entirely.
    if (vin == g_vehicle->vin()) {
        return send_json(req, 200, make_response(true, "set_vin", vin.c_str(),
                                                 "VIN unchanged — no reboot"));
    }

    // Validate plausibility before applying a *changed* value. Shared with the BLE pairing
    // gate (VehicleController::vin_is_plausible) so the web check and the pairing check agree.
    if (!VehicleController::vin_is_plausible(vin)) {
        return send_json(req, 400, make_response(false, "set_vin", vin.c_str(),
                                                 "VIN must be 17 valid characters"));
    }

    // A changed VIN points the device at a *different* vehicle, so the current key's
    // pairing, session, cached data and discovered BLE MAC all belong to the old car.
    // Wipe them (regenerate the key + clear the session/cache/MAC) before rebooting so
    // the device pairs cleanly with the new vehicle and shows no stale data.
    bool ok = g_vehicle->save_config_vin(vin);
    if (ok) g_vehicle->reset_for_new_vehicle();
    esp_err_t r = send_json(req, ok ? 200 : 500,
        make_response(ok, "set_vin", vin.c_str(),
                      ok ? "VIN saved — rebooting" : "failed to save VIN"));
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    return r;
}

// ─── POST /set_mqtt — persist the MQTT broker, then reboot ────────────────────
// Body: {"broker":"host:port"} (a full "mqtt://host:port" URI is also accepted; an
// empty string disables MQTT). Stored in NVS ("mqtt_uri") and applied on reboot —
// the bridge reads it once at start, so a reboot is the clean way to (re)init it.

// Plausibility check for a broker value: empty is fine (disables MQTT); otherwise it
// must be host:port — a non-empty host, a ':' separator, and a numeric port in 1..65535.
// An optional "scheme://" prefix is tolerated (mqtt_ha prepends mqtt:// for bare hosts).
static bool mqtt_broker_is_plausible(const std::string& broker) {
    if (broker.empty()) return true;                  // empty = disable
    if (broker.size() > 120) return false;
    if (broker.find_first_of(" \t\r\n") != std::string::npos) return false;

    std::string authority = broker;
    size_t scheme = authority.find("://");
    if (scheme != std::string::npos) authority = authority.substr(scheme + 3);

    size_t colon = authority.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;   // need host:port
    std::string host = authority.substr(0, colon);
    std::string port = authority.substr(colon + 1);
    if (host.empty() || port.empty() || port.size() > 5) return false;
    for (char c : port) if (c < '0' || c > '9') return false;
    int p = atoi(port.c_str());
    return p >= 1 && p <= 65535;
}

static esp_err_t handle_set_mqtt(httpd_req_t* req) {
    char* body = read_body(req);
    cJSON* json = body ? cJSON_Parse(body) : nullptr;
    free(body);

    std::string broker;
    if (json) {
        cJSON* j = cJSON_GetObjectItemCaseSensitive(json, "broker");
        if (cJSON_IsString(j) && j->valuestring) broker = j->valuestring;
    }
    cJSON_Delete(json);

    // Trim surrounding whitespace.
    size_t s = broker.find_first_not_of(" \t\r\n");
    size_t e = broker.find_last_not_of(" \t\r\n");
    broker = (s == std::string::npos) ? std::string{} : broker.substr(s, e - s + 1);

    // Unchanged → nothing to apply: skip the NVS write and the reboot entirely. The
    // stored value is the bare broker string as last saved (mqtt_ha adds the scheme).
    if (broker == g_vehicle->load_config_str("mqtt_uri")) {
        return send_json(req, 200, make_response(true, "set_mqtt", "",
            broker.empty() ? "MQTT already disabled — no reboot"
                           : "MQTT broker unchanged — no reboot"));
    }

    // Validate plausibility before applying a *changed* value.
    if (!mqtt_broker_is_plausible(broker)) {
        return send_json(req, 400, make_response(false, "set_mqtt", "",
                                                 "invalid broker (use host:port)"));
    }

    bool ok = g_vehicle->save_config_str("mqtt_uri", broker);
    esp_err_t r = send_json(req, ok ? 200 : 500,
        make_response(ok, "set_mqtt", "",
                      ok ? (broker.empty() ? "MQTT disabled — rebooting"
                                           : "MQTT broker saved — rebooting")
                         : "failed to save MQTT broker"));
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    return r;
}

// ─── GET / — web UI (embedded from main/www/index.html) ─────────────────────────

// The web UI is embedded pre-gzipped (see main/CMakeLists.txt) — ~13 KB vs 41 KB raw,
// the biggest first-paint win over a high-latency WiFi link. Browsers always accept
// gzip; the only consumer of "/" is a browser (evcc/curl hit /api and /status), so the
// encoding is sent unconditionally. Length is end-start (binary blob, not a C string).
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

static esp_err_t handle_index(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    // The UI is embedded in the firmware and changes with every flash/OTA. Without
    // this, browsers cache index.html and keep rendering the OLD layout (with live
    // /status data) after an update — so tell them never to cache the page.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    const size_t len = index_html_gz_end - index_html_gz_start;
    return httpd_resp_send(req, (const char*)index_html_gz_start, len);
}

// ─── Wildcard handler dispatching ─────────────────────────────────────────────

// Single catch-all handler registered for /*
static esp_err_t handle_all_dispatch(httpd_req_t* req) {
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

    // OTA routes first: "/ota/status" must not fall through to the generic
    // "/status" handler below (substring match).
    if (req->method == HTTP_GET && strstr(uri, "/ota/check")) {
        return handle_ota_check(req);
    }
    if (req->method == HTTP_POST && strstr(uri, "/ota/update")) {
        return handle_ota_update(req);
    }
    if (req->method == HTTP_GET && strstr(uri, "/ota/status")) {
        return handle_ota_status(req);
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
    if (req->method == HTTP_POST && strstr(uri, "/set_time")) {
        return handle_set_time(req);
    }
    if (req->method == HTTP_POST && strstr(uri, "/set_vin")) {
        return handle_set_vin(req);
    }
    if (req->method == HTTP_POST && strstr(uri, "/set_mqtt")) {
        return handle_set_mqtt(req);
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
