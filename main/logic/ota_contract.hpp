#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Pure, hardware-free logic shared by the firmware and the host-side mock build
// (test/, built without ESP-IDF). Keep this file free of IDF/FreeRTOS/NimBLE/NVS/
// cJSON/esp_http_server includes so it compiles with a plain host toolchain.
// Single source of truth — ota_update.cpp delegates here. See test/README.md.
namespace tk {

inline constexpr std::size_t kOtaVersionMaxBytes = 31;
inline constexpr std::size_t kOtaManifestMaxBytes = 8192;

struct OtaVersionParts {
    std::array<std::string_view, 3> core{};
    std::string_view suffix{};
};

inline constexpr bool ota_ascii_digit(char value) {
    return value >= '0' && value <= '9';
}

inline constexpr bool ota_suffix_char(char value) {
    return ota_ascii_digit(value) || (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') || value == '.' || value == '-';
}

// The release/build contract is a SemVer-like three-component core plus an optional
// `[0-9A-Za-z.-]+` suffix. Components are canonical decimal (zero, or no leading zero), the
// complete input must be consumed, and the ESP app-descriptor's 31-byte limit is authoritative.
// Components are retained as digit spans so arbitrarily large values are compared without integer
// conversion, overflow or undefined behaviour.
inline bool parse_ota_version(std::string_view input, OtaVersionParts& out) {
    out = {};
    if (input.empty() || input.size() > kOtaVersionMaxBytes) return false;

    std::size_t position = 0;
    for (std::size_t component = 0; component < out.core.size(); ++component) {
        const std::size_t begin = position;
        if (position >= input.size() || !ota_ascii_digit(input[position])) return false;
        if (input[position] == '0') {
            ++position;
            if (position < input.size() && ota_ascii_digit(input[position])) return false;
        } else {
            while (position < input.size() && ota_ascii_digit(input[position])) ++position;
        }
        out.core[component] = input.substr(begin, position - begin);
        if (component + 1 < out.core.size()) {
            if (position >= input.size() || input[position] != '.') return false;
            ++position;
        }
    }

    if (position == input.size()) return true;
    if (input[position] != '-' || ++position == input.size()) return false;
    const std::size_t suffix_begin = position;
    while (position < input.size()) {
        if (!ota_suffix_char(input[position])) return false;
        ++position;
    }
    out.suffix = input.substr(suffix_begin, position - suffix_begin);
    return true;
}

inline bool canonical_ota_version(std::string_view input) {
    OtaVersionParts ignored{};
    return parse_ota_version(input, ignored);
}

// Returns the PR number if the suffix is "PR-<number>" with 1-7 digits and > 0, else 0.
inline unsigned parse_pr_suffix(std::string_view suffix) {
    if (suffix.size() < 4 || suffix.substr(0, 3) != "PR-") return 0;
    std::string_view digits = suffix.substr(3);
    if (digits.empty() || digits.size() > 7) return 0;
    if (digits[0] == '0') return 0; // PR numbers are positive, without leading zero
    unsigned result = 0;
    for (char c : digits) {
        if (!ota_ascii_digit(c)) return 0;
        result = result * 10 + static_cast<unsigned>(c - '0');
    }
    return result;
}

inline unsigned parse_version_pr(std::string_view version) {
    OtaVersionParts parts{};
    if (!parse_ota_version(version, parts)) return 0;
    return parse_pr_suffix(parts.suffix);
}

// Parse a positive numeric PR query string parameter (e.g. from ?pr=326).
inline unsigned parse_pr_query(std::string_view query_val) {
    if (query_val.empty() || query_val.size() > 7) return 0;
    if (query_val[0] == '0') return 0;
    unsigned val = 0;
    for (char c : query_val) {
        if (!ota_ascii_digit(c)) return 0;
        val = val * 10 + static_cast<unsigned>(c - '0');
    }
    return val;
}

// Format the official GitHub Pages PR manifest URL safely without heap allocation.
// URL format: "https://0bu.github.io/tesla-key-esp32/PR/<pr>/manifest.json"
inline bool format_pr_manifest_url(unsigned pr_number, char* buf, std::size_t buf_len) {
    if (pr_number == 0 || pr_number > 9999999 || buf == nullptr) return false;
    char num_buf[16];
    std::size_t num_len = 0;
    unsigned temp = pr_number;
    while (temp > 0) {
        num_buf[num_len++] = static_cast<char>('0' + (temp % 10));
        temp /= 10;
    }
    for (std::size_t i = 0; i < num_len / 2; ++i) {
        char t = num_buf[i];
        num_buf[i] = num_buf[num_len - 1 - i];
        num_buf[num_len - 1 - i] = t;
    }
    static constexpr std::string_view kPrefix = "https://0bu.github.io/tesla-key-esp32/PR/";
    static constexpr std::string_view kSuffix = "/manifest.json";
    const std::size_t total_len = kPrefix.size() + num_len + kSuffix.size();
    if (buf_len < total_len + 1) return false;
    std::size_t offset = 0;
    for (char c : kPrefix) buf[offset++] = c;
    for (std::size_t i = 0; i < num_len; ++i) buf[offset++] = num_buf[i];
    for (char c : kSuffix) buf[offset++] = c;
    buf[offset] = '\0';
    return true;
}

// Format the official GitHub Pages PR firmware URL safely without heap allocation.
// URL format: "https://0bu.github.io/tesla-key-esp32/PR/<pr>/tesla-key-esp32<chip_suffix>.bin"
inline bool format_pr_firmware_url(unsigned pr_number, std::string_view chip_suffix,
                                   char* buf, std::size_t buf_len) {
    if (pr_number == 0 || pr_number > 9999999 || buf == nullptr) return false;
    if (!chip_suffix.empty() && chip_suffix != "-s3" && chip_suffix != "-c3" && chip_suffix != "-c6") {
        return false;
    }
    char num_buf[16];
    std::size_t num_len = 0;
    unsigned temp = pr_number;
    while (temp > 0) {
        num_buf[num_len++] = static_cast<char>('0' + (temp % 10));
        temp /= 10;
    }
    for (std::size_t i = 0; i < num_len / 2; ++i) {
        char t = num_buf[i];
        num_buf[i] = num_buf[num_len - 1 - i];
        num_buf[num_len - 1 - i] = t;
    }
    static constexpr std::string_view kPrefix = "https://0bu.github.io/tesla-key-esp32/PR/";
    static constexpr std::string_view kMid = "/tesla-key-esp32";
    static constexpr std::string_view kExt = ".bin";
    const std::size_t total_len = kPrefix.size() + num_len + kMid.size() + chip_suffix.size() + kExt.size();
    if (buf_len < total_len + 1) return false;
    std::size_t offset = 0;
    for (char c : kPrefix) buf[offset++] = c;
    for (std::size_t i = 0; i < num_len; ++i) buf[offset++] = num_buf[i];
    for (char c : kMid) buf[offset++] = c;
    for (char c : chip_suffix) buf[offset++] = c;
    for (char c : kExt) buf[offset++] = c;
    buf[offset] = '\0';
    return true;
}

enum class OtaChannel : uint8_t {
    Release = 0,
    Dev = 1,
};

inline constexpr const char* kOtaDevSubdir = "dev";

inline const char* ota_channel_name(OtaChannel c) {
    return c == OtaChannel::Dev ? "dev" : "release";
}

// Accepts only the two names /status and POST /set_ota document. Anything else is REFUSED.
inline bool ota_channel_valid(std::string_view s) {
    return s == "release" || s == "dev";
}

// Parse a stored/POSTed channel name, falling back to `def` for anything unrecognised.
inline OtaChannel ota_channel_parse(std::string_view s, OtaChannel def = OtaChannel::Release) {
    if (s == "dev") return OtaChannel::Dev;
    if (s == "release") return OtaChannel::Release;
    return def;
}

// On-flash encoding (logic/config_store.hpp).
inline int32_t ota_channel_to_int(OtaChannel c) {
    return c == OtaChannel::Dev ? 1 : 0;
}

inline OtaChannel ota_channel_from_int(int32_t v) {
    return v == 1 ? OtaChannel::Dev : OtaChannel::Release;
}

// Returns the dev counter if the suffix is "dev" (returns 0) or "dev.<number>" (returns number), else -1.
inline int parse_dev_suffix(std::string_view suffix) {
    if (suffix == "dev") return 0;
    if (suffix.size() < 5 || suffix.substr(0, 4) != "dev.") return -1;
    std::string_view digits = suffix.substr(4);
    if (digits.empty() || digits.size() > 9) return -1;
    int result = 0;
    for (char c : digits) {
        if (!ota_ascii_digit(c)) return -1;
        result = result * 10 + static_cast<int>(c - '0');
    }
    return result;
}

// Does this version string come from the dev feed? CI stamps dev builds as "<next release>-dev.<n>".
inline bool ota_version_is_dev(std::string_view v) {
    OtaVersionParts parts{};
    if (!parse_ota_version(v, parts)) return false;
    return parse_dev_suffix(parts.suffix) >= 0;
}

inline OtaChannel default_ota_channel_for_version(std::string_view v) {
    return ota_version_is_dev(v) ? OtaChannel::Dev : OtaChannel::Release;
}

// Join `rest` onto `base` with exactly one '/' between them. An EMPTY base yields an empty string.
inline std::string ota_url_join(std::string_view base, std::string_view rest) {
    if (base.empty()) return "";
    if (rest.empty()) return std::string(base);
    std::string out(base);
    if (out.back() != '/') out += '/';
    out.append(rest.data(), rest.size());
    return out;
}

// The manifest to check for THIS channel. The release channel uses the configured manifest URL
// verbatim; the dev channel is always <firmware base>/dev/manifest.json.
inline std::string ota_channel_manifest_url(std::string_view release_manifest_url,
                                            std::string_view firmware_base_url,
                                            OtaChannel c) {
    if (c == OtaChannel::Release) return std::string(release_manifest_url);
    return ota_url_join(ota_url_join(firmware_base_url, kOtaDevSubdir), "manifest.json");
}

// The image to download for THIS channel. `image` is the per-target file name (e.g. tesla-key-esp32.bin).
inline std::string ota_channel_firmware_url(std::string_view firmware_base_url,
                                            OtaChannel c,
                                            std::string_view image) {
    const std::string dir = (c == OtaChannel::Dev)
        ? ota_url_join(firmware_base_url, kOtaDevSubdir)
        : std::string(firmware_base_url);
    return ota_url_join(dir, image);
}

// Format the official GitHub Pages Dev manifest URL safely without heap allocation.
// URL format: "https://0bu.github.io/tesla-key-esp32/dev/manifest.json"
inline bool format_dev_manifest_url(char* buf, std::size_t buf_len) {
    static constexpr std::string_view kUrl = "https://0bu.github.io/tesla-key-esp32/dev/manifest.json";
    if (buf == nullptr || buf_len < kUrl.size() + 1) return false;
    for (std::size_t i = 0; i < kUrl.size(); ++i) buf[i] = kUrl[i];
    buf[kUrl.size()] = '\0';
    return true;
}

// Format the official GitHub Pages Dev firmware URL safely without heap allocation.
// URL format: "https://0bu.github.io/tesla-key-esp32/dev/tesla-key-esp32<chip_suffix>.bin"
inline bool format_dev_firmware_url(std::string_view chip_suffix, char* buf, std::size_t buf_len) {
    if (buf == nullptr) return false;
    if (!chip_suffix.empty() && chip_suffix != "-s3" && chip_suffix != "-c3" && chip_suffix != "-c6") {
        return false;
    }
    static constexpr std::string_view kPrefix = "https://0bu.github.io/tesla-key-esp32/dev/tesla-key-esp32";
    static constexpr std::string_view kExt = ".bin";
    const std::size_t total_len = kPrefix.size() + chip_suffix.size() + kExt.size();
    if (buf_len < total_len + 1) return false;
    std::size_t offset = 0;
    for (char c : kPrefix) buf[offset++] = c;
    for (char c : chip_suffix) buf[offset++] = c;
    for (char c : kExt) buf[offset++] = c;
    buf[offset] = '\0';
    return true;
}

enum class OtaVersionOrder : std::int8_t {
    Invalid = -2,
    Older = -1,
    Equal = 0,
    Newer = 1,
};

// Preserve the established OTA policy: freshness is determined by the numeric x.y.z core; a
// suffix does not make an otherwise equal core newer. Manifest and image suffixes are separately
// required to match byte-for-byte by ota_update.cpp.
inline OtaVersionOrder compare_ota_versions(std::string_view candidate,
                                            std::string_view current) {
    OtaVersionParts candidate_parts{};
    OtaVersionParts current_parts{};
    if (!parse_ota_version(candidate, candidate_parts) ||
        !parse_ota_version(current, current_parts)) {
        return OtaVersionOrder::Invalid;
    }
    for (std::size_t component = 0; component < candidate_parts.core.size(); ++component) {
        const auto lhs = candidate_parts.core[component];
        const auto rhs = current_parts.core[component];
        if (lhs.size() != rhs.size()) {
            return lhs.size() > rhs.size() ? OtaVersionOrder::Newer : OtaVersionOrder::Older;
        }
        const int order = lhs.compare(rhs);
        if (order != 0) return order > 0 ? OtaVersionOrder::Newer : OtaVersionOrder::Older;
    }
    return OtaVersionOrder::Equal;
}

// Evaluates whether candidate firmware is eligible for OTA update given the running version,
// an optional target PR number (0 = standard channel), and the selected update channel.
[[gnu::noinline]] inline bool is_ota_update_available(std::string_view candidate,
                                                     std::string_view current,
                                                     unsigned target_pr = 0,
                                                     OtaChannel channel = OtaChannel::Release) {
    OtaVersionParts candidate_parts{};
    OtaVersionParts current_parts{};
    if (!parse_ota_version(candidate, candidate_parts) ||
        !parse_ota_version(current, current_parts)) {
        return false;
    }

    if (target_pr > 0) {
        // Targeted PR update (target_pr > 0):
        // 1. Candidate must carry the matching PR suffix
        const unsigned cand_pr = parse_pr_suffix(candidate_parts.suffix);
        if (cand_pr != target_pr) return false;

        // 2. Candidate core must not be older than the running core
        const OtaVersionOrder order = compare_ota_versions(candidate, current);
        if (order == OtaVersionOrder::Invalid || order == OtaVersionOrder::Older) return false;

        return true;
    }

    const OtaVersionOrder core_order = compare_ota_versions(candidate, current);
    if (core_order == OtaVersionOrder::Invalid) return false;

    if (channel == OtaChannel::Release) {
        // Release feed candidates must be official stable releases (no suffix)
        if (!candidate_parts.suffix.empty()) return false;

        // Dev -> Release downgrade: switching from Dev channel to Release channel allows installing stable release
        if (parse_dev_suffix(current_parts.suffix) >= 0) {
            return true;
        }

        if (core_order == OtaVersionOrder::Newer) return true;
        if (core_order == OtaVersionOrder::Equal) {
            // Allow returning from a PR or pre-release build to the official stable release of the same core
            return !current_parts.suffix.empty();
        }
        return false;
    }

    // Dev channel: candidate must be a dev build
    const int cand_dev = parse_dev_suffix(candidate_parts.suffix);
    if (cand_dev < 0) return false;

    if (core_order == OtaVersionOrder::Newer) return true;
    if (core_order == OtaVersionOrder::Equal) {
        const int curr_dev = parse_dev_suffix(current_parts.suffix);
        if (curr_dev < 0) {
            // Current is not a dev build: dev build of same core is acceptable
            return true;
        }
        return cand_dev > curr_dev;
    }

    return false;
}

template <std::size_t N>
inline std::string_view bounded_c_string_view(const char (&value)[N]) {
    std::size_t length = 0;
    while (length < N && value[length] != '\0') ++length;
    return std::string_view(value, length);
}

enum class BoundedBodyReadResult : std::uint8_t {
    Continue,
    Complete,
    Reject,
};

// Pure state machine for the IDF HTTP read seam. A fixed response needs a positive, bounded
// Content-Length and an exact byte count. Chunked responses may omit it, but both forms require
// the client to confirm complete reception. A negative read after any prefix, a premature zero,
// an empty body and a limit+1 byte all fail closed.
class BoundedHttpBodyGate {
public:
    explicit BoundedHttpBodyGate(std::int64_t content_length, bool chunked,
                                 std::size_t limit = kOtaManifestMaxBytes)
        : content_length_(content_length), chunked_(chunked), limit_(limit) {
        valid_ = limit_ != 0 && content_length_ >= 0 &&
                 (chunked_ || (content_length_ > 0 &&
                               static_cast<std::uint64_t>(content_length_) <= limit_));
    }

    bool valid_headers() const { return valid_; }
    std::size_t bytes_received() const { return received_; }

    std::size_t next_read_size(std::size_t buffer_size) const {
        if (!valid_ || finished_ || buffer_size == 0 || received_ > limit_) return 0;
        // Keep one probe byte beyond the accepted limit so a chunked/lying peer cannot turn an
        // exact-limit prefix into apparent EOF without the transport's complete-data signal.
        const std::size_t remaining_with_probe = (limit_ - received_) + 1;
        return buffer_size < remaining_with_probe ? buffer_size : remaining_with_probe;
    }

    BoundedBodyReadResult accept_read(int read_result, bool complete_data_received) {
        if (!valid_ || finished_ || read_result < 0) return reject();
        if (read_result == 0) {
            if (!complete_data_received || received_ == 0 || !exact_fixed_length()) {
                return reject();
            }
            finished_ = true;
            return BoundedBodyReadResult::Complete;
        }

        const auto count = static_cast<std::size_t>(read_result);
        if (count > limit_ - received_) return reject();
        received_ += count;
        if (!chunked_ && static_cast<std::uint64_t>(received_) >
                             static_cast<std::uint64_t>(content_length_)) {
            return reject();
        }
        if (complete_data_received) {
            if (!exact_fixed_length()) return reject();
            finished_ = true;
            return BoundedBodyReadResult::Complete;
        }
        return BoundedBodyReadResult::Continue;
    }

private:
    bool exact_fixed_length() const {
        return chunked_ || static_cast<std::uint64_t>(received_) ==
                               static_cast<std::uint64_t>(content_length_);
    }

    BoundedBodyReadResult reject() {
        valid_ = false;
        finished_ = true;
        return BoundedBodyReadResult::Reject;
    }

    std::int64_t content_length_{};
    bool chunked_{};
    std::size_t limit_{};
    std::size_t received_{};
    bool valid_{};
    bool finished_{};
};

}  // namespace tk
