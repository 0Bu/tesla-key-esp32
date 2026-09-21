#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Pure, hardware-free BLE RX framing logic shared by the firmware and the host mock build.
// Single source of truth for BLE stream reassembly according to normative reference:
// teslamotors/vehicle-command pkg/connector/ble/ble.go:67-105.
//
// Wire format: 2-byte big-endian payload length (uint16_t) followed by protobuf payload.
// Discards stale incomplete buffers when the inter-chunk timeout expires (default 1000 ms as in
// ble.go; tk::CommandRunner configures 3000 ms, a documented ADR-0005 departure).
// Discards buffer immediately on implausible length prefix (L == 0 or L > kDefaultMaxPayload).
// Strictly deterministic: NO heuristic scanning, NO candidate-offset shifting, NO speculative probing.
// The TX direction (build_ble_tx_frame / is_well_formed_ble_frame) lives at the end of this file.
namespace tk {

enum class RxFramerDropReason : uint8_t {
    None,
    InterChunkTimeout,
    CorruptLength,
    BufferOverflow,
};

struct RxFramerStats {
    uint32_t frames_completed{0};
    uint32_t timeout_drops{0};
    uint32_t corrupt_length_drops{0};
    uint32_t overflow_drops{0};
};

class RxFramer {
public:
    static constexpr size_t kHeaderSize = 2;
    static constexpr size_t kDefaultMaxPayload = 2048;
    static constexpr size_t kMaxFrameLength = kDefaultMaxPayload;
    static constexpr uint32_t kDefaultInterChunkTimeoutMs = 1000;

    explicit RxFramer(uint32_t timeout_ms = kDefaultInterChunkTimeoutMs,
                      size_t max_payload = kDefaultMaxPayload)
        : timeout_ms_(timeout_ms), max_payload_(max_payload) {}

    // Reset buffer state (e.g. on BLE disconnect or channel teardown)
    void reset() noexcept {
        buffer_.clear();
        last_chunk_time_ms_ = 0;
        has_chunk_time_ = false;
        ++reset_generation_;
    }

    // Inspect internal buffer size and statistics
    size_t buffered_bytes() const noexcept { return buffer_.size(); }
    const RxFramerStats& stats() const noexcept { return stats_; }
    uint32_t timeout_ms() const noexcept { return timeout_ms_; }

    // Standalone timeout check. Drops stale incomplete buffer if elapsed time exceeds timeout_ms_.
    // Useful for periodic task ticks without requiring incoming data.
    template <typename DropCb>
    bool check_timeout(uint32_t now_ms, DropCb&& drop_cb) {
        if (!buffer_.empty() && has_chunk_time_) {
            const uint32_t elapsed = now_ms - last_chunk_time_ms_;
            if (elapsed > timeout_ms_) {
                const size_t dropped = buffer_.size();
                buffer_.clear();
                has_chunk_time_ = false;
                stats_.timeout_drops++;
                drop_cb(RxFramerDropReason::InterChunkTimeout, dropped);
                return true;
            }
        }
        return false;
    }

    bool check_timeout(uint32_t now_ms) {
        return check_timeout(now_ms, [](RxFramerDropReason, size_t) {});
    }

    // Push an incoming chunk.
    // Calls frame_cb(const uint8_t* payload, size_t len) for each extracted frame.
    // If a drop occurs, calls drop_cb(RxFramerDropReason reason, size_t dropped_bytes).
    // Returns the number of frames successfully extracted.
    template <typename FrameCb, typename DropCb>
    size_t push_chunk(const uint8_t* data, size_t len, uint32_t now_ms,
                      FrameCb&& frame_cb, DropCb&& drop_cb) {
        // 1. Inter-chunk timeout: drop stale bytes before evaluating new input
        check_timeout(now_ms, drop_cb);

        if (!data || len == 0) {
            return 0;
        }

        // 2. Hard safety ceiling to protect against heap runaway (with integer overflow check)
        const size_t kMaxBufferSize = kHeaderSize + max_payload_ + 512;
        if (len > kMaxBufferSize || buffer_.size() > kMaxBufferSize - len) {
            const size_t dropped = buffer_.size() + len;
            buffer_.clear();
            has_chunk_time_ = false;
            stats_.overflow_drops++;
            drop_cb(RxFramerDropReason::BufferOverflow, dropped);
            return 0;
        }

        // 3. Append incoming data to reassembly buffer
        buffer_.insert(buffer_.end(), data, data + len);
        last_chunk_time_ms_ = now_ms;
        has_chunk_time_ = true;

        // 4. Extract complete frames in order
        size_t frames_extracted = 0;
        while (buffer_.size() >= kHeaderSize) {
            const uint16_t expected_payload_len =
                (static_cast<uint16_t>(buffer_[0]) << 8) | static_cast<uint16_t>(buffer_[1]);

            if (expected_payload_len == 0 || expected_payload_len > max_payload_) {
                // Implausible length prefix: drop buffer immediately without heuristic search
                const size_t dropped = buffer_.size();
                buffer_.clear();
                has_chunk_time_ = false;
                stats_.corrupt_length_drops++;
                drop_cb(RxFramerDropReason::CorruptLength, dropped);
                break;
            }

            const size_t total_frame_size = kHeaderSize + expected_payload_len;
            if (buffer_.size() < total_frame_size) {
                // Frame still incomplete; await next chunk
                break;
            }

            // Complete frame available: pass payload (excluding 2-byte header) to callback
            const uint32_t current_gen = reset_generation_;
            frame_cb(buffer_.data() + kHeaderSize, expected_payload_len);
            frames_extracted++;
            stats_.frames_completed++;

            // If reset() was invoked inside frame_cb, buffer was cleared; terminate extraction cleanly
            if (reset_generation_ != current_gen) {
                break;
            }

            buffer_.erase(buffer_.begin(), buffer_.begin() + total_frame_size);
        }

        if (buffer_.empty()) {
            has_chunk_time_ = false;
        }

        return frames_extracted;
    }

