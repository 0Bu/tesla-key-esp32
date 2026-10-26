#pragma once

#include "ble_chunk.hpp"
#include "ble_deferred_event.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Pure C++17, hardware-free FIFO. The firmware shell must serialize every call.
// No RTOS, IDF, allocation, callback, logging or storage operation belongs here.
namespace tk {

class BleDeferredQueue {
public:
    static constexpr std::size_t kDepth = 16;
    static constexpr std::size_t kCellBytes = kBleMaxWritePayload;
    static constexpr std::size_t kSliceBytes = kBleDefaultWritePayload;
    static_assert(kCellBytes == 244 && kSliceBytes == 20);

    struct Event {
        std::uint32_t generation{0};
        std::uint16_t conn_handle{UINT16_MAX};
        std::uint16_t size{0};
        BleDeferredEventKind kind{BleDeferredEventKind::LinkDown};
        std::array<std::uint8_t, kSliceBytes> data{};
    };

    bool empty() const noexcept { return count_ == 0; }
    std::size_t cells_used() const noexcept { return count_; }

    void clear() noexcept {
        head_ = count_ = read_offset_ = 0;
    }

    bool push_link(bool connected, std::uint16_t handle,
                   std::uint32_t generation) noexcept {
        if (count_ == kDepth) return false;
        Cell& cell = cells_[index(count_)];
        cell.generation = generation;
        cell.conn_handle = handle;
        cell.size = 0;
        cell.kind = connected ? BleDeferredEventKind::LinkUp
                              : BleDeferredEventKind::LinkDown;
        ++count_;
        return true;
    }

    bool push_rx(const std::uint8_t* data, std::size_t size,
                 std::uint32_t generation) noexcept {
        if (!data || size == 0 || size > kCellBytes) return false;
        Cell* tail = count_ ? &cells_[index(count_ - 1)] : nullptr;
        const std::size_t space = tail && tail->kind == BleDeferredEventKind::Rx &&
                                  tail->generation == generation
                                      ? kCellBytes - tail->size : 0;
        const std::size_t append = size < space ? size : space;
        // A callback is accepted in full or not at all. At most one new cell is
        // needed; do not modify even the old tail before proving capacity.
        if (append != size && count_ == kDepth) return false;
        if (append != 0) {
            std::memcpy(tail->data.data() + tail->size, data, append);
            tail->size = static_cast<std::uint16_t>(tail->size + append);
        }
        if (append != size) {
            Cell& cell = cells_[index(count_)];
            cell.generation = generation;
            cell.conn_handle = UINT16_MAX;
            cell.size = static_cast<std::uint16_t>(size - append);
            cell.kind = BleDeferredEventKind::Rx;
            std::memcpy(cell.data.data(), data + append, size - append);
            ++count_;
        }
        return true;
    }

    // Failed pop leaves its output untouched. A successful pop owns a complete
    // link barrier or at most20 RX bytes; no reference into storage can escape.
    bool pop(Event& out) noexcept {
        if (count_ == 0) return false;
        const Cell& cell = cells_[head_];
        out = {};
        out.generation = cell.generation;
        out.conn_handle = cell.conn_handle;
        out.kind = cell.kind;
        if (cell.kind == BleDeferredEventKind::Rx) {
            const std::size_t remaining = cell.size - read_offset_;
            const std::size_t size = remaining < kSliceBytes ? remaining : kSliceBytes;
            out.size = static_cast<std::uint16_t>(size);
            std::memcpy(out.data.data(), cell.data.data() + read_offset_, size);
            const std::size_t next = read_offset_ + size;
            if (next != cell.size) {
                read_offset_ = static_cast<std::uint8_t>(next);
                return true;
            }
        }
        head_ = static_cast<std::uint8_t>((head_ + 1) % kDepth);
        --count_;
        read_offset_ = 0;
        return true;
    }

private:
    // No nonzero member defaults: static backing remains zero-initializable
    // without a repeated4KiB LinkDown/invalid-handle constant template.
    struct Cell {
        std::uint32_t generation;
        std::uint16_t conn_handle;
        std::uint16_t size;
        BleDeferredEventKind kind;
        std::array<std::uint8_t, kCellBytes> data;
    };
    static_assert(sizeof(Cell) == 256);
    std::array<Cell, kDepth> cells_{};
    std::uint8_t head_{0};
    std::uint8_t count_{0};
    std::uint8_t read_offset_{0};

    std::size_t index(std::size_t offset) const noexcept {
        return (head_ + offset) % kDepth;
    }
};

static_assert(sizeof(BleDeferredQueue::Event) <= 32);
static_assert(sizeof(BleDeferredQueue) <= 4100);

}  // namespace tk
