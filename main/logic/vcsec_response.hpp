#pragma once

#include "ble_dispatcher.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

// Pure, hardware-free logic shared by the firmware and host mock build. Keep this file free of
// IDF/FreeRTOS/NimBLE/NVS/cJSON dependencies. VehicleController delegates request capture and
// reply admission here; hardware effects remain in its native shell.
namespace tk {

enum class VcsecReplyKind : uint8_t { Action, Status, SessionInfo, WakePrerequisite };
enum class VcsecRequestPayload : uint8_t { Protobuf, SessionInfoRequest };

struct VcsecRequestMetadata {
    BleUuid reply_address{};
    BleUuid uuid{};
    VcsecRequestPayload payload{VcsecRequestPayload::Protobuf};
};

inline bool vcsec_reply_address_is_fresh(const BleUuid& candidate,
                                         const BleUuid& previous) noexcept {
    return !is_zero_uuid(candidate) && candidate != previous;
}

namespace vcsec_detail {

// Bounded protobuf scanning for routing metadata only. Payloads are skipped in place; no
// RoutableMessage-sized stack object or allocation is needed. Tags are pinned by the real-library
// harness against universal_message.pb.h and actual serialized request frames.
class Cursor {
public:
    Cursor(const uint8_t* data, size_t size) noexcept : data_(data), size_(size) {}
    bool done() const noexcept { return offset_ == size_; }
    bool varint(uint64_t& value) noexcept {
        value = 0;
        for (unsigned byte = 0; byte < 10; ++byte) {
            if (offset_ == size_) return false;
            const uint8_t next = data_[offset_++];
            if (byte == 9 && next > 1) return false;
            value |= static_cast<uint64_t>(next & 0x7f) << (7 * byte);
            if (!(next & 0x80)) return byte == 0 || next != 0;
        }
        return false;
    }
    bool tag(uint32_t& field, uint8_t& wire) noexcept {
        uint64_t key = 0;
        if (!varint(key) || (key >> 3) == 0 || (key >> 3) > 0x1fffffff) return false;
        field = static_cast<uint32_t>(key >> 3);
        wire = static_cast<uint8_t>(key & 7);
        return true;
    }
    bool bytes(const uint8_t*& data, size_t& size) noexcept {
        uint64_t length = 0;
        if (!varint(length) || length > size_ - offset_) return false;
        size = static_cast<size_t>(length);
        data = data_ + offset_;
        offset_ += size;
        return true;
    }
    bool skip(uint8_t wire) noexcept {
        uint64_t ignored = 0;
        const uint8_t* data = nullptr;
        size_t size = 0;
        switch (wire) {
            case 0: return varint(ignored);
            case 1: return fixed(8);
            case 2: return bytes(data, size);
            case 5: return fixed(4);
            default: return false;
        }
    }
private:
    bool fixed(size_t size) noexcept {
        if (size > size_ - offset_) return false;
        offset_ += size;
        return true;
    }
    const uint8_t* data_;
    size_t size_;
    size_t offset_{0};
};

inline bool destination(const uint8_t* data, size_t size, bool source,
                        BleUuid& address) noexcept {
    Cursor input(data, size);
    bool seen = false;
    while (!input.done()) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!input.tag(field, wire)) return false;
        if (field == 1) {  // Destination.domain
            uint64_t domain = 0;
            if (source || seen || wire != 0 || !input.varint(domain) ||
                domain != static_cast<uint8_t>(BleDomain::VehicleSecurity)) return false;
            seen = true;
        } else if (field == 2) {  // Destination.routing_address
            const uint8_t* bytes = nullptr;
            size_t length = 0;
            if (!source || seen || wire != 2 || !input.bytes(bytes, length) ||
                length != address.size()) return false;
            std::memcpy(address.data(), bytes, address.size());
            if (is_zero_uuid(address)) return false;
            seen = true;
        } else if (!input.skip(wire)) {
            return false;
        }
    }
    return seen;
}

}  // namespace vcsec_detail

