#pragma once

#include "nvs_storage.hpp"

// Starts a SoftAP "tesla-key-esp32-setup" + captive config portal so WiFi/VIN can be
// entered from a phone without recompiling. Save updates one verified ConfigBlob in tesla_cfg,
// preserving service settings, and reboots after a confirmed save. An unreadable snapshot is refused.
//
// provisioning_run() NEVER returns — it is the terminal state of first-time setup until the
// user reboots the device by saving the form. Call it instead of connecting to WiFi.
[[noreturn]] void provisioning_run(NvsStorageAdapter& config_store);

// The setup portal after a boot-time WiFi failure WITH stored credentials (#386). The portal joins
// the running station as APSTA and retries stored credentials (logic/setup_portal.hpp). Before the
// open-time cap, this policy defers new attempts while an AP client is present; station-event retries
// remain independent. AP closure is requested at the idle/open-time limits and retried with backoff
// on failure. Saving the form still reboots. Returns once the network is up and the portal torn down,
// so boot continues in place. Never reboots on its own. Call only after net_start_wifi() failed; when it
// never started the station (rejected credentials), this is provisioning_run() and never returns.
void provisioning_run_with_retry(NvsStorageAdapter& config_store);
