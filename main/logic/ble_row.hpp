#pragma once
// What the web UI's Bluetooth connection row shows: which of its five states it is in, and
// which countdown (if any) belongs beside that state. Pure, IDF-free, host-tested
// (test/test_logic.cpp), and parity-checked against the JavaScript that actually runs in the
// browser (scripts/check-ble-row-parity.sh) — the same arrangement `display_model.hpp` has
// with `tools/display_sim.py`.
//
// NOTE this header is a SPEC, not runtime code: no firmware translation unit includes it. Its
// only consumers are test/test_logic.cpp and test/ble_row_golden_dump.cpp — the browser is what
// actually runs these rules, and the parity harness is what holds it to them. So it costs no
// flash, and "wiring it up" somewhere in main/ would be a misunderstanding, not a fix.
//
// WHY THIS EXISTS. The row is the one UI surface whose "what to show" decision used to live
// inline in app.js, where nothing could test it. The display and the status LED both got a
// presenter (`display_model.hpp`, `led_status.hpp`); this row did not, and it is exactly where
// three rounds of user-visible bugs landed — a label naming one phase while the number beside
// it counted down another, and labels left with no countdown at all.
//
// NOTHING SITS BETWEEN /status AND THE DECISION. decide() takes the raw fields as they arrive
// (`RowStatus`) and does its own deriving, so the JS mirror is one function from JSON to verdict
// and the parity harness covers all of it. An earlier cut handed it a pre-computed `link_known`
// and `has_vin`, which left that mapping as the one step no gate could see — and it is where the
// `paired &&` factoring lives. Dropping `paired` there renders an unpaired cold-start board's row
// amber instead of green; with the derivation outside the fence, every golden vector still passed.
//
// THE STRUCTURAL GUARANTEE. `ble.scanning` is deliberately NOT an input. The radio scans for
// reasons that have no deadline and no schedule — `loop_task`'s warm-up connect toggles that
// flag on and off straight through the idle wait (observed on hardware: 171 of 573 sampled
// frames had `scanning=true` while the armed phase was `waiting`). Driving the label off that
// flag while the countdown came from the phase is what made the two disagree. Leaving the flag
// out of this struct makes that class of bug *unrepresentable* rather than merely tested-for.
//
// THE INVARIANTS the CHECKs pin, each one a bug that shipped:
//   1. `Scanning` ⟺ an attempt is actually running (`Phase::Connecting`). The label follows
//      the phase, never the radio's scanning flag.
//   2. A countdown belongs to exactly one state: `GivesUp` only on `Scanning`, `Retries` only
//      on `Idle`.
//   3. While a phase is armed, a state that can show a countdown always does — a phase-less
//      moment may leave the row bare, an armed one never may.
//   4. `Discovering` (the no-VIN listing scan) is the ONE state that says "Searching…" with no
//      countdown, because with no VIN there is no pairing schedule to count down at all.
#include <cstdint>

#include "ble_phase.hpp"
#include "link_state.hpp"

namespace tk {
namespace ble {

// Consecutive failed connects to the target car before the row switches from "still looking"
// to naming the failure. Shared with the UI so both sides agree on when that flip happens.
inline constexpr uint32_t kConnectFailWarn = 2;

enum class Row : uint8_t {
    Discovering,  // no VIN yet, nothing seen — listing-only scan, no schedule ("Searching…")
    Listing,      // no VIN yet, nearby Teslas found — show the list
    Linked,       // GATT link up — signal, dBm, MAC
    Failed,       // car heard but the link keeps failing ("Connection failed" / at its limit)
    Scanning,     // a bounded connect attempt is running ("Searching…")
    Idle,         // link down, next attempt scheduled ("Disconnected")
};

enum class Countdown : uint8_t {
    None,
    GivesUp,   // this attempt gives up in phase_s
    Retries,   // the next attempt starts in phase_s
};

// The /status fields the row reads, in the shape they arrive in — NOT pre-derived. Every
// derivation the row depends on (is there a VIN, is the link "known", how many nearby) happens
// INSIDE decide(), so it is covered by the CHECKs and by the parity harness. An earlier cut took
// a pre-computed `link_known` and `has_vin`, which left the mapping from /status to those two
// booleans as the one step no gate could see — and that mapping is exactly where the `paired &&`
// factoring lives. Dropping `paired` there would have rendered an unpaired cold-start board's row
// amber instead of green with all 192 golden vectors still green.
struct RowStatus {
    const char* vin{""};              // "" or "UNKNOWN" ⇒ no VIN configured
    bool        paired{false};
    LinkState   link{LinkState::Unknown};
    bool        ble_connected{false}; // GATT link up right now
    int         ble_devices{0};       // devices[] length (only meaningful without a VIN)
    uint32_t    ble_connect_fail{0};  // consecutive failed connects to the target
    Phase       phase{Phase::None};
    // NOTE: no `scanning` field, by design — see the header comment.
};

struct RowView {
    Row       row{Row::Idle};
    Countdown cd{Countdown::None};
    bool      stateless{false};   // linked but no signed round-trip yet ⇒ amber, not green
};

// A VIN gates pairing entirely; the firmware reports "UNKNOWN" when none is stored.
inline bool has_vin(const char* vin) {
    if (!vin || !*vin) return false;
    const char* u = "UNKNOWN";
    for (int i = 0; i < 8; i++) if (vin[i] != u[i]) return true;   // includes the NUL
    return false;
}

// "Known" = we have a current picture of the car. Only meaningful once paired: before that,
// link is unknown for the mundane reason that we have never talked to the car.
inline bool link_known(bool paired, LinkState link) {
    return !(paired && (link == LinkState::Unknown || link == LinkState::Unreachable));
}

inline RowView decide(const RowStatus& s) {
    if (!has_vin(s.vin)) return { s.ble_devices > 0 ? Row::Listing : Row::Discovering, Countdown::None, false };
    if (s.ble_connected) return { Row::Linked, Countdown::None, !link_known(s.paired, s.link) };
    // Found the car but the link won't come up: name the failure rather than keep saying
    // "Searching…", which would imply we can't find it. Outranks the phase — an attempt may
    // well be running, but "it keeps failing" is the more useful thing to say about it.
    if (s.ble_connect_fail >= kConnectFailWarn) return { Row::Failed, Countdown::None, false };
    if (s.phase == Phase::Connecting) return { Row::Scanning, Countdown::GivesUp, false };
    return { Row::Idle, s.phase == Phase::Waiting ? Countdown::Retries : Countdown::None, false };
}

// Stable names for the golden dump / parity harness (and for reading a test failure).
inline constexpr const char* row_name(Row r) {
    switch (r) {
        case Row::Discovering: return "discovering";
        case Row::Listing:     return "listing";
        case Row::Linked:      return "linked";
        case Row::Failed:      return "failed";
        case Row::Scanning:    return "scanning";
        default:               return "idle";
    }
}
inline constexpr const char* cd_name(Countdown c) {
    switch (c) {
        case Countdown::GivesUp: return "givesup";
        case Countdown::Retries: return "retries";
        default:                 return "none";
    }
}

} // namespace ble
} // namespace tk
