#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Allocation-free JSON streaming for GET /heap. The endpoint is most valuable when contiguous
// heap is already fragmented, so it must not build hundreds of cJSON nodes and then request one
// whole-body print buffer. This pure seam is shared with host send-fault and full-history tests.
namespace tk {

struct HeapJsonStreamView {
    std::uint32_t dt_s{0};
    std::uint32_t bucket0{0};
    std::uint32_t boot_bucket{0};
    const std::int16_t* free_samples{nullptr};
    const std::int16_t* largest_samples{nullptr};
    std::size_t count{0};
    std::int16_t absent{INT16_MIN};
};

template <typename Send>
class HeapJsonChunkWriter {
public:
    explicit HeapJsonChunkWriter(Send& send) : send_(send) {}

    bool text(std::string_view value) {
        while (!value.empty()) {
            if (used_ == sizeof(buffer_) && !flush()) return false;
            const std::size_t room = sizeof(buffer_) - used_;
            const std::size_t take = value.size() < room ? value.size() : room;
            for (std::size_t i = 0; i < take; ++i) buffer_[used_ + i] = value[i];
            used_ += take;
            value.remove_prefix(take);
        }
        return true;
    }

    bool unsigned_number(std::uint32_t value) {
        char reversed[10];
        std::size_t count = 0;
        do {
            reversed[count++] = static_cast<char>('0' + value % 10u);
            value /= 10u;
        } while (value != 0);
        char digits[10];
        for (std::size_t i = 0; i < count; ++i) digits[i] = reversed[count - 1 - i];
        return text(std::string_view(digits, count));
    }

    bool signed_number(std::int16_t value) {
        if (value < 0) {
            if (!text("-")) return false;
            const std::uint32_t magnitude =
                static_cast<std::uint32_t>(-static_cast<std::int32_t>(value));
            return unsigned_number(magnitude);
        }
        return unsigned_number(static_cast<std::uint32_t>(value));
    }

    bool finish() { return flush(); }

private:
    bool flush() {
        if (used_ == 0) return true;
        if (!send_(buffer_, used_)) return false;
        used_ = 0;
        return true;
    }

    Send& send_;
    char buffer_[192]{};
    std::size_t used_{0};
};

template <typename Send>
bool stream_heap_json(const HeapJsonStreamView& view, Send send) {
    if (view.count != 0 && (!view.free_samples || !view.largest_samples)) return false;
    HeapJsonChunkWriter<Send> writer(send);
    if (!writer.text("{\"dt\":") || !writer.unsigned_number(view.dt_s) ||
        !writer.text(",\"b0\":") || !writer.unsigned_number(view.bucket0) ||
        !writer.text(",\"b_boot\":") || !writer.unsigned_number(view.boot_bucket) ||
        !writer.text(",\"unit\":\"KiB\",\"scale\":10,\"free\":[")) {
        return false;
    }
    for (std::size_t i = 0; i < view.count; ++i) {
        if (i != 0 && !writer.text(",")) return false;
        if (view.free_samples[i] == view.absent) {
            if (!writer.text("null")) return false;
        } else if (!writer.signed_number(view.free_samples[i])) {
            return false;
        }
    }
    if (!writer.text("],\"largest\":[")) return false;
    for (std::size_t i = 0; i < view.count; ++i) {
        if (i != 0 && !writer.text(",")) return false;
        if (view.largest_samples[i] == view.absent) {
            if (!writer.text("null")) return false;
        } else if (!writer.signed_number(view.largest_samples[i])) {
            return false;
        }
    }
    return writer.text("]}") && writer.finish();
}

}  // namespace tk
