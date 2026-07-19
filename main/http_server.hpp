#pragma once

#include "vehicle_ctrl.hpp"
#include "nvs_storage.hpp"

// Starts the HTTP server and registers all handlers.
// vehicle and config_store must remain alive for the server lifetime. config_store is the
// runtime-config NVS namespace ("tesla_cfg"): the /set_vin, /set_mqtt and /set_time handlers
// persist through it directly — config persistence is storage behaviour, not vehicle
// behaviour, so it does not route through VehicleController.
bool http_server_start(VehicleController& vehicle, NvsStorageAdapter& config_store);
