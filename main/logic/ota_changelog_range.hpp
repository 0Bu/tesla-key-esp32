#pragma once
// Select the part of a cumulative OTA changelog which belongs to one offered update.
// IDF-free, allocation-free and host-tested.
//
// Older firmware accepts exactly the existing {"version","changelog"} document and renders every
// non-empty line. The publisher keeps that schema and prefixes cumulative development notes
// with their build version:
//
//     v1.0.3-dev.19 — Preserve legacy bench restore compatibility
//     v1.0.3-dev.20 — Accept exact legacy writer evidence
//
// A new client can remove entries which its running build already contains without another HTTPS
// request or a larger retained document. Legacy unprefixed notes remain unchanged. A document
// which starts in the versioned format but is malformed, unordered or not bound to the offered
// target is rejected instead of presenting a plausible partial history.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "ota_contract.hpp"

namespace tk {

enum class OtaChangelogRangeResult {
    Legacy,
    Selected,
    Invalid,
};

// Full SemVer-aware version comparison for changelog evaluation.
// Returns >0 if a > b, <0 if a < b, 0 if equal, -2 if either is invalid.
inline int compare_ota_full_versions(std::string_view a, std::string_view b) {
    OtaVersionParts ap{};
    OtaVersionParts bp{};
    if (!parse_ota_version(a, ap) || !parse_ota_version(b, bp)) {
        return -2;
    }
    const OtaVersionOrder core_order = compare_ota_versions(a, b);
    if (core_order == OtaVersionOrder::Newer) return 1;
    if (core_order == OtaVersionOrder::Older) return -1;
    if (core_order == OtaVersionOrder::Invalid) return -2;

    // Cores are equal
    if (ap.suffix.empty() && bp.suffix.empty()) return 0;
    // Release version without suffix has higher precedence than pre-release with suffix
    if (ap.suffix.empty() && !bp.suffix.empty()) return 1;
    if (!ap.suffix.empty() && bp.suffix.empty()) return -1;

    // Both have suffixes
    const int da = parse_dev_suffix(ap.suffix);
    const int db = parse_dev_suffix(bp.suffix);
    if (da >= 0 && db >= 0) {
        if (da > db) return 1;
        if (da < db) return -1;
        return 0;
    }
    return ap.suffix.compare(bp.suffix);
}

// Derive a sibling URL (e.g. changelog.json) from manifest_url without heap allocation.
inline bool ota_manifest_sibling_url(const std::string& manifest_url, const char* sibling,
                                     char* out, size_t outlen) {
    if (!out || outlen == 0) return false;
    out[0] = '\0';
    if (manifest_url.empty() || !sibling || sibling[0] == '\0') return false;
    const auto slash = manifest_url.rfind('/');
    if (slash == std::string::npos) return false;
    const size_t prefix_len = slash + 1;
    const size_t sibling_len = std::strlen(sibling);
    if (prefix_len + sibling_len >= outlen) return false;
    std::memcpy(out, manifest_url.data(), prefix_len);
    std::memcpy(out + prefix_len, sibling, sibling_len + 1);
    return true;
}

namespace detail {

inline void skip_ws(const char* json, size_t len, size_t& pos) {
    while (pos < len && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
}

inline bool scan_string(const char* json, size_t len, size_t pos,
                        size_t& start, size_t& end, size_t& next) {
    if (pos >= len || json[pos] != '"') return false;
    start = pos + 1;
    size_t i = start;
    while (i < len) {
        if (json[i] == '\\') {
            i += 2;
            continue;
        }
        if (json[i] == '"') {
            end = i;
            next = i + 1;
            return true;
        }
        ++i;
    }
    return false;
}

inline bool decode_text_string(const char* json, size_t len, size_t i,
                               char* out, size_t outlen, size_t& next) {
    if (!json || !out || outlen == 0 || i >= len || json[i] != '"') return false;
    out[0] = '\0';
    size_t written = 0;
    ++i;
    while (i < len && json[i] != '"') {
        unsigned char c = static_cast<unsigned char>(json[i++]);
        if (c < 0x20) return false;
        if (c == '\\') {
            if (i >= len) return false;
            const char escaped = json[i++];
            switch (escaped) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                default: return false;
            }
        }
        if (written + 1 >= outlen) { out[0] = '\0'; return false; }
        out[written++] = static_cast<char>(c);
    }
    if (i >= len || json[i] != '"') { out[0] = '\0'; return false; }
    out[written] = '\0';
    next = i + 1;
    return written > 0;
}

} // namespace detail

// Parse the optional sibling changelog.json document:
//     {"version":"1.2.3-dev.4","changelog":"Add …\nFix …"}
// Stale or mismatched version fails closed.
inline bool manifest_changelog(const char* json, size_t len, const char* expected_version,
                               char* out, size_t outlen) {
    if (!json || !expected_version || !out || outlen == 0) return false;
    if (out != json) out[0] = '\0';
    char version[32] = {0};
    bool have_version = false;
    bool have_changelog = false;
    size_t i = 0;

    auto copy_plain_string = [&](size_t& pos, char* target, size_t capacity) -> bool {
        detail::skip_ws(json, len, pos);
        size_t start, end, next;
        if (!detail::scan_string(json, len, pos, start, end, next)) return false;
        const size_t value_len = end - start;
        if (value_len == 0 || value_len >= capacity) return false;
        for (size_t k = 0; k < value_len; ++k) {
            if (json[start + k] == '\\' || static_cast<unsigned char>(json[start + k]) < 0x20)
                return false;
        }
        std::memcpy(target, json + start, value_len);
        target[value_len] = '\0';
        pos = next;
        return true;
    };

    detail::skip_ws(json, len, i);
    if (i >= len || json[i++] != '{') { out[0] = '\0'; return false; }
    while (true) {
        detail::skip_ws(json, len, i);
        if (i >= len || json[i] != '"') { out[0] = '\0'; return false; }
        size_t key_start, key_end, next;
        if (!detail::scan_string(json, len, i, key_start, key_end, next)) {
            out[0] = '\0';
            return false;
        }
        i = next;
        detail::skip_ws(json, len, i);
        if (i >= len || json[i++] != ':') { out[0] = '\0'; return false; }

        const size_t key_len = key_end - key_start;
        if (key_len == 7 && std::memcmp(json + key_start, "version", 7) == 0) {
            if (have_version || !copy_plain_string(i, version, sizeof(version))) {
                out[0] = '\0';
                return false;
            }
            have_version = true;
        } else if (key_len == 9 && std::memcmp(json + key_start, "changelog", 9) == 0) {
            if (have_changelog) { out[0] = '\0'; return false; }
            detail::skip_ws(json, len, i);
            if (!detail::decode_text_string(json, len, i, out, outlen, next)) {
                out[0] = '\0';
                return false;
            }
            i = next;
            have_changelog = true;
        } else {
            out[0] = '\0';
            return false;
        }

        detail::skip_ws(json, len, i);
        if (i < len && json[i] == ',') { ++i; continue; }
        if (i < len && json[i] == '}') { ++i; break; }
        out[0] = '\0';
        return false;
    }

    detail::skip_ws(json, len, i);
    const bool valid = (i == len) && have_version && have_changelog &&
                       (std::strcmp(version, expected_version) == 0);
    if (!valid) out[0] = '\0';
    return valid;
}

inline OtaChangelogRangeResult ota_changelog_select_range(char* text, const char* running,
                                                          const char* target) {
    if (!text || !running || !target || !canonical_ota_version(running) || !canonical_ota_version(target)) {
        if (text) text[0] = '\0';
        return OtaChangelogRangeResult::Invalid;
    }

    constexpr char separator[] = " — ";
    constexpr size_t separator_len = sizeof(separator) - 1;
    const size_t text_len = std::strlen(text);
    if (text_len == 0) return OtaChangelogRangeResult::Legacy;

    size_t selected_offset = 0;
    bool running_seen = false;
    bool versioned = false;
    char previous[32] = {};
    char last[32] = {};

    for (size_t offset = 0; offset < text_len;) {
        const size_t line_start = offset;
        while (offset < text_len && text[offset] != '\n') ++offset;
        const size_t line_end = offset;
        const size_t next = offset < text_len ? offset + 1 : offset;
        if (line_end == line_start) {
            text[0] = '\0';
            return OtaChangelogRangeResult::Invalid;
        }

        const bool looks_versioned = text[line_start] == 'v' && line_start + 1 < line_end &&
                                     text[line_start + 1] >= '0' && text[line_start + 1] <= '9';
        size_t split = line_start + 1;
        while (split + separator_len <= line_end &&
               std::memcmp(text + split, separator, separator_len) != 0) {
            ++split;
        }
        const bool has_separator = split + separator_len <= line_end;
        bool compact_prefix = looks_versioned && has_separator;
        if (compact_prefix) {
            for (size_t k = line_start + 1; k < split; ++k) {
                if (static_cast<unsigned char>(text[k]) <= 0x20) {
                    compact_prefix = false;
                    break;
                }
            }
        }
        if (!compact_prefix) {
            if (!versioned && line_start == 0) return OtaChangelogRangeResult::Legacy;
            text[0] = '\0';
            return OtaChangelogRangeResult::Invalid;
        }
        versioned = true;

        if (line_start == 0 && compare_ota_full_versions(running, target) >= 0) {
            text[0] = '\0';
            return OtaChangelogRangeResult::Invalid;
        }

        const size_t version_len = split - (line_start + 1);
        if (version_len == 0 || version_len >= sizeof(last) || split + separator_len == line_end) {
            text[0] = '\0';
            return OtaChangelogRangeResult::Invalid;
        }
        std::memcpy(last, text + line_start + 1, version_len);
        last[version_len] = '\0';
        if (!canonical_ota_version(last) || (previous[0] && compare_ota_full_versions(previous, last) > 0)) {
            text[0] = '\0';
            return OtaChangelogRangeResult::Invalid;
        }

        if (std::strcmp(last, running) == 0) {
            running_seen = true;
            selected_offset = next;
        }
        std::memcpy(previous, last, sizeof(previous));
        offset = next;
    }

    if (!versioned || std::strcmp(last, target) != 0) {
        text[0] = '\0';
        return OtaChangelogRangeResult::Invalid;
    }
    if (running_seen) {
        std::memmove(text, text + selected_offset, text_len - selected_offset + 1);
    }
    return OtaChangelogRangeResult::Selected;
}

} // namespace tk
