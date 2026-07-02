#pragma once

#include <string>

// Pure, hardware-free command-outcome text, shared by the REST /command reason
// (http_api.cpp) and the MCP tools/call result (mcp_server.cpp) so the two paths can
// never report the same outcome differently: success → fixed string; failure with a
// reply → the car's own reason; no reply → "vehicle not reachable".
//
// LIFETIME: on the failure-with-reason path the returned pointer aliases `err` — the
// caller must keep `err` alive (a named local, not a temporary bound across statements)
// until the text has been consumed/copied.
namespace tk {

inline const char* command_result_text(bool ok, const std::string& err) {
    if (ok) return "command executed successfully";
    return err.empty() ? "vehicle not reachable" : err.c_str();
}

}  // namespace tk
