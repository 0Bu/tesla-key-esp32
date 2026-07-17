// Setup / pairing / persisted-config endpoints:
//   POST /gen_keys[?force=1]  (generate key — refuses overwrite w/o force)
//   POST /send_key            (pair with vehicle, Charging Manager only)
//   POST /set_time            (browser wall clock — NTP fallback)
//   POST /set_vin             (persist VIN + reboot)
//   POST /set_mqtt            (persist MQTT broker + reboot)
//   POST /set_syslog          (persist Syslog server + reboot)
// Dispatched from handle_all in http_server.cpp (inside its try/catch OOM guard).

#include "http_handlers.hpp"
#include "logic/syslog_policy.hpp"
#include <esp_log.h>
#include <esp_system.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdlib>
#include <cctype>
#include <string>

static const char* TAG = "http_server";

// ─── POST /gen_keys ───────────────────────────────────────────────────────────

esp_err_t handle_gen_keys(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    // Refuse to silently overwrite an existing key: regenerating un-pairs the device
    // from the vehicle (the old whitelisted key stops working) and breaks charging
    // until a physical re-pair. Require explicit ?force=1 to replace a present key.
    if (g_vehicle->has_key() && !query_param_is(req, "force", "1")) {
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

esp_err_t handle_send_key(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    // This firmware only enrolls a Charging Manager key (charging + wake), never an
    // owner key — its sole purpose is the evcc BLE integration. Reject an explicit
    // owner request rather than silently enrolling a different role than asked for.
    if (query_param_is(req, "role", "owner")) {
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

// ─── POST /set_time — set the wall clock from the browser (NTP fallback) ───────
// TLS certificate validation (OTA) and the human-readable key_created/paired_at
// timestamps need a real UTC clock. (tesla-ble signed-command freshness does NOT —
// expires_at is the vehicle's clock plus a monotonic delta.) NTP is the primary
// source; this endpoint is the fallback for networks that block NTP. The web UI posts the browser's clock ({"ms": <epoch ms>})
// on load and before an OTA check, but we only apply it while SNTP has not synced —
// otherwise NTP (which is more trustworthy than a possibly-skewed browser) wins. The
// applied fallback time is persisted so a later offline reboot starts plausibly.
esp_err_t handle_set_time(GuardedReq rq) {
    httpd_req_t* req = rq.req;
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

    // Reject a missing/implausible browser clock so we never push the device clock into the
    // cert-invalid range — floor ~2023-11, ceiling build year + 10 (see browser_time_plausible).
    if (!browser_time_plausible(epoch_ms)) {
        return send_json(req, 400, make_response(false, "set_time", "",
                                                 "implausible timestamp"));
    }

    long long sec = apply_browser_clock(epoch_ms);
    ESP_LOGI(TAG, "clock set from browser: %lld", sec);

    return send_json(req, 200, make_response(true, "set_time", "", "clock set"));
}

// ─── POST /set_vin — persist VIN, then reboot ─────────────────────────────────

esp_err_t handle_set_vin(GuardedReq rq) {
    httpd_req_t* req = rq.req;
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

esp_err_t handle_set_mqtt(GuardedReq rq) {
    httpd_req_t* req = rq.req;
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

// ─── POST /set_syslog — persist the Syslog server, then reboot ───────────────────
// Body: {"server":"host:port"} (a bare host defaults to port 514; an empty string
// disables Syslog). Stored in NVS ("syslog_uri") and applied on reboot — the
// forwarder resolves it once at start (syslog.cpp), same as /set_mqtt.

esp_err_t handle_set_syslog(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    char* body = read_body(req);
    cJSON* json = body ? cJSON_Parse(body) : nullptr;
    free(body);

    std::string server;
    if (json) {
        cJSON* j = cJSON_GetObjectItemCaseSensitive(json, "server");
        if (cJSON_IsString(j) && j->valuestring) server = j->valuestring;
    }
    cJSON_Delete(json);

    // Trim surrounding whitespace.
    size_t s = server.find_first_not_of(" \t\r\n");
    size_t e = server.find_last_not_of(" \t\r\n");
    server = (s == std::string::npos) ? std::string{} : server.substr(s, e - s + 1);

    // Unchanged → nothing to apply: skip the NVS write and the reboot entirely.
    if (server == g_vehicle->load_config_str("syslog_uri")) {
        return send_json(req, 200, make_response(true, "set_syslog", "",
            server.empty() ? "Syslog already disabled — no reboot"
                           : "Syslog server unchanged — no reboot"));
    }

    // Validate plausibility before applying a *changed* value.
    if (!tk::syslog_target_is_plausible(server)) {
        return send_json(req, 400, make_response(false, "set_syslog", "",
                                                 "invalid server (use host:port)"));
    }

    bool ok = g_vehicle->save_config_str("syslog_uri", server);
    esp_err_t r = send_json(req, ok ? 200 : 500,
        make_response(ok, "set_syslog", "",
                      ok ? (server.empty() ? "Syslog disabled — rebooting"
                                           : "Syslog server saved — rebooting")
                         : "failed to save Syslog server"));
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    return r;
}
