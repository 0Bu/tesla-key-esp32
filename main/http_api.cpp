// evcc-facing HTTP API — the TeslaBleHttpProxy-compatible routes:
//   POST /api/1/vehicles/{VIN}/command/{CMD}
//   GET  /api/1/vehicles/{VIN}/vehicle_data
//   GET  /api/1/vehicles/{VIN}/body_controller_state
//   GET  /api/proxy/1/version
// Dispatched from handle_all in http_server.cpp (inside its try/catch OOM guard).

#include "http_handlers.hpp"
#include "logic/command_result.hpp"   // outcome text shared with the MCP tools/call path
#include "platform.hpp"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

static const char* TAG = "http_server";

// Derived from PROJECT_VER (project root version.txt) so the reported version,
// the built firmware filename, and the web-installer manifest never drift apart.
static const char* fw_version() {
    static char buf[40] = {0};
    if (buf[0] == '\0') {
        snprintf(buf, sizeof(buf), "%s-esp32", esp_app_get_description()->version);
    }
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

// Read an integer command parameter from the JSON body, clamped to [lo,hi]. The double is
// clamped BEFORE the int cast — casting an out-of-range double to int is undefined behaviour,
// so a hostile `{"percent": 1e300}` must never reach `(int)valuedouble`. The string path is
// bounded too. The car is the real backstop; this just keeps a malformed/hostile LAN request
// in range (amps 0–48, percent 50–100, start_minutes 0–1439 — matching the controller's own
// clamps in vehicle_commands.cpp). `obj` may be null (no body) → returns the clamped default.
static int json_int_clamped(const cJSON* obj, const char* key, int dflt, int lo, int hi) {
    int v = dflt;
    const cJSON* j = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(j)) {
        double d = j->valuedouble;
        if (d < (double)lo) d = lo; else if (d > (double)hi) d = hi;
        v = (int)d;
    } else if (cJSON_IsString(j) && j->valuestring) {
        v = atoi(j->valuestring);
    }
    if (v < lo) v = lo; else if (v > hi) v = hi;
    return v;
}

// ─── POST /api/1/vehicles/{VIN}/command/{CMD} ─────────────────────────────────

esp_err_t handle_command(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    char vin[64], cmd[64];
    if (!parse_uri(req->uri, vin, sizeof(vin), cmd, sizeof(cmd))) {
        return send_json(req, 400, make_response(false, "unknown", "?", "invalid URI"));
    }
    ESP_LOGI(TAG, "CMD %s on VIN %s", cmd, vin);

    // Parse optional JSON body
    char* body = read_body(req);
    cJSON* json = body ? cJSON_Parse(body) : nullptr;
    free(body);

    // Name → descriptor via the shared registry (logic/command_registry.hpp) — the SAME
    // table the MCP tools read, so the two surfaces can never disagree about names or
    // argument bounds. REST stays LENIENT by protocol compat: an absent/unparseable
    // value falls back to the spec's api_default (e.g. set_charge_limit with no body
    // still means 80%), unlike MCP's strict -32602. Values are positional: args[i]
    // fills ival[i]/bval[i], which execute_vehicle_command consumes by position.
    const tk::CmdInfo* info = tk::cmd_from_api_name(cmd);
    if (!info) {
        cJSON_Delete(json);
        return send_json(req, 404, make_response(false, cmd, vin, "unknown command"));
    }

    int  ival[tk::kCmdMaxArgs] = {};
    bool bval[tk::kCmdMaxArgs] = {};
    for (int i = 0; i < tk::kCmdMaxArgs; ++i) {
        const tk::CmdArg& a = info->args[i];
        if (a.type == tk::CmdArgType::None || !a.api_key) continue;
        if (a.type == tk::CmdArgType::Int) {
            ival[i] = json_int_clamped(json, a.api_key, a.api_default, a.lo, a.hi);
        } else {  // Bool: absent → false (cJSON_GetObjectItemCaseSensitive(NULL,…) is NULL)
            const cJSON* j = cJSON_GetObjectItemCaseSensitive(json, a.api_key);
            bval[i] = j ? cJSON_IsTrue(j) : false;
        }
    }

    bool ok = execute_vehicle_command(*g_vehicle, info->kind, ival, bval);
    cJSON_Delete(json);
    // On failure, distinguish "the car rejected it" (we got an error reply, e.g.
    // "complete") from "the car was unreachable" (no reply / timed out). The former
    // carries the real Tesla reason; only the latter is an in-range problem. The text
    // selection is shared with the MCP tools/call result (logic/mcp.hpp) so the two
    // paths can never report the same outcome differently.
    std::string err = g_vehicle->last_command_error();
    return send_json(req, 200, make_response(ok, cmd, vin, tk::command_result_text(ok, err)));
}

// ─── GET /api/1/vehicles/{VIN}/vehicle_data ───────────────────────────────────

esp_err_t handle_vehicle_data(GuardedReq rq) {
    httpd_req_t* req = rq.req;
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

esp_err_t handle_body_controller(GuardedReq rq) {
    httpd_req_t* req = rq.req;
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

// ─── GET /api/proxy/1/version ─────────────────────────────────────────────────

esp_err_t handle_version(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", fw_version());
    cJSON_AddStringToObject(root, "platform", TK_PLATFORM);
    return send_json(req, 200, root);
}
