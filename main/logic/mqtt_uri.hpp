#pragma once
// The ONE place that turns a saved broker string into the URI the client actually dials, plus the
// rules the save-time pre-flight is decided by. Pure, IDF-free, host-tested.
//
// WHY THIS IS ONE TABLE AND NOT TWO. `/set_mqtt` now CONNECTS to the broker before it persists it
// (http_config.cpp) and the bridge connects to it at boot (mqtt_ha.cpp). If those two derived the
// URI separately they would eventually derive it differently — and the failure would be the worst
// shape available for a test-before-persist endpoint: a probe that succeeds against something the
// bridge never dials, i.e. a green check followed by a broker that does not connect. The scheme
// rule in particular is not obvious enough to retype (see mqtt_effective_uri): it silently decides
// whether the password crosses the network in the clear.
//
// The plausibility check moved here from http_config.cpp for the same reason it belongs in
// main/logic/ at all — it is a decision with edge cases (port range, userinfo, an explicit scheme)
// and it was previously only checkable on the device.
#include <cstddef>
#include <string>

namespace tk {

// Surrounding whitespace, as every entry path trims it: the web UI form, a curl body and the NVS
// value read back at boot must all agree on what "the same broker" means, or an unchanged value
// looks changed and reboots the device.
inline std::string mqtt_trim(const std::string& in) {
    const size_t s = in.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return {};
    const size_t e = in.find_last_not_of(" \t\r\n");
    return in.substr(s, e - s + 1);
}

// Is this a broker value we are willing to store? Empty is fine (it DISABLES MQTT); otherwise it
// must be host:port — a non-empty host, a ':' separator, and a numeric port in 1..65535. An
// optional "scheme://" prefix is tolerated, and so is "user:pass@host:port" userinfo, because
// rfind(':') takes the LAST colon and the port is what follows it.
inline bool mqtt_broker_is_plausible(const std::string& broker) {
    if (broker.empty()) return true;                  // empty = disable
    if (broker.size() > 120) return false;
    if (broker.find_first_of(" \t\r\n") != std::string::npos) return false;

    std::string authority = broker;
    const size_t scheme = authority.find("://");
    if (scheme != std::string::npos) authority = authority.substr(scheme + 3);

    const size_t colon = authority.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;   // need host:port
    const std::string host = authority.substr(0, colon);
    const std::string port = authority.substr(colon + 1);
    if (host.empty() || port.empty() || port.size() > 5) return false;
    for (char c : port) if (c < '0' || c > '9') return false;
    unsigned p = 0;
    for (char c : port) p = p * 10u + static_cast<unsigned>(c - '0');
    return p >= 1 && p <= 65535;
}

// The URI the client dials, from the stored broker string.
//
// THE SCHEME RULE, which is the part worth having in one place: the web UI keeps the simple
// "host:port" form, so most entries arrive without a scheme. When credentials are present we
// default to TLS rather than plaintext, because an MQTT CONNECT carries username and password in
// the clear on plain mqtt — and a credentialed broker is exactly the one that tends to live off the
// trusted LAN (a cloud, VLAN or VPN broker in the usual Home Assistant setup). Credentials are
// "present" if a username is configured or the authority embeds userinfo. A bare, credential-free
// local broker stays on plaintext. An explicit scheme is always honoured, in either direction.
inline std::string mqtt_effective_uri(const std::string& broker, bool have_username) {
    const std::string b = mqtt_trim(broker);
    if (b.empty()) return {};
    if (b.find("://") != std::string::npos) return b;
    const bool has_creds = have_username || b.find('@') != std::string::npos;
    return (has_creds ? "mqtts://" : "mqtt://") + b;
}

// Credential-free authority for status, UI and logs. The stored URI remains untouched for the
// MQTT client; only human-readable surfaces use this projection. `rfind('@')` is intentional:
// userinfo may contain percent escapes or unusual characters, while the final '@' separates it
// from the host. A path is not part of the broker identity shown to the user.
inline std::string mqtt_broker_display(const std::string& uri) {
    std::string authority = mqtt_trim(uri);
    const size_t scheme = authority.find("://");
    if (scheme != std::string::npos) authority.erase(0, scheme + 3);
    const size_t slash = authority.find('/');
    if (slash != std::string::npos) authority.erase(slash);
    const size_t userinfo = authority.rfind('@');
    if (userinfo != std::string::npos) authority.erase(0, userinfo + 1);
    return authority;
}

// Does this URI open a TLS session? Decides both the CA bundle attachment and the pre-flight's
// memory budget below.
inline bool mqtt_uri_is_tls(const std::string& uri) {
    return uri.rfind("mqtts://", 0) == 0;
}

// ── save-time pre-flight ────────────────────────────────────────────────────────────────────────
// What the probe may cost, and when it must refuse to run.
//
// WHY THERE IS A MEMORY GATE AT ALL. The probe opens a SECOND mqtt client beside the live bridge's
// own. On TLS that is a full mbedTLS session — roughly a 16 KB input buffer and a 4 KB output
// buffer, each ONE contiguous block — on a device whose binding limit is the largest contiguous
// free block and whose steady state is a few tens of KB. Attempting it on a fragmented heap is not
// a failed probe, it is a bad_alloc on the HTTP task, which is the exact class of crash this
// firmware spends the most effort avoiding. So the probe asks first, and a device that cannot
// afford it answers 503 WITHOUT persisting: the user retries, and nothing was silently half-done.
//
// The thresholds are deliberately well above the raw buffer sizes. mbedTLS allocates the handshake
// scratch, the certificate chain and the parsed CA on top of them, and the number that matters is
// the LARGEST BLOCK — so a budget that merely equals the sum would still be a coin flip.
inline constexpr size_t kMqttProbeHeapTlsB   = 48u * 1024u;
inline constexpr size_t kMqttProbeHeapPlainB = 12u * 1024u;

inline constexpr size_t mqtt_probe_heap_need(bool tls) {
    return tls ? kMqttProbeHeapTlsB : kMqttProbeHeapPlainB;
}

// May the probe run right now? `largest_free_b` is heap_caps_get_largest_free_block over
// MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL — INTERNAL for the same reason the heap watchdog uses it: a
// plain 8BIT query reports any PSRAM too, and mbedTLS's buffers are not satisfied from there.
inline constexpr bool mqtt_probe_affordable(size_t largest_free_b, bool tls) {
    return largest_free_b >= mqtt_probe_heap_need(tls);
}

// How the probe ended. Mapped to an HTTP status by mqtt_probe_http_status() so the browser and a
// curl caller cannot disagree about which failures are the user's input and which are the device's.
enum class MqttProbeResult {
    Ok,             // CONNECT accepted (and authenticated, where credentials were sent)
    Unreachable,    // DNS or TCP never got there
    Refused,        // the broker answered and rejected us — bad credentials, or not authorised
    Timeout,        // it neither accepted nor refused within the window
    NoHeap,         // the probe could not be afforded (see above) — nothing was attempted
    Internal,       // client/semaphore creation failed
};

// 400 for "your broker value is wrong", 502 for "the broker is not answering", 503 for "ask again
// later". Notably NOT 200-with-an-error: a save endpoint that reports failure inside a 200 body is
// how a UI ends up showing a success toast for a broker that was never reachable.
inline constexpr int mqtt_probe_http_status(MqttProbeResult r) {
    return r == MqttProbeResult::Ok          ? 200
         : r == MqttProbeResult::Refused     ? 400
         : r == MqttProbeResult::NoHeap      ? 503
         : r == MqttProbeResult::Internal    ? 500
                                             : 502;   // Unreachable, Timeout
}

inline const char* mqtt_probe_reason(MqttProbeResult r) {
    switch (r) {
        case MqttProbeResult::Ok:          return "broker reachable";
        case MqttProbeResult::Unreachable: return "broker unreachable (DNS or TCP failed) — not saved";
        case MqttProbeResult::Refused:     return "broker refused the connection (credentials or "
                                                  "authorisation) — not saved";
        case MqttProbeResult::Timeout:     return "broker did not answer in time — not saved";
        case MqttProbeResult::NoHeap:      return "device busy — not enough contiguous memory to "
                                                  "verify the broker; retry";
        case MqttProbeResult::Internal:    return "could not start the broker check — not saved";
    }
    return "not saved";
}

}  // namespace tk
