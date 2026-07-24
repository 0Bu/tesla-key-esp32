#pragma once

#include <cerrno>
#include <cstddef>
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

// What a send failure should do to the forwarding state machine (syslog.cpp handle_send_failure).
// The subtlety this pins down: on a hard error the immediate re-resolve+re-probe (clearing the
// throttle so getaddrinfo()+ping run NOW) must happen at most ONCE per outage — only on the first
// failure, before `send_failing` latches. Firing it on every hard failure re-runs getaddrinfo()+ping
// per queued line — the very probe storm syslog_error_is_hard's asymmetry exists to prevent, run
// hardest exactly when the link is worst. Subsequent hard failures still pause forwarding, but let
// the ordinary resolve cadence (check_interval) govern re-checks.
struct SendFailureActions {
    bool stop_forwarding;   // set resolved = false — the route/destination is implicated
    bool reprobe_once;      // clear have_checked + logged_state — ONE immediate re-resolve+re-probe
};

inline constexpr SendFailureActions syslog_send_failure_actions(bool hard, bool already_failing) {
    return { /*stop_forwarding*/ hard, /*reprobe_once*/ hard && !already_failing };
}

// RFC 5424 PRI for one captured log line: facility * 8 + severity, facility 1 (user).
//
// Every line used to ship as a hardcoded <14> (user.info). That made the collector's own
// severity field a constant: 211366 lines over a week in VictoriaLogs, all "info", including
// 61417 ESP_LOGE and 81708 ESP_LOGW ones — so `severity:error` matched nothing and the only
// way to find a fault was a substring match on `_msg:"E ("`. The level is already in hand:
// diag_log.cpp's capture hook sees the FORMATTED line, and esp_log renders the level as the
// first character ("E (12345) tag: message").
//
// Robustness rules, both load-bearing:
//   * Skip a leading ANSI colour escape. With CONFIG_LOG_COLORS=y (the IDF default) the line
//     starts "\033[0;31mE (…"; keying on line[0] would then classify every line as unknown
//     and silently reproduce the constant-severity bug this function exists to fix.
//   * Anything that is not a recognised "<L> (" prefix stays INFO. Plenty of forwarded lines
//     come from the tesla-ble library and NimBLE, which print without an esp_log prefix at
//     all ("Loaded private key from storage"). Guessing a severity for those would be worse
//     than the honest default — a wrong "error" is a false alarm someone has to chase.
inline constexpr int kSyslogFacilityUser = 1;

inline int syslog_severity_for_line(const char* text, size_t len) {
    constexpr int kInfo = 6;
    if (text == nullptr) return kInfo;

    size_t i = 0;
    // ESC '[' … 'm' — one CSI SGR sequence, bounded so a malformed line can't run off the end.
    if (len >= 2 && text[0] == '\033' && text[1] == '[') {
        i = 2;
        while (i < len && text[i] != 'm') i++;
        if (i < len) i++;   // step past the 'm'; if none was found we fall through to no match
    }

    // The level letter is only meaningful as the esp_log prefix "<L> (" — requiring the
    // space and paren keeps an ordinary sentence starting with "エ"/"Warning:"/"I " from
    // being read as a level.
    if (i + 2 >= len || text[i + 1] != ' ' || text[i + 2] != '(') return kInfo;

    switch (text[i]) {
        case 'E': return 3;   // err
        case 'W': return 4;   // warning
        case 'I': return kInfo;
        case 'D':
        case 'V': return 7;   // debug
        default:  return kInfo;
    }
}

inline int syslog_pri_for_line(const char* text, size_t len) {
    return kSyslogFacilityUser * 8 + syslog_severity_for_line(text, len);
}

}  // namespace tk
