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
inline bool device_host_allowed(std::string_view host, std::string_view device_ipv4) {
    const std::string_view name = host_without_port(host);
    if (name.empty()) return false;
    return ascii_iequal(name, "tesla-key-esp32.local") ||
           ascii_iequal(name, "tesla-key-esp32") ||
           (!device_ipv4.empty() && ascii_iequal(name, device_ipv4));
}

// The device API remains intentionally unauthenticated for evcc and other trusted-LAN clients.
// This gate addresses a narrower browser threat: a foreign web origin using a LAN user's browser
// to submit a mutating request. Headerless non-browser clients remain allowed. Same-origin browser
// requests are accepted only when Host is the canonical device name or its current local IPv4.
// Router-expanded DHCP FQDNs are not device-owned and therefore cannot be allowlisted safely.
inline bool mutation_origin_allowed(std::string_view host, std::string_view origin,
                                    std::string_view fetch_site,
                                    std::string_view device_ipv4) {
    if (ascii_iequal(fetch_site, "cross-site")) return false;
    if (origin.empty()) return true;
    if (!device_host_allowed(host, device_ipv4) || ascii_iequal(origin, "null")) return false;

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

    // Origin is an origin tuple, never a URL with path/query/fragment or userinfo.
    if (authority.empty() || authority.find_first_of("/?#@ \t\r\n") != std::string_view::npos) {
        return false;
    }
    return authority_matches_host(authority, host, default_port);
}

inline std::string_view request_path(std::string_view uri) {
    const size_t query = uri.find('?');
    return uri.substr(0, query);
}

inline bool query_has_exact(std::string_view uri, std::string_view key,
                            std::string_view value) {
    const size_t query = uri.find('?');
    if (query == std::string_view::npos) return false;
    std::string_view rest = uri.substr(query + 1);
    while (!rest.empty()) {
        const size_t amp = rest.find('&');
        const std::string_view item = rest.substr(0, amp);
        const size_t equals = item.find('=');
        if (equals != std::string_view::npos && item.substr(0, equals) == key &&
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
