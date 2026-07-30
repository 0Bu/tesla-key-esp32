#pragma once

// ONE command table for BOTH HTTP command surfaces — the evcc-facing REST route
// (POST /api/1/vehicles/{VIN}/command/{CMD}, http_api.cpp) and the MCP tools
// (tools/list + tools/call, mcp_server.cpp). Each row carries the REST name, the MCP
// tool name (nullptr = deliberately not a tool: the car refuses these for the
// Charging-Manager key role, and advertising them would only mislead a calling model),
// and the argument specs. An argument's REST key and MCP key may differ
// (TeslaBleHttpProxy compat says "charging_amps"; the MCP tool says "amps") but its
// {lo,hi} bounds are ONE pair of struct members read by the REST clamp, the MCP clamp
// AND the advertised tools/list JSON schema — so the surfaces can never disagree about
// a range again by construction. IDF-free; host-tested in test/test_logic.cpp.
//
// Surface semantics stay deliberately different and are encoded per-surface:
//   REST is generally LENIENT for protocol compatibility, but safety-critical
//   arguments may set api_required (set_charging_amps does): absent or malformed
//   input must not silently turn into a different vehicle command.
//   MCP is STRICT: an absent REQUIRED arg or a present-but-unparseable one is a
//   -32602 protocol error (silently defaulting set_scheduled_charging's "enable"
//   would DISABLE the schedule and report success).

#include <cstring>

