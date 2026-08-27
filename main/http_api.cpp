// evcc-facing HTTP API — the TeslaBleHttpProxy-compatible routes:
//   POST /api/1/vehicles/{VIN}/command/{CMD}
//   GET  /api/1/vehicles/{VIN}/vehicle_data
//   GET  /api/1/vehicles/{VIN}/body_controller_state
//   GET  /api/proxy/1/version
// Dispatched from handle_all in http_server.cpp (inside its try/catch OOM guard).

#include "http_handlers.hpp"
#include "logic/json_syntax.hpp"
#include "logic/command_result.hpp"   // outcome text shared with the MCP tools/call path
#include "platform.hpp"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
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

static constexpr const char* kVehiclePrefix = "/api/1/vehicles/";

static const char* uri_path_end(const char* uri) {
    const char* query = uri ? strchr(uri, '?') : nullptr;
    return query ? query : (uri ? uri + strlen(uri) : nullptr);
}

// Parse exactly /api/1/vehicles/{VIN}/command/{CMD}. The central classifier already admits only
// this route shape; this second full anchor keeps the handler safe if it is ever called directly
// and prevents a matching substring inside an unrelated path.
static bool parse_uri(const char* uri, char* vin_out, size_t vin_sz,
                      char* cmd_out, size_t cmd_sz) {
    if (!uri || strncmp(uri, kVehiclePrefix, strlen(kVehiclePrefix)) != 0) return false;
    const char* path_end = uri_path_end(uri);
    const char* vin_start = uri + strlen(kVehiclePrefix);
    const char* command = strchr(vin_start, '/');
    static constexpr const char* kCommand = "/command/";
    if (!command || command >= path_end ||
        strncmp(command, kCommand, strlen(kCommand)) != 0) return false;
    size_t vin_len = command - vin_start;
    if (vin_len == 0 || vin_len >= vin_sz) return false;
    strncpy(vin_out, vin_start, vin_len);
    vin_out[vin_len] = '\0';
    const char* cmd_start = command + strlen(kCommand);
    size_t cmd_len = static_cast<size_t>(path_end - cmd_start);
    if (cmd_len == 0 || cmd_len >= cmd_sz) return false;
    if (memchr(cmd_start, '/', cmd_len) != nullptr) return false;
    strncpy(cmd_out, cmd_start, cmd_len);
    cmd_out[cmd_len] = '\0';
    return true;
}

// Parse exactly /api/1/vehicles/{VIN}{suffix}, allowing only a trailing query string.
static bool parse_vin_route(const char* uri, const char* suffix,
                            char* vin_out, size_t vin_sz) {
    if (!uri || !suffix ||
        strncmp(uri, kVehiclePrefix, strlen(kVehiclePrefix)) != 0) return false;
    const char* path_end = uri_path_end(uri);
    const char* vin_start = uri + strlen(kVehiclePrefix);
    const char* slash = strchr(vin_start, '/');
    if (!slash || slash >= path_end) return false;
    size_t vin_len = static_cast<size_t>(slash - vin_start);
    if (vin_len == 0 || vin_len >= vin_sz) return false;
    const size_t suffix_len = strlen(suffix);
    if (static_cast<size_t>(path_end - slash) != suffix_len ||
        strncmp(slash, suffix, suffix_len) != 0) return false;
    strncpy(vin_out, vin_start, vin_len);
    vin_out[vin_len] = '\0';
    return true;
}

static bool request_vin_matches(const char* vin) {
    return g_vehicle && vin && g_vehicle->vin() == vin;
}

// Parse one REST integer argument against the shared command registry. Optional values keep
// the TeslaBleHttpProxy-compatible default, but an api_required value (charging_amps) must be
// present and parseable. Any supplied value must be an in-range integer: silently truncating or
// clamping executes a different command than the controller requested while reporting success.
static bool json_int_arg(const cJSON* obj, const tk::CmdArg& arg,
                         int& out, const char*& problem) {
    const cJSON* j = obj ? cJSON_GetObjectItemCaseSensitive(obj, arg.api_key) : nullptr;
    if (!j) {
        out = arg.api_default;
        if (arg.api_required) {
            problem = "missing required argument";
            return false;
        }
        return true;
    }

    double d;
    if (cJSON_IsNumber(j)) {
        d = j->valuedouble;
    } else if (cJSON_IsString(j) && j->valuestring) {
        char* end = nullptr;
        d = strtod(j->valuestring, &end);
        if (end == j->valuestring || !end || *end != '\0') {
            problem = "invalid argument";
            return false;
        }
    } else {
        problem = "invalid argument";
        return false;
    }
    if (!std::isfinite(d) || std::trunc(d) != d) {
        problem = "integer required";
        return false;
    }
    if (!tk::strict_rest_int(d, arg, out)) {
        problem = "argument out of range";
        return false;
    }
    return true;
}

