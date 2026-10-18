#pragma once

#include <algorithm>
#include <cstdint>

// Pure, hardware-free logic shared by the firmware and the host-side mock build
// (test/, built without ESP-IDF). Anything in this directory must stay free of IDF,
// FreeRTOS, NimBLE, NVS, cJSON and esp_http_server includes so it compiles with a
// plain host toolchain. See test/README.md and the project AGENTS.md.
namespace tk {

// Setup-portal policy after a boot-time WiFi failure with stored credentials (#386, F-028).
//
// A boot with stored credentials, no pending rollback and no wired lease gives the access point
// one boot window (kWifiBootWindowS). A shared power cut is the common way to miss it: the board
// boots faster than the router. The portal used to be terminal from then on — no timeout and no
// retry — so the device stayed offline, with an open setup AP, until someone power-cycled it.
//
// The portal now keeps the stored credentials in use instead of surrendering to the form:
//   * The station retries the stored pair in the background: the first attempt kPortalRetryFirstS
//     after the portal opens, then with doubling backoff capped at kPortalRetryMaxS.
//   * Before the open-time cap, this policy starts no new attempt while a phone is on the setup
//     AP. The station event handler can still finish its remaining boot retry budget when the
//     boot window timed out first (net.hpp); this is not exclusive ownership of station retries.
//   * The open setup AP closes after kPortalIdleCloseS without a client, and at the latest
//     kPortalApMaxOpenS after it opened even with one, which bounds its window (docs/SECURITY.md).
//     The cap also keeps a station that stays associated (anyone in range, or a phone that
//     remembered the setup network) from deferring the retries indefinitely. Background retries
//     continue, and a power cycle reopens the AP.
//   * Once the network is up the portal is left at once, even with a phone on the AP, and the
//     boot continues in place. The form is no longer needed, and its server would otherwise also
//     answer on the LAN lease without the browser gate of the normal web server.
//
// Deliberately no reboot. A timed restart would revert a pending OTA image (the portal never
// confirms one), reset the crash counter so a fault loop could never reach safe mode, re-seed the
// vehicle active window on every cycle, and put the armed credential backup at risk.
//
// First-time setup and unusable stored credentials do not use this policy: that portal stays
// open until the form is saved, which reboots (provisioning_run()).

inline constexpr uint32_t kPortalRetryFirstS = 30;
inline constexpr uint32_t kPortalRetryMaxS   = 300;
inline constexpr uint32_t kPortalIdleCloseS  = 600;
inline constexpr uint32_t kPortalApMaxOpenS  = 1800;

struct PortalInput {
    bool     network_up;  // a WiFi or wired lease exists
    bool     ap_open;     // the setup AP is still advertised
    uint32_t ap_clients;  // stations associated with the setup AP
    uint32_t now_s;       // seconds since the portal opened (monotonic)
};

struct PortalState {
    uint32_t next_attempt_s  = kPortalRetryFirstS;
    uint32_t backoff_s       = kPortalRetryFirstS;
    uint32_t idle_since_s    = 0;  // when the AP last had a client (or opened)
    uint32_t attempts        = 0;
    uint32_t close_due_s     = 0;  // earliest next AP close after a failed one
    uint32_t close_backoff_s = 0;
};

enum class PortalAction {
    None,
    AttemptStation,  // start one association attempt with the stored credentials
    CloseAp,         // stop advertising the setup AP; keep retrying in the background
    Leave,           // tear the portal down and continue the boot
};

// One decision per tick. Leave wins over CloseAp, which wins over an attempt.
//
// Closing the AP can fail (the HTTP server stop or WiFi mode switch). The shell then reports it with
// portal_close_failed() and keeps the AP marked open: the next close (CloseAp, or Leave while the
// AP is open) waits with the same doubling backoff as attempts, so a persistent failure neither
// crowds out the attempts nor floods the log, and the boot never continues with the AP up.
inline PortalAction portal_step(PortalState& s, const PortalInput& in) {
    const bool close_due = in.now_s >= s.close_due_s;
    if (in.network_up) return !in.ap_open || close_due ? PortalAction::Leave : PortalAction::None;
    if (in.ap_clients > 0) s.idle_since_s = in.now_s;
    if (in.ap_open && close_due && in.now_s >= kPortalApMaxOpenS) return PortalAction::CloseAp;
    // Past the cap the form is gone (the close stops it first), so a station still associated
    // because the close failed no longer defers attempts.
    if (in.ap_clients > 0 && in.now_s < kPortalApMaxOpenS) return PortalAction::None;
    if (in.ap_open && close_due && in.now_s - s.idle_since_s >= kPortalIdleCloseS) return PortalAction::CloseAp;
    if (in.now_s < s.next_attempt_s) return PortalAction::None;
    ++s.attempts;
    s.backoff_s      = std::min(kPortalRetryMaxS, s.backoff_s * 2);
    s.next_attempt_s = in.now_s + s.backoff_s;
    return PortalAction::AttemptStation;
}

// The AP close requested by CloseAp or Leave failed; the AP is still up.
inline void portal_close_failed(PortalState& s, uint32_t now_s) {
    s.close_backoff_s = s.close_backoff_s ? std::min(kPortalRetryMaxS, s.close_backoff_s * 2)
                                          : kPortalRetryFirstS;
    s.close_due_s = now_s + s.close_backoff_s;
}

}  // namespace tk
