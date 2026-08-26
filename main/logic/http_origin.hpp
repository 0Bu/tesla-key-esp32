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

// The device API remains intentionally unauthenticated for evcc and other trusted-LAN clients.
// This gate addresses a narrower browser threat: a foreign web origin using a LAN user's browser
// to submit a mutating POST. Headerless non-browser clients remain allowed. Same-origin browser
// POSTs are accepted by comparing Origin authority with Host, including default HTTP(S) ports.
inline bool mutation_origin_allowed(std::string_view host, std::string_view origin,
                                    std::string_view fetch_site) {
    if (ascii_iequal(fetch_site, "cross-site")) return false;
    if (origin.empty()) return true;
    if (host.empty() || ascii_iequal(origin, "null")) return false;

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

}  // namespace tk
