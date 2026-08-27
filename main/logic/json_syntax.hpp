#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>

// Allocation-free JSON syntax validation for the bounded HTTP configuration envelope. This is
// intentionally a syntax gate, not a data model: cJSON still builds and owns the parsed tree. Its
// purpose is to distinguish malformed input (400), valid-but-over-complex input (400/-32600), and
// an accepted document for which cJSON could not allocate a tree (503). Keep it IDF/cJSON-free so
// every boundary is host-tested.
namespace tk {

inline constexpr unsigned kJsonMaxNesting = 16;
inline constexpr int64_t kJsonSafeIntegerMax = INT64_C(9007199254740991);

// IEEE-754 doubles preserve every integer in [-2^53+1, 2^53-1] exactly. cJSON materializes
// numbers as doubles, so a JSON-RPC numeric id must stay inside this range and must have no
// fractional component before it can be snapshotted and echoed without changing its value.
inline bool json_safe_integer(double value, int64_t& out) noexcept {
    if (!std::isfinite(value) || std::trunc(value) != value ||
        value < -static_cast<double>(kJsonSafeIntegerMax) ||
        value > static_cast<double>(kJsonSafeIntegerMax)) {
        return false;
    }
    out = static_cast<int64_t>(value);
    return true;
}

enum class JsonRawNumberStatus { Missing, NonNumber, ValidInteger, InvalidNumber };

struct JsonRawNumberId {
    int64_t value{};
    JsonRawNumberStatus status{JsonRawNumberStatus::Missing};
};

// Parse the original JSON number token, not cJSON's rounded double. Numeric request ids deliberately
// use one canonical, exactly echoable subset: base-10 integer notation, no exponent/fraction and no
// negative zero. The magnitude accumulator is bounded before every multiply/add.
inline bool json_canonical_safe_integer(const char* first, const char* last,
                                        int64_t& out) noexcept {
    if (!first || !last || first >= last) return false;
    bool negative = false;
    if (*first == '-') {
        negative = true;
        ++first;
        if (first == last) return false;
    }
    if (*first == '0' && first + 1 != last) return false;

    uint64_t magnitude = 0;
    for (const char* p = first; p != last; ++p) {
        if (*p < '0' || *p > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(*p - '0');
        if (magnitude > (static_cast<uint64_t>(kJsonSafeIntegerMax) - digit) / 10u)
            return false;
        magnitude = magnitude * 10u + digit;
    }
    if (negative && magnitude == 0) return false;
    out = negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
    return true;
}

enum class JsonSyntaxStatus {
    Valid,
    Malformed,
    TooDeep,
    UnsupportedNul,
};

namespace json_detail {

class SyntaxCursor {
public:
    SyntaxCursor(const char* data, size_t len, JsonRawNumberId* raw_id = nullptr)
        : cur_(data), end_(data ? data + len : nullptr), raw_id_(raw_id) {}

    JsonSyntaxStatus document() {
        if (!cur_) return JsonSyntaxStatus::Malformed;
        whitespace();
        if (!value(0)) {
            if (too_deep_) return JsonSyntaxStatus::TooDeep;
            if (unsupported_nul_) return JsonSyntaxStatus::UnsupportedNul;
            return JsonSyntaxStatus::Malformed;
        }
        whitespace();
        return cur_ == end_ ? JsonSyntaxStatus::Valid : JsonSyntaxStatus::Malformed;
    }

private:
    const char* cur_;
    const char* end_;
    bool        too_deep_{false};
    bool        unsupported_nul_{false};
    JsonRawNumberId* raw_id_{nullptr};

    void whitespace() {
        while (cur_ < end_ && (*cur_ == ' ' || *cur_ == '\t' || *cur_ == '\r' || *cur_ == '\n'))
            ++cur_;
    }

    bool take(char want) {
        if (cur_ == end_ || *cur_ != want) return false;
        ++cur_;
        return true;
    }

    bool literal(const char* text) {
        while (*text) {
            if (cur_ == end_ || *cur_++ != *text++) return false;
        }
        return true;
    }

    static bool hex(char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    }

    static uint8_t hex_value(char c) {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        return static_cast<uint8_t>(c - 'A' + 10);
    }

    bool hex4(uint16_t& value) {
        if (end_ - cur_ < 4) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) {
            if (!hex(*cur_)) return false;
            value = static_cast<uint16_t>((value << 4) | hex_value(*cur_++));
        }
        return true;
    }

    bool utf8_continuation(unsigned char minimum = 0x80, unsigned char maximum = 0xbf) {
        if (cur_ == end_) return false;
        const unsigned char c = static_cast<unsigned char>(*cur_);
        if (c < minimum || c > maximum) return false;
        ++cur_;
        return true;
    }

    // Validate one already-consumed non-ASCII lead byte as a Unicode scalar encoded in the
    // shortest UTF-8 form. The restricted second-byte ranges reject overlong sequences, UTF-16
    // surrogate code points and values above U+10FFFF without decoding or allocating.
    bool utf8_scalar(unsigned char lead) {
        if (lead >= 0xc2 && lead <= 0xdf) {
            return utf8_continuation();
        }
        if (lead == 0xe0) {
            return utf8_continuation(0xa0, 0xbf) && utf8_continuation();
        }
        if ((lead >= 0xe1 && lead <= 0xec) || (lead >= 0xee && lead <= 0xef)) {
            return utf8_continuation() && utf8_continuation();
        }
        if (lead == 0xed) {
            return utf8_continuation(0x80, 0x9f) && utf8_continuation();
        }
        if (lead == 0xf0) {
            return utf8_continuation(0x90, 0xbf) && utf8_continuation() &&
                   utf8_continuation();
        }
        if (lead >= 0xf1 && lead <= 0xf3) {
            return utf8_continuation() && utf8_continuation() && utf8_continuation();
        }
        if (lead == 0xf4) {
            return utf8_continuation(0x80, 0x8f) && utf8_continuation() &&
                   utf8_continuation();
        }
        return false;
    }

    bool string(bool* exact_decoded_id = nullptr) {
        if (exact_decoded_id) *exact_decoded_id = false;
        if (!take('"')) return false;
        size_t decoded_length = 0;
        bool decoded_id = true;
        const auto observe_scalar = [&](uint32_t scalar) {
            if (decoded_length == 0) {
                decoded_id = decoded_id && scalar == static_cast<uint32_t>('i');
            } else if (decoded_length == 1) {
                decoded_id = decoded_id && scalar == static_cast<uint32_t>('d');
            } else {
                decoded_id = false;
            }
            ++decoded_length;
        };
        while (cur_ < end_) {
            const unsigned char c = static_cast<unsigned char>(*cur_++);
            if (c == '"') {
                if (exact_decoded_id)
                    *exact_decoded_id = decoded_id && decoded_length == 2;
                return true;
            }
            if (c < 0x20) return false;
            if (c >= 0x80) {
                if (!utf8_scalar(c)) return false;
                observe_scalar(UINT32_C(0x110000));  // valid scalar, definitely not ASCII "id"
                continue;
            }
            if (c != '\\') {
                observe_scalar(c);
                continue;
            }
            if (cur_ == end_) return false;
            const char escaped = *cur_++;
            if (escaped == '"' || escaped == '\\' || escaped == '/' || escaped == 'b' ||
                escaped == 'f' || escaped == 'n' || escaped == 'r' || escaped == 't') {
                uint32_t scalar = static_cast<unsigned char>(escaped);
                if (escaped == 'b') scalar = '\b';
                if (escaped == 'f') scalar = '\f';
                if (escaped == 'n') scalar = '\n';
                if (escaped == 'r') scalar = '\r';
                if (escaped == 't') scalar = '\t';
                observe_scalar(scalar);
                continue;
            }
            if (escaped != 'u') return false;
            uint16_t first = 0;
            if (!hex4(first)) return false;
            // cJSON exposes valuestring only as a NUL-terminated C string. Accepting U+0000 would
            // silently truncate data such as a JSON-RPC id ("a\\u0000b" -> "a") and could
            // correlate a reply with the wrong request. Reject it as a supported-input limit
            // before materialization; raw control bytes remain ordinary malformed JSON.
            if (first == 0) {
                unsupported_nul_ = true;
                return false;
            }
            if (first >= 0xd800 && first <= 0xdbff) {
                // cJSON follows UTF-16 here: a high surrogate must be immediately followed by a
                // low surrogate escape. Treating an unpaired surrogate as syntactically valid
                // would misclassify cJSON's parse-null as OOM/503 instead of malformed/400.
                if (end_ - cur_ < 6 || cur_[0] != '\\' || cur_[1] != 'u') return false;
                cur_ += 2;
                uint16_t second = 0;
                if (!hex4(second) || second < 0xdc00 || second > 0xdfff) return false;
                observe_scalar(UINT32_C(0x110000));
            } else if (first >= 0xdc00 && first <= 0xdfff) {
                return false;
            } else {
                observe_scalar(first);
            }
        }
        return false;
    }

    bool number() {
        const char* start = cur_;
        if (cur_ < end_ && *cur_ == '-') ++cur_;
        if (cur_ == end_) return false;
        if (*cur_ == '0') {
            ++cur_;
            if (cur_ < end_ && *cur_ >= '0' && *cur_ <= '9') return false;
        } else {
            if (*cur_ < '1' || *cur_ > '9') return false;
            while (cur_ < end_ && *cur_ >= '0' && *cur_ <= '9') ++cur_;
        }
        if (cur_ < end_ && *cur_ == '.') {
            ++cur_;
            if (cur_ == end_ || *cur_ < '0' || *cur_ > '9') return false;
            while (cur_ < end_ && *cur_ >= '0' && *cur_ <= '9') ++cur_;
        }
        if (cur_ < end_ && (*cur_ == 'e' || *cur_ == 'E')) {
            ++cur_;
            if (cur_ < end_ && (*cur_ == '+' || *cur_ == '-')) ++cur_;
            if (cur_ == end_ || *cur_ < '0' || *cur_ > '9') return false;
            while (cur_ < end_ && *cur_ >= '0' && *cur_ <= '9') ++cur_;
        }
        return cur_ != start;
    }

    bool array(unsigned depth) {
        if (!take('[')) return false;
        whitespace();
        if (take(']')) return true;
        for (;;) {
            if (!value(depth)) return false;
            whitespace();
            if (take(']')) return true;
            if (!take(',')) return false;
            whitespace();
        }
    }

    bool object(unsigned depth) {
        if (!take('{')) return false;
        whitespace();
        if (take('}')) return true;
        for (;;) {
            bool exact_decoded_id = false;
            if (!string(depth == 1 && raw_id_ ? &exact_decoded_id : nullptr)) return false;
            whitespace();
            if (!take(':')) return false;
            whitespace();
            const char* value_start = cur_;
            if (!value(depth)) return false;
            if (exact_decoded_id) {
                if (value_start < end_ &&
                    (*value_start == '-' || (*value_start >= '0' && *value_start <= '9'))) {
                    int64_t exact = 0;
                    raw_id_->status = json_canonical_safe_integer(value_start, cur_, exact)
                                          ? JsonRawNumberStatus::ValidInteger
                                          : JsonRawNumberStatus::InvalidNumber;
                    raw_id_->value = exact;
                } else {
                    raw_id_->status = JsonRawNumberStatus::NonNumber;
                }
            }
            whitespace();
            if (take('}')) return true;
            if (!take(',')) return false;
            whitespace();
        }
    }

    bool value(unsigned depth) {
        if (depth > kJsonMaxNesting) {
            too_deep_ = true;
            return false;
        }
        if (cur_ == end_) return false;
        switch (*cur_) {
            case '{':
                if (depth >= kJsonMaxNesting) {
                    too_deep_ = true;
                    return false;
                }
                return object(depth + 1);
            case '[':
                if (depth >= kJsonMaxNesting) {
                    too_deep_ = true;
                    return false;
                }
                return array(depth + 1);
            case '"': return string();
            case 't': return literal("true");
            case 'f': return literal("false");
            case 'n': return literal("null");
            default:  return number();
        }
    }
};

}  // namespace json_detail

inline JsonSyntaxStatus json_syntax_status(const char* data, size_t len) {
    return json_detail::SyntaxCursor(data, len).document();
}

inline bool json_syntax_valid(const char* data, size_t len) {
    return json_syntax_status(data, len) == JsonSyntaxStatus::Valid;
}

inline JsonRawNumberId json_top_level_numeric_id(const char* data, size_t len) {
    JsonRawNumberId result;
    json_detail::SyntaxCursor cursor(data, len, &result);
    if (cursor.document() != JsonSyntaxStatus::Valid) return {};
    return result;
}

enum class JsonMaterializeStatus {
    Ok,
    Malformed,
    TooDeep,
    UnsupportedNul,
    NoMemory,
};

template <typename Node>
struct JsonMaterializeResult {
    Node*                 root{nullptr};
    JsonMaterializeStatus status{JsonMaterializeStatus::Malformed};
};

// Narrow parser fault seam used by the cJSON shells. `parse` is cJSON_Parse on-device and an
// n-th-allocation failpoint in the host gate. Because syntax validation allocates nothing, nullptr
// after valid syntax is unambiguously a materialisation/resource failure and maps to 503.
template <typename Node, typename Parse>
JsonMaterializeResult<Node> json_materialize(const char* data, size_t len, Parse parse) {
    const JsonSyntaxStatus syntax = json_syntax_status(data, len);
    if (syntax == JsonSyntaxStatus::Malformed) {
        return {nullptr, JsonMaterializeStatus::Malformed};
    }
    if (syntax == JsonSyntaxStatus::TooDeep) {
        return {nullptr, JsonMaterializeStatus::TooDeep};
    }
    if (syntax == JsonSyntaxStatus::UnsupportedNul) {
        return {nullptr, JsonMaterializeStatus::UnsupportedNul};
    }
    Node* root = parse(data);
    if (!root) return {nullptr, JsonMaterializeStatus::NoMemory};
    return {root, JsonMaterializeStatus::Ok};
}

}  // namespace tk
