#pragma once

#include <string_view>

// Pure, hardware-free logic shared by the firmware and the host-side mock build
// (test/, built without ESP-IDF). Anything in this directory must stay free of IDF,
// FreeRTOS, NimBLE, NVS, cJSON and esp_http_server includes so it compiles with a
// plain host toolchain. See test/README.md and the project .claude/CLAUDE.md.
namespace tk {

// Captive-portal reply policy: what the ONE catch-all GET route answers with, plus the single
// place the portal's address is written.
//
// The setup AP (provisioning.cpp, "tesla-key-esp32-setup") is the device's only path back in
// when it has no WiFi credentials, and its whole usability rests on ONE behaviour: the sign-in
// sheet popping up by itself when a phone joins. Nobody types an IP address at a garage wall.
// That pop-up is not something the AP can request — it happens only if the joining OS's own
// connectivity probe gets the answer that OS keys on. Every OS probes a well-known URL over
// plain HTTP right after associating:
//
//   iOS/macOS  GET http://captive.apple.com/hotspot-detect.html      expects a body of "Success"
//   Android    GET http://connectivitycheck.gstatic.com/generate_204 expects 204 + empty body
//   Windows    GET http://www.msftconnecttest.com/connecttest.txt    expects "Microsoft Connect Test"
//
// provisioning.cpp's DNS task answers EVERY A query with the portal address, so all three probes
// land on the catch-all route. The catch-all then serves the setup PAGE with 200. That is not
// what any of the three agents is looking for — they are not browsers:
//
//   * A 302 with a Location header is the one signal all three understand as "you are behind a
//     portal". A 200 carrying a body is only a heuristic fallback, and Android additionally runs
//     a parallel HTTPS probe it cannot reach here, so a 200 can leave it undecided rather than
//     showing the sign-in prompt. The user then sees a joined-but-useless network and is told
//     nothing.
//   * The page is served pre-gzipped with an unconditional `Content-Encoding: gzip`. That drags
//     a compressed body onto a path walked by minimal HTTP clients rather than by the WebKit
//     view that later renders the portal. A REDIRECT has an empty body, which takes gzip off the
//     probe path entirely — while the real browser that follows the redirect still gets the
//     compressed page. Gzip has no place on a probe response; there is nothing to compress.
//
// So: in SETUP mode every unmatched GET redirects to the portal root, and only the portal root
// itself serves the page. In STA mode nothing changes — the catch-all is the normal page there,
// and turning a deep link into a redirect would break the web UI.
//
// Pure and host-testable rather than an `if` inside the handler, because the STA carve-out is
// the regression nobody would notice: a portal that stops popping gets reported within a day, a
// dashboard link that starts redirecting does not.
//
// NO FIRMWARE CALLER YET — provisioning.cpp's wildcard GET still answers 200 with the gzipped
// page for every path, and its DNS task still carries its own literal 192.168.4.1 in the answer
// template. This header is where that address and that decision belong once the reply is wired
// through it.

// The SoftAP's own address, in the two forms the firmware needs. Written ONCE here so the places
// that advertise it cannot drift apart: the DNS A-record answer and the AP's own address use the
// OCTETS, the HTTP Location header and an RFC 8910 DHCP option-114 payload (which recent
// iOS/Android prefer over probing at all) use the URI. A redirect pointing somewhere the DNS
// does not answer for is a dead end that nothing in the build would catch — the client simply
// hangs, and the report says "the portal doesn't open".
//
// The value is 192.168.4.1: IDF's default SoftAP subnet, which provisioning.cpp takes as-is (it
// sets no custom IP info) and already prints in its own "open http://192.168.4.1" boot line and
// hardcodes in its DNS answer bytes.
inline constexpr unsigned char CAPTIVE_PORTAL_OCTETS[4] = {192, 168, 4, 1};
inline constexpr const char*   CAPTIVE_PORTAL_IP        = "192.168.4.1";
inline constexpr const char*   CAPTIVE_PORTAL_URI       = "http://192.168.4.1/";

enum class CaptiveReply {
    Page,      // serve the HTML (the setup form in AP mode, the web UI in STA mode)
    Redirect,  // 302 -> CAPTIVE_PORTAL_URI, empty body (so: no gzip on this path)
};

// `setup_mode` = a SoftAP is live, i.e. this is the provisioning portal rather than the normal
// LAN surface. On this device that is unambiguous: provisioning_run() sets WIFI_MODE_AP and
// never returns, so the two HTTP surfaces are never both up.
//
// The portal ROOT stays a Page even though it is reachable as its own path: the catch-all should
// never see "/", but a redirect loop if it ever did would cost the user the only way into the
// device, which is far worse than this redundant check costs. Only GETs come here — the form's
// POST /save is a route of its own and must never be answered with a redirect, since a 302 would
// throw away the body the user just submitted.
inline CaptiveReply captive_reply_for(std::string_view path, bool setup_mode) {
    if (!setup_mode) return CaptiveReply::Page;
    if (path == "/" || path == "/index.html") return CaptiveReply::Page;
    return CaptiveReply::Redirect;
}

}  // namespace tk
