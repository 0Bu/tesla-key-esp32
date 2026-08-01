#pragma once

#include <cstdint>

class NvsStorageAdapter;

// Boot-loop safe mode — the escalation for a device that keeps CRASHING, as distinct from one that
// has run out of heap.
//
// This firmware already had one restart ladder: logic/heap_watchdog.hpp counts the restarts IT
// chooses (breadcrumb `reboot_why=heap:<n>`) and stops after five. That counter is deliberately
// narrow — it only ever sees a restart the firmware decided on. A PANIC loop, a brownout loop, a
// task-watchdog loop: none of them are counted, none of them are bounded, and each boot costs more
// than it looks like, because on THIS device a boot re-opens the vehicle polling window. A board
// that crashes every 40 seconds therefore keeps a parked car awake indefinitely and drains its
// traction battery while appearing, from the outside, merely offline.
//
// So: count crash-only boots in NVS. Past the threshold, latch SAFE MODE — app_main brings up WiFi,
// the web UI and OTA, and SKIPS the BLE/vehicle stack and the MQTT bridge. That inverts the failure:
// a bad state that crashes the vehicle path becomes a device you can still reach in a browser and
// fix (or OTA past) instead of one that needs a USB cable and physical access. It also, by
// construction, stops the polling window from re-opening.
//
// A clean or intentional reboot resets the count, and a boot that stays up past kBootHealthyS
// clears it — so a single crash, or a burst that resolves, leaves nothing behind.
//
// The DECISIONS (what counts, saturating increment, the threshold, the healthy-uptime bound) live
// in the pure, host-tested logic/boot_guard.hpp. This file is the NVS glue and the latch.

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
