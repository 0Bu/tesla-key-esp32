#pragma once

#include <cstddef>
#include <string_view>

// Pure, hardware-free logic shared by the firmware and the host-side mock build
// (test/, built without ESP-IDF). Keep this file free of IDF/FreeRTOS/cJSON/
// esp_http_server includes so it compiles with a plain host toolchain.
// Single source of truth — http_server.cpp delegates browser-origin decisions here.
namespace tk {

inline constexpr char ascii_lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

inline bool ascii_iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

inline bool ascii_iends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           ascii_iequal(value.substr(value.size() - suffix.size()), suffix);
}

inline bool authority_matches_host(std::string_view authority, std::string_view host,
                                   std::string_view default_port) {
    if (ascii_iequal(authority, host)) return true;

    const auto without_default_port = [default_port](std::string_view value) {
        if (ascii_iends_with(value, default_port)) value.remove_suffix(default_port.size());
        return value;
    };
    return ascii_iequal(without_default_port(authority), without_default_port(host));
}

inline std::string_view host_without_port(std::string_view host) {
    if (host.empty() || host.find_first_of("/?#@ \t\r\n") != std::string_view::npos) return {};
    const size_t colon = host.rfind(':');
    if (colon == std::string_view::npos) return host;
    if (host.find(':') != colon || colon == 0 || colon + 1 == host.size()) return {};
    for (size_t i = colon + 1; i < host.size(); ++i) {
        if (host[i] < '0' || host[i] > '9') return {};
    }
    return host.substr(0, colon);
}

// Never use the request's Host header itself as the trust anchor: a DNS-rebinding page controls
// both Host and Origin and could otherwise make two attacker-owned strings compare equal while the
// browser connects to this board. Bind browser requests to names the device owns instead.
//
// The device can hold several IPv4 addresses at once: WiFi and Ethernet keep their leases side by
// side, so after an Ethernet takeover a page that was opened through the still-valid WiFi address
// is being served by this very board and must keep working. Every non-empty entry of
// `device_ipv4s` counts as device-owned; an address the board does not hold never does.
inline bool device_host_allowed(std::string_view host, const std::string_view* device_ipv4s,
                                size_t device_ipv4_count) {
    const std::string_view name = host_without_port(host);
    if (name.empty()) return false;
    if (ascii_iequal(name, "tesla-key-esp32.local") || ascii_iequal(name, "tesla-key-esp32")) {
        return true;
    }
    for (size_t i = 0; i < device_ipv4_count; ++i) {
        if (!device_ipv4s[i].empty() && ascii_iequal(name, device_ipv4s[i])) return true;
    }
    return false;
}

inline bool device_host_allowed(std::string_view host, std::string_view device_ipv4) {
    return device_host_allowed(host, &device_ipv4, 1);
}

// The device API remains intentionally unauthenticated for evcc and other trusted-LAN clients.
// This gate addresses a narrower browser threat: a foreign web origin using a LAN user's browser
// to submit a mutating request. Headerless non-browser clients remain allowed. Same-origin browser
// requests are accepted only when Host is the canonical device name or its current local IPv4.
inline bool extract_referer_authority(std::string_view referer,
                                      std::string_view& authority,
                                      std::string_view& default_port) {
    if (referer.size() >= 7 && ascii_iequal(referer.substr(0, 7), "http://")) {
        referer.remove_prefix(7);
        default_port = ":80";
    } else if (referer.size() >= 8 && ascii_iequal(referer.substr(0, 8), "https://")) {
        referer.remove_prefix(8);
        default_port = ":443";
    } else {
        return false;
    }
    const size_t end = referer.find_first_of("/?#");
    authority = (end == std::string_view::npos) ? referer : referer.substr(0, end);
    if (authority.empty() || authority.find_first_of("@ \t\r\n") != std::string_view::npos) {
        return false;
    }
    return true;
}

