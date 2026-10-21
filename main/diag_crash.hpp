#pragma once

#include "logic/crashinfo.hpp"
#include "logic/effect_reply.hpp"
#include <esp_partition.h>

// Capture once before service tasks exist. Boot data stays immutable; live readers
// query only bounded headers and recheck atomic admission after borrowing the immutable payload.
namespace tk {
struct CrashResetInfo { int code; bool fault; };
void diag_crash_capture() noexcept;
// The reset facts are immutable after capture; the reference has static lifetime.
const CrashResetInfo& diag_crash_reset_info() noexcept;
CrashInfoView diag_crash_info_live();

// Nonblocking request ownership, not a leaf cache lock. Hold across download I/O;
// no other download/delete may enter. The type cannot be copied or forged as held.
class CoredumpLease {
public:
    CoredumpLease() noexcept;
    ~CoredumpLease();
    CoredumpLease(const CoredumpLease&) = delete;
    CoredumpLease& operator=(const CoredumpLease&) = delete;
    bool held() const noexcept { return held_; }
private:
    bool held_;
};
struct CoredumpReadBuffer {
    char bytes[256]{};
    ~CoredumpReadBuffer() noexcept {
        volatile char* out = bytes;
        for (size_t i = 0; i < sizeof(bytes); ++i) out[i] = 0;
    }
};
struct CoredumpDownload { const esp_partition_t* partition{nullptr}; uint32_t bytes{0}; };
enum class CoredumpOpenResult { Ready, Busy, Absent, Unverified, Error };
CoredumpOpenResult diag_crash_open_dump(const CoredumpLease&, bool raw, CoredumpDownload&);
CrashEraseResult diag_crash_erase(bool dismiss, EffectReplyState&) noexcept;
}  // namespace tk
