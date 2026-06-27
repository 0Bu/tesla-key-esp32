// Host-side mock build — exercises the pure, hardware-free logic in main/logic/
// without ESP-IDF. Dependency-free (no gtest): a tiny CHECK macro tallies failures
// and the process exit code is the verdict, so it runs anywhere in seconds. See
// test/README.md.
//
// Scope: the IDF-free decision/conversion cores the firmware delegates to —
//   * VIN plausibility               (logic/vin.hpp        <- VehicleController::vin_is_plausible)
//   * imperial -> metric conversion  (logic/units.hpp      <- MQTT bridge / telemetry)
//   * link_state() four-state machine + its web/MQTT string mappings
//                                    (logic/link_state.hpp <- /status "link", MQTT sleep_status)
//   * per-target platform / OTA-suffix mapping
//                                    (logic/target.hpp     <- platform.hpp, ota_update.cpp)
// The cJSON envelope builders themselves stay IDF/cJSON-coupled; their pure inputs
// (the conversions and link strings that feed /status and the MQTT payloads) are
// what regress silently, and those are covered here.

#include "logic/vin.hpp"
#include "logic/units.hpp"
#include "logic/link_state.hpp"
#include "logic/target.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("  FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

// Compare two C strings allowing nullptr on either side.
static bool streq(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) return a == b;
    return std::strcmp(a, b) == 0;
}
#define CHECK_STR(got, want) CHECK(streq((got), (want)))

// Floating compare with a small tolerance (conversions are inexact by nature).
static bool approx(double got, double want) { return std::fabs(got - want) < 1e-6; }
#define CHECK_NEAR(got, want) CHECK(approx((got), (want)))

// ─── VIN plausibility ──────────────────────────────────────────────────────────
static void test_vin() {
    // Canonical 17-char Tesla VIN.
    CHECK(tk::vin_is_plausible("5YJ3E1EA7KF000316"));
    CHECK(tk::vin_is_plausible("LRWYGCEK9PC000001"));
    CHECK(tk::vin_is_plausible("00000000000000000"));  // all digits, 17 chars

    // Wrong length.
    CHECK(!tk::vin_is_plausible(""));
    CHECK(!tk::vin_is_plausible("UNKNOWN"));                 // 7-char boot placeholder
    CHECK(!tk::vin_is_plausible("5YJ3E1EA7KF00031"));        // 16
    CHECK(!tk::vin_is_plausible("5YJ3E1EA7KF0003166"));      // 18

    // Lowercase is not accepted (the standard / our /set_vin are uppercase-only).
    CHECK(!tk::vin_is_plausible("5yj3e1ea7kf000316"));

    // I, O, Q are reserved by the VIN standard and must be rejected.
    CHECK(!tk::vin_is_plausible("5YJ3E1EA7KF0003I6"));
    CHECK(!tk::vin_is_plausible("5YJ3E1EA7KF0003O6"));
    CHECK(!tk::vin_is_plausible("5YJ3E1EA7KF0003Q6"));

    // Stray punctuation / spaces.
    CHECK(!tk::vin_is_plausible("5YJ3E1EA7KF00031-"));
    CHECK(!tk::vin_is_plausible("5YJ3E1EA7KF 00316"));
}

// ─── Unit conversion ────────────────────────────────────────────────────────────
static void test_units() {
    CHECK_NEAR(tk::kMilesToKm, 1.609344);

    CHECK_NEAR(tk::mi_to_km(0.0),   0.0);
    CHECK_NEAR(tk::mi_to_km(1.0),   1.609344);
    CHECK_NEAR(tk::mi_to_km(100.0), 160.9344);    // ~100 mi range
    CHECK_NEAR(tk::mph_to_kmh(60.0), 96.56064);   // 60 mph charge rate

    // Odometer arrives in hundredths of a mile: 1,234,567 -> 12,345.67 mi -> km.
    CHECK_NEAR(tk::odo_hundredths_mi_to_km(1234567.0), 12345.67 * 1.609344);
    CHECK_NEAR(tk::odo_hundredths_mi_to_km(0.0), 0.0);
}

// Build a LinkInputs snapshot tersely.
static tk::LinkInputs mk(bool hc, uint32_t ca, bool hr, uint32_t ra, bool asleep) {
    tk::LinkInputs in;
    in.have_contact        = hc;
    in.contact_age_s       = ca;
    in.have_reachable      = hr;
    in.reachable_age_s     = ra;
    in.vcsec_stably_asleep = asleep;
    return in;
}