// Accept one complete length-prefixed routable VCSEC request, never raw WhitelistAdd.
inline bool capture_vcsec_request(const uint8_t* frame, size_t size,
                                  VcsecRequestMetadata& out) noexcept {
    out = {};
    if (!frame || size < 2 || size > 2050 ||
        (static_cast<size_t>(frame[0]) << 8 | frame[1]) != size - 2) return false;
    VcsecRequestMetadata metadata;
    vcsec_detail::Cursor input(frame + 2, size - 2);
    bool destination = false, source = false, uuid = false, payload = false;
    while (!input.done()) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!input.tag(field, wire)) return false;
        if (field == 6 || field == 7 || field == 10 || field == 14 || field == 51) {
            const uint8_t* data = nullptr;
            size_t length = 0;
            if (wire != 2 || !input.bytes(data, length)) return false;
            if (field == 6) {
                if (destination || !vcsec_detail::destination(data, length, false, metadata.reply_address)) return false;
                destination = true;
            } else if (field == 7) {
                if (source || !vcsec_detail::destination(data, length, true, metadata.reply_address)) return false;
                source = true;
            } else if (field == 51) {
                if (uuid || length != metadata.uuid.size()) return false;
                std::memcpy(metadata.uuid.data(), data, metadata.uuid.size());
                if (is_zero_uuid(metadata.uuid)) return false;
                uuid = true;
            } else {
                if (payload || length == 0) return false;
                metadata.payload = field == 10 ? VcsecRequestPayload::Protobuf
                                               : VcsecRequestPayload::SessionInfoRequest;
                payload = true;
            }
        } else if (field == 15 || field == 50 || !input.skip(wire)) {
            return false;  // A reply payload/request_uuid is not an outbound request.
        }
    }
    if (!(destination && source && uuid && payload)) return false;
    out = metadata;
    return true;
}

struct VcsecAttemptState {
    uint32_t generation{0};
    uint32_t command_id{0};
    uint32_t attempt{0};
    VcsecReplyKind kind{VcsecReplyKind::Action};
    bool executing{false};
    uint32_t now_ms{0};
    uint32_t enqueued_at_ms{0};
    uint32_t total_timeout_ms{0};
    uint32_t phase_started_at_ms{0};
    uint32_t phase_timeout_ms{0};
};

class VcsecReplyContext {
public:
    void retire() noexcept { *this = {}; }
    bool prepare(const VcsecRequestMetadata& metadata, const BleUuid& expected_address,
                 uint32_t generation, uint32_t command_id, uint32_t attempt,
                 VcsecReplyKind kind) noexcept {
        retire();
        if (!command_id || !attempt || is_zero_uuid(expected_address) ||
            metadata.reply_address != expected_address || is_zero_uuid(metadata.uuid) ||
            (kind != VcsecReplyKind::Action && kind != VcsecReplyKind::Status &&
             kind != VcsecReplyKind::SessionInfo && kind != VcsecReplyKind::WakePrerequisite) ||
            (kind == VcsecReplyKind::SessionInfo
                 ? metadata.payload != VcsecRequestPayload::SessionInfoRequest
                 : metadata.payload != VcsecRequestPayload::Protobuf)) return false;
        reply_address_ = metadata.reply_address;
        uuid_ = metadata.uuid;
        generation_ = generation;
        command_id_ = command_id;
        attempt_ = attempt;
        kind_ = kind;
        prepared_ = true;
        return true;
    }
    void transmitted() noexcept { transmitted_ = prepared_; }
    VcsecReplyKind kind() const noexcept { return kind_; }
    bool matches(const VcsecAttemptState& state, const uint8_t* address, size_t address_size,
                 const uint8_t* uuid, size_t uuid_size) noexcept {
        // A caller may ask several typed admission questions for one received frame.
        // A different question must not invalidate the active request's real phase.
        if (!prepared_ || !transmitted_ || state.kind != kind_) return false;
        if (!state.executing || state.generation != generation_ || state.command_id != command_id_ ||
            state.attempt != attempt_ ||
            (state.total_timeout_ms && state.now_ms - state.enqueued_at_ms >= state.total_timeout_ms) ||
            !state.phase_timeout_ms || state.now_ms - state.phase_started_at_ms >= state.phase_timeout_ms) {
            retire();
            return false;
        }
        if (!address || address_size != reply_address_.size() ||
            std::memcmp(address, reply_address_.data(), address_size) != 0) return false;
        return uuid_size == 0 ||
               (uuid && uuid_size == uuid_.size() &&
                std::memcmp(uuid, uuid_.data(), uuid_size) == 0);
    }
private:
    BleUuid reply_address_{};
    BleUuid uuid_{};
    uint32_t generation_{0};
    uint32_t command_id_{0};
    uint32_t attempt_{0};
    VcsecReplyKind kind_{VcsecReplyKind::Action};
    bool prepared_{false};
    bool transmitted_{false};
};

}  // namespace tk
