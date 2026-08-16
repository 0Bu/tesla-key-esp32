#pragma once

#include <cstdint>

// Pure, hardware-free logic shared by the firmware and the host-side mock build
// (test/, built without ESP-IDF). Anything in this directory must stay free of IDF,
// FreeRTOS, NimBLE, NVS, cJSON and esp_http_server includes so it compiles with a
// plain host toolchain. See test/README.md and the project AGENTS.md.
namespace tk {

// WiFi credential-ROLLBACK policy: on the boot that follows a credential change, when does
// "still no IP" mean the NEW credentials are WRONG (restore the one-shot backup), and when does
// it only mean the network is not back YET (keep waiting)?
//
// This guards the one setting a user can lock themselves out with. Changing the WiFi credentials
// over the LAN is a one-way jump: the device reboots into the new SSID and, if it cannot join,
// the only route back in is the setup AP — i.e. physically walking to the board and re-typing
// everything. The cure is a one-shot backup of the last working credentials plus a rule for
// spending it, and the RULE is the whole difficulty, because the two failure modes look
// identical from here: no IP, no route, nothing to poll, no way to ask anybody.
//
// A blind deadline gets the COMMON case exactly backwards. Reconfiguring a router and then
// pointing the device at the new SSID is the main reason anyone edits these credentials at all,
// and a router takes 1-3 minutes to come back — many times over any boot-window-sized deadline.
// The deadline expires while the AP is still booting, the OLD SSID (which no longer exists) is
// restored, and the CORRECT new credentials are gone. The rollback then strands the device on a
// network that is never coming back — a worse outcome than the one it was built to prevent, and
// one the user cannot even diagnose, because the device reports the old SSID as if nothing had
// happened.
//
// So the disconnect REASON, not the clock, is what separates evidence from mere silence:
//
//   * The AP answered and refused us (the auth/handshake class) — positive evidence about the
//     CREDENTIALS themselves. Nothing else in the sequence produces that evidence.
//   * The SSID was never on the air (NO_AP_FOUND) — no evidence about the credentials
//     whatsoever. An absent AP is precisely what a rebooting router looks like.
//   * Nothing conclusive — associated and waiting on DHCP, or no disconnect logged at all. Also
//     not evidence: an all-channel scan plus a WPA3-SAE handshake plus a first DHCP lease
//     legitimately approaches the 30 s window main.cpp's wifi_connect() already waits.
//
// The asymmetry follows from the COST, not from any belief about which case is likelier: a
// rollback DISCARDS the new credentials and cannot be undone, while waiting costs only time on a
// device that is already off the LAN. Being wrong in the patient direction delays a rollback;
// being wrong in the other direction destroys what the user just typed. So absence of evidence
// buys patience, and only the AP's own "no" is allowed to be fast.
//
// THE CALLER is main.cpp's wifi_connect(), and only on the boot that follows a /set_wifi change
// (`cfg_blob.wifi_rollback_active`): its WIFI_EVENT_STA_DISCONNECTED handler keeps the reason code,
// and the boot window feeds one sample per kWifiBootWindowS checkpoint into rollback_step() until
// it says RollBack — after which main.cpp restores the one-shot backup from the same atomic blob
// (logic/config_store.hpp) and reboots. A boot with NO credential change pending never enters that
// loop and keeps the long-standing single-window + setup-portal behaviour.
//
// The policy lives here rather than inside the event handler so it is asserted and host-tested
// instead of re-litigated under time pressure, exactly the way logic/connect_outcome.hpp pins the
// classification the BLE connect path applies. It is ported from a sibling ESP-IDF firmware where
// the blind-deadline version it replaces did destroy valid credentials in the field.

// What a WIFI_EVENT_STA_DISCONNECTED reason says about the CREDENTIALS — never about link
// quality, signal or timing. Deliberately four values and not a bool: "we have no idea" and "the
// AP told us no" must not collapse into one bucket, because they buy opposite amounts of
// patience below.
enum class DiscoClass : uint8_t {
    None,      // no disconnect seen yet — inconclusive (may be associated, waiting on DHCP)
    Auth,      // the AP was reached and refused/failed the credentials — they are implicated
    ApAbsent,  // the SSID was not on the air — says nothing at all about the credentials
    Other,     // beacon timeout, assoc fail, an ordinary deauth, ... — inconclusive
};

// Classify one reason code.
//
// The codes are IDF's `wifi_err_reason_t` (the low numbers are the 802.11 reason codes, the
// 200-range is Espressif's own), but they arrive here as a plain int so this header pulls in no
// IDF header and the host test can drive every branch directly. The numeric values are PINNED in
// the comments on purpose: an enum this file cannot see is an enum that could be renumbered
// without breaking a build, so a reader has to be able to check them against esp_wifi_types.h by
// eye. Verified against ESP-IDF 5.x.
//
// Reason 2 (AUTH_EXPIRE) is the ambiguous member of the auth class and worth naming as such: an
// AP also sends it on a routine idle deauth, so on its own it is not proof of a bad password.
// It is safe here only because of the SUSTAIN rule below — a single sample never spends the
// credentials, and a housekeeping deauth cannot keep being the story across two independent
// checkpoints a minute apart. Read the two together; either one alone is wrong.
//
// 210/211 sit in the auth class rather than the absent class although both mention NO_AP_FOUND:
// they mean the AP WAS heard and its security/authmode does not match what we are configured to
// speak. That is a statement about our own settings, which is what a credential change edits.
// 212 (heard, but under the RSSI threshold) is the opposite — a presence problem, not a
// settings one — so it classes as absent.
inline DiscoClass disco_class(int reason) {
    switch (reason) {
        case 0:    // no STA_DISCONNECTED observed this boot at all
            return DiscoClass::None;

        // ── The AP was reached and would not let us on ────────────────────────────
        case 2:    // WIFI_REASON_AUTH_EXPIRE (see the caveat above)
        case 15:   // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT — the PSK did not verify
        case 202:  // WIFI_REASON_AUTH_FAIL
        case 204:  // WIFI_REASON_HANDSHAKE_TIMEOUT
        case 210:  // WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY
        case 211:  // WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
            return DiscoClass::Auth;

        // ── The SSID simply was not there ─────────────────────────────────────────
        case 201:  // WIFI_REASON_NO_AP_FOUND — nothing answered for that SSID
        case 212:  // WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD — heard, but out of range
            return DiscoClass::ApAbsent;

        default:
            // Beacon timeout (200), assoc failures, an ordinary DEAUTH_LEAVING (3), anything
            // unrecognised. None of these is the AP judging our credentials, so none of them may
            // shorten the wait. Unknown lands here BY DESIGN: a reason code this firmware has
            // never seen must buy patience, not a destructive shortcut.
            return DiscoClass::Other;
    }
}

// The ordinary boot connect window — how long the STA is given to produce its first IP before
// anything is decided. Matches the 30 s wifi_connect() already waits on WIFI_CONNECTED_BIT, so
// wiring this up changes nothing for a boot with NO credential change pending: the setup portal
// is the fallback there, and a user is standing in front of the device.
inline constexpr int kWifiBootWindowS = 30;

// The grace window for a PENDING credential change when nothing implicates the credentials.
// Sized past the 1-3 minutes a router takes to reboot — the scenario in which a blind deadline
// destroys correct credentials. Bounded rather than endless: an SSID that never appears (a
// typo'd network name) must still fall back to the network we know works, or the backup is
// decoration.
inline constexpr int kWifiRollbackGraceS = 180;

// How many CONSECUTIVE checkpoints the AP must keep refusing us before the fast path spends the
// new credentials. Two, not one, and the reason is that the reason slot is a SAMPLE: one reading
// cannot tell "this password is wrong" from "a transient WPA3-SAE failure happened to be the
// last thing logged when we looked" — the same class of transient the retry logic already exists
// to ride out. Sustained refusal across two independent checkpoints can tell them apart. At the
// kWifiBootWindowS cadence that is ~60 s, still far inside the grace window, so the fast path
// stays meaningfully faster than the patient one.
inline constexpr int kWifiAuthToRollback = 2;

enum class RollbackAction : uint8_t {
    Wait,      // keep trying — nothing decided yet
    RollBack,  // restore the backup credentials and reboot
};

// Consecutive-observation counter. Reset by any checkpoint whose story is NOT "the AP refused
// us", so a single auth sample among absent/inconclusive ones can never accumulate its way to a
// rollback across an unrelated hour.
struct RollbackWatch {
    int auth{0};
};

// Decide, at one boot-window checkpoint, whether to restore the backup credentials. Called only
// while a credential change is pending AND the STA still has no IP.
//
//   st        — the streak carried across checkpoints of this boot
//   last      — class of the CURRENT story: the most recent disconnect reason, or None once the
//               STA has re-associated. The caller must CLEAR its stored reason on
//               WIFI_EVENT_STA_CONNECTED: an earlier refusal must not outlive the association
//               that disproved it, or a device that got on the network on attempt three still
//               rolls back on the evidence of attempt one.
//   elapsed_s — seconds since the STA was started this boot
inline RollbackAction rollback_step(RollbackWatch& st, DiscoClass last, int elapsed_s) {
    // Before the ordinary window is even up, nothing is decided — not even for the auth class.
    // Those codes (2/15/202/204) double as the transient handshake failures a normal connect
    // recovers from on its own, so no early one may spend the new credentials.
    if (elapsed_s < kWifiBootWindowS) return RollbackAction::Wait;

    if (last == DiscoClass::Auth) {
        if (++st.auth >= kWifiAuthToRollback) return RollbackAction::RollBack;  // sustained "no"
    } else {
        st.auth = 0;   // anything else is not the AP refusing us — the streak is broken
    }

    // Nothing implicates the credentials (or not yet often enough): keep looking until the grace
    // is spent. This is the ONLY path an absent SSID can ever take to a rollback — which is the
    // point, since an absent SSID is what a rebooting router looks like.
    return elapsed_s >= kWifiRollbackGraceS ? RollbackAction::RollBack : RollbackAction::Wait;
}

}  // namespace tk
