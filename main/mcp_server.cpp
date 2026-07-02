#include "http_handlers.hpp"
#include "logic/mcp.hpp"
#include "logic/link_state.hpp"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <cstdlib>
#include <cstring>
#include <string>

// MCP endpoint (POST /mcp) — Streamable HTTP transport, STATELESS profile:
// one JSON-RPC 2.0 message per POST, answered with application/json. No SSE stream, no
// Mcp-Session-Id, no server-initiated requests — an evcc-class LAN device has no use for
// them and every long-lived stream would pin one of the few httpd sockets. Notifications
// (and client responses) get HTTP 202 with no body, as the transport spec prescribes.
// Batches are rejected: protocol 2025-06-18 removed them, and a bounded single-message
// parse keeps the heap cost predictable. Method/tool routing, version negotiation and the
// argument clamps live in logic/mcp.hpp (host-tested); this file is the cJSON/httpd shell.
// User/integrator guide (wire + client examples): docs/MCP.md.

static const char* TAG = "mcp_server";

// Serialize + send a JSON-RPC envelope. Mirrors send_json (http_common.cpp): on a
// fragmented heap cJSON_PrintUnformatted returns NULL (it does not throw), which would
// bypass the handle_all try/catch — degrade to a 503 instead of strlen(NULL)-crashing
// the httpd task.
static esp_err_t send_rpc_(httpd_req_t* req, cJSON* root) {
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    if (!body) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":"
                                       "{\"code\":-32603,\"message\":\"out of memory\"}}");
    }
    esp_err_t ret = httpd_resp_sendstr(req, body);
    free(body);
    return ret;
}

// Envelope helpers. `id` is adopted (ownership transferred); pass nullptr for a null id.
static cJSON* rpc_envelope_(cJSON* id) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(root, "id", id);
    else    cJSON_AddNullToObject(root, "id");
    return root;
}

static esp_err_t send_rpc_result_(httpd_req_t* req, cJSON* id, cJSON* result) {
    cJSON* root = rpc_envelope_(id);
    cJSON_AddItemToObject(root, "result", result);
    return send_rpc_(req, root);
}

static esp_err_t send_rpc_error_(httpd_req_t* req, cJSON* id, int code, const char* message) {
    cJSON* root = rpc_envelope_(id);
    cJSON* err  = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(root, "error", err);
    return send_rpc_(req, root);
}

// 202 Accepted, empty body — the transport's reply to notifications and client responses.
static esp_err_t send_accepted_(httpd_req_t* req) {
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req, nullptr, 0);
}

// ─── initialize ───────────────────────────────────────────────────────────────

static esp_err_t handle_initialize_(httpd_req_t* req, cJSON* id, const cJSON* params) {
    const cJSON* jv = cJSON_GetObjectItemCaseSensitive(params, "protocolVersion");
    const char* requested = cJSON_IsString(jv) ? jv->valuestring : nullptr;

    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", tk::mcp_negotiate_version(requested));
    cJSON* caps = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());  // tools only — no resources/prompts
    cJSON_AddItemToObject(result, "capabilities", caps);
    cJSON* info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name",    "tesla-key-esp32");
    cJSON_AddStringToObject(info, "version", esp_app_get_description()->version);
    cJSON_AddItemToObject(result, "serverInfo", info);
    cJSON_AddStringToObject(result, "instructions",
        "BLE-to-HTTP bridge for one Tesla, paired as Charging Manager: charging commands "
        "and cached read-only state only. get_vehicle_state never wakes the car; commands "
        "may take ~5s (BLE scan+connect).");
    return send_rpc_result_(req, id, result);
}

// ─── tools/list ───────────────────────────────────────────────────────────────

// Build one tool's inputSchema. All schemas are tiny; the parameterized commands carry
// the same bounds the executor clamps to, so a well-behaved client never sends an
// out-of-range value in the first place.
static cJSON* tool_schema_(tk::McpTool t) {
    cJSON* schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON* props = cJSON_CreateObject();
    cJSON_AddItemToObject(schema, "properties", props);

    auto add_int = [&](const char* key, int lo, int hi) {
        cJSON* p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "type", "integer");
        cJSON_AddNumberToObject(p, "minimum", lo);
        cJSON_AddNumberToObject(p, "maximum", hi);
        cJSON_AddItemToObject(props, key, p);
    };

    switch (t) {
        case tk::McpTool::SetChargingAmps: {
            add_int("amps", 0, 48);
            cJSON* reqd = cJSON_CreateArray();
            cJSON_AddItemToArray(reqd, cJSON_CreateString("amps"));
            cJSON_AddItemToObject(schema, "required", reqd);
            break;
        }
        case tk::McpTool::SetChargeLimit: {
            add_int("percent", 50, 100);
            cJSON* reqd = cJSON_CreateArray();
            cJSON_AddItemToArray(reqd, cJSON_CreateString("percent"));
            cJSON_AddItemToObject(schema, "required", reqd);
            break;
        }
        case tk::McpTool::SetScheduledCharging: {
            cJSON* p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "type", "boolean");
            cJSON_AddItemToObject(props, "enable", p);
            add_int("start_minutes", 0, 1439);
            cJSON* reqd = cJSON_CreateArray();
            cJSON_AddItemToArray(reqd, cJSON_CreateString("enable"));
            cJSON_AddItemToObject(schema, "required", reqd);
            break;
        }
        default:  // no-arg tools keep the empty properties object
            break;
    }
    return schema;
}

