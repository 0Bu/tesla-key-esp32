// The ONE kind → VehicleController mapping both command surfaces execute through:
// REST POST /api/1/vehicles/{VIN}/command/{CMD} (http_api.cpp) and MCP tools/call
// (mcp_server.cpp). Each surface parses/validates its arguments against the shared
// spec table (logic/command_registry.hpp) into the positional value arrays, then
// dispatches here — so a command can never behave differently depending on which
// surface invoked it.

#include "http_handlers.hpp"

bool execute_vehicle_command(VehicleController& v, tk::CmdKind kind,
                             const int* ival, const bool* bval) {
    switch (kind) {
        case tk::CmdKind::WakeUp:          return v.wake_up();
        case tk::CmdKind::ChargeStart:     return v.charge_start();
        case tk::CmdKind::ChargeStop:      return v.charge_stop();
        case tk::CmdKind::ChargePortOpen:  return v.charge_port_open();
        case tk::CmdKind::ChargePortClose: return v.charge_port_close();
        case tk::CmdKind::SetChargingAmps: return v.set_charging_amps(ival[0]);
        case tk::CmdKind::SetChargeLimit:  return v.set_charge_limit(ival[0]);
        case tk::CmdKind::SetScheduledCharging:
            return v.set_scheduled_charging(bval[0], ival[1]);
        case tk::CmdKind::DoorLock:        return v.door_lock();
        case tk::CmdKind::DoorUnlock:      return v.door_unlock();
        case tk::CmdKind::FlashLights:     return v.flash_lights();
        case tk::CmdKind::HonkHorn:        return v.honk_horn();
        case tk::CmdKind::SetSentryMode:   return v.set_sentry_mode(bval[0]);
        case tk::CmdKind::ClimateStart:    return v.climate_start();
        case tk::CmdKind::ClimateStop:     return v.climate_stop();
        // Read-only tool — answered from the caches by its caller (mcp_server.cpp),
        // never dispatched as a vehicle command.
        case tk::CmdKind::GetVehicleState:
        case tk::CmdKind::Unknown:
            break;
    }
    return false;
}
