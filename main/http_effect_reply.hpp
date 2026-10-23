#pragma once

#include "logic/effect_reply.hpp"
#include "mcp_json_payloads.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <cstring>

namespace tk {

// Owned by handle_all OUTSIDE its try block. No borrowed handler-local data survives unwind.
struct HttpEffectReply {
    EffectReplyState state{};
    mcp_json::RpcId id{};
};
static_assert(std::is_nothrow_default_constructible<HttpEffectReply>::value,
              "record construction before the catch boundary must not throw");
static_assert(std::is_trivially_copyable<HttpEffectReply>::value,
              "request outcome must not acquire ownership or a throwing copy");
static_assert(sizeof(HttpEffectReply) <= 88, "bounded request outcome and RPC id");

// Transport returns zero on success for every operation. terminal is set before headers too:
// even a header failure/throw aborts this logical response. No fallback after a partial write.
template <typename Transport>
int begin_effect_reply(Transport& transport, HttpEffectReply& reply, int status) {
    if (reply.state.terminal) return -1;
    reply.state.terminal = true;
    int result = transport.set_status(status);
    if (result != 0) return result;
    return transport.set_json_type();
}

template <typename Transport>
int write_effect_id(Transport& transport, const mcp_json::RpcId& id) {
    if (id.kind == mcp_json::RpcIdKind::Number) {
        // Envelope validation already restricted the value to +/-9007199254740991.
        // Avoid printf/string/cJSON allocation on the degraded path.
        char digits[22];
        size_t cursor = sizeof(digits);
        const bool negative = id.number < 0;
        uint64_t value = negative ? uint64_t(-(id.number + 1)) + 1 : uint64_t(id.number);
        do { digits[--cursor] = char('0' + value % 10); value /= 10; } while (value);
        if (negative) digits[--cursor] = '-';
        return transport.chunk(digits + cursor, sizeof(digits) - cursor);
    }
    if (id.kind != mcp_json::RpcIdKind::String) return transport.chunk("null", 4);
    int result = transport.chunk("\"", 1);
    if (result != 0) return result;
    const char* run = id.string;
    size_t length = 0;
    while (length < mcp_json::kMaxStringIdBytes && id.string[length]) ++length;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(id.string[i]);
        if (c >= 0x20 && c != '\\' && c != '"') continue;
        if (run != id.string + i) {
            result = transport.chunk(run, size_t(id.string + i - run));
            if (result != 0) return result;
        }
        char escaped[6] = {'\\', 'u', '0', '0', '0', '0'};
        size_t count = 6;
        if (c == '\\' || c == '"') { escaped[1] = char(c); count = 2; }
        else {
            constexpr char hex[] = "0123456789abcdef";
            escaped[4] = hex[c >> 4]; escaped[5] = hex[c & 15];
        }
        result = transport.chunk(escaped, count);
        if (result != 0) return result;
        run = id.string + i + 1;
    }
    if (run != id.string + length) {
        result = transport.chunk(run, size_t(id.string + length - run));
        if (result != 0) return result;
    }
    return transport.chunk("\"", 1);
}

// Project renderer allocates no memory. IDF networking may allocate or fail; this does not prove
// receipt, physical command execution or prevent an external client's own retries.
template <typename Transport>
int degraded_effect_reply(Transport& transport, HttpEffectReply& reply) {
    int result = begin_effect_reply(transport, reply, 200);
    if (result != 0) return result;
    const auto write = [&](const char* value) {
        if (result == 0) result = transport.chunk(value, std::strlen(value));
    };
    const bool rpc = reply.state.shape == ReplyShape::Rpc;
    if (rpc) {
        write("{\"jsonrpc\":\"2.0\",\"id\":");
        if (result == 0) result = write_effect_id(transport, reply.id);
        write(",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"");
        write(reply.state.reason());
        write(reply.state.success() ? "\"}],\"isError\":false," : "\"}],\"isError\":true,");
    } else {
        write(reply.state.shape == ReplyShape::WrappedRest ? "{\"response\":{" : "{");
    }
    write(reply.state.success() ? "\"result\":true" : "\"result\":false");
    if (reply.state.shape == ReplyShape::FlatResult)
        write(reply.state.success() ? ",\"ok\":true" : ",\"ok\":false");
    if (reply.state.shape == ReplyShape::OtaCheck)
        write(reply.state.success() ? ",\"started\":true" : ",\"started\":null");
    write(",\"outcome\":\""); write(reply.state.outcome_text());
    write("\",\"reply_degraded\":true,\"reason\":\""); write(reply.state.reason()); write("\"");
    if (reply.state.image != ConfigImageResult::None) {
        write(",\"firmware_verification\":\"");
        write(config_image_code(reply.state.image)); write("\"");
    }
    if (reply.state.recovery_pending) write(",\"recovery_pending\":true");
    write(rpc || reply.state.shape == ReplyShape::WrappedRest ? "}}" : "}");
    if (result != 0) return result;
    return transport.chunk(nullptr, 0);
}

// Actual cJSON production serialization, with effect-aware fallback BEFORE any headers/bytes.
// Normal complete bodies retain their original status and envelope. A transport error is terminal.
template <typename Transport>
int effect_json_reply(Transport& transport, HttpEffectReply& reply, int status, JsonOwner root) {
    JsonPrintOwner body(root ? cJSON_PrintUnformatted(root.get()) : nullptr);
    if (!body) return degraded_effect_reply(transport, reply);
    const int result = begin_effect_reply(transport, reply, status);
    if (result != 0) return result;
    return transport.send(body.get());
}

}  // namespace tk
