#pragma once
#include "ota_transport.hpp"
#include <http_parser.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace tk::ota {
struct HttpLimits {
    std::uint64_t body_bytes;
    std::uint32_t headers_bytes{8192};
    std::uint32_t chunk_metadata_bytes{65536};
    std::uint16_t field_bytes{64}, value_bytes{1023}, fields{64}, chunk_line_bytes{256};
    std::uint8_t interim_responses{2};
    bool allow_eof_image{false}; // Metadata preserves current CL/chunked requirement.
};
enum class ParseResult : std::uint8_t { More, Complete, Redirect, Rejected, SinkFailed };
using BodyCallback = bool (*)(void*, const std::uint8_t*, std::size_t) noexcept;
class Framing {
public:
    Framing(HttpLimits, BodyCallback, void*) noexcept;
    Framing(const Framing&) = delete;
    Framing& operator=(const Framing&) = delete;
    ParseResult feed(const std::uint8_t*, std::size_t) noexcept;
    ParseResult eof() noexcept;
    ParseResult result() const noexcept { return result_; }
    const char* location() const noexcept { return location_.data(); }
    std::uint64_t body_bytes() const noexcept { return body_bytes_; }
    bool has_declared_size() const noexcept { return has_length_; }
    std::uint64_t declared_size() const noexcept { return length_; }
    unsigned status() const noexcept { return status_; }
private:
    enum class State : std::uint8_t { Headers, ChunkMetadata, Body, Trailers, Done };
    enum class Field : std::uint8_t { None, Unknown, Length, Transfer, Encoding, Location };
    static Framing& self(http_parser* p) noexcept { return *static_cast<Framing*>(p->data); }
    static int parser_begin(http_parser*) noexcept;
    static int parser_field(http_parser*, const char*, std::size_t) noexcept;
    static int parser_value(http_parser*, const char*, std::size_t) noexcept;
    static int parser_headers(http_parser*) noexcept;
    static int parser_body(http_parser*, const char*, std::size_t) noexcept;
    static int parser_complete(http_parser*) noexcept;
    static int parser_chunk(http_parser*) noexcept;
    static int parser_chunk_done(http_parser*) noexcept;
    bool finish_field() noexcept;
    bool reject() noexcept { result_ = ParseResult::Rejected; return false; }
    bool count_metadata() noexcept;
    void reset_fields() noexcept;
    http_parser parser_{};
    HttpLimits limits_;
    BodyCallback sink_;
    void* sink_context_;
    std::array<char, 32> name_{}, small_{};
    std::array<char, kUrlBytes> location_{};
    std::uint64_t body_bytes_{0}, length_{0}, remaining_{0};
    std::uint32_t header_bytes_{0}, chunk_bytes_{0};
    std::uint16_t field_size_{0}, value_size_{0}, field_count_{0}, line_bytes_{0};
    unsigned status_{0};
    Field current_{Field::None};
    State state_{State::Headers};
    ParseResult result_{ParseResult::More};
    std::uint8_t interims_{0};
    bool in_value_{false}, has_length_{false}, chunked_{false}, has_transfer_{false};
    bool encoded_{false}, has_location_{false}, interim_{false}, eof_body_{false};
};
// Bounded HTTPS URL checks/resolution used by worker and coordinator, not a regex URL parser.
bool valid_https_url(const char*, http_parser_url&) noexcept;
bool resolve_redirect(const char* base, const char* location,
                      std::array<char, kUrlBytes>& out) noexcept;
} // namespace tk::ota