// tools/list is this endpoint's LARGEST response (~3 KB) and cJSON prints it into one
// contiguous block — that is the crash-risk currency on this heap (see CLAUDE.md), so
// tool descriptions in logic/mcp.hpp stay terse and the tool set stays small.
static esp_err_t handle_tools_list_(httpd_req_t* req, cJSON* id) {
    cJSON* result = cJSON_CreateObject();
    cJSON* tools  = cJSON_CreateArray();
    cJSON_AddItemToObject(result, "tools", tools);
    for (const auto& t : tk::kMcpTools) {
        cJSON* tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name",        t.name);
        cJSON_AddStringToObject(tool, "description", t.description);
        cJSON_AddItemToObject(tool, "inputSchema", tool_schema_(t.tool));
        cJSON_AddItemToArray(tools, tool);
    }
    return send_rpc_result_(req, id, result);
}

// ─── tools/call ───────────────────────────────────────────────────────────────

// Wrap `text` as a tools/call result: {content:[{type:"text",text}], isError}. The
// string is copied by cJSON.
static cJSON* tool_result_(const char* text, bool is_error) {
    cJSON* result  = cJSON_CreateObject();
    cJSON* content = cJSON_CreateArray();
    cJSON* block   = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "type", "text");
    cJSON_AddStringToObject(block, "text", text);
    cJSON_AddItemToArray(content, block);
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddBoolToObject(result, "isError", is_error);
    return result;
}

// Read-only state snapshot from the caches — by design this NEVER touches BLE (no scan,
// no connect, no wake), so an agent polling it cannot keep a parked car awake. Same
// no-wake rule as the background telemetry poll.
static cJSON* vehicle_state_result_() {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "vin",    g_vehicle->vin().c_str());
    cJSON_AddBoolToObject(o,   "paired", g_vehicle->has_session());
    cJSON_AddStringToObject(o, "link",   tk::link_state_web_str(g_vehicle->link_state()));
    uint32_t ago = 0;
    if (g_vehicle->seconds_since_contact(ago))
        cJSON_AddNumberToObject(o, "last_seen_s", (double)ago);
    ChargeStateResult cs = g_vehicle->get_cached_charge();
    if (cs.valid) {
        if (cs.has_battery_level)    cJSON_AddNumberToObject(o, "soc", cs.battery_level);
        if (!cs.charging_state.empty())
            cJSON_AddStringToObject(o, "charging_state", cs.charging_state.c_str());
        if (cs.has_charge_limit_soc) cJSON_AddNumberToObject(o, "charge_limit", cs.charge_limit_soc);
        if (cs.has_charging_amps)    cJSON_AddNumberToObject(o, "charge_amps",  cs.charging_amps);
        if (cs.has_charger_power)    cJSON_AddNumberToObject(o, "charger_power_kw", cs.charger_power);
    }
    // The content block must be text, so print the object into it (small, bounded).
    char* text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!text) return tool_result_("out of memory", true);
    cJSON* result = tool_result_(text, false);
    free(text);
    return result;
}