static bool json_bool_arg(const cJSON* obj, const tk::CmdArg& arg,
                          bool& out, const char*& problem) {
    const cJSON* j = obj ? cJSON_GetObjectItemCaseSensitive(obj, arg.api_key) : nullptr;
    if (!j) {
        out = false;
        if (arg.api_required) {
            problem = "missing required argument";
            return false;
        }
        return true;
    }
    if (!cJSON_IsBool(j)) {
        problem = "invalid argument";
        return false;
    }
    out = cJSON_IsTrue(j);
    return true;
}

// ─── POST /api/1/vehicles/{VIN}/command/{CMD} ─────────────────────────────────

bool is_command_route(const char* uri) {
    char vin[64], cmd[64];
    return parse_uri(uri, vin, sizeof(vin), cmd, sizeof(cmd));
}

esp_err_t handle_command(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    char vin[64], cmd[64];
    if (!parse_uri(req->uri, vin, sizeof(vin), cmd, sizeof(cmd))) {
        return send_json(req, 400, make_response(false, "unknown", "?", "invalid URI"));
    }
    if (!request_vin_matches(vin)) {
        return send_json(req, 404, make_response(false, cmd, vin, "vehicle VIN mismatch"));
    }
    ESP_LOGI(TAG, "CMD %s on VIN %s", cmd, vin);

    const tk::CmdInfo* info = tk::cmd_from_api_name(cmd);
    if (!info) {
        return send_json(req, 404, make_response(false, cmd, vin, "unknown command"));
    }

    // Parse the body once. Empty is valid for no-argument and legacy-optional commands.
    // evcc's generic boolean setter sends the JSON scalar true for charge_start and false
    // for charge_stop; the shared registry policy admits exactly those two compatibility
    // forms while keeping every other non-object body invalid.
    tk::BodyReadResult body = read_body_result(req);
    if (body.status == tk::BodyReadStatus::TooLarge) {
        return send_json(req, 413, make_response(false, cmd, vin, "request body too large"));
    }
    if (body.status == tk::BodyReadStatus::NoMemory) {
        return send_json(req, 503, make_response(false, cmd, vin, "out of memory"));
    }
    if (body.status == tk::BodyReadStatus::ReceiveFailed) {
        return send_json(req, 400, make_response(false, cmd, vin, "request body read failed"));
    }

    std::unique_ptr<char, decltype(&free)> body_owner(body.data, &free);
    tk::JsonOwner json;
    if (body.status == tk::BodyReadStatus::Ok) {
        const auto materialized = tk::json_materialize<cJSON>(
            body.data, req->content_len, [](const char* text) { return cJSON_Parse(text); });
        if (materialized.status == tk::JsonMaterializeStatus::Malformed) {
            return send_json(req, 400, make_response(false, cmd, vin, "invalid JSON"));
        }
        if (materialized.status == tk::JsonMaterializeStatus::TooDeep) {
            return send_json(req, 400, make_response(false, cmd, vin, "JSON nesting too deep"));
        }
        if (materialized.status == tk::JsonMaterializeStatus::UnsupportedNul) {
            return send_json(req, 400,
                             make_response(false, cmd, vin, "JSON NUL escape not supported"));
        }
        if (materialized.status == tk::JsonMaterializeStatus::NoMemory) {
            return send_json(req, 503, make_response(false, cmd, vin, "out of memory"));
        }
        json.reset(materialized.root);
    }
    // cJSON owns its own parsed tree; release the separate <=2 KiB receive buffer before any BLE
    // command or response allocation. Keeping both alive needlessly shrinks the largest contiguous
    // INTERNAL block at the most allocation-heavy point in this request.
    body_owner.reset();
    tk::RestBodyShape body_shape = tk::RestBodyShape::Empty;
    if (body.status == tk::BodyReadStatus::Ok) {
        if (cJSON_IsObject(json.get())) {
            body_shape = tk::RestBodyShape::Object;
        } else if (cJSON_IsBool(json.get())) {
            body_shape = cJSON_IsTrue(json.get()) ? tk::RestBodyShape::BoolTrue
                                           : tk::RestBodyShape::BoolFalse;
        } else {
            body_shape = tk::RestBodyShape::Other;
        }
    }
    if (!tk::rest_body_allowed(info->kind, body_shape)) {
        return send_json(req, 400, make_response(false, cmd, vin, "invalid JSON object"));
    }

    int  ival[tk::kCmdMaxArgs] = {};
    bool bval[tk::kCmdMaxArgs] = {};
    const char* problem = nullptr;
    const char* problem_key = nullptr;
    for (int i = 0; i < tk::kCmdMaxArgs; ++i) {
        const tk::CmdArg& a = info->args[i];
        if (a.type == tk::CmdArgType::None || !a.api_key) continue;
        if (a.type == tk::CmdArgType::Int) {
            if (!json_int_arg(json.get(), a, ival[i], problem)) {
                problem_key = a.api_key;
                break;
            }
        } else {
            if (!json_bool_arg(json.get(), a, bval[i], problem)) {
                problem_key = a.api_key;
                break;
            }
        }
    }
    if (problem) {
        char reason[96];
        snprintf(reason, sizeof(reason), "%s: %s", problem, problem_key);
        return send_json(req, 400, make_response(false, cmd, vin, reason));
    }

    // All command arguments now live in fixed local arrays. Drop the input-scaled parse tree before
    // the BLE round-trip (up to ~20 s) and before any response/error-string allocation.
    json.reset();
    bool ok = execute_vehicle_command(*g_vehicle, info->kind, ival, bval);
    // On failure, distinguish "the car rejected it" (we got an error reply, e.g.
    // "complete") from "the car was unreachable" (no reply / timed out). The former
    // carries the real Tesla reason; only the latter is an in-range problem. The text
    // selection is shared with the MCP tools/call result (logic/mcp.hpp) so the two
    // paths can never report the same outcome differently.
    std::string err = g_vehicle->last_command_error();
    // A false command outcome is not an HTTP success: evcc otherwise accepts the request
    // as delivered and the old current can remain active. Keep the Tesla-compatible JSON
    // body, but make the transport status retryable/observable.
    return send_json(req, ok ? 200 : 502,
                     make_response(ok, cmd, vin, tk::command_result_text(ok, err)));
}

