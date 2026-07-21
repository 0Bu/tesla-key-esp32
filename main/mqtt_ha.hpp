#pragma once

#include <string>

class VehicleController;
class NvsStorageAdapter;

// ─── Home Assistant MQTT bridge ───────────────────────────────────────────────
// Publishes every cached telemetry value + device status to an MQTT broker using
// Home Assistant's MQTT-Discovery convention, so all entities auto-appear in HA
// grouped under one device. Read-only: no command topics are subscribed (the car
// is never controlled or woken from HA — see the "telemetry only" design choice).
//
// Broker config is resolved at start: NVS key "mqtt_uri" (set from the web UI)
// overrides CONFIG_TESLA_MQTT_BROKER_URI. If no broker is configured the bridge
// stays disabled and mqtt_ha_start() is a no-op.

// Start the bridge (call after WiFi is up). Spawns its own publisher task and returns
// true; unconfigured MQTT is a successful no-op. Returns false after cleaning up a
// configured bridge that could not be started.
bool mqtt_ha_start(VehicleController& vehicle, NvsStorageAdapter& config_store);

// Status accessors for GET /status (the web-UI "Connection" block).
bool        mqtt_ha_configured();  // a broker URI is set (bridge enabled)
bool        mqtt_ha_connected();   // a live MQTT session is up
bool        mqtt_ha_tls();         // the connection uses mqtts:// (TLS)
std::string mqtt_ha_broker();      // "host:port" shown in /status ("" if unconfigured)
std::string mqtt_ha_last_error();  // last connection error ("" when none / connected)
