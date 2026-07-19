#pragma once

#include <cstring>

#include "command_registry.hpp"

// Pure, hardware-free core of the MCP endpoint (POST /mcp — see main/mcp_server.cpp):
// protocol-version negotiation, JSON-RPC method routing, and the integer-argument
// clamp. The tool registry itself lives in logic/command_registry.hpp — ONE table
// shared with the REST /command dispatch (http_api.cpp), so the two surfaces can never
// disagree about names or argument bounds. Everything IDF/cJSON-coupled stays in
// mcp_server.cpp; keeping the decisions here lets the host mock build (test/) verify
// them without a board.
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

// The MCP tool registry was here (enum McpTool + kMcpTools). It is now the MCP-visible
// half of tk::kCommands in logic/command_registry.hpp: rows with a non-null mcp_name are
// exactly the Charging-Manager command set plus the read-only get_vehicle_state; the
// role-refused commands carry mcp_name == nullptr and never reach tools/list (the car
// rejects them for this key role, so advertising them would only mislead the calling
// model). Descriptions stay terse on purpose — tools/list is the endpoint's largest
// response and cJSON prints it into ONE contiguous heap block (see CLAUDE.md).

// Clamp a JSON number to an int range BEFORE the int cast — casting an out-of-range
// double to int is undefined behaviour, so a hostile {"amps":1e300} must never reach
// (int). Same rule as http_api.cpp's json_int_clamped; the cJSON unwrapping stays
// in mcp_server.cpp. NaN falls through BOTH comparisons (NaN compares false), so it
// must be caught explicitly — the string-argument path parses with strtod, which
// accepts "nan"; without this check {"amps":"nan"} would reach the (int) cast.
inline int clamped_int(double d, int lo, int hi) {
    if (d != d) return lo;   // NaN → the safe bound, never the cast
    if (d < (double)lo) return lo;
    if (d > (double)hi) return hi;
    return (int)d;
}

}  // namespace tk
