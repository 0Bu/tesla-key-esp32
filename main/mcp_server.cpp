#include "http_handlers.hpp"
#include "logic/mcp.hpp"
#include "logic/command_result.hpp"
#include "logic/json_syntax.hpp"
#include "logic/link_state.hpp"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// MCP endpoint (POST /mcp) — Streamable HTTP transport, STATELESS profile:
// one JSON-RPC 2.0 message per POST, answered with application/json. No SSE stream, no
// Mcp-Session-Id, no server-initiated requests — an evcc-class LAN device has no use for
// them and every long-lived stream would pin one of the few httpd sockets. Notifications
// (method present, id absent) get HTTP 202 with no body, as the transport spec prescribes.
// Batches are rejected: protocol 2025-06-18 removed them, and a bounded single-message
// parse keeps the heap cost predictable. Method/tool routing, version negotiation and the
// argument validation lives in logic/mcp.hpp (host-tested); this file is the cJSON/httpd shell.
// User/integrator guide (wire + client examples): docs/MCP.md.

static const char* TAG = "mcp_server";

// Fixed 503 fallback for the paths a fragmented heap can starve: cJSON returns NULL on
// alloc failure (it does not throw), so these bypass the handle_all try/catch. Mirrors
// send_json's guard (http_common.cpp) — never strlen(NULL)-crash the httpd task.
static esp_err_t send_oom_503_(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":"
                                   "{\"code\":-32603,\"message\":\"out of memory\"}}");
}

static esp_err_t send_too_large_413_(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "413 Payload Too Large");
    return httpd_resp_sendstr(req, "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":"
                                   "{\"code\":-32600,\"message\":\"request body too large\"}}");
}

struct RpcJsonTransport {
    httpd_req_t* req;

    void set_json_type() const noexcept {
        httpd_resp_set_type(req, "application/json");
    }
    void set_status(int status) const noexcept {
        if (status == 503) httpd_resp_set_status(req, "503 Service Unavailable");
    }
    esp_err_t send(const char* body) const noexcept {
        return httpd_resp_sendstr(req, body);
    }
};

// Serialize + send a JSON-RPC envelope (consumes root).
static esp_err_t send_rpc_(httpd_req_t* req, tk::JsonOwner root) {
    RpcJsonTransport transport{req};
    return tk::json_http_reply(
        transport,
        std::move(root),
        200,
        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":"
        "{\"code\":-32603,\"message\":\"out of memory\"}}");
}

static esp_err_t send_rpc_result_(httpd_req_t* req, const tk::mcp_json::RpcId& id,
                                  tk::JsonOwner result) {
    return send_rpc_(req, tk::mcp_json::build_result_envelope(id, std::move(result)));
}

static esp_err_t send_rpc_error_(httpd_req_t* req, const tk::mcp_json::RpcId& id,
                                 int code, const char* message) {
    return send_rpc_(req, tk::mcp_json::build_error_envelope(id, code, message));
}

// 202 Accepted, empty body — the transport's reply to notifications.
static esp_err_t send_accepted_(httpd_req_t* req) {
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req, nullptr, 0);
}

// ─── initialize ───────────────────────────────────────────────────────────────

static esp_err_t handle_initialize_(httpd_req_t* req, const tk::mcp_json::RpcId& id,
                                    const char* negotiated_version) {
    tk::JsonBuilder json;
    json.string(json.root(), "protocolVersion", negotiated_version);
    cJSON* caps = json.object(json.root(), "capabilities");
    json.object(caps, "tools");  // tools only — no resources/prompts
    cJSON* info = json.object(json.root(), "serverInfo");
    json.string(info, "name", "tesla-key-esp32");
    json.string(info, "version", esp_app_get_description()->version);
    json.string(json.root(), "instructions",
        "BLE-to-HTTP bridge for one Tesla, paired as Charging Manager: charging commands "
        "and cached read-only state only. get_vehicle_state never wakes the car; commands "
        "block for the BLE round-trip — typically 3-5s after idle, up to 20s when the car "
        "is unreachable.");
    return send_rpc_result_(req, id, json.finish());
}

// ─── tools/list ───────────────────────────────────────────────────────────────

