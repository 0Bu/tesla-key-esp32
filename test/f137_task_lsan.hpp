#pragma once
// Host-only. Keep the real self-delete/no-unwind oracle; explicitly check before _Exit.
#include <cstdlib>
#include <cstdint>
#if defined(F137_WITH_LSAN)
#include <sanitizer/lsan_interface.h>
namespace f137_host_detail {
// A deliberate unreachable allocation, never enabled in a positive/product run. The subsequent
// recoverable check and its actual LeakSanitizer output must both be observed by the runner.
inline void* volatile lost_allocation = nullptr;
__attribute__((noinline)) inline void allocate_detector_leak() {
    lost_allocation = std::malloc(137);
    if (!lost_allocation) std::_Exit(91);
    *static_cast<volatile unsigned char*>(lost_allocation) = 0x51;
    lost_allocation = nullptr;
}
__attribute__((noinline)) inline void overwrite_detector_scratch() {
    volatile std::uintptr_t scratch[128]{};
    for (unsigned i = 0; i != 128; ++i) scratch[i] = i;
    (void)scratch[0];
}
}  // namespace f137_host_detail
#endif
inline int f137_task_leak_check(bool deliberate_detector = false) {
#if defined(F137_WITH_LSAN)
    if (deliberate_detector) {
        f137_host_detail::allocate_detector_leak();
        f137_host_detail::overwrite_detector_scratch();
    }
    return __lsan_do_recoverable_leak_check();
#else
    (void)deliberate_detector;
    return 0;
#endif
}
