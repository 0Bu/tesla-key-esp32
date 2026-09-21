#pragma once

#include <string>
#include <string_view>

// Pure, hardware-free command-outcome text, shared by the REST /command reason
// (http_api.cpp) and the MCP tools/call result (mcp_server.cpp) so the two paths can
// never report the same outcome differently: success → fixed string; failure with a
// reply → the car's own reason; no reply → "vehicle not reachable".
//
// LIFETIME: on the failure-with-reason path the returned pointer aliases `err` — the
// caller must keep `err` alive (a named local, not a temporary bound across statements)
// until the text has been consumed/copied.
namespace tk {

// Evaluates whether an error string represents a nominal already_set response from the vehicle
// (teslamotors/vehicle-command NominalError, treated as idempotent success by caller).
inline bool is_nominal_already_set(std::string_view err) noexcept {
    return err.find("already_set") != std::string_view::npos;
}

inline const char* command_result_text(bool ok, const std::string& err) {
    if (ok || is_nominal_already_set(err)) return "command executed successfully";
    return err.empty() ? "vehicle not reachable" : err.c_str();
}

}  // namespace tk