// General browser-mutation gate.
// For POST:
//   - headerless clients (Origin, Sec-Fetch-Site and Referer empty) are allowed (curl, evcc).
//   - browsers always send Origin on POST (even with no-referrer). If Origin is present, it must match.
//   - if Referer is present, it must also match.
//   - Sec-Fetch-Site must not be cross-site.
// For state-changing GET:
//   - headerless requests are rejected: Chromium omits Origin & Sec-Fetch-Site on plain HTTP,
//     and no-referrer strips Referer, so a headerless GET cannot prove non-browser origin.
//   - allowed only with proof of same-origin browser or intentional API usage:
//     * valid same-origin Referer, OR
//     * valid same-origin Origin, OR
//     * Sec-Fetch-Site: same-origin or none (with device-owned host), OR
//     * a non-simple custom header (has_custom_header, e.g. X-Requested-With).
inline bool mutation_request_allowed(bool is_post,
                                     std::string_view host,
                                     std::string_view origin,
                                     std::string_view fetch_site,
                                     std::string_view referer,
                                     bool has_custom_header,
                                     const std::string_view* device_ipv4s,
                                     size_t device_ipv4_count) {
    if (ascii_iequal(fetch_site, "cross-site")) return false;
    if (ascii_iequal(origin, "null")) return false;

    // Check Referer if present: foreign Referer must never pass.
    if (!referer.empty()) {
        std::string_view ref_auth;
        std::string_view ref_port;
        if (!extract_referer_authority(referer, ref_auth, ref_port)) return false;
        if (!device_host_allowed(host, device_ipv4s, device_ipv4_count)) return false;
        if (!authority_matches_host(ref_auth, host, ref_port)) return false;
    }

    if (is_post) {
        // Genuinely headerless client on POST (evcc, curl)
        if (origin.empty() && fetch_site.empty() && referer.empty()) return true;
        if (!device_host_allowed(host, device_ipv4s, device_ipv4_count)) return false;
        if (origin.empty()) return true;

        std::string_view authority;
        std::string_view default_port;
        if (origin.size() >= 7 && ascii_iequal(origin.substr(0, 7), "http://")) {
            authority = origin.substr(7);
            default_port = ":80";
        } else if (origin.size() >= 8 && ascii_iequal(origin.substr(0, 8), "https://")) {
            authority = origin.substr(8);
            default_port = ":443";
        } else {
            return false;
        }
        if (authority.empty() || authority.find_first_of("/?#@ \t\r\n") != std::string_view::npos) {
            return false;
        }
        return authority_matches_host(authority, host, default_port);
    }

    // State-changing GET:
    if (has_custom_header) {
        return device_host_allowed(host, device_ipv4s, device_ipv4_count);
    }

    if (!referer.empty()) return true;

    if (!origin.empty() && !ascii_iequal(origin, "null")) {
        if (!device_host_allowed(host, device_ipv4s, device_ipv4_count)) return false;
        std::string_view authority;
        std::string_view default_port;
        if (origin.size() >= 7 && ascii_iequal(origin.substr(0, 7), "http://")) {
            authority = origin.substr(7);
            default_port = ":80";
        } else if (origin.size() >= 8 && ascii_iequal(origin.substr(0, 8), "https://")) {
            authority = origin.substr(8);
            default_port = ":443";
        } else {
            return false;
        }
        if (authority.empty() || authority.find_first_of("/?#@ \t\r\n") != std::string_view::npos) {
            return false;
        }
        return authority_matches_host(authority, host, default_port);
    }

    if (ascii_iequal(fetch_site, "same-origin") || ascii_iequal(fetch_site, "none")) {
        return device_host_allowed(host, device_ipv4s, device_ipv4_count);
    }

    // Genuinely headerless GET on a mutating endpoint: REJECT
    return false;
}

inline bool mutation_request_allowed(bool is_post,
                                     std::string_view host,
                                     std::string_view origin,
                                     std::string_view fetch_site,
                                     std::string_view referer,
                                     bool has_custom_header,
                                     std::string_view device_ipv4) {
    return mutation_request_allowed(is_post, host, origin, fetch_site, referer,
                                    has_custom_header, &device_ipv4, 1);
}

inline bool mutation_origin_allowed(std::string_view host, std::string_view origin,
                                    std::string_view fetch_site,
                                    const std::string_view* device_ipv4s,
                                    size_t device_ipv4_count) {
    return mutation_request_allowed(true, host, origin, fetch_site, "", false,
                                    device_ipv4s, device_ipv4_count);
}

inline bool mutation_origin_allowed(std::string_view host, std::string_view origin,
                                    std::string_view fetch_site,
                                    std::string_view device_ipv4) {
    return mutation_origin_allowed(host, origin, fetch_site, &device_ipv4, 1);
}

inline std::string_view request_path(std::string_view uri) {
    const size_t query = uri.find('?');
    return uri.substr(0, query);
}

// Decide whether the raw query string carries key=value, using the SAME matching rules the
// handler's read will use. This must track esp_http_server's httpd_query_key_value(), because the
// gate below classifies the request and http_common.cpp's query_param_is() performs it — the two
// disagreeing is a gate bypass, not a style difference:
//   * the KEY is compared case-INSENSITIVELY (IDF uses strncasecmp on an exact-length match), so
//     `?CLEAR=1` is the same parameter as `?clear=1`. Comparing it case-sensitively here left
//     `/diag?CLEAR=1`, `?VERBOSE=0|1` and `/coredump?CLEAR=1` unclassified while the handler still
//     cleared the ring, flipped verbose logging and erased the core dump.
//   * the VALUE is compared case-SENSITIVELY and byte-for-byte up to '&'. IDF copies it verbatim —
//     no percent-decoding, no case folding — and query_param_is() then does a plain strcmp, so
//     folding it here would classify requests the handler ignores and, worse, invite the inverse
//     mistake later. Keep this half exact.
inline bool query_has_exact(std::string_view uri, std::string_view key,
                            std::string_view value) {
    const size_t query = uri.find('?');
    if (query == std::string_view::npos) return false;
    std::string_view rest = uri.substr(query + 1);
    while (!rest.empty()) {
        const size_t amp = rest.find('&');
        const std::string_view item = rest.substr(0, amp);
        const size_t equals = item.find('=');
        if (equals != std::string_view::npos && ascii_iequal(item.substr(0, equals), key) &&
            item.substr(equals + 1) == value) {
            return true;
        }
        if (amp == std::string_view::npos) break;
        rest.remove_prefix(amp + 1);
    }
    return false;
}

// GET is normally read-only, but these legacy diagnostic/OTA routes deliberately mutate state.
// Keep their browser-CSRF gate adjacent to the POST policy so a method name cannot hide a write.
inline bool mutation_origin_required(bool is_post, std::string_view uri) {
    if (is_post) return true;
    const std::string_view path = request_path(uri);
    if (path == "/ota/check") return true;
    if (path == "/diag") {
        return query_has_exact(uri, "clear", "1") ||
               query_has_exact(uri, "verbose", "0") ||
               query_has_exact(uri, "verbose", "1");
    }
    return path == "/coredump" && query_has_exact(uri, "clear", "1");
}

}  // namespace tk
