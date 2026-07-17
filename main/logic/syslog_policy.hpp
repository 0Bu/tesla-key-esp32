#pragma once

#include <cerrno>
#include <cstdlib>
#include <string>

// Pure, hardware-free logic shared by the firmware and the host-side mock build
// (test/, built without ESP-IDF). Anything in this directory must stay free of IDF,
// FreeRTOS, NimBLE, NVS, cJSON and esp_http_server includes so it compiles with a
// plain host toolchain. See test/README.md and the project CLAUDE.md.
namespace tk {

// Parse a web-UI "host:port" syslog target (e.g. "192.168.1.22:514") into its parts.
// A bare host with no ':' defaults to port 514. Returns false (host/port left
// untouched) for an empty string or anything malformed — callers treat "" as
// "syslog disabled", not a parse error.
inline bool syslog_target_parse(const std::string& raw, std::string& host, int& port) {
    if (raw.empty()) return false;
    size_t colon = raw.rfind(':');
    if (colon == std::string::npos) {
        host = raw;
        port = 514;
        return !host.empty();
    }
    host = raw.substr(0, colon);
    std::string port_s = raw.substr(colon + 1);
    if (host.empty() || port_s.empty() || port_s.size() > 5) return false;
    for (char c : port_s) if (c < '0' || c > '9') return false;
    long p = strtol(port_s.c_str(), nullptr, 10);
    if (p < 1 || p > 65535) return false;
    port = static_cast<int>(p);
    return true;
}

// Validate a POST /set_syslog submission. Empty disables Syslog (always valid);
// otherwise the value must parse via syslog_target_parse above — same rule the web
// UI enforces client-side (main/www/app.js), kept here so the authoritative check is
// host-tested. No scheme is accepted (syslog is UDP, not a URI-style broker).
inline bool syslog_target_is_plausible(const std::string& raw) {
    if (raw.empty()) return true;
    if (raw.size() > 120) return false;
    if (raw.find_first_of(" \t\r\n") != std::string::npos) return false;
    std::string host;
    int port = 0;
    return syslog_target_parse(raw, host, port);
}

// True when `err` (an errno from sendto()/socket()) indicts the destination or the
// route, so re-resolving DNS and re-probing the collector is worth doing now rather
// than at the next resolve-cadence tick.
//
// A send failure says one of two very different things:
//   HARD      — the destination or the route is the problem (network unreachable,
//               host down, our address gone). Re-resolving could plausibly help.
//   TRANSIENT — the stack momentarily couldn't take the datagram (out of memory/
//               pbufs, would block, interrupted). The destination is fine; a fresh
//               getaddrinfo() wouldn't change anything.
//
// Defaulting the unknown case to TRANSIENT is deliberate and cheap: clearing the
// throttle only *accelerates* the next resolve (the ordinary cadence still runs
// regardless), so mis-classifying a hard error costs at most one cadence of delay,
// while mis-classifying a transient one as hard would re-run getaddrinfo()+ping on
// every failing send — a storm that runs hardest exactly when the link is worst
// (e.g. a chatty failure mode logging several lines a second). The asymmetry is the
// whole point.
inline bool syslog_error_is_hard(int err) {
    switch (err) {
        case ENETUNREACH:     // no route to that network
        case EHOSTUNREACH:    // no route to that host
        case ENETDOWN:        // our interface went down
        case EHOSTDOWN:       // the peer is down
        case EADDRNOTAVAIL:   // our source address is gone (lease lost)
            return true;
        default:
            // ENOMEM / ENOBUFS (no pbufs), EAGAIN / EWOULDBLOCK, EINTR, and anything
            // unrecognised: the collector is not implicated. Hold the destination,
            // keep the throttle, let the cadence re-check.
            return false;
    }
}

}  // namespace tk