static esp_err_t handle_tools_call_(httpd_req_t* req, cJSON* id, const cJSON* params) {
    const cJSON* jname = cJSON_GetObjectItemCaseSensitive(params, "name");
    const char* name   = cJSON_IsString(jname) ? jname->valuestring : nullptr;
    tk::McpTool tool   = tk::mcp_tool_from(name);
    if (tool == tk::McpTool::Unknown) {
        return send_rpc_error_(req, id, tk::kJsonRpcInvalidParams, "unknown tool");
    }
    const cJSON* args = cJSON_GetObjectItemCaseSensitive(params, "arguments");

    // Clamped integer argument (missing/non-numeric → clamped default). The car is the
    // real backstop; this keeps a malformed value in range and out of UB (logic/mcp.hpp).
    auto arg_int = [&](const char* key, int dflt, int lo, int hi) {
        const cJSON* j = cJSON_GetObjectItemCaseSensitive(args, key);
        double d = cJSON_IsNumber(j) ? j->valuedouble : (double)dflt;
        return tk::clamped_int(d, lo, hi);
    };
    auto arg_bool = [&](const char* key) {
        const cJSON* j = cJSON_GetObjectItemCaseSensitive(args, key);
        return cJSON_IsTrue(j);
    };

    if (tool == tk::McpTool::GetVehicleState) {
        return send_rpc_result_(req, id, vehicle_state_result_());
    }

    ESP_LOGI(TAG, "tools/call %s", name);
    bool ok = false;
    switch (tool) {
        case tk::McpTool::WakeUp:          ok = g_vehicle->wake_up();           break;
        case tk::McpTool::ChargeStart:     ok = g_vehicle->charge_start();      break;
        case tk::McpTool::ChargeStop:      ok = g_vehicle->charge_stop();       break;
        case tk::McpTool::ChargePortOpen:  ok = g_vehicle->charge_port_open();  break;
        case tk::McpTool::ChargePortClose: ok = g_vehicle->charge_port_close(); break;
        case tk::McpTool::SetChargingAmps:
            ok = g_vehicle->set_charging_amps(arg_int("amps", 0, 0, 48));
            break;
        case tk::McpTool::SetChargeLimit:
            ok = g_vehicle->set_charge_limit(arg_int("percent", 80, 50, 100));
            break;
        case tk::McpTool::SetScheduledCharging:
            ok = g_vehicle->set_scheduled_charging(arg_bool("enable"),
                                                   arg_int("start_minutes", 0, 0, 1439));
            break;
        default: break;  // GetVehicleState/Unknown handled above
    }

    // Same failure split as the REST path: a car-rejected command carries the real Tesla
    // reason; no reply at all means unreachable. Tool-level failures are isError results,
    // not JSON-RPC errors (the protocol reserves those for malformed calls).
    std::string err = g_vehicle->last_command_error();
    const char* text = ok ? "command executed successfully"
                          : (!err.empty() ? err.c_str() : "vehicle not reachable");
    return send_rpc_result_(req, id, tool_result_(text, !ok));
}

// ─── entry points (dispatched from http_server.cpp's handle_all) ──────────────

esp_err_t mcp_handle_post(GuardedReq rq) {
    httpd_req_t* req = rq.req;

    // Shared bounded body read (http_common.cpp): NULL on empty/oversized (>2 KB)/failed.
    char* body = read_body(req);
    cJSON* msg = body ? cJSON_Parse(body) : nullptr;
    free(body);

    if (!msg) return send_rpc_error_(req, nullptr, tk::kJsonRpcParseError, "parse error");
    if (cJSON_IsArray(msg)) {
        cJSON_Delete(msg);
        return send_rpc_error_(req, nullptr, tk::kJsonRpcInvalidRequest,
                               "batching not supported");
    }

    // Detach the id so it can be adopted into the response envelope. A null id is
    // treated as absent (JSON-RPC 2.0 discourages it; MCP requests must not use it).
    cJSON* id = cJSON_DetachItemFromObjectCaseSensitive(msg, "id");
    if (id && cJSON_IsNull(id)) { cJSON_Delete(id); id = nullptr; }

    const cJSON* jm    = cJSON_GetObjectItemCaseSensitive(msg, "method");
    const char* method = cJSON_IsString(jm) ? jm->valuestring : nullptr;
    tk::McpMethod m    = tk::mcp_method_from(method);

    // No id ⇒ notification (notifications/initialized, notifications/cancelled, …) or a
    // client response to a server request we never send — both are acknowledged with 202
    // and no body per the Streamable HTTP transport.
    if (!id) {
        cJSON_Delete(msg);
        return send_accepted_(req);
    }

    const cJSON* params = cJSON_GetObjectItemCaseSensitive(msg, "params");

    esp_err_t ret;
    switch (m) {
        case tk::McpMethod::Initialize:
            ret = handle_initialize_(req, id, params);
            break;
        case tk::McpMethod::ToolsList:
            ret = handle_tools_list_(req, id);
            break;
        case tk::McpMethod::ToolsCall:
            ret = handle_tools_call_(req, id, params);
            break;
        case tk::McpMethod::Ping:
            ret = send_rpc_result_(req, id, cJSON_CreateObject());
            break;
        default:
            ret = send_rpc_error_(req, id, method ? tk::kJsonRpcMethodNotFound
                                                  : tk::kJsonRpcInvalidRequest,
                                  method ? "method not found" : "missing method");
            break;
    }
    // id was adopted by the response envelope; msg still owns method/params.
    cJSON_Delete(msg);
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
