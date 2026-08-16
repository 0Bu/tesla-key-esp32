#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Pure, hardware-free logic shared by the firmware and the host-side mock build
// (test/, built without ESP-IDF). Anything in this directory must stay free of IDF,
// FreeRTOS, NimBLE, NVS, cJSON and esp_http_server includes so it compiles with a
// plain host toolchain. See test/README.md and the project AGENTS.md.
namespace tk {

// What a diagnostic snapshot must NOT carry when it leaves the device — the ONE implementation
// of that rule, for `GET /status?redact=1` and `GET /diag?redact=1`.
//
// A bug report from this device is a paste of its own `/status` and `/diag`. Both are written for
// a trusted LAN and say so: `/status` is the web UI's live feed and legitimately shows the SSID
// and the broker, `/diag` is the console ring. Pasted into an issue tracker they stop being LAN
// data. Redaction is therefore OPT-IN per request — the dashboard keeps polling the unredacted
// form — and this header is the whole definition of what "redacted" means, so the browser button,
// a curl fallback and anything added later cannot each hold a slightly different answer. The copy
// that silently stops covering a newly-added field is the copy that leaks it.
//
// THE VIN IS THE SHARP ONE, and it has no counterpart in an ordinary device report. It names a
// specific car — one that can be located in a driveway — and it is a live input to Tesla's own
// APIs and to this device's whole BLE identity (the car's advert name is derived from it, so a
// captured advert name is the VIN in another dress). Nothing else here is in that class; treat a
// new field that touches the VIN the way this file treats the VIN itself.
//
// TWO SHAPES, because the two routes leak differently:
//
//   * /status leaks by FIELD. The contract lives in logic/status_model.hpp and is golden-pinned,
//     so redaction happens where each value is WRITTEN — the caller substitutes into the
//     status::Inputs fields it gathers, BEFORE emit_status() walks them — never as a
//     post-processing pass over the finished JSON. That is a memory rule, not a style
//     preference: a second pass needs a second full-size buffer, and on this device the binding
//     limit is the largest CONTIGUOUS free block (see the project AGENTS.md), so the pass is a
//     crash risk on exactly the request a user makes when something is already wrong.
//
//   * /diag leaks by LINE. A handful of log statements interpolate a VIN, an SSID, an IP, a
//     vehicle/nearby-device MAC or a host into free text. That is the non-trivial half, and the
//     table below is its allowlist. The physical board MAC is intentionally retained for triage.
//
// THE VALUE IS REPLACED, THE KEY IS KEPT — and on this device that is stronger than a matter of
// taste, because status_model.hpp's presence rules key on emptiness. `mqtt.broker` and
// `syslog.host` are emitted ONLY when non-empty, so clearing them instead of substituting would
// not merely drop a field: the report would positively state "no broker configured", and the
// reader would chase a configuration problem that does not exist. The same argument applies to
// an omitted key generally — it is indistinguishable from an older build that never had the
// field, and "which build produced this?" is the first question anyone asks of a frozen report.
//
// DELIBERATELY NOT REDACTED, because they identify the FIRMWARE or the fault rather than the
// reporter: `version`, `link`/`vcsec_sleep`, the heap and telemetry numbers, `wifi.rssi`,
// `syslog.port`, `last_reboot` — the point of the report is that those still answer. Nor
// `key_fingerprint`: it is a fingerprint of a PUBLIC key, it names no person, network or car,
// and it is the primary evidence in every pairing-lifecycle report (which key is on the car,
// whether it changed) — withholding it would cost the reports this exists to make possible. Nor
// `sys.board_mac`: it identifies the replaceable ESP32 hardware rather than the car or home
// network, and lets a board-swap report distinguish the old hardware from the new.

// The one replacement token. Readers of a report key on this exact string to tell "the reporter
// scrubbed it" apart from an empty/absent value (the device genuinely had nothing to say).
inline constexpr const char* kRedacted = "<redacted>";

// The /status values the caller substitutes. Listed here rather than only at the call sites so
// the set is reviewable in one place — this header cannot ENFORCE that a call site uses it (that
// stays a review point), but it can state what the set is:
//
//   vin           the car itself — see above
//   ip            this device's LAN address
//   wifi.ssid     the reporter's network name
//   ble.addr      the vehicle's BLE MAC (and ble.devices[].addr, which carries the same value
//                 for the target car plus the addresses of every other BLE device in the
//                 reporter's home — a second, broader leak riding on one array)
//   mqtt.broker   a LAN address ("host:port"), often naming the reporter's HA install
//   syslog.host   a LAN address
inline constexpr std::size_t kRedactedStatusFields = 6;

// Field-level substitution for the /status gather step. Returns by value because every caller
// feeds it straight into a std::string field of status::Inputs, which copies anyway.
inline std::string redact_or(const std::string& value, bool on) {
    return on ? std::string(kRedacted) : value;
}

// One diag-line rule: everything between the end of `marker` and the next `end` is replaced.
struct DiagRedaction {
    const char* marker;   // matched anywhere in the line; the value starts right after it
    const char* end;      // the value ends here (exclusive). Empty = run to the end of the line.
};

// The log statements that interpolate an identifier. Each entry is ONE ESP_LOGx() in the
// firmware and the comment names it, because a reworded log line silently stops matching and the
// only symptom is a leak nobody sees. diag_log.cpp captures the FORMATTED line, so a marker
// matches against esp_log's rendering — "I (12345) <tag>: <message>" — which is why some markers
// carry the tag: where the message phrase alone is too generic to be safe ("IP: ", "VIN: "), the
// tag is part of the match, and a TAG rename must be mirrored here.
inline constexpr DiagRedaction kDiagRedactions[] = {
    // main.cpp "VIN: %s  BLE MAC: %s  Board MAC: %s" — redact the car identifiers but retain
    // the physical board identity. Two rules preserve all three labels; if the ring truncates
    // before the Board-MAC delimiter, the second rule still fails closed to the line end.
    {"main: VIN: ", "  BLE MAC: "},
    {"  BLE MAC: ", "  Board MAC: "},
    // vehicle_ctrl.cpp "VehicleController ready for VIN %s"
    {"VehicleController ready for VIN ", ""},
    // http_api.cpp "CMD %s on VIN %s" — the marker starts AFTER the command name, so which
    // command ran survives. That half is the diagnostic content; the VIN is only routing.
    {" on VIN ", ""},
    // http_server.cpp "REQ: %s %s" — the request LOG line, whose second value is the raw URI.
    // Every evcc REST route embeds the VIN in its path (/api/1/vehicles/<VIN>/vehicle_data,
    // /api/1/vehicles/<VIN>/command/<cmd>), and evcc polls on a loop, so on a device doing its
    // job this is the single most FREQUENT line in the ring — measured at 31 of 286 lines on a
    // board eleven minutes after boot. It was the one VIN sink not covered here, which made the
    // whole `?redact=1` promise false for its primary user: the reader pastes a log they believe
    // is scrubbed and it names their car dozens of times.
    //
    // The end token is "/", so only the path segment holding the VIN is replaced and the route
    // that was called survives — which command or endpoint ran is the diagnostic content, the
    // VIN is only addressing. A truncated line with no closing "/" fails closed to the end of
    // the line, like every other rule here.
    {"/api/1/vehicles/", "/"},
    // vehicle_ctrl.cpp "Tesla MAC saved: %s"
    {"Tesla MAC saved: ", ""},
    // vehicle_ctrl.cpp "could not persist Tesla MAC %s — next boot rescans" — the SAME address on
    // the failure branch of the very same write. A separate rule because the prefix differs, and
    // that is the whole lesson: this table is keyed on log PHRASES, so a new phrase carrying an
    // old value is a silent leak. The failure branch arrived with the [[nodiscard]] NVS work and
    // no rule came with it. Rare (it needs an NVS write to fail) but the promise is all-or-nothing.
    // A MAC has no spaces, so the end token can keep the explanatory tail.
    {"could not persist Tesla MAC ", " — next boot"},
    // ble_client.cpp "Tesla '%s' found: %s — connecting". The FIRST value is the advert name,
    // which is derived from the VIN and is therefore as identifying as the VIN — it is not a
    // nickname. The second is the MAC. Two rules keep the "connecting" tail, which is what
    // distinguishes this line from a scan listing.
    //
    // An end token inside a value would cut a span short, and an advert name is nominally bytes
    // off the air — but this line is only reached after TeslaBLE::matches_vin() has accepted the
    // name, so it holds the car's own S<hash>C form over a fixed alphabet. Nothing arbitrary can
    // reach these two spans.
    {"Tesla '", "' found: "},
    {"' found: ", " — connecting"},
    // main.cpp "WiFi connected to '%s'" — deliberately NO end token even though the format ends
    // in a closing quote. An SSID is arbitrary bytes chosen by whoever runs the AP, so an SSID
    // containing a quote would end the span early and leak its own tail. Running to the end of
    // the line costs nothing here (the value is last) and cannot be gamed.
    {"WiFi connected to '", ""},
    // main.cpp "IP: " IPSTR — this device's own LAN address.
    {"main: IP: ", ""},
    // provisioning.cpp "saved config: ssid='%s' vin='%s' — rebooting" — BOTH values in one span,
    // for the SSID reason above (a quote inside the SSID would otherwise expose the VIN behind
    // it). The lost "— rebooting" tail costs nothing: the reboot is self-evident from the next
    // boot banner. The setup portal serves no /diag today, so this line reaches a report only if
    // that ever changes — one table entry is the cheap side of that bet.
    {"provisioning: saved config: ssid='", ""},
    // syslog.cpp "target set to %s:%d" — the port goes with it (no end token). /status carries
    // syslog.port, so nothing diagnostic is lost, and guessing where a host ends in a string
    // that may itself contain a colon is not worth the risk.
    {"syslog: target set to ", ""},
    // syslog.cpp "forwarding to %s (%s), reachable=%s" — host AND resolved IP in one span;
    // reachable= survives, which is the half that says whether the collector answers.
    {"syslog: forwarding to ", ", reachable="},
    // syslog.cpp "DNS lookup failed for %s (error %d)" — the errno is the diagnosis, keep it.
    {"syslog: DNS lookup failed for ", " (error"},
    // mqtt_ha.cpp "MQTT bridge started → %s (base topic %s, HA prefix %s)" — the first value is
    // s_broker_disp, the very string /status reports as mqtt.broker. Leaving it would have made
    // the redaction incoherent: scrubbed in the JSON, printed in the log a few sections below it
    // in the same report.
    //
    // The HA prefix deliberately SURVIVES, but the base topic now embeds the vehicle-stable VIN
    // node id. Scrub it just like the VIN itself; leaving the broker redacted but the car identity
    // visible in the same line would make a public bug report internally inconsistent.
    {"MQTT bridge started → ", " (base topic "},
    {" (base topic ", ", HA prefix "},
};
inline constexpr std::size_t kDiagRedactionCount = sizeof(kDiagRedactions) / sizeof(kDiagRedactions[0]);

// Two log lines are deliberately NOT in the table, and it is worth saying why so nobody "fixes"
// them: provisioning.cpp's "failed to persist setup form (ssid=%s pass=%s vin=%s)" interpolates
// the strings "ok"/"failed", never the values; main.cpp's "could not set DHCP hostname '%s'"
// interpolates a compile-time constant. A rule on either would redact a word that says nothing.

// Redact one /diag line. A line matching no rule is returned unchanged — the ring is mostly
// lifecycle and BLE chatter that names nothing.
//
// FAILS CLOSED: if the end token is not found (a line the ring truncated mid-value — which is
// exactly when a value is most likely to be sitting unterminated at the end), the redaction runs
// to the end of the line rather than giving up and leaving the value in place. The wrong answer
// here is asymmetric in the same way the rollback policy is: over-redacting costs a word of
// context, under-redacting costs the user's data, permanently and in public.
//
// A trailing newline is PRESERVED rather than swallowed by that fail-closed span, so the caller
// can hand lines over with or without their terminator and the ring's line structure survives
// either way — a /diag whose lines ran together would be redacted and unreadable, which is its
// own way of losing the report.
inline std::string redact_diag_line(std::string_view line) {
    std::string out(line);
    std::size_t limit = out.size();
    while (limit > 0 && (out[limit - 1] == '\n' || out[limit - 1] == '\r')) limit--;
    for (std::size_t i = 0; i < kDiagRedactionCount; i++) {
        const DiagRedaction& r = kDiagRedactions[i];
        std::string_view marker(r.marker);
        std::size_t start = out.find(marker);
        if (start == std::string::npos || start + marker.size() > limit) continue;
        start += marker.size();
        std::string_view end_tok(r.end);
        std::size_t stop = end_tok.empty() ? std::string::npos : out.find(end_tok, start);
        if (stop == std::string::npos || stop > limit) stop = limit;   // fail closed, keep the newline
        std::size_t before = stop - start;
        out.replace(start, before, kRedacted);
        limit = limit - before + std::string_view(kRedacted).size();
    }
    return out;
}

}  // namespace tk
