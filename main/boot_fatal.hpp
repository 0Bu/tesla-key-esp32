#pragma once

// An essential startup failure is permanent for the current boot. Defined in main.cpp; declared
// here so the components app_main brings up (net.cpp) can reach the same handling instead of
// each inventing its own "give up" — the two behaviours that matter are not obvious and must
// not be re-derived per call site:
//
//   • a PENDING_VERIFY OTA image actively ROLLS BACK (merely parking the task would leave the
//     device wedged on an unverified slot until somebody resets it), and
//   • an already-valid image HALTS rather than reboots, because a reboot loop repeatedly opens
//     the vehicle polling window (draining a parked traction battery) while erasing the most
//     useful in-memory diagnostic context.
[[noreturn]] void boot_fatal(const char* component);
