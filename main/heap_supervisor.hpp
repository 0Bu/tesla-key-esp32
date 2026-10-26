#pragma once

#include "reboot_reason.hpp"

namespace tk {

// app_main calls once, after consuming reboot_why and obtaining the BootGuard verdict, before
// any optional/network/portal startup. The checked task is essential; it never owns a Vehicle.
bool heap_supervisor_start(NvsStorageAdapter& cfg, const RebootReasonRecord& boot_reason) noexcept;

}  // namespace tk