// tools/list is this endpoint's LARGEST response (~1.5 KB serialized) and cJSON prints it
// into one contiguous block — the crash-risk currency on this heap (see .claude/CLAUDE.md), so
// tool descriptions in logic/mcp.hpp stay terse and the tool set stays small. The static
// registry strings are attached as references (no per-request strdup of .rodata).
static esp_err_t handle_tools_list_(httpd_req_t* req, const tk::mcp_json::RpcId& id) {
    return send_rpc_result_(req, id, tk::mcp_json::build_tools_list_result());
}

// ─── tools/call ───────────────────────────────────────────────────────────────

// Read-only state snapshot from the caches — by design this NEVER touches BLE (no scan,
// no connect, no wake), so an agent polling it cannot keep a parked car awake. Same
// no-wake rule as the background telemetry poll.
static tk::JsonOwner vehicle_state_result_() {
    tk::mcp_json::VehicleStatePayload state;
    const std::string vin = g_vehicle->vin();
    state.vin = vin.c_str();
    state.paired = g_vehicle->has_session();
    state.link = tk::link_state_web_str(g_vehicle->link_state());
    uint32_t ago = 0;
    state.has_last_seen = g_vehicle->seconds_since_contact(ago);
    state.last_seen_s = static_cast<double>(ago);
    ChargeStateResult cs = g_vehicle->get_cached_charge();
    if (cs.valid) {
        state.has_soc = cs.has_battery_level;
        state.soc = cs.battery_level;
        state.charging_state = cs.charging_state.c_str();
        state.has_charge_limit = cs.has_charge_limit_soc;
        state.charge_limit = cs.charge_limit_soc;
        state.has_charge_amps = cs.has_charging_amps;
        state.charge_amps = cs.charging_amps;
        state.has_charger_power = cs.has_charger_power;
        state.charger_power_kw = cs.charger_power;
    }
    return tk::mcp_json::build_vehicle_state_result(state);
}

static esp_err_t handle_tools_call_(httpd_req_t* req, const tk::mcp_json::RpcId& id,
                                    tk::JsonOwner request) {
    const cJSON* params = cJSON_GetObjectItemCaseSensitive(request.get(), "params");
    const cJSON* jname = cJSON_GetObjectItemCaseSensitive(params, "name");
    const char* name   = cJSON_IsString(jname) ? jname->valuestring : nullptr;
    const tk::CmdInfo* info = tk::cmd_from_mcp_name(name);
    if (!info) {
        request.reset();
        return send_rpc_error_(req, id, tk::kJsonRpcInvalidParams, "unknown tool");
    }
    ESP_LOGI(TAG, "tools/call %s", name);
    const cJSON* args = cJSON_GetObjectItemCaseSensitive(params, "arguments");

    // Validate + extract the arguments against the spec table (logic/mcp.hpp), keeping
    // "absent" and "invalid" apart: an ABSENT required arg or a PRESENT-but-unparseable
    // arg of either kind is a protocol error (-32602) — silently defaulting either would
    // execute a wrong command and report success (set_scheduled_charging without
    // "enable" would DISABLE the schedule; start_minutes:"08:00" would schedule
    // midnight). Absent optional Int args default to 0. LLM clients routinely encode
    // loosely, so numeric strings are accepted for Int args ("16" → 16) and exact 0/1
    // numbers for Bool args. Integers must be finite, integral and inside the spec bounds;
    // booleans reject every numeric spelling except finite 0 and 1.
    int  ival[tk::kCmdMaxArgs] = {};
    bool bval[tk::kCmdMaxArgs] = {};
    for (int i = 0; i < tk::kCmdMaxArgs; ++i) {
        const tk::CmdArg& a = info->args[i];
        if (a.type == tk::CmdArgType::None || !a.mcp_key) continue;
        const cJSON* j = cJSON_GetObjectItemCaseSensitive(args, a.mcp_key);
        const char* problem = nullptr;
        if (!j) {
            if (a.mcp_required) problem = "missing required argument";
            // absent optional → keep the zero default
        } else if (a.type == tk::CmdArgType::Int) {
            if (cJSON_IsNumber(j)) {
                if (!tk::mcp_integer_value(j->valuedouble, a.lo, a.hi, ival[i]))
                    problem = "integer required";
            } else if (cJSON_IsString(j) && j->valuestring) {
                char* end = nullptr;
                double d = strtod(j->valuestring, &end);
                const bool parsed = end != j->valuestring && end && *end == '\0' &&
                                    tk::mcp_integer_value(d, a.lo, a.hi, ival[i]);
                if (!parsed)
                    problem = "integer required";
            } else {
                problem = "invalid argument";
            }
        } else {  // Bool
            if      (cJSON_IsBool(j))   bval[i] = cJSON_IsTrue(j);
            else if (cJSON_IsNumber(j)) {
                if (!tk::mcp_bool_value(j->valuedouble, bval[i]))
                    problem = "boolean required";
            }
            else                        problem = "invalid argument";
        }
        if (problem) {
            char m[64];
            snprintf(m, sizeof(m), "%s: %s", problem, a.mcp_key);
            request.reset();
            return send_rpc_error_(req, id, tk::kJsonRpcInvalidParams, m);
        }
    }

    // name/arguments have been reduced to registry pointers plus fixed local arrays. Release the
    // complete input-scaled request tree before cached-state formatting or the long BLE dispatch.
    request.reset();
    if (info->kind == tk::CmdKind::GetVehicleState) {
        return send_rpc_result_(req, id, vehicle_state_result_());
    }

    // Same positional-values dispatch the REST /command path uses (command_exec.cpp).
    bool ok = execute_vehicle_command(*g_vehicle, info->kind, ival, bval);

    // Command outcome text is shared with the REST /command path (logic/command_result.hpp)
    // so the two can never report the same outcome differently. Tool-level failures are
    // isError results, not JSON-RPC errors (the protocol reserves those for malformed calls).
    std::string err = g_vehicle->last_command_error();
    const char* text = tk::command_result_text(ok, err);
    return send_rpc_result_(req, id, tk::mcp_json::build_tool_result(text, !ok));
}