    // Convenience overload without drop callback
    template <typename FrameCb>
    size_t push_chunk(const uint8_t* data, size_t len, uint32_t now_ms, FrameCb&& frame_cb) {
        return push_chunk(data, len, now_ms, std::forward<FrameCb>(frame_cb),
                          [](RxFramerDropReason, size_t) {});
    }

private:
    uint32_t timeout_ms_;
    size_t max_payload_;
    uint32_t last_chunk_time_ms_{0};
    bool has_chunk_time_{false};
    uint32_t reset_generation_{0};
    std::vector<uint8_t> buffer_;
    RxFramerStats stats_{};
};

// ─── TX direction ────────────────────────────────────────────────────────────────────────────
// Every tesla-ble Client::build_*_message() builder already emits the complete BLE wire frame:
// the 2-byte big-endian length (Client::prepend_length) followed by the encoded
// UniversalMessage.RoutableMessage (the pairing whitelist request is a bare VCSEC
// ToVCSECMessage instead). The TX path must write that output unchanged; prepending a second
// header made every frame undecodable on the vehicle (#314 review, B1).
//
// Builds one frame into `wire`: resized to `capacity` for the builder, then to the length the
// builder reports. Returns 0 on success, the builder's non-zero code, or -1 for an empty or
// oversized output; `wire` is left empty on failure.
template <typename BuildFn>
int build_ble_tx_frame(std::vector<uint8_t>& wire, size_t capacity, BuildFn&& build) {
    wire.resize(capacity);
    size_t len = wire.size();
    const int rc = build(wire.data(), &len);
    if (rc != 0 || len == 0 || len > capacity) {
        wire.clear();
        return rc != 0 ? rc : -1;
    }
    wire.resize(len);
    return 0;
}

// Structural check of one outgoing frame before it is written: the 2-byte big-endian length must
// cover exactly the rest of the buffer, and the payload must open with a valid protobuf field tag
// (field number >= 1, wire type 0/1/2/5). A doubly length-prefixed frame fails the tag check: its
// payload starts with the inner length's high byte, at most 0x02 because tesla-ble caps a
// RoutableMessage at 741 B (a ToVCSECMessage at 106 B), which decodes as field number 0.
inline bool is_well_formed_ble_frame(const uint8_t* wire, size_t size) noexcept {
    if (wire == nullptr || size < RxFramer::kHeaderSize + 1) return false;
    const size_t declared = (static_cast<size_t>(wire[0]) << 8) | static_cast<size_t>(wire[1]);
    if (declared != size - RxFramer::kHeaderSize || declared > RxFramer::kMaxFrameLength) {
        return false;
    }
    uint64_t tag = 0;
    unsigned shift = 0;
    for (size_t i = RxFramer::kHeaderSize; i < size && shift < 35; ++i, shift += 7) {
        tag |= static_cast<uint64_t>(wire[i] & 0x7F) << shift;
        if ((wire[i] & 0x80) == 0) {
            const uint64_t wire_type = tag & 0x07;
            const uint64_t field_number = tag >> 3;
            return field_number >= 1 &&
                   (wire_type == 0 || wire_type == 1 || wire_type == 2 || wire_type == 5);
        }
    }
    return false;
}

inline bool is_well_formed_ble_frame(const std::vector<uint8_t>& wire) noexcept {
    return is_well_formed_ble_frame(wire.data(), wire.size());
}

}  // namespace tk