namespace tk {

// Every dispatchable command kind across both surfaces, plus the MCP-only read tool.
// The executor mapping kind → VehicleController call lives once, device-side
// (execute_vehicle_command, main/command_exec.cpp).
enum class CmdKind {
    GetVehicleState,   // MCP-only, read-only, cache-only
    WakeUp,
    ChargeStart,
    ChargeStop,
    ChargePortOpen,
    ChargePortClose,
    SetChargingAmps,
    SetChargeLimit,
    SetScheduledCharging,
    // REST-only from here: accepted for TeslaBleHttpProxy API compatibility, but the
    // Charging-Manager key's role can't run them (car answers "authentication failed").
    DoorLock,
    DoorUnlock,
    FlashLights,
    HonkHorn,
    SetSentryMode,
    ClimateStart,
    ClimateStop,
    Unknown,
};

// Parsed REST request-body shape. TeslaBleHttpProxy-compatible callers normally send
// objects (or no body), but evcc's generic boolean setter serializes chargeEnable as the
// JSON scalar true/false. Keep that exception command-specific: accepting arbitrary
// scalars for argument-bearing commands would bypass their object-key validation.
enum class RestBodyShape {
    Empty,
    Object,
    BoolFalse,
    BoolTrue,
    Other,
};

inline constexpr bool rest_body_allowed(CmdKind kind, RestBodyShape shape) {
    if (shape == RestBodyShape::Empty || shape == RestBodyShape::Object) return true;
    return (kind == CmdKind::ChargeStart && shape == RestBodyShape::BoolTrue) ||
           (kind == CmdKind::ChargeStop && shape == RestBodyShape::BoolFalse);
}

enum class CmdArgType { None, Int, Bool };

struct CmdArg {
    const char* api_key;       // REST body key; nullptr = arg not on the REST surface
    const char* mcp_key;       // MCP arguments key; nullptr = arg not on the MCP surface
    CmdArgType  type;
    bool        api_required;  // REST: absent/malformed body is HTTP 400 when true
    bool        mcp_required;  // MCP: absent argument is JSON-RPC -32602 when true
    int         api_default;   // REST fallback when absent (Int; must lie in [lo,hi])
    int         lo, hi;        // Int only: THE shared inclusive clamp/schema bounds
};

inline constexpr CmdArg kNoCmdArg{ nullptr, nullptr, CmdArgType::None, false, false, 0, 0, 0 };

// Maximum arguments per command — sizes the spec array and both executors' value
// arrays; widening it in one place widens the whole pipeline.
inline constexpr int kCmdMaxArgs = 2;

struct CmdInfo {
    CmdKind     kind;
    const char* api_name;   // REST /command/{CMD} name; nullptr = not a REST command
    const char* mcp_name;   // MCP tool name; nullptr = not an MCP tool
    const char* mcp_desc;   // tools/list description (terse ON PURPOSE — tools/list is
                            // the MCP endpoint's largest single cJSON print, see CLAUDE.md)
    CmdArg      args[kCmdMaxArgs];   // kNoCmdArg-padded, positional (values arrays match)
};

// Row order is load-bearing for the wire: tools/list emits the MCP-visible rows in
// TABLE ORDER, and this order reproduces the pre-registry kMcpTools output exactly.
inline constexpr CmdInfo kCommands[] = {
    { CmdKind::GetVehicleState, nullptr, "get_vehicle_state",
      "Read cached vehicle state (VIN, link state, SOC, charging). Never wakes the car.",
      { kNoCmdArg, kNoCmdArg } },
    { CmdKind::WakeUp,          "wake_up",           "wake_up",           "Wake the vehicle over BLE.",
      { kNoCmdArg, kNoCmdArg } },
    { CmdKind::ChargeStart,     "charge_start",      "charge_start",      "Start charging.",
      { kNoCmdArg, kNoCmdArg } },
    { CmdKind::ChargeStop,      "charge_stop",       "charge_stop",       "Stop charging.",
      { kNoCmdArg, kNoCmdArg } },
    { CmdKind::ChargePortOpen,  "charge_port_door_open",  "charge_port_open",  "Open the charge port door.",
      { kNoCmdArg, kNoCmdArg } },
    { CmdKind::ChargePortClose, "charge_port_door_close", "charge_port_close", "Close the charge port door.",
      { kNoCmdArg, kNoCmdArg } },
    { CmdKind::SetChargingAmps, "set_charging_amps", "set_charging_amps",
      "Set the charging current in amps.",
      { { "charging_amps", "amps", CmdArgType::Int, true, true, 0, 0, 48 }, kNoCmdArg } },
    { CmdKind::SetChargeLimit,  "set_charge_limit",  "set_charge_limit",
      "Set the charge limit in percent.",
      { { "percent", "percent", CmdArgType::Int, false, true, 80, 50, 100 }, kNoCmdArg } },
    { CmdKind::SetScheduledCharging, "set_scheduled_charging", "set_scheduled_charging",
      "Enable/disable daily scheduled charging; start_minutes = minutes after local midnight.",
      { { "enable",        "enable",        CmdArgType::Bool, false, true,  0, 0, 0 },
        { "start_minutes", "start_minutes", CmdArgType::Int,  false, false, 0, 0, 1439 } } },
    // ── REST-only (role-refused; kept for API compatibility, absent from tools/list) ──
    { CmdKind::DoorLock,     "door_lock",     nullptr, nullptr, { kNoCmdArg, kNoCmdArg } },
    { CmdKind::DoorUnlock,   "door_unlock",   nullptr, nullptr, { kNoCmdArg, kNoCmdArg } },
    { CmdKind::FlashLights,  "flash_lights",  nullptr, nullptr, { kNoCmdArg, kNoCmdArg } },
    { CmdKind::HonkHorn,     "honk_horn",     nullptr, nullptr, { kNoCmdArg, kNoCmdArg } },
    { CmdKind::SetSentryMode, "set_sentry_mode", nullptr, nullptr,
      { { "on", nullptr, CmdArgType::Bool, false, false, 0, 0, 0 }, kNoCmdArg } },
    { CmdKind::ClimateStart, "auto_conditioning_start", nullptr, nullptr, { kNoCmdArg, kNoCmdArg } },
    { CmdKind::ClimateStop,  "auto_conditioning_stop",  nullptr, nullptr, { kNoCmdArg, kNoCmdArg } },
};

inline const CmdInfo* cmd_from_api_name(const char* name) {
    if (name) {
        for (const auto& c : kCommands)
            if (c.api_name && std::strcmp(name, c.api_name) == 0) return &c;
    }
    return nullptr;
}

inline const CmdInfo* cmd_from_mcp_name(const char* name) {
    if (name) {
        for (const auto& c : kCommands)
            if (c.mcp_name && std::strcmp(name, c.mcp_name) == 0) return &c;
    }
    return nullptr;
}

// Registry row by kind (tests, schema builder).
inline const CmdInfo* cmd_info(CmdKind k) {
    for (const auto& c : kCommands)
        if (c.kind == k) return &c;
    return nullptr;
}

}  // namespace tk