// ─── entry points (dispatched from http_server.cpp's handle_all) ──────────────

esp_err_t mcp_handle_post(GuardedReq rq) {
    httpd_req_t* req = rq.req;

    // Preserve transport failures: a missing/failed body is a parse error, an oversized body is a
    // transport-level 413, and failure to allocate the bounded buffer is a 503. A raw nullptr used
    // to collapse all four states into a JSON-RPC parse error and hid the actual resource failure.
    tk::BodyReadResult body = read_body_result(req);
    if (body.status == tk::BodyReadStatus::NoMemory) return send_oom_503_(req);
    if (body.status == tk::BodyReadStatus::TooLarge) return send_too_large_413_(req);
    if (body.status != tk::BodyReadStatus::Ok) {
        return send_rpc_error_(req, {}, tk::kJsonRpcParseError, "parse error");
    }
    std::unique_ptr<char, decltype(&free)> body_owner(body.data, &free);
    const tk::JsonRawNumberId raw_numeric_id =
        tk::json_top_level_numeric_id(body.data, req->content_len);
    const auto materialized = tk::json_materialize<cJSON>(
        body.data, req->content_len, [](const char* text) { return cJSON_Parse(text); });
    body_owner.reset();
    if (materialized.status == tk::JsonMaterializeStatus::Malformed) {
        return send_rpc_error_(req, {}, tk::kJsonRpcParseError, "parse error");
    }
    if (materialized.status == tk::JsonMaterializeStatus::TooDeep) {
        return send_rpc_error_(req, {}, tk::kJsonRpcInvalidRequest, "JSON nesting too deep");
    }
    if (materialized.status == tk::JsonMaterializeStatus::UnsupportedNul) {
        return send_rpc_error_(req, {}, tk::kJsonRpcInvalidRequest,
                               "JSON NUL escape not supported");
    }
    if (materialized.status == tk::JsonMaterializeStatus::NoMemory) return send_oom_503_(req);
    tk::JsonOwner msg(materialized.root);
    if (!cJSON_IsObject(msg.get())) {
        // Arrays are JSON-RPC batches (removed in protocol 2025-06-18); a bare scalar or
        // string is simply not a request. Both are Invalid Request, not silent 202s.
        const bool is_batch = cJSON_IsArray(msg.get());
        msg.reset();
        return send_rpc_error_(req, {}, tk::kJsonRpcInvalidRequest,
                               is_batch ? "batching not supported" : "invalid request");
    }

    // Validate the common envelope before notification detection or method dispatch. The shared
    // inspector rejects recursive duplicate keys, requires jsonrpc:"2.0", and snapshots only a
    // unique protocol-valid id (safe integer or bounded string). This leaves no input-scaled
    // ownership alive after msg.reset() while preserving a valid id for correlated -32600 errors.
    const tk::mcp_json::RpcRequestEnvelope envelope =
        tk::mcp_json::inspect_request_envelope(msg.get(), raw_numeric_id);
    if (envelope.status != tk::mcp_json::RpcRequestStatus::Valid) {
        const char* problem = "invalid request";
        switch (envelope.status) {
            case tk::mcp_json::RpcRequestStatus::DuplicateKey:
                problem = "duplicate JSON key";
                break;
            case tk::mcp_json::RpcRequestStatus::InvalidId:
                problem = "invalid or oversized id";
                break;
            case tk::mcp_json::RpcRequestStatus::InvalidVersion:
                problem = "jsonrpc must be \"2.0\"";
                break;
            case tk::mcp_json::RpcRequestStatus::InvalidObject:
            case tk::mcp_json::RpcRequestStatus::Valid:
                break;
        }
        const tk::mcp_json::RpcId correlation = envelope.id;
        msg.reset();
        return send_rpc_error_(req, correlation, tk::kJsonRpcInvalidRequest, problem);
    }
    const tk::mcp_json::RpcId id = envelope.id;
    const tk::mcp_json::RpcIdStatus id_status = envelope.id_status;

    const cJSON* jm    = cJSON_GetObjectItemCaseSensitive(msg.get(), "method");
    const char* method = cJSON_IsString(jm) ? jm->valuestring : nullptr;
    tk::McpMethod m    = tk::mcp_method_from(method);

    // No id + a method => notification (notifications/initialized, ...) acknowledged with 202 and
    // no body per the Streamable HTTP transport. This server never initiates requests, so client
    // responses have no valid route. No id AND no method is not a notification, it is a
    // malformed message (e.g. "{}") — flag it instead of leaving the client waiting.
    if (id_status == tk::mcp_json::RpcIdStatus::Missing) {
        const bool is_notification = (method != nullptr);
        msg.reset();
        return is_notification ? send_accepted_(req)
                               : send_rpc_error_(req, {}, tk::kJsonRpcInvalidRequest,
                                                 "missing method");
    }

    // Reduce every input-dependent value to static/fixed data before building a response. Only
    // tools/call retains the parse tree long enough to copy its bounded argument arrays itself.
    // In particular, tools/list drops an otherwise attacker-inflatable 2-KiB input tree before
    // constructing and printing this endpoint's largest contiguous response.
    const bool has_method = method != nullptr;
    const char* negotiated_version = tk::mcp_negotiate_version(nullptr);
    if (m == tk::McpMethod::Initialize) {
        const cJSON* params = cJSON_GetObjectItemCaseSensitive(msg.get(), "params");
        const cJSON* jv = cJSON_GetObjectItemCaseSensitive(params, "protocolVersion");
        const char* requested = cJSON_IsString(jv) ? jv->valuestring : nullptr;
        negotiated_version = tk::mcp_negotiate_version(requested);
    }

    if (m == tk::McpMethod::ToolsCall) {
        return handle_tools_call_(req, id, std::move(msg));
    }
    msg.reset();

    esp_err_t ret = ESP_FAIL;
    switch (m) {
        case tk::McpMethod::Initialize:
            ret = handle_initialize_(req, id, negotiated_version);
            break;
        case tk::McpMethod::ToolsList:
            ret = handle_tools_list_(req, id);
            break;
        case tk::McpMethod::ToolsCall:
            break;  // handled before the input tree is released
        case tk::McpMethod::Ping: {
            tk::JsonBuilder result;
            ret = send_rpc_result_(req, id, result.finish());
            break;
        }
        case tk::McpMethod::Notification:
            // A notifications/* method MUST NOT carry an id (MCP lifecycle). Answering
            // "method not found" would wrongly imply the namespace is unsupported.
            ret = send_rpc_error_(req, id, tk::kJsonRpcInvalidRequest,
                                  "notification must not have an id");
            break;
        default:
            ret = send_rpc_error_(req, id, has_method ? tk::kJsonRpcMethodNotFound
                                                                 : tk::kJsonRpcInvalidRequest,
                                  has_method ? "method not found" : "missing method");
            break;
    }
    return ret;
}

esp_err_t mcp_handle_get(GuardedReq rq) {
    // Stateless profile: no server-initiated SSE stream is offered.
    httpd_req_t* req = rq.req;
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_set_hdr(req, "Allow", "POST");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"error\":\"use POST (stateless MCP, no SSE stream)\"}");
}
