#pragma once

#include <cstddef>
#include <string>

class NvsStorageAdapter;

// UDP Syslog forwarder (RFC 5424, best-effort). Every line the in-RAM diag log
// captures (diag_log.cpp's esp_log hook) is queued here and forwarded to a
// configured collector — a network home for /diag once the ring wraps or the
// device reboots. See docs/ARCHITECTURE.md.

struct SyslogStatus {
    bool        configured;  // "syslog_uri" (NVS/Kconfig) is set
    bool        resolved;    // DNS resolved -> a destination is known, lines are being forwarded
    bool        reachable;   // ADVISORY: last ARP/ICMP probe answered. Never gates delivery —
                              // syslog is best-effort UDP, and a healthy collector routinely
                              // firewalls ICMP — this is a /status hint only.
    std::string host;
    int         port;
    std::string error;
};

// Start the forwarder task. Reads NVS "syslog_uri" (web UI: Connections -> Syslog,
// POST /set_syslog), falling back to CONFIG_TESLA_SYSLOG_SERVER. "" disables
// forwarding (the task idles, draining and dropping whatever is queued). Config is
// resolved once here, at boot — like the MQTT bridge, /set_syslog reboots to apply
// a change, so there is nothing to re-read at runtime. Call once, early (before
// WiFi), from app_main; safe to call before the network stack is up. Unconfigured
// Syslog is a successful no-op; false means configured forwarding could not be started.
bool syslog_start(NvsStorageAdapter& config_store);

// Queue one already-formatted line for forwarding. Non-blocking (drops silently on
// a full queue or before syslog_start() has run) so a stuck collector can never
// stall the caller. Called from diag_log.cpp's esp_log capture hook.
void syslog_send(const char* msg, size_t len);

// Snapshot for GET /status (the web-UI Connections card).
SyslogStatus syslog_status();
