#pragma once

#include "nvs_storage.hpp"

// Starts a SoftAP "tesla-key-esp32-setup" + captive config portal so WiFi/VIN can be
// entered from a phone without recompiling. On save it writes wifi_ssid/wifi_pass/
// vin into the given config store (namespace "tesla_cfg") and reboots.
//
// This function NEVER returns — it is the terminal state until the user reboots
// the device by saving the form. Call it instead of connecting to WiFi.
void provisioning_run(NvsStorageAdapter& config_store);
