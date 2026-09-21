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
// Discards stale incomplete buffers when the inter-chunk timeout expires (1000 ms).
// Discards buffer immediately on implausible length prefix (L == 0 or L > kDefaultMaxPayload).
// Strictly deterministic: NO heuristic scanning, NO candidate-offset shifting, NO speculative probing.
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

}  // namespace tk
