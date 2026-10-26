#include "ota_http.hpp"
#include "stack_watch.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace tk::ota {
namespace {
bool equal_ci(const char* value, std::size_t n, const char* literal) noexcept {
    if (std::strlen(literal) != n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        auto c = static_cast<unsigned char>(value[i]);
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != static_cast<unsigned char>(literal[i])) return false;
    }
    return true;
}
std::size_t bounded_len(const char* p, std::size_t maximum) noexcept {
    if (!p) return maximum;
    std::size_t n = 0;
    while (n < maximum && p[n]) ++n;
    return n;
}
bool append(std::array<char, kUrlBytes>& out, std::size_t& n,
            const char* text, std::size_t count) noexcept {
    if (n >= out.size() || count >= out.size() - n) return false;
    std::memcpy(out.data() + n, text, count);
    n += count;
    out[n] = '\0';
    return true;
}
} // namespace

Framing::Framing(HttpLimits limits, BodyCallback sink, void* context) noexcept
    : limits_(limits), sink_(sink), sink_context_(context) {
    http_parser_init(&parser_, HTTP_RESPONSE);
    parser_.data = this;
    if (!limits.body_bytes || !limits.headers_bytes || !limits.chunk_metadata_bytes ||
        !limits.fields || !limits.field_bytes || !limits.value_bytes ||
        limits.value_bytes >= kUrlBytes || !limits.chunk_line_bytes) reject();
}
void Framing::reset_fields() noexcept {
    field_size_ = value_size_ = field_count_ = 0;
    name_.fill(0); small_.fill(0); location_.fill(0);
    current_ = Field::None;
    in_value_ = has_length_ = chunked_ = has_transfer_ = encoded_ = has_location_ = false;
    length_ = remaining_ = 0;
}
int Framing::parser_begin(http_parser* p) noexcept {
    tk::StackWatchScope f135_callback_exit(tk::StackWatch::OtaParser);
    auto& s = self(p);
    s.reset_fields();
    s.state_ = State::Headers;
    return 0;
}
bool Framing::finish_field() noexcept {
    if (!in_value_) return true;
    if (state_ == State::Trailers) {
        // Trailers cannot replace framing, redirects or content coding.
        if (current_ != Field::Unknown) return reject();
    } else if (current_ == Field::Length) {
        if (has_length_ || value_size_ >= small_.size()) return reject();
        std::size_t a = 0, b = value_size_;
        while (a < b && (small_[a] == ' ' || small_[a] == '\t')) ++a;
        while (b > a && (small_[b-1] == ' ' || small_[b-1] == '\t')) --b;
        if (a == b) return reject();
        std::uint64_t n = 0;
        for (; a < b; ++a) {
            const auto c = static_cast<unsigned char>(small_[a]);
            if (c < '0' || c > '9' || n > (UINT64_MAX - (c - '0')) / 10) return reject();
            n = n * 10 + (c - '0');
        }
        has_length_ = true; length_ = n;
    } else if (current_ == Field::Transfer || current_ == Field::Encoding) {
        if (value_size_ >= small_.size()) return reject();
        std::size_t a = 0, b = value_size_;
        while (a < b && (small_[a] == ' ' || small_[a] == '\t')) ++a;
        while (b > a && (small_[b-1] == ' ' || small_[b-1] == '\t')) --b;
        if (current_ == Field::Transfer) {
            if (has_transfer_ || !equal_ci(small_.data()+a, b-a, "chunked")) return reject();
            has_transfer_ = chunked_ = true;
        } else {
            if (encoded_ || !equal_ci(small_.data()+a, b-a, "identity")) return reject();
            encoded_ = true;
        }
    } else if (current_ == Field::Location) {
        if (has_location_) return reject();
        has_location_ = true;
        // Leading/trailing OWS is not part of Location.
        std::size_t a = 0, b = value_size_;
        while (a < b && (location_[a] == ' ' || location_[a] == '\t')) ++a;
        while (b > a && (location_[b-1] == ' ' || location_[b-1] == '\t')) --b;
        if (a == b) return reject();
        std::memmove(location_.data(), location_.data()+a, b-a);
        location_[b-a] = '\0';
    }
    in_value_ = false;
    return true;
}
int Framing::parser_field(http_parser* p, const char* at, std::size_t n) noexcept {
    tk::StackWatchScope f135_callback_exit(tk::StackWatch::OtaParser);
    auto& s = self(p);
    if (s.in_value_) {
        if (!s.finish_field()) return -1;
        s.field_size_ = s.value_size_ = 0; s.current_ = Field::None;
        s.name_.fill(0); s.small_.fill(0);
    }
    if (s.field_size_ == 0) {
        if (s.field_count_ == s.limits_.fields) return s.reject(), -1;
        ++s.field_count_;
    }
    if (n > s.limits_.field_bytes - std::min<unsigned>(s.field_size_, s.limits_.field_bytes))
        return s.reject(), -1;
    for (std::size_t i = 0; i < n; ++i) {
        if (s.field_size_ < s.name_.size()-1) s.name_[s.field_size_] = at[i];
        ++s.field_size_;
    }
    return 0;
}
int Framing::parser_value(http_parser* p, const char* at, std::size_t n) noexcept {
    tk::StackWatchScope f135_callback_exit(tk::StackWatch::OtaParser);
    auto& s = self(p);
    if (!s.in_value_) {
        s.in_value_ = true;
        if (s.field_size_ >= s.name_.size()) s.current_ = Field::Unknown;
        else if (equal_ci(s.name_.data(),s.field_size_,"content-length")) s.current_ = Field::Length;
        else if (equal_ci(s.name_.data(),s.field_size_,"transfer-encoding")) s.current_ = Field::Transfer;
        else if (equal_ci(s.name_.data(),s.field_size_,"content-encoding")) s.current_ = Field::Encoding;
        else if (equal_ci(s.name_.data(),s.field_size_,"location")) s.current_ = Field::Location;
        else s.current_ = Field::Unknown;
        if (s.current_ == Field::Location && s.has_location_) return s.reject(), -1;
    }
    if (n > s.limits_.value_bytes - std::min<unsigned>(s.value_size_, s.limits_.value_bytes))
        return s.reject(), -1;
    if (s.current_ == Field::Location) {
        std::memcpy(s.location_.data()+s.value_size_, at, n);
        s.location_[s.value_size_+n] = 0;
    } else if (s.current_ != Field::Unknown) {
        if (n >= s.small_.size() - std::min<std::size_t>(s.value_size_,s.small_.size()))
            return s.reject(), -1;
        std::memcpy(s.small_.data()+s.value_size_, at, n);
        s.small_[s.value_size_+n] = 0;
    }
    s.value_size_ += static_cast<std::uint16_t>(n);
    return 0;
}
int Framing::parser_headers(http_parser* p) noexcept {
    tk::StackWatchScope f135_callback_exit(tk::StackWatch::OtaParser);
    auto& s = self(p);
    if (!s.finish_field()) return -1;
    s.field_size_ = s.value_size_ = 0; s.current_ = Field::None;
    s.status_ = p->status_code;
    if (p->upgrade || s.status_ == 101) return s.reject(), -1;
    if (s.status_ >= 100 && s.status_ < 200) {
        if (s.interims_ >= s.limits_.interim_responses || s.has_length_ || s.has_transfer_)
            return s.reject(), -1;
        ++s.interims_; s.interim_ = true;
        return 1; // Public documented HEAD/skip-body convention, not callback error.
    }
    if (s.status_ == 301 || s.status_ == 302 || s.status_ == 303 ||
        s.status_ == 307 || s.status_ == 308) {
        if (!s.has_location_) return s.reject(), -1;
        s.result_ = ParseResult::Redirect;
        http_parser_pause(p, 1); // Stop before redirect/error bodies; never drain them.
        return 0;
    }
    if (s.status_ != 200 || (s.has_length_ && s.chunked_) ||
        (s.has_length_ && (s.length_ == 0 || s.length_ > s.limits_.body_bytes)))
        return s.reject(), -1;
    if (s.chunked_) s.state_ = State::ChunkMetadata;
    else if (s.has_length_) { s.state_ = State::Body; s.remaining_ = s.length_; }
    else if (s.limits_.allow_eof_image) { s.state_ = State::Body; s.eof_body_ = true; }
    else return s.reject(), -1;
    return 0;
}
int Framing::parser_body(http_parser* p, const char* at, std::size_t n) noexcept {
    tk::StackWatchScope f135_callback_exit(tk::StackWatch::OtaParser);
    auto& s = self(p);
    if (n > s.limits_.body_bytes - std::min(s.body_bytes_,s.limits_.body_bytes) ||
        (!s.eof_body_ && n > s.remaining_)) return s.reject(), -1;
    if (n && s.sink_ && !s.sink_(s.sink_context_,reinterpret_cast<const std::uint8_t*>(at),n)) {
        s.result_ = ParseResult::SinkFailed; return -1;
    }
    s.body_bytes_ += n;
    if (!s.eof_body_) {
        s.remaining_ -= n;
        if (s.chunked_ && s.remaining_ == 0) s.state_ = State::ChunkMetadata;
    }
    return 0;
}
int Framing::parser_complete(http_parser* p) noexcept {
    tk::StackWatchScope f135_callback_exit(tk::StackWatch::OtaParser);
    auto& s = self(p);
    if (s.interim_) { s.interim_ = false; s.state_ = State::Headers; return 0; }
    if (!s.finish_field() || s.body_bytes_ == 0 || (!s.eof_body_ && !s.chunked_ && s.remaining_))
        return s.reject(), -1;
    s.state_ = State::Done; s.result_ = ParseResult::Complete;
    http_parser_pause(p,1); // Do not accept a second final response/pipelined bytes.
    return 0;
}
int Framing::parser_chunk(http_parser* p) noexcept {
    tk::StackWatchScope f135_callback_exit(tk::StackWatch::OtaParser);
    auto& s = self(p);
    // content_length is explicitly documented as readable in this callback.
    if (p->content_length > s.limits_.body_bytes - s.body_bytes_) return s.reject(), -1;
    s.remaining_ = p->content_length;
    s.line_bytes_ = 0;
    s.state_ = s.remaining_ ? State::Body : State::Trailers;
    return 0;
}
int Framing::parser_chunk_done(http_parser* p) noexcept {
    tk::StackWatchScope f135_callback_exit(tk::StackWatch::OtaParser);
    auto& s = self(p);
    if (s.state_ != State::Trailers) s.state_ = State::ChunkMetadata;
    s.line_bytes_ = 0;
    return 0;
}
bool Framing::count_metadata() noexcept {
    if (state_ == State::Headers) {
        if (header_bytes_ >= limits_.headers_bytes) return reject();
        ++header_bytes_;
    } else {
        if (chunk_bytes_ >= limits_.chunk_metadata_bytes || line_bytes_ >= limits_.chunk_line_bytes)
            return reject();
        ++chunk_bytes_; ++line_bytes_;
        // Trailers have ordinary fields/values but no chunk callbacks between lines.
    }
    return true;
}
ParseResult Framing::feed(const std::uint8_t* bytes, std::size_t n) noexcept {
    static const http_parser_settings settings = {
        parser_begin, nullptr, nullptr, parser_field, parser_value, parser_headers, parser_body, parser_complete, parser_chunk, parser_chunk_done
    };
    if (result_ != ParseResult::More) return result_;
    if (!bytes && n) return reject(), result_;
    std::size_t used = 0;
    while (used < n && result_ == ParseResult::More) {
        const bool payload = state_ == State::Body;
        const auto take = payload ? (eof_body_ ? n-used :
            static_cast<std::size_t>(std::min<std::uint64_t>(n-used, remaining_))) : 1;
        if (!take || (!payload && !count_metadata())) return reject(), result_;
        const auto parsed = http_parser_execute(&parser_, &settings,
            reinterpret_cast<const char*>(bytes+used), take);
        const auto error = HTTP_PARSER_ERRNO(&parser_);
        if (result_ == ParseResult::Redirect) return result_;
        if (result_ == ParseResult::Complete) {
            if (parsed != take || used+parsed != n) reject();
            return result_;
        }
        if (error != HPE_OK || parsed != take) {
            if (result_ == ParseResult::More) reject();
            return result_;
        }
        if (!payload && state_ == State::Trailers && bytes[used] == '\n') line_bytes_ = 0;
        used += parsed;
    }
    return result_;
}
ParseResult Framing::eof() noexcept {
    static const http_parser_settings settings = {
        parser_begin,nullptr,nullptr,parser_field,parser_value,parser_headers,parser_body,parser_complete,parser_chunk,parser_chunk_done
    };
    if (result_ != ParseResult::More) return result_;
    (void)http_parser_execute(&parser_, &settings, "", 0);
    if (result_ != ParseResult::Complete) reject();
    return result_;
}

