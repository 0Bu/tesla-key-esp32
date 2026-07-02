#pragma once

#include <cstring>
#include <string>

// Pure, hardware-free core of the MCP endpoint (POST /mcp — see main/mcp_server.cpp):
// protocol-version negotiation, JSON-RPC method routing, the tool registry, and the
// integer-argument clamp. Everything IDF/cJSON-coupled stays in mcp_server.cpp; keeping
// the decisions here lets the host mock build (test/) verify them without a board.
namespace tk {

// JSON-RPC 2.0 error codes used by the endpoint.
inline constexpr int kJsonRpcParseError     = -32700;
inline constexpr int kJsonRpcInvalidRequest = -32600;
inline constexpr int kJsonRpcMethodNotFound = -32601;
inline constexpr int kJsonRpcInvalidParams  = -32602;
inline constexpr int kJsonRpcInternalError  = -32603;

// MCP protocol revisions this server speaks (Streamable HTTP transport, stateless,
// JSON responses only — no SSE, no sessions). Newest first: index 0 is the answer when
// the client requests an unknown revision (the spec has the server reply with its own
// latest supported version; the client disconnects if it can't live with it).
inline constexpr const char* kMcpSupportedVersions[] = { "2025-06-18", "2025-03-26" };

inline const char* mcp_negotiate_version(const char* requested) {
    if (requested) {
        for (const char* v : kMcpSupportedVersions)
            if (std::strcmp(requested, v) == 0) return v;
    }
    return kMcpSupportedVersions[0];
}

// JSON-RPC method routing. Notification covers every "notifications/*" method — a
// notification carries no id and gets HTTP 202 with no body instead of a JSON-RPC reply.
enum class McpMethod {
    Initialize,
    ToolsList,
    ToolsCall,
    Ping,
    Notification,
    Unknown,
};

inline McpMethod mcp_method_from(const char* m) {
    if (!m) return McpMethod::Unknown;
    if (std::strcmp(m, "initialize") == 0) return McpMethod::Initialize;
    if (std::strcmp(m, "tools/list") == 0) return McpMethod::ToolsList;
    if (std::strcmp(m, "tools/call") == 0) return McpMethod::ToolsCall;
    if (std::strcmp(m, "ping")       == 0) return McpMethod::Ping;
    if (std::strncmp(m, "notifications/", std::strlen("notifications/")) == 0)
        return McpMethod::Notification;
    return McpMethod::Unknown;
}

// Tool registry — exactly the Charging-Manager command set the enrolled key can execute,
// plus one read-only state tool. The role-refused commands (doors/climate/horn/sentry)
// are deliberately absent: the car rejects them for this key role, so exposing them as
// tools would only mislead the calling model. Descriptions stay terse on purpose —
// tools/list is the endpoint's largest response and cJSON prints it into ONE contiguous
// heap block (see the memory note in CLAUDE.md).
enum class McpTool {
    GetVehicleState,
    WakeUp,
    ChargeStart,
    ChargeStop,
    ChargePortOpen,
    ChargePortClose,
    SetChargingAmps,
    SetChargeLimit,
    SetScheduledCharging,
    Unknown,
};

// Argument specification — the SINGLE source of truth for each tool's parameters. The
// advertised JSON schema (tool_schema_ in mcp_server.cpp), the executor's presence check
// and clamp, and the host tests all read this table, so the advertised bounds and the
// enforced bounds can never drift apart.
enum class McpArgType { None, Int, Bool };

struct McpArg {
    const char* key;
    McpArgType  type;
    bool        required;
    int         lo, hi;   // Int only: inclusive clamp/schema bounds
    int         dflt;     // Int only: value when an optional arg is absent
};

inline constexpr McpArg kNoArg{ nullptr, McpArgType::None, false, 0, 0, 0 };

struct McpToolInfo {
    McpTool     tool;
    const char* name;
    const char* description;
    McpArg      args[2];   // up to two parameters per tool; kNoArg-padded
};

inline constexpr McpToolInfo kMcpTools[] = {
    { McpTool::GetVehicleState, "get_vehicle_state",
      "Read cached vehicle state (VIN, link state, SOC, charging). Never wakes the car.",
      { kNoArg, kNoArg } },
    { McpTool::WakeUp,           "wake_up",           "Wake the vehicle over BLE.",
      { kNoArg, kNoArg } },
    { McpTool::ChargeStart,      "charge_start",      "Start charging.",
      { kNoArg, kNoArg } },
    { McpTool::ChargeStop,       "charge_stop",       "Stop charging.",
      { kNoArg, kNoArg } },
    { McpTool::ChargePortOpen,   "charge_port_open",  "Open the charge port door.",
      { kNoArg, kNoArg } },
    { McpTool::ChargePortClose,  "charge_port_close", "Close the charge port door.",
      { kNoArg, kNoArg } },
    { McpTool::SetChargingAmps,  "set_charging_amps",
      "Set the charging current in amps.",
      { { "amps", McpArgType::Int, true, 0, 48, 0 }, kNoArg } },
    { McpTool::SetChargeLimit,   "set_charge_limit",
      "Set the charge limit in percent.",
      { { "percent", McpArgType::Int, true, 50, 100, 80 }, kNoArg } },
    { McpTool::SetScheduledCharging, "set_scheduled_charging",
      "Enable/disable daily scheduled charging; start_minutes = minutes after local midnight.",
      { { "enable",        McpArgType::Bool, true, 0, 0,    0 },
        { "start_minutes", McpArgType::Int,  false, 0, 1439, 0 } } },
};

// Registry lookup by tool enum (for the executor after mcp_tool_from resolved the name).
inline const McpToolInfo* mcp_tool_info(McpTool t) {
    for (const auto& ti : kMcpTools)
        if (ti.tool == t) return &ti;
    return nullptr;
}

inline McpTool mcp_tool_from(const char* name) {
    if (name) {
        for (const auto& t : kMcpTools)
            if (std::strcmp(name, t.name) == 0) return t.tool;
    }
    return McpTool::Unknown;
}

// Clamp a JSON number to an int range BEFORE the int cast — casting an out-of-range
// double to int is undefined behaviour, so a hostile {"amps":1e300} must never reach
// (int). Same rule as http_api.cpp's json_int_clamped; the cJSON unwrapping stays
// in mcp_server.cpp.
inline int clamped_int(double d, int lo, int hi) {
    if (d < (double)lo) return lo;
    if (d > (double)hi) return hi;
    return (int)d;
}

// Command-outcome text, shared verbatim by the MCP tools/call result and the REST
// /command reason so the two paths can never report the same outcome differently:
// success → fixed string; failure with a reply → the car's own reason; no reply →
// "vehicle not reachable".
inline const char* command_result_text(bool ok, const std::string& err) {
    if (ok) return "command executed successfully";
    return err.empty() ? "vehicle not reachable" : err.c_str();
}

}  // namespace tk
