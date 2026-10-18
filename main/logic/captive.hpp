#pragma once

#include <cstddef>
#include <string_view>

// Pure, hardware-free logic shared by the firmware and the host-side mock build
// (test/, built without ESP-IDF). Anything in this directory must stay free of IDF,
// FreeRTOS, NimBLE, NVS, cJSON and esp_http_server includes so it compiles with a
// plain host toolchain. See test/README.md and the project AGENTS.md.
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
// dashboard link that starts redirecting does not. provisioning.cpp's catch-all GET calls
// captive_reply_for(), and its DNS task answers through build_captive_dns_reply() below, so the
// address below is the single source for the DNS answer, the DHCP option and the Location header.

// The SoftAP's own address, in the two forms the firmware needs. Written ONCE here so the places
// that advertise it cannot drift apart: the DNS A-record answer and the AP's own address use the
// OCTETS, the HTTP Location header and an RFC 8910 DHCP option-114 payload (which recent
// iOS/Android prefer over probing at all) use the URI. A redirect pointing somewhere the DNS
// does not answer for is a dead end that nothing in the build would catch — the client simply
// hangs, and the report says "the portal doesn't open".
//
// The value is 192.168.4.1: IDF's default SoftAP subnet, which provisioning.cpp takes as-is (it
// sets no custom IP info) and prints in its own "open http://192.168.4.1" boot line.
inline constexpr unsigned char CAPTIVE_PORTAL_OCTETS[4] = {192, 168, 4, 1};
inline constexpr const char*   CAPTIVE_PORTAL_IP        = "192.168.4.1";
inline constexpr const char*   CAPTIVE_PORTAL_URI       = "http://192.168.4.1/";

enum class CaptiveReply {
    Page,      // serve the HTML (the setup form in AP mode, the web UI in STA mode)
    Redirect,  // 302 -> CAPTIVE_PORTAL_URI, empty body (so: no gzip on this path)
};

// `setup_mode` = a SoftAP is live, i.e. this is the provisioning portal rather than the normal
// LAN surface. On this device that is unambiguous: the portal's HTTP server is stopped before
// the boot continues to the normal web server, so the two HTTP surfaces are never both up.
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

// Captive DNS reply (F-079). The portal's DNS task answers every name with the portal address so
// a joining phone's connectivity probe lands on the setup page. The reply is BUILT, not patched
// into the query buffer: echoing the query and appending an answer would leave any additional
// section (an EDNS OPT record) in front of it, so a client would read OPT as the answer.
//
// Accepted: a standard query (QR=0, OPCODE=0) with exactly one question, no answer or authority
// records, any number of additional records (dropped from the reply), an uncompressed name of at
// most 255 bytes and class IN. Type A or ANY gets one A answer; any other type (e.g. AAAA) gets
// an empty NOERROR answer, so the client falls back to A instead of waiting. Anything else is
// dropped (return 0). The reply sets QR, keeps RD, sets RA and RCODE 0, and never exceeds
// kCaptiveDnsReplyMax bytes.
inline constexpr std::size_t kCaptiveDnsHeader   = 12;
inline constexpr std::size_t kCaptiveDnsReplyMax = kCaptiveDnsHeader + 255 + 4 + 16;

inline std::size_t build_captive_dns_reply(const unsigned char* query, std::size_t len, unsigned char* out,
                                           std::size_t cap) noexcept {
    if (!query || !out || len < kCaptiveDnsHeader + 5 || cap < kCaptiveDnsReplyMax) return 0;
    auto u16 = [&](std::size_t at) { return static_cast<unsigned>(query[at] << 8 | query[at + 1]); };
    if ((query[2] & 0x80) != 0 || (query[2] & 0x78) != 0) return 0;  // a response, or not OPCODE 0
    if (u16(4) != 1 || u16(6) != 0 || u16(8) != 0) return 0;          // one question, nothing else
    std::size_t at = kCaptiveDnsHeader;
    for (;;) {
        if (at >= len) return 0;
        const unsigned label = query[at];
        if (label == 0) break;
        if (label > 63) return 0;  // compression pointers and reserved label types
        at += 1 + label;
        if (at - kCaptiveDnsHeader > 254) return 0;
    }
    const std::size_t question_end = at + 1 + 4;  // root label, QTYPE, QCLASS
    if (question_end > len) return 0;
    const unsigned qtype  = u16(at + 1);
    const unsigned qclass = u16(at + 3);
    if (qclass != 1) return 0;
    const bool answer_a = qtype == 1 || qtype == 255;

    for (std::size_t i = 0; i < question_end; ++i) out[i] = query[i];
    out[2] = static_cast<unsigned char>(0x80 | (query[2] & 0x01));  // QR, OPCODE 0, AA 0, TC 0, RD kept
    out[3] = 0x80;                                                  // RA, Z 0, RCODE 0
    out[6] = 0x00; out[7] = answer_a ? 0x01 : 0x00;                 // ANCOUNT
    out[8] = out[9] = out[10] = out[11] = 0x00;                     // NSCOUNT, ARCOUNT
    if (!answer_a) return question_end;
    const unsigned char answer[] = {
        0xC0, 0x0C,              // name: pointer to the question at offset 12
        0x00, 0x01, 0x00, 0x01,  // type A, class IN
        0x00, 0x00, 0x00, 0x3C,  // TTL 60 s
        0x00, 0x04,              // RDLENGTH 4
        CAPTIVE_PORTAL_OCTETS[0], CAPTIVE_PORTAL_OCTETS[1], CAPTIVE_PORTAL_OCTETS[2], CAPTIVE_PORTAL_OCTETS[3],
    };
    for (std::size_t i = 0; i < sizeof answer; ++i) out[question_end + i] = answer[i];
    return question_end + sizeof answer;
}

}  // namespace tk