// ─── GET /api/1/vehicles/{VIN}/vehicle_data ───────────────────────────────────

esp_err_t handle_vehicle_data(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    char vin[64] = {0};
    if (!parse_vin_route(req->uri, "/vehicle_data", vin, sizeof(vin)))
        return send_json(req, 400, make_response(false, "vehicle_data", "?", "invalid URI"));
    if (!request_vin_matches(vin))
        return send_json(req, 404, make_response(false, "vehicle_data", vin, "vehicle VIN mismatch"));

    ChargeStateResult cs{};
    bool ok = g_vehicle->get_charge_state(cs);

    // Shape MUST match the Tesla Fleet API as proxied by TeslaBleHttpProxy:
    //   { "response": { "response": { "charge_state": { ... } } } }
    // evcc reads e.g. .response.response.charge_state.battery_level and
    // .response.response.charge_state.charge_amps — note the doubled "response"
    // and the field name "charge_amps" (not "charging_amps").
    tk::JsonBuilder json;
    cJSON* outer = json.object(json.root(), "response");
    json.boolean(outer, "result", ok);
    json.string(outer, "vin", vin);

    // Always emit a fully-populated charge_state. evcc's tesla-ble template parses
    // .response.response.charge_state.battery_range etc. as floats; a missing field
    // would make it parse "<nil>" and fail. On failure cs is zero-initialised, which
    // still yields valid numbers (and get_charge_state already falls back to the cache).
    cJSON* inner = json.object(outer, "response");
    cJSON* state = json.object(inner, "charge_state");
    json.string(state, "charging_state",
                cs.charging_state.empty() ? "Disconnected" : cs.charging_state.c_str());
    json.number(state, "battery_level", cs.battery_level);
    json.number(state, "charge_limit_soc", cs.charge_limit_soc);
    json.number(state, "charger_power", cs.charger_power);
    json.number(state, "charge_rate", cs.charge_rate);
    json.number(state, "charge_amps", cs.charging_amps);
    json.number(state, "battery_range", cs.battery_range);
    json.string(outer, "reason", ok ? "success" : "stale or unavailable");

    return send_json(req, ok ? 200 : 503, json.release());
}

// ─── GET /api/1/vehicles/{VIN}/body_controller_state ─────────────────────────

esp_err_t handle_body_controller(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    char vin[64] = {0};
    if (!parse_vin_route(req->uri, "/body_controller_state", vin, sizeof(vin)))
        return send_json(req, 400, make_response(false, "body_controller_state", "?", "invalid URI"));
    if (!request_vin_matches(vin))
        return send_json(req, 404, make_response(false, "body_controller_state", vin, "vehicle VIN mismatch"));

    VehicleStatusResult vs{};
    bool ok = g_vehicle->get_vehicle_status(vs, tk::ConnectOrigin::Foreground);

    tk::JsonBuilder json;
    cJSON* response = json.object(json.root(), "response");
    json.boolean(response, "result", ok);
    json.string(response, "vin", vin);

    if (ok) {
        cJSON* data = json.object(response, "data");
        json.string(data, "vehicle_lock_state", vs.lock_state.c_str());
        json.string(data, "vehicle_sleep_status", vs.sleep_status.c_str());
        json.string(data, "user_presence", vs.user_presence.c_str());
        json.string(response, "reason", "success");
    } else {
        json.string(response, "reason", "failed to retrieve vehicle status");
    }

    return send_json(req, 200, json.release());
}

// ─── GET /api/proxy/1/version ─────────────────────────────────────────────────

esp_err_t handle_version(GuardedReq rq) {
    httpd_req_t* req = rq.req;
    tk::JsonBuilder json;
    json.string(json.root(), "version", fw_version());
    json.string(json.root(), "platform", TK_PLATFORM);
    return send_json(req, 200, json.release());
}
