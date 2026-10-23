#pragma once

#include <cstdint>

class NvsStorageAdapter;

// Boot-loop escalation: count fault boots and disable BLE/vehicle/MQTT at the threshold.
// Network/UI/OTA recovery remains available if its own startup succeeds. A non-fault boot
// clears the sequence only if the persisted counter is readable, valid and successfully reset.
// The healthy timer is never armed while recovery is latched.
// Decisions live in logic/boot_guard.hpp; this adapter owns storage and the boot-local latch.
// Recovery contract: docs/ARCHITECTURE.md#boot-loop-safe-mode.

namespace tk {

// Read the persisted crash-boot count, apply this boot's verdict, and persist the new value.
// Call ONCE in app_main, after NVS is up and after diag_crash_capture() (whose reset
// classification is the input), and BEFORE deciding which subsystems to start.
//   was_fault — reset_is_fault(esp_reset_reason()), i.e. did the LAST run end in a crash?
// Returns true when safe mode is latched for THIS boot.
bool safe_mode_begin(NvsStorageAdapter& config_store, bool was_fault);

// Is safe mode latched for this boot? Cheap; safe from any task. Feeds /status.sys.safe_mode and
// the web UI's recovery banner.
bool safe_mode_active();

// Start the one-shot task that clears the crash counter once this boot has stayed up for
// kBootHealthyS. Call after the components are started. Kept as a timer rather than "clear it at
// the end of app_main" on purpose: app_main returning proves the device INITIALISED, not that it
// RUNS — the crashes this guards against happen under load, minutes in, which is exactly the window
// an end-of-init clear would have declared healthy.
void safe_mode_arm_healthy_timer(NvsStorageAdapter& config_store);

}  // namespace tk
