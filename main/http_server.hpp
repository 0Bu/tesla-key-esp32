#pragma once

#include "vehicle_ctrl.hpp"

// Starts the HTTP server and registers all handlers.
// vehicle must remain alive for the server lifetime.
bool http_server_start(VehicleController& vehicle);