bool valid_https_url(const char* text, http_parser_url& parsed) noexcept {
    const auto n = bounded_len(text, kUrlBytes);
    if (n == 0 || n == kUrlBytes) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c <= 32 || c == 127 || c == '\\') return false;
    }
    http_parser_url_init(&parsed);
    if (http_parser_parse_url(text,n,0,&parsed) != 0) return false;
    const auto has = [&](unsigned field) { return (parsed.field_set & (1u<<field)) != 0; };
    if (!has(UF_SCHEMA) || !has(UF_HOST) || has(UF_USERINFO) || has(UF_FRAGMENT)) return false;
    const auto schema = parsed.field_data[UF_SCHEMA];
    if (!equal_ci(text+schema.off,schema.len,"https")) return false;
    return parsed.field_data[UF_HOST].len != 0 && parsed.field_data[UF_HOST].len <= 253 &&
        (!has(UF_PORT) || parsed.port != 0);
}
bool resolve_redirect(const char* base, const char* target,
                      std::array<char, kUrlBytes>& out) noexcept {
    http_parser_url previous{}, next{};
    if (!valid_https_url(base, previous)) return false;
    const auto n = bounded_len(target, kUrlBytes);
    if (!n || n == kUrlBytes) return false;
    std::size_t used = 0; out.fill(0);
    if (n >= 8 && equal_ci(target,8,"https://")) {
        if (!append(out,used,target,n)) return false;
    } else if (n >= 2 && target[0]=='/' && target[1]=='/') {
        if (!append(out,used,"https:",6) || !append(out,used,target,n)) return false;
    } else {
        // A relative reference cannot hide another absolute scheme as a path segment.
        for (std::size_t i=0;i<n && target[i]!='/' && target[i]!='?' && target[i]!='#';++i)
            if (target[i]==':') return false;
        const auto path = previous.field_data[UF_PATH];
        std::size_t authority_end = path.len ? path.off : std::strlen(base);
        if (!path.len && (previous.field_set & (1u<<UF_QUERY)))
            authority_end = previous.field_data[UF_QUERY].off - 1;
        if (!append(out,used,base,authority_end)) return false;
        if (target[0] == '/') {
            if (!append(out,used,target,n)) return false;
        } else if (target[0] == '?') {
            if (!append(out,used,path.len ? base+path.off : "/",path.len ? path.len : 1) ||
                !append(out,used,target,n)) return false;
        } else {
            std::size_t directory = path.len;
            while (directory && base[path.off+directory-1] != '/') --directory;
            if (!append(out,used,directory ? base+path.off : "/",directory ? directory : 1) ||
                !append(out,used,target,n)) return false;
        }
    }
    if (!valid_https_url(out.data(),next)) return false;
    // Remove literal dot segments in-place; query bytes are retained verbatim. No second URL
    // buffer or percent-decoding is introduced. Encoded dot segments remain encoded.
    const auto path = next.field_data[UF_PATH];
    if (path.len) {
        std::size_t read=path.off,write=path.off,end=path.off+path.len;
        const auto starts=[&](const char* s){const auto z=std::strlen(s);return z<=end-read&&std::memcmp(out.data()+read,s,z)==0;};
        const auto pop=[&]{while(write>path.off&&out[write-1]!='/')--write;if(write>path.off)--write;};
        while(read<end) {
            if(starts("../"))read+=3;
            else if(starts("./"))read+=2;
            else if(starts("/./"))read+=2;
            else if(starts("/../")){read+=3;pop();}
            else if(end-read==2&&starts("/.")){read=end;out[write++]='/';}
            else if(end-read==3&&starts("/..")){read=end;pop();out[write++]='/';}
            else if((end-read==1&&starts("."))||(end-read==2&&starts("..")))read=end;
            else {
                if(out[read]=='/')out[write++]=out[read++];
                while(read<end&&out[read]!='/')out[write++]=out[read++];
            }
        }
        const auto tail=std::strlen(out.data()+end);
        std::memmove(out.data()+write,out.data()+end,tail+1);
    }
    return valid_https_url(out.data(),next);
}
} // namespace tk::ota
