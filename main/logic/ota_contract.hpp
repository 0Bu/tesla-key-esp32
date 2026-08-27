#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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
    while (position < input.size()) {
        if (!ota_suffix_char(input[position])) return false;
        ++position;
    }
    return true;
}

inline bool canonical_ota_version(std::string_view input) {
    OtaVersionParts ignored{};
    return parse_ota_version(input, ignored);
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
