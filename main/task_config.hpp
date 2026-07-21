#pragma once

// Central FreeRTOS task-priority table — the ONE place the firmware's relative task
// priorities are declared, so they are reviewable at a glance and the narrative task
// inventory in docs/ARCHITECTURE.md ("Concurrency") cannot drift from the code. Every
// xTaskCreate site takes its priority from here; stack sizes stay at the call sites,
// where each carries its own sizing rationale.
//
// Relative ordering (higher preempts lower):
//   5 — work that must not starve behind anything else of ours: the vehicle loop feeds
//       every consumer (evcc, UI, MQTT), the OTA tasks hold a TLS peer that times out,
//       and the setup-portal DNS must answer a phone's probe within its timeout.
//   4 — supervisors and bridge publishers: periodic, latency-tolerant, but expected to
//       run promptly when due.
//   3 — local reporting with relaxed deadlines (display frames, the one-shot OTA
//       health gate).
//   2 — pure cosmetics (status LED), below everything.
// The NimBLE host task and esp_http_server's task are created by ESP-IDF with their own
// Kconfig-set priorities and are not governed by this table (see the task inventory in
// docs/ARCHITECTURE.md).

#include "freertos/FreeRTOS.h"

namespace tk {

inline constexpr UBaseType_t kPrioVehicleLoop  = 5;  // vehicle_ctrl.cpp    background poll + sleep gating
inline constexpr UBaseType_t kPrioCaptiveDns   = 5;  // provisioning.cpp    setup-AP captive DNS (setup mode only)
inline constexpr UBaseType_t kPrioOta          = 5;  // ota_update.cpp      OTA download + flash (transient)
inline constexpr UBaseType_t kPrioOtaCheck     = 5;  // ota_update.cpp      OTA manifest check (transient)
inline constexpr UBaseType_t kPrioAutoPair     = 4;  // vehicle_pairing.cpp auto-pair supervisor
inline constexpr UBaseType_t kPrioWifiWatchdog = 4;  // main.cpp            ghost-association watchdog
inline constexpr UBaseType_t kPrioMqttPub      = 4;  // mqtt_ha.cpp         MQTT/HA publisher
inline constexpr UBaseType_t kPrioWsBroadcast  = 4;  // http_events.cpp     /events live-status push
inline constexpr UBaseType_t kPrioSyslog       = 3;  // syslog.cpp          UDP diagnostic forwarding
inline constexpr UBaseType_t kPrioDisplay      = 3;  // display.cpp         ST7735 renderer (C5/S3 only)
inline constexpr UBaseType_t kPrioOtaGate      = 3;  // main.cpp            one-shot OTA health gate
inline constexpr UBaseType_t kPrioLed          = 2;  // led_status.cpp      APA102 status LED (opt-in)

}  // namespace tk
