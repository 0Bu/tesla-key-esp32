#pragma once
// The boot-loop safe-mode decision: count CRASH-ONLY boots and, past a threshold, latch a
// degraded-but-STABLE mode. Pure, IDF-free, host-testable.
//
// WHY THIS EXISTS. Everything this device is configured with lives in NVS and is reachable only
// over the network: the VIN (/set_vin), the MQTT broker (/set_mqtt), the syslog server
// (/set_syslog), the WiFi credentials, the key material. There is no auth and no console — the
// whole recovery story is "open the web UI and fix it". A setting that crashes a task at start-up
// breaks exactly that: the device reboots before the HTTP server is up, so the one interface that
// could undo the setting is never reachable, and the only remaining exit is `esptool erase_flash`
// over USB — which also destroys the private key and the pairing, i.e. a trip to the car.
//
// So: count boots that ended in a CRASH, and once enough of them have accumulated with no healthy
// run in between, come up MINIMALLY — WiFi + web UI + OTA only, with the risky subsystems (the BLE
// client / VehicleController, the auto-pair task, the MQTT bridge) skipped. A device in safe mode
// is useless as an evcc proxy and that is the point: it is reachable, it says why, and the setting
// or the image that wedged it can be replaced from a browser.
//
// WHY THE CONSEQUENCE IS SHARPER HERE THAN ON A BOARD THAT ONLY LOSES ITS OWN SERVICE. A reboot
// re-opens the active vehicle-polling window (vehicle_ctrl seeds it at boot), so a device that
// reboots in a loop keeps opening an infotainment session on a PARKED car. The car never reaches
// sleep and its battery drains — the firmware damages the thing it exists to manage, silently, for
// as long as the loop runs. logic/heap_watchdog.hpp already refuses to seed that window on its own
// restarts for this reason; the same argument applies with more force to a loop nobody chose. A
// latched, boring safe mode is therefore strictly better than continuing to crash: it stops the
// cycling, and the car gets to sleep while the operator works out what happened.
//
// TWO RESTART COUNTERS, DELIBERATELY SEPARATE — do not merge them.
//   * logic/heap_watchdog.hpp caps ITS OWN restarts at kHeapMaxConsecutiveRestarts (5). That
//     counter is persisted as the `heap:<n>` breadcrumb and counts exactly one thing: restarts WE
//     chose, after five unbroken minutes of an unusable heap. It is a bound on a deliberate
//     self-heal.
//   * This counter covers the other, much larger class: restarts the SYSTEM forced — a panic, an
//     interrupt/task watchdog, a brownout. Nothing counts those today, so a firmware that panics
//     four seconds into every boot loops forever with no ceiling and no breadcrumb.
// Keeping them apart matters in both directions. Folding the heap watchdog's restarts in here
// would push a device that legitimately self-healed twice toward safe mode, i.e. punish the
// recovery; folding a panic loop into the heap counter would attribute it to a heap verdict the
// watchdog never reached. One counts an action, the other counts an accident.
//
// WHAT COUNTS AS A CRASH IS THE CALLER'S CALL — and it is the correctness point. `was_fault` is
// passed in as a bool rather than derived from a reset code here, so this header stays free of
// esp_system.h; the device glue maps esp_reset_reason() and must map it NARROWLY:
//   count    — ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_BROWNOUT.
//   ignore   — ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_USB/JTAG, ESP_RST_DEEPSLEEP, and above all
//              ESP_RST_SW: /set_vin, /set_mqtt, /set_syslog, /set_wifi and the setup portal each
//              persist-and-reboot, and an OTA install reboots too, so ordinary provisioning is a
//              BURST of software reboots. Read
//              as crashes they would latch safe mode on a device nobody has broken — the one
//              failure this guard must never have.
//   ignore   — the heap watchdog's own restart (also ESP_RST_SW, and identifiable by the
//              `reboot_why=heap:<n>` breadcrumb): it is bounded by its own cap, see above.
//
// WHAT IT DOES NOT COVER, stated rather than implied. The counter is cleared once a boot has run
// healthily for kBootHealthyS, so a loop whose crashes arrive LATER than that is invisible to it —
// by construction, this is an instrument for a crash during start-up. The other class, an image
// that boots and then dies under load, is what the OTA rollback health gate handles
// (ota_health_gate_task defers esp_ota_mark_app_valid_cancel_rollback() by ~90 s, so the bootloader
// reverts it). The two are complementary and neither subsumes the other: rollback only arms after
// an update and can only undo an IMAGE, while both OTA slots read the same `tesla_cfg` NVS — so a
// bad persisted setting survives every rollback and needs this instead.
#include <cstdint>

