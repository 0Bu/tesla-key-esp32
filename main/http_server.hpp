#pragma once

#include "vehicle_ctrl.hpp"
#include "nvs_storage.hpp"

// Starts the HTTP server and registers all handlers.
// vehicle and config_store must remain alive for the server lifetime. config_store is the
// runtime-config NVS namespace ("tesla_cfg"): /set_vin, /set_mqtt, /set_syslog and /set_wifi
// update its atomic `cfg` blob, while /set_time updates the separate `last_time` key. Config
// persistence is storage behaviour, not vehicle behaviour, so it does not route through
// VehicleController.
bool http_server_start(VehicleController& vehicle, NvsStorageAdapter& config_store);
