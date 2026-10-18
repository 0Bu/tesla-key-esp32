#pragma once

#include "nvs_storage.hpp"

// Starts a SoftAP "tesla-key-esp32-setup" + captive config portal so WiFi/VIN can be
// entered from a phone without recompiling. On save it writes wifi_ssid/wifi_pass/
// vin into the given config store (namespace "tesla_cfg") and reboots.
//
// provisioning_run() NEVER returns — it is the terminal state of first-time setup until the
// user reboots the device by saving the form. Call it instead of connecting to WiFi.
[[noreturn]] void provisioning_run(NvsStorageAdapter& config_store);

// The setup portal after a boot-time WiFi failure WITH stored credentials (#386). The portal joins
// the running station as APSTA and retries the stored credentials in the background
// (logic/setup_portal.hpp): no attempt while a phone is on the setup AP, the open AP closes after
// kPortalIdleCloseS without a client and at the latest kPortalApMaxOpenS after opening, and saving
// the form still reboots. Returns once the network is up, with the portal torn down, so the boot
// continues in place. Never reboots on its own. Call only after net_start_wifi() failed; when it
// never started the station (rejected credentials), this is provisioning_run() and never returns.
void provisioning_run_with_retry(NvsStorageAdapter& config_store);