// ─── link_state() four-state machine ─────────────────────────────────────────────
static void test_link_state() {
    using LS = tk::LinkState;

    // Never heard anything ⇒ Unknown.
    CHECK(tk::compute_link_state(mk(false, 0, false, 0, false)) == LS::Unknown);

    // Fresh infotainment data ⇒ Awake (boundary: < kAwakeMaxAgeS).
    CHECK(tk::compute_link_state(mk(true, 0,  true, 0, false)) == LS::Awake);
    CHECK(tk::compute_link_state(mk(true, tk::kAwakeMaxAgeS - 1, true, 0, false)) == LS::Awake);
    // At exactly the boundary it is no longer Awake (>= is stale).
    CHECK(tk::compute_link_state(mk(true, tk::kAwakeMaxAgeS, true, 10, false)) != LS::Awake);

    // Reachable, no fresh data, NOT provably asleep ⇒ Idle (never a sleep claim).
    CHECK(tk::compute_link_state(mk(false, 0, true, 0, false)) == LS::Idle);
    CHECK(tk::compute_link_state(mk(true, tk::kAwakeMaxAgeS, true, 30, false)) == LS::Idle);

    // Reachable + debounced VCSEC ASLEEP ⇒ Asleep.
    CHECK(tk::compute_link_state(mk(false, 0, true, 30, true)) == LS::Asleep);

    // Reachable boundary: at/over kReachableMaxAgeS it stops being reachable.
    CHECK(tk::compute_link_state(mk(false, 0, true, tk::kReachableMaxAgeS - 1, false)) == LS::Idle);
    CHECK(tk::compute_link_state(mk(false, 0, true, tk::kReachableMaxAgeS, false)) == LS::Unreachable);

    // ASYMMETRY 1: fresh contact wins over a VCSEC ASLEEP flag — Awake, never Asleep.
    CHECK(tk::compute_link_state(mk(true, 5, true, 5, true)) == LS::Awake);

    // ASYMMETRY 2: VCSEC AWAKE (asleep=false) can never assert Awake without fresh
    // telemetry — a reachable car with stale data stays Idle, not Awake.
    CHECK(tk::compute_link_state(mk(false, 0, true, 10, false)) == LS::Idle);

    // Heard once but everything stale now ⇒ Unreachable (contact-only and reachable-only).
    CHECK(tk::compute_link_state(mk(true, 999, false, 0, false)) == LS::Unreachable);
    CHECK(tk::compute_link_state(mk(false, 0, true, 999, false)) == LS::Unreachable);

    // A stuck "stably asleep" flag must NOT resurrect Asleep once unreachable.
    CHECK(tk::compute_link_state(mk(false, 0, true, 999, true)) == LS::Unreachable);
}

// ─── link_state -> string mappings (web hero + MQTT sleep_status) ─────────────────
static void test_link_state_strings() {
    using LS = tk::LinkState;

    CHECK_STR(tk::link_state_web_str(LS::Awake),       "awake");
    CHECK_STR(tk::link_state_web_str(LS::Asleep),      "asleep");
    CHECK_STR(tk::link_state_web_str(LS::Idle),        "idle");
    CHECK_STR(tk::link_state_web_str(LS::Unreachable), "unreachable");
    CHECK_STR(tk::link_state_web_str(LS::Unknown),     "unknown");

    CHECK_STR(tk::link_state_mqtt_str(LS::Awake),       "AWAKE");
    CHECK_STR(tk::link_state_mqtt_str(LS::Asleep),      "ASLEEP");
    CHECK_STR(tk::link_state_mqtt_str(LS::Idle),        "IDLE");
    CHECK_STR(tk::link_state_mqtt_str(LS::Unreachable), "UNREACHABLE");
    // Unknown is omitted on the MQTT side (HA shows "unknown").
    CHECK(tk::link_state_mqtt_str(LS::Unknown) == nullptr);
}

// ─── per-target platform / OTA-suffix mapping ─────────────────────────────────────
static void test_target() {
    CHECK_STR(tk::platform_name(tk::Target::Esp32),   "ESP32");
    CHECK_STR(tk::platform_name(tk::Target::Esp32S3), "ESP32-S3");
    CHECK_STR(tk::platform_name(tk::Target::Esp32C3), "ESP32-C3");
    CHECK_STR(tk::platform_name(tk::Target::Esp32C6), "ESP32-C6");

    // esp32 has no suffix (tesla-key-esp32.bin); the rest get -s3/-c3/-c6.
    CHECK_STR(tk::image_suffix(tk::Target::Esp32),   "");
    CHECK_STR(tk::image_suffix(tk::Target::Esp32S3), "-s3");
    CHECK_STR(tk::image_suffix(tk::Target::Esp32C3), "-c3");
    CHECK_STR(tk::image_suffix(tk::Target::Esp32C6), "-c6");

    // The full OTA filename the device pulls, assembled the same way ota_update.cpp does.
    const std::string base = "tesla-key-esp32";
    CHECK(base + tk::image_suffix(tk::Target::Esp32)   + ".bin" == "tesla-key-esp32.bin");
    CHECK(base + tk::image_suffix(tk::Target::Esp32S3) + ".bin" == "tesla-key-esp32-s3.bin");
}

int main() {
    test_vin();
    test_units();
    test_link_state();
    test_link_state_strings();
    test_target();

    if (g_failures == 0) {
        std::printf("OK  %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAILED  %d/%d checks failed\n", g_failures, g_checks);
    return 1;
}
