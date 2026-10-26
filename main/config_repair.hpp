#pragma once
#include <cstddef>
#include <cstdint>

namespace tk::repair {
// The physical console owner must implement these as raw, non-echoing, bounded local I/O.
// Return false on deadline, disconnect, partial I/O or driver failure. No HTTP/network adapter.
// Absolute deadlines are esp_timer_get_time() microseconds; the route must independently enforce
// them (a blocking read hidden in this interface is forbidden). This interface is not itself a
// qualified USB driver: each target/board console route remains an explicit integration requirement.
struct Console {
    virtual bool read_exact(std::uint8_t*,std::size_t,std::uint64_t deadline_us) noexcept=0;
    virtual bool write_exact(const std::uint8_t*,std::size_t,std::uint64_t deadline_us) noexcept=0;
    virtual ~Console()=default;
};
// Called only by the selected maintenance build before normal startup. Never returns to normal
// startup or restarts; all results park awaiting a separately authorized app-only USB restoration.
[[noreturn]] void run(Console& console) noexcept;
[[noreturn]] void console_entry() noexcept;
}