namespace tk {

// Crash boots needed to latch safe mode. Four, not one: a single panic is an event (a cosmic ray,
// a brownout on a marginal USB supply, one unlucky allocation) and coming up degraded because of it
// would cost the user their evcc integration for no reason. Four consecutive crash boots with no
// healthy run in between is a pattern, and the device reaches it in well under a minute — the loop
// is not left running for long.
inline constexpr unsigned kBootFailThreshold = 4;

// Continuous uptime that proves THIS boot is good, after which the glue clears the counter. Long
// enough to be past everything a bad configuration crashes in — NVS load, WiFi association, the
// BLE stack and the first pairing/telemetry pass all complete in a few seconds — and deliberately
// short, because the window cuts both ways: a value large enough to span a working device's normal
// operation would let unrelated crashes weeks apart accumulate to the threshold and latch safe mode
// on a healthy board. Shorter than the ~90 s OTA health gate on purpose; they answer different
// questions (see the header note).
inline constexpr uint32_t kBootHealthyS = 30;

// Where the stored counter saturates. It exists so a long loop cannot overflow the persisted value
// and wrap back under the threshold — the one arithmetic accident that would silently un-latch
// safe mode on the boot that needed it most.
inline constexpr unsigned kBootFailMax = 100;

// Parse only the canonical decimal strings store_count() can produce. A present-but-empty,
// signed, prefixed, overflowing or out-of-range value is storage ambiguity, not zero: the boot
// glue fails closed into safe mode rather than authorizing the vehicle/MQTT stack on a possible
// crash loop.
inline bool boot_fail_parse(const char* raw, unsigned& out) noexcept {
    if (!raw || *raw == '\0') return false;
    if (*raw == '0') {
        if (raw[1] != '\0') return false;
        out = 0;
        return true;
    }
    unsigned value = 0;
    for (const char* p = raw; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        const unsigned digit = static_cast<unsigned>(*p - '0');
        if (value > (kBootFailMax - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    out = value;
    return true;
}

// The next value of the persisted crash counter. A fault boot increments (saturating); ANY other
// boot resets to 0, because the reset is what makes this a consecutive-failure counter rather than
// a lifetime crash tally — a device that crashed twice last winter and has run since is not in a
// loop, and must not be one panic away from coming up crippled.
//
// Production calls this only after boot_fail_parse. The defensive clamp still fails toward the
// latch if another caller ever passes an out-of-contract value directly.
inline constexpr unsigned boot_fail_next(unsigned stored, bool was_fault) {
    if (!was_fault) return 0;
    const unsigned cur = (stored > kBootFailMax) ? kBootFailMax : stored;
    return (cur >= kBootFailMax) ? kBootFailMax : cur + 1u;
}

// Latched once the count reaches the threshold — and it stays latched on every subsequent boot,
// since only a healthy run (or a clean reset) ever writes the counter back down. No off-by-one:
// the glue increments FIRST and asks about the result, so with a threshold of 4 the 4th crash boot
// comes up in safe mode.
inline constexpr bool boot_safe_mode(unsigned count) {
    return count >= kBootFailThreshold;
}

// Has this boot earned the right to clear the counter? Sampled from a timer/task against monotonic
// uptime. Not a one-shot at start-up: the point is that the device SURVIVED the window, so it can
// only be answered by still being alive at the end of it.
inline constexpr bool boot_healthy_elapsed(uint32_t uptime_s) {
    return uptime_s >= kBootHealthyS;
}

}  // namespace tk
