#pragma once
// When may a freshly-OTA'd image cancel its own rollback? Pure, IDF-free, host-tested.
//
// WHAT THE MECHANISM IS. CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE leaves an image installed by
// esp_https_ota in ESP_OTA_IMG_PENDING_VERIFY. The bootloader reverts to the previous slot if the
// device reboots before the app calls esp_ota_mark_app_valid_cancel_rollback(). So the app's only
// decision is WHEN to make that call — and that call is irreversible: it spends the one automatic
// way back to a known-good build. A USB-flashed image boots UNDEFINED (blank otadata), never
// PENDING_VERIFY, so none of this runs for it and a fresh board — which has no previous slot — can
// never be stranded by it.
//
// WHY A TIMER IS THE WRONG ANSWER, WHICH IS WHAT THIS REPLACES. The gate used to be
// `vTaskDelay(90 s); mark_valid()`. That commits on a single piece of evidence — "the image did not
// crash" — and it is blind to the one regression that OTA cannot recover from on its own: an image
// that boots perfectly and never gets on the network. Such an image is precisely the one nobody can
// push a fix to, because the fix would have to arrive over the network it broke. It survives 90 s
// of doing nothing without difficulty, and the old gate then sealed it in as valid and threw away
// the rollback that would have undone it. The remedy at that point is a USB cable.
//
// So the health signal is CONNECTIVITY, not uptime: the image must prove it can still reach the
// transport it will be updated over. Uptime is kept as a floor, not as the proof — committing the
// instant a lease appears would ignore the other failure this gate has always caught, the image
// that boots, works for a minute and then dies under load.
//
// WHY "EXPECTED" AND NOT JUST "UP". A device with no credentials and no wire is legitimately
// offline: it is running the setup portal, which is a valid state and must not be read as a broken
// image. That is what link_expected distinguishes. On this firmware the portal never returns to
// app_main, so the gate is armed only on a device that HAS a route — but the rule belongs in the
// decision rather than in the caller's control flow, because it is the caller's control flow that
// would quietly change.
//
// WHAT GIVE-UP DOES, AND WHAT IT DELIBERATELY DOES NOT DO. Past the hard cap an unhealthy image is
// simply LEFT pending: no mark, no restart. The next reboot from any cause then rolls it back. It
// does not restart itself, because a self-restart would turn a long router outage into an automatic
// downgrade of a perfectly good build — the failure mode is silent and the operator never asked for
// it. Leaving it pending keeps the rollback armed and costs nothing while the device runs; and any
// deliberate configuration save (/set_wifi, /set_mqtt, …) calls ota_confirm_pending_image() on its
// way to a reboot, which is the manual override for someone who knows the image is fine.
#include <cstdint>

namespace tk {

enum class HealthVerdict {
    Wait,    // not enough evidence either way yet — keep observing
    Commit,  // healthy: cancel rollback and seal this image in as valid
    GiveUp,  // no health within the hard cap — leave PENDING_VERIFY, a reboot rolls it back
};

// The minimum uptime before even a healthy image commits. Long enough that an image which boots and
// then dies under load (BLE + HTTP + MQTT + the telemetry poll are all running by then) reboots
// while still pending, which is what rolls it back.
inline constexpr uint32_t kHealthGateBaseS = 90;

// How long a device with a route may fail to have one before the image is judged broken. Generous
// on purpose: this window has to outlast an ordinary router reboot or a DHCP renewal storm, because
// the cost of being wrong here is discarding a good build. Ten minutes is far past both and still
// far inside the time anyone would take to notice.
inline constexpr uint32_t kHealthGateCapS = 600;

static_assert(kHealthGateCapS > kHealthGateBaseS,
              "the hard cap must leave a window in which a healthy image can still commit");

// Decide the verdict for an image running in PENDING_VERIFY.
//   elapsed_s      seconds this image has been running (monotonic since boot)
//   base_window_s  minimum uptime before committing even a healthy image
//   hard_cap_s     stop waiting past this (must exceed base_window_s)
//   link_expected  the device HAS a configured route (credentials or a wire), so being online is
//                  the expected state; false means setup mode, where offline is correct
//   link_up        a transport currently holds a lease (tk::net_is_up() — either transport)
//
// Commit is checked before GiveUp on purpose: a device that comes online exactly at the cap has met
// the condition, and reading the two in the other order would discard it over one sample of timing.
inline HealthVerdict health_gate_decide(uint32_t elapsed_s,
                                        uint32_t base_window_s,
                                        uint32_t hard_cap_s,
                                        bool     link_expected,
                                        bool     link_up) {
    const bool healthy = link_up || !link_expected;
    if (elapsed_s >= base_window_s && healthy) return HealthVerdict::Commit;
    if (elapsed_s >= hard_cap_s)               return HealthVerdict::GiveUp;
    return HealthVerdict::Wait;
}

}  // namespace tk
