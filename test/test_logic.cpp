// Host-side mock build — exercises the pure, hardware-free logic in main/logic/
// without ESP-IDF. Dependency-free (no gtest): a tiny CHECK macro tallies failures
// and the process exit code is the verdict, so it runs anywhere in seconds. See
// test/README.md.
//
// Scope: the IDF-free decision/conversion cores the firmware delegates to —
//   * VIN plausibility               (logic/vin.hpp        <- VehicleController::vin_is_plausible)
//   * vehicle-stable HA identity     (logic/ha_identity.hpp <- mqtt_ha.cpp)
//   * imperial -> metric conversion  (logic/units.hpp      <- MQTT bridge / telemetry)
//   * link_state() four-state machine + its web/MQTT string mappings
//                                    (logic/link_state.hpp <- /status "link", MQTT sleep_status)
//   * per-target platform / OTA-suffix mapping
//                                    (logic/target.hpp     <- platform.hpp, ota_update.cpp)
//   * the shared command registry — names/kinds/arg bounds + REST body compatibility
//                                    (logic/command_registry.hpp <- http_api.cpp + mcp_server.cpp)
//   * the /status field contract, pinned by golden emissions
//                                    (logic/status_model.hpp <- http_status.cpp)
//   * Syslog target parsing, send-failure classification + the RFC 5424 PRI derived from
//     each line's esp_log level
//                                    (logic/syslog_policy.hpp <- syslog.cpp, /set_syslog)
//   * BLE connect-failure cause + its log level / rate limit
//                                    (logic/connect_outcome.hpp <- ensure_connected_)
//   * browser same-origin mutation gate + negotiated ATT write payload
//                                    (logic/http_origin.hpp <- http_server.cpp,
//                                     logic/ble_chunk.hpp <- ble_client.cpp)
// The cJSON serializers stay IDF/cJSON-coupled, but they are one-to-one visitors now:
// every which-field/when/what-value decision they render is host-tested here.

#include "logic/vin.hpp"
#include "logic/clock_policy.hpp"
#include "logic/effect_reply.hpp"
#include "logic/config_commit.hpp"
#include "logic/config_repair.hpp"
#include "logic/ha_identity.hpp"
#include "logic/units.hpp"
#include "logic/uptime.hpp"
#include "logic/iteration_fault.hpp"
#include "logic/link_state.hpp"
#include "logic/target.hpp"
#include "logic/mcp.hpp"
#include "logic/command_registry.hpp"
#include "logic/auto_enrollment.hpp"
#include "logic/vcsec_response.hpp"
#include "logic/status_model.hpp"
#include "logic/command_result.hpp"
#include "logic/ui_state.hpp"
#include "logic/display_model.hpp"
#include "logic/eth_board.hpp"
#include "logic/soc_gradient.hpp"
#include "logic/led_status.hpp"
#include "logic/led_driver.hpp"
#include "logic/syslog_policy.hpp"
#include "logic/syslog_start_gate.hpp"
#include "logic/connect_outcome.hpp"
#include "logic/active_window.hpp"
#include "logic/wake_poll.hpp"
#include "logic/telemetry_poll.hpp"
#include "logic/ble_readiness.hpp"
#include "logic/ble_phase.hpp"
#include "logic/ble_row.hpp"
#include "logic/ha_templates.hpp"
#include "logic/mqtt_discovery_registry.hpp"
#include "logic/http_body.hpp"
#include "logic/http_route.hpp"
#include "logic/json_syntax.hpp"
#include "logic/config_request.hpp"
#include "logic/heap_json_stream.hpp"
#include "logic/heap_watchdog.hpp"
#include "logic/ota_lifetime.hpp"
#include "logic/heap_supervision.hpp"
#include "logic/ota_contract.hpp"
#include "logic/tls_memory.hpp"
#include "logic/ota_recovery_memory.hpp"
#include "logic/ota_changelog_range.hpp"
#include "logic/task_start_gate.hpp"
#include "logic/nimble_start_gate.hpp"
#include "logic/ping_probe.hpp"
#include "logic/runtime_admission.hpp"
#include "logic/identity_recovery.hpp"
#include "logic/charge_control.hpp"
#include "logic/reset_reason.hpp"
#include "logic/crashinfo.hpp"
#include "logic/coredump_image.hpp"
#include "coredump_fixture.hpp"
#include "logic/bootlog.hpp"
#include "logic/boot_guard.hpp"
#include "logic/heap_history.hpp"
#include "logic/health_gate.hpp"
#include "logic/mqtt_state_lifecycle.hpp"
#include "logic/mqtt_uri.hpp"
#include "logic/http_origin.hpp"
#include "logic/ble_chunk.hpp"
#include "logic/ble_deferred_event.hpp"
#include "logic/ble_deferred_queue.hpp"
#include "logic/wifi_rollback.hpp"
#include "logic/redact.hpp"
#include "logic/captive.hpp"
#include "logic/setup_portal.hpp"
#include "logic/config_store.hpp"
#include "logic/service_target_limits.hpp"
#include "logic/wifi_credentials.hpp"
#include "logic/key_rotation.hpp"
#include "logic/nvs_string_load.hpp"
#include "logic/vin_transition.hpp"
#include "logic/rx_framing.hpp"
#include "logic/ble_dispatcher.hpp"
#include "logic/session_state.hpp"
#include "logic/command_runner.hpp"
#include "logic/command_fault.hpp"
#include "logic/command_deadline.hpp"
#include "logic/session_availability.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

static void test_wifi_credentials() {
    using E = tk::WifiCredentialError;
    CHECK(tk::wifi_credentials_error("open", "") == E::None);
    CHECK(tk::wifi_credentials_error("secure", "12345678") == E::None);
    CHECK(tk::wifi_credentials_error("secure", std::string(63, 'x')) == E::None);
    CHECK(tk::wifi_credentials_error("raw", std::string(64, 'a')) == E::None);
    CHECK(tk::wifi_credentials_error("", "12345678") == E::SsidLength);
    CHECK(tk::wifi_credentials_error(std::string(33, 's'), "12345678") == E::SsidLength);
    CHECK(tk::wifi_credentials_error("short", "1234567") == E::PasswordLength);
    CHECK(tk::wifi_credentials_error("raw", std::string(64, 'z')) == E::RawPskNotHex);
    std::string nul_ssid("a\0b", 3);
    CHECK(tk::wifi_credentials_error(nul_ssid, "12345678") == E::EmbeddedNul);
}

static void test_clock_policy() {
    const int64_t ceiling = tk::clock_ceiling_seconds("Oct 20 2026");
    CHECK(ceiling == 2082758400LL); // 2036-01-01 UTC; calendar oracle independent of implementation
    CHECK(tk::clock_jan1_seconds(1970) == 0);
    CHECK(tk::clock_jan1_seconds(2023) == 1672531200LL);
    CHECK(tk::clock_ceiling_seconds("") == 1988150400LL);
    CHECK(tk::clock_ceiling_seconds("invalid abcd") == 1988150400LL);
    CHECK(tk::clock_ceiling_seconds("Jan 01 2022") == 1988150400LL);
    CHECK(tk::clock_ceiling_seconds("Jan 01 2027") == 2114380800LL);
    CHECK(tk::clock_should_restore(0));
    CHECK(tk::clock_should_restore(1699999999));
    CHECK(!tk::clock_should_restore(1700000000));
    CHECK(!tk::clock_should_restore(1900000000)); // preserve a plausible warm clock over old cache

    int64_t seconds = 42;
    CHECK(tk::clock_parse_checkpoint("1700000000", ceiling, seconds) && seconds == 1700000000);
    CHECK(tk::clock_parse_checkpoint("2082758399", ceiling, seconds) && seconds == ceiling - 1);
    for (const char* invalid : {"", "0", "-1", "+1800000000", " 1800000000", "1800000000 ",
                               "01800000000", "1800000000junk", "1.8e9", "1699999999",
                               "2082758400", "18446744073709551615", "18446744073709551616",
                               "9223372036854775808"}) {
        seconds = 42;
        CHECK(!tk::clock_parse_checkpoint(invalid, ceiling, seconds));
        CHECK(seconds == 42);
    }
    CHECK(tk::clock_parse_date("1600000001", ceiling, seconds) && seconds == 1600000001);
    CHECK(!tk::clock_parse_date("1600000000", ceiling, seconds));
    CHECK(tk::clock_parse_date("1699999999", ceiling, seconds)); // historical floor stays distinct
    CHECK(!tk::clock_parse_date("2082758400", ceiling, seconds));
    CHECK(!tk::clock_parse_date("1800000000?", ceiling, seconds));

    tk::ClockSample sample{42, 17};
    CHECK(tk::browser_clock_sample(1700000000000.0, ceiling, sample));
    CHECK(sample.seconds == 1700000000 && sample.microseconds == 0);
    CHECK(tk::browser_clock_sample(1800000000123.75, ceiling, sample));
    CHECK(sample.seconds == 1800000000 && sample.microseconds == 123000);
    CHECK(tk::browser_clock_sample(2082758399999.0, ceiling, sample));
    CHECK(sample.seconds == ceiling - 1 && sample.microseconds == 999000);
    for (double invalid : {-1.0, 0.0, 1699999999999.0, 2082758400000.0,
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        sample = {42, 17};
        CHECK(!tk::browser_clock_sample(invalid, ceiling, sample));
        CHECK(sample.seconds == 42 && sample.microseconds == 17);
    }
    CHECK(!tk::browser_clock_sample(1800000000000.0, INT64_MAX, sample));
    CHECK(tk::clock_parse_query_ms("1800000000123", ceiling, sample));
    CHECK(sample.seconds == 1800000000 && sample.microseconds == 123000);
    for (const char* invalid : {"", "0", "01800000000123", "+1800000000123", "1.8e12",
                               "1800000000123.0", "1800000000123x", "2082758400000",
                               "1699999999999", "18446744073709551616"})
        CHECK(!tk::clock_parse_query_ms(invalid, ceiling, sample));

    CHECK(!tk::clock_sample_valid({1800000000, -1}, ceiling));
    CHECK(!tk::clock_sample_valid({1800000000, 1000000}, ceiling));
    CHECK(tk::clock_advance_sample({1800000000, 999999}, 1, ceiling, sample));
    CHECK(sample.seconds == 1800000001 && sample.microseconds == 0);
    CHECK(tk::clock_advance_sample({1800000000, 123456}, 2000789, ceiling, sample));
    CHECK(sample.seconds == 1800000002 && sample.microseconds == 124245);
    CHECK(!tk::clock_advance_sample({ceiling-1, 999999}, 1, ceiling, sample));
    CHECK(!tk::clock_advance_sample({ceiling-1, 0}, 1000000, ceiling, sample));
    CHECK(!tk::clock_advance_sample({1800000000, 0}, -1, ceiling, sample));
    CHECK(!tk::clock_advance_sample({1800000000, 0}, INT64_MAX, ceiling, sample));
    // Cross-check unit carry at every millisecond boundary rather than only a model happy path.
    for (int32_t ms = 0; ms < 1000; ++ms) {
        CHECK(tk::clock_advance_sample({1800000000, ms * 1000}, 1000001 - ms * 1000,
                                       ceiling, sample));
        CHECK(sample.seconds == 1800000001 && sample.microseconds == 1);
    }

    using P = tk::ClockCheckpointPolicy;
    for (uint64_t start : {uint64_t(0), uint64_t(1000), UINT64_MAX - 1000}) {
        P policy;
        CHECK(policy.due(start));
        policy.note_attempt(start);
        policy.note_result(false, start);
        CHECK(!policy.due(start + P::kAttemptIntervalUs - 1));
        CHECK(policy.due(start + P::kAttemptIntervalUs));
        const uint64_t stored_at = start + P::kAttemptIntervalUs;
        policy.note_attempt(stored_at);
        policy.note_result(true, stored_at);
        CHECK(!policy.due(stored_at + P::kSuccessIntervalUs - 1));
        CHECK(policy.due(stored_at + P::kSuccessIntervalUs));
        const auto next = stored_at + P::kSuccessIntervalUs;
        policy.note_attempt(next);
        policy.note_result(false, next);
        CHECK(policy.last_success_us == stored_at);
        CHECK(!policy.due(next + P::kAttemptIntervalUs - 1));
        CHECK(policy.due(next + P::kAttemptIntervalUs));
    }
}

static void test_vin_transition() {
    using R = tk::VinTransitionRecovery;
    using A = tk::VinTransitionApply;
    using J = tk::VinTransitionJournalAction;
    // The marker is a versioned, length-delimited record, not `vin|key`: a legacy VIN may contain
    // the old delimiter or be empty/invalid, and POST /set_vin is precisely its migration path.
    const std::string legacy_vin = "legacy|VIN with spaces|?";
    const std::string legacy_encoded =
        tk::make_vin_transition_marker(legacy_vin, "AA:BB:CC:DD");
    tk::VinTransitionMarker marker;
    CHECK(tk::parse_vin_transition_marker(legacy_encoded, marker));
    CHECK(marker.previous_vin == legacy_vin);
    CHECK(marker.previous_key_id == "AA:BB:CC:DD");
    const std::string keyless_encoded = tk::make_vin_transition_marker("", "");
    CHECK(tk::parse_vin_transition_marker(keyless_encoded, marker));
    CHECK(marker.previous_vin.empty());
    CHECK(marker.previous_key_id.empty());

    // This format is not released, so accepting the ambiguous prototype buys no compatibility and
    // would preserve its delimiter bug. Unknown versions, length overflow/mismatch, truncation,
    // trailing bytes and malformed key ids all leave the prior output untouched.
    marker.previous_vin = "untouched";
    marker.previous_key_id = "AA:BB:CC:DD";
    for (const std::string& malformed : {
             std::string{},
             std::string{"5YJ3E1EA7KF000316|AA:BB:CC:DD"},
             std::string{"VT2:0:0:"},
             std::string{"VT1::0:"},
             std::string{"VT1:999999999999999999999999999999:0:"},
             std::string{"VT1:5:0:abc"},
             std::string{"VT1:0:11:AA:BB:CC:G1"},
             std::string{"VT1:0:0:trailing"}}) {
        CHECK(!tk::parse_vin_transition_marker(malformed, marker));
        CHECK(marker.previous_vin == "untouched");
        CHECK(marker.previous_key_id == "AA:BB:CC:DD");
    }

    const std::string encoded =
        tk::make_vin_transition_marker("5YJ3E1EA7KF000316", "AA:BB:CC:DD");
    CHECK(tk::parse_vin_transition_marker(encoded, marker));
    CHECK(marker.previous_vin == "5YJ3E1EA7KF000316");
    CHECK(marker.previous_key_id == "AA:BB:CC:DD");

    // CommitUnknown/CleanupPending keep this exact POST-staged marker armed for boot recovery.
    for (const auto& pair : {
             std::pair{tk::KeyRotationResult::CommitUnknown, A::RecoverAmbiguousIdentity},
             std::pair{tk::KeyRotationResult::CleanupPending, A::RecoverCommittedIdentity}}) {
        tk::VinTransitionMarker staged;
        CHECK(tk::parse_vin_transition_marker(legacy_encoded, staged));
        CHECK(staged.previous_vin == legacy_vin);
        CHECK(tk::decide_vin_transition_apply(true, true, pair.first) == pair.second);
    }

    // A present-but-malformed cross-namespace journal still proves that identity mutation may
    // have started. It must remain armed and halt boot; deleting it would discard the only
    // recovery authority for a staged ConfigBlob/new VIN paired with the previous key.
    CHECK(tk::decide_vin_transition_journal(tk::NvsStringLoadState::Missing, false) ==
          J::Continue);
    CHECK(tk::decide_vin_transition_journal(tk::NvsStringLoadState::Error, false) ==
          J::Halt);
    CHECK(tk::decide_vin_transition_journal(tk::NvsStringLoadState::Present, false) ==
          J::Halt);
    CHECK(tk::decide_vin_transition_journal(tk::NvsStringLoadState::Present, true) ==
          J::Recover);

    // Armed-journal fingerprint evidence is valid only when the private-key blob probe agrees
    // with what Vehicle loaded. In particular, an empty previous id is a legitimate first-VIN
    // transaction, but Present+empty current fingerprint is ambiguous and must halt rather than
    // rolling the VIN back and erasing the journal.
    CHECK(!tk::vin_transition_key_evidence_verified("", "", false, false));
    CHECK(!tk::vin_transition_key_evidence_verified("", "", false, true));
    CHECK(tk::vin_transition_key_evidence_verified("", "", true, false));
    CHECK(!tk::vin_transition_key_evidence_verified("", "", true, true));
    CHECK(!tk::vin_transition_key_evidence_verified("", "NEW", true, false));
    CHECK(tk::vin_transition_key_evidence_verified("", "NEW", true, true));
    CHECK(!tk::vin_transition_key_evidence_verified("OLD", "", true, false));
    CHECK(!tk::vin_transition_key_evidence_verified("OLD", "", true, true));
    CHECK(tk::vin_transition_key_evidence_verified("OLD", "OLD", true, true));
    CHECK(tk::vin_transition_key_evidence_verified("OLD", "NEW", true, true));

    // Power loss after journal only: both durable identities are still old.
    CHECK(tk::decide_vin_transition_recovery(marker, "5YJ3E1EA7KF000316", "AA:BB:CC:DD") ==
          R::ClearMarker);
    // Power loss after ConfigBlob commit but before/after a failed key write.
    CHECK(tk::decide_vin_transition_recovery(marker, "LRWYGCEK9PC000001", "AA:BB:CC:DD") ==
          R::RollBackPreviousVin);
    // Power loss after the key commit: finish the new identity rather than reverting its VIN.
    CHECK(tk::decide_vin_transition_recovery(marker, "LRWYGCEK9PC000001", "11:22:33:44") ==
          R::CompleteNewIdentity);
    // A changed key beside the previous VIN is not a completed transaction. It contradicts the
    // journaled write order (ConfigBlob commits before key mutation), so deleting the marker would
    // bless a mismatched durable identity. This includes verified-Missing/rolled-back blobs.
    CHECK(tk::decide_vin_transition_recovery(marker, "5YJ3E1EA7KF000316", "11:22:33:44") ==
          R::HaltInconsistent);

    // The request-local rotation result, captured while command_mutex_ is still held, is the
    // only authority for the HTTP transaction. A fingerprint sampled after unlocking could
    // belong to an auto-rekey and must never reclassify this request.
    CHECK(tk::decide_vin_transition_apply(false, false,
                                          tk::KeyRotationResult::NotCommitted) ==
          A::IdentityUnverified);
    CHECK(tk::decide_vin_transition_apply(true, false,
                                          tk::KeyRotationResult::NotCommitted) ==
          A::StageFailed);
    CHECK(tk::decide_vin_transition_apply(true, true,
                                          tk::KeyRotationResult::NotCommitted) ==
          A::RollBackPreviousIdentity);
    CHECK(tk::decide_vin_transition_apply(true, true,
                                          tk::KeyRotationResult::CleanupPending) ==
          A::RecoverCommittedIdentity);
    CHECK(tk::decide_vin_transition_apply(true, true,
                                          tk::KeyRotationResult::CommitUnknown) ==
          A::RecoverAmbiguousIdentity);
    CHECK(tk::decide_vin_transition_apply(true, true,
                                          tk::KeyRotationResult::Complete) ==
          A::Complete);
    // A previous key rotation whose runtime identity or peer cleanup is still pending must
    // block before /set_vin writes vin_txn or ConfigBlob. The prior CleanupPending result does
    // not belong to this request and cannot be reclassified as its committed key mutation.
    CHECK(!tk::vin_transition_recovery_blocks_staging(false, false));
    CHECK(tk::vin_transition_recovery_blocks_staging(true, false));
    CHECK(tk::vin_transition_recovery_blocks_staging(false, true));
    CHECK(tk::vin_transition_recovery_blocks_staging(true, true));

    // Transition journal removal is permitted only after all session, pairing, and MAC erasures succeed.
    CHECK(!tk::vin_transition_cleanup_ready_for_marker_removal(false, true, true, true));
    CHECK(!tk::vin_transition_cleanup_ready_for_marker_removal(true, false, true, true));
    CHECK(!tk::vin_transition_cleanup_ready_for_marker_removal(true, true, false, true));
    CHECK(!tk::vin_transition_cleanup_ready_for_marker_removal(true, true, true, false));
    CHECK(!tk::vin_transition_cleanup_ready_for_marker_removal(false, false, false, false));
    CHECK(tk::vin_transition_cleanup_ready_for_marker_removal(true, true, true, true));
}

static void test_key_rotation() {
    for (bool allowed : {false, true}) for (int stamp_fault : {0, 1, 2})
    for (int retire_fault : {0, 1, 2}) {
        std::string order;
        const auto result = tk::stamp_or_retire_key_date(allowed,
            [&] { order += 'S'; if (stamp_fault == 2) throw 1; return stamp_fault == 0; },
            [&] { order += 'R'; if (retire_fault == 2) throw 1; return retire_fault == 0; });
        const bool stamped = allowed && stamp_fault == 0;
        CHECK(order == (stamped ? "S" : allowed ? "SR" : "R"));
        CHECK(result == (stamped ? tk::KeyCreatedDateResult::Stamped
                           : retire_fault == 0 ? tk::KeyCreatedDateResult::Retired
                                               : tk::KeyCreatedDateResult::Untrusted));
    }
    // Pin the old printf spelling for every byte value at each prefix position, and couple the
    // actual formatter to the VIN-journal validator/decoder. Tail digest bytes are not identity.
    for (size_t position = 0; position < 4; ++position) {
        for (unsigned byte = 0; byte < 256; ++byte) {
            uint8_t digest[20] = {0x01, 0x23, 0x45, 0x67};
            digest[position] = static_cast<uint8_t>(byte);
            digest[19] = 0xa5;
            const auto text = tk::key_fingerprint_text(digest, sizeof(digest));
            char previous[16];
            std::snprintf(previous, sizeof(previous), "%02X:%02X:%02X:%02X",
                static_cast<unsigned>(digest[0]), static_cast<unsigned>(digest[1]),
                static_cast<unsigned>(digest[2]), static_cast<unsigned>(digest[3]));
            CHECK(std::strcmp(text.data(), previous) == 0);
            CHECK(text[11] == '\0' && tk::vin_transition_key_id_valid(text.data()));
            const std::string encoded = tk::make_vin_transition_marker("synthetic", text.data());
            tk::VinTransitionMarker marker;
            CHECK(!encoded.empty() && tk::parse_vin_transition_marker(encoded, marker));
            CHECK(marker.previous_key_id == previous && marker.previous_vin == "synthetic");
        }
    }
    const uint8_t short_digest[3] = {0x01, 0x23, 0x45};
    CHECK(tk::key_fingerprint_text(nullptr, 20)[0] == '\0');
    CHECK(tk::key_fingerprint_text(short_digest, sizeof(short_digest))[0] == '\0');

    using A = tk::AutomaticKeyAction;
    using B = tk::KeyRotationBootState;
    using G = tk::KeyGenerationPreflight;
    using I = tk::InitialKeyBootAction;
    using P = tk::KeyRotationMarkerProbe;
    using R = tk::KeyRotationResult;
    using S = tk::VehicleTaskStartPhase;

    // NVS keys are limited to 15 bytes. This marker must stay in the tesla_ble namespace so
    // it is committed before private-key mutation and survives power loss independently of
    // HTTP/config state.
    CHECK(std::strlen(tk::kKeyRotationMarker) <= 15);

    // Marker absence and a failed/corrupt NVS probe are distinct safety states. Treating both
    // as blob_exists()==false would construct Vehicle and load stale sessions on a read error.
    CHECK(tk::classify_key_rotation_marker_probe(false, false) == P::Error);
    CHECK(tk::classify_key_rotation_marker_probe(false, true) == P::Error);
    CHECK(tk::classify_key_rotation_marker_probe(true, false) == P::Missing);
    CHECK(tk::classify_key_rotation_marker_probe(true, true) == P::Present);

    CHECK(tk::decide_key_rotation_boot(false, false, false, false) == B::Ready);
    CHECK(tk::decide_key_rotation_boot(true, false, false, false) == B::CleanupRequired);

    // Fault injection: any failed session erase, or a failed marker erase after otherwise
    // successful cleanup, keeps boot blocked. Only the fully durable terminal state is Ready.
    CHECK(tk::decide_key_rotation_boot(true, true, false, false) == B::Blocked);
    CHECK(tk::decide_key_rotation_boot(true, true, true, false) == B::Blocked);
    CHECK(tk::decide_key_rotation_boot(true, true, true, true) == B::Ready);

    // The erase sequences the firmware runs, driven through a fault-injecting storage that logs
    // every remove() in order. test_nvs_storage.cpp runs the same templates against the real
    // NvsStorageAdapter; this fake keeps the ordering contract in the logic suite.
    namespace NC = tk::nvs_contract;
    struct FakeStorage {
        std::vector<std::string> failing;
        std::vector<std::string> calls;
        bool remove(const std::string& key) {
            calls.push_back(key);
            return std::find(failing.begin(), failing.end(), key) == failing.end();
        }
        bool attempted(const std::string& key) const {
            return std::find(calls.begin(), calls.end(), key) != calls.end();
        }
    };
    const std::string marker = tk::kKeyRotationMarker;
    const std::vector<std::string> peers = {NC::kSessionVcsec, NC::kSessionInfotainment,
                                            NC::kPairedAt, NC::kKeyCreated};

    {   // Healthy boot recovery: every peer record incl. key_created, journal strictly LAST.
        FakeStorage s;
        const tk::KeyRotationBootCleanup c = tk::run_key_rotation_boot_cleanup(s);
        CHECK(c.date_removed && c.peers_erased() && c.marker_removed);
        CHECK(c.state() == B::Ready);
        CHECK((s.calls == std::vector<std::string>{NC::kSessionVcsec, NC::kSessionInfotainment,
                                                   NC::kPairedAt, NC::kKeyCreated, marker}));
    }
    for (const std::string& failing_key : peers) {
        // Any single failed peer erase blocks boot, never touches the journal, and does NOT
        // stop the remaining erases (each commits on its own and is idempotent on retry).
        FakeStorage s;
        s.failing = {failing_key};
        const tk::KeyRotationBootCleanup c = tk::run_key_rotation_boot_cleanup(s);
        CHECK(!c.peers_erased());
        CHECK(!c.marker_removed);
        CHECK(c.state() == B::Blocked);
        CHECK(!s.attempted(marker));
        CHECK(s.calls == peers);
    }
    {   // A failed erase of key_created is a failure in its own right: the old key's date must
        // not be allowed to outlive its key beside the new fingerprint after a reboot.
        FakeStorage s;
        s.failing = {NC::kKeyCreated};
        const tk::KeyRotationBootCleanup c = tk::run_key_rotation_boot_cleanup(s);
        CHECK(c.vcsec_removed && c.info_removed && c.paired_removed);
        CHECK(!c.date_removed);
        CHECK(c.state() == B::Blocked);
    }
    {   // Everything erased but the journal itself: still blocked, and the retry is the same
        // idempotent sequence.
        FakeStorage s;
        s.failing = {marker};
        const tk::KeyRotationBootCleanup c = tk::run_key_rotation_boot_cleanup(s);
        CHECK(c.peers_erased() && !c.marker_removed);
        CHECK(c.state() == B::Blocked);
        CHECK(s.calls.back() == marker);
    }

    using J = tk::KeyRotationJournalRetire;
    {   // Trusted date: key_created is left alone, only the journal is removed.
        FakeStorage s;
        bool untrusted = false;
        CHECK(tk::retire_key_rotation_journal(s, untrusted) == J::Retired);
        CHECK(!untrusted);
        CHECK((s.calls == std::vector<std::string>{marker}));
    }
    {   // Untrusted date: erased FIRST, the flag is cleared, then the journal.
        FakeStorage s;
        bool untrusted = true;
        CHECK(tk::retire_key_rotation_journal(s, untrusted) == J::Retired);
        CHECK(!untrusted);
        CHECK((s.calls == std::vector<std::string>{NC::kKeyCreated, marker}));
    }
    {   // Untrusted date that cannot be erased: the journal stays armed (never attempted) and the
        // date stays distrusted, so the supervisor/next boot retries.
        FakeStorage s;
        s.failing = {NC::kKeyCreated};
        bool untrusted = true;
        CHECK(tk::retire_key_rotation_journal(s, untrusted) == J::DatePending);
        CHECK(untrusted);
        CHECK(!s.attempted(marker));
    }
    {   // Date retired but the journal erase failed: the date erase is durable, so the flag is
        // cleared and the retry only has the journal left.
        FakeStorage s;
        s.failing = {marker};
        bool untrusted = true;
        CHECK(tk::retire_key_rotation_journal(s, untrusted) == J::JournalPending);
        CHECK(!untrusted);
        FakeStorage retry;
        CHECK(tk::retire_key_rotation_journal(retry, untrusted) == J::Retired);
        CHECK((retry.calls == std::vector<std::string>{marker}));
    }

    CHECK(!tk::key_rotation_committed(R::NotCommitted));
    CHECK(!tk::key_rotation_committed(R::CommitUnknown));
    CHECK(tk::key_rotation_committed(R::CleanupPending));
    CHECK(tk::key_rotation_committed(R::Complete));
    CHECK(!tk::key_rotation_runtime_safe(R::NotCommitted));
    CHECK(!tk::key_rotation_runtime_safe(R::CommitUnknown));
    CHECK(!tk::key_rotation_runtime_safe(R::CleanupPending));
    CHECK(tk::key_rotation_runtime_safe(R::Complete));
    CHECK(!tk::key_rotation_requires_reload(R::NotCommitted));
    CHECK(tk::key_rotation_requires_reload(R::CommitUnknown));
    CHECK(!tk::key_rotation_requires_reload(R::CleanupPending));
    CHECK(!tk::key_rotation_requires_reload(R::Complete));
    CHECK(!tk::key_rotation_reboot_confirms_ota(R::NotCommitted));
    CHECK(!tk::key_rotation_reboot_confirms_ota(R::CommitUnknown));
    CHECK(!tk::key_rotation_reboot_confirms_ota(R::CleanupPending));
    CHECK(tk::key_rotation_reboot_confirms_ota(R::Complete));

    // A failed first-boot commit gets one classification reboot. If marker recovery then finds
    // no durable key, this boot halts instead of performing the same ambiguous mutation again.
    CHECK(tk::decide_initial_key_boot_action(true, false) == I::UseExisting);
    CHECK(tk::decide_initial_key_boot_action(true, true) == I::UseExisting);
    CHECK(tk::decide_initial_key_boot_action(false, false) == I::Generate);
    CHECK(tk::decide_initial_key_boot_action(false, true) == I::HaltAfterEmptyRecovery);

    // Vehicle mutation tasks remain absent through controller wiring, VIN/key recovery and BLE
    // startup. Only the terminal state after every essential initializer may start them; safe
    // mode never may. HaltAfterEmptyRecovery exits while still in IdentityRecovery.
    CHECK(!tk::vehicle_tasks_may_start(S::ControllerWired, false));
    CHECK(!tk::vehicle_tasks_may_start(S::IdentityResolved, false));
    CHECK(!tk::vehicle_tasks_may_start(S::BleReady, false));
    CHECK(tk::vehicle_tasks_may_start(S::EssentialServicesReady, false));
    CHECK(!tk::vehicle_tasks_may_start(S::EssentialServicesReady, true));

    // No-force generation must distinguish an absent key from an unreadable key record.
    CHECK(tk::decide_key_generation_preflight(true, false, true) == G::Proceed);
    CHECK(tk::decide_key_generation_preflight(false, false, false) == G::ProbeFailed);
    CHECK(tk::decide_key_generation_preflight(false, true, true) == G::ExistingKeyRefused);
    CHECK(tk::decide_key_generation_preflight(false, true, false) == G::Proceed);

    // VIN staging requires both a successful private-key probe and, when present, a readable
    // fingerprint. Probe failure must never be treated like first-boot absence.
    CHECK(!tk::private_key_identity_verified(false, false, false));
    CHECK(tk::private_key_identity_verified(true, false, false));
    CHECK(!tk::private_key_identity_verified(true, true, false));
    CHECK(tk::private_key_identity_verified(true, true, true));

    // Automatic generation is limited to verified first-boot absence. A pairing hint requires
    // explicit recovery and can never authorize replacement of a durable identity.
    CHECK(tk::decide_automatic_key_action(false, false, false, false) ==
          A::StorageUnavailable);
    CHECK(tk::decide_automatic_key_action(true, false, false, false) == A::Generate);
    CHECK(tk::decide_automatic_key_action(true, true, false, false) ==
          A::RebootRequired);
    // A revocation flag cannot authorize another mutation after a prior key commit became
    // ambiguous. With a durable key present, unsafe RAM identity always requires reload first.
    CHECK(tk::decide_automatic_key_action(true, true, false, true) ==
          A::RebootRequired);
    CHECK(tk::decide_automatic_key_action(true, true, true, false) == A::Continue);
    CHECK(tk::decide_automatic_key_action(true, true, true, true) == A::RecoveryRequired);
    CHECK(tk::decide_automatic_key_action(true, false, true, false) ==
          A::RebootRequired);
    CHECK(tk::decide_automatic_key_action(true, false, false, true) == A::RecoveryRequired);
    CHECK(tk::decide_automatic_key_action(true, false, true, true) == A::RecoveryRequired);
}

static void test_nvs_string_load() {
    using P = tk::NvsStringProbe;
    using S = tk::NvsStringLoadState;

    // Fault-injected adapter contract: ONLY ESP_ERR_NVS_NOT_FOUND maps to Missing.
    CHECK(tk::classify_nvs_string_load(P::NotFound, 0, false, 0, false) == S::Missing);
    CHECK(tk::classify_nvs_string_load(P::NotFound, 99, true, 7, true) == S::Missing);
    CHECK(tk::classify_nvs_string_load(P::Error, 0, false, 0, false) == S::Error);
    CHECK(tk::classify_nvs_string_load(P::Error, 32, true, 32, true) == S::Error);

    // NVS strings include their trailing NUL. len=0 and len=1 (empty value) cannot be a valid,
    // nonempty safety journal. A failed second read or empty returned value also fails closed.
    CHECK(tk::classify_nvs_string_load(P::Ok, 0, true, 0, true) == S::Error);
    CHECK(tk::classify_nvs_string_load(P::Ok, 1, true, 1, false) == S::Error);
    CHECK(tk::classify_nvs_string_load(P::Ok, 8, false, 8, false) == S::Error);
    CHECK(tk::classify_nvs_string_load(P::Ok, 8, true, 8, false) == S::Error);
    CHECK(tk::classify_nvs_string_load(P::Ok, 8, true, 7, true) == S::Error);
    CHECK(tk::classify_nvs_string_load(P::Ok, 8, true, 9, true) == S::Error);
    CHECK(tk::classify_nvs_string_load(P::Ok, 8, true, 8, true) == S::Present);
    // Legacy config may explicitly store an empty WiFi/MQTT/Syslog value. This exception must
    // still reject probe/read failures and must not relax the nonempty VIN journal default.
    CHECK(tk::classify_nvs_string_load(P::Ok, 1, true, 1, true, true) == S::Present);
    CHECK(tk::classify_nvs_string_load(P::Ok, 1, false, 1, true, true) == S::Error);
    CHECK(tk::classify_nvs_string_load(P::Ok, 1, true, 2, true, true) == S::Error);
}

// ─── Home Assistant vehicle identity ─────────────────────────────────────────
static void test_ha_identity() {
    // The full VIN is retained (lowercase for a canonical MQTT-safe spelling): no shortened
    // serial and no hash collision can merge two vehicles into one HA device.
    CHECK(tk::ha_node_id_from_vin("5YJ3E1EA7KF000316") ==
          "teslakey_5yj3e1ea7kf000316");
    CHECK(tk::ha_node_id_from_vin("LRWYGCEK9PC000001") ==
          "teslakey_lrwygcek9pc000001");

    // Same VIN is the same entity identity regardless of which board calls this pure function;
    // changing the vehicle intentionally creates a different HA device.
    const std::string node = tk::ha_node_id_from_vin("5YJ3E1EA7KF000316");
    CHECK(node == tk::ha_node_id_from_vin("5YJ3E1EA7KF000316"));
    CHECK(node != tk::ha_node_id_from_vin("5YJ3E1EA7KF000317"));

    // MQTT can be configured before a VIN exists. The firmware detects this empty result and
    // uses a board-local setup fallback; an invalid string must never become a stable identity.
    CHECK(tk::ha_node_id_from_vin("").empty());
    CHECK(tk::ha_node_id_from_vin("UNKNOWN").empty());
    CHECK(tk::ha_node_id_from_vin("5yj3e1ea7kf000316").empty());
}

// ─── Syslog target parsing + send-failure classification ────────────────────────
static void test_syslog_policy() {
    std::string host; int port = 0;

    // host:port.
    CHECK(tk::syslog_target_parse("192.0.2.1:514", host, port));
    CHECK(host == "192.0.2.1" && port == 514);
    CHECK(tk::syslog_target_parse("syslog.lan:1514", host, port));
    CHECK(host == "syslog.lan" && port == 1514);

    // Bare host defaults to port 514.
    CHECK(tk::syslog_target_parse("192.0.2.1", host, port));
    CHECK(host == "192.0.2.1" && port == 514);

    // Rejected: empty, empty host/port either side of ':', a non-numeric or
    // out-of-range port.
    CHECK(!tk::syslog_target_parse("", host, port));
    CHECK(!tk::syslog_target_parse(":514", host, port));
    CHECK(!tk::syslog_target_parse("192.0.2.1:", host, port));
    CHECK(!tk::syslog_target_parse("192.0.2.1:abc", host, port));
    CHECK(!tk::syslog_target_parse("192.0.2.1:0", host, port));
    CHECK(!tk::syslog_target_parse("192.0.2.1:65536", host, port));

    // /set_syslog validation: empty always disables; a valid target passes; a
    // malformed one, or one carrying whitespace, is rejected.
    CHECK(tk::syslog_target_is_plausible(""));
    CHECK(tk::syslog_target_is_plausible("192.0.2.1:514"));
    CHECK(!tk::syslog_target_is_plausible("192.0.2.1:abc"));
    CHECK(!tk::syslog_target_is_plausible("192.0.2.1: 514"));
    CHECK(!tk::syslog_target_is_plausible(std::string(200, 'a') + ":514"));

    // Send-failure classification: routing/host errors are HARD (re-resolve now);
    // resource/blocking errors are TRANSIENT (hold the destination, let the
    // ordinary cadence re-check).
    CHECK(tk::syslog_error_is_hard(ENETUNREACH));
    CHECK(tk::syslog_error_is_hard(EHOSTUNREACH));
    CHECK(tk::syslog_error_is_hard(ENETDOWN));
    CHECK(tk::syslog_error_is_hard(EHOSTDOWN));
    CHECK(tk::syslog_error_is_hard(EADDRNOTAVAIL));
    CHECK(!tk::syslog_error_is_hard(ENOMEM));
    CHECK(!tk::syslog_error_is_hard(ENOBUFS));
    CHECK(!tk::syslog_error_is_hard(EAGAIN));
    CHECK(!tk::syslog_error_is_hard(EINTR));
    CHECK(!tk::syslog_error_is_hard(0));

    // Send-failure actions: the once-per-outage re-probe (the probe-storm fix). The FIRST hard
    // failure (not yet failing) pauses forwarding AND triggers one immediate re-resolve+re-probe;
    // a LATER hard failure in the same outage (already failing) still pauses forwarding but must
    // NOT re-probe — else getaddrinfo()+ping fires per queued line.
    CHECK(tk::syslog_send_failure_actions(/*hard*/true,  /*already_failing*/false).stop_forwarding);
    CHECK(tk::syslog_send_failure_actions(true,  false).reprobe_once);
    CHECK(tk::syslog_send_failure_actions(true,  true).stop_forwarding);
    CHECK(!tk::syslog_send_failure_actions(true, true).reprobe_once);   // the fix: no storm
    // Transient errors never pause forwarding or touch the throttle, whatever the latch state.
    CHECK(!tk::syslog_send_failure_actions(false, false).stop_forwarding);
    CHECK(!tk::syslog_send_failure_actions(false, false).reprobe_once);
    CHECK(!tk::syslog_send_failure_actions(false, true).stop_forwarding);
    CHECK(!tk::syslog_send_failure_actions(false, true).reprobe_once);

    // PRI derivation: the whole point is that severity DISCRIMINATES. The forwarder used to
    // stamp every line <14> (user.info), so a week of logs came back from the collector with
    // 61417 ESP_LOGE lines indistinguishable from heartbeats. Facility stays user (1) — only
    // the severity varies with the esp_log level letter.
    auto pri = [](const char* s) { return tk::syslog_pri_for_line(s, std::strlen(s)); };
    auto sev = [](const char* s) { return tk::syslog_severity_for_line(s, std::strlen(s)); };

    CHECK(sev("E (206191310) vehicle_ctrl: vehicle loop() threw") == 3);   // err
    CHECK(sev("I (79064752) vehicle_ctrl: HEAP free=71304") == 6);          // healthy trend = info
    CHECK(pri("I (79064752) vehicle_ctrl: HEAP free=71304") == 14);         // user.info
    CHECK(sev("I (79061122) http_server: REQ: POST /mcp") == 6);            // info
    CHECK(sev("D (1234) tag: noisy detail") == 7);                          // debug
    CHECK(sev("V (1234) tag: noisier detail") == 7);
    CHECK(pri("E (1) t: x") == 1 * 8 + 3);      // facility stays "user", severity varies
    CHECK(pri("I (1) t: x") == 14);             // the historical value, now only for real INFO

    // With CONFIG_LOG_COLORS=y (the IDF default) esp_log prefixes an SGR escape. Keying on
    // line[0] would classify every coloured line as unknown -> info, silently restoring the
    // constant-severity bug on a config change nobody would connect to syslog.
    CHECK(sev("\033[0;31mE (206191310) vehicle_ctrl: boom") == 3);
    CHECK(sev("\033[0;33mW (1234) main: careful") == 4);
    CHECK(sev("\033[0;32mI (1234) main: fine") == 6);

    // Unprefixed lines keep INFO rather than being guessed at. These are real forwarded lines:
    // tesla-ble and NimBLE print without an esp_log prefix, and a fabricated "error" for one of
    // them is a false alarm someone has to chase.
    CHECK(sev("Loaded private key from storage") == 6);
    CHECK(sev("[VCSEC Health Poll] Command completed successfully in 270 ms") == 6);
    CHECK(sev("Warning: this is prose, not a level") == 6);
    CHECK(sev("Established connection") == 6);   // leading 'E', but no " (" — not a level
    CHECK(sev("I am not a log level") == 6);     // leading 'I' + space, but no '('

    // Degenerate input must not read out of bounds or crash: the level test needs 3 chars,
    // and anything shorter falls back to info rather than indexing past the end.
    CHECK(sev("") == 6);
    CHECK(sev("E") == 6);
    CHECK(sev("E ") == 6);
    CHECK(sev("E (") == 3);      // exactly the prefix and nothing else — still a valid level
    CHECK(tk::syslog_severity_for_line(nullptr, 0) == 6);
    CHECK(sev("\033[") == 6);                    // truncated escape, no terminator
    CHECK(sev("\033[0;31m") == 6);               // escape only, nothing after it

    // Replay identity comes from the firmware caller, never a string prefix.
    for (const char* line : {"BOOT version=1", "CRASH task=worker", "E (1) ordinary", ""}) {
        CHECK(tk::syslog_pri_for_record(tk::SyslogRecord::Boot, line, std::strlen(line)) == 14);
        CHECK(tk::syslog_pri_for_record(tk::SyslogRecord::Crash, line, std::strlen(line)) == 11);
        CHECK(tk::syslog_pri_for_record(tk::SyslogRecord::Ordinary, line, std::strlen(line)) == pri(line));
    }
    CHECK(pri("CRASH task=worker") == 14);
    CHECK(tk::syslog_pri_for_record(tk::SyslogRecord::Ordinary, "W (1) warning", 13) == 12);
    CHECK(tk::syslog_pri_for_record(tk::SyslogRecord::Crash, nullptr, 0) == 11);
}

// ─── BLE connect-failure classification + rate limit ─────────────────────────────
static void test_connect_outcome() {
    // The scanner's verdict maps onto the cause. -1 (target never seen this window) is the
    // one that used to be logged as "connection timeout" although no connect was attempted.
    CHECK(tk::connect_fail_from_connectable(-1) == tk::ConnectFail::OutOfRange);
    CHECK(tk::connect_fail_from_connectable(0)  == tk::ConnectFail::AtBleLimit);
    CHECK(tk::connect_fail_from_connectable(1)  == tk::ConnectFail::ConnectFailed);
    CHECK(!tk::advert_seen_in_attempt(999, 1000));
    CHECK(tk::advert_seen_in_attempt(1000, 1000));
    CHECK(tk::advert_seen_in_attempt(1001, 1000));
    // A fresh SCAN_RSP must not promote an older/default connectability bit into the current
    // attempt. Both target identity and the primary-advert bit need their own fresh timestamp.
    CHECK(tk::connectable_verdict_in_attempt(true, true, 1001, 999, 1000) == -1);
    CHECK(tk::connectable_verdict_in_attempt(true, false, 999, 1001, 1000) == -1);
    CHECK(tk::connectable_verdict_in_attempt(false, true, 1001, 1001, 1000) == -1);
    CHECK(tk::connectable_verdict_in_attempt(true, false, 1000, 1000, 1000) == 0);
    CHECK(tk::connectable_verdict_in_attempt(true, true, 1001, 1001, 1000) == 1);

    // Foreground: an evcc/MCP/user request was blocked on this attempt. ALWAYS an error and
    // never rate-limited, whatever the cause or how long it has been failing — a request that
    // returned nothing must leave a line behind.
    {
        tk::ConnectFailState st;
        for (int i = 0; i < 300; i++) {
            CHECK(tk::connect_fail_note(st, tk::ConnectFail::OutOfRange,
                                        tk::ConnectOrigin::Foreground, (uint64_t)i)
                  == tk::ConnectLog::Error);
        }
        CHECK(st.streak == 300);
    }

    // Background, car simply away: warn once, then silence until a monotonic hour elapsed.
    // Model the unpaired supervisor's worst burst (roughly ten attempts per 40 s): attempt
    // counts must not turn the intended hourly heartbeat into one every few minutes.
    {
        tk::ConnectFailState st;
        constexpr uint64_t t0 = 1000;
        CHECK(tk::connect_fail_note(st, tk::ConnectFail::OutOfRange,
                                    tk::ConnectOrigin::Background, t0) == tk::ConnectLog::Warn);
        int emitted = 1;
        for (uint32_t i = 1; i <= 899; i++) {
            if (tk::connect_fail_note(st, tk::ConnectFail::OutOfRange,
                                      tk::ConnectOrigin::Background, t0 + i * 4000ULL)
                != tk::ConnectLog::Suppress)
                emitted++;
        }
        CHECK(st.streak == 900);
        CHECK(emitted == 1);  // 900 attempts in <1 h still emit only the first
        CHECK(tk::connect_fail_note(st, tk::ConnectFail::OutOfRange,
                                    tk::ConnectOrigin::Background,
                                    t0 + tk::kConnectFailRepeatMs) == tk::ConnectLog::Warn);
        CHECK(st.streak == 901);
    }

    // Background, car present but unusable: still an ERROR even unattended (this is the
    // two-boards-on-one-car signature), and still rate-limited so it can't storm either.
    {
        tk::ConnectFailState st;
        CHECK(tk::connect_fail_note(st, tk::ConnectFail::ConnectFailed,
                                    tk::ConnectOrigin::Background, 1000) == tk::ConnectLog::Error);
        CHECK(tk::connect_fail_note(st, tk::ConnectFail::ConnectFailed,
                                    tk::ConnectOrigin::Background, 1001) == tk::ConnectLog::Suppress);
        tk::ConnectFailState lim;
        CHECK(tk::connect_fail_note(lim, tk::ConnectFail::AtBleLimit,
                                    tk::ConnectOrigin::Background, 1000) == tk::ConnectLog::Error);
        CHECK(tk::connect_fail_note(lim, tk::ConnectFail::AtBleLimit,
                                    tk::ConnectOrigin::Background, 1001) == tk::ConnectLog::Suppress);
    }

    // A CHANGE of cause is reported immediately, mid-suppression. "The car came back but now
    // the connect fails" is the transition worth waking up for; folding it into the running
    // streak would hide it for up to an hour and defeat the rate limit's purpose.
    {
        tk::ConnectFailState st;
        for (int i = 0; i < 50; i++)
            tk::connect_fail_note(st, tk::ConnectFail::OutOfRange,
                                  tk::ConnectOrigin::Background, (uint64_t)i);
        CHECK(st.streak == 50);
        CHECK(tk::connect_fail_note(st, tk::ConnectFail::ConnectFailed,
                                    tk::ConnectOrigin::Background, 50) == tk::ConnectLog::Error);
        CHECK(st.streak == 1);   // the new cause starts its own run
    }

    // A successful connect closes the run, so the next failure reports as a first one rather
    // than inheriting the old streak's suppression.
    {
        tk::ConnectFailState st;
        for (int i = 0; i < 50; i++)
            tk::connect_fail_note(st, tk::ConnectFail::OutOfRange,
                                  tk::ConnectOrigin::Background, (uint64_t)i);
        tk::connect_ok_note(st);
        CHECK(st.streak == 0);
        CHECK(st.last_emit_ms == 0);
        CHECK(tk::connect_fail_note(st, tk::ConnectFail::OutOfRange,
                                    tk::ConnectOrigin::Background, 50) == tk::ConnectLog::Warn);
        CHECK(st.streak == 1);
    }

    // Saturating streak: a device left unreachable for months must not wrap its counter back
    // through 1 and re-announce as if the condition were new.
    {
        tk::ConnectFailState st{tk::ConnectFail::OutOfRange, UINT32_MAX, 0};
        tk::connect_fail_note(st, tk::ConnectFail::OutOfRange,
                              tk::ConnectOrigin::Background, 1);
        CHECK(st.streak == UINT32_MAX);
    }

    // Completion timeouts have an independent policy: auto-enrolment is known not to complete,
    // the non-waking health probe must still leave a first/hourly warning, and a blocked user call
    // warns every time. One command response closes the health-timeout run.
    {
        tk::CompletionTimeoutState st;
        CHECK(tk::completion_timeout_note(st, tk::CompletionTimeoutPolicy::ExpectedSilent, 0)
              == tk::CompletionTimeoutLog::Suppress);
        CHECK(st.streak == 0);
        CHECK(tk::completion_timeout_note(st, tk::CompletionTimeoutPolicy::ForegroundWarn, 0)
              == tk::CompletionTimeoutLog::Warn);
        CHECK(st.streak == 0);

        constexpr uint64_t t0 = 1000;
        CHECK(tk::completion_timeout_note(st, tk::CompletionTimeoutPolicy::BackgroundHealth, t0)
              == tk::CompletionTimeoutLog::Warn);
        int emitted = 1;
        for (uint32_t i = 1; i <= 89; i++) {
            if (tk::completion_timeout_note(st, tk::CompletionTimeoutPolicy::BackgroundHealth,
                                            t0 + i * 40000ULL)
                == tk::CompletionTimeoutLog::Warn) {
                emitted++;
            }
        }
        CHECK(st.streak == 90);
        CHECK(emitted == 1);
        CHECK(tk::completion_timeout_note(st, tk::CompletionTimeoutPolicy::BackgroundHealth,
                                          t0 + tk::kCompletionTimeoutRepeatMs)
              == tk::CompletionTimeoutLog::Warn);

        tk::completion_ok_note(st);
        CHECK(st.streak == 0);
        CHECK(st.last_emit_ms == 0);
        CHECK(tk::completion_timeout_note(st, tk::CompletionTimeoutPolicy::BackgroundHealth,
                                          t0 + tk::kCompletionTimeoutRepeatMs + 1)
              == tk::CompletionTimeoutLog::Warn);

        st.streak = UINT32_MAX;
        tk::completion_timeout_note(st, tk::CompletionTimeoutPolicy::BackgroundHealth,
                                    t0 + tk::kCompletionTimeoutRepeatMs + 2);
        CHECK(st.streak == UINT32_MAX);
    }

    // No-session setup guidance is INFO on entry and then only once per monotonic hour,
    // even though the supervisor can complete hundreds of read-only rounds in that interval.
    {
        tk::PeriodicLogState st;
        constexpr uint64_t t0 = 5000;
        CHECK(tk::periodic_log_due(st, t0, tk::kAutoPairNoticeRepeatMs));
        int emitted = 1;
        for (uint32_t i = 1; i <= 899; i++) {
            if (tk::periodic_log_due(st, t0 + i * 4000ULL,
                                     tk::kAutoPairNoticeRepeatMs)) {
                emitted++;
            }
        }
        CHECK(emitted == 1);
        CHECK(tk::periodic_log_due(st, t0 + tk::kAutoPairNoticeRepeatMs,
                                  tk::kAutoPairNoticeRepeatMs));
        tk::periodic_log_reset(st);
        CHECK(!st.active);
        CHECK(st.last_emit_ms == 0);
        CHECK(tk::periodic_log_due(st, t0 + tk::kAutoPairNoticeRepeatMs + 1,
                                  tk::kAutoPairNoticeRepeatMs));
    }

    // Every cause names itself; the text is what the log line carries.
    CHECK(std::strstr(tk::connect_fail_text(tk::ConnectFail::OutOfRange), "range") != nullptr);
    CHECK(std::strstr(tk::connect_fail_text(tk::ConnectFail::AtBleLimit), "non-connectable") != nullptr);
    CHECK(std::strstr(tk::connect_fail_text(tk::ConnectFail::ConnectFailed), "connect") != nullptr);
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

    // km -> miles (the /api evcc odometer stays in miles).
    CHECK_NEAR(tk::km_to_mi(0.0), 0.0);
    CHECK(tk::km_to_mi(tk::kMilesToKm) == 1.0);
    CHECK_NEAR(tk::km_to_mi(160.9344), 100.0);
    for (double mi : {0.0, 1.0, 12345.67, 250000.0}) CHECK_NEAR(tk::km_to_mi(tk::mi_to_km(mi)), mi);
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
    CHECK(base + tk::image_suffix(tk::Target::Esp32C6) + ".bin" == "tesla-key-esp32-c6.bin");
}

// ─── OTA manifest body and version freshness contracts ─────────────────────────
static void test_ota_contract() {
    using Order = tk::OtaVersionOrder;
    using Read = tk::BoundedBodyReadResult;

    static_assert(tk::ota_https_url("https://example.test/manifest.json"));
    static_assert(!tk::ota_https_url("http://example.test/manifest.json"));
    for (const auto url : {"https://example.test/manifest.json", "https://example.test:8443/"})
        CHECK(tk::ota_https_url(url));
    for (const auto url : {"", "http://example.test/", "HTTPS://example.test/", "//example.test/", " https://example.test/", "https://"})
        CHECK(!tk::ota_https_url(url));
    using Start = tk::OtaStartResult;
    for (const auto result : {Start::Started, Start::Busy, Start::PendingVerification, Start::Unavailable, Start::InternalError,Start::NoMemory}) {
        CHECK(tk::ota_start_http_status(result) == (result == Start::Started ? 200 : result == Start::Busy || result == Start::PendingVerification ? 409 : result == Start::InternalError ? 500 : 503));
        CHECK(std::strlen(tk::ota_start_reason(result, true)) > 0);
        CHECK(std::strlen(tk::ota_start_reason(result, false)) > 0);
    }
    CHECK_STR(tk::ota_start_reason(Start::PendingVerification, false), "running firmware is still pending verification");
    CHECK(tk::ota_start_http_status(Start::NoMemory)==503);
    CHECK_STR(tk::ota_start_reason(Start::NoMemory,true),"not enough memory for update");
    CHECK_STR(tk::ota_start_reason(Start::NoMemory,false),"not enough memory for update");
    CHECK_STR(tk::ota_verification_name(tk::OtaVerificationState::Stable), "stable");
    CHECK_STR(tk::ota_verification_name(tk::OtaVerificationState::PendingVerify), "pending_verify");
    CHECK_STR(tk::ota_verification_name(tk::OtaVerificationState::Unknown), "unknown");

    CHECK(tk::canonical_ota_version("0.0.0"));
    CHECK(tk::canonical_ota_version("1.4.84"));
    CHECK(tk::canonical_ota_version("1.4.84-PR-123"));
    CHECK(tk::canonical_ota_version("1.4.84-dev.7"));
    CHECK(!tk::canonical_ota_version(""));
    CHECK(!tk::canonical_ota_version("+1.2.3"));
    CHECK(!tk::canonical_ota_version("-1.2.3"));
    CHECK(!tk::canonical_ota_version("01.2.3"));
    CHECK(!tk::canonical_ota_version("1.02.3"));
    CHECK(!tk::canonical_ota_version("1.2.03"));
    CHECK(!tk::canonical_ota_version("1.2"));
    CHECK(!tk::canonical_ota_version("1.2.3x"));
    CHECK(!tk::canonical_ota_version("1.2.3 "));
    CHECK(!tk::canonical_ota_version("1.2.3-"));
    CHECK(!tk::canonical_ota_version("1.2.3+meta"));

    const std::string max_version = "1.2.3-" + std::string(25, 'a');
    const std::string overlong_version = "1.2.3-" + std::string(26, 'a');
    CHECK(max_version.size() == tk::kOtaVersionMaxBytes);
    CHECK(tk::canonical_ota_version(max_version));
    CHECK(overlong_version.size() == tk::kOtaVersionMaxBytes + 1);
    CHECK(!tk::canonical_ota_version(overlong_version));

    // Magnitude uses digit spans, not bounded integer conversion: these values exceed uint64_t,
    // yet compare deterministically without scanf overflow/UB.
    CHECK(tk::compare_ota_versions("18446744073709551616.0.0",
                                   "9999999999999999999.0.0") == Order::Newer);
    CHECK(tk::compare_ota_versions("9999999999999999999.0.0",
                                   "18446744073709551616.0.0") == Order::Older);
    CHECK(tk::compare_ota_versions("1.10.0", "1.9.99") == Order::Newer);
    CHECK(tk::compare_ota_versions("1.2.3", "1.2.3") == Order::Equal);
    CHECK(tk::compare_ota_versions("1.2.3-dev", "1.2.3") == Order::Equal);
    CHECK(tk::compare_ota_versions("01.2.3", "1.2.2") == Order::Invalid);

    // PR suffix parsing and targeted OTA availability
    CHECK(tk::parse_pr_suffix("PR-326") == 326);
    CHECK(tk::parse_pr_suffix("PR-1") == 1);
    CHECK(tk::parse_pr_suffix("PR-9999999") == 9999999);
    CHECK(tk::parse_pr_suffix("PR-0") == 0);
    CHECK(tk::parse_pr_suffix("PR-") == 0);
    CHECK(tk::parse_pr_suffix("PR-abc") == 0);
    CHECK(tk::parse_pr_suffix("dev") == 0);
    CHECK(tk::parse_pr_suffix("") == 0);

    CHECK(tk::parse_version_pr("1.5.4-PR-326") == 326);
    CHECK(tk::parse_version_pr("1.5.4") == 0);
    CHECK(tk::parse_version_pr("invalid") == 0);

    CHECK(tk::parse_pr_query("326") == 326);
    CHECK(tk::parse_pr_query("0") == 0);
    CHECK(tk::parse_pr_query("01") == 0);
    CHECK(tk::parse_pr_query("-1") == 0);
    CHECK(tk::parse_pr_query("abc") == 0);
    CHECK(tk::parse_pr_query("") == 0);

    CHECK(!tk::ota_query_parameter("ms=1800000000000", "pr").present);
    CHECK(!tk::ota_query_parameter("prefixpr=7", "pr").present);
    CHECK(!tk::ota_query_parameter("%70r=7", "pr").present);
    CHECK(tk::ota_query_parameter("ms=1&PR=7", "pr").value == "7");
    CHECK(tk::ota_query_parameter("pr=7&pr=8", "pr").value == "7");
    CHECK(tk::ota_query_parameter("pr=", "pr").present);
    CHECK(tk::ota_query_parameter("pr=", "pr").value.empty());
    CHECK(tk::ota_query_parameter("pr", "pr").present);
    CHECK(!tk::ota_query_parameter("pr", "pr").valid);
    CHECK(!tk::ota_query_parameter("x=1&MS&y=2", "ms").valid);
    CHECK(tk::ota_query_parameter("x=1&MS=1800000000000&y=2", "ms").value == "1800000000000");

    char url_buf[128];
    CHECK(tk::format_pr_manifest_url(326, url_buf, sizeof(url_buf)));
    CHECK(std::string_view(url_buf) == "https://0bu.github.io/tesla-key-esp32/PR/326/manifest.json");
    CHECK(!tk::format_pr_manifest_url(0, url_buf, sizeof(url_buf)));
    CHECK(!tk::format_pr_manifest_url(326, url_buf, 30));

    CHECK(tk::format_pr_firmware_url(326, "", url_buf, sizeof(url_buf)));
    CHECK(std::string_view(url_buf) == "https://0bu.github.io/tesla-key-esp32/PR/326/tesla-key-esp32.bin");
    CHECK(tk::format_pr_firmware_url(326, "-s3", url_buf, sizeof(url_buf)));
    CHECK(std::string_view(url_buf) == "https://0bu.github.io/tesla-key-esp32/PR/326/tesla-key-esp32-s3.bin");
    CHECK(!tk::format_pr_firmware_url(326, "-invalid", url_buf, sizeof(url_buf)));
    CHECK(!tk::format_pr_firmware_url(0, "", url_buf, sizeof(url_buf)));

    // Standard channel (target_pr == 0)
    CHECK(tk::is_ota_update_available("1.5.5", "1.5.4", 0));
    CHECK(!tk::is_ota_update_available("1.5.4", "1.5.4", 0));
    CHECK(!tk::is_ota_update_available("1.5.3", "1.5.4", 0));
    // Returning from PR build to stable release of same core
    CHECK(tk::is_ota_update_available("1.5.4", "1.5.4-PR-326", 0));
    // PR candidate not accepted on standard channel
    CHECK(!tk::is_ota_update_available("1.5.4-PR-326", "1.5.4", 0));

    // Targeted PR channel (target_pr == 326)
    CHECK(tk::is_ota_update_available("1.5.4-PR-326", "1.5.4", 326));
    CHECK(tk::is_ota_update_available("1.5.4-PR-326", "1.5.4-PR-326", 326));
    CHECK(tk::is_ota_update_available("1.5.5-PR-326", "1.5.4-PR-326", 326));
    CHECK(tk::is_ota_update_available("1.5.5-PR-326", "1.5.4", 326));
    // Wrong PR candidate rejected
    CHECK(!tk::is_ota_update_available("1.5.4-PR-227", "1.5.4", 326));
    // Downgrade to older core rejected
    CHECK(!tk::is_ota_update_available("1.4.30-PR-326", "1.5.4", 326));
    // Candidate without PR suffix rejected on PR channel
    CHECK(!tk::is_ota_update_available("1.5.4", "1.5.4", 326));

    char unterminated[32];
    std::memset(unterminated, '1', sizeof(unterminated));
    const auto unterminated_view = tk::bounded_c_string_view(unterminated);
    CHECK(unterminated_view.size() == sizeof(unterminated));
    CHECK(!tk::canonical_ota_version(unterminated_view));

    tk::BoundedHttpBodyGate exact_limit(8192, false);
    CHECK(exact_limit.valid_headers());
    CHECK(exact_limit.next_read_size(512) == 512);
    CHECK(exact_limit.accept_read(8191, false) == Read::Continue);
    CHECK(exact_limit.next_read_size(512) == 2);  // last accepted byte plus oversize probe
    CHECK(exact_limit.accept_read(1, true) == Read::Complete);
    CHECK(exact_limit.bytes_received() == 8192);

    tk::BoundedHttpBodyGate over_limit(8193, false);
    CHECK(!over_limit.valid_headers());
    CHECK(over_limit.accept_read(1, true) == Read::Reject);

    tk::BoundedHttpBodyGate negative_after_prefix(10, false);
    CHECK(negative_after_prefix.accept_read(5, false) == Read::Continue);
    CHECK(negative_after_prefix.accept_read(-1, false) == Read::Reject);

    tk::BoundedHttpBodyGate truncated(10, false);
    CHECK(truncated.accept_read(5, false) == Read::Continue);
    CHECK(truncated.accept_read(0, true) == Read::Reject);

    tk::BoundedHttpBodyGate premature_complete(10, false);
    CHECK(premature_complete.accept_read(5, true) == Read::Reject);

    tk::BoundedHttpBodyGate empty(0, false);
    CHECK(!empty.valid_headers());
    CHECK(empty.accept_read(0, true) == Read::Reject);

    tk::BoundedHttpBodyGate chunked_exact(0, true);
    CHECK(chunked_exact.valid_headers());
    CHECK(chunked_exact.accept_read(8192, true) == Read::Complete);

    tk::BoundedHttpBodyGate chunked_over(0, true);
    CHECK(chunked_over.accept_read(8192, false) == Read::Continue);
    CHECK(chunked_over.next_read_size(512) == 1);
    CHECK(chunked_over.accept_read(1, true) == Read::Reject);

    std::string depth_16_manifest = "{\"version\":\"1.2.3\",\"ignored\":";
    depth_16_manifest.append(15, '[');  // root object + 15 arrays = the accepted depth 16
    depth_16_manifest += '0';
    depth_16_manifest.append(15, ']');
    depth_16_manifest += '}';
    CHECK(tk::json_syntax_status(depth_16_manifest.data(), depth_16_manifest.size()) ==
          tk::JsonSyntaxStatus::Valid);
    std::string depth_17_manifest = "{\"version\":\"1.2.3\",\"ignored\":";
    depth_17_manifest.append(16, '[');
    depth_17_manifest += '0';
    depth_17_manifest.append(16, ']');
    depth_17_manifest += '}';
    CHECK(tk::json_syntax_status(depth_17_manifest.data(), depth_17_manifest.size()) ==
          tk::JsonSyntaxStatus::TooDeep);
    const std::string nul_key = "{\"ver\\u0000sion\":\"1.2.3\"}";
    const std::string nul_value = "{\"version\":\"1.2.3\\u0000junk\"}";
    CHECK(tk::json_syntax_status(nul_key.data(), nul_key.size()) ==
          tk::JsonSyntaxStatus::UnsupportedNul);
    CHECK(tk::json_syntax_status(nul_value.data(), nul_value.size()) ==
          tk::JsonSyntaxStatus::UnsupportedNul);
    const std::string trailing = "{\"version\":\"1.2.3\"}junk";
    CHECK(tk::json_syntax_status(trailing.data(), trailing.size()) ==
          tk::JsonSyntaxStatus::Malformed);
}

// ─── OTA update channel selection & URL routing ──────────────────────────────
static void test_ota_channel() {
    using tk::OtaChannel;

    // Names and validation
    CHECK(std::string(tk::ota_channel_name(OtaChannel::Release)) == "release");
    CHECK(std::string(tk::ota_channel_name(OtaChannel::Dev)) == "dev");
    CHECK(tk::ota_channel_valid("release"));
    CHECK(tk::ota_channel_valid("dev"));
    CHECK(!tk::ota_channel_valid(""));
    CHECK(!tk::ota_channel_valid("main"));
    CHECK(!tk::ota_channel_valid("beta"));
    CHECK(!tk::ota_channel_valid("RELEASE"));
    CHECK(!tk::ota_channel_valid("dev\n"));

    // Parsing
    CHECK(tk::ota_channel_parse("release") == OtaChannel::Release);
    CHECK(tk::ota_channel_parse("dev") == OtaChannel::Dev);
    CHECK(tk::ota_channel_parse("unknown", OtaChannel::Release) == OtaChannel::Release);
    CHECK(tk::ota_channel_parse("unknown", OtaChannel::Dev) == OtaChannel::Dev);

    // Int conversion for the OTA flag bits in config blob v1
    CHECK(tk::ota_channel_to_int(OtaChannel::Release) == 0);
    CHECK(tk::ota_channel_to_int(OtaChannel::Dev) == 1);
    CHECK(tk::ota_channel_from_int(0) == OtaChannel::Release);
    CHECK(tk::ota_channel_from_int(1) == OtaChannel::Dev);
    CHECK(tk::ota_channel_from_int(99) == OtaChannel::Release);

    // Static buffer zero-alloc formatters for dev channel
    char dev_manifest_buf[128]{};
    CHECK(tk::format_dev_manifest_url(dev_manifest_buf, sizeof(dev_manifest_buf)));
    CHECK(std::string(dev_manifest_buf) == "https://0bu.github.io/tesla-key-esp32/dev/manifest.json");
    CHECK(!tk::format_dev_manifest_url(dev_manifest_buf, 10));

    char dev_fw_buf[128]{};
    CHECK(tk::format_dev_firmware_url("-s3", dev_fw_buf, sizeof(dev_fw_buf)));
    CHECK(std::string(dev_fw_buf) == "https://0bu.github.io/tesla-key-esp32/dev/tesla-key-esp32-s3.bin");
    CHECK(tk::format_dev_firmware_url("", dev_fw_buf, sizeof(dev_fw_buf)));
    CHECK(std::string(dev_fw_buf) == "https://0bu.github.io/tesla-key-esp32/dev/tesla-key-esp32.bin");
    CHECK(!tk::format_dev_firmware_url("-invalid", dev_fw_buf, sizeof(dev_fw_buf)));

    // resolve_manifest_url_into
    std::string res_out;
    const char* default_manifest = "https://example.com/manifest.json";
    CHECK_STR(tk::resolve_manifest_url_into(0, OtaChannel::Release, default_manifest, res_out),
              default_manifest);
    CHECK_STR(tk::resolve_manifest_url_into(0, OtaChannel::Dev, default_manifest, res_out),
              "https://0bu.github.io/tesla-key-esp32/dev/manifest.json");
    CHECK_STR(tk::resolve_manifest_url_into(331, OtaChannel::Dev, default_manifest, res_out),
              "https://0bu.github.io/tesla-key-esp32/PR/331/manifest.json");
    CHECK_STR(tk::resolve_manifest_url_into(331, OtaChannel::Release, default_manifest, res_out),
              "https://0bu.github.io/tesla-key-esp32/PR/331/manifest.json");

    // Dev version classification & channel default
    CHECK(tk::parse_dev_suffix("dev") == 0);
    CHECK(tk::parse_dev_suffix("dev.1") == 1);
    CHECK(tk::parse_dev_suffix("dev.42") == 42);
    CHECK(tk::parse_dev_suffix("") == -1);
    CHECK(tk::parse_dev_suffix("PR-1") == -1);
    CHECK(tk::parse_dev_suffix("rc.1") == -1);

    CHECK(tk::ota_version_is_dev("1.4.84-dev"));
    CHECK(tk::ota_version_is_dev("1.4.84-dev.1"));
    CHECK(tk::ota_version_is_dev("1.4.84-dev.12"));
    CHECK(!tk::ota_version_is_dev("1.4.84"));
    CHECK(!tk::ota_version_is_dev("1.4.84-rc.1"));
    CHECK(!tk::ota_version_is_dev("1.4.84-PR-42"));
    CHECK(!tk::ota_version_is_dev(""));

    CHECK(tk::default_ota_channel_for_version("1.4.84") == OtaChannel::Release);
    CHECK(tk::default_ota_channel_for_version("1.4.84-dev.5") == OtaChannel::Dev);

    // Canonical version parsing and version comparison
    CHECK(tk::canonical_ota_version("1.2.3"));
    CHECK(tk::canonical_ota_version("1.2.3-dev.1"));
    CHECK(!tk::canonical_ota_version("1.2"));
    CHECK(!tk::canonical_ota_version("v1.2.3"));
    CHECK(!tk::canonical_ota_version(""));

    CHECK(tk::compare_ota_versions("1.2.4", "1.2.3") == tk::OtaVersionOrder::Newer);
    CHECK(tk::compare_ota_versions("1.2.3", "1.2.4") == tk::OtaVersionOrder::Older);
    CHECK(tk::compare_ota_versions("1.2.3", "1.2.3") == tk::OtaVersionOrder::Equal);

    // is_ota_update_available: Release channel
    // Release channel requires official release candidate (no suffix)
    CHECK(!tk::is_ota_update_available("1.2.4-dev.1", "1.2.3", 0, OtaChannel::Release));
    // Release channel: newer stable version is accepted
    CHECK(tk::is_ota_update_available("1.2.4", "1.2.3", 0, OtaChannel::Release));
    // Release channel: older stable version is rejected
    CHECK(!tk::is_ota_update_available("1.2.2", "1.2.3", 0, OtaChannel::Release));
    CHECK(!tk::is_ota_update_available("1.2.3", "1.2.3", 0, OtaChannel::Release));
    // Release channel: switching Dev -> Release allows installing stable release (downgrade allowed)
    CHECK(tk::is_ota_update_available("1.2.0", "1.2.4-dev.5", 0, OtaChannel::Release));
    CHECK(tk::is_ota_update_available("1.2.4", "1.2.4-dev.5", 0, OtaChannel::Release));
    // Release channel: PR or other non-dev pre-release build can return to stable of same core
    CHECK(tk::is_ota_update_available("1.2.3", "1.2.3-PR-42", 0, OtaChannel::Release));

    // is_ota_update_available: Dev channel
    // Candidate must be a dev build
    CHECK(!tk::is_ota_update_available("1.2.4", "1.2.3", 0, OtaChannel::Dev));
    // Newer core version dev build is accepted
    CHECK(tk::is_ota_update_available("1.3.0-dev.1", "1.2.3-dev.50", 0, OtaChannel::Dev));
    // Same core: monotonic dev updates
    CHECK(tk::is_ota_update_available("1.2.3-dev.6", "1.2.3-dev.5", 0, OtaChannel::Dev));
    // Same core: equal dev version rejected
    CHECK(!tk::is_ota_update_available("1.2.3-dev.5", "1.2.3-dev.5", 0, OtaChannel::Dev));
    // Downgrade between dev builds is REJECTED
    CHECK(!tk::is_ota_update_available("1.2.3-dev.4", "1.2.3-dev.5", 0, OtaChannel::Dev));
    CHECK(!tk::is_ota_update_available("1.2.2-dev.99", "1.2.3-dev.1", 0, OtaChannel::Dev));
    // Current is stable release: dev build of same or newer core is accepted
    CHECK(tk::is_ota_update_available("1.2.3-dev.1", "1.2.3", 0, OtaChannel::Dev));
    CHECK(tk::is_ota_update_available("1.2.4-dev.1", "1.2.3", 0, OtaChannel::Dev));
    CHECK(!tk::is_ota_update_available("1.2.2-dev.1", "1.2.3", 0, OtaChannel::Dev));

    // is_ota_update_available: PR preview
    CHECK(tk::is_ota_update_available("1.2.3-PR-331", "1.2.3", 331));
    CHECK(!tk::is_ota_update_available("1.2.3-PR-332", "1.2.3", 331)); // mismatched PR
    CHECK(!tk::is_ota_update_available("1.2.2-PR-331", "1.2.3", 331)); // older core
    CHECK(tk::is_ota_update_available("1.2.3-PR-331", "1.2.3-PR-331", 331)); // same PR version accepted for new pushes
    CHECK(tk::is_ota_update_available("1.2.4-PR-331", "1.2.3-PR-331", 331)); // newer core on same PR
}

// ─── OTA changelog parsing and range selection ──────────────────────────────
static void test_ota_changelog_range() {
    using Result = tk::OtaChangelogRangeResult;

    // ota_manifest_sibling_url
    char sibling[128] = {};
    CHECK(tk::ota_manifest_sibling_url("https://example.com/manifest.json", "changelog.json", sibling, sizeof(sibling)));
    CHECK_STR(sibling, "https://example.com/changelog.json");

    CHECK(tk::ota_manifest_sibling_url("https://example.com/dev/manifest.json", "changelog.json", sibling, sizeof(sibling)));
    CHECK_STR(sibling, "https://example.com/dev/changelog.json");

    CHECK(tk::ota_manifest_sibling_url("https://0bu.github.io/tesla-key-esp32/PR/42/manifest.json", "changelog.json", sibling, sizeof(sibling)));
    CHECK_STR(sibling, "https://0bu.github.io/tesla-key-esp32/PR/42/changelog.json");

    CHECK(!tk::ota_manifest_sibling_url("", "changelog.json", sibling, sizeof(sibling)));
    CHECK(!tk::ota_manifest_sibling_url("invalid_url_without_slash", "changelog.json", sibling, sizeof(sibling)));
    CHECK(!tk::ota_manifest_sibling_url("https://example.com/manifest.json", "", sibling, sizeof(sibling)));
    CHECK(!tk::ota_manifest_sibling_url("https://example.com/manifest.json", "changelog.json", sibling, 10)); // too small

    // compare_ota_full_versions
    CHECK(tk::compare_ota_full_versions("1.2.0", "1.2.1") == -1);
    CHECK(tk::compare_ota_full_versions("1.2.1", "1.2.0") == 1);
    CHECK(tk::compare_ota_full_versions("1.2.1", "1.2.1") == 0);
    CHECK(tk::compare_ota_full_versions("1.2.1-dev.3", "1.2.1-dev.5") == -1);
    CHECK(tk::compare_ota_full_versions("1.2.1-dev.12", "1.2.1-dev.5") == 1);
    CHECK(tk::compare_ota_full_versions("1.2.1-dev.5", "1.2.1") == -1);
    CHECK(tk::compare_ota_full_versions("1.2.1", "1.2.1-dev.5") == 1);
    CHECK(tk::compare_ota_full_versions("invalid", "1.2.1") == -2);

    // manifest_changelog JSON parsing
    const char valid_json[] = "{\"version\":\"1.2.3\",\"changelog\":\"First change\\nSecond change\\\"escaped\\\"\"}";
    char decoded[256] = {};
    CHECK(tk::manifest_changelog(valid_json, std::strlen(valid_json), "1.2.3", decoded, sizeof(decoded)));
    CHECK_STR(decoded, "First change\nSecond change\"escaped\"");

    // Mismatched version fails closed
    char fail_buf[256] = {};
    CHECK(!tk::manifest_changelog(valid_json, std::strlen(valid_json), "1.2.4", fail_buf, sizeof(fail_buf)));
    CHECK(fail_buf[0] == '\0');

    // Unknown top-level fields fail closed
    const char extra_field_json[] = "{\"version\":\"1.2.3\",\"changelog\":\"note\",\"extra\":\"bad\"}";
    CHECK(!tk::manifest_changelog(extra_field_json, std::strlen(extra_field_json), "1.2.3", fail_buf, sizeof(fail_buf)));

    // ota_changelog_select_range: legacy notes
    char legacy_buf[256] = "Maintenance and reliability improvements.\nFix corner case.";
    CHECK(tk::ota_changelog_select_range(legacy_buf, "1.2.0", "1.2.1") == Result::Legacy);
    CHECK_STR(legacy_buf, "Maintenance and reliability improvements.\nFix corner case.");

    // ota_changelog_select_range: versioned notes
    char versioned_buf[512] =
        "v1.2.1-dev.1 — Initial dev work\n"
        "v1.2.1-dev.2 — Fix heap issue\n"
        "v1.2.1-dev.3 — Add release channel";

    // Running 1.2.1-dev.1, target 1.2.1-dev.3 -> keeps dev.2 and dev.3
    CHECK(tk::ota_changelog_select_range(versioned_buf, "1.2.1-dev.1", "1.2.1-dev.3") == Result::Selected);
    CHECK_STR(versioned_buf, "v1.2.1-dev.2 — Fix heap issue\nv1.2.1-dev.3 — Add release channel");

    // Running 1.2.0 (older stable), target 1.2.1-dev.3 -> keeps all dev notes
    char all_dev_buf[512] =
        "v1.2.1-dev.1 — Initial dev work\n"
        "v1.2.1-dev.2 — Fix heap issue\n"
        "v1.2.1-dev.3 — Add release channel";
    CHECK(tk::ota_changelog_select_range(all_dev_buf, "1.2.0", "1.2.1-dev.3") == Result::Selected);
    CHECK_STR(all_dev_buf, "v1.2.1-dev.1 — Initial dev work\nv1.2.1-dev.2 — Fix heap issue\nv1.2.1-dev.3 — Add release channel");

    // Target mismatch with last entry -> Invalid
    char mismatch_buf[512] =
        "v1.2.1-dev.1 — Initial dev work\n"
        "v1.2.1-dev.2 — Fix heap issue";
    CHECK(tk::ota_changelog_select_range(mismatch_buf, "1.2.1-dev.1", "1.2.1-dev.3") == Result::Invalid);
    CHECK(mismatch_buf[0] == '\0');

    // Running >= target -> Invalid
    char downgrade_buf[512] =
        "v1.2.1-dev.1 — Initial dev work\n"
        "v1.2.1-dev.2 — Fix heap issue";
    CHECK(tk::ota_changelog_select_range(downgrade_buf, "1.2.1-dev.2", "1.2.1-dev.2") == Result::Invalid);
    CHECK(downgrade_buf[0] == '\0');
}

// ─── atomic two-task start / second-create fault injection ───────────────────
static void test_task_start_gate() {
    using Action = tk::TaskEntryAction;
    using State = tk::TaskStartState;

    tk::DualTaskStartGate gate;
    CHECK(gate.state() == State::Idle);
    CHECK(gate.begin());

    // Fault injection: task one has entered, but task two's xTaskCreate has not returned yet.
    // The entry may only wait; the protected-resource counter models TWDT/mutex/BLE/vehicle work.
    int protected_resource_touches = 0;
    Action first = gate.entry_action();
    if (first == Action::Run) ++protected_resource_touches;
    CHECK(first == Action::Wait);
    CHECK(protected_resource_touches == 0);

    // Simulate task-two allocation failure. Task one observes Cancel, acknowledges exactly once,
    // and would self-delete; it still cannot touch a protected resource.
    CHECK(gate.cancel());
    first = gate.entry_action();
    if (first == Action::Run) ++protected_resource_touches;
    CHECK(first == Action::Cancel);
    gate.acknowledge_cancel();
    CHECK(gate.cancelled_tasks_acknowledged(1));
    CHECK(!gate.cancelled_tasks_acknowledged(0));
    CHECK(protected_resource_touches == 0);
    CHECK(gate.reset_cancelled(1));
    CHECK(gate.state() == State::Idle);

    // The success path releases both entries only after both creates are proven.
    CHECK(gate.begin());
    CHECK(gate.entry_action() == Action::Wait);
    CHECK(gate.release());
    CHECK(gate.entry_action() == Action::Run);
    CHECK(!gate.cancel());
    CHECK(!gate.begin());
}

// ─── whole-runtime vehicle admission / partial-boot fault injection ──────────
#include "config_repair_cases.hpp"
static void test_config_repair() {
    repair_fixture::cases();
}

static void test_runtime_admission() {
    CHECK(std::string(tk::runtime_admission_state_name(tk::RuntimeAdmissionState::Booting)) == "booting");
    CHECK(std::string(tk::runtime_admission_state_name(tk::RuntimeAdmissionState::Ready)) == "ready");
    CHECK(std::string(tk::runtime_admission_state_name(tk::RuntimeAdmissionState::SafeMode)) == "safe_mode");
    CHECK(std::string(tk::runtime_admission_state_name(tk::RuntimeAdmissionState::Fatal)) == "fatal");
    CHECK(std::string(tk::runtime_admission_state_name(static_cast<tk::RuntimeAdmissionState>(255))) == "fatal");
    using AKA = tk::AutomaticKeyAction;
    for(auto action : {AKA::Continue, AKA::Generate, AKA::RecoveryRequired})
        CHECK(tk::pairing_block_reason(false, false, false, action) == nullptr);
    CHECK(std::string(tk::pairing_block_reason(false, false, false, AKA::StorageUnavailable)) == "key_storage_unavailable");
    CHECK(std::string(tk::pairing_block_reason(false, false, false, AKA::RebootRequired)) == "key_runtime_mismatch");
    CHECK(std::string(tk::pairing_block_reason(true, true, true, AKA::StorageUnavailable)) == "key_reload_required");
    CHECK(std::string(tk::pairing_block_reason(false, true, true, AKA::Continue)) == "vin_transition_pending");
    CHECK(std::string(tk::pairing_block_reason(false, true, false, AKA::Continue)) == "key_cleanup_pending");

    using Action = tk::RuntimeAdmissionAction;
    using State = tk::RuntimeAdmissionState;

    tk::RuntimeAdmissionGate normal;
    CHECK(normal.state() == State::Booting);
    CHECK(normal.action() == Action::Wait);
    CHECK(!normal.vehicle_ready());
    CHECK(normal.mark_ready());
    CHECK(normal.state() == State::Ready);
    CHECK(normal.action() == Action::Run);
    CHECK(normal.vehicle_ready());
    CHECK(!normal.mark_safe_mode());

    // Every late essential-start fault closes a previously admitted surface as defense in depth.
    normal.mark_fatal();
    CHECK(normal.state() == State::Fatal);
    CHECK(normal.action() == Action::Stop);
    CHECK(!normal.vehicle_ready());
    CHECK(!normal.mark_ready());

    tk::RuntimeAdmissionGate safe_mode;
    CHECK(safe_mode.mark_safe_mode());
    CHECK(safe_mode.state() == State::SafeMode);
    CHECK(safe_mode.action() == Action::Stop);
    CHECK(!safe_mode.vehicle_ready());
    CHECK(!safe_mode.mark_ready());

    tk::RuntimeAdmissionGate partial_boot;
    partial_boot.mark_fatal();
    CHECK(partial_boot.action() == Action::Stop);
    CHECK(!partial_boot.mark_ready());
}

// F142: cached names are diagnostic; only a fresh owned shell may reserve a restart.
static void test_identity_recovery() {
    using State = tk::RuntimeAdmissionState;
    using Component = tk::RuntimeFatalComponent;
    tk::RuntimeAdmissionGate gate;
    CHECK(gate.snapshot().component == Component::Unknown);
    CHECK(gate.mark_ready());
    gate.mark_fatal(Component::Http);
    CHECK(gate.snapshot().state == State::Fatal);
    CHECK(gate.snapshot().component == Component::Http);
    gate.mark_fatal(Component::Network); // first publication wins, never reopens
    CHECK(gate.snapshot().component == Component::Http);
    CHECK(!gate.mark_ready() && !gate.mark_safe_mode());
    for (unsigned value = 0; value != 256; ++value) {
        const auto snapshot = tk::runtime_admission_unpack(static_cast<std::uint8_t>(value));
        CHECK(static_cast<unsigned>(snapshot.state) < 4);
        CHECK(snapshot.state == State::Fatal || snapshot.component == Component::Unknown);
        CHECK(static_cast<unsigned>(snapshot.component) <= 11);
        const unsigned code = (value & ~tk::kRuntimeIdentityReloadClaim) >> 2;
        CHECK(snapshot.component == (snapshot.state == State::Fatal && code <= 11
              ? static_cast<Component>(code) : Component::Unknown));
    }
    for (unsigned code = 0; code <= 11; ++code) {
        const auto encoded = tk::runtime_admission_pack(State::Fatal, static_cast<Component>(code));
        CHECK(tk::runtime_admission_unpack(encoded).component == static_cast<Component>(code));
        CHECK(tk::runtime_admission_unpack(encoded | tk::kRuntimeIdentityReloadClaim).component ==
              static_cast<Component>(code));
    }
    tk::RuntimeAdmissionGate boot_claim;
    CHECK(!boot_claim.try_claim_identity_reload());
    CHECK(boot_claim.mark_ready() && boot_claim.try_claim_identity_reload());
    CHECK(!boot_claim.try_claim_identity_reload() && boot_claim.vehicle_ready());
    boot_claim.mark_fatal(Component::Identity);
    CHECK(boot_claim.snapshot().component == Component::Identity && !boot_claim.vehicle_ready());
    CHECK(!boot_claim.try_claim_identity_reload() && !boot_claim.mark_ready());
    boot_claim.mark_fatal(Component::Network);
    CHECK(boot_claim.snapshot().component == Component::Identity);
    tk::RuntimeAdmissionGate fatal_first;
    CHECK(fatal_first.mark_ready()); fatal_first.mark_fatal(Component::Unknown);
    CHECK(!fatal_first.try_claim_identity_reload());
    fatal_first.mark_fatal(Component::Http);
    CHECK(fatal_first.snapshot().component == Component::Unknown);
    tk::RuntimeAdmissionGate safe_claim;
    CHECK(safe_claim.mark_safe_mode() && !safe_claim.try_claim_identity_reload());
    CHECK(tk::runtime_fatal_component(nullptr) == Component::Unknown);
    CHECK(tk::runtime_fatal_component("HTTP server") == Component::Http);
    CHECK(tk::runtime_fatal_component("NimBLE host") == Component::Bluetooth);
    CHECK(tk::runtime_fatal_component("future component") == Component::Unknown);
    CHECK(tk::identity_recovery_state(nullptr, State::Ready) == tk::IdentityRecoveryState::Ok);
    CHECK(tk::identity_recovery_state(nullptr, State::Fatal) == tk::IdentityRecoveryState::Unknown);
    CHECK(tk::identity_recovery_state("future reason", State::Ready) == tk::IdentityRecoveryState::Unknown);
    CHECK(std::string(tk::pairing_block_reason(false, false, false,
          tk::AutomaticKeyAction::AutomaticCommitReload)) == "key_reload_required");
    const tk::IdentityReloadAdmission admitted{true, true, true, false, false, false, true, true, true};
    CHECK(tk::identity_reload_allowed(admitted));
    for (unsigned fault = 0; fault != 9; ++fault) {
        auto input = admitted;
        switch (fault) {
            case 0: input.ready = false; break;
            case 1: input.automatic_commit = false; break;
            case 2: input.reload_required = false; break;
            case 3: input.vin_pending = true; break;
            case 4: input.cleanup_pending = true; break;
            case 5: input.recovered_boot = true; break;
            case 6: input.stable = false; break;
            case 7: input.journal_readable = false; break;
            case 8: input.journal_present = false; break;
        }
        tk::IdentityReloadOnce retry;
        CHECK(!tk::identity_reload_allowed(input));
        CHECK(!retry.reserve(input) && !retry.reserved());
        CHECK(retry.reserve(admitted) && retry.reserved());
        CHECK(!retry.reserve(admitted));
        // Fifteen minutes of unchanged supervisor input cannot re-arm a consumed restart.
        for (unsigned elapsed_ms = 0; elapsed_ms <= 900000; elapsed_ms += 30000)
            CHECK(!retry.reserve(admitted));
    }
}

// `ota_confirm_pending_image()` already owns HealthCommit before consulting this gate. Pin the
// complete post-owner admission matrix here; the runtime-boundary contract below binds the real
// irreversible mark-valid call to this exact Ready-only predicate.
static void test_ota_confirm_runtime_admission_matrix() {
    auto may_mark_valid_after_owner = [](const tk::RuntimeAdmissionGate& admission,
                                         bool health_commit_owner) {
        return health_commit_owner && admission.vehicle_ready();
    };

    tk::RuntimeAdmissionGate booting;
    CHECK(!may_mark_valid_after_owner(booting, true));

    tk::RuntimeAdmissionGate ready;
    CHECK(ready.mark_ready());
    CHECK(may_mark_valid_after_owner(ready, true));
    CHECK(!may_mark_valid_after_owner(ready, false));

    tk::RuntimeAdmissionGate safe_mode;
    CHECK(safe_mode.mark_safe_mode());
    CHECK(!may_mark_valid_after_owner(safe_mode, true));

    tk::RuntimeAdmissionGate fatal;
    fatal.mark_fatal();
    CHECK(!may_mark_valid_after_owner(fatal, true));

    // A late essential failure revokes even an admission that was Ready moments earlier.
    ready.mark_fatal();
    CHECK(!may_mark_valid_after_owner(ready, true));
}

// ─── NimBLE hidden-host-task start acknowledgement ──────────────────────────
static void test_nimble_start_gate() {
    using Action = tk::NimbleStartAction;
    using State = tk::NimbleStartState;

    tk::NimbleStartGate success;
    CHECK(success.state() == State::Idle);
    CHECK(success.action() == Action::Wait);
    CHECK(!success.acknowledge_sync());  // sync cannot pre-authorize start()
    CHECK(success.begin());
    CHECK(!success.begin());
    CHECK(success.state() == State::AwaitingSync);
    CHECK(success.action() == Action::Wait);
    CHECK(success.acknowledge_sync());
    CHECK(success.state() == State::Synced);
    CHECK(success.action() == Action::Ready);
    CHECK(!success.mark_timed_out());  // on_sync won the exact deadline race

    // The IDF wrapper hides its xTaskCreatePinnedToCore result. A create failure is therefore
    // observed as no on_sync acknowledgement: the bounded waiter times out and stays terminal.
    tk::NimbleStartGate hidden_create_failure;
    CHECK(hidden_create_failure.begin());
    CHECK(hidden_create_failure.action() == Action::Wait);
    CHECK(hidden_create_failure.mark_timed_out());
    CHECK(hidden_create_failure.state() == State::TimedOut);
    CHECK(hidden_create_failure.action() == Action::Fail);
    CHECK(!hidden_create_failure.acknowledge_sync());  // late sync cannot resurrect boot
    CHECK(hidden_create_failure.state() == State::TimedOut);
    CHECK(hidden_create_failure.action() == Action::Fail);
    CHECK(!hidden_create_failure.begin());

    // Explicitly pin the other ordering of the timeout/on_sync race.
    tk::NimbleStartGate timeout_wins;
    CHECK(timeout_wins.begin());
    CHECK(timeout_wins.mark_timed_out());
    CHECK(!timeout_wins.acknowledge_sync());
    CHECK(timeout_wins.action() == Action::Fail);
}

static void test_syslog_start_gate() {
    using Action = tk::SyslogStartAction;
    using State = tk::SyslogStartState;

    tk::SyslogStartGate success;
    CHECK(success.state() == State::Idle);
    CHECK(success.action() == Action::Wait);
    CHECK(success.begin());
    CHECK(!success.begin());
    CHECK(success.state() == State::Waiting);
    CHECK(success.action() == Action::Wait);  // an immediately scheduled task cannot consume
    CHECK(success.commit());
    CHECK(success.state() == State::Running);
    CHECK(success.action() == Action::Run);
    CHECK(!success.cancel());
    CHECK(!success.commit());

    tk::SyslogStartGate task_create_failure;
    CHECK(task_create_failure.begin());
    CHECK(task_create_failure.cancel());
    CHECK(task_create_failure.state() == State::Cancelled);
    CHECK(task_create_failure.action() == Action::Cancel);
    CHECK(!task_create_failure.commit());  // a late creator cannot resurrect deleted locals
    CHECK(!task_create_failure.begin());

    // Neither terminal action can be manufactured before startup owns resources.
    tk::SyslogStartGate idle;
    CHECK(!idle.commit());
    CHECK(!idle.cancel());
    CHECK(idle.action() == Action::Wait);
}

static void test_ble_host_health_ota_matrix() {
    auto explicit_commit_allowed = [](bool runtime_ready, bool host_synced,
                                      std::uint32_t host_reset_count, bool heap_healthy,
                                      bool health_commit_owner) {
        return health_commit_owner && runtime_ready && host_synced && host_reset_count == 0 &&
               heap_healthy;
    };

    bool host_synced = true;
    std::uint32_t host_reset_count = 0;
    CHECK(explicit_commit_allowed(true, host_synced, host_reset_count, true, true));

    // on_reset is dynamic degradation after a successful boot. Uptime, link and heap staying
    // healthy must not let either OTA path spend rollback again during this boot; on_sync restores
    // BLE operations, not the already-failed OTA stability proof.
    host_synced = false;
    host_reset_count = 1;
    CHECK(!explicit_commit_allowed(true, host_synced, host_reset_count, true, true));
    CHECK(tk::health_gate_decide(tk::kHealthGateBaseS, tk::kHealthGateBaseS,
                                 tk::kHealthGateCapS, true, true,
                                 true && host_synced && host_reset_count == 0) ==
          tk::HealthVerdict::Wait);

    // A quick re-sync restores BLE operations, but it cannot erase the sticky reset evidence
    // accumulated during this boot. Only a fresh boot with no reset may spend OTA rollback.
    host_synced = true;  // later on_sync after reset
    CHECK(!explicit_commit_allowed(true, host_synced, host_reset_count, true, true));
    CHECK(tk::health_gate_decide(tk::kHealthGateBaseS, tk::kHealthGateBaseS,
                                 tk::kHealthGateCapS, true, true,
                                 true && host_synced && host_reset_count == 0) ==
          tk::HealthVerdict::Wait);

    host_reset_count = 0;  // fresh stable boot
    CHECK(explicit_commit_allowed(true, true, host_reset_count, true, true));
    CHECK(tk::health_gate_decide(tk::kHealthGateBaseS, tk::kHealthGateBaseS,
                                 tk::kHealthGateCapS, true, true,
                                 true && host_reset_count == 0) == tk::HealthVerdict::Commit);

    CHECK(!explicit_commit_allowed(false, true, 0, true, true));
    CHECK(!explicit_commit_allowed(true, true, 0, false, true));
    CHECK(!explicit_commit_allowed(true, true, 0, true, false));
    CHECK(!explicit_commit_allowed(true, true, UINT32_MAX, true, true));
}

// ─── asynchronous ping generation / late callback ownership ─────────────────
static void test_ping_probe_generation() {
    tk::PingProbeGeneration probe;
    std::uint32_t replies = 99;

    const std::uint32_t first = probe.begin();
    CHECK(first != 0);
    CHECK(probe.begin() == 0);  // no overlapping session / callback-args reuse
    CHECK(!probe.complete(first + 1, 7));  // out-of-order callback is ignored
    CHECK(!probe.ended(first));
    CHECK(probe.complete(first, 2));
    CHECK(probe.ended(first));
    CHECK(probe.result(first, replies));
    CHECK(replies == 2);
    CHECK(probe.retire(first));

    const std::uint32_t second = probe.begin();
    CHECK(second != 0 && second != first);
    CHECK(!probe.complete(first, 9));  // late callback cannot complete/wake the new generation
    CHECK(!probe.ended(second));
    CHECK(probe.complete(second, 0));
    CHECK(probe.result(second, replies));
    CHECK(replies == 0);
    CHECK(probe.retire(second));

    const std::uint32_t invalid_measurement = probe.begin();
    CHECK(invalid_measurement != 0);
    CHECK(probe.complete(invalid_measurement, 0, false));
    bool measurement_valid = true;
    CHECK(probe.result(invalid_measurement, replies, measurement_valid));
    CHECK(!measurement_valid);
    CHECK(!probe.result(invalid_measurement, replies));
    CHECK(probe.retire(invalid_measurement));

    const std::uint32_t never_started = probe.begin();
    CHECK(never_started != 0);
    CHECK(probe.abandon_unstarted(never_started));
    CHECK(probe.active() == 0);
    CHECK(!probe.complete(never_started, 1));
}

// ─── MCP endpoint core (version negotiation, routing, tool registry, clamp) ──────
// Bounded /heap JSON stream: full history must remain fixed-chunk and fail closed.
struct HeapStreamCapture {
    std::vector<std::string> chunks;
    int calls{0};
    int fail_at{-1};

    bool send(const char* data, std::size_t size) {
        const int call = calls++;
        if (call == fail_at) return false;
        CHECK(data != nullptr);
        CHECK(size != 0);
        CHECK(size <= 192);
        chunks.emplace_back(data, size);
        return true;
    }

    std::string joined() const {
        std::string result;
        for (const std::string& chunk : chunks) result += chunk;
        return result;
    }
};

static bool capture_heap_stream(const tk::HeapJsonStreamView& view,
                                HeapStreamCapture& capture) {
    return tk::stream_heap_json(view, [&capture](const char* data, std::size_t size) {
        return capture.send(data, size);
    });
}

static void test_heap_json_stream() {
    constexpr std::int16_t kAbsent = INT16_MIN;

    HeapStreamCapture empty;
    CHECK(capture_heap_stream({300, 4, 7, nullptr, nullptr, 0, kAbsent}, empty));
    CHECK(empty.joined() ==
          "{\"dt\":300,\"b0\":4,\"b_boot\":7,\"unit\":\"KiB\",\"scale\":10,"
          "\"free\":[],\"largest\":[]}");

    // A non-empty view without either caller-owned series fails before the first send.
    const std::int16_t one[] = {1};
    HeapStreamCapture missing_free;
    CHECK(!capture_heap_stream({1, 2, 3, nullptr, one, 1, kAbsent}, missing_free));
    CHECK(missing_free.calls == 0);
    HeapStreamCapture missing_largest;
    CHECK(!capture_heap_stream({1, 2, 3, one, nullptr, 1, kAbsent}, missing_largest));
    CHECK(missing_largest.calls == 0);

    // Pin null/value edges and the complete int16_t numeric range that remains after reserving
    // INT16_MIN as the absent sentinel.
    const std::int16_t free_edges[] = {kAbsent, -32767, 0, 32767};
    const std::int16_t largest_edges[] = {32767, 0, -32767, kAbsent};
    HeapStreamCapture edges;
    CHECK(capture_heap_stream(
        {UINT32_MAX, UINT32_MAX, 0, free_edges, largest_edges, 4, kAbsent}, edges));
    CHECK(edges.joined() ==
          "{\"dt\":4294967295,\"b0\":4294967295,\"b_boot\":0,\"unit\":\"KiB\","
          "\"scale\":10,\"free\":[null,-32767,0,32767],"
          "\"largest\":[32767,0,-32767,null]}");

    // Full production occupancy: 288 buckets in each series, with every transport call bounded
    // by the writer's fixed 192-byte buffer.
    constexpr std::size_t kFullCount = 288;
    std::array<std::int16_t, kFullCount> free_samples{};
    std::array<std::int16_t, kFullCount> largest_samples{};
    for (std::size_t i = 0; i < kFullCount; ++i) {
        free_samples[i] = i % 17 == 0 ? kAbsent : static_cast<std::int16_t>(i);
        largest_samples[i] =
            i % 19 == 0 ? kAbsent : static_cast<std::int16_t>(30000 - i);
    }
    HeapStreamCapture full;
    const tk::HeapJsonStreamView full_view{
        300, 123456, 120000, free_samples.data(), largest_samples.data(), kFullCount, kAbsent,
    };
    CHECK(capture_heap_stream(full_view, full));
    CHECK(full.chunks.size() > 2);

    std::string expected =
        "{\"dt\":300,\"b0\":123456,\"b_boot\":120000,\"unit\":\"KiB\","
        "\"scale\":10,\"free\":[";
    for (std::size_t i = 0; i < kFullCount; ++i) {
        if (i != 0) expected += ',';
        expected += free_samples[i] == kAbsent ? "null" : std::to_string(free_samples[i]);
    }
    expected += "],\"largest\":[";
    for (std::size_t i = 0; i < kFullCount; ++i) {
        if (i != 0) expected += ',';
        expected += largest_samples[i] == kAbsent ? "null" : std::to_string(largest_samples[i]);
    }
    expected += "]}";
    CHECK(full.joined() == expected);

    // A failed first or later send aborts immediately and cannot fabricate completed JSON.
    HeapStreamCapture first_send_failure;
    first_send_failure.fail_at = 0;
    CHECK(!capture_heap_stream(full_view, first_send_failure));
    CHECK(first_send_failure.calls == 1);
    CHECK(first_send_failure.chunks.empty());

    HeapStreamCapture nth_send_failure;
    nth_send_failure.fail_at = 2;
    CHECK(!capture_heap_stream(full_view, nth_send_failure));
    CHECK(nth_send_failure.calls == 3);
    CHECK(nth_send_failure.chunks.size() == 2);
}

// ─── MCP endpoint core (version negotiation, routing, tool registry, validation) ──────
static void test_mcp() {
    using MM = tk::McpMethod;

    // Version negotiation: echo a supported revision; anything else (or absent) answers
    // with our latest supported version, per the MCP lifecycle spec.
    CHECK_STR(tk::mcp_negotiate_version("2025-06-18"), "2025-06-18");
    CHECK_STR(tk::mcp_negotiate_version("2025-03-26"), "2025-03-26");
    CHECK_STR(tk::mcp_negotiate_version("2024-11-05"), "2025-06-18");
    CHECK_STR(tk::mcp_negotiate_version("bogus"),      "2025-06-18");
    CHECK_STR(tk::mcp_negotiate_version(nullptr),      "2025-06-18");

    // JSON-RPC method routing.
    CHECK(tk::mcp_method_from("initialize") == MM::Initialize);
    CHECK(tk::mcp_method_from("tools/list") == MM::ToolsList);
    CHECK(tk::mcp_method_from("tools/call") == MM::ToolsCall);
    CHECK(tk::mcp_method_from("ping")       == MM::Ping);
    // Any notifications/* is a notification (202, no body) — never "method not found".
    CHECK(tk::mcp_method_from("notifications/initialized") == MM::Notification);
    CHECK(tk::mcp_method_from("notifications/cancelled")   == MM::Notification);
    // Unimplemented capabilities and garbage stay Unknown (→ -32601).
    CHECK(tk::mcp_method_from("resources/list") == MM::Unknown);
    CHECK(tk::mcp_method_from("Initialize")     == MM::Unknown);  // case-sensitive
    CHECK(tk::mcp_method_from(nullptr)          == MM::Unknown);

    // Tool registry — now the MCP-visible half of the ONE shared command table
    // (logic/command_registry.hpp): every MCP row round-trips by name and by kind;
    // role-refused commands (which the Charging-Manager key can't execute) carry no
    // mcp_name and must not resolve as tools.
    for (const auto& t : tk::kCommands) {
        if (!t.mcp_name) continue;
        CHECK(tk::cmd_from_mcp_name(t.mcp_name) == &t);
        CHECK(t.mcp_desc != nullptr);
        CHECK(tk::cmd_info(t.kind) == &t);
    }
    CHECK(tk::cmd_from_mcp_name("door_unlock")     == nullptr);  // REST-only, never a tool
    CHECK(tk::cmd_from_mcp_name("set_sentry_mode") == nullptr);
    CHECK(tk::cmd_from_mcp_name(nullptr)           == nullptr);
    CHECK(tk::cmd_info(tk::CmdKind::Unknown)       == nullptr);

    // Arg-spec table — the single source of truth the advertised schema, the MCP
    // executor validation AND the REST /command validation all read (drift between them is
    // impossible by construction; this pins the values themselves). Every non-None arg
    // has sane shared bounds; an absent OPTIONAL Int defaults to 0 on the MCP side and
    // to api_default on the REST side, so both must lie inside the advertised bounds.
    for (const auto& t : tk::kCommands) {
        for (const auto& a : t.args) {
            if (a.type == tk::CmdArgType::None) continue;
            CHECK(a.api_key != nullptr || a.mcp_key != nullptr);
            if (a.type == tk::CmdArgType::Int) {
                CHECK(a.lo <= a.hi);
                if (a.mcp_key && !a.mcp_required) CHECK(a.lo <= 0 && 0 <= a.hi);
                if (a.api_key) CHECK(a.lo <= a.api_default && a.api_default <= a.hi);
            }
        }
    }
    const tk::CmdInfo* amps = tk::cmd_info(tk::CmdKind::SetChargingAmps);
    CHECK_STR(amps->args[0].mcp_key, "amps");
    CHECK_STR(amps->args[0].api_key, "charging_amps");   // TeslaBleHttpProxy compat name
    CHECK(amps->args[0].type == tk::CmdArgType::Int);
    CHECK(amps->args[0].api_required && amps->args[0].mcp_required);
    CHECK(amps->args[0].lo == 0 && amps->args[0].hi == 48);
    CHECK(amps->args[0].api_default == 0);
    const tk::CmdInfo* lim = tk::cmd_info(tk::CmdKind::SetChargeLimit);
    CHECK_STR(lim->args[0].mcp_key, "percent");
    CHECK_STR(lim->args[0].api_key, "percent");
    CHECK(!lim->args[0].api_required && lim->args[0].mcp_required);
    CHECK(lim->args[0].lo == 50 && lim->args[0].hi == 100);
    CHECK(lim->args[0].api_default == 80);               // REST: absent body still means 80%
    const tk::CmdInfo* sched = tk::cmd_info(tk::CmdKind::SetScheduledCharging);
    CHECK_STR(sched->args[0].mcp_key, "enable");
    CHECK_STR(sched->args[0].api_key, "enable");
    CHECK(sched->args[0].type == tk::CmdArgType::Bool && sched->args[0].mcp_required);
    CHECK(sched->args[0].api_required);
    CHECK_STR(sched->args[1].mcp_key, "start_minutes");
    CHECK(sched->args[1].type == tk::CmdArgType::Int && !sched->args[1].mcp_required);
    CHECK(sched->args[1].lo == 0 && sched->args[1].hi == 1439);
    // Read-only + no-arg tools carry no args.
    CHECK(tk::cmd_info(tk::CmdKind::GetVehicleState)->args[0].type == tk::CmdArgType::None);
    CHECK(tk::cmd_info(tk::CmdKind::WakeUp)->args[0].type          == tk::CmdArgType::None);

    // REST never reports success for a value different from the one supplied. Optional fields
    // may still use their registry default when absent, but a present value is strict.
    int strict = -1;
    CHECK(tk::strict_rest_int(50.0, lim->args[0], strict) && strict == 50);
    CHECK(tk::strict_rest_int(100.0, lim->args[0], strict) && strict == 100);
    CHECK(!tk::strict_rest_int(49.0, lim->args[0], strict));
    CHECK(!tk::strict_rest_int(101.0, lim->args[0], strict));
    CHECK(!tk::strict_rest_int(80.5, lim->args[0], strict));
    CHECK(!tk::strict_rest_int(std::nan(""), lim->args[0], strict));

    // REST surface of the shared table: exactly the 15 TeslaBleHttpProxy commands
    // resolve by API name, each to its own kind; get_vehicle_state is NOT one of them.
    const char* api_names[] = {
        "wake_up", "charge_start", "charge_stop", "charge_port_door_open",
        "charge_port_door_close", "door_lock", "door_unlock", "flash_lights",
        "honk_horn", "auto_conditioning_start", "auto_conditioning_stop",
        "set_charging_amps", "set_charge_limit", "set_sentry_mode",
        "set_scheduled_charging",
    };
    int api_count = 0;
    for (const auto& t : tk::kCommands) if (t.api_name) ++api_count;
    CHECK(api_count == 15);
    for (const char* n : api_names) {
        const tk::CmdInfo* c = tk::cmd_from_api_name(n);
        CHECK(c != nullptr);
        CHECK_STR(c->api_name, n);
    }
    CHECK(tk::cmd_from_api_name("get_vehicle_state") == nullptr);
    CHECK(tk::cmd_from_api_name("bogus")             == nullptr);
    CHECK(tk::cmd_from_api_name(nullptr)             == nullptr);
    // MCP name mapping of the charge-port pair differs from the REST name on purpose.
    CHECK_STR(tk::cmd_from_api_name("charge_port_door_open")->mcp_name, "charge_port_open");

    // REST body compatibility. Empty bodies and objects retain the normal
    // TeslaBleHttpProxy contract for every command. evcc's generic boolean setter emits
    // the scalar true for charge_start and wake_up and false for charge_stop; admit only
    // those exact command/value pairs, never a mismatched toggle or a scalar for another
    // command (an argument-bearing command would skip its object-key validation).
    using BS = tk::RestBodyShape;
    CHECK(tk::rest_body_allowed(tk::CmdKind::ChargeStart, BS::Empty));
    CHECK(tk::rest_body_allowed(tk::CmdKind::ChargeStart, BS::Object));
    CHECK(tk::rest_body_allowed(tk::CmdKind::SetChargingAmps, BS::Object));
    CHECK(tk::rest_body_allowed(tk::CmdKind::ChargeStart, BS::BoolTrue));
    CHECK(!tk::rest_body_allowed(tk::CmdKind::ChargeStart, BS::BoolFalse));
    CHECK(tk::rest_body_allowed(tk::CmdKind::ChargeStop, BS::BoolFalse));
    CHECK(!tk::rest_body_allowed(tk::CmdKind::ChargeStop, BS::BoolTrue));
    CHECK(tk::rest_body_allowed(tk::CmdKind::WakeUp, BS::Empty));
    CHECK(tk::rest_body_allowed(tk::CmdKind::WakeUp, BS::BoolTrue));
    CHECK(!tk::rest_body_allowed(tk::CmdKind::WakeUp, BS::BoolFalse));
    CHECK(!tk::rest_body_allowed(tk::CmdKind::WakeUp, BS::Other));
    CHECK(!tk::rest_body_allowed(tk::CmdKind::SetChargingAmps, BS::BoolFalse));
    CHECK(!tk::rest_body_allowed(tk::CmdKind::SetChargingAmps, BS::BoolTrue));
    CHECK(!tk::rest_body_allowed(tk::CmdKind::SetChargeLimit, BS::BoolTrue));
    CHECK(!tk::rest_body_allowed(tk::CmdKind::SetScheduledCharging, BS::BoolTrue));
    CHECK(!tk::rest_body_allowed(tk::CmdKind::SetScheduledCharging, BS::BoolFalse));
    CHECK(!tk::rest_body_allowed(tk::CmdKind::ChargeStart, BS::Other));
    // Every command that takes a REST argument, present or future, stays object-only.
    for (const auto& t : tk::kCommands) {
        bool has_rest_arg = false;
        for (const auto& a : t.args) has_rest_arg = has_rest_arg || a.api_key != nullptr;
        if (!has_rest_arg) continue;
        CHECK(tk::rest_body_allowed(t.kind, BS::Object));
        CHECK(!tk::rest_body_allowed(t.kind, BS::BoolTrue));
        CHECK(!tk::rest_body_allowed(t.kind, BS::BoolFalse));
        CHECK(!tk::rest_body_allowed(t.kind, BS::Other));
    }

    // tools/list wire order is table order — pin the 9 MCP rows exactly (a reorder
    // would change the serialized tools/list even with identical content).
    const char* mcp_order[] = {
        "get_vehicle_state", "wake_up", "charge_start", "charge_stop",
        "charge_port_open", "charge_port_close", "set_charging_amps",
        "set_charge_limit", "set_scheduled_charging",
    };
    int mi = 0;
    for (const auto& t : tk::kCommands) {
        if (!t.mcp_name) continue;
        CHECK(mi < 9);
        if (mi < 9) CHECK_STR(t.mcp_name, mcp_order[mi]);
        ++mi;
    }
    CHECK(mi == 9);

    // MCP's advertised integer/boolean schema is enforced after permissive JSON/string
    // decoding: out-of-range, fractional and every non-finite spelling fails;
    // numeric booleans are exactly 0/1 rather than C-style "any non-zero".
    int parsed_int = -1;
    CHECK(tk::mcp_integer_value(16.0, 0, 48, parsed_int) && parsed_int == 16);
    CHECK(!tk::mcp_integer_value(-1.0, 0, 48, parsed_int));
    CHECK(!tk::mcp_integer_value(49.0, 0, 48, parsed_int));
    CHECK(!tk::mcp_integer_value(1e300, 0, 48, parsed_int));
    CHECK(!tk::mcp_integer_value(16.5, 0, 48, parsed_int));
    CHECK(!tk::mcp_integer_value(std::nan(""), 0, 48, parsed_int));
    CHECK(!tk::mcp_integer_value(HUGE_VAL, 0, 48, parsed_int));
    bool parsed_bool = false;
    CHECK(tk::mcp_bool_value(0.0, parsed_bool) && !parsed_bool);
    CHECK(tk::mcp_bool_value(1.0, parsed_bool) && parsed_bool);
    CHECK(!tk::mcp_bool_value(-1.0, parsed_bool));
    CHECK(!tk::mcp_bool_value(2.0, parsed_bool));
    CHECK(!tk::mcp_bool_value(std::nan(""), parsed_bool));
    CHECK(!tk::mcp_bool_value(HUGE_VAL, parsed_bool));

    // Command outcome text — shared by the REST /command reason and the MCP tools/call
    // result, so both paths report identical outcomes.
    const std::string tesla_reason = "complete";
    const std::string no_reason;
    CHECK_STR(tk::command_result_text(true,  no_reason),    "command executed successfully");
    CHECK_STR(tk::command_result_text(true,  tesla_reason), "command executed successfully");
    CHECK_STR(tk::command_result_text(false, tesla_reason), "complete");
    CHECK_STR(tk::command_result_text(false, no_reason),    "vehicle not reachable");

    // Nominal error: already_set is classified as success for idempotent commands
    CHECK(tk::is_nominal_already_set("already_set"));
    CHECK(!tk::is_nominal_already_set("complete"));
    CHECK(!tk::is_nominal_already_set(""));
    CHECK(!tk::is_nominal_already_set("not_already_set"));
    CHECK(!tk::is_nominal_already_set("already_set_suffix"));
    CHECK(!tk::is_nominal_already_set("foo_already_set_bar"));
    CHECK_STR(tk::command_result_text(false, "already_set"), "command executed successfully");

    // Charging no-ops are an explicit request-context policy, never a global reason rule.
    using OutcomePolicy = tk::CommandOutcomePolicy;
    struct NoopCase { const char* reason; bool start; bool stop; };
    const NoopCase noops[] = {
        {"is_charging",true,false}, {"complete",true,false}, {"not_charging",false,true},
        {"Infotainment action failed: is_charging",true,false},
        {"Infotainment action failed: complete",true,false},
        {"Infotainment action failed: not_charging",false,true},
        {"already_set",false,false}, {"requested",false,false}, {"",false,false},
        {"complete_suffix",false,false}, {"prefix_complete",false,false},
        {"COMPLETE",false,false}, {"is_Charging",false,false},
        {" not_charging",false,false}, {"not_charging ",false,false},
        {"Infotainment action failed:complete",false,false},
        {"Infotainment action failed:  complete",false,false},
        {"unknown: Infotainment action failed: complete",false,false},
        {"command outcome unknown; command may have executed; complete",false,false},
    };
    for (const auto& sample : noops) {
        CHECK(tk::is_nominal_charge_outcome(OutcomePolicy::ChargeStart,sample.reason)==sample.start);
        CHECK(tk::is_nominal_charge_outcome(OutcomePolicy::ChargeStop,sample.reason)==sample.stop);
        CHECK(!tk::is_nominal_charge_outcome(OutcomePolicy::AlreadySetOnly,sample.reason));
        CHECK(!tk::is_nominal_charge_outcome(static_cast<OutcomePolicy>(255),sample.reason));
    }
    CHECK(!tk::is_nominal_charge_outcome(OutcomePolicy::ChargeStart,std::string_view{}));
    constexpr char exact_complete[] = {'c','o','m','p','l','e','t','e'};
    CHECK(tk::is_nominal_charge_outcome(OutcomePolicy::ChargeStart,
        std::string_view(exact_complete,sizeof(exact_complete))));
    constexpr char embedded_nul[] = "complete\0extra";
    CHECK(!tk::is_nominal_charge_outcome(OutcomePolicy::ChargeStart,
        std::string_view(embedded_nul,sizeof(embedded_nul)-1)));
    CHECK(!tk::is_nominal_charge_outcome(OutcomePolicy::ChargeStart,"Infotainment action failed: "));

    // Soft-desync link backstop: which failures prove contact, which are local, which count
    // toward the drop-and-resync streak (tk::classify_command_failure, make_result_cb_).
    using FailureOrigin = tk::CommandFailureOrigin;
    CHECK(tk::classify_command_failure("vehicle asleep") == FailureOrigin::LocalPolicy);
    CHECK(tk::classify_command_failure("signed message authentication failed") == FailureOrigin::VehicleResponse);
    CHECK(tk::classify_command_failure("VCSEC authentication failed") == FailureOrigin::VehicleResponse);
    CHECK(tk::classify_command_failure("authentication failed") == FailureOrigin::VehicleResponse);
    CHECK(tk::classify_command_failure("key not on whitelist - pairing required") == FailureOrigin::VehicleResponse);
    CHECK(tk::classify_command_failure("command rejected by vehicle") == FailureOrigin::VehicleResponse);
    CHECK(tk::classify_command_failure("Infotainment action failed: could_not_reach_service") ==
          FailureOrigin::VehicleResponse);
    CHECK(tk::classify_command_failure("Infotainment action failed") == FailureOrigin::VehicleResponse);
    CHECK(tk::classify_command_failure("action failed: charging_port_closed") == FailureOrigin::VehicleResponse);
    CHECK(tk::classify_command_failure("failed with error status") == FailureOrigin::VehicleResponse);
    CHECK(tk::classify_command_failure("VCSEC command failed with error status") == FailureOrigin::VehicleResponse);
    CHECK(tk::classify_command_failure("VCSEC command failed") == FailureOrigin::VehicleResponse);
    CHECK(tk::classify_command_failure("command response timeout; max retries exceeded") ==
          FailureOrigin::TransportOrTimeout);
    CHECK(tk::classify_command_failure("authentication / wake timeout; max retries exceeded") ==
          FailureOrigin::TransportOrTimeout);
    CHECK(tk::classify_command_failure("Payload build failed") == FailureOrigin::TransportOrTimeout);
    CHECK(tk::classify_command_failure("connection lost") == FailureOrigin::TransportOrTimeout);
    CHECK(tk::classify_command_failure("") == FailureOrigin::TransportOrTimeout);

    // Protocol Invariant Gate: command table integrity & role boundaries.
    // Every registered command must have:
    // 1. Non-empty description if MCP tool is exposed.
    // 2. Strict type assignment for every argument.
    // 3. Role-denied commands (DoorLock..ClimateStop) MUST NOT be exposed as MCP tools
    //    (mcp_name must be nullptr, protecting models from sending commands the car refuses).
    for (const auto& cmd : tk::kCommands) {
        if (cmd.mcp_name) {
            CHECK(cmd.mcp_desc != nullptr && std::strlen(cmd.mcp_desc) > 0);
            CHECK(cmd.kind != tk::CmdKind::DoorLock &&
                  cmd.kind != tk::CmdKind::DoorUnlock &&
                  cmd.kind != tk::CmdKind::FlashLights &&
                  cmd.kind != tk::CmdKind::HonkHorn &&
                  cmd.kind != tk::CmdKind::SetSentryMode &&
                  cmd.kind != tk::CmdKind::ClimateStart &&
                  cmd.kind != tk::CmdKind::ClimateStop);
        }
        for (int a = 0; a < tk::kCmdMaxArgs; ++a) {
            const auto& arg = cmd.args[a];
            if (arg.type == tk::CmdArgType::None) {
                CHECK(arg.api_key == nullptr && arg.mcp_key == nullptr);
            } else {
                CHECK(arg.api_key != nullptr || arg.mcp_key != nullptr);
                if (arg.type == tk::CmdArgType::Int) {
                    CHECK(arg.lo <= arg.hi);
                }
            }
        }
    }

}

// ─── Charging-current ACK/readback + active-cache freshness ───────────────────
static void test_charge_control() {
    using tk::ChargingAmpsReadback;

    // A valid cached reading remains usable while the car is idle, even without a
    // freshness timestamp: read-only evcc polling must not wake a sleeping vehicle.
    CHECK(tk::charge_cache_usable(true, false, false, 0));
    CHECK(tk::charge_cache_usable(true, false, true, 86400));

    // During a command/charging window the 10-second background poll must have produced
    // a recent ChargeState. The exact 30-second boundary is accepted; 31 seconds is stale.
    CHECK(!tk::charge_cache_usable(false, true, true, 0));
    CHECK(!tk::charge_cache_usable(true, true, false, 0));
    CHECK(tk::charge_cache_usable(true, true, true, 0));
    CHECK(tk::charge_cache_usable(true, true, true, tk::kActiveChargeStateMaxAgeS));
    CHECK(!tk::charge_cache_usable(true, true, true, tk::kActiveChargeStateMaxAgeS + 1));

    // Tesla's actionStatus=OK is not sufficient. Only a fresh ChargeState with the
    // requested effective charging_amps value verifies the command.
    CHECK(tk::verify_charging_amps(8, true, true, 8) == ChargingAmpsReadback::Verified);
    CHECK(tk::verify_charging_amps(8, true, true, 16) == ChargingAmpsReadback::Mismatch);
    CHECK(tk::verify_charging_amps(8, true, false, 0) == ChargingAmpsReadback::Missing);
    CHECK(tk::verify_charging_amps(8, false, true, 8) == ChargingAmpsReadback::Missing);
}

// ─── /status field contract (logic/status_model.hpp <- http_status.cpp) ──────────
// Flattening emitter for the goldens: containers print as "path{" / "path[", leaves as
// "path=value" — one line per emitter call, so the golden pins field ORDER, key NAMES,
// PRESENCE rules and VALUE shaping all at once (that is the /status contract app.js
// and any LAN script consume).
struct CollectEmitter {
    std::string out;
    std::vector<std::string> path;   // open containers
    std::vector<int> arr_next;       // per-container: next array index, or -1 for objects

    std::string joined(const std::string& leaf) {
        std::string p;
        for (const auto& s : path) { p += s; p += '.'; }
        return p + leaf;
    }
    std::string elem_name(const char* key) {
        if (key) return key;
        return std::to_string(arr_next.back()++);   // array element: enclosing counter
    }
    void obj_begin(const char* key) {
        std::string nm = elem_name(key);
        out += joined(nm) + "{\n";
        path.push_back(nm); arr_next.push_back(-1);
    }
    void obj_end() { path.pop_back(); arr_next.pop_back(); }
    void arr_begin(const char* key) {
        std::string nm = elem_name(key);
        out += joined(nm) + "[\n";
        path.push_back(nm); arr_next.push_back(0);
    }
    void arr_end() { obj_end(); }
    void str(const char* k, const char* v) { out += joined(elem_name(k)) + "=\"" + v + "\"\n"; }
    void num(const char* k, double v) {
        char b[40];
        if (v == (double)(long long)v) std::snprintf(b, sizeof b, "%lld", (long long)v);
        else                           std::snprintf(b, sizeof b, "%.10g", v);
        out += joined(elem_name(k)) + "=" + b + "\n";
    }
    void null(const char* k) { out += joined(elem_name(k)) + "=null\n"; }
    void boolean(const char* k, bool v) { out += joined(elem_name(k)) + (v ? "=true\n" : "=false\n"); }
};

// Golden compare that prints the diff on mismatch (a bare CHECK only names the line).
static void test_runtime_status_visibility() {
    for(auto state : {tk::RuntimeAdmissionState::Booting, tk::RuntimeAdmissionState::Ready,
                     tk::RuntimeAdmissionState::SafeMode, tk::RuntimeAdmissionState::Fatal}) {
        tk::status::Inputs in;
        in.runtime_state = state;
        in.fatal_component = tk::RuntimeFatalComponent::Http;
        in.pairing_block_reason = "key_reload_required";
        in.diag_dropped_lines = 17;
        in.syslog_dropped_lines = 23;
        CollectEmitter emitter;
        tk::status::emit_status(in, emitter);
        CHECK(emitter.out.find("sys.diag_dropped_lines=17\n") != std::string::npos);
        CHECK(emitter.out.find("syslog.dropped_lines=23\n") != std::string::npos);
        CHECK(emitter.out.find(std::string("sys.runtime_state=\"") + tk::runtime_admission_state_name(state) + "\"\n") != std::string::npos);
        CHECK(emitter.out.find("pairing_blocked=\"key_reload_required\"\n") != std::string::npos);
        CHECK(emitter.out.find("identity_state=\"reboot_required\"\n") != std::string::npos);
        CHECK((emitter.out.find("sys.fatal_component=\"http\"\n") != std::string::npos) ==
              (state == tk::RuntimeAdmissionState::Fatal));
    }
}

static bool golden_eq(const std::string& got, const char* want) {
    if (got == want) return true;
    std::printf("---- golden mismatch: got ----\n%s---- want ----\n%s----\n", got.c_str(), want);
    return false;
}

static void test_status_model() {
    using tk::status::Inputs;

    // Unrelated contact stays fresh while ChargeState independently ages or is absent.
    // Zero charge values are present values; age zero also represents a real tick-zero sample.
    for (auto link : {tk::LinkState::Unknown, tk::LinkState::Awake, tk::LinkState::Asleep,
                      tk::LinkState::Idle, tk::LinkState::Unreachable}) {
        for (bool valid : {false, true}) for (bool have_age : {false, true}) {
            for (uint32_t age : {0u, 29u, 30u, 31u, 86400u, UINT32_MAX}) {
                Inputs sample;
                sample.link = link;
                sample.charge.valid = valid;
                sample.charge.has_battery_level = true;
                sample.charge.battery_level = 0;
                sample.charge.charging_state = "Charging";
                sample.charge.has_charger_power = true;
                sample.charge.charger_power = 0;
                sample.have_charge_age = have_age;
                sample.charge_age_s = age;
                sample.have_last_seen = true;
                sample.last_seen_s = 0;
                CollectEmitter actual;
                tk::status::emit_status(sample, actual);
                const bool live = link == tk::LinkState::Awake && valid && have_age && age <= 30;
                CHECK((actual.out.find("vehicle{\n") != std::string::npos) == live);
                CHECK((actual.out.find("vehicle.soc=0\n") != std::string::npos) == live);
                CHECK((actual.out.find("vehicle.power=0\n") != std::string::npos) == live);
                CHECK((actual.out.find("last.soc=0\n") != std::string::npos) == valid);
                CHECK((actual.out.find("charge_age_s=") != std::string::npos) == (valid && have_age));
                CHECK(actual.out.find("last_seen_s=0\n") != std::string::npos);
            }
        }
    }

    // Scenario 1 — awake + charging, BLE link up, full telemetry with a deliberate mix
    // of present/absent fields (float-exact values so the golden is bit-stable).
    Inputs in;
    in.vin = "5YJ3E1EA7KF000316"; in.ip = "192.168.1.50"; in.version = "1.4.2";
    in.board_mac = "02:00:00:32:55:20";
    in.stack_context_samples[0] = tk::stack_sample::merge(in.stack_context_samples[0], 0, 4096);
    in.stack_context_samples[0] = tk::stack_sample::merge(in.stack_context_samples[0], 1, 3072);
    in.stack_context_samples[0] = tk::stack_sample::merge(in.stack_context_samples[0], 2, 2048);
    in.stack_context_samples[0] = tk::stack_sample::merge(in.stack_context_samples[0], 3, 1024);
    in.key_present = true; in.key_fingerprint = "AB:CD:EF:01";
    in.key_created = 1750000000; in.paired = true; in.paired_at = 1750500000;
    in.wifi_connected = true; in.wifi_ssid = "HomeNet"; in.wifi_rssi = -55; in.wifi_std = "Wi-Fi 6";
    in.mqtt_configured = true; in.mqtt_connected = true; in.mqtt_tls = true;
    in.mqtt_broker = "mqtt.local:8883";
    in.syslog_configured = true; in.syslog_resolved = true; in.syslog_reachable = true;
    in.syslog_host = "syslog.lan"; in.syslog_port = 514;   // error absent → omitted
    in.ble_connected = true; in.have_ble_rssi = true; in.ble_rssi = -60;
    in.ble_addr = "aa:bb:cc:dd:ee:ff";
    in.climate.valid = true;
    in.climate.has_inside = true;   in.climate.inside_temp = 21.5f;
    in.climate.has_outside = true;  in.climate.outside_temp = 18.0f;
    in.climate.has_setpoint = true; in.climate.driver_setpoint = 20.5f;
    in.climate.has_climate_on = true;      in.climate.is_climate_on = true;
    in.climate.has_preconditioning = true; in.climate.is_preconditioning = false;
    in.climate.has_cop = true;         in.climate.cop = "On";
    in.climate.has_cop_cooling = true; in.climate.cop_cooling = true;
    in.climate.has_cop_temp = true;    in.climate.cop_temp = "Medium";
    // cop_reason absent on purpose (presence rule)
    in.climate.has_rear_defrost = true; in.climate.rear_defrost = false;
    in.climate.has_defrost_mode = true; in.climate.defrost_mode = "Off";
    in.drive.valid = true; in.drive.shift_state = "P";
    in.drive.has_odometer = true; in.drive.odometer_km = 12345.5f;
    in.tires.valid = true;
    in.tires.has_fl = true; in.tires.fl = 2.75f;
    in.tires.has_fr = true; in.tires.fr = 2.75f;
    in.tires.has_rl = true; in.tires.rl = 3.0f;
    // rr absent on purpose; warn always emitted
    in.closures.valid = true;
    in.closures.has_locked = true; in.closures.locked = true;
    in.closures.trunk_open = true;   // door/frunk/window false, always emitted
    // user absent on purpose
    in.link = tk::LinkState::Awake; in.vcsec_sleep = "AWAKE";
    in.charge.valid = true;
    in.charge.has_battery_level = true;    in.charge.battery_level = 72.5f;
    in.charge.charging_state = "Charging";
    in.charge.has_charge_limit_soc = true; in.charge.charge_limit_soc = 80.0f;
    in.charge.has_charger_power = true;    in.charge.charger_power = 11.4f;  // → rounds to 11
    in.charge.has_charging_amps = true;    in.charge.charging_amps = 16;
    in.charge.has_actual_current = true;   in.charge.charger_actual_current = 16;
    in.charge.has_voltage = true;          in.charge.charger_voltage = 231;
    in.charge.has_charger_phases = true;   in.charge.charger_phases = 2;
    in.have_last_seen = true; in.last_seen_s = 5;
    in.have_charge_age = true; in.charge_age_s = 5;

    CollectEmitter e1;
    tk::status::emit_status(in, e1);
    CHECK(golden_eq(e1.out, 
        "vin=\"5YJ3E1EA7KF000316\"\n"
        "ip=\"192.168.1.50\"\n"
        "version=\"1.4.2\"\n"
        "key_present=true\n"
        "key_fingerprint=\"AB:CD:EF:01\"\n"
        "key_created=1750000000\n"
        "paired=true\n"
        "session_available=false\n"
        "sessions{\n"
        "sessions.vcsec{\n"
        "sessions.vcsec.runtime_known=false\n"
        "sessions.vcsec.runtime_usable=false\n"
        "sessions.vcsec.persistence=\"unknown\"\n"
        "sessions.infotainment{\n"
        "sessions.infotainment.runtime_known=false\n"
        "sessions.infotainment.runtime_usable=false\n"
        "sessions.infotainment.persistence=\"unknown\"\n"
        "paired_at=1750500000\n"
        "reauth=false\n"
        "wifi{\n"
        "wifi.ssid=\"HomeNet\"\n"
        "wifi.rssi=-55\n"
        "wifi.std=\"Wi-Fi 6\"\n"
        "mqtt{\n"
        "mqtt.configured=true\n"
        "mqtt.connected=true\n"
        "mqtt.tls=true\n"
        "mqtt.broker=\"mqtt.local:8883\"\n"
        "syslog{\n"
        "syslog.configured=true\n"
        "syslog.resolved=true\n"
        "syslog.reachable=true\n"
        "syslog.host=\"syslog.lan\"\n"
        "syslog.port=514\n"
        "tele{\n"
        "tele.climate{\n"
        "tele.climate.inside=21.5\n"
        "tele.climate.outside=18\n"
        "tele.climate.setpoint=20.5\n"
        "tele.climate.on=true\n"
        "tele.climate.preconditioning=false\n"
        "tele.climate.cop=\"On\"\n"
        "tele.climate.cop_cooling=true\n"
        "tele.climate.cop_temp=\"Medium\"\n"
        "tele.climate.rear_defrost=false\n"
        "tele.climate.defrost_mode=\"Off\"\n"
        "tele.drive{\n"
        "tele.drive.shift=\"P\"\n"
        "tele.drive.odometer_km=12345.5\n"
        "tele.tires{\n"
        "tele.tires.fl=2.75\n"
        "tele.tires.fr=2.75\n"
        "tele.tires.rl=3\n"
        "tele.tires.warn=false\n"
        "tele.closures{\n"
        "tele.closures.locked=true\n"
        "tele.closures.door=false\n"
        "tele.closures.frunk=false\n"
        "tele.closures.trunk=true\n"
        "tele.closures.window=false\n"
        "ble{\n"
        "ble.connected=true\n"
        "ble.scanning=false\n"
        "ble.rssi=-60\n"
        "ble.addr=\"aa:bb:cc:dd:ee:ff\"\n"
        "link=\"awake\"\n"
        "vcsec_sleep=\"AWAKE\"\n"
        "vehicle{\n"
        "vehicle.soc=72.5\n"
        "vehicle.status=\"Charging\"\n"
        "vehicle.charge_limit=80\n"
        "vehicle.power=11\n"
        "vehicle.amps=16\n"
        "vehicle.actual_amps=16\n"
        "vehicle.volts=231\n"
        "vehicle.phases=2\n"
        "last{\n"
        "last.soc=72.5\n"
        "last.status=\"Charging\"\n"
        "last_seen_s=5\n"
        "charge_age_s=5\n"
        "sys{\n"
        "sys.board_mac=\"02:00:00:32:55:20\"\n"
        "sys.free_heap=0\n"
        "sys.min_free_heap=0\n"
        "sys.largest_block=0\n"
        "sys.uptime_s=0\n"
        "sys.wifi_reconnects=0\n"
        "sys.reset_reason=\"\"\n"
        "sys.safe_mode=false\n"
        "sys.stack_min_free_quantum_bytes=256\n"
        "sys.stack_min_free_bytes{\n"
        "sys.stack_min_free_bytes.httpd=4096\n"
        "sys.stack_min_free_bytes.vehicle=3072\n"
        "sys.stack_min_free_bytes.auto_pair=2048\n"
        "sys.stack_min_free_bytes.mqtt=1024\n"
        "sys.stack_min_free_bytes.main=null\n"
        "sys.stack_min_free_bytes.heap_supervisor=null\n"
        "sys.stack_min_free_bytes.display=null\n"
        "sys.stack_min_free_bytes.led=null\n"
        "sys.stack_min_free_bytes.ota_check=null\n"
        "sys.stack_min_free_bytes.ota_writer=null\n"
        "sys.stack_min_free_bytes.ota_gate=null\n"
        "sys.stack_min_free_bytes.net_watchdog=null\n"
        "sys.stack_min_free_bytes.syslog=null\n"
        "sys.stack_min_free_bytes.captive_dns=null\n"
        "sys.stack_min_free_bytes.captive_http=null\n"
        "sys.stack_min_free_bytes.wifi_event=null\n"
        "sys.stack_min_free_bytes.eth_event=null\n"
        "sys.stack_min_free_bytes.mqtt_event=null\n"
        "sys.stack_min_free_bytes.mqtt_probe_event=null\n"
        "sys.stack_min_free_bytes.nimble_host=null\n"
        "sys.stack_min_free_bytes.ble_scan_timer=null\n"
        "sys.stack_min_free_bytes.diag_log_callback=null\n"
        "sys.stack_min_free_bytes.sntp_sync=null\n"
        "sys.stack_min_free_bytes.ping_net=null\n"
        "sys.stack_min_free_bytes.ping_syslog=null\n"
        "sys.stack_min_free_bytes.ota_network=null\n"
        "sys.stack_min_free_bytes.ota_parser=null\n"));

    // A context that has not sampled yet is explicitly null. In safe mode the vehicle,
    // pairing and MQTT tasks intentionally do not exist.
    Inputs stack_partial;
    stack_partial.vin = "UNKNOWN";
    stack_partial.version = "1.4.2";
    stack_partial.stack_context_samples[0] = tk::stack_sample::merge(stack_partial.stack_context_samples[0], 0, 1536);
    CollectEmitter stack_out;
    tk::status::emit_status(stack_partial, stack_out);
    CHECK(stack_out.out.find("sys.stack_min_free_bytes.httpd=1536\n") != std::string::npos);
    CHECK(stack_out.out.find("sys.stack_min_free_bytes.vehicle=null\n") != std::string::npos);
    CHECK(stack_out.out.find("sys.stack_min_free_bytes.auto_pair=null\n") != std::string::npos);
    CHECK(stack_out.out.find("sys.stack_min_free_bytes.mqtt=null\n") != std::string::npos);

    // Unknown is distinct from a measured lower bound of zero (raw0..255 bytes).
    // Quantization must not turn a real sample into the unsampled sentinel.
    Inputs stack_zero;
    stack_zero.vin = "UNKNOWN";
    stack_zero.version = "1.4.2";
    stack_zero.stack_context_samples[0] = tk::stack_sample::merge(stack_zero.stack_context_samples[0], 0, 0);
    CollectEmitter stack_zero_out;
    tk::status::emit_status(stack_zero, stack_zero_out);
    CHECK(stack_zero_out.out.find("sys.stack_min_free_bytes.httpd=0\n") != std::string::npos);
    CHECK(stack_zero_out.out.find("sys.stack_min_free_bytes.vehicle=null\n") != std::string::npos);
    CHECK(stack_zero_out.out.find("sys.stack_min_free_bytes.auto_pair=null\n") != std::string::npos);
    CHECK(stack_zero_out.out.find("sys.stack_min_free_bytes.mqtt=null\n") != std::string::npos);

    // Scenario 2 — asleep: BLE link down (dropped between polls), charge cache retained,
    // debounced VCSEC sleep proven; no devices seen, no connect failures.
    Inputs s2;
    s2.vin = "5YJ3E1EA7KF000316"; s2.ip = "192.168.1.50"; s2.version = "1.4.2";
    s2.board_mac = "02:00:00:32:55:20";
    s2.key_present = true; s2.key_fingerprint = "AB:CD:EF:01";
    s2.paired = true;   // stamps below the plausibility floor stay omitted
    s2.wifi_connected = true; s2.wifi_ssid = "HomeNet"; s2.wifi_rssi = -58;
    s2.mqtt_configured = true; s2.mqtt_connected = false; s2.mqtt_tls = false;
    s2.mqtt_broker = "mqtt.local:1883"; s2.mqtt_error = "connection refused";
    s2.link = tk::LinkState::Asleep; s2.vcsec_sleep = "ASLEEP";
    s2.charge.valid = true;
    s2.charge.has_battery_level = true; s2.charge.battery_level = 68.0f;
    s2.charge.charging_state = "Stopped";
    s2.have_last_seen = true; s2.last_seen_s = 3600;
    // This boot was ended by the heap watchdog, so /status must say so — the whole point of
    // persisting it is that a device which self-heals unattended can still be diagnosed.
    s2.last_reboot = "heap:2";

    CollectEmitter e2;
    tk::status::emit_status(s2, e2);
    CHECK(golden_eq(e2.out, 
        "vin=\"5YJ3E1EA7KF000316\"\n"
        "ip=\"192.168.1.50\"\n"
        "version=\"1.4.2\"\n"
        "key_present=true\n"
        "key_fingerprint=\"AB:CD:EF:01\"\n"
        "paired=true\n"
        "session_available=false\n"
        "sessions{\n"
        "sessions.vcsec{\n"
        "sessions.vcsec.runtime_known=false\n"
        "sessions.vcsec.runtime_usable=false\n"
        "sessions.vcsec.persistence=\"unknown\"\n"
        "sessions.infotainment{\n"
        "sessions.infotainment.runtime_known=false\n"
        "sessions.infotainment.runtime_usable=false\n"
        "sessions.infotainment.persistence=\"unknown\"\n"
        "reauth=false\n"
        "wifi{\n"
        "wifi.ssid=\"HomeNet\"\n"
        "wifi.rssi=-58\n"
        "mqtt{\n"
        "mqtt.configured=true\n"
        "mqtt.connected=false\n"
        "mqtt.tls=false\n"
        "mqtt.broker=\"mqtt.local:1883\"\n"
        "mqtt.error=\"connection refused\"\n"
        "syslog{\n"
        "syslog.configured=false\n"
        "syslog.resolved=false\n"
        "syslog.reachable=false\n"
        "ble{\n"
        "ble.connected=false\n"
        "ble.scanning=false\n"
        "ble.devices[\n"
        "link=\"asleep\"\n"
        "vcsec_sleep=\"ASLEEP\"\n"
        "last{\n"
        "last.soc=68\n"
        "last.status=\"Stopped\"\n"
        "last_seen_s=3600\n"
        "last_reboot=\"heap:2\"\n"
        "sys{\n"
        "sys.board_mac=\"02:00:00:32:55:20\"\n"
        "sys.free_heap=0\n"
        "sys.min_free_heap=0\n"
        "sys.largest_block=0\n"
        "sys.uptime_s=0\n"
        "sys.wifi_reconnects=0\n"
        "sys.reset_reason=\"\"\n"
        "sys.safe_mode=false\n"
        "sys.stack_min_free_quantum_bytes=256\n"
        "sys.stack_min_free_bytes{\n"
        "sys.stack_min_free_bytes.httpd=null\n"
        "sys.stack_min_free_bytes.vehicle=null\n"
        "sys.stack_min_free_bytes.auto_pair=null\n"
        "sys.stack_min_free_bytes.mqtt=null\n"
        "sys.stack_min_free_bytes.main=null\n"
        "sys.stack_min_free_bytes.heap_supervisor=null\n"
        "sys.stack_min_free_bytes.display=null\n"
        "sys.stack_min_free_bytes.led=null\n"
        "sys.stack_min_free_bytes.ota_check=null\n"
        "sys.stack_min_free_bytes.ota_writer=null\n"
        "sys.stack_min_free_bytes.ota_gate=null\n"
        "sys.stack_min_free_bytes.net_watchdog=null\n"
        "sys.stack_min_free_bytes.syslog=null\n"
        "sys.stack_min_free_bytes.captive_dns=null\n"
        "sys.stack_min_free_bytes.captive_http=null\n"
        "sys.stack_min_free_bytes.wifi_event=null\n"
        "sys.stack_min_free_bytes.eth_event=null\n"
        "sys.stack_min_free_bytes.mqtt_event=null\n"
        "sys.stack_min_free_bytes.mqtt_probe_event=null\n"
        "sys.stack_min_free_bytes.nimble_host=null\n"
        "sys.stack_min_free_bytes.ble_scan_timer=null\n"
        "sys.stack_min_free_bytes.diag_log_callback=null\n"
        "sys.stack_min_free_bytes.sntp_sync=null\n"
        "sys.stack_min_free_bytes.ping_net=null\n"
        "sys.stack_min_free_bytes.ping_syslog=null\n"
        "sys.stack_min_free_bytes.ota_network=null\n"
        "sys.stack_min_free_bytes.ota_parser=null\n"));

    // Scenario 3 — unreachable, scanning, found-but-can't-connect: devices listed, a
    // connect-fail streak with last-seen advert RSSI and a non-connectable target (the
    // car at its ~3-device BLE limit).
    Inputs s3;
    s3.vin = "5YJ3E1EA7KF000316"; s3.ip = "10.0.0.7"; s3.version = "1.4.2";
    s3.board_mac = "02:00:00:32:55:20";
    s3.key_present = true; s3.key_fingerprint = "AB:CD:EF:01"; s3.paired = true;
    s3.wifi_connected = true; s3.wifi_ssid = "Garage"; s3.wifi_rssi = -71; s3.wifi_std = "Wi-Fi 4";
    s3.ble_scanning = true;
    s3.devices.push_back({ "de:ad:be:ef:00:01", "S1a87029XXXXXXXXC", -77, true });
    s3.devices.push_back({ "de:ad:be:ef:00:02", "Sxxxxxxxxxxxxxxxx", -90, false });
    s3.connect_fail = 4;
    s3.have_seen_rssi = true; s3.seen_rssi = -77;
    s3.target_connectable = 0;
    s3.link = tk::LinkState::Unreachable; s3.vcsec_sleep = "UNKNOWN";
    s3.charge.valid = true;
    s3.charge.has_battery_level = true; s3.charge.battery_level = 54.0f;
    s3.charge.charging_state = "Disconnected";
    s3.have_last_seen = true; s3.last_seen_s = 90000;
    s3.ble_phase = "waiting"; s3.ble_phase_s = 15;

    CollectEmitter e3;
    tk::status::emit_status(s3, e3);
    CHECK(golden_eq(e3.out, 
        "vin=\"5YJ3E1EA7KF000316\"\n"
        "ip=\"10.0.0.7\"\n"
        "version=\"1.4.2\"\n"
        "key_present=true\n"
        "key_fingerprint=\"AB:CD:EF:01\"\n"
        "paired=true\n"
        "session_available=false\n"
        "sessions{\n"
        "sessions.vcsec{\n"
        "sessions.vcsec.runtime_known=false\n"
        "sessions.vcsec.runtime_usable=false\n"
        "sessions.vcsec.persistence=\"unknown\"\n"
        "sessions.infotainment{\n"
        "sessions.infotainment.runtime_known=false\n"
        "sessions.infotainment.runtime_usable=false\n"
        "sessions.infotainment.persistence=\"unknown\"\n"
        "reauth=false\n"
        "wifi{\n"
        "wifi.ssid=\"Garage\"\n"
        "wifi.rssi=-71\n"
        "wifi.std=\"Wi-Fi 4\"\n"
        "mqtt{\n"
        "mqtt.configured=false\n"
        "mqtt.connected=false\n"
        "mqtt.tls=false\n"
        "syslog{\n"
        "syslog.configured=false\n"
        "syslog.resolved=false\n"
        "syslog.reachable=false\n"
        "ble{\n"
        "ble.connected=false\n"
        "ble.scanning=true\n"
        "ble.phase=\"waiting\"\n"
        "ble.phase_s=15\n"
        "ble.devices[\n"
        "ble.devices.0{\n"
        "ble.devices.0.addr=\"de:ad:be:ef:00:01\"\n"
        "ble.devices.0.name=\"S1a87029XXXXXXXXC\"\n"
        "ble.devices.0.rssi=-77\n"
        "ble.devices.0.connectable=true\n"
        "ble.devices.1{\n"
        "ble.devices.1.addr=\"de:ad:be:ef:00:02\"\n"
        "ble.devices.1.name=\"Sxxxxxxxxxxxxxxxx\"\n"
        "ble.devices.1.rssi=-90\n"
        "ble.devices.1.connectable=false\n"
        "ble.connect_fail=4\n"
        "ble.rssi=-77\n"
        "ble.car_connectable=false\n"
        "link=\"unreachable\"\n"
        "vcsec_sleep=\"UNKNOWN\"\n"
        "last{\n"
        "last.soc=54\n"
        "last.status=\"Disconnected\"\n"
        "last_seen_s=90000\n"
        "sys{\n"
        "sys.board_mac=\"02:00:00:32:55:20\"\n"
        "sys.free_heap=0\n"
        "sys.min_free_heap=0\n"
        "sys.largest_block=0\n"
        "sys.uptime_s=0\n"
        "sys.wifi_reconnects=0\n"
        "sys.reset_reason=\"\"\n"
        "sys.safe_mode=false\n"
        "sys.stack_min_free_quantum_bytes=256\n"
        "sys.stack_min_free_bytes{\n"
        "sys.stack_min_free_bytes.httpd=null\n"
        "sys.stack_min_free_bytes.vehicle=null\n"
        "sys.stack_min_free_bytes.auto_pair=null\n"
        "sys.stack_min_free_bytes.mqtt=null\n"
        "sys.stack_min_free_bytes.main=null\n"
        "sys.stack_min_free_bytes.heap_supervisor=null\n"
        "sys.stack_min_free_bytes.display=null\n"
        "sys.stack_min_free_bytes.led=null\n"
        "sys.stack_min_free_bytes.ota_check=null\n"
        "sys.stack_min_free_bytes.ota_writer=null\n"
        "sys.stack_min_free_bytes.ota_gate=null\n"
        "sys.stack_min_free_bytes.net_watchdog=null\n"
        "sys.stack_min_free_bytes.syslog=null\n"
        "sys.stack_min_free_bytes.captive_dns=null\n"
        "sys.stack_min_free_bytes.captive_http=null\n"
        "sys.stack_min_free_bytes.wifi_event=null\n"
        "sys.stack_min_free_bytes.eth_event=null\n"
        "sys.stack_min_free_bytes.mqtt_event=null\n"
        "sys.stack_min_free_bytes.mqtt_probe_event=null\n"
        "sys.stack_min_free_bytes.nimble_host=null\n"
        "sys.stack_min_free_bytes.ble_scan_timer=null\n"
        "sys.stack_min_free_bytes.diag_log_callback=null\n"
        "sys.stack_min_free_bytes.sntp_sync=null\n"
        "sys.stack_min_free_bytes.ping_net=null\n"
        "sys.stack_min_free_bytes.ping_syslog=null\n"
        "sys.stack_min_free_bytes.ota_network=null\n"
        "sys.stack_min_free_bytes.ota_parser=null\n"));

    // Scenario 4 — factory-fresh: no key, unpaired, no WiFi yet (portal case aside),
    // nothing cached, nothing heard. The contract still emits the empty wifi/mqtt/ble
    // containers and the devices[] array (app.js reads them unconditionally).
    Inputs s4;
    s4.vin = "UNKNOWN"; s4.version = "1.4.2";
    s4.board_mac = "02:00:00:32:55:20";
    CollectEmitter e4;
    tk::status::emit_status(s4, e4);
    CHECK(golden_eq(e4.out, 
        "vin=\"UNKNOWN\"\n"
        "ip=\"\"\n"
        "version=\"1.4.2\"\n"
        "key_present=false\n"
        "key_fingerprint=\"\"\n"
        "paired=false\n"
        "session_available=false\n"
        "sessions{\n"
        "sessions.vcsec{\n"
        "sessions.vcsec.runtime_known=false\n"
        "sessions.vcsec.runtime_usable=false\n"
        "sessions.vcsec.persistence=\"unknown\"\n"
        "sessions.infotainment{\n"
        "sessions.infotainment.runtime_known=false\n"
        "sessions.infotainment.runtime_usable=false\n"
        "sessions.infotainment.persistence=\"unknown\"\n"
        "reauth=false\n"
        "wifi{\n"
        "mqtt{\n"
        "mqtt.configured=false\n"
        "mqtt.connected=false\n"
        "mqtt.tls=false\n"
        "syslog{\n"
        "syslog.configured=false\n"
        "syslog.resolved=false\n"
        "syslog.reachable=false\n"
        "ble{\n"
        "ble.connected=false\n"
        "ble.scanning=false\n"
        "ble.devices[\n"
        "link=\"unknown\"\n"
        "vcsec_sleep=\"UNKNOWN\"\n"
        "sys{\n"
        "sys.board_mac=\"02:00:00:32:55:20\"\n"
        "sys.free_heap=0\n"
        "sys.min_free_heap=0\n"
        "sys.largest_block=0\n"
        "sys.uptime_s=0\n"
        "sys.wifi_reconnects=0\n"
        "sys.reset_reason=\"\"\n"
        "sys.safe_mode=false\n"
        "sys.stack_min_free_quantum_bytes=256\n"
        "sys.stack_min_free_bytes{\n"
        "sys.stack_min_free_bytes.httpd=null\n"
        "sys.stack_min_free_bytes.vehicle=null\n"
        "sys.stack_min_free_bytes.auto_pair=null\n"
        "sys.stack_min_free_bytes.mqtt=null\n"
        "sys.stack_min_free_bytes.main=null\n"
        "sys.stack_min_free_bytes.heap_supervisor=null\n"
        "sys.stack_min_free_bytes.display=null\n"
        "sys.stack_min_free_bytes.led=null\n"
        "sys.stack_min_free_bytes.ota_check=null\n"
        "sys.stack_min_free_bytes.ota_writer=null\n"
        "sys.stack_min_free_bytes.ota_gate=null\n"
        "sys.stack_min_free_bytes.net_watchdog=null\n"
        "sys.stack_min_free_bytes.syslog=null\n"
        "sys.stack_min_free_bytes.captive_dns=null\n"
        "sys.stack_min_free_bytes.captive_http=null\n"
        "sys.stack_min_free_bytes.wifi_event=null\n"
        "sys.stack_min_free_bytes.eth_event=null\n"
        "sys.stack_min_free_bytes.mqtt_event=null\n"
        "sys.stack_min_free_bytes.mqtt_probe_event=null\n"
        "sys.stack_min_free_bytes.nimble_host=null\n"
        "sys.stack_min_free_bytes.ble_scan_timer=null\n"
        "sys.stack_min_free_bytes.diag_log_callback=null\n"
        "sys.stack_min_free_bytes.sntp_sync=null\n"
        "sys.stack_min_free_bytes.ping_net=null\n"
        "sys.stack_min_free_bytes.ping_syslog=null\n"
        "sys.stack_min_free_bytes.ota_network=null\n"
        "sys.stack_min_free_bytes.ota_parser=null\n"));

    // Scenario 5 — cold pack (issue #276): vehicle reports nominal battery_level (tag 114)
    // AND usable_battery_level (tag 115) with delta due to cold buffer.
    Inputs s5;
    s5.vin = "5YJ3E1EA7KF000316"; s5.ip = "192.168.1.50"; s5.version = "1.4.2";
    s5.board_mac = "02:00:00:32:55:20";
    s5.key_present = true; s5.key_fingerprint = "AB:CD:EF:01";
    s5.paired = true;
    s5.wifi_connected = true; s5.wifi_ssid = "HomeNet"; s5.wifi_rssi = -55;
    s5.ble_connected = false;
    s5.ble_scanning = false;
    s5.link = tk::LinkState::Awake; s5.vcsec_sleep = "AWAKE";
    s5.charge.valid = true;
    s5.charge.has_battery_level = true;        s5.charge.battery_level = 76.0f;
    s5.charge.has_usable_battery_level = true; s5.charge.usable_battery_level = 72.0f;
    s5.charge.charging_state = "Charging";
    s5.have_last_seen = true; s5.last_seen_s = 5;
    s5.have_charge_age = true; s5.charge_age_s = 0;

    CollectEmitter e5;
    tk::status::emit_status(s5, e5);
    CHECK(golden_eq(e5.out,
        "vin=\"5YJ3E1EA7KF000316\"\n"
        "ip=\"192.168.1.50\"\n"
        "version=\"1.4.2\"\n"
        "key_present=true\n"
        "key_fingerprint=\"AB:CD:EF:01\"\n"
        "paired=true\n"
        "session_available=false\n"
        "sessions{\n"
        "sessions.vcsec{\n"
        "sessions.vcsec.runtime_known=false\n"
        "sessions.vcsec.runtime_usable=false\n"
        "sessions.vcsec.persistence=\"unknown\"\n"
        "sessions.infotainment{\n"
        "sessions.infotainment.runtime_known=false\n"
        "sessions.infotainment.runtime_usable=false\n"
        "sessions.infotainment.persistence=\"unknown\"\n"
        "reauth=false\n"
        "wifi{\n"
        "wifi.ssid=\"HomeNet\"\n"
        "wifi.rssi=-55\n"
        "mqtt{\n"
        "mqtt.configured=false\n"
        "mqtt.connected=false\n"
        "mqtt.tls=false\n"
        "syslog{\n"
        "syslog.configured=false\n"
        "syslog.resolved=false\n"
        "syslog.reachable=false\n"
        "ble{\n"
        "ble.connected=false\n"
        "ble.scanning=false\n"
        "ble.devices[\n"
        "link=\"awake\"\n"
        "vcsec_sleep=\"AWAKE\"\n"
        "vehicle{\n"
        "vehicle.soc=76\n"
        "vehicle.usable_soc=72\n"
        "vehicle.status=\"Charging\"\n"
        "last{\n"
        "last.soc=76\n"
        "last.usable_soc=72\n"
        "last.status=\"Charging\"\n"
        "last_seen_s=5\n"
        "charge_age_s=0\n"
        "sys{\n"
        "sys.board_mac=\"02:00:00:32:55:20\"\n"
        "sys.free_heap=0\n"
        "sys.min_free_heap=0\n"
        "sys.largest_block=0\n"
        "sys.uptime_s=0\n"
        "sys.wifi_reconnects=0\n"
        "sys.reset_reason=\"\"\n"
        "sys.safe_mode=false\n"
        "sys.stack_min_free_quantum_bytes=256\n"
        "sys.stack_min_free_bytes{\n"
        "sys.stack_min_free_bytes.httpd=null\n"
        "sys.stack_min_free_bytes.vehicle=null\n"
        "sys.stack_min_free_bytes.auto_pair=null\n"
        "sys.stack_min_free_bytes.mqtt=null\n"
        "sys.stack_min_free_bytes.main=null\n"
        "sys.stack_min_free_bytes.heap_supervisor=null\n"
        "sys.stack_min_free_bytes.display=null\n"
        "sys.stack_min_free_bytes.led=null\n"
        "sys.stack_min_free_bytes.ota_check=null\n"
        "sys.stack_min_free_bytes.ota_writer=null\n"
        "sys.stack_min_free_bytes.ota_gate=null\n"
        "sys.stack_min_free_bytes.net_watchdog=null\n"
        "sys.stack_min_free_bytes.syslog=null\n"
        "sys.stack_min_free_bytes.captive_dns=null\n"
        "sys.stack_min_free_bytes.captive_http=null\n"
        "sys.stack_min_free_bytes.wifi_event=null\n"
        "sys.stack_min_free_bytes.eth_event=null\n"
        "sys.stack_min_free_bytes.mqtt_event=null\n"
        "sys.stack_min_free_bytes.mqtt_probe_event=null\n"
        "sys.stack_min_free_bytes.nimble_host=null\n"
        "sys.stack_min_free_bytes.ble_scan_timer=null\n"
        "sys.stack_min_free_bytes.diag_log_callback=null\n"
        "sys.stack_min_free_bytes.sntp_sync=null\n"
        "sys.stack_min_free_bytes.ping_net=null\n"
        "sys.stack_min_free_bytes.ping_syslog=null\n"
        "sys.stack_min_free_bytes.ota_network=null\n"
        "sys.stack_min_free_bytes.ota_parser=null\n"));

    // ── BLE phase countdown presence rules ────────────────────────────────────────────
    // phase_s rides WITH phase and is emitted even at 0. A countdown that vanished on its
    // last second left the Bluetooth row showing a bare "Disconnected" for the seconds
    // between the wait expiring and the next attempt being visible — the bug this pair of
    // fields exists to fix. Never emit one without the other, and never gate on > 0.
    Inputs p0;                       // phase running, zero seconds left
    p0.vin = "UNKNOWN"; p0.version = "1.4.2";
    p0.ble_phase = "connecting"; p0.ble_phase_s = 0;
    CollectEmitter ep0;
    tk::status::emit_status(p0, ep0);
    CHECK(ep0.out.find("ble.phase=\"connecting\"\n") != std::string::npos);
    CHECK(ep0.out.find("ble.phase_s=0\n") != std::string::npos);

    Inputs pn;                       // no phase running → both fields absent
    pn.vin = "UNKNOWN"; pn.version = "1.4.2";
    pn.ble_phase_s = 42;             // ignored: phase is what gates the pair
    CollectEmitter epn;
    tk::status::emit_status(pn, epn);
    CHECK(epn.out.find("ble.phase") == std::string::npos);

    // ── OTA update channel presence rules ─────────────────────────────────────────────
    Inputs ota_dev;
    ota_dev.vin = "UNKNOWN"; ota_dev.version = "1.4.2";
    ota_dev.ota_channel = "dev";
    ota_dev.ota_verification = tk::OtaVerificationState::PendingVerify;
    CollectEmitter e_ota_dev;
    tk::status::emit_status(ota_dev, e_ota_dev);
    CHECK(e_ota_dev.out.find("ota{\nota.channel=\"dev\"\n") != std::string::npos);
    CHECK(e_ota_dev.out.find("ota.verification=\"pending_verify\"\n") != std::string::npos);

    Inputs ota_none;
    ota_none.vin = "UNKNOWN"; ota_none.version = "1.4.2";
    CollectEmitter e_ota_none;
    tk::status::emit_status(ota_none, e_ota_none);
    CHECK(e_ota_none.out.find("ota{") == std::string::npos);
    // RAM usability is independent of durable presence and confirms no NVS write.
    tk::status::Inputs ram_only;
    ram_only.paired = false;
    ram_only.sessions.vcsec = {true, true, tk::SessionSaveState::Error};
    ram_only.sessions.infotainment = {true, true, tk::SessionSaveState::Confirmed};
    CollectEmitter runtime_status;
    tk::status::emit_status(ram_only, runtime_status);
    CHECK(runtime_status.out.find("paired=false\nsession_available=true\n") != std::string::npos);
    CHECK(runtime_status.out.find("sessions.vcsec.persistence=\"error\"") != std::string::npos);
    CHECK(runtime_status.out.find("sessions.infotainment.persistence=\"confirmed\"") != std::string::npos);

}

// ─── GET /api/1/vehicles/{VIN}/vehicle_data charge_state contract ─────────────
static void test_vehicle_data() {
    CHECK(tk::vehicle_data_number(0.0) == 0.0);
    CHECK(tk::vehicle_data_number(-0.0) == 0.0);
    CHECK(tk::vehicle_data_number(12.5) == 12.5);
    CHECK(tk::vehicle_data_number(-3.25) == -3.25);
    CHECK(tk::vehicle_data_number(std::numeric_limits<double>::max()) ==
          std::numeric_limits<double>::max());
    CHECK(tk::vehicle_data_number(std::numeric_limits<double>::denorm_min()) ==
          std::numeric_limits<double>::denorm_min());
    CHECK(tk::vehicle_data_number(std::numeric_limits<double>::quiet_NaN()) == 0.0);
    CHECK(tk::vehicle_data_number(std::numeric_limits<double>::infinity()) == 0.0);
    CHECK(tk::vehicle_data_number(-std::numeric_limits<double>::infinity()) == 0.0);

    // Scenario 1 — actively charging: all fields populated, non-zero minutes_to_full_charge
    ChargeStateResult cs1{};
    cs1.valid = true;
    cs1.charging_state = "Charging";
    cs1.battery_level = 72;
    cs1.charge_limit_soc = 80;
    cs1.charger_power = 11;
    cs1.charge_rate = 58.25f;
    cs1.charging_amps = 16;
    cs1.charge_energy_added = 12.5f;
    cs1.battery_range = 280.5f;
    cs1.minutes_to_full_charge = 45;

    CollectEmitter e1;
    tk::emit_vehicle_charge_state(cs1, e1);
    CHECK(golden_eq(e1.out,
        "charging_state=\"Charging\"\n"
        "battery_level=72\n"
        "usable_battery_level=72\n"
        "charge_limit_soc=80\n"
        "charger_power=11\n"
        "charge_rate=58.25\n"
        "charge_amps=16\n"
        "charge_energy_added=12.5\n"
        "battery_range=280.5\n"
        "minutes_to_full_charge=45\n"));

    // Scenario 2 — zero-initialized / disconnected: every field must still be emitted (evcc requirement),
    // with minutes_to_full_charge emitting 0 so evcc's '> 0' guard yields ""
    ChargeStateResult cs2{};

    CollectEmitter e2;
    tk::emit_vehicle_charge_state(cs2, e2);
    CHECK(golden_eq(e2.out,
        "charging_state=\"Disconnected\"\n"
        "battery_level=0\n"
        "usable_battery_level=0\n"
        "charge_limit_soc=0\n"
        "charger_power=0\n"
        "charge_rate=0\n"
        "charge_amps=0\n"
        "charge_energy_added=0\n"
        "battery_range=0\n"
        "minutes_to_full_charge=0\n"));

    // Scenario 3 — climate_state actively conditioning & defroster on (evcc climater / .is_preconditioning)
    ClimateStateResult cl1{};
    cl1.valid = true;
    cl1.is_climate_on = true;
    cl1.has_preconditioning = true;
    cl1.is_preconditioning = true;
    cl1.inside_temp = 21.5f;
    cl1.outside_temp = 5.0f;
    cl1.driver_setpoint = 22.0f;
    cl1.front_defrost = true;
    cl1.rear_defrost = false;
    cl1.defrost_mode = "Normal";
    CollectEmitter ec1;
    tk::emit_vehicle_climate_state(cl1, ec1);
    CHECK(golden_eq(ec1.out,
        "is_climate_on=true\n"
        "is_preconditioning=true\n"
        "inside_temp=21.5\n"
        "outside_temp=5\n"
        "driver_temp_setting=22\n"));

    const auto climate_data = tk::vehicle_climate_data(cl1);
    CHECK(climate_data.valid && climate_data.has_preconditioning);
    CHECK(climate_data.is_climate_on && climate_data.is_preconditioning);
    CHECK_NEAR(climate_data.inside_temp, 21.5);
    CHECK_NEAR(climate_data.outside_temp, 5.0);
    CHECK_NEAR(climate_data.driver_setpoint, 22.0);
    CollectEmitter projected;
    tk::emit_vehicle_climate_state(climate_data, projected);
    CHECK(projected.out == ec1.out);

    // Scenario 4 — zero-initialized climate_state: every field must still be emitted so JSON parsers
    // never hit a missing key or <nil>
    ClimateStateResult cl2{};
    CollectEmitter ec2;
    tk::emit_vehicle_climate_state(cl2, ec2);
    CHECK(golden_eq(ec2.out,
        "is_climate_on=false\n"
        "is_preconditioning=false\n"
        "inside_temp=0\n"
        "outside_temp=0\n"
        "driver_temp_setting=0\n"));

    // An unreported is_preconditioning emits false (TeslaBleHttpProxy GetIsPreconditioning());
    // only a present-AND-true field emits true.
    {
        tk::VehicleClimateData absent{};
        absent.valid = true;
        absent.is_preconditioning = true;   // stale value behind an absent presence flag
        CollectEmitter ea;
        tk::emit_vehicle_climate_state(absent, ea);
        CHECK(ea.out.find("is_preconditioning=false\n") != std::string::npos);
        absent.has_preconditioning = true;
        CollectEmitter er;
        tk::emit_vehicle_climate_state(absent, er);
        CHECK(er.out.find("is_preconditioning=true\n") != std::string::npos);
        absent.is_preconditioning = false;
        CollectEmitter ef;
        tk::emit_vehicle_climate_state(absent, ef);
        CHECK(ef.out.find("is_preconditioning=false\n") != std::string::npos);
    }

    // Climate freshness: invalid never usable; outside the window last-known at any age;
    // inside it a sample must exist and be <= kActiveClimateStateMaxAgeS.
    CHECK(tk::kActiveClimateStateMaxAgeS == 300);
    CHECK(!tk::climate_cache_usable(false, false, false, 0));
    CHECK(!tk::climate_cache_usable(false, true, true, 0));
    CHECK(tk::climate_cache_usable(true, false, false, 0));
    CHECK(tk::climate_cache_usable(true, false, true, 86400));
    CHECK(!tk::climate_cache_usable(true, true, false, 0));
    CHECK(tk::climate_cache_usable(true, true, true, 0));
    CHECK(tk::climate_cache_usable(true, true, true, 300));
    CHECK(!tk::climate_cache_usable(true, true, true, 301));
    CHECK(tk::climate_cache_usable(true, true, true, tk::kActiveClimateStateMaxAgeS));
    CHECK(!tk::climate_cache_usable(true, true, true, tk::kActiveClimateStateMaxAgeS + 1));

    // drive_state: odometer in MILES, 0 when absent; projection never copies shift_state.
    {
        DriveStateResult dr{};
        dr.valid = true;
        dr.shift_state = "D";
        dr.has_odometer = true;
        dr.odometer_km = 1609.344f;   // 1000 mi
        const auto dd = tk::vehicle_drive_data(dr);
        CHECK(dd.valid && dd.has_odometer);
        CHECK(dd.odometer_km == 1609.344f);
        CollectEmitter ed;
        tk::emit_vehicle_drive_state(dd, ed);
        CHECK(golden_eq(ed.out, "odometer=1000\n"));
        CHECK(tk::vehicle_drive_available(dd));

        CollectEmitter ed2;
        tk::emit_vehicle_drive_state(tk::VehicleDriveData{}, ed2);
        CHECK(golden_eq(ed2.out, "odometer=0\n"));
        // Absent odometer emits 0 even if a stale km value is left in the struct.
        tk::VehicleDriveData stale{true, false, 1609.344f};
        CollectEmitter ed3;
        tk::emit_vehicle_drive_state(stale, ed3);
        CHECK(golden_eq(ed3.out, "odometer=0\n"));

        CHECK(!tk::vehicle_drive_available(tk::VehicleDriveData{}));
        CHECK(!tk::vehicle_drive_available(stale));
        CHECK(!tk::vehicle_drive_available(tk::VehicleDriveData{false, true, 1609.344f}));
        CHECK(tk::vehicle_drive_available(tk::VehicleDriveData{true, true, 1609.344f}));
    }

    // Endpoint selection factories.
    {
        CHECK(tk::VehicleDataEndpoints{}.empty() && !tk::VehicleDataEndpoints{}.has_unsupported);
        CHECK(tk::VehicleDataEndpoints::of(0).empty());
        const auto all = tk::VehicleDataEndpoints::of(
            tk::kVehicleDataCharge | tk::kVehicleDataClimate | tk::kVehicleDataDrive);
        CHECK(all.charge_state && all.climate_state && all.drive_state && !all.has_unsupported && !all.empty());
        const auto drive_only = tk::VehicleDataEndpoints::of(tk::kVehicleDataDrive);
        CHECK(!drive_only.charge_state && !drive_only.climate_state && drive_only.drive_state && !drive_only.empty());
        const auto bad = tk::VehicleDataEndpoints::unsupported();
        CHECK(bad.empty() && bad.has_unsupported);
        CHECK(sizeof(tk::VehicleDataEndpoints) == 1);
    }

    // vehicle_data_available: every selected domain must be ok; empty/unsupported never.
    for (unsigned mask = 0; mask < 8; ++mask) {
        for (unsigned oks = 0; oks < 8; ++oks) {
            const bool charge_ok = oks & 1, climate_ok = oks & 2, drive_ok = oks & 4;
            auto ep = tk::VehicleDataEndpoints::of(mask);
            const bool expected = mask != 0 &&
                (!(mask & tk::kVehicleDataCharge) || charge_ok) &&
                (!(mask & tk::kVehicleDataClimate) || climate_ok) &&
                (!(mask & tk::kVehicleDataDrive) || drive_ok);
            CHECK(tk::vehicle_data_available(ep, charge_ok, climate_ok, drive_ok) == expected);
            ep.has_unsupported = true;
            CHECK(!tk::vehicle_data_available(ep, charge_ok, climate_ok, drive_ok));
        }
    }
    CHECK(!tk::vehicle_data_available(tk::VehicleDataEndpoints::unsupported(), true, true, true));
    CHECK(tk::parse_vehicle_data_endpoints("cached=" + std::string(120, 'x')).has_unsupported == false);
    CHECK(tk::parse_vehicle_data_endpoints("cached=" + std::string(121, 'x')).has_unsupported);
    CHECK(tk::parse_vehicle_data_endpoint_list({}).has_unsupported);

    // Scenario 5 — endpoints query parsing
    {
        // No endpoints query -> both charge_state and climate_state
        auto ep = tk::parse_vehicle_data_endpoints("");
        CHECK(ep.charge_state && ep.climate_state && !ep.has_unsupported && !ep.empty());

        // Empty pairs around an unrelated parameter -> default (charge + climate, not drive)
        ep = tk::parse_vehicle_data_endpoints("&&");
        CHECK(ep.charge_state && ep.climate_state && !ep.drive_state && !ep.has_unsupported && !ep.empty());

        // Other query parameter without endpoints -> charge + climate, never drive
        ep = tk::parse_vehicle_data_endpoints("cached=1");
        CHECK(ep.charge_state && ep.climate_state && !ep.drive_state && !ep.has_unsupported && !ep.empty());
        ep = tk::parse_vehicle_data_endpoints("cached=1&");
        CHECK(ep.charge_state && ep.climate_state && !ep.drive_state && !ep.has_unsupported && !ep.empty());

        // Explicit endpoints=charge_state (evcc standard poll)
        ep = tk::parse_vehicle_data_endpoints("endpoints=charge_state");
        CHECK(ep.charge_state && !ep.climate_state && !ep.has_unsupported && !ep.empty());

        // endpoints=charge_state with a trailing unrelated parameter -> charge only
        ep = tk::parse_vehicle_data_endpoints("endpoints=charge_state&cached=1");
        CHECK(ep.charge_state && !ep.climate_state && !ep.drive_state && !ep.has_unsupported && !ep.empty());

        // Selector-list parser used by the query parser
        ep = tk::parse_vehicle_data_endpoint_list("charge_state");
        CHECK(ep.charge_state && !ep.climate_state && !ep.has_unsupported && !ep.empty());

        // Explicit endpoints=climate_state (evcc preconditioning poll)
        ep = tk::parse_vehicle_data_endpoints("endpoints=climate_state");
        CHECK(!ep.charge_state && ep.climate_state && !ep.has_unsupported && !ep.empty());

        // Semicolon delimited (Tesla Fleet API standard)
        ep = tk::parse_vehicle_data_endpoints("endpoints=charge_state;climate_state");
        CHECK(ep.charge_state && ep.climate_state && !ep.has_unsupported && !ep.empty());

        // Comma delimited
        ep = tk::parse_vehicle_data_endpoints("endpoints=climate_state,charge_state");
        CHECK(ep.charge_state && ep.climate_state && !ep.has_unsupported && !ep.empty());

        // Delimited with whitespace
        ep = tk::parse_vehicle_data_endpoints("endpoints= charge_state ; climate_state ");
        CHECK(ep.charge_state && ep.climate_state && !ep.has_unsupported && !ep.empty());

        // Multi-query parameters
        ep = tk::parse_vehicle_data_endpoints("foo=1&endpoints=climate_state&bar=2");
        CHECK(!ep.charge_state && ep.climate_state && !ep.has_unsupported && !ep.empty());

        // Exact query keys: a suffix or a value containing endpoints= is unrelated.
        ep = tk::parse_vehicle_data_endpoints("not_endpoints=climate_state&endpoints=charge_state");
        CHECK(ep.charge_state && !ep.climate_state && !ep.has_unsupported);
        ep = tk::parse_vehicle_data_endpoints("other=endpoints=climate_state");
        CHECK(ep.charge_state && ep.climate_state && !ep.has_unsupported);
        for (const char* encoded : {"charge_state%3Bclimate_state", "climate_state%3bcharge_state",
                                    "charge_state%2Cclimate_state", "climate_state%2ccharge_state"}) {
            ep = tk::parse_vehicle_data_endpoint_list(encoded);
            CHECK(ep.charge_state && ep.climate_state && !ep.has_unsupported);
        }
        for (const char* invalid : {"endpoints=", "endpoints", "endpoints=charge_state&endpoints=climate_state"}) {
            ep = tk::parse_vehicle_data_endpoints(invalid);
            CHECK(ep.has_unsupported);
        }
        for (const char* invalid : {"charge_state;charge_state", "charge_state;", ";climate_state",
                                    "charge_state,,climate_state", "charge_state%3", "charge_state%ZZ",
                                    "charge_state%00climate_state", "charge_state%3Bunknown_state"}) {
            ep = tk::parse_vehicle_data_endpoint_list(invalid);
            CHECK(ep.has_unsupported);
        }

        // drive_state (evcc odometer): alone, in every delimiter form, and fail-closed variants
        ep = tk::parse_vehicle_data_endpoints("endpoints=drive_state");
        CHECK(!ep.charge_state && !ep.climate_state && ep.drive_state && !ep.has_unsupported && !ep.empty());
        for (const char* all3 : {"charge_state;climate_state;drive_state", "drive_state,climate_state,charge_state",
                                 "charge_state%3Bclimate_state%3Bdrive_state",
                                 "drive_state%2cclimate_state%2ccharge_state",
                                 " drive_state ; charge_state , climate_state "}) {
            ep = tk::parse_vehicle_data_endpoint_list(all3);
            CHECK(ep.charge_state && ep.climate_state && ep.drive_state && !ep.has_unsupported);
        }
        ep = tk::parse_vehicle_data_endpoints("endpoints=charge_state;drive_state");
        CHECK(ep.charge_state && !ep.climate_state && ep.drive_state && !ep.has_unsupported);
        for (const char* invalid : {"drive_state;drive_state", "drive_stat", "drive_state;", ";drive_state",
                                    "drive_states", "Drive_State", "drive_state%3", "drive_state;unknown_state",
                                    "charge_state;climate_state;drive_state%3B"}) {
            ep = tk::parse_vehicle_data_endpoint_list(invalid);
            CHECK(ep.has_unsupported && ep.empty());
        }

        // Unsupported endpoint
        ep = tk::parse_vehicle_data_endpoints("endpoints=location_data");
        CHECK(!ep.charge_state && !ep.climate_state && !ep.drive_state && ep.has_unsupported && ep.empty());

        // Mixed supported and unsupported
        ep = tk::parse_vehicle_data_endpoints("endpoints=charge_state;unknown_state");
        CHECK(ep.has_unsupported && ep.empty());
    }
}

// ─── on-device display presenter (logic/display_model.hpp <- display.cpp compose()) ──────
namespace dm = tk::display;

// Build a UiSnapshot tersely for the presenter tests.
static tk::UiSnapshot us(tk::LinkState ls, bool net_up, bool ble_conn, bool paired,
                         bool have_soc, int soc, bool charging) {
    tk::UiSnapshot s;
    s.link_state     = ls;
    s.net            = net_up ? tk::NetLink::Wifi : tk::NetLink::None;
    s.ble_connected  = ble_conn;
    s.paired         = paired;
    s.have_soc       = have_soc;
    s.soc            = soc;
    s.charging       = charging;
    return s;
}

static void test_display_helpers() {
    // RSSI → 0..4 bars, at and around every threshold.
    CHECK(dm::rssi_bars(-50) == 4);  CHECK(dm::rssi_bars(-55) == 4);  CHECK(dm::rssi_bars(-56) == 3);
    CHECK(dm::rssi_bars(-65) == 3);  CHECK(dm::rssi_bars(-66) == 2);
    CHECK(dm::rssi_bars(-75) == 2);  CHECK(dm::rssi_bars(-76) == 1);
    CHECK(dm::rssi_bars(-85) == 1);  CHECK(dm::rssi_bars(-86) == 0);  CHECK(dm::rssi_bars(-99) == 0);

    // Text width: 5px glyph + 1px gap = 6 px/char, times the scale.
    CHECK(dm::text_w("",     2) == 0);
    CHECK(dm::text_w("A",    2) == 12);
    CHECK(dm::text_w("WiFi", 2) == 48);

    // SoC gradient: the five stops land on exact colours (mirrors soc_rgb()).
    int r, g, b;
    tk::soc_rgb(0,   r, g, b); CHECK(r == 231 && g ==  76 && b == 60);
    tk::soc_rgb(18,  r, g, b); CHECK(r == 240 && g == 190 && b == 40);
    tk::soc_rgb(45,  r, g, b); CHECK(r == 120 && g == 200 && b == 90);
    tk::soc_rgb(80,  r, g, b); CHECK(r ==  60 && g == 175 && b == 80);
    tk::soc_rgb(100, r, g, b); CHECK(r ==  30 && g == 140 && b == 60);
    // Out-of-range clamps to the endpoints, not past them.
    tk::soc_rgb(-5,  r, g, b); CHECK(r == 231 && g ==  76 && b == 60);
    tk::soc_rgb(150, r, g, b); CHECK(r ==  30 && g == 140 && b == 60);

    // SSID marquee ping-pong (span 10, pause 8, speed 2 → travel 5, period 26).
    CHECK(dm::scroll_offset(0,  10) == 0);    // paused at the start
    CHECK(dm::scroll_offset(7,  10) == 0);    // still in the start pause
    CHECK(dm::scroll_offset(10, 10) == 4);    // scrolling out
    CHECK(dm::scroll_offset(15, 10) == 10);   // paused at the end (fully revealed)
    CHECK(dm::scroll_offset(200, 0) == 0);    // nothing to scroll
}

static void test_display_model() {
    using LS = tk::LinkState;

    // Recovery overrides every vehicle/connectivity combination without showing charge or
    // pairing. It uses the full SSID row and only the existing marquee can animate it.
    for (LS link : {LS::Unknown, LS::Idle, LS::Awake, LS::Asleep, LS::Unreachable})
        for (auto net : {tk::NetLink::None, tk::NetLink::Wifi, tk::NetLink::Eth})
            for (bool ble : {false, true}) for (bool paired : {false, true})
                for (bool soc : {false, true}) for (bool eth : {false, true})
                    for (auto orient : {dm::Orient::Landscape, dm::Orient::Portrait}) {
                        auto s = us(link, true, ble, paired, soc, 55, true);
                        s.net = net; s.eth_present = eth;
                        const auto m = dm::compose(s, 0, orient, true);
                        CHECK(m.hero == dm::Hero::SafeMode);
                        CHECK(!m.show_ble && !m.ble_glyph_on && m.ble_bars == 0);
                        CHECK(m.soc == 0 && !m.asleep && !m.show_bolt);
                        CHECK(m.fill_r == 0 && m.fill_g == 0 && m.fill_b == 0);
                        CHECK(m.show_wifi == (net == tk::NetLink::Wifi));
                        CHECK(m.show_lan == (net == tk::NetLink::Eth));
                        CHECK(m.animating == m.ssid_scrolling);
                        if (net == tk::NetLink::Wifi)
                            CHECK(m.ssid_avail == (orient == dm::Orient::Portrait ? 72 : 130));
                    }

    // ── priority ladder: network search > pairing > BLE search > battery ──
    // No lease ⇒ NetSearch, whatever else is true.
    CHECK(dm::compose(us(LS::Awake, /*net*/false, /*ble*/true, /*paired*/true,
                         /*soc?*/true, 50, false), 0).hero == dm::Hero::NetSearch);
    // WiFi up, BLE link up but not yet paired ⇒ Pairing.
    CHECK(dm::compose(us(LS::Idle, true, /*ble*/true, /*paired*/false,
                         false, 0, false), 0).hero == dm::Hero::Pairing);
    // WiFi up, no BLE link, no usable SoC ⇒ BLE search.
    CHECK(dm::compose(us(LS::Unknown, true, /*ble*/false, false,
                         /*soc?*/false, 0, false), 0).hero == dm::Hero::BleSearch);
    // Unreachable overrides a stale cached SoC ⇒ still BLE search, never battery.
    CHECK(dm::compose(us(LS::Unreachable, true, false, true,
                         /*soc?*/true, 50, false), 0).hero == dm::Hero::BleSearch);
    // WiFi up, reachable, have SoC ⇒ Battery.
    CHECK(dm::compose(us(LS::Idle, true, true, true, true, 50, false), 0).hero == dm::Hero::Battery);

    // ── header indicator visibility (a search hero hides its own small indicator) ──
    dm::Model wifi_s = dm::compose(us(LS::Unknown, false, false, false, false, 0, false), 0);
    CHECK(!wifi_s.show_wifi && wifi_s.show_ble);          // WiFi hero: wifi hidden, ble shown
    dm::Model ble_s = dm::compose(us(LS::Unknown, true, false, false, false, 0, false), 0);
    CHECK(ble_s.show_wifi && !ble_s.show_ble);            // BLE hero: ble hidden, wifi shown
    dm::Model batt = dm::compose(us(LS::Idle, true, true, true, true, 50, false), 0);
    CHECK(batt.show_wifi && batt.show_ble);               // battery: both shown

    // ── SSID available width: wider when the BLE header is absent (BLE-search hero) ──
    CHECK(ble_s.ssid_avail == 130);                       // 158 - 28
    CHECK(batt.ssid_avail  ==  98);                       // 158 - 28 - 32

    // ── BLE header bars: 0 without a live RSSI, mapped when present ──
    tk::UiSnapshot rssi_snap = us(LS::Idle, true, true, true, true, 50, false);
    rssi_snap.ble_rssi_valid = true; rssi_snap.ble_rssi = -60;
    dm::Model rssi_m = dm::compose(rssi_snap, 0);
    CHECK(rssi_m.ble_bars == 3 && rssi_m.ble_glyph_on);
    CHECK(dm::compose(us(LS::Idle, true, true, true, true, 50, false), 0).ble_bars == 0);  // no RSSI

    // ── battery fill colour, SoC clamp, and the charging bolt ──
    dm::Model full = dm::compose(us(LS::Idle, true, true, true, true, 150, /*charging*/true), 0);
    CHECK(full.soc == 100);                               // clamped
    CHECK(!full.show_bolt);                               // no bolt at 100%
    dm::Model chg = dm::compose(us(LS::Idle, true, true, true, true, 80, /*charging*/true), 0);
    CHECK(chg.show_bolt);                                 // charging below 100% ⇒ bolt
    CHECK(chg.fill_r == 60 && chg.fill_g == 175 && chg.fill_b == 80);   // soc 80 → the 0.80 gradient stop

    // Asleep dims the fill toward the panel colour AND suppresses the bolt even when charging.
    dm::Model slp = dm::compose(us(LS::Asleep, true, true, true, true, 80, /*charging*/true), 0);
    CHECK(slp.asleep && !slp.show_bolt);
    int br, bg, bb; tk::soc_rgb(80, br, bg, bb);
    CHECK(slp.fill_r == dm::lerp8(br, dm::kAsleepDimR, 0.5f));
    CHECK(slp.fill_g == dm::lerp8(bg, dm::kAsleepDimG, 0.5f));
    CHECK(slp.fill_b == dm::lerp8(bb, dm::kAsleepDimB, 0.5f));

    // ── animating: search/pairing heroes always; battery only while the SSID scrolls ──
    CHECK(wifi_s.animating);
    CHECK(dm::compose(us(LS::Idle, true, true, false, false, 0, false), 0).animating);  // pairing
    CHECK(ble_s.animating);
    CHECK(!batt.animating);                               // short SSID ("") ⇒ static battery
    tk::UiSnapshot long_ssid = us(LS::Idle, true, true, true, true, 50, false);
    std::strcpy(long_ssid.ssid, "AVeryLongNetworkNameXYZ");   // > 98 px at scale 2 ⇒ marquee
    dm::Model scr = dm::compose(long_ssid, 10);
    CHECK(scr.ssid_scrolling && scr.animating);
    for (auto orient : {dm::Orient::Landscape, dm::Orient::Portrait}) {
        const auto recovery = dm::compose(long_ssid, 10, orient, true);
        CHECK(recovery.hero == dm::Hero::SafeMode && recovery.ssid_scrolling && recovery.animating);
    }


    // ── portrait orientation (BOOT-rotated 80x160) — only the SSID geometry differs ──
    // The SSID gets its own header row, full width (72px) at scale 1, independent of the BLE cluster.
    dm::Model p_batt = dm::compose(us(LS::Idle, true, true, true, true, 50, false), 0, dm::Orient::Portrait);
    dm::Model p_ble  = dm::compose(us(LS::Unknown, true, false, false, false, 0, false), 0, dm::Orient::Portrait);
    CHECK(p_batt.ssid_avail == 72 && p_ble.ssid_avail == 72);   // same avail with or without BLE indicator
    // A 12-char SSID fits portrait's dedicated row (72px at scale 1) but scrolls landscape (144px > 98 at scale 2).
    tk::UiSnapshot mid_ssid = us(LS::Idle, true, true, true, true, 50, false);
    std::strcpy(mid_ssid.ssid, "TwelveCharSS");                // 12 ch → 72px (portrait fits) / 144px (landscape scrolls)
    CHECK(!dm::compose(mid_ssid, 10, dm::Orient::Portrait).ssid_scrolling);
    CHECK( dm::compose(mid_ssid, 10, dm::Orient::Landscape).ssid_scrolling);
    tk::UiSnapshot long_p = us(LS::Idle, true, true, true, true, 50, false);
    std::strcpy(long_p.ssid, "ThirteenChars");                 // 13 ch → 78px > 72 ⇒ portrait marquee too
    CHECK(dm::compose(long_p, 10, dm::Orient::Portrait).ssid_scrolling);
    // Orientation changes ONLY the SSID geometry — the hero/battery decisions are identical.
    CHECK(p_batt.hero == batt.hero && p_batt.soc == batt.soc);
    CHECK(p_batt.fill_r == batt.fill_r && p_batt.fill_g == batt.fill_g && p_batt.fill_b == batt.fill_b);
}

// ─── status LED priority ladder (logic/led_status.hpp <- shared UiSnapshot + LedAlerts) ──
// The LED reads the SAME tk::UiSnapshot the display presenter does (one input contract), plus
// a tiny LED-only LedAlerts for its latched error/warn/OTA tiers. Base = healthy, parked
// (Idle), paired, 55 % SoC, network up, no alerts; each case flips one field to prove the ladder
// picks the right colour/animation and that a higher tier wins.
static tk::UiSnapshot led_snap() {
    tk::UiSnapshot s;
    s.net        = tk::NetLink::Wifi;
    s.link_state = tk::LinkState::Idle;
    s.paired     = true;
    s.have_soc   = true;
    s.soc        = 55;
    return s;
}
static void test_led() {
    using C = tk::LedColor;
    using A = tk::LedAnim;
    const tk::LedAlerts none;   // no latched alerts

    // Parked with a known SoC → dimmed gradient, steady.
    CHECK(tk::led_pattern(led_snap(), none) == (tk::LedPattern{C::SocGradient, A::Solid, true}));
    // Full charge reads as steady green (still dim/resting).
    { auto s = led_snap(); s.soc = 100; CHECK(tk::led_pattern(s, none) == (tk::LedPattern{C::Green, A::Solid, true})); }
    // Awake is also a "have live reading" state → gradient, not search.
    { auto s = led_snap(); s.link_state = tk::LinkState::Awake; CHECK(tk::led_pattern(s, none).color == C::SocGradient); }

    // Charging outranks the static SoC display (green swell).
    { auto s = led_snap(); s.link_state = tk::LinkState::Awake; s.charging = true; CHECK(tk::led_pattern(s, none) == (tk::LedPattern{C::Green, A::Breathe, false})); }

    // Cached charging never promotes a sleeping, uncertain or merely reachable vehicle.
    for (auto link : {tk::LinkState::Asleep, tk::LinkState::Unknown,
                      tk::LinkState::Unreachable, tk::LinkState::Idle}) {
        auto snapshot = led_snap(); snapshot.link_state = link;
        const auto resting = tk::led_pattern(snapshot, none);
        snapshot.charging = true;
        CHECK(tk::led_pattern(snapshot, none) == resting);
    }
    { auto snapshot = led_snap(); snapshot.link_state = tk::LinkState::Awake;
      snapshot.have_soc = false; snapshot.charging = true;
      CHECK(tk::led_pattern(snapshot, none).color == C::Teal); }

    // Asleep → LED off entirely.
    { auto s = led_snap(); s.link_state = tk::LinkState::Asleep; s.have_soc = false;
      CHECK(tk::led_pattern(s, none) == (tk::LedPattern{C::Off, A::Off, false})); }

    // Pairing: BLE up but no session → magenta pulse. Outranks charging/SoC.
    { auto s = led_snap(); s.paired = false; s.ble_connected = true; s.charging = true;
      CHECK(tk::led_pattern(s, none) == (tk::LedPattern{C::Magenta, A::Pulse, false})); }

    // Network search: no LAN → blue breathe, and it outranks pairing/charging below it.
    // Transport-blind by design: an Ethernet board with no lease must reach the SAME tier.
    { auto s = led_snap(); s.net = tk::NetLink::None; s.ble_connected = true; s.paired = false;
      CHECK(tk::led_pattern(s, none) == (tk::LedPattern{C::Blue, A::Breathe, false})); }
    { auto s = led_snap(); s.net = tk::NetLink::None; s.eth_present = true;
      CHECK(tk::led_pattern(s, none) == (tk::LedPattern{C::Blue, A::Breathe, false})); }

    // Searching for the car: reachable readings absent → teal breathe.
    { auto s = led_snap(); s.link_state = tk::LinkState::Unreachable;
      CHECK(tk::led_pattern(s, none) == (tk::LedPattern{C::Teal, A::Breathe, false})); }
    // A stale SoC while Unreachable must NOT show as a live gradient — still searching.
    { auto s = led_snap(); s.link_state = tk::LinkState::Unreachable; s.have_soc = true; s.soc = 80;
      CHECK(tk::led_pattern(s, none).color == C::Teal); }

    // ── LED-only latched alerts (top of the ladder), passed via LedAlerts ──
    // Warning (repeated connect failures) → amber blink, above WiFi/pairing.
    { auto s = led_snap(); s.net = tk::NetLink::None; tk::LedAlerts a; a.warn = true;
      CHECK(tk::led_pattern(s, a) == (tk::LedPattern{C::Amber, A::Blink, false})); }
    // OTA download → blue pulse, above the warning.
    { tk::LedAlerts a; a.ota_downloading = true; a.warn = true;
      CHECK(tk::led_pattern(led_snap(), a) == (tk::LedPattern{C::Blue, A::Pulse, false})); }
    // Error is the top of the ladder — beats OTA, warn, charging, everything.
    { auto s = led_snap(); s.charging = true; tk::LedAlerts a; a.error = true; a.ota_downloading = true; a.warn = true;
      CHECK(tk::led_pattern(s, a) == (tk::LedPattern{C::Red, A::Blink, false})); }
}

static void test_led_driver() {
    using namespace tk;
    for (bool ota : {false, true}) for (auto net : {NetLink::None, NetLink::Wifi, NetLink::Eth}) {
        LedLatches recovery{};
        const auto alerts = led_sample_alerts(recovery, 100, false, false, ota, 0, true);
        CHECK(alerts.error && !alerts.warn);
        auto snapshot = us(LinkState::Awake, true, true, true, true, 55, true);
        snapshot.net = net;
        const auto pattern = led_pattern(snapshot, alerts);
        CHECK(pattern.color == LedColor::Red && pattern.anim == LedAnim::Blink && !pattern.dim);
        CHECK(led_sample_alerts(recovery, 15'000'099, false, false, false, 0).error);
        CHECK(!led_sample_alerts(recovery, 15'000'100, false, false, false, 0).error);
        CHECK(led_sample_alerts(recovery, UINT64_MAX, false, false, false, 0, true).error);
    }
    LedLatches state{};
    CHECK(!led_sample_alerts(state, 0, false, false, false, 2).error);
    CHECK(!led_sample_alerts(state, 0, false, false, false, 2).warn);
    // Normal-mode error causes refresh the same 15s hold; warnings have an independent 10s hold.
    for (const bool ota_error : {false, true}) {
        state = {};
        auto alerts = led_sample_alerts(state, 100, !ota_error, ota_error, false, 3);
        CHECK(alerts.error && alerts.warn && !alerts.ota_downloading);
        alerts = led_sample_alerts(state, 10'000'099, false, false, true, 2);
        CHECK(alerts.error && alerts.warn && alerts.ota_downloading);
        alerts = led_sample_alerts(state, 10'000'100, false, false, false, 0);
        CHECK(alerts.error && !alerts.warn && !alerts.ota_downloading);
        CHECK(led_sample_alerts(state, 15'000'099, false, false, false, 0).error);
        CHECK(!led_sample_alerts(state, 15'000'100, false, false, false, 0).error);
    }
    // Repeated faults extend the hold; intervening healthy samples do not clear it early.
    for (uint64_t cycle = 0; cycle < 3; ++cycle) {
        const uint64_t start = 20'000'000 + cycle * 40'000'000;
        CHECK(led_sample_alerts(state, start, true, false, false, 3).error);
        CHECK(led_sample_alerts(state, start + 5'000'000, true, false, false, 3).warn);
        CHECK(led_sample_alerts(state, start + 15'000'000, false, false, false, 0).error);
        CHECK(!led_sample_alerts(state, start + 15'000'000, false, false, false, 0).warn);
        CHECK(!led_sample_alerts(state, start + 20'000'000, false, false, false, 0).error);
    }
    state = {};
    CHECK(led_sample_alerts(state, UINT64_MAX, true, false, false, UINT32_MAX).error);
    CHECK(state.error_until == UINT64_MAX && state.warn_until == UINT64_MAX);

    const LedRgb red = led_color_rgb(LedColor::Red, 55);
    CHECK(red.r == 255 && red.g == 0 && red.b == 0);
    const auto low = led_color_rgb(LedColor::SocGradient, -100);
    const auto high = led_color_rgb(LedColor::SocGradient, 200);
    CHECK(low.r == 231 && low.g == 76 && low.b == 60);
    CHECK(high.r == 30 && high.g == 140 && high.b == 60);
    const auto dim = led_frame_rgb({LedColor::Green, LedAnim::Solid, true}, 55, 0);
    CHECK(dim.r == 0 && dim.g == 66 && dim.b == 12);
    const auto off = led_frame_rgb({LedColor::Red, LedAnim::Off, false}, 55, 0);
    CHECK(off.r == 0 && off.g == 0 && off.b == 0);
    CHECK(led_animation_factor(LedAnim::Blink, 0.0f) == 1.0f);
    CHECK(led_animation_factor(LedAnim::Blink, 0.449f) == 1.0f);
    CHECK(led_animation_factor(LedAnim::Blink, 0.45f) == 0.0f);
    CHECK(led_animation_factor(LedAnim::Blink, 0.9f) == 1.0f);
    CHECK(std::fabs(led_animation_factor(LedAnim::Breathe, 0.65f) - 1.0f) < 0.00001f);
    CHECK(std::fabs(led_animation_factor(LedAnim::Breathe, 1.95f) - 0.15f) < 0.00001f);
    CHECK(std::fabs(led_animation_factor(LedAnim::Pulse, 0.0f) - 0.08f) < 0.00001f);
    CHECK(std::fabs(led_animation_factor(LedAnim::Pulse, 0.132f) - 1.0f) < 0.00001f);
    CHECK(std::fabs(led_animation_factor(LedAnim::Pulse, 0.66f) - 0.08f) < 0.00001f);
    for (const auto animation : {LedAnim::Off, LedAnim::Solid, LedAnim::Breathe,
                                  LedAnim::Pulse, LedAnim::Blink}) {
        for (int tick = 0; tick < 10'000; ++tick) {
            const float factor = led_animation_factor(animation, tick * 0.033f);
            CHECK(std::isfinite(factor) && factor >= 0 && factor <= 1);
        }
    }
    for (const int brightness : {-100, 0, 1, 8, 31, 32, 100}) {
        for (int channel = 0; channel < 256; ++channel) {
            const LedRgb rgb{static_cast<uint8_t>(channel), 0x65, 0xa7};
            const auto frame = led_apa102_frame(rgb, brightness);
            CHECK(frame.size() == 12);
            CHECK(frame[0] == 0 && frame[1] == 0 && frame[2] == 0 && frame[3] == 0);
            CHECK(frame[4] == (brightness < 1 ? 0xe1 : brightness > 31 ? 0xff : 0xe0 | brightness));
            CHECK(frame[5] == 0xa7 && frame[6] == 0x65 && frame[7] == channel);
            CHECK(frame[8] == 0xff && frame[9] == 0xff && frame[10] == 0xff && frame[11] == 0xff);
        }
    }
}

// ── heap-exhaustion watchdog (logic/heap_watchdog.hpp) ────────────────────────────────────────
// The escalation that was missing on 2026-07-18, when a permanently exhausted heap left the
// device wedged for ten hours instead of restarting.
static void test_heap_watchdog() {
    using namespace tk;
    const uint32_t HOLD = kHeapCriticalHoldMs;

    // Pin the five-minute policy with wall-time samples independent of the configured constant.
    CHECK(kHeapCriticalHoldMs == 300000);
    HeapWatchdog fixed_hold;
    CHECK(heap_watch(fixed_hold, {768, 0, false}).action == HeapAction::Armed);
    CHECK(heap_watch(fixed_hold, {768, 30000, false}).action == HeapAction::Watching);
    CHECK(heap_watch(fixed_hold, {768, 299999, false}).action == HeapAction::Watching);
    CHECK(heap_watch(fixed_hold, {768, 300000, false}).action == HeapAction::Restart);
    HeapWatchdog fixed_recovery;
    CHECK(heap_watch(fixed_recovery, {768, 0, false}).action == HeapAction::Armed);
    CHECK(heap_watch(fixed_recovery, {31744, 299999, false}).action == HeapAction::Recovered);
    CHECK(heap_watch(fixed_recovery, {768, 300000, false}).action == HeapAction::Armed);

    // A healthy heap never arms anything, however long it runs, and says nothing while it stays
    // healthy — Ok is the "no line in syslog" verdict.
    HeapWatchdog h;
    for (uint32_t t = 0; t < 3 * HOLD; t += 30000) {
        HeapVerdict v = heap_watch(h, {31744, t, false});
        CHECK(v.action == HeapAction::Ok);
        CHECK(v.critical_ms == 0);
    }

    // Sustained exhaustion: the run is ARMED once (the line that opens the countdown), watched
    // until the hold elapses, then restart. The boundary is inclusive — at exactly
    // kHeapCriticalHoldMs we act.
    HeapWatchdog w;
    CHECK(heap_watch(w, {768, 0,        false}).action == HeapAction::Armed);
    CHECK(heap_watch(w, {768, HOLD / 2, false}).action == HeapAction::Watching);
    CHECK(heap_watch(w, {768, HOLD - 1, false}).action == HeapAction::Watching);
    CHECK(heap_watch(w, {768, HOLD,     false}).action == HeapAction::Restart);

    // Only the FIRST critical sample arms; the rest are Watching. Otherwise the log would claim
    // a fresh countdown every 30 s and a reader could not tell a sustained run from a flapping one.
    HeapWatchdog a;
    CHECK(heap_watch(a, {768, 0,     false}).action == HeapAction::Armed);
    CHECK(heap_watch(a, {768, 1000,  false}).action == HeapAction::Watching);
    CHECK(heap_watch(a, {768, 2000,  false}).action == HeapAction::Watching);

    // The elapsed time reported is the TRUE age of the run, not the configured hold — the log line
    // must state what actually happened. Sampling on a cadence means a fired run has usually run
    // somewhat longer than the threshold, and printing the constant would hide exactly that.
    HeapWatchdog m;
    heap_watch(m, {768, 1000, false});
    CHECK(heap_watch(m, {768, 1000 + 90000, false}).critical_ms == 90000);
    HeapVerdict late = heap_watch(m, {768, 1000 + HOLD + 27000, false});
    CHECK(late.action == HeapAction::Restart);
    CHECK(late.critical_ms == HOLD + 27000);          // reported, not rounded down to HOLD

    // The countdown the "restarting in N s" line prints, and its saturation: a run that overshoots
    // the hold must print 0, never wrap to ~49 days.
    CHECK(heap_restart_in_ms(0)          == HOLD);
    CHECK(heap_restart_in_ms(30000)      == HOLD - 30000);
    CHECK(heap_restart_in_ms(HOLD)       == 0);
    CHECK(heap_restart_in_ms(HOLD + 999) == 0);

    // THE property that keeps this from becoming a reboot loop: the run must be UNBROKEN. A
    // single recovered sample resets the clock, so a device that dips and recovers — the
    // transient every other OOM guard exists to absorb — never restarts. Recovery reports how
    // long the run lasted, so syslog can close it with a fact instead of falling silent.
    HeapWatchdog b;
    CHECK(heap_watch(b, {768,   0,        false}).action == HeapAction::Armed);
    CHECK(heap_watch(b, {768,   HOLD - 1, false}).action == HeapAction::Watching);
    HeapVerdict rec = heap_watch(b, {31744, HOLD, false});
    CHECK(rec.action == HeapAction::Recovered);
    CHECK(rec.critical_ms == HOLD);       // how long it had been critical
    CHECK(!rec.ota_excused);              // the heap itself healed
    CHECK(heap_watch(b, {31744, HOLD + 1,     false}).action == HeapAction::Ok);  // stays quiet now
    CHECK(heap_watch(b, {768,   HOLD + 2,     false}).action == HeapAction::Armed);
    CHECK(heap_watch(b, {768,   2 * HOLD,     false}).action == HeapAction::Watching);
    CHECK(heap_watch(b, {768,   2 * HOLD + 2, false}).action == HeapAction::Restart);

    // The threshold boundary: at exactly kHeapCriticalBytes we are still healthy.
    HeapWatchdog e;
    CHECK(heap_watch(e, {kHeapCriticalBytes,     0, false}).action == HeapAction::Ok);
    CHECK(heap_watch(e, {kHeapCriticalBytes - 1, 0, false}).action == HeapAction::Armed);

    // OTA cannot reset the continuous critical hold. The actual supervisor separately
    // applies writer grace and requires raw-writer abort/owner acknowledgement before effects.
    HeapWatchdog o;
    CHECK(heap_watch(o, {768, 0, false}).action == HeapAction::Armed);
    const auto held_during_ota = heap_watch(o, {768, HOLD - 1, true});
    CHECK(held_during_ota.action == HeapAction::Watching && !held_during_ota.ota_excused);
    CHECK(held_during_ota.critical_ms == HOLD - 1);
    CHECK(heap_watch(o, {768, HOLD, true}).action == HeapAction::Restart);
    CHECK(heap_watch(o, {768, HOLD + 1, false}).action == HeapAction::Restart);
    CHECK(heap_watch(o, {768, 2 * HOLD, true}).action == HeapAction::Restart);

    // An OTA running while the heap is HEALTHY again is an ordinary recovery, not an excusal —
    // the two get different sentences in the log, so the distinction has to hold.
    HeapWatchdog oh;
    CHECK(heap_watch(oh, {768, 0, false}).action == HeapAction::Armed);
    HeapVerdict healed = heap_watch(oh, {31744, 60000, true});
    CHECK(healed.action == HeapAction::Recovered);
    CHECK(!healed.ota_excused);

    // A 32-bit millisecond wrap inside the hold window must measure true elapsed time, not go
    // hugely negative and fire instantly (uptime crosses this after ~49 days).
    HeapWatchdog p;
    const uint32_t near_wrap = 0xFFFFFFFFu - (HOLD / 2);
    CHECK(heap_watch(p, {768, near_wrap, false}).action == HeapAction::Armed);
    HeapVerdict mid = heap_watch(p, {768, (uint32_t)(near_wrap + HOLD / 4), false});
    CHECK(mid.action == HeapAction::Watching);
    CHECK(mid.critical_ms == HOLD / 4);        // measured across the wrap, not a huge bogus number
    CHECK(heap_watch(p, {768, (uint32_t)(near_wrap + HOLD), false}).action == HeapAction::Restart);

    // ── The restart cap: a loop that isn't fixing anything must stop ──────────────────────
    // Restarting is only worth it while it might help. Five consecutive proves it doesn't, and
    // continuing would re-seed the polling window every ~10 min forever, so a parked car next to
    // it could never sleep.
    for (uint8_t n = 0; n < kHeapMaxConsecutiveRestarts; n++) CHECK(heap_may_restart(n));
    CHECK(!heap_may_restart(kHeapMaxConsecutiveRestarts));
    CHECK(!heap_may_restart(255));

    // ── The NVS breadcrumb round-trips ───────────────────────────────────────────────────
    for (uint8_t n : {(uint8_t)0, (uint8_t)1, (uint8_t)5, (uint8_t)9, (uint8_t)10,
                      (uint8_t)99, (uint8_t)100, (uint8_t)255}) {
        CHECK(heap_reason_parse(heap_reason_format(n).text) == n);
    }
    CHECK_STR(heap_reason_format(1).text,   "heap:1");
    CHECK_STR(heap_reason_format(12).text,  "heap:12");
    CHECK_STR(heap_reason_format(255).text, "heap:255");

    // These are numeric-parser cases only. The storage consumer separately maps present-invalid
    // values and read/clear errors to the restart cap; parser zero is not NVS error authority.
    CHECK(heap_reason_parse(nullptr)     == 0);
    CHECK(heap_reason_parse("")          == 0);
    CHECK(heap_reason_parse("heap")      == 0);   // no separator/count at all
    CHECK(heap_reason_parse("heap:")     == 0);   // separator but no digits
    CHECK(heap_reason_parse("heap:x")    == 0);   // non-numeric
    CHECK(heap_reason_parse("heap:1x")   == 0);   // trailing garbage
    CHECK(heap_reason_parse("panic:3")   == 0);   // a different writer's reason
    CHECK(heap_reason_parse("heap:9999") == 255); // saturates instead of wrapping to a small n
}

// ── request-body reassembly (logic/http_body.hpp) — the multi-segment truncation fix ──────────
static void test_http_body() {
    using namespace tk;
    char buf[64];

    // Whole body in one recv.
    {
        std::string body = "{\"amps\":16}";
        int r = http_body_read(buf, sizeof(buf), body.size(),
            [&](char* dst, size_t len) -> BodyChunk { std::memcpy(dst, body.data(), len); return { BodyRecv::Data, len }; });
        CHECK(r == (int)body.size());
        CHECK(std::string(buf) == body);
    }
    // Delivered ONE byte at a time (the segmentation the old single-recv truncated).
    {
        std::string body = "{\"broker\":\"host:1883\"}";
        size_t off = 0;
        int r = http_body_read(buf, sizeof(buf), body.size(),
            [&](char* dst, size_t) -> BodyChunk { dst[0] = body[off++]; return { BodyRecv::Data, 1 }; });
        CHECK(r == (int)body.size());
        CHECK(std::string(buf) == body);
    }
    // A mid-body peer error / close → fail (no truncated result handed back).
    {
        int r = http_body_read(buf, sizeof(buf), 10,
            [&](char* dst, size_t) -> BodyChunk { dst[0] = 'x'; return { BodyRecv::Error, 0 }; });
        CHECK(r == -1);
    }
    // Bounded timeouts: BODY_MAX_IDLE consecutive timeouts are tolerated, one more abandons it.
    {
        int calls = 0;
        int r = http_body_read(buf, sizeof(buf), 4,
            [&](char*, size_t) -> BodyChunk { ++calls; return { BodyRecv::Timeout, 0 }; });
        CHECK(r == -1);
        CHECK(calls == BODY_MAX_IDLE + 1);   // gives up after one too many
    }
    // Body too large for the buffer (no room for the terminator) → fail, never overflow.
    CHECK(http_body_read(buf, sizeof(buf), sizeof(buf), [&](char*, size_t) -> BodyChunk { return { BodyRecv::Data, 0 }; }) == -1);
    // Empty / null guards.
    CHECK(http_body_read(buf, sizeof(buf), 0, [&](char*, size_t) -> BodyChunk { return { BodyRecv::Data, 0 }; }) == -1);
    CHECK(http_body_read(nullptr, 8, 4, [&](char*, size_t) -> BodyChunk { return { BodyRecv::Data, 0 }; }) == -1);

    // Progress cannot renew the total budget, with or without recoverable timeouts.
    // Exercise the actual reader over the full production body size without real sleeps.
    for (int timeouts_per_byte : {0, BODY_MAX_IDLE}) {
        char slow[2049];
        std::memset(slow, '!', sizeof(slow));
        uint64_t ms = 0;
        int pending_timeouts = timeouts_per_byte;
        int calls = 0;
        int received = http_body_read(slow, sizeof(slow), sizeof(slow) - 1,
            [&](char* dst, size_t) -> BodyChunk {
                ++calls;
                if (pending_timeouts > 0) {
                    --pending_timeouts;
                    ms += BODY_RECV_TIMEOUT_SECONDS * 1000;
                    return {BodyRecv::Timeout, 0};
                }
                ms += 4900;
                *dst = 'x';
                pending_timeouts = timeouts_per_byte;
                return {BodyRecv::Data, 1};
            }, [&]() -> uint64_t { return ms; });
        CHECK(received == -1);
        CHECK(calls == 4);
        CHECK(ms >= BODY_MAX_DURATION_MS);
        CHECK(ms < BODY_MAX_DURATION_MS + BODY_RECV_TIMEOUT_SECONDS * 1000);
        CHECK(slow[sizeof(slow) - 1] == '!');
    }
    // The final byte must itself arrive before the deadline; late completeness is failure.
    for (uint64_t elapsed : {BODY_MAX_DURATION_MS - 1, BODY_MAX_DURATION_MS,
                             BODY_MAX_DURATION_MS + 1}) {
        uint64_t ms = UINT64_MAX - 10000;  // unsigned elapsed arithmetic also survives rollover
        int r = http_body_read(buf, sizeof(buf), 1,
            [&](char* dst, size_t) -> BodyChunk {
                ms += elapsed;
                *dst = 'x';
                return {BodyRecv::Data, 1};
            }, [&]() -> uint64_t { return ms; });
        CHECK(r == (elapsed < BODY_MAX_DURATION_MS ? 1 : -1));
    }
    // If time runs out between receives, do not start another blocking socket call.
    {
        int clock_calls = 0, recv_calls = 0;
        int r = http_body_read(buf, sizeof(buf), 2,
            [&](char* dst, size_t) -> BodyChunk {
                ++recv_calls; *dst = 'x'; return {BodyRecv::Data, 1};
            }, [&]() -> uint64_t {
                return ++clock_calls < 4 ? 0 : BODY_MAX_DURATION_MS;
            });
        CHECK(r == -1);
        CHECK(recv_calls == 1);
    }
    // Allocation ownership is unchanged when a complete body arrives too late.
    {
        int frees = 0;
        uint64_t ms = 0;
        auto r = http_body_receive(4, 8,
            [](size_t n) -> void* { return std::malloc(n); },
            [&](void* p) { ++frees; std::free(p); },
            [&](char* dst, size_t n) -> BodyChunk {
                std::memset(dst, 'x', n); ms = BODY_MAX_DURATION_MS;
                return {BodyRecv::Data, n};
            }, [&]() -> uint64_t { return ms; });
        CHECK(r.status == BodyReadStatus::ReceiveFailed);
        CHECK(r.data == nullptr);
        CHECK(frees == 1);
    }

    // Captive-portal production accepts exactly 1024 wire bytes in fixed storage with a separate
    // terminator byte. The next wire byte is rejected before recv.
    {
        constexpr size_t max_payload = 1024;
        constexpr size_t capacity = max_payload + 1;
        char fixed[capacity]{};
        CHECK(!http_body_fits_buffer(0, capacity));
        CHECK(http_body_fits_buffer(1, capacity));
        CHECK(http_body_fits_buffer(max_payload, capacity));
        CHECK(!http_body_fits_buffer(max_payload + 1, capacity));

        int recv_calls = 0;
        CHECK(http_body_read(fixed, capacity, max_payload + 1,
            [&](char*, size_t) -> BodyChunk {
                ++recv_calls;
                return {BodyRecv::Data, 1};
            }) == -1);
        CHECK(recv_calls == 0);

        const std::string largest(max_payload, 'x');
        CHECK(http_body_read(fixed, capacity, largest.size(),
            [&](char* dst, size_t wanted) -> BodyChunk {
                std::memcpy(dst, largest.data(), wanted);
                return {BodyRecv::Data, wanted};
            }) == static_cast<int>(largest.size()));
        CHECK(fixed[max_payload] == '\0');
        CHECK(!http_body_has_embedded_nul(fixed, largest.size()));
    }
    {
        const char embedded[] = {'a', '\0', 'b', '\0'};
        CHECK(http_body_has_embedded_nul(embedded, 3));
        CHECK(!http_body_has_embedded_nul(embedded, 1));
        CHECK(!http_body_has_embedded_nul(nullptr, 0));
        CHECK(http_body_has_embedded_nul(nullptr, 1));
    }

    // The allocation-owning adapter keeps every failure class distinct. Persisted configuration
    // handlers rely on this: only Ok proceeds to JSON parsing, while Empty/recv/OOM can never be
    // mistaken for an explicitly submitted empty string that disables a service.
    {
        int allocs = 0;
        auto r = http_body_receive(0, 8,
            [&](size_t n) -> void* { ++allocs; return std::malloc(n); },
            [](void* p) { std::free(p); },
            [](char*, size_t) -> BodyChunk { return {BodyRecv::Error, 0}; });
        CHECK(r.status == BodyReadStatus::Empty);
        CHECK(r.data == nullptr);
        CHECK(allocs == 0);
        CHECK(body_read_http_status(r.status) == 400);
    }
    {
        int allocs = 0;
        auto r = http_body_receive(9, 8,
            [&](size_t n) -> void* { ++allocs; return std::malloc(n); },
            [](void* p) { std::free(p); },
            [](char*, size_t) -> BodyChunk { return {BodyRecv::Error, 0}; });
        CHECK(r.status == BodyReadStatus::TooLarge);
        CHECK(allocs == 0);
        CHECK(body_read_http_status(r.status) == 413);
    }
    {
        auto r = http_body_receive(4, 8,
            [](size_t) -> void* { return nullptr; },
            [](void*) {},
            [](char*, size_t) -> BodyChunk { return {BodyRecv::Error, 0}; });
        CHECK(r.status == BodyReadStatus::NoMemory);
        CHECK(body_read_http_status(r.status) == 503);
    }
    {
        int releases = 0;
        auto r = http_body_receive(4, 8,
            [](size_t n) -> void* { return std::malloc(n); },
            [&](void* p) { ++releases; std::free(p); },
            [](char*, size_t) -> BodyChunk { return {BodyRecv::Error, 0}; });
        CHECK(r.status == BodyReadStatus::ReceiveFailed);
        CHECK(r.data == nullptr);
        CHECK(releases == 1);
        CHECK(body_read_http_status(r.status) == 400);
    }
    {
        const std::string body = "{\"broker\":\"\"}";  // explicit disable remains a valid body
        size_t offset = 0;
        int releases = 0;
        auto r = http_body_receive(body.size(), 64,
            [](size_t n) -> void* { return std::malloc(n); },
            [&](void* p) { ++releases; std::free(p); },
            [&](char* dst, size_t) -> BodyChunk {
                dst[0] = body[offset++];
                return {BodyRecv::Data, 1};
            });
        CHECK(r.status == BodyReadStatus::Ok);
        CHECK(r.data != nullptr);
        CHECK(std::string(r.data) == body);
        CHECK(releases == 0);
        std::free(r.data);
    }
}

static void test_json_materialize() {
    using namespace tk;

    int64_t safe_integer = 0;
    CHECK(json_safe_integer(0.0, safe_integer) && safe_integer == 0);
    CHECK(json_safe_integer(-0.0, safe_integer) && safe_integer == 0);
    CHECK(json_safe_integer(9007199254740991.0, safe_integer) &&
          safe_integer == kJsonSafeIntegerMax);
    CHECK(json_safe_integer(-9007199254740991.0, safe_integer) &&
          safe_integer == -kJsonSafeIntegerMax);
    CHECK(!json_safe_integer(9007199254740992.0, safe_integer));  // +2^53
    CHECK(!json_safe_integer(-9007199254740992.0, safe_integer)); // -2^53
    CHECK(!json_safe_integer(7.5, safe_integer));
    CHECK(!json_safe_integer(-0.5, safe_integer));
    CHECK(!json_safe_integer(std::nan(""), safe_integer));
    CHECK(!json_safe_integer(HUGE_VAL, safe_integer));

    static constexpr char max_safe_lexeme[] = "9007199254740991";
    static constexpr char min_safe_lexeme[] = "-9007199254740991";
    CHECK(json_canonical_safe_integer(max_safe_lexeme,
                                      max_safe_lexeme + sizeof(max_safe_lexeme) - 1,
                                      safe_integer) &&
          safe_integer == kJsonSafeIntegerMax);
    CHECK(json_canonical_safe_integer(min_safe_lexeme,
                                      min_safe_lexeme + sizeof(min_safe_lexeme) - 1,
                                      safe_integer) &&
          safe_integer == -kJsonSafeIntegerMax);
    for (const char* invalid_integer : {
             "9007199254740992", "-9007199254740992", "1.5", "1e3", "1e-400",
             "9007199254740990.5", "-0", "01"}) {
        CHECK(!json_canonical_safe_integer(
            invalid_integer, invalid_integer + std::strlen(invalid_integer), safe_integer));
    }

    for (const char* valid_id : {
             "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"ping\"}",
             "{\"jsonrpc\":\"2.0\",\"id\":9007199254740991,\"method\":\"ping\"}",
             "{\"jsonrpc\":\"2.0\",\"id\":-9007199254740991,\"method\":\"ping\"}",
             "{\"jsonrpc\":\"2.0\",\"\\u0069d\":17,\"method\":\"ping\"}",
             "{\"jsonrpc\":\"2.0\",\"i\\u0064\":18,\"method\":\"ping\"}",
             "{\"jsonrpc\":\"2.0\",\"\\u0069\\u0064\":19,\"method\":\"ping\"}"}) {
        const JsonRawNumberId raw =
            json_top_level_numeric_id(valid_id, std::strlen(valid_id));
        CHECK(raw.status == JsonRawNumberStatus::ValidInteger);
    }
    for (const char* invalid_id : {
             "{\"id\":9007199254740992}", "{\"id\":1.5}", "{\"id\":1e-400}",
             "{\"id\":1e3}", "{\"id\":9007199254740990.5}", "{\"id\":-0}",
             "{\"\\u0069d\":1e3}"}) {
        const JsonRawNumberId raw =
            json_top_level_numeric_id(invalid_id, std::strlen(invalid_id));
        CHECK(raw.status == JsonRawNumberStatus::InvalidNumber);
    }
    const char* string_id = "{\"id\":\"request-1\"}";
    CHECK(json_top_level_numeric_id(string_id, std::strlen(string_id)).status ==
          JsonRawNumberStatus::NonNumber);
    const char* escaped_string_id = "{\"\\u0069\\u0064\":\"request-2\"}";
    CHECK(json_top_level_numeric_id(escaped_string_id, std::strlen(escaped_string_id)).status ==
          JsonRawNumberStatus::NonNumber);
    const char* lookalike_id = "{\"\\u0131d\":20}";
    CHECK(json_top_level_numeric_id(lookalike_id, std::strlen(lookalike_id)).status ==
          JsonRawNumberStatus::Missing);

    for (const char* valid : {
             "{}", "{\"broker\":\"\"}", " { \"ms\" : 1.7e+12 } \n",
             "[true,false,null,-1,0,3.14]", "{\"escaped\":\"a\\\\b\\\"c\\u20ac\"}",
             "{\"astral\":\"\\uD834\\uDD1E\"}"}) {
        CHECK(json_syntax_valid(valid, std::strlen(valid)));
    }
    for (const char* invalid : {
             "", "{", "{\"broker\"}", "{\"broker\":}", "{\"broker\":\"x\",}",
             "{\"x\":01}", "{\"x\":1.}", "{\"x\":1e}", "{\"x\":\"\\q\"}",
             "true false", "[1,]", "{\"x\":\"\\uD800\"}",
             "{\"x\":\"\\uDC00\"}", "{\"x\":\"\\uD800\\u0041\"}",
             "{\"x\":\"\\uD800x\\uDC00\"}"}) {
        CHECK(!json_syntax_valid(invalid, std::strlen(invalid)));
    }

    struct RawUtf8 {
        const char* bytes;
        size_t length;
    };
    static constexpr char utf8_2_min[] = "\xc2\x80";
    static constexpr char utf8_2_max[] = "\xdf\xbf";
    static constexpr char utf8_3_min[] = "\xe0\xa0\x80";
    static constexpr char utf8_before_surrogate[] = "\xed\x9f\xbf";
    static constexpr char utf8_after_surrogate[] = "\xee\x80\x80";
    static constexpr char utf8_4_min[] = "\xf0\x90\x80\x80";
    static constexpr char utf8_4_max[] = "\xf4\x8f\xbf\xbf";
    const RawUtf8 valid_utf8[] = {
        {utf8_2_min, sizeof(utf8_2_min) - 1},
        {utf8_2_max, sizeof(utf8_2_max) - 1},
        {utf8_3_min, sizeof(utf8_3_min) - 1},
        {utf8_before_surrogate, sizeof(utf8_before_surrogate) - 1},
        {utf8_after_surrogate, sizeof(utf8_after_surrogate) - 1},
        {utf8_4_min, sizeof(utf8_4_min) - 1},
        {utf8_4_max, sizeof(utf8_4_max) - 1},
    };
    for (const RawUtf8& raw : valid_utf8) {
        const std::string document =
            std::string("{\"x\":\"") + std::string(raw.bytes, raw.length) + "\"}";
        CHECK(json_syntax_status(document.data(), document.size()) == JsonSyntaxStatus::Valid);
    }

    static constexpr char lone_continuation[] = "\x80";
    static constexpr char bad_two_byte_lead[] = "\xc0\x80";
    static constexpr char truncated_two_byte[] = "\xc2";
    static constexpr char bad_two_byte_tail[] = "\xc2\x20";
    static constexpr char truncated_three_byte[] = "\xe2\x82";
    static constexpr char overlong_three_byte[] = "\xe0\x80\x80";
    static constexpr char high_surrogate[] = "\xed\xa0\x80";
    static constexpr char low_surrogate[] = "\xed\xbf\xbf";
    static constexpr char truncated_four_byte[] = "\xf0\x90\x80";
    static constexpr char overlong_four_byte[] = "\xf0\x80\x80\x80";
    static constexpr char above_unicode[] = "\xf4\x90\x80\x80";
    static constexpr char bad_four_byte_lead[] = "\xf5\x80\x80\x80";
    static constexpr char impossible_lead[] = "\xff";
    const RawUtf8 invalid_utf8[] = {
        {lone_continuation, sizeof(lone_continuation) - 1},
        {bad_two_byte_lead, sizeof(bad_two_byte_lead) - 1},
        {truncated_two_byte, sizeof(truncated_two_byte) - 1},
        {bad_two_byte_tail, sizeof(bad_two_byte_tail) - 1},
        {truncated_three_byte, sizeof(truncated_three_byte) - 1},
        {overlong_three_byte, sizeof(overlong_three_byte) - 1},
        {high_surrogate, sizeof(high_surrogate) - 1},
        {low_surrogate, sizeof(low_surrogate) - 1},
        {truncated_four_byte, sizeof(truncated_four_byte) - 1},
        {overlong_four_byte, sizeof(overlong_four_byte) - 1},
        {above_unicode, sizeof(above_unicode) - 1},
        {bad_four_byte_lead, sizeof(bad_four_byte_lead) - 1},
        {impossible_lead, sizeof(impossible_lead) - 1},
    };
    for (const RawUtf8& raw : invalid_utf8) {
        const std::string document =
            std::string("{\"x\":\"") + std::string(raw.bytes, raw.length) + "\"}";
        CHECK(json_syntax_status(document.data(), document.size()) == JsonSyntaxStatus::Malformed);
    }

    // Independent decode-based oracle: shortest scalar value, rather than the production
    // parser's restricted second-byte ranges. Exercise every non-ASCII lead/second byte and
    // both sides of the continuation range at each remaining position, including truncation.
    const auto scalar = [](const unsigned char* bytes, size_t size) {
        unsigned width = 0;
        uint32_t value = 0;
        uint32_t minimum = 0;
        if ((bytes[0] & 0xe0) == 0xc0) {
            width = 2; value = bytes[0] & 0x1f; minimum = 0x80;
        } else if ((bytes[0] & 0xf0) == 0xe0) {
            width = 3; value = bytes[0] & 0x0f; minimum = 0x800;
        } else if ((bytes[0] & 0xf8) == 0xf0) {
            width = 4; value = bytes[0] & 0x07; minimum = 0x10000;
        } else return false;
        if (size != width) return false;
        for (unsigned i = 1; i < width; ++i) {
            if ((bytes[i] & 0xc0) != 0x80) return false;
            value = (value << 6) | (bytes[i] & 0x3f);
        }
        return value >= minimum && value <= 0x10ffff &&
               !(value >= 0xd800 && value <= 0xdfff);
    };
    const auto check_scalar = [&](const unsigned char* bytes, size_t size) {
        char document[6] = {'"'};
        for (size_t i = 0; i < size; ++i) document[i + 1] = static_cast<char>(bytes[i]);
        document[size + 1] = '"';
        CHECK((json_syntax_status(document, size + 2) == JsonSyntaxStatus::Valid) ==
              scalar(bytes, size));
    };
    constexpr unsigned char tails[] = {0x7f, 0x80, 0xbf, 0xc0};
    for (unsigned lead = 0x80; lead <= 0xff; ++lead) {
        unsigned char bytes[4] = {static_cast<unsigned char>(lead), 0, 0, 0};
        check_scalar(bytes, 1);
        for (unsigned second = 0; second <= 0xff; ++second) {
            bytes[1] = static_cast<unsigned char>(second);
            check_scalar(bytes, 2);
            if (lead < 0xe0) continue;
            for (unsigned char third : tails) {
                bytes[2] = third;
                check_scalar(bytes, 3);
                if (lead < 0xf0) continue;
                for (unsigned char fourth : tails) {
                    bytes[3] = fourth;
                    check_scalar(bytes, 4);
                }
            }
        }
    }
    // Cover every representable non-ASCII scalar, including noncharacters; JSON accepts
    // Unicode scalar values rather than imposing a text-language or character-assignment filter.
    for (uint32_t value = 0x80; value <= 0x10ffff; ++value) {
        if (value >= 0xd800 && value <= 0xdfff) continue;
        unsigned char bytes[4]{};
        size_t size = 0;
        if (value < 0x800) {
            size = 2; bytes[0] = static_cast<unsigned char>(0xc0 | (value >> 6));
        } else if (value < 0x10000) {
            size = 3; bytes[0] = static_cast<unsigned char>(0xe0 | (value >> 12));
        } else {
            size = 4; bytes[0] = static_cast<unsigned char>(0xf0 | (value >> 18));
        }
        for (size_t i = 1; i < size; ++i)
            bytes[i] = static_cast<unsigned char>(0x80 | ((value >> (6 * (size - 1 - i))) & 0x3f));
        check_scalar(bytes, size);
    }
    std::string max_depth(kJsonMaxNesting, '[');
    max_depth += '0';
    max_depth.append(kJsonMaxNesting, ']');
    CHECK(json_syntax_status(max_depth.data(), max_depth.size()) == JsonSyntaxStatus::Valid);
    std::string too_deep(kJsonMaxNesting + 1, '[');
    too_deep += '0';
    too_deep.append(kJsonMaxNesting + 1, ']');
    CHECK(!json_syntax_valid(too_deep.data(), too_deep.size()));
    CHECK(json_syntax_status(too_deep.data(), too_deep.size()) == JsonSyntaxStatus::TooDeep);
    std::string too_deep_empty(kJsonMaxNesting + 1, '[');
    too_deep_empty.append(kJsonMaxNesting + 1, ']');
    CHECK(json_syntax_status(too_deep_empty.data(), too_deep_empty.size()) ==
          JsonSyntaxStatus::TooDeep);
    static constexpr char embedded_nul[] = "{\"id\":\"a\\u0000b\"}";
    CHECK(json_syntax_status(embedded_nul, std::strlen(embedded_nul)) ==
          JsonSyntaxStatus::UnsupportedNul);

    struct FakeJson { int value; } node{7};
    const char* valid = "{\"broker\":\"host:1883\"}";

    // Deterministic n-th-allocation cJSON seam: the production callback is cJSON_Parse; this fake
    // models its internal allocation sequence and fails each allocation in turn. The shared
    // wrapper must classify every one as NoMemory/503 territory, never malformed JSON.
    for (int fail_at = 1; fail_at <= 12; ++fail_at) {
        int allocations = 0;
        auto parsed = json_materialize<FakeJson>(valid, std::strlen(valid),
            [&](const char*) -> FakeJson* {
                for (int i = 0; i < 12; ++i) {
                    ++allocations;
                    if (allocations == fail_at) return nullptr;
                }
                return &node;
            });
        CHECK(parsed.status == JsonMaterializeStatus::NoMemory);
        CHECK(parsed.root == nullptr);
        CHECK(allocations == fail_at);
    }

    auto malformed = json_materialize<FakeJson>("{bad", 4,
        [&](const char*) -> FakeJson* { return &node; });
    CHECK(malformed.status == JsonMaterializeStatus::Malformed);
    CHECK(malformed.root == nullptr);
    int too_deep_parse_calls = 0;
    auto limited = json_materialize<FakeJson>(too_deep.data(), too_deep.size(),
        [&](const char*) -> FakeJson* { ++too_deep_parse_calls; return &node; });
    CHECK(limited.status == JsonMaterializeStatus::TooDeep);
    CHECK(limited.root == nullptr);
    CHECK(too_deep_parse_calls == 0);
    int nul_parse_calls = 0;
    auto unsupported_nul = json_materialize<FakeJson>(
        embedded_nul, std::strlen(embedded_nul),
        [&](const char*) -> FakeJson* { ++nul_parse_calls; return &node; });
    CHECK(unsupported_nul.status == JsonMaterializeStatus::UnsupportedNul);
    CHECK(unsupported_nul.root == nullptr);
    CHECK(nul_parse_calls == 0);
    auto ok = json_materialize<FakeJson>(valid, std::strlen(valid),
        [&](const char*) -> FakeJson* { return &node; });
    CHECK(ok.status == JsonMaterializeStatus::Ok);
    CHECK(ok.root == &node);
}

static void test_config_request_gate() {
    using namespace tk;

    struct Spies {
        int normalize = 0;
        int load = 0;
        int validate = 0;
        int probe = 0;
        int save = 0;
        int respond = 0;
        int commit = 0;
        int response_status = 0;
        bool response_ok = false;
        std::string response_reason;
        std::string saved_value;
        bool responded = false;
    } s;

    const ConfigStringMessages messages{
        "already disabled", "unchanged", "invalid", "disabled", "saved", "save failed"};
    std::string current = "old:1";
    bool valid = true;
    ConfigProbeVerdict probe_result{};
    bool save_ok = true;

    auto run = [&](const ConfigStringSubmission& submitted) {
        s = {};
        return apply_config_string(
            submitted,
            [&](const std::string& value) { ++s.normalize; return value; },
            [&]() { ++s.load; return current; },
            [&](const std::string&) { ++s.validate; return valid; },
            [&](const std::string&) { ++s.probe; return probe_result; },
            [&](const std::string& value) {
                ++s.save;
                s.saved_value = value;
                return save_ok;
            },
            [&](int status, bool ok, const char* reason) {
                ++s.respond;
                s.responded = true;
                s.response_status = status;
                s.response_ok = ok;
                s.response_reason = reason;
                return 77;
            },
            [&]() {
                CHECK(s.responded);  // response must leave before delay/restart
                ++s.commit;
            },
            messages);
    };

    // Every transport/schema/parser rejection exits before even loading current NVS state. This
    // is the no-mutation proof for the common path the real MQTT/Syslog handlers delegate to.
    for (ConfigSubmissionStatus status : {
             ConfigSubmissionStatus::MissingBody, ConfigSubmissionStatus::TooLarge,
             ConfigSubmissionStatus::BodyNoMemory, ConfigSubmissionStatus::ReceiveFailed,
             ConfigSubmissionStatus::MalformedJson, ConfigSubmissionStatus::JsonTooDeep,
             ConfigSubmissionStatus::JsonUnsupportedNul, ConfigSubmissionStatus::JsonNoMemory,
             ConfigSubmissionStatus::ObjectRequired,
             ConfigSubmissionStatus::StringFieldRequired,
             ConfigSubmissionStatus::JsonTooManyValues,
             ConfigSubmissionStatus::DuplicateJsonKey}) {
        CHECK(run({status, {}}) == 77);
        CHECK(s.normalize == 0 && s.load == 0 && s.validate == 0 && s.probe == 0);
        CHECK(s.save == 0 && s.commit == 0 && s.respond == 1);
        CHECK(s.response_status == config_submission_http_status(status));
        CHECK(!s.response_ok);
    }

    // Empty is accepted only when it arrived as a Ready string field. It is persisted and rebooted
    // after the response, preserving the intentional disable operation without conflating it
    // with any failure above.
    current = "host:1883";
    valid = true;
    probe_result = {};
    save_ok = true;
    CHECK(run({ConfigSubmissionStatus::Ready, ""}) == 77);
    CHECK(s.normalize == 1 && s.load == 1 && s.validate == 1 && s.probe == 1);
    CHECK(s.save == 1 && s.saved_value.empty());
    CHECK(s.response_status == 200 && s.response_ok && s.response_reason == "disabled");
    CHECK(s.commit == 1);

    current = "same";
    CHECK(run({ConfigSubmissionStatus::Ready, "same"}) == 77);
    CHECK(s.load == 1 && s.validate == 0 && s.probe == 0 && s.save == 0 && s.commit == 0);
    CHECK(s.response_status == 200 && s.response_ok && s.response_reason == "unchanged");

    current = "old";
    valid = false;
    CHECK(run({ConfigSubmissionStatus::Ready, "bad"}) == 77);
    CHECK(s.validate == 1 && s.probe == 0 && s.save == 0 && s.commit == 0);
    CHECK(s.response_status == 400 && !s.response_ok);

    valid = true;
    probe_result = {false, 502, "unreachable"};
    CHECK(run({ConfigSubmissionStatus::Ready, "new"}) == 77);
    CHECK(s.probe == 1 && s.save == 0 && s.commit == 0);
    CHECK(s.response_status == 502 && s.response_reason == "unreachable");

    probe_result = {};
    save_ok = false;
    CHECK(run({ConfigSubmissionStatus::Ready, "new"}) == 77);
    CHECK(s.save == 1 && s.commit == 0);
    CHECK(s.response_status == 500 && !s.response_ok && s.response_reason == "save failed");
}

// ── HA MQTT-discovery value templates (logic/ha_templates.hpp) — absent field ⇒ unknown ─────────
static void test_ha_templates() {
    // HA ignores an EMPTY rendering (the entity keeps its previous state) and keeps the previous
    // state on a template error; only the literal `None` moves an entity to unknown. So every
    // template must render an absent — or JSON-null — field as `None`, and must also guard the
    // truthiness test so an unreported optional never becomes a phantom OFF.
    std::string door = tk::ha_binary_value_template("door", false);
    CHECK(door == "{% if value_json.door is defined and value_json.door is not none %}"
                  "{{ 'ON' if value_json.door else 'OFF' }}{% else %}None{% endif %}");
    CHECK(door.find("is defined") != std::string::npos);
    CHECK(door.find("is not none") != std::string::npos);
    CHECK(door.find("{% else %}None{% endif %}") != std::string::npos);

    // The inverted `lock` class emits OFF-when-true so a locked car reads "Locked", still guarded.
    std::string locked = tk::ha_binary_value_template("locked", true);
    CHECK(locked == "{% if value_json.locked is defined and value_json.locked is not none %}"
                    "{{ 'OFF' if value_json.locked else 'ON' }}{% else %}None{% endif %}");
    CHECK(locked.find("{% else %}None{% endif %}") != std::string::npos);

    // Numeric and text sensors: a present value is rendered unchanged, an absent one is `None`
    // instead of an empty string that HA would ignore (leaving the old reading on screen).
    std::string soc = tk::ha_value_template("battery_level");
    CHECK(soc == "{% if value_json.battery_level is defined and value_json.battery_level is not none %}"
                 "{{ value_json.battery_level }}{% else %}None{% endif %}");
    CHECK(soc.find("{% else %}None{% endif %}") != std::string::npos);

    // Golden grammar uses the old concatenation model independently of the append builder.
    // Bind every shipped discovery field, both boolean polarities and non-registry edge lengths.
    const auto check_field = [](const std::string& field) {
        const std::string present = "value_json." + field +
            " is defined and value_json." + field + " is not none";
        CHECK(tk::ha_field_present_expr(field.c_str()) == present);
        CHECK(tk::ha_value_template(field.c_str()) ==
              "{% if " + present + " %}{{ value_json." + field +
              " }}{% else %}None{% endif %}");
        for (bool invert : {false, true}) {
            const std::string on = invert ? "OFF" : "ON";
            const std::string off = invert ? "ON" : "OFF";
            CHECK(tk::ha_binary_value_template(field.c_str(), invert) ==
                  "{% if " + present + " %}{{ '" + on + "' if value_json." + field +
                  " else '" + off + "' }}{% else %}None{% endif %}");
        }
    };
    for (const auto& entry : tk::mqtt::kDiscoveryEntries) check_field(std::string(entry.field));
    for (const std::string& field : {std::string{}, std::string(300, 'x'),
                                     std::string("field_with_underscore")}) check_field(field);
    std::string unchanged = "before";
    bool refused = false;
    try {
        tk::ha_detail::reserve_fields(unchanged, 0, SIZE_MAX / 3 + 1, 3);
    } catch (const std::length_error&) { refused = true; }
    CHECK(refused && unchanged == "before");
    refused = false;
    try {
        tk::ha_detail::reserve_fields(unchanged, SIZE_MAX, 1, 1);
    } catch (const std::length_error&) { refused = true; }
    CHECK(refused && unchanged == "before");
    std::string zero_occurrences;
    tk::ha_detail::reserve_fields(zero_occurrences, 5, SIZE_MAX, 0);
    CHECK(zero_occurrences.empty() && zero_occurrences.capacity() >= 5);
}

// ── Retained MQTT state lifecycle (logic/mqtt_state_lifecycle.hpp) — stale broker values ───────
static void test_mqtt_state_lifecycle() {
    using tk::mqtt::DomainPublishAction;
    using tk::mqtt::DomainPublishState;
    using tk::mqtt::decide_domain_publish;

    // A valid cache always publishes, whatever was published before.
    for (const DomainPublishState prev : {DomainPublishState::Unknown, DomainPublishState::Published,
                                          DomainPublishState::Cleared}) {
        const auto d = decide_domain_publish(true, prev);
        CHECK(d.action == DomainPublishAction::Publish);
        CHECK(d.next == DomainPublishState::Published);
    }

    // After a reboot nothing has been published yet, but the broker still holds the retained
    // pre-reboot values: an invalid cache must clear them exactly once.
    {
        const auto d = decide_domain_publish(false, DomainPublishState::Unknown);
        CHECK(d.action == DomainPublishAction::PublishClear);
        CHECK(d.next == DomainPublishState::Cleared);
    }
    // A cache that becomes invalid after being published (pairing reset, invalidation) clears too.
    {
        const auto d = decide_domain_publish(false, DomainPublishState::Published);
        CHECK(d.action == DomainPublishAction::PublishClear);
        CHECK(d.next == DomainPublishState::Cleared);
    }
    // Once cleared, later cycles stay quiet instead of re-publishing the empty object every 15 s.
    {
        const auto d = decide_domain_publish(false, DomainPublishState::Cleared);
        CHECK(d.action == DomainPublishAction::Skip);
        CHECK(d.next == DomainPublishState::Cleared);
    }

    // Full lifecycle across cache transitions: valid, invalid, invalid, valid, invalid.
    DomainPublishState state = DomainPublishState::Unknown;
    int clears = 0, publishes = 0, skips = 0;
    for (const bool valid : {true, false, false, true, false, false}) {
        const auto d = decide_domain_publish(valid, state);
        if (d.action == DomainPublishAction::Publish) ++publishes;
        else if (d.action == DomainPublishAction::PublishClear) ++clears;
        else ++skips;
        state = d.next;
    }
    CHECK(publishes == 2 && clears == 2 && skips == 2);
}

// ── Active-window gate (logic/active_window.hpp) — the stale-charging-window fix ───────────────
static void test_active_window() {
    using namespace tk;
    // A recent command opens the window regardless of charging/contact.
    CHECK(active_window_open({/*recent_cmd*/true, false, false, 0}) == true);
    CHECK(active_window_open({true, false, true, 999999}) == true);

    // Charging + FRESH contact holds the window open.
    CHECK(active_window_open({false, /*charging*/true, /*have_contact*/true, 0}) == true);
    CHECK(active_window_open({false, true, true, kAwakeMaxAgeS - 1}) == true);

    // The bug: charging cached true but contact STALE must NOT hold the window open (car departed
    // while "Charging" → otherwise perpetual scanning). Boundary is exactly kAwakeMaxAgeS.
    CHECK(active_window_open({false, true, true, kAwakeMaxAgeS})     == false);
    CHECK(active_window_open({false, true, true, kAwakeMaxAgeS + 1}) == false);
    CHECK(active_window_open({false, true, true, 999999})           == false);

    // Charging cached true but we have NO contact at all (never heard) → not fresh → closed.
    CHECK(active_window_open({false, true, /*have_contact*/false, 0}) == false);

    // Not charging and no recent command → closed, whatever the contact age.
    CHECK(active_window_open({false, false, true, 0}) == false);
    CHECK(active_window_open({false, false, false, 0}) == false);

    // The cached-charge read (get_charge_state) asks the SAME window decision as the poll. A car
    // cached as "Charging" whose contact went stale has a closed window, so the last-known reading
    // is served whatever its age instead of being rejected as stale (which nothing would refresh
    // any more — the permanent evcc 503). With fresh contact the window is open and the 30 s
    // freshness bound applies.
    {
        const bool stale_window = active_window_open({false, true, true, kAwakeMaxAgeS + 60});
        CHECK(!stale_window);
        CHECK(charge_cache_usable(true, stale_window, true, 3600));
        const bool fresh_window = active_window_open({false, true, true, 5});
        CHECK(fresh_window);
        CHECK(charge_cache_usable(true, fresh_window, true, kActiveChargeStateMaxAgeS));
        CHECK(!charge_cache_usable(true, fresh_window, true, kActiveChargeStateMaxAgeS + 1));
    }
}

static void test_telemetry_poll() {
    using Domain = tk::TelemetryPollDomain;
    tk::TelemetryPollState st{};
    const auto attempt = [&](uint32_t ticks, bool usable, bool enqueued) {
        st.begin_attempt(ticks);
        st.note_admission(usable, enqueued);
    };
    constexpr uint32_t interval = 30000;  // Test ticks; the shell converts the cadence to IDF ticks.
    tk::TelemetryPollInputs in{true, true, true, false, interval + 1, interval};
    CHECK(st.due(in));
    in.now_ticks = interval;
    CHECK(!st.due(in));
    in.now_ticks = interval - 1;
    CHECK(!st.due(in));
    in.now_ticks = interval + 1;
    in.paired = false;
    CHECK(!st.due(in));
    in.paired = true;
    in.window = false;
    CHECK(!st.due(in));  // Being awake does not grant an idle climate poll.
    in.window = true;
    in.connected = false;
    CHECK(!st.due(in));
    in.connected = true;
    in.command_in_flight = true;
    CHECK(!st.due(in));
    in.command_in_flight = false;

    // Regression: a queued climate attempt expires/fails without publishing a sample. Retry at
    // the next 30 s slot, rather than silently moving through Drive/Tires/Closures for ~120 s.
    CHECK(st.next(false) == Domain::Climate);
    attempt(in.now_ticks, false, true);
    in.now_ticks += interval;
    CHECK(!st.due(in));
    ++in.now_ticks;
    CHECK(st.due(in));
    CHECK(st.next(false) == Domain::Climate);

    // Queue refusal cannot consume the pending retry, and still cannot create a tight loop.
    attempt(in.now_ticks, false, false);
    CHECK(!st.due(in));
    CHECK(st.next(false) == Domain::Climate);
    in.now_ticks += interval + 1;
    CHECK(st.due(in));
    attempt(in.now_ticks, false, true);
    CHECK(st.next(false) == Domain::Drive);
    attempt(in.now_ticks + interval + 1, false, false);
    CHECK(st.next(false) == Domain::Drive);

    // Once climate arrives, ordinary rotation resumes where it left off.
    const Domain normal[] = {Domain::Drive, Domain::Tires, Domain::Closures, Domain::Climate};
    for (const auto expected : normal) {
        CHECK(st.next(true) == expected);
        attempt(0, true, true);
    }

    // Both absence and the shared 300/301 s freshness boundary trigger priority, not a looser
    // API freshness allowance. A valid current sample leaves Drive as the next normal domain.
    st.reset();
    attempt(1, true, true);
    CHECK(st.next(tk::climate_cache_usable(true, true, true, 300)) == Domain::Drive);
    CHECK(st.next(tk::climate_cache_usable(true, true, true, 301)) == Domain::Climate);
    CHECK(st.next(tk::climate_cache_usable(false, true, false, 0)) == Domain::Climate);

    // Persistent climate failure must not starve any other domain or repeatedly reset to Drive.
    st.reset();
    const Domain failing[] = {Domain::Climate, Domain::Climate, Domain::Drive,
                              Domain::Climate, Domain::Climate, Domain::Tires,
                              Domain::Climate, Domain::Climate, Domain::Closures,
                              Domain::Climate, Domain::Climate, Domain::Drive};
    uint32_t now = 0;
    for (const auto expected : failing) {
        now += interval + 1;
        in.now_ticks = now;
        CHECK(st.due(in));
        CHECK(st.next(false) == expected);
        attempt(now, false, true);
    }

    // Unsigned elapsed ticks remain correct across the scheduler's wrap boundary.
    attempt(UINT32_MAX - 100, true, true);
    in.now_ticks = interval - 101;
    CHECK(!st.due(in));
    ++in.now_ticks;
    CHECK(st.due(in));
    st.reset();
    CHECK(st.next(false) == Domain::Climate);
    in.now_ticks = interval;
    CHECK(!st.due(in));

    // An exception before queue admission skips note_admission. It must still throttle the next
    // iteration (50 ms in the shell), preserve the domain and permit a later bounded retry.
    in.now_ticks = interval + 1;
    CHECK(st.due(in));
    st.begin_attempt(in.now_ticks);
    in.now_ticks += 50;
    CHECK(!st.due(in));
    CHECK(st.next(false) == Domain::Climate);
    in.now_ticks += interval - 50;
    CHECK(!st.due(in));
    ++in.now_ticks;
    CHECK(st.due(in));
    CHECK(st.next(false) == Domain::Climate);
}

// ── Wake-edge one-shot charge poll (logic/wake_poll.hpp) — the stale-SOC-after-plug-in fix ──
static void test_wake_poll() {
    using namespace tk;

    // Boot / reconnect: UNKNOWN then AWAKE with no prior stable-asleep run must NOT fire.
    {
        WakePollState st{};
        CHECK(wake_edge_should_poll(st, {WakeSample::Unknown, false}) == false);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake,   false}) == false);
        CHECK(st.armed == false);
    }

    // The fix: a debounced ASLEEP run arms, and the next self-initiated AWAKE fires exactly one
    // poll (cable plug-in after a long sleep).
    {
        WakePollState st{};
        CHECK(wake_edge_should_poll(st, {WakeSample::Asleep, false}) == false); // not yet stable
        CHECK(st.armed == false);
        CHECK(wake_edge_should_poll(st, {WakeSample::Asleep, true})  == false); // debounce reached → arm
        CHECK(st.armed == true);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake,  false}) == true);  // fire once
        CHECK(st.armed == false);
    }

    // One poll per wake episode: a second AWAKE without a fresh stable-asleep run must not re-fire,
    // and re-arming requires another debounced ASLEEP run.
    {
        WakePollState st{};
        (void)wake_edge_should_poll(st, {WakeSample::Asleep, true});
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false}) == true);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false}) == false);
        CHECK(wake_edge_should_poll(st, {WakeSample::Asleep, true}) == false);  // re-arm
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false}) == true);
    }

    // COP flap: an ASLEEP run that never reaches the debounce (vcsec_stably_asleep stays false)
    // never arms, so the ~60 s AWAKE↔ASLEEP cycling cannot spawn spurious polls.
    {
        WakePollState st{};
        for (int i = 0; i < 5; ++i) {
            CHECK(wake_edge_should_poll(st, {WakeSample::Asleep, false}) == false);
            CHECK(wake_edge_should_poll(st, {WakeSample::Awake,  false}) == false);
        }
        CHECK(st.armed == false);
    }

    // UNKNOWN between arm and wake preserves the arm (a wake during a BLE gap is still honoured).
    {
        WakePollState st{};
        (void)wake_edge_should_poll(st, {WakeSample::Asleep, true});   // armed
        CHECK(wake_edge_should_poll(st, {WakeSample::Unknown, false}) == false);
        CHECK(st.armed == true);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false}) == true);
    }

    // A bare ASLEEP reading after arming does not disarm; the pending wake still fires.
    {
        WakePollState st{};
        (void)wake_edge_should_poll(st, {WakeSample::Asleep, true});
        CHECK(st.armed == true);
        CHECK(wake_edge_should_poll(st, {WakeSample::Asleep, false}) == false);
        CHECK(st.armed == true);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false}) == true);
    }

    // Regression test for PR #265: health probe holds cmd_in_flight_ on the cycle the AWAKE edge is
    // observed. The pending wake-poll MUST stay latched across blocked cycles until queue-idle.
    {
        WakePollState st{};
        st.note_asleep(true);  // stably asleep -> armed
        CHECK(st.armed == true);
        CHECK(st.pending == false);

        st.note_awake();       // car wakes -> armed false, pending latched
        CHECK(st.armed == false);
        CHECK(st.pending == true);

        // Cycle 1: loop runs while health probe is still in-flight on auto_pair_task
        CHECK(charge_poll_should_fire({true, false, true, true /* cmd_in_flight */}, st) == false);
        CHECK(st.pending == true);  // MUST NOT be consumed while channel is blocked!

        // Cycle 2: health probe completes, channel becomes idle -> poll fires and consumes latch
        CHECK(charge_poll_should_fire({true, false, true, false /* cmd_in_flight */}, st) == true);
        CHECK(st.pending == false); // consumed now that poll actually dispatched

        // Cycle 3: subsequent cycle outside active window -> does not re-fire
        CHECK(charge_poll_should_fire({true, false, true, false}, st) == false);
        CHECK(st.pending == false);
        CHECK(st.armed == false);
    }

    // Disconnected link preserves the pending latch until reconnected and queue-idle.
    {
        WakePollState st{};
        st.note_asleep(true);
        st.note_awake();
        CHECK(st.pending == true);

        // Link down: must not consume pending
        CHECK(charge_poll_should_fire({true, false, false /* ble_connected */, false}, st) == false);
        CHECK(st.pending == true);

        // Link up: fires
        CHECK(charge_poll_should_fire({true, false, true, false}, st) == true);
        CHECK(st.pending == false);
    }

    // Active window: cadence poll consumes any lingering pending wake request.
    {
        WakePollState st{};
        st.pending = true;
        CHECK(charge_poll_should_fire({true, true /* poll_cadence */, true, false}, st) == true);
        CHECK(st.pending == false);
    }

    // Drive-cache refresh rides only on the wake/bootstrap one-shot, never on the in-window cadence.
    CHECK(wake_poll_refreshes_drive(false) == true);
    CHECK(wake_poll_refreshes_drive(true) == false);
    {
        WakePollState st{};
        st.pending = true;
        const ChargePollGateInputs one_shot{true, false /* poll_cadence */, true, false};
        CHECK(charge_poll_should_fire(one_shot, st) == true);
        CHECK(wake_poll_refreshes_drive(one_shot.poll_cadence) == true);
    }

    // Unpaired device: gate prevents firing.
    {
        WakePollState st{};
        st.pending = true;
        CHECK(charge_poll_should_fire({false /* paired */, false, true, false}, st) == false);
        CHECK(st.pending == true);
    }

    // Fresh debounced stable-asleep run clears any stale pending latch and re-arms.
    {
        WakePollState st{};
        st.pending = true;
        st.note_asleep(true);
        CHECK(st.pending == false);
        CHECK(st.armed == true);
    }

    // ── Cache-invalid bootstrap trigger (issue #264 second variant) ──

    // Boot with invalid cache and car already AWAKE: fires exactly once, no spam on next cycles.
    {
        WakePollState st{};
        // First AWAKE cycle with invalid cache fires the bootstrap poll
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false}) == true);
        CHECK(st.armed == false);
        CHECK(st.pending == false);
        CHECK(st.bootstrap_dispatched == true);

        // Next cycles while cache is still invalid do NOT re-fire (prevent queue flooding)
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false}) == false);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false}) == false);

        // Once cache is populated, remains idle
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true}) == false);
    }

    // Boot with invalid cache and car ASLEEP: does NOT poll while asleep, fires once car wakes.
    {
        WakePollState st{};
        // Asleep with invalid cache: must NOT poll sleeping car
        CHECK(wake_edge_should_poll(st, {WakeSample::Asleep, false, true, false}) == false);
        CHECK(st.armed == true);
        CHECK(st.pending == false);

        // Car wakes up: fires the armed bootstrap poll
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false}) == true);
        CHECK(st.armed == false);
        CHECK(st.pending == false);

        // Does not re-fire
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false}) == false);
    }

    // Boot with invalid cache and UNKNOWN flag: does NOT poll while UNKNOWN, fires on first AWAKE.
    {
        WakePollState st{};
        CHECK(wake_edge_should_poll(st, {WakeSample::Unknown, false, true, false}) == false);
        CHECK(st.armed == true);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false}) == true);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false}) == false);
    }

    // Reconnect retry: if connection drops while cache is still invalid, reconnect allows retry.
    {
        WakePollState st{};
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false}) == true);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false}) == false);

        // BLE drops before cache could be populated
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, false, false}) == false);
        CHECK(st.bootstrap_dispatched == false);

        // BLE reconnects while cache still invalid: retries once
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false}) == true);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false}) == false);

        // Once cache becomes valid, future reconnects never fire bootstrap
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, false, true}) == false);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true}) == false);
    }

    // Direct WakePollState methods unit tests.
    {
        WakePollState st{};
        // Disconnected: does not arm
        st.note_bootstrap(false, false);
        CHECK(st.armed == false);
        CHECK(st.bootstrap_dispatched == false);
        CHECK(st.episode_fresh == false);

        // Connected + valid cache: does not arm, latches episode_fresh (issue #308)
        st.note_bootstrap(true, true);
        CHECK(st.armed == false);
        CHECK(st.bootstrap_dispatched == false);
        CHECK(st.episode_fresh == true);

        // Once episode_fresh is latched, subsequent stale cache on same connection does not arm
        st.note_bootstrap(true, false);
        CHECK(st.armed == false);
        CHECK(st.bootstrap_dispatched == false);

        // Clean state: connected + invalid cache arms bootstrap
        WakePollState st_stale{};
        st_stale.note_bootstrap(true, false);
        CHECK(st_stale.armed == true);
        CHECK(st_stale.bootstrap_dispatched == true);
        CHECK(st_stale.episode_fresh == false);

        // Second call while connected does not re-arm
        st_stale.armed = false;
        st_stale.note_bootstrap(true, false);
        CHECK(st_stale.armed == false);

        // Disconnect resets dispatch and episode flags
        st_stale.note_disconnected();
        CHECK(st_stale.bootstrap_dispatched == false);
        CHECK(st_stale.episode_fresh == false);
    }

    // Direct loop wiring simulation: wake_poll_update followed by charge_poll_should_fire
    // verifies that pending latch is preserved when health probe is busy on single core.
    {
        WakePollState st{};
        // Boot awake with invalid cache, but channel is busy (cmd_in_flight = true)
        wake_poll_update(st, {WakeSample::Awake, false, true, false});
        CHECK(st.armed == false);
        CHECK(st.pending == true);
        CHECK(st.bootstrap_dispatched == true);

        // Gate evaluation while busy: does not fire, pending remains latched
        CHECK(charge_poll_should_fire({true, false, true, true /* cmd_in_flight */}, st) == false);
        CHECK(st.pending == true);

        // Next loop iteration: health probe finished, cmd_in_flight drops to false
        wake_poll_update(st, {WakeSample::Awake, false, true, false});
        CHECK(st.pending == true);
        CHECK(charge_poll_should_fire({true, false, true, false /* idle */}, st) == true);
        CHECK(st.pending == false);
    }

    // Issue #301: Bootstrap latch on success rather than dispatch.
    // Device reboots with invalid cache, car is AWAKE.
    // 1. Initial attempt fires at now_s = 0
    // 2. Poll in flight, times out at now_s = 25 (cache remains invalid)
    // 3. Before backoff elapsed (now_s = 29), does not re-fire
    // 4. Once backoff expires (now_s = 30), re-arms and fires retry
    // 5. Retry in flight, times out (cache remains invalid)
    // 6. Exponential backoff (now 60s): does not re-fire at now_s = 89; fires at now_s = 90
    // 7. Success at now_s = 92: cache becomes valid. Future cycles (even after backoff) do NOT fire!
    {
        WakePollState st{};
        // Cycle 1 at now_s = 0: car is AWAKE, cache invalid -> fires first attempt
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false, 0, 0}) == true);
        CHECK(st.bootstrap_dispatched == true);
        CHECK(st.bootstrap_backoff_s == kBootstrapInitialBackoffS);
        CHECK(st.next_bootstrap_retry_s == 30);

        // While in-flight and before backoff expiry: does not re-fire
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false, 0, 10}) == false);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false, 0, 25}) == false); // timeout point
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false, 0, 29}) == false);

        // At backoff expiry (now_s = 30): re-arms and fires retry!
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false, 0, 30}) == true);
        CHECK(st.bootstrap_backoff_s == 60);
        CHECK(st.next_bootstrap_retry_s == 90);

        // Second attempt also times out at 55; before 90 does not fire
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false, 0, 55}) == false);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false, 0, 89}) == false);

        // At now_s = 90 (30 + 60): fires third attempt!
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, false, 0, 90}) == true);
        CHECK(st.bootstrap_backoff_s == 120);
        CHECK(st.next_bootstrap_retry_s == 210);

        // Third attempt succeeds at now_s = 92: cache becomes valid!
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 0, 92}) == false);
        CHECK(st.bootstrap_backoff_s == 0);
        CHECK(st.next_bootstrap_retry_s == 0);

        // Long after initial backoff and subsequent backoffs, cache remains valid -> no more polls
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 0, 215}) == false);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 0, 1000}) == false);
    }

    // ── Issue #300: Stale-cache bootstrap on age rather than validity ──

    // Freshness predicate boundary checks:
    {
        // Missing cache is never fresh
        CHECK(charge_cache_fresh({WakeSample::Awake, false, true, false /* have_cache */, 0}) == false);

        // Present cache strictly younger than kChargeCacheFreshS is fresh
        CHECK(charge_cache_fresh({WakeSample::Awake, false, true, true, kChargeCacheFreshS - 1}) == true);
        CHECK(charge_cache_fresh({WakeSample::Awake, false, true, true, kChargeCacheFreshS}) == false);
        CHECK(charge_cache_fresh({WakeSample::Awake, false, true, true, kChargeCacheFreshS + 1}) == false);
        CHECK(charge_cache_fresh({WakeSample::Awake, false, true, true, 28800}) == false);
    }

    // Post-drive regression (common daily case):
    // Car was parked and asleep, cache was valid but hours old (e.g. 8 hours = 28800 s).
    // Car wakes and departs (BLE disconnects -> UNKNOWN sleep).
    // Car returns and plugs in: BLE reconnects with UNKNOWN->AWAKE transition (no wake edge).
    // Stale-cache bootstrap must fire to refresh the pre-drive SoC.
    {
        WakePollState st{};

        // Car departs: BLE drops
        wake_poll_update(st, {WakeSample::Unknown, false, false /* disconnected */, true, 28800, 100});
        CHECK(st.bootstrap_dispatched == false);

        // Car arrives and reconnects: AWAKE with 8-hour-old cache
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true /* connected */, true, 28800, 101}) == true);
        CHECK(st.bootstrap_dispatched == true);

        // While cache is still stale (poll in-flight), does not re-fire immediately
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 28800, 102}) == false);

        // Poll completes successfully, cache is now fresh (age 0 s)
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 0, 105}) == false);

        // Car stays parked and awake; no further polls
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 10, 115}) == false);
    }

    // Short hop: car leaves for 30 seconds (< kChargeCacheFreshS) and reconnects AWAKE.
    // Cache is still fresh, so it must NOT re-arm bootstrap poll.
    {
        WakePollState st{};
        // Disconnect on departure
        wake_poll_update(st, {WakeSample::Unknown, false, false, true, 10, 50});

        // Reconnect after 30s drive: cache age is 40s (< 60s)
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 40, 80}) == false);
        CHECK(st.armed == false);
        CHECK(st.pending == false);
    }

    // Boundary check on reconnect: exactly at kChargeCacheFreshS arms, at kChargeCacheFreshS - 1 does not
    {
        WakePollState st_just_fresh{};
        wake_poll_update(st_just_fresh, {WakeSample::Unknown, false, false, true, 0, 0});
        CHECK(wake_edge_should_poll(st_just_fresh, {WakeSample::Awake, false, true, true, kChargeCacheFreshS - 1, 10}) == false);

        WakePollState st_stale{};
        wake_poll_update(st_stale, {WakeSample::Unknown, false, false, true, 0, 0});
        CHECK(wake_edge_should_poll(st_stale, {WakeSample::Awake, false, true, true, kChargeCacheFreshS, 10}) == true);
    }

    // Stale-cache bootstrap combined with retry backoff (#300 + #301):
    // Reconnect with stale cache, but CarServer poll times out. Backoff must govern retries.
    {
        WakePollState st{};
        // Reconnect at now_s = 200 with stale cache (age 3600s) -> fires attempt 1
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 3600, 200}) == true);

        // In flight / timeout at 225, cache still stale (age 3625s) -> within 30s backoff
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 3625, 225}) == false);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 3629, 229}) == false);

        // Backoff expires at now_s = 230 -> retry fires
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 3630, 230}) == true);

        // Retry succeeds at now_s = 232 -> cache becomes fresh (age = 0)
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 0, 232}) == false);

        // Stays quiet even after subsequent backoff deadlines
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 10, 300}) == false);
    }

    // Pending poll must NOT survive BLE disconnect into a new connection where the car is ASLEEP.
    {
        WakePollState st{};
        // Awake with invalid cache, but channel is busy
        wake_poll_update(st, {WakeSample::Awake, false, true, false, 0, 0});
        CHECK(st.pending == true);

        // BLE disconnects before command could be sent
        wake_poll_update(st, {WakeSample::Unknown, false, false, false, 0, 1});
        // Disconnect must clear pending!
        CHECK(st.pending == false);

        // BLE reconnects while car is ASLEEP (not stably asleep)
        wake_poll_update(st, {WakeSample::Asleep, false, true, false, 0, 2});
        // Must NOT poll a sleeping car on reconnect!
        CHECK(charge_poll_should_fire({true, false, true, false, 2}, st) == false);
    }

    // Issue #308: Continuous awake quiescence latch.
    // When connected with a fresh cache (or once fresh cache is acquired during an awake episode),
    // the system MUST remain quiescent even after charge_cache_age_s exceeds kChargeCacheFreshS (60s).
    // It must NOT periodically re-arm a bootstrap poll every 60 seconds while the vehicle stays awake.
    {
        WakePollState st{};
        // Connect at t = 0 with fresh cache (age = 10s < kChargeCacheFreshS) -> no poll
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 10, 0}) == false);
        CHECK(st.bootstrap_dispatched == false);
        CHECK(st.episode_fresh == true);

        // At t = 49 (age = 59s < 60s): still fresh -> no poll
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 59, 49}) == false);

        // At t = 50 (age = 60s = kChargeCacheFreshS): cache age exceeds threshold, but
        // episode_fresh is latched -> remains completely quiescent (does NOT fire)!
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 60, 50}) == false);
        CHECK(st.armed == false);
        CHECK(st.pending == false);
        CHECK(st.bootstrap_dispatched == false);

        // Long-term awake quiescence: vehicle remains parked and connected without commands or charging
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 110, 100}) == false);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 310, 300}) == false);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 610, 600}) == false);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 1810, 1800}) == false);
    }

    // Multi-cycle steady-state simulation (Issue #308 + Section 3a of $add-logic-test):
    // Verifies the complete lifecycle across multiple sleep/wake/disconnect/drive episodes:
    //   Cycle 1: Stably asleep -> wake-edge poll -> fresh cache latched -> 15 min awake quiescence.
    //   Transition: Enters stable sleep -> episode latch clears -> armed for next wake edge.
    //   Cycle 2: Second wake-edge poll -> poll times out -> retry backoff -> retry succeeds -> quiescent.
    //   Cycle 3: Drive away (disconnect) -> reconnect with stale cache -> bootstrap fires -> succeeds -> quiescent.
    {
        WakePollState st{};

        // ── Cycle 1: Stable sleep to wake-edge poll and long awake quiescence ──
        // Car is stably asleep
        CHECK(wake_edge_should_poll(st, {WakeSample::Asleep, true, true, true, 3600, 0}) == false);
        CHECK(st.armed == true);
        CHECK(st.episode_fresh == false);

        // Car wakes up (plugged in): wake-edge poll fires once
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 3600, 1}) == true);
        CHECK(st.armed == false);
        CHECK(st.pending == false);

        // In-flight (now_s = 2): no duplicate poll
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 3601, 2}) == false);

        // Poll completes at now_s = 3: fresh charge cache received (age = 0s)
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 0, 3}) == false);
        CHECK(st.episode_fresh == true);

        // Vehicle stays parked and awake for 15 minutes (900s) without charging or commands.
        // Simulate periodic checks every 30 seconds: 0 polls must fire!
        for (uint32_t t = 30; t <= 900; t += 30) {
            uint32_t age = t - 3; // cache ages naturally
            CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, age, t}) == false);
        }

        // ── Transition: Car goes to sleep ──
        // COP flap (unstable asleep) must NOT clear episode latch
        CHECK(wake_edge_should_poll(st, {WakeSample::Asleep, false, true, true, 905, 908}) == false);
        CHECK(st.episode_fresh == true);

        // Stable asleep (> kAsleepDebounceS) ends the awake episode, clears latch, arms next wake edge
        CHECK(wake_edge_should_poll(st, {WakeSample::Asleep, true, true, true, 915, 918}) == false);
        CHECK(st.armed == true);
        CHECK(st.episode_fresh == false);

        // ── Cycle 2: Second wake episode with timeout and retry backoff ──
        // Car wakes up: wake-edge fires attempt 1
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 1200, 1200}) == true);
        CHECK(st.armed == false);

        // Attempt 1 times out at 1225 (cache remains stale age 1225s)
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 1225, 1225}) == false);
        // Before 30s backoff (1200 + 30 = 1230), does NOT re-fire
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 1229, 1229}) == false);

        // At backoff expiry (now_s = 1230): retry fires!
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 1230, 1230}) == true);

        // Retry succeeds at now_s = 1232: fresh cache acquired
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 0, 1232}) == false);
        CHECK(st.episode_fresh == true);

        // Quiescence resumes while awake: no polls even after cache reaches 60s, 100s, 300s
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 70, 1302}) == false);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 200, 1432}) == false);

        // ── Cycle 3: Car drives away and returns (reconnect with stale cache) ──
        // Departure: BLE disconnects
        wake_poll_update(st, {WakeSample::Unknown, false, false, true, 300, 1500});
        CHECK(st.episode_fresh == false);
        CHECK(st.bootstrap_dispatched == false);

        // Return from 2-hour drive: reconnects AWAKE with 7200s old cache
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 7200, 8700}) == true);
        CHECK(st.bootstrap_dispatched == true);

        // Bootstrap poll succeeds at now_s = 8702: fresh cache acquired
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 0, 8702}) == false);
        CHECK(st.episode_fresh == true);

        // Quiescent indefinitely while parked and connected
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 100, 8802}) == false);
        CHECK(wake_edge_should_poll(st, {WakeSample::Awake, false, true, true, 1000, 9702}) == false);
    }
}

// ── BLE command readiness (logic/ble_readiness.hpp) — GAP is not GATT-ready ───────────
static void test_ble_readiness() {
    using namespace tk::ble;

    const uint32_t generation = 2;  // stable generations are even

    // Cold boot and an in-progress handle publication are never command-ready.
    CHECK(!gap_link_available(false, 0, 0, false));
    CHECK(!command_ready(false, 0, 0, false, false, kNoReadyGeneration));
    CHECK(!gap_link_available(false, 1, 1, true));
    CHECK(!command_ready(false, 1, 1, true, true, 1));

    // The regression: GAP CONNECT publishes a valid handle before Tesla service,
    // characteristics and CCCD subscription complete. Internal BLE code may use that link,
    // but the first vehicle command must keep waiting.
    CHECK(gap_link_available(false, generation, generation, true));
    CHECK(!command_ready(false, generation, generation, true, false,
                         kNoReadyGeneration));  // service discovery
    CHECK(!command_ready(false, generation, generation, true, true,
                         kNoReadyGeneration));  // write characteristic, no CCCD ack yet

    // Only the generation published after a successful CCCD write and connected callback is
    // usable by commands.
    CHECK(command_ready(false, generation, generation, true, true, generation));

    // Disconnect starts before the GAP event and closes readiness immediately. A delayed old
    // GATT callback cannot bless a replacement link, even if NimBLE reuses its 16-bit handle:
    // the ready token belongs to the previous connection generation.
    CHECK(!command_ready(true, generation, generation, true, true, generation));
    CHECK(!command_ready(false, generation, generation + 2, true, true, generation));
    CHECK(!command_ready(false, generation + 2, generation + 2, true, true, generation));
    CHECK(command_ready(false, generation + 2, generation + 2, true, true,
                        generation + 2));

    // Host reset must clear the in-flight latch while preserving the caller's explicit intent.
    // want_connect now remains asserted until command-ready, so a concurrent stop_connecting()
    // can still cancel cleanly instead of `connecting` accidentally resurrecting its request.
    ConnectLifecycle reset_idle = connect_lifecycle_after_host_reset(false);
    CHECK(!reset_idle.want_connect);
    CHECK(!reset_idle.connecting);
    ConnectLifecycle reset_wanted = connect_lifecycle_after_host_reset(true);
    CHECK(reset_wanted.want_connect);
    CHECK(!reset_wanted.connecting);

    // A synchronous ble_gap_connect() rejection keeps the same bounded caller armed, while an
    // already-canceled caller must stay canceled; neither outcome may leave `connecting` latched.
    ConnectLifecycle sync_fail = connect_lifecycle_after_start_failure(true);
    CHECK(sync_fail.want_connect);
    CHECK(!sync_fail.connecting);
    ConnectLifecycle canceled_fail = connect_lifecycle_after_start_failure(false);
    CHECK(!canceled_fail.want_connect);
    CHECK(!canceled_fail.connecting);

    // The bounded caller's intent remains live across the whole cold path, not merely until GAP
    // CONNECT. A host reset or link loss during service/characteristic/CCCD discovery must restart
    // the scan; only confirmed command readiness consumes the intent.
    ConnectLifecycle starting = connect_lifecycle_during_gap_start(true);
    CHECK(starting.want_connect);
    CHECK(starting.connecting);
    ConnectLifecycle canceled_start = connect_lifecycle_during_gap_start(false);
    CHECK(!canceled_start.want_connect);
    CHECK(!canceled_start.connecting);
    ConnectLifecycle discovering = connect_lifecycle_after_gap_connected(true);
    CHECK(discovering.want_connect);
    CHECK(!discovering.connecting);
    ConnectLifecycle canceled_discovery = connect_lifecycle_after_gap_connected(false);
    CHECK(!canceled_discovery.want_connect);
    CHECK(!canceled_discovery.connecting);
    ConnectLifecycle ready = connect_lifecycle_after_command_ready();
    CHECK(!ready.want_connect);
    CHECK(!ready.connecting);

    CHECK(connect_scan_should_start(true, false, false, false, false));
    CHECK(!connect_scan_should_start(false, false, false, false, false));
    CHECK(!connect_scan_should_start(true, true, false, false, false));
    CHECK(!connect_scan_should_start(true, false, true, false, false));
    CHECK(!connect_scan_should_start(true, false, false, true, false));
    CHECK(!connect_scan_should_start(true, false, false, false, true));

    // Scan start/stop publication is serialized by intent_mutex_. The first concurrent starter
    // publishes the live procedure, so the second never calls NimBLE. If the host nevertheless
    // reports EALREADY (for a scan started before our publication), that is positive evidence of
    // a running scan and must not be collapsed to false. A failed redundant call likewise cannot
    // erase an already-published running procedure.
    CHECK(scan_running_after_start(false, ScanStartResult::Started));
    CHECK(scan_running_after_start(false, ScanStartResult::AlreadyRunning));
    CHECK(!scan_running_after_start(false, ScanStartResult::Failed));
    CHECK(scan_running_after_start(true, ScanStartResult::Failed));
    const bool first_start = scan_running_after_start(false, ScanStartResult::Started);
    CHECK(first_start);
    CHECK(!connect_scan_should_start(true, false, false, false, first_start));

    // A successful timeout/deadline cancel, or EALREADY (host proves no scan exists), publishes
    // stopped. Unexpected cancel failures leave the prior running state visible so a later stop
    // can retry instead of orphaning an unbounded controller scan.
    CHECK(!scan_running_after_cancel(true, ScanCancelResult::Canceled));
    CHECK(!scan_running_after_cancel(true, ScanCancelResult::AlreadyStopped));
    CHECK(scan_running_after_cancel(true, ScanCancelResult::Failed));
    CHECK(!scan_running_after_cancel(false, ScanCancelResult::Failed));
    CHECK(manual_discovery_timeout_may_cancel(
        scan_running_after_cancel(true, ScanCancelResult::Failed), false, false, false));

    // Manual discovery owns only its listing timer, never the command intent. The lock-protected
    // start predicate may pass before a concurrent connect arrives, but that fresh intent then
    // prevents the listing timeout from canceling the shared scan.
    CHECK(manual_discovery_may_start(false, false, false, false));
    CHECK(!manual_discovery_may_start(true, false, false, false));
    CHECK(!manual_discovery_may_start(false, true, false, false));
    CHECK(!manual_discovery_may_start(false, false, true, false));
    CHECK(!manual_discovery_may_start(false, false, false, true));
    CHECK(manual_discovery_timeout_may_cancel(true, false, false, false));
    CHECK(!manual_discovery_timeout_may_cancel(true, true, false, false));
    CHECK(!manual_discovery_timeout_may_cancel(true, false, true, false));
    CHECK(!manual_discovery_timeout_may_cancel(true, false, false, true));
    CHECK(!manual_discovery_timeout_may_cancel(false, false, false, false));

    // A deadline cancellation linearized before GAP success or CCCD completion invalidates that
    // continuation. A later callback may advance only for a still-live intent and matching link;
    // a fresh same-target command is allowed to adopt the pending link by asserting a new intent.
    CHECK(connect_attempt_may_advance(true, true));
    CHECK(!connect_attempt_may_advance(false, true));
    CHECK(!connect_attempt_may_advance(true, false));
    CHECK(!connect_attempt_may_advance(false, false));
}

// ── BLE phase countdown (logic/ble_phase.hpp) — what the Bluetooth row counts down ────────────
static void test_ble_phase() {
    using namespace tk::ble;
    const uint32_t HZ = 100;   // configTICK_RATE_HZ on the device; the maths is hz-agnostic

    // Nothing armed → no phase, and /status omits both fields.
    CHECK(phase(0, 0, 5000, HZ).kind == Phase::None);
    CHECK(std::string(phase_name(Phase::None)).empty());

    // A phase reads its FULL length the moment it is armed. The 30 s idle wait must say 30,
    // not 29 — truncating instead of rounding up is what made the countdown start a second
    // short and end a second early.
    CHECK(phase(0, 5000 + 30 * HZ, 5000, HZ).secs == 30);
    CHECK(secs_left(5000 + 10 * HZ, 5000, HZ) == 10);

    // …and holds a non-zero value for every instant before the deadline, right down to the
    // final tick. This is the "bare Disconnected" bug: the last second must still count.
    CHECK(secs_left(5000 + HZ, 5000, HZ)     == 1);   // exactly 1 s out
    CHECK(secs_left(5000 + 1, 5000, HZ)      == 1);   // 1 tick out — still "1s", never 0
    CHECK(secs_left(5000, 5000, HZ)          == 0);   // deadline reached ⇒ "right now"
    CHECK(secs_left(5000 - 10 * HZ, 5000, HZ) == 0);  // overshot ⇒ clamped, not a huge number

    // 0 is a real, reportable answer — the phase is still armed, so the UI keeps its label's
    // countdown ("retrying…") instead of dropping the suffix while the next phase spins up.
    CHECK(phase(0, 5000, 5000, HZ).kind == Phase::Waiting);
    CHECK(phase(0, 5000, 5000, HZ).secs == 0);

    // An EXPIRED deadline stays armed — "armed" is the deadline being set, never it being in
    // the future. auto_pair leans on this: it no longer clears the retry deadline before a
    // health probe, because that probe first blocks on command_mutex_ behind any in-flight
    // command, and a cleared deadline left the row with no phase (bare label) for the whole
    // wait. The stale-but-armed deadline reads "retrying… right now" until the attempt's own
    // countdown takes over.
    PhaseView stale = phase(0, 5000 - 60 * HZ, 5000, HZ);
    CHECK(stale.kind == Phase::Waiting);
    CHECK(stale.secs == 0);
    // …and an attempt starting during that wait still wins, so the row shows the attempt.
    CHECK(phase(5000 + 9 * HZ, 5000 - 60 * HZ, 5000, HZ).kind == Phase::Connecting);

    // Connecting outranks Waiting: an attempt started inside the idle wait (a command, or
    // loop_task's warm-up connect) is what the radio is actually doing.
    PhaseView both = phase(/*connect*/5000 + 8 * HZ, /*retry*/5000 + 25 * HZ, 5000, HZ);
    CHECK(both.kind == Phase::Connecting);
    CHECK(both.secs == 8);
    // …and because the deadlines are independent, the idle wait's countdown is still intact
    // underneath: when the attempt clears, the row resumes counting to the next one.
    CHECK(phase(0, 5000 + 25 * HZ, 5000, HZ).kind == Phase::Waiting);
    CHECK(phase(0, 5000 + 25 * HZ, 5000, HZ).secs == 25);

    // Tick-counter wrap: a deadline just past the wrap with `now` just before it is ~2 s out.
    // An unsigned difference would read as ~49 days here.
    CHECK(secs_left(/*deadline*/100, /*now*/0xFFFFFFFF - 99, HZ) == 2);
    // And a deadline just BEFORE the wrap with `now` just after it is in the past ⇒ 0.
    CHECK(secs_left(0xFFFFFFFF - 99, 100, HZ) == 0);


    // /status spellings the UI switches on.
    CHECK(std::string(phase_name(Phase::Connecting)) == "connecting");
    CHECK(std::string(phase_name(Phase::Waiting))    == "waiting");
}

// ── BLE connection row (logic/ble_row.hpp) — what the web UI's Bluetooth row shows ──────────
static void test_ble_row() {
    using namespace tk::ble;
    const char* VIN = "5YJ3E1EA7KF000316";
    auto st = [](const char* vin, bool paired, tk::LinkState link, bool conn,
                 int devices, uint32_t cf, Phase ph) {
        RowStatus s; s.vin=vin; s.paired=paired; s.link=link; s.ble_connected=conn;
        s.ble_devices=devices; s.ble_connect_fail=cf; s.phase=ph; return s;
    };
    const tk::LinkState UNK = tk::LinkState::Unknown, UNR = tk::LinkState::Unreachable,
                        AWK = tk::LinkState::Awake;

    // ── The derivations the presenter owns (they used to sit uncovered at the call site) ──
    CHECK(has_vin(VIN)       == true);
    CHECK(has_vin("UNKNOWN") == false);   // the firmware's "no VIN stored" spelling
    CHECK(has_vin("")        == false);
    CHECK(has_vin(nullptr)   == false);
    CHECK(has_vin("UNKNOWNX") == true);   // prefix of the sentinel is still a VIN-ish string
    // "Known" is gated on paired: before pairing, `unknown` just means we never talked to the
    // car, and an unpaired cold-start board must render green, not amber. Dropping `paired`
    // here is the regression this CHECK exists to catch.
    CHECK(link_known(/*paired*/false, UNK) == true);
    CHECK(link_known(false, UNR)           == true);
    CHECK(link_known(true,  UNK)           == false);
    CHECK(link_known(true,  UNR)           == false);
    CHECK(link_known(true,  AWK)           == true);

    // ── The six row states ────────────────────────────────────────────────────────────
    CHECK(decide(st("UNKNOWN", false, UNK, false, 0, 0, Phase::None)).row       == Row::Discovering);
    CHECK(decide(st("UNKNOWN", false, UNK, false, 3, 0, Phase::None)).row       == Row::Listing);
    CHECK(decide(st(VIN, true,  AWK, true,  0, 0, Phase::None)).row             == Row::Linked);
    CHECK(decide(st(VIN, true,  UNK, false, 0, 2, Phase::Connecting)).row       == Row::Failed);
    CHECK(decide(st(VIN, true,  UNK, false, 0, 0, Phase::Connecting)).row       == Row::Scanning);
    CHECK(decide(st(VIN, true,  UNK, false, 0, 0, Phase::Waiting)).row          == Row::Idle);

    // Linked-but-stateless reads amber; an UNPAIRED board on the same link string reads green.
    CHECK(decide(st(VIN, true,  UNK, true, 0, 0, Phase::None)).stateless == true);
    CHECK(decide(st(VIN, false, UNK, true, 0, 0, Phase::None)).stateless == false);
    CHECK(decide(st(VIN, true,  AWK, true, 0, 0, Phase::None)).stateless == false);

    // Failure outranks the phase; boundary is exactly kConnectFailWarn.
    CHECK(decide(st(VIN, true, UNK, false, 0, kConnectFailWarn-1, Phase::Connecting)).row == Row::Scanning);
    CHECK(decide(st(VIN, true, UNK, false, 0, kConnectFailWarn,   Phase::Connecting)).row == Row::Failed);

    // ── The invariants, swept over every representable input ──────────────────────────
    // Each one is a bug that shipped; the sweep is what makes them structural rather than
    // "checked in the cases someone remembered".
    const Phase phases[] = { Phase::None, Phase::Connecting, Phase::Waiting };
    const tk::LinkState links[] = { UNK, UNR, AWK, tk::LinkState::Asleep, tk::LinkState::Idle };
    for (const char* vin : { "UNKNOWN", VIN })
    for (int paired = 0; paired < 2; paired++)
    for (tk::LinkState link : links)
    for (int conn = 0; conn < 2; conn++)
    for (int dev : {0, 4})
    for (uint32_t cf : {0u, 1u, 2u, 9u})
    for (Phase ph : phases) {
        RowView v = decide(st(vin, paired, link, conn, dev, cf, ph));

        // 1. The label follows the PHASE, never the radio's scanning flag. Scanning is shown
        //    if and only if a bounded attempt is actually running (and nothing outranks it).
        if (v.row == Row::Scanning) CHECK(ph == Phase::Connecting);

        // 2. A countdown belongs to exactly one state.
        CHECK((v.cd == Countdown::GivesUp) == (v.row == Row::Scanning));
        CHECK((v.cd == Countdown::Retries) == (v.row == Row::Idle && ph == Phase::Waiting));

        // 3. While a phase is armed, a state that CAN show a countdown always does. A bare
        //    "Disconnected"/"Searching…" with a live phase behind it is the original bug.
        if (ph != Phase::None && (v.row == Row::Scanning || v.row == Row::Idle))
            CHECK(v.cd != Countdown::None);

        // 4. Only the no-VIN listing scan says "Searching…" with nothing to count down.
        if (v.row == Row::Discovering) CHECK(!has_vin(vin));

        // 5. Amber ("stateless") is reachable ONLY on a linked row, and only while paired.
        if (v.stateless) { CHECK(v.row == Row::Linked); CHECK(paired == 1); }
    }

    // ── Replay: input vectors actually observed on hardware ───────────────────────────
    // Distilled from 573 /status samples taken off a paired board whose car was out of BLE
    // range (two runs: active window closed, then forced open with a command). The middle row
    // is the one that mattered — 171 frames where the radio was scanning while the ARMED phase
    // was the idle wait. Under the old "label from ble.scanning" rule that frame rendered
    // "Searching…" next to a "retry in …" countdown; here it must resolve to Idle + Retries.
    struct Rec { Phase phase; const char* row; const char* cd; };
    const Rec seen[] = {
        { Phase::Waiting,    "idle",     "retries" },   // 261 frames
        { Phase::Waiting,    "idle",     "retries" },   // 171 frames — radio scanning, bug case
        { Phase::Connecting, "scanning", "givesup" },   // 141 frames
    };
    for (const Rec& r : seen) {
        RowView v = decide(st(VIN, true, UNK, false, 0, 0, r.phase));
        CHECK_STR(row_name(v.row), r.row);
        CHECK_STR(cd_name(v.cd),   r.cd);
    }
}


// ─── Reset reason / crash reporting ───────────────────────────────────────────
// Before this existed, a panic left no artifact a remote reader could reach: the reset reason was
// printed once at boot and nothing else survived. These pin the vocabulary all crash reporting is
// built on — a WRONG label is worse than no label, because it sends the next investigation
// somewhere else entirely.
static void test_reset_reason() {
    // The numeric values mirror ESP-IDF's esp_reset_reason_t; diag_crash.cpp static_asserts that
    // mapping against the real enum, so here we pin only the spelling and the classification.
    const char* expected[] = {"unknown", "power_on", "ext", "sw", "panic", "int_wdt",
        "task_wdt", "wdt", "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse",
        "pwr_glitch", "cpu_lockup"};
    // Independent literal fault set: all three watchdogs, panic, brownout, power glitch
    // and CPU lockup. Do not derive the oracle from reset_is_fault itself.
    const bool expected_fault[] = {false, false, false, false, true, true, true, true,
        false, true, false, false, false, false, true, true};
    for (int code = 0; code < 16; ++code) {
        CHECK(std::string(tk::reset_reason_slug(code)) == expected[code]);
        CHECK(tk::reset_is_fault(code) == expected_fault[code]);
    }
    for (int code : {-2147483647 - 1, -1, 16, 9999, 2147483647}) {
        CHECK(std::string(tk::reset_reason_slug(code)) == "unknown");
        CHECK(!tk::reset_is_fault(code));
    }

    // The fault set drives BOTH the crash report and the safe-mode counter, so its boundaries are
    // the load-bearing part. SW is NOT a fault: every successfully persisted rebooting
    // configuration/portal save, every OTA and the heap watchdog's own restart report it, and
    // counting those would latch safe mode on a device whose user merely changed the broker four
    // times.
    CHECK(tk::reset_is_fault((int)tk::ResetCode::Panic));
    CHECK(tk::reset_is_fault((int)tk::ResetCode::IntWdt));
    CHECK(tk::reset_is_fault((int)tk::ResetCode::TaskWdt));
    CHECK(tk::reset_is_fault((int)tk::ResetCode::Brownout));
    CHECK(!tk::reset_is_fault((int)tk::ResetCode::PowerOn));
    CHECK(!tk::reset_is_fault((int)tk::ResetCode::Sw));
    CHECK(!tk::reset_is_fault((int)tk::ResetCode::Ext));
    CHECK(!tk::reset_is_fault((int)tk::ResetCode::DeepSleep));
}

static void test_crashinfo() {
    // An ordinary boot reports NOTHING. That property is what keeps the report meaningful:
    // /status.last_crash is absent on a healthy device, so its presence alone is the signal.
    tk::CrashInfo ok;
    tk::crash_set_reset(ok, (int)tk::ResetCode::PowerOn);
    CHECK(!ok.fault);
    CHECK(!tk::crash_is_notable(ok));

    tk::CrashInfo panic;
    tk::crash_set_reset(panic, (int)tk::ResetCode::Panic);
    CHECK(panic.fault);
    CHECK(tk::crash_is_notable(panic));

    // A dump left in flash is notable even on a CLEAN boot — the crash happened, its evidence is
    // still downloadable, and a device that has power-cycled since must not hide it.
    tk::CrashInfo dump_only;
    tk::crash_set_reset(dump_only, (int)tk::ResetCode::PowerOn);
    dump_only.coredump = true;
    CHECK(tk::crash_is_notable(dump_only));

    // Dismissal is a real deletion: once acknowledged, every surface goes quiet at once.
    tk::CrashInfo dismissed = panic;
    dismissed.dismissed = true;
    CHECK(!tk::crash_is_notable(dismissed));

    CHECK(tk::crash_erase_permits_dismiss(tk::CrashEraseResult::Absent));
    CHECK(tk::crash_erase_permits_dismiss(tk::CrashEraseResult::Erased));
    CHECK(!tk::crash_erase_permits_dismiss(tk::CrashEraseResult::Busy));
    CHECK(!tk::crash_erase_permits_dismiss(tk::CrashEraseResult::Unconfirmed));
    CHECK(!tk::crash_has_summary(panic));
    panic.task = "vehicle_loop";
    CHECK(!tk::crash_has_summary(panic)); // fields cannot manufacture verification
    panic.summary_available = true;
    CHECK(tk::crash_has_summary(panic));
    tk::CrashInfo zero;
    zero.summary_available = true; zero.corrupted = true; zero.truncated = true; zero.isr = true;
    CHECK(tk::crash_has_summary(zero));
    const auto zero_text = tk::build_crash_text(zero);
    CHECK(zero_text.find("pc=0x00000000") != std::string::npos);
    CHECK(zero_text.find("corrupted") != std::string::npos);
    CHECK(zero_text.find("truncated") != std::string::npos && zero_text.find("isr=yes") != std::string::npos);
    tk::CrashInfo uncertain;
    uncertain.query_error = true;
    CHECK(tk::crash_is_notable(uncertain));
    uncertain.query_error = false; uncertain.artifact_present = true;
    CHECK(tk::crash_is_notable(uncertain));

    // Foreign identity detection never authorizes erasing crash evidence.
    // Unknown hashes remain Unverified; a meaningful differing prefix establishes Foreign.
    CHECK(!tk::coredump_is_foreign("", ""));
    CHECK(!tk::coredump_is_foreign("abcdef1234", ""));
    CHECK(!tk::coredump_is_foreign("", "abcdef1234"));
    CHECK(!tk::coredump_is_foreign("abcdef1234", "abcdef1234"));
    // Truncation is normal (IDF retrieves 9 hex chars by default): a shorter rendering of the SAME
    // hash must not read as a different build.
    CHECK(!tk::coredump_is_foreign("abcdef1234567890", "abcdef123"));
    // A real mismatch over a meaningful length IS proof.
    CHECK(tk::coredump_is_foreign("abcdef1234567890", "0123456789abcdef"));

    // The text bundle names the crash without inventing anything.
    tk::CrashInfo full;
    tk::crash_set_reset(full, (int)tk::ResetCode::TaskWdt);
    full.summary_available = true;
    full.task = "mqtt_pub"; full.pc = 0x400d1234;
    full.backtrace = {0x400d1234, 0x400d5678};
    full.elf_sha256 = "deadbeef1";
    const std::string txt = tk::build_crash_text(full);
    CHECK(txt.find("task_wdt")  != std::string::npos);
    CHECK(txt.find("mqtt_pub")  != std::string::npos);
    CHECK(txt.find("400d1234")  != std::string::npos);
    CHECK(txt.find("deadbeef1") != std::string::npos);

    // Borrowed metadata has the same presentation and points only to its immutable owner.
    full.coredump = full.raw_available = full.artifact_present = full.checksum_verified = true;
    full.dump_identity = tk::DumpIdentity::VerifiedCurrent;
    full.dump_elf_sha256.assign(64, 'b');
    const auto view = tk::crash_borrow(full);
    CHECK(view.task.data() == full.task.data());
    CHECK(view.elf_sha256.data() == full.elf_sha256.data());
    CHECK(view.dump_elf_sha256.data() == full.dump_elf_sha256.data());
    CHECK(view.backtrace.begin() == full.backtrace.data());
    CHECK(tk::build_crash_text(full) == tk::build_crash_text(view));
    std::vector<std::string> owned_lines, view_lines;
    CHECK(tk::build_crash_log_lines(full, owned_lines) ==
          tk::build_crash_log_lines(view, view_lines));
    CHECK(owned_lines == view_lines);
    CHECK(tk::CrashTraceView{}.begin() == tk::CrashTraceView{}.end());
    CHECK(tk::CrashTraceView(nullptr, 4).size() == 0);

    auto stable = view;
    tk::crash_finish_snapshot(stable, {8, 8, true, false, true, true, false, false});
    CHECK(stable.coredump && stable.summary_available && stable.backtrace.size() == 2);
    for (const tk::CrashSnapshotState rejected : {
        tk::CrashSnapshotState{8, 16, true, false, true, true, false, false},
        tk::CrashSnapshotState{8, 8, false, false, true, true, false, false},
        tk::CrashSnapshotState{8, 8, true, true, true, true, false, false},
        tk::CrashSnapshotState{8, 8, true, false, false, true, false, false},
        tk::CrashSnapshotState{8, 8, true, false, true, false, false, false}}) {
        auto revoked = view;
        tk::crash_finish_snapshot(revoked, rejected);
        CHECK(!revoked.coredump && !revoked.summary_available && revoked.task.empty());
        CHECK(revoked.backtrace.size() == 0 && revoked.pc == 0);
        CHECK(!revoked.corrupted && !revoked.truncated && !revoked.isr);
        if (rejected.before != rejected.after) CHECK(!revoked.raw_available);
        CHECK(revoked.elf_sha256 == full.elf_sha256); // running build remains useful
        CHECK(full.task == "mqtt_pub" && full.backtrace.size() == 2); // owner never cleared
    }
    auto absent = view;
    tk::crash_finish_snapshot(absent, {8, 16, true, false, false, false, false, true});
    CHECK(!absent.artifact_present && !absent.raw_available && !absent.checksum_verified);
    CHECK(absent.dump_elf_sha256.empty() && absent.dump_identity == tk::DumpIdentity::Unverified);
    auto foreign = view;
    foreign.foreign_dump = true; foreign.dump_identity = tk::DumpIdentity::Foreign;
    tk::crash_finish_snapshot(foreign, {8, 8, true, false, true, false, true, false});
    CHECK(!foreign.coredump && !foreign.summary_available && foreign.foreign_dump);
    CHECK(foreign.checksum_verified && foreign.dump_elf_sha256 == full.dump_elf_sha256);

}

static void test_bootlog() {
    // The build-identity line is the only way to tell WHICH firmware produced a log stream — a
    // collector holding months of lines across several flashes has nothing else to key on.
    const std::string b = tk::build_boot_line("1.4.2", "deadbeef1", "panic", false);
    CHECK(b.find("1.4.2")     != std::string::npos);
    CHECK(b.find("deadbeef1") != std::string::npos);
    CHECK(b.find("panic")     != std::string::npos);
    CHECK(b.find('\n')        == std::string::npos);   // ONE datagram, one line
    CHECK(b.size() <= tk::kBootlogLineMax);
    // Safe mode has to be visible here: a board in recovery answers /status but never touches the
    // car, and a reader of the log stream would otherwise diagnose a broken BLE stack.
    CHECK(tk::build_boot_line("1.4.2", "x", "panic", true).find("safe_mode=yes") != std::string::npos);

    // A HEALTHY boot must add nothing. Replaying a crash record on every boot forever is how a
    // collector gets trained to ignore the one line that mattered.
    std::vector<std::string> out;
    tk::CrashInfo clean;
    tk::crash_set_reset(clean, (int)tk::ResetCode::PowerOn);
    CHECK(tk::build_crash_log_lines(clean, out) == 0);
    CHECK(out.empty());

    tk::CrashInfo crash;
    tk::crash_set_reset(crash, (int)tk::ResetCode::Panic);
    crash.summary_available = true;
    crash.task = "vehicle_loop"; crash.pc = 0x400d1234;
    crash.backtrace.assign(32, 0x400d1234);   // deliberately deeper than any sane stack
    crash.elf_sha256 = "deadbeef1";
    out.clear();
    const int n = tk::build_crash_log_lines(crash, out);
    CHECK(n > 0);
    CHECK((size_t)n == out.size());
    for (const std::string& l : out) {
        // Every record must survive the transport: diag_log.cpp formats one line into a 256-byte
        // buffer, so an over-long record reaches the collector cut off mid-sentence — and these are
        // precisely the lines that must not be.
        CHECK(l.size() <= tk::kBootlogLineMax);
        CHECK(l.find('\n') == std::string::npos);
    }
    crash.task.assign(100, 'x'); crash.elf_sha256.assign(100, 'a');
    crash.dump_elf_sha256.assign(100, 'b');
    crash.artifact_present = crash.raw_available = crash.query_error = true;
    crash.corrupted = crash.truncated = crash.isr = true;
    for (auto identity : {tk::DumpIdentity::Unverified, tk::DumpIdentity::Foreign,
                          tk::DumpIdentity::VerifiedCurrent}) {
        crash.dump_identity = identity; out.clear();
        CHECK(tk::build_crash_log_lines(crash, out) == 3);
        for (const auto& line : out) CHECK(line.size() <= tk::kBootlogLineMax);
    }
    crash.task.clear(); crash.pc = 0; crash.backtrace.clear(); out.clear();
    CHECK(tk::build_crash_log_lines(crash, out) == 2);
    CHECK(out[1].find("task=? pc=0x00000000") != std::string::npos);
    CHECK(out[1].find("corrupted=yes") != std::string::npos);
    CHECK(out[1].find("truncated=yes") != std::string::npos && out[1].find("isr=yes") != std::string::npos);

}

// ─── Boot-loop safe mode ──────────────────────────────────────────────────────
static void test_boot_guard() {
    unsigned parsed = 999u;
    CHECK(tk::boot_fail_parse("0", parsed) && parsed == 0);
    CHECK(tk::boot_fail_parse("1", parsed) && parsed == 1);
    CHECK(tk::boot_fail_parse("100", parsed) && parsed == tk::kBootFailMax);
    static constexpr const char* invalid_values[] = {
        nullptr, "", "00", "01", "+1", "-1", "junk", "1x", "1.0", "101",
        "42949672960",
    };
    for (const char* invalid : invalid_values) {
        parsed = 77u;
        CHECK(!tk::boot_fail_parse(invalid, parsed));
    }

    // A clean boot RESETS the count. Without that, four crashes spread over a year would latch
    // safe mode on a device that has been healthy the whole time.
    CHECK(tk::boot_fail_next(3, tk::BootCounterCause::Clean) == 0);
    CHECK(tk::boot_fail_next(0, tk::BootCounterCause::Clean) == 0);

    CHECK(tk::boot_fail_next(0, tk::BootCounterCause::Fault) == 1);
    CHECK(tk::boot_fail_next(2, tk::BootCounterCause::Fault) == 3);

    // Saturating: the counter must never wrap around into "healthy".
    CHECK(tk::boot_fail_next(tk::kBootFailMax, tk::BootCounterCause::Fault) == tk::kBootFailMax);
    // Defensive direct callers fail toward the latch even though production parses first.
    CHECK(tk::boot_fail_next(tk::kBootFailMax + 1u, tk::BootCounterCause::Fault) == tk::kBootFailMax);
    CHECK(tk::boot_fail_next(999999u, tk::BootCounterCause::Fault) == tk::kBootFailMax);

    CHECK(!tk::boot_safe_mode(0));
    CHECK(!tk::boot_safe_mode(tk::kBootFailThreshold - 1));
    CHECK(tk::boot_safe_mode(tk::kBootFailThreshold));
    CHECK(tk::boot_safe_mode(tk::kBootFailThreshold + 5));

    // These literal boundaries must not move with the implementation's threshold constant.
    CHECK(tk::kBootFailThreshold == 4);
    for (uint32_t failures : {0u, 1u, 2u, 3u}) CHECK(!tk::boot_safe_mode(failures));
    for (uint32_t failures : {4u, 5u}) CHECK(tk::boot_safe_mode(failures));

    // The healthy window must mean "it RAN", not "it initialised".
    CHECK(!tk::boot_healthy_elapsed(0));
    CHECK(!tk::boot_healthy_elapsed(tk::kBootHealthyS - 1));
    CHECK(tk::boot_healthy_elapsed(tk::kBootHealthyS));
}

// ─── Exact HTTP method/path routing ──────────────────────────────────────────
static void test_http_route() {
    using tk::HttpRoute;
    using tk::HttpVerb;

    static_assert(tk::kFixedHttpRoutes.size() == 25,
                  "extend the complete fixed-route matrix when a route is added");
    for (const tk::FixedHttpRoute& fixed : tk::kFixedHttpRoutes) {
        CHECK(tk::classify_http_route(fixed.verb, fixed.path) == fixed.route);

        const HttpVerb opposite = fixed.verb == HttpVerb::Get ? HttpVerb::Post : HttpVerb::Get;
        HttpRoute expected = HttpRoute::NotFound;
        for (const tk::FixedHttpRoute& candidate : tk::kFixedHttpRoutes) {
            if (candidate.verb == opposite && candidate.path == fixed.path) {
                expected = candidate.route;  // /mcp intentionally has one route per verb.
                break;
            }
        }
        CHECK(tk::classify_http_route(opposite, fixed.path) == expected);
    }

    static constexpr std::string_view vin = "5YJ3E1EA7KF000316";
    const std::string base = std::string(tk::kVehicleRoutePrefix) + std::string(vin);
    CHECK(tk::classify_http_route(HttpVerb::Get, base + "/vehicle_data") ==
          HttpRoute::VehicleData);
    CHECK(tk::classify_http_route(HttpVerb::Get, base + "/body_controller_state") ==
          HttpRoute::BodyController);
    CHECK(tk::classify_http_route(HttpVerb::Post, base + "/command/charge_start") ==
          HttpRoute::Command);

    CHECK(tk::classify_http_route(HttpVerb::Post, base + "/vehicle_data") ==
          HttpRoute::NotFound);
    CHECK(tk::classify_http_route(HttpVerb::Post, base + "/body_controller_state") ==
          HttpRoute::NotFound);
    CHECK(tk::classify_http_route(HttpVerb::Get, base + "/command/charge_start") ==
          HttpRoute::NotFound);
    CHECK(tk::classify_http_route(HttpVerb::Get, "/api/1/vehicles//vehicle_data") ==
          HttpRoute::NotFound);
    CHECK(tk::classify_http_route(HttpVerb::Post, base + "/command/") ==
          HttpRoute::NotFound);
    CHECK(tk::classify_http_route(HttpVerb::Get,
                                  "/api/1/vehicle/5YJ3E1EA7KF000316/vehicle_data") ==
          HttpRoute::NotFound);
    CHECK(tk::classify_http_route(HttpVerb::Get, base + "/vehicle_data/extra") ==
          HttpRoute::NotFound);
    CHECK(tk::classify_http_route(HttpVerb::Post,
                                  base + "/command/charge_start/extra") ==
          HttpRoute::NotFound);
    CHECK(tk::classify_http_route(HttpVerb::Get, base + "/not_vehicle_data") ==
          HttpRoute::NotFound);
    CHECK(tk::classify_http_route(HttpVerb::Get, base + "/vehicle_data?cached=1") ==
          HttpRoute::NotFound);
    CHECK(tk::classify_http_route(HttpVerb::Get, "/prefix/status") ==
          HttpRoute::NotFound);
    CHECK(tk::classify_http_route(HttpVerb::Get, "/status?next=/ota/status") ==
          HttpRoute::NotFound);
    CHECK(tk::http_path_only("/status?next=/ota/status") == "/status");
    CHECK(tk::classify_http_route(
              HttpVerb::Get, tk::http_path_only("/status?next=/ota/status")) ==
          HttpRoute::Status);
    CHECK(tk::http_path_only("/unknown?next=/status") == "/unknown");
    CHECK(tk::classify_http_route(
              HttpVerb::Get, tk::http_path_only("/unknown?next=/status")) ==
          HttpRoute::NotFound);
    CHECK(tk::classify_http_route(HttpVerb::Get, "/ota/status") == HttpRoute::OtaStatus);
    CHECK(tk::classify_http_route(HttpVerb::Post, "/set_ota") == HttpRoute::SetOta);
    CHECK(tk::classify_http_route(HttpVerb::Get, "/status") == HttpRoute::Status);
    CHECK(tk::classify_http_route(HttpVerb::Other, "/status") == HttpRoute::NotFound);
    CHECK(tk::classify_http_route(HttpVerb::Other, base + "/vehicle_data") ==
          HttpRoute::NotFound);
    CHECK(tk::classify_http_route(HttpVerb::Get, "/unknown") == HttpRoute::NotFound);

    // Exact fail-closed admission map. The seven vehicle-touching wildcard routes must return 503
    // during Booting, SafeMode and Fatal; recovery/status/config/OTA routes stay available.
    constexpr std::array<HttpRoute, 7> gated{{
        HttpRoute::Command, HttpRoute::VehicleData, HttpRoute::BodyController,
        HttpRoute::GenKeys, HttpRoute::SetVin,
        HttpRoute::Scan, HttpRoute::McpPost,
    }};
    for (HttpRoute route : gated) CHECK(tk::http_route_requires_vehicle_runtime(route));

    constexpr std::array<HttpRoute, 16> recovery{{
        HttpRoute::NotFound, HttpRoute::OtaCheck, HttpRoute::OtaUpdate,
        HttpRoute::OtaStatus, HttpRoute::SetTime, HttpRoute::SetMqtt,
        HttpRoute::SetSyslog, HttpRoute::SetWifi, HttpRoute::SetOta,
        HttpRoute::Coredump, HttpRoute::CrashDismiss, HttpRoute::Heap,
        HttpRoute::McpGet, HttpRoute::Version, HttpRoute::Status, HttpRoute::Diag,
    }};
    for (HttpRoute route : recovery) CHECK(!tk::http_route_requires_vehicle_runtime(route));
    CHECK(!tk::http_route_requires_vehicle_runtime(HttpRoute::Index));

    // ── Route conflict matrix during active OTA update ─────────────────────────────
    // Exact documented behavior from docs/API.md and docs/FEATURES.md:
    // - Five runtime/config mutation endpoints return 409 Conflict with reason "an OTA update is in progress"
    // - Two identity mutation endpoints (/gen_keys, /set_vin) return 503 via OtaIdentityMutationGuard
    // - All other routes return None (0) and are not rejected by the update conflict guard
    CHECK(tk::kOtaUpdateInProgressReason == "an OTA update is in progress");

    constexpr std::array<HttpRoute, 5> ota_conflicts_409{{
        HttpRoute::SetTime,
        HttpRoute::SetMqtt,
        HttpRoute::SetSyslog,
        HttpRoute::SetWifi,
        HttpRoute::SetOta,
    }};
    for (HttpRoute route : ota_conflicts_409) {
        CHECK(tk::http_route_ota_conflict(route) == tk::HttpRouteOtaConflict::Reject409);
        CHECK(tk::http_route_blocks_during_ota_update_409(route));
        CHECK(static_cast<int>(tk::http_route_ota_conflict(route)) == 409);
    }

    constexpr std::array<HttpRoute, 2> ota_identity_503{{
        HttpRoute::GenKeys,
        HttpRoute::SetVin,
    }};
    for (HttpRoute route : ota_identity_503) {
        CHECK(tk::http_route_ota_conflict(route) == tk::HttpRouteOtaConflict::Reject503);
        CHECK(!tk::http_route_blocks_during_ota_update_409(route));
        CHECK(static_cast<int>(tk::http_route_ota_conflict(route)) == 503);
    }

    constexpr std::array<HttpRoute, 18> ota_allowed{{
        HttpRoute::NotFound,
        HttpRoute::Command,
        HttpRoute::VehicleData,
        HttpRoute::BodyController,
        HttpRoute::OtaCheck,
        HttpRoute::OtaUpdate,
        HttpRoute::OtaStatus,
        HttpRoute::OtaChangelog,
        HttpRoute::Scan,
        HttpRoute::Coredump,
        HttpRoute::CrashDismiss,
        HttpRoute::Heap,
        HttpRoute::McpPost,
        HttpRoute::McpGet,
        HttpRoute::Version,
        HttpRoute::Status,
        HttpRoute::Diag,
        HttpRoute::Index,
    }};
    for (HttpRoute route : ota_allowed) {
        CHECK(tk::http_route_ota_conflict(route) == tk::HttpRouteOtaConflict::None);
        CHECK(!tk::http_route_blocks_during_ota_update_409(route));
        CHECK(static_cast<int>(tk::http_route_ota_conflict(route)) == 0);
    }

    // Verify fixed URL path resolution and corresponding OTA conflict mapping
    CHECK(tk::classify_http_route(HttpVerb::Post, "/send_key") == HttpRoute::NotFound);
    CHECK(tk::http_route_ota_conflict(tk::classify_http_route(HttpVerb::Post, "/set_time")) == tk::HttpRouteOtaConflict::Reject409);
    CHECK(tk::http_route_ota_conflict(tk::classify_http_route(HttpVerb::Post, "/set_mqtt")) == tk::HttpRouteOtaConflict::Reject409);
    CHECK(tk::http_route_ota_conflict(tk::classify_http_route(HttpVerb::Post, "/set_syslog")) == tk::HttpRouteOtaConflict::Reject409);
    CHECK(tk::http_route_ota_conflict(tk::classify_http_route(HttpVerb::Post, "/set_wifi")) == tk::HttpRouteOtaConflict::Reject409);
    CHECK(tk::http_route_ota_conflict(tk::classify_http_route(HttpVerb::Post, "/set_ota")) == tk::HttpRouteOtaConflict::Reject409);
    CHECK(tk::http_route_ota_conflict(tk::classify_http_route(HttpVerb::Post, "/gen_keys")) == tk::HttpRouteOtaConflict::Reject503);
    CHECK(tk::http_route_ota_conflict(tk::classify_http_route(HttpVerb::Post, "/set_vin")) == tk::HttpRouteOtaConflict::Reject503);
    CHECK(tk::http_route_ota_conflict(tk::classify_http_route(HttpVerb::Get, "/status")) == tk::HttpRouteOtaConflict::None);
    CHECK(tk::http_route_ota_conflict(tk::classify_http_route(HttpVerb::Get, "/diag")) == tk::HttpRouteOtaConflict::None);
    CHECK(tk::http_route_ota_conflict(tk::classify_http_route(HttpVerb::Get, "/heap")) == tk::HttpRouteOtaConflict::None);
    CHECK(tk::http_route_ota_conflict(tk::classify_http_route(HttpVerb::Get, "/ota/status")) == tk::HttpRouteOtaConflict::None);
}

// ─── Heap trend ring ──────────────────────────────────────────────────────────
static void test_heap_history() {
    // Tenths of a KiB, clamped — the encoding is what keeps a whole day in .bss without floats.
    CHECK(tk::heap_tenths_kib(1024) == 10);
    CHECK(tk::heap_tenths_kib(0)    == 0);
    CHECK(tk::heap_tenths_kib(512)  == 5);
    // A PSRAM-sized number must clamp rather than wrap into a negative reading.
    CHECK(tk::heap_tenths_kib(8u * 1024u * 1024u) > 0);

    tk::HeapRing r;
    CHECK(r.count() == 0);

    // Samples inside one bucket fold together and publish nothing yet: an open bucket is not a
    // measurement, and emitting it would put a half-formed value on the chart's live edge.
    r.record(0, 40u * 1024, 20u * 1024);
    r.record(10, 30u * 1024, 10u * 1024);
    CHECK(r.count() == 0);

    // Crossing into the next bucket commits the previous one.
    r.record(tk::kHeapHistoryDtS, 35u * 1024, 15u * 1024);
    CHECK(r.count() == 1);

    static tk::HeapTrendSample f[tk::kHeapHistorySamples];
    static tk::HeapTrendSample l[tk::kHeapHistorySamples];
    CHECK(r.snapshot_free(f, tk::kHeapHistorySamples) == 1);
    CHECK(r.snapshot_largest(l, tk::kHeapHistorySamples) == 1);
    // The MINIMUM of the bucket, not the last reading: on a resource ceiling the dip IS the signal,
    // and keeping the last value would draw a calm curve right up to the restart.
    CHECK(f[0] == tk::heap_tenths_kib(30u * 1024));
    CHECK(l[0] == tk::heap_tenths_kib(10u * 1024));

    // A decreasing sample folds into the open bucket without committing or rewinding it.
    tk::HeapRing backward;
    backward.record(600, 40u * 1024, 20u * 1024);
    CHECK(backward.count() == 0 && backward.bucket0() == 2);
    backward.record(300, 30u * 1024, 10u * 1024);
    CHECK(backward.count() == 0 && backward.bucket0() == 2);
    CHECK(backward.snapshot_free(f, tk::kHeapHistorySamples) == 0);
    CHECK(backward.snapshot_largest(l, tk::kHeapHistorySamples) == 0);
    backward.record(900, 35u * 1024, 15u * 1024);
    CHECK(backward.count() == 1 && backward.bucket0() == 2);
    CHECK(backward.snapshot_free(f, tk::kHeapHistorySamples) == 1);
    CHECK(backward.snapshot_largest(l, tk::kHeapHistorySamples) == 1);
    CHECK(f[0] == tk::heap_tenths_kib(30u * 1024));
    CHECK(l[0] == tk::heap_tenths_kib(10u * 1024));

    // A SKIPPED bucket is FILLED, never compressed. Compressing slides every earlier sample forward
    // in time and silently mislabels the whole curve — a chart that is wrong about WHEN.
    tk::HeapRing g;
    g.record(0, 40u * 1024, 20u * 1024);
    g.record(tk::kHeapHistoryDtS * 5, 40u * 1024, 20u * 1024);
    CHECK(g.count() == 5);
    CHECK(g.snapshot_free(f, tk::kHeapHistorySamples) == 5);
    CHECK(!tk::heap_trend_absent(f[0]));
    CHECK(tk::heap_trend_absent(f[1]));   // the gap is stated, not invented
    CHECK(tk::heap_trend_absent(f[4]));

    // Wrap-around: the ring holds a fixed span and bucket0 tracks what was evicted, so a consumer
    // can still place the series on a clock after a full day.
    tk::HeapRing w;
    for (size_t i = 0; i < tk::kHeapHistorySamples + 10; i++)
        w.record((uint32_t)i * tk::kHeapHistoryDtS, 40u * 1024, 20u * 1024);
    CHECK(w.count() == tk::kHeapHistorySamples);
    // 298 records open buckets 0..297, and the LAST one is still open — so 297 buckets are
    // published, of which the ring keeps the newest 288 and sample zero is bucket 9. The off-by-one
    // is the open-bucket rule showing through, and it is exactly what a consumer must not have to
    // guess: bucket0() is what lets it place the series on a clock after a wrap.
    CHECK(w.bucket0() == 297 - tk::kHeapHistorySamples);

    // The budget is asserted where it is spent.
    CHECK(sizeof(tk::HeapRing) <= 1536);
    CHECK(tk::kHeapHistorySamples * tk::kHeapHistoryDtS == 86400);
}

// ─── retaining the trend across a restart (logic/heap_history.hpp, HeapPersist) ────────────────
static void test_heap_persist() {
    // A zeroed image is NOT valid. This is the case that matters most: .noinit holds noise after a
    // power-on, and "all zeroes" is the single most likely accidental pattern. Adopting it would
    // publish a fabricated day of memory readings.
    tk::HeapPersist p{};
    CHECK(!tk::heap_persist_valid(p));

    // A sealed image is valid, and re-sealing is stable.
    p.magic       = tk::kHeapPersistMagic;
    p.fingerprint = tk::heap_persist_fingerprint();
    p.crc         = tk::heap_persist_crc(p);
    CHECK(tk::heap_persist_valid(p));
    CHECK(tk::heap_persist_crc(p) == p.crc);

    // Every guard is load-bearing on its own.
    { auto q = p; q.magic ^= 1u;                 CHECK(!tk::heap_persist_valid(q)); }
    { auto q = p; q.fingerprint ^= 1u;           CHECK(!tk::heap_persist_valid(q)); }
    { auto q = p; q.crc ^= 1u;                   CHECK(!tk::heap_persist_valid(q)); }
    // A single flipped bit anywhere in the RING must invalidate too — the CRC covers the payload,
    // not just the header, or a corrupted trend would be adopted under a correct-looking magic.
    { auto q = p; q.ring.free_[7] = 1234;        CHECK(!tk::heap_persist_valid(q)); }
    { auto q = p; q.ring.open_bucket_ += 1;      CHECK(!tk::heap_persist_valid(q)); }

    // The fingerprint is DERIVED, so it cannot be forgotten — it must actually depend on the
    // geometry it claims to cover.
    CHECK(tk::heap_persist_fingerprint() != 0);
    CHECK(tk::heap_persist_fingerprint() == tk::heap_persist_fingerprint());

    // ── continuity across the restart ──
    // Run a "first boot": three committed buckets, the fourth left open.
    tk::HeapPersist a{};
    a.ring.record(0,                          40u * 1024, 20u * 1024);
    a.ring.record(tk::kHeapHistoryDtS * 1,    38u * 1024, 18u * 1024);
    a.ring.record(tk::kHeapHistoryDtS * 2,    36u * 1024, 16u * 1024);
    a.ring.record(tk::kHeapHistoryDtS * 3,    34u * 1024, 14u * 1024);   // opens bucket 3
    CHECK(a.ring.count() == 3);

    // Reboot: the carry places the next boot in the bucket AFTER the one left open, so the restart
    // lands on a bucket boundary rather than folding two runs into one sample.
    const uint32_t carry = tk::heap_persist_next_carry(a.ring);
    CHECK(carry == tk::kHeapHistoryDtS * 4);

    // The new boot's clock starts at zero and is offset by the carry. The first sample closes the
    // bucket the previous run left open — 4 committed, no gap invented.
    a.ring.record(0 + carry, 32u * 1024, 12u * 1024);
    CHECK(a.ring.count() == 4);
    static tk::HeapTrendSample pf[tk::kHeapHistorySamples];
    CHECK(a.ring.snapshot_free(pf, tk::kHeapHistorySamples) == 4);
    CHECK(pf[0] == tk::heap_tenths_kib(40u * 1024));   // the pre-reboot samples are still there…
    CHECK(pf[3] == tk::heap_tenths_kib(34u * 1024));   // …including the bucket the restart closed
    CHECK(a.ring.bucket0() == 0);                      // and the timeline never restarted

    // Downtime longer than one bucket is drawn as a GAP, not silently closed up: the device was
    // off, and a compressed gap would slide every earlier sample forward in time.
    tk::HeapPersist b{};
    b.ring.record(0, 40u * 1024, 20u * 1024);
    const uint32_t carry_b = tk::heap_persist_next_carry(b.ring);
    b.ring.record(carry_b + tk::kHeapHistoryDtS * 3, 30u * 1024, 10u * 1024);
    CHECK(b.ring.count() == 4);
    CHECK(b.ring.snapshot_free(pf, tk::kHeapHistorySamples) == 4);
    CHECK(!tk::heap_trend_absent(pf[0]));
    CHECK(tk::heap_trend_absent(pf[1]));
    CHECK(tk::heap_trend_absent(pf[3]));

    // The retained header costs a handful of bytes on top of the ring, not a second copy of it.
    CHECK(sizeof(tk::HeapPersist) <= sizeof(tk::HeapRing) + 16);
}

// ─── OTA rollback health gate (logic/health_gate.hpp) ──────────────────────────────────────────
static void test_health_gate() {
    using F = tk::RuntimeHealthFailure;
    for (bool ready : {false, true}) for (bool synced : {false, true})
    for (unsigned resets : {0U, 1U, UINT32_MAX}) for (size_t heap : {size_t(0), size_t(4095), size_t(4096), SIZE_MAX}) {
        std::string calls;
        const auto sample = tk::sample_runtime_health(
            [&] { calls += 'R'; return ready; }, [&] { calls += 'S'; return synced; },
            [&] { calls += 'C'; return resets; }, [&] { calls += 'H'; return heap; }, 4096);
        const F expected = !ready ? F::NotReady : !synced ? F::HostUnsynced : resets ? F::HostReset
                           : heap < 4096 ? F::CriticalHeap : F::None;
        CHECK(sample.failure == expected && sample.healthy() == (expected == F::None));
        CHECK(calls == (!ready ? "R" : !synced ? "RS" : resets ? "RSC" : "RSCH"));
        CHECK(sample.largest == (ready && synced && !resets ? heap : 0));
    }
    for (int fault = 0; fault != 4; ++fault) {
        int calls = 0;
        auto probe = [&] { if (calls++ == fault) throw fault; return true; };
        bool caught = false;
        try { (void)tk::sample_runtime_health(probe, probe, [&] { probe(); return 0; },
                                            [&] { probe(); return size_t(4096); }, 4096); }
        catch (int value) { caught = value == fault; }
        CHECK(caught && calls == fault + 1);
    }
    size_t fresh_heap = 4096;
    auto sample = [&] { return tk::sample_runtime_health([] { return true; }, [] { return true; },
        [] { return 0; }, [&] { return fresh_heap; }, 4096); };
    CHECK(sample().healthy());
    fresh_heap = 4095;
    CHECK(!sample().healthy());

    using V = tk::HealthVerdict;
    using O = tk::OtaVerificationState;
    using E = tk::IdentityMutationEntry;
    using G = tk::OtaIdentityGateState;
    using R = tk::OtaRebootClass;
    const uint32_t base = 90, cap = 600;

    // Inside the base window nothing commits, however healthy it looks. The window is what catches
    // the image that boots, works, and then dies under load.
    CHECK(tk::health_gate_decide(0,      base, cap, true, true, true)  == V::Wait);
    CHECK(tk::health_gate_decide(89,     base, cap, true, true, true)  == V::Wait);
    CHECK(tk::health_gate_decide(base,   base, cap, true, true, true)  == V::Commit);

    // THE REGRESSION THIS EXISTS FOR: an image that boots perfectly and never gets a link. The old
    // gate was a 90-second sleep, so this case committed — and it is precisely the image no OTA can
    // fix afterwards, because the fix would have to arrive over the link it broke.
    CHECK(tk::health_gate_decide(base,   base, cap, true, false, true) == V::Wait);
    CHECK(tk::health_gate_decide(cap - 1,base, cap, true, false, true) == V::Wait);
    CHECK(tk::health_gate_decide(cap,    base, cap, true, false, true) == V::GiveUp);

    // A device with no route is legitimately offline (setup mode) — that must read as healthy, not
    // as a broken image, or an OTA installed just before the credentials were cleared would be
    // thrown away.
    CHECK(tk::health_gate_decide(base,   base, cap, false, false, true) == V::Commit);
    CHECK(tk::health_gate_decide(base-1, base, cap, false, false, true) == V::Wait);

    // Coming online exactly at the cap still commits: Commit is evaluated before GiveUp, so one
    // sample of timing cannot cost a good build.
    CHECK(tk::health_gate_decide(cap,    base, cap, true, true, true)  == V::Commit);
    CHECK(tk::health_gate_decide(cap + 60, base, cap, true, true, true) == V::Commit);

    // A working LAN is not health proof when the largest internal block is already below the
    // watchdog's critical threshold. This is the rollback regression: the old 90-second commit
    // preceded the deliberate heap restart by roughly 210 seconds and made that restart permanent.
    CHECK(tk::health_gate_decide(base, base, cap, true, true, false) == V::Wait);
    CHECK(tk::health_gate_decide(cap - 1, base, cap, true, true, false) == V::Wait);
    CHECK(tk::health_gate_decide(cap, base, cap, true, true, false) == V::GiveUp);
    CHECK(tk::health_gate_decide(base, base, cap, false, false, false) == V::Wait);

    // The shipped constants leave a real window between "earliest possible commit" and "give up".
    CHECK(tk::kHealthGateCapS > tk::kHealthGateBaseS);

    // Identity writes create journals that only this image version may know how to recover. A
    // PENDING_VERIFY reboot can roll back the executable, so every rotation entry point stays
    // mutation-free until probation completes. Unknown OTA state is fail-closed as well.
    for (const E entry : {E::HttpGenerateKey, E::HttpSetVin,
                          E::InitialKey, E::AutomaticKey, E::AutomaticEnrollment}) {
        CHECK(tk::identity_mutation_allowed(O::Stable, entry));
        CHECK(!tk::identity_mutation_allowed(O::PendingVerify, entry));
        CHECK(!tk::identity_mutation_allowed(O::Unknown, entry));

        // A stable running partition is not sufficient while an OTA worker owns the reboot path.
        // The shared operation gate closes the TOCTOU window in both directions: a mutation cannot
        // enter during OTA, and OTA cannot enter after a mutation has acquired exclusivity.
        CHECK(tk::identity_mutation_may_start(O::Stable, G::Idle, entry));
        CHECK(!tk::identity_mutation_may_start(O::Stable, G::Ota, entry));
        CHECK(!tk::identity_mutation_may_start(O::Stable, G::IdentityMutation, entry));
        CHECK(!tk::identity_mutation_may_start(O::Stable, G::FaultRestart, entry));
        CHECK(!tk::identity_mutation_may_start(O::Stable, G::HealthCommit, entry));
        CHECK(!tk::identity_mutation_may_start(O::Stable, G::ConfigRestart, entry));
        CHECK(!tk::identity_mutation_may_start(O::PendingVerify, G::Idle, entry));
        CHECK(!tk::identity_mutation_may_start(O::Unknown, G::Idle, entry));
    }
    CHECK(tk::ota_operation_may_start(G::Idle));
    CHECK(!tk::ota_operation_may_start(G::Ota));
    CHECK(!tk::ota_operation_may_start(G::IdentityMutation));
    CHECK(!tk::ota_operation_may_start(G::FaultRestart));
    CHECK(!tk::ota_operation_may_start(G::HealthCommit));
    CHECK(!tk::ota_operation_may_start(G::ConfigRestart));

    // Exercise the exact atomic CAS seam used by the firmware, not only its admission predicates.
    // Every owner blocks both peers; an owner-mismatched cleanup cannot clear the winner.
    tk::OtaIdentityOperationGate operation;
    CHECK(operation.state() == G::Idle);
    CHECK(!operation.try_begin(G::Idle));

    CHECK(operation.try_begin(G::Ota));
    CHECK(operation.state() == G::Ota);
    CHECK(!operation.try_begin(G::IdentityMutation));
    CHECK(!operation.try_begin(G::FaultRestart));
    CHECK(!operation.try_begin(G::HealthCommit));
    CHECK(!operation.try_begin(G::ConfigRestart));
    CHECK(!operation.finish(G::IdentityMutation));
    CHECK(operation.state() == G::Ota);
    CHECK(operation.finish(G::Ota));

    CHECK(operation.try_begin(G::IdentityMutation));
    CHECK(!operation.try_begin(G::Ota));
    CHECK(!operation.try_begin(G::FaultRestart));
    CHECK(!operation.try_begin(G::HealthCommit));
    CHECK(!operation.try_begin(G::ConfigRestart));
    CHECK(operation.finish(G::IdentityMutation));

    // Persistence failure: FaultRestart releases its own owner, so a postponed OTA or identity
    // operation can enter on the next attempt rather than remaining wedged for the boot.
    CHECK(operation.try_begin(G::FaultRestart));
    CHECK(!operation.try_begin(G::Ota));
    CHECK(!operation.try_begin(G::IdentityMutation));
    CHECK(!operation.try_begin(G::HealthCommit));
    CHECK(!operation.try_begin(G::ConfigRestart));
    CHECK(operation.finish(G::FaultRestart));
    CHECK(operation.try_begin(G::HealthCommit));
    CHECK(!operation.try_begin(G::Ota));
    CHECK(!operation.try_begin(G::IdentityMutation));
    CHECK(!operation.try_begin(G::FaultRestart));
    CHECK(!operation.try_begin(G::ConfigRestart));
    CHECK(operation.finish(G::HealthCommit));

    // ConfigRestart: blocks OTA and IdentityMutation, and releases on failure or persists through reboot.
    CHECK(operation.try_begin(G::ConfigRestart));
    CHECK(!operation.try_begin(G::ConfigRestart));
    CHECK(!operation.try_begin(G::Ota));
    CHECK(!operation.try_begin(G::IdentityMutation));
    CHECK(!operation.try_begin(G::FaultRestart));
    CHECK(!operation.try_begin(G::HealthCommit));
    CHECK(operation.finish(G::ConfigRestart));

    CHECK(operation.try_begin(G::Ota));
    CHECK(operation.finish(G::Ota));
    CHECK(operation.try_begin(G::IdentityMutation));
    CHECK(operation.finish(G::IdentityMutation));
    CHECK(!operation.finish(G::FaultRestart));
    CHECK(operation.state() == G::Idle);

    // Only a successful, explicit user configuration commit is authority to spend OTA rollback.
    // Automatic recovery/fault reboots must leave PENDING_VERIFY armed for the bootloader.
    CHECK(tk::ota_reboot_confirms_pending_image(R::SuccessfulUserConfigCommit));
    CHECK(!tk::ota_reboot_confirms_pending_image(R::AutomaticIdentityRecovery));
    CHECK(!tk::ota_reboot_confirms_pending_image(R::AutomaticWifiRollback));
    CHECK(!tk::ota_reboot_confirms_pending_image(R::AutomaticFaultRecovery));
}

// ─── MQTT broker URI + save-time pre-flight (logic/mqtt_uri.hpp) ───────────────────────────────
static void test_ota_recovery_memory() {
    const tk::OtaRecoveryRequirements profile{12345,6789};
    CHECK(tk::ota_recovery_required_bytes_for(tk::OtaRecoveryUse::OperationEntry,profile)==12345);
    CHECK(tk::ota_recovery_required_bytes_for(tk::OtaRecoveryUse::ImageConfirmation,profile)==12345);
    CHECK(tk::ota_recovery_required_bytes_for(tk::OtaRecoveryUse::TlsAdmission,profile)==6789);
    CHECK(tk::ota_recovery_required_bytes_for(static_cast<tk::OtaRecoveryUse>(255),profile)==0);
    CHECK(tk::ota_recovery_required_bytes_for(tk::OtaRecoveryUse::OperationEntry)==tk::kOtaRecoveryEntryRequiredB);
    CHECK(tk::ota_recovery_required_bytes_for(tk::OtaRecoveryUse::ImageConfirmation)==tk::kOtaRecoveryEntryRequiredB);
    // A fresh ordinary S3 workload completed three checks at this recovered entry point.
    // Both consumers must accept it; the next lower sample still fails closed.
    for (const auto use : {tk::OtaRecoveryUse::OperationEntry, tk::OtaRecoveryUse::ImageConfirmation}) {
        const auto required = tk::ota_recovery_required_bytes_for(use);
        CHECK(!tk::ota_recovery_memory_affordable(59391, required));
        CHECK(tk::ota_recovery_memory_affordable(59392, required));
        CHECK(tk::ota_recovery_memory_affordable(59393, required));
        for (const size_t observed : {size_t(59391), size_t(59392), size_t(59393)}) {
            const auto sample = tk::sample_runtime_health([] { return true; }, [] { return true; },
                [] { return 0U; }, [&] { return observed; }, required);
            CHECK(sample.healthy() == (observed == 59392 || observed == 59393));
            CHECK(sample.largest == observed);
        }
    }
    CHECK(tk::kOtaTlsInRecordBoundB==18445&&tk::kOtaTlsOutRecordBoundB==6157);
    CHECK(tk::kOtaTlsRecordCapacityB==tk::kOtaTlsInRecordBoundB);
    CHECK(tk::ota_recovery_required_bytes_for(tk::OtaRecoveryUse::TlsAdmission)==18445);
    CHECK(!tk::ota_recovery_requirement_valid(0));
    CHECK(tk::ota_recovery_requirement_valid(1));
    CHECK(tk::ota_recovery_requirement_valid(SIZE_MAX));
    CHECK(!tk::ota_recovery_memory_affordable(SIZE_MAX, 0));
    CHECK(!tk::ota_recovery_memory_affordable(0, 1));
    CHECK(tk::ota_recovery_memory_affordable(1, 1));
    CHECK(!tk::ota_recovery_memory_affordable(12344, 12345));
    CHECK(tk::ota_recovery_memory_affordable(12345, 12345));
    CHECK(tk::ota_recovery_memory_affordable(12346, 12345));
    CHECK(!tk::ota_recovery_memory_affordable(SIZE_MAX - 1, SIZE_MAX));
    CHECK(tk::ota_recovery_memory_affordable(SIZE_MAX, SIZE_MAX));
    for (size_t observed : {size_t(12344), size_t(12345), size_t(12346)}) {
        const auto sample = tk::sample_runtime_health([] { return true; }, [] { return true; },
            [] { return 0U; }, [&] { return observed; }, 12345);
        CHECK(sample.healthy() == (observed == 12345 || observed == 12346));
        CHECK(sample.largest == observed);
    }
    CHECK(!tk::sample_runtime_health([] { return true; }, [] { return true; },
        [] { return 0U; }, [] { return SIZE_MAX; }, 0).healthy());
}

static void test_tls_memory() {
    CHECK(tk::kTlsAttemptPolicyFloorB == 49152);
    CHECK(!tk::tls_attempt_affordable(0));
    CHECK(!tk::tls_attempt_affordable(49151));
    CHECK(tk::tls_attempt_affordable(49152));
    CHECK(tk::tls_attempt_affordable(49153));
    CHECK(tk::tls_attempt_affordable(SIZE_MAX));
    CHECK(tk::kMqttProbeHeapTlsB == tk::kTlsAttemptPolicyFloorB);
    CHECK(tk::kMqttProbeHeapPlainB == 12288);
    for (size_t n : {size_t(0), size_t(12287), size_t(12288), size_t(49151), size_t(49152)})
        CHECK(tk::mqtt_probe_affordable(n, true) == tk::tls_attempt_affordable(n));
}

static void test_mqtt_uri() {
    CHECK(tk::mqtt_trim("  host:1883 \r\n") == "host:1883");
    CHECK(tk::mqtt_trim("   ").empty());

    // Plausibility: empty DISABLES and is fine; everything else needs host:port.
    CHECK(tk::mqtt_broker_is_plausible(""));
    CHECK(tk::mqtt_broker_is_plausible("192.168.1.5:1883"));
    CHECK(tk::mqtt_broker_is_plausible("mqtts://broker.example:8883"));
    CHECK(tk::mqtt_broker_is_plausible("user:pw@broker.example:1883"));   // last colon is the port
    CHECK(!tk::mqtt_broker_is_plausible("broker.example"));               // no port
    CHECK(!tk::mqtt_broker_is_plausible("broker.example:"));
    CHECK(!tk::mqtt_broker_is_plausible(":1883"));
    CHECK(!tk::mqtt_broker_is_plausible("broker.example:0"));
    CHECK(!tk::mqtt_broker_is_plausible("broker.example:65536"));
    CHECK(!tk::mqtt_broker_is_plausible("broker.example:18a3"));
    CHECK(!tk::mqtt_broker_is_plausible("broker example:1883"));          // whitespace

    // THE RULE WORTH SHARING: credentials imply TLS. A plain-mqtt CONNECT carries the password in
    // the clear, and a credentialed broker is the one that tends to live off the trusted LAN.
    CHECK(tk::mqtt_effective_uri("host:1883", false) == "mqtt://host:1883");
    CHECK(tk::mqtt_effective_uri("host:1883", true)  == "mqtts://host:1883");
    CHECK(tk::mqtt_effective_uri("u:p@host:1883", false) == "mqtts://u:p@host:1883");
    // An explicit scheme is honoured in BOTH directions — including a deliberate downgrade.
    CHECK(tk::mqtt_effective_uri("mqtt://host:1883", true) == "mqtt://host:1883");
    CHECK(tk::mqtt_effective_uri("mqtts://host:8883", false) == "mqtts://host:8883");
    CHECK(tk::mqtt_effective_uri("   host:1883  ", false) == "mqtt://host:1883");
    CHECK(tk::mqtt_effective_uri("", true).empty());   // empty stays empty: it disables the bridge

    // Human-readable surfaces never receive embedded credentials. The client still dials the
    // original URI; this projection is only for /status, UI defaults and logs.
    CHECK(tk::mqtt_broker_display("mqtt://host:1883") == "host:1883");
    CHECK(tk::mqtt_broker_display("mqtts://user:secret@broker.example:8883") ==
          "broker.example:8883");
    CHECK(tk::mqtt_broker_display(" user@realm:pw@host:1883/path ") == "host:1883");
    CHECK(tk::mqtt_broker_display("mqtt://user:secret/path@host:1883") == "host:1883");
    CHECK(tk::mqtt_broker_display("").empty());

    CHECK(tk::mqtt_uri_is_tls("mqtts://h:8883"));
    CHECK(!tk::mqtt_uri_is_tls("mqtt://h:1883"));
    CHECK(!tk::mqtt_uri_is_tls(""));

    // The probe's memory gate. TLS costs far more than plaintext, and the number that binds is the
    // LARGEST CONTIGUOUS block — refusing to probe costs a retry, probing anyway costs a bad_alloc
    // on the HTTP task.
    CHECK(tk::mqtt_probe_heap_need(true) > tk::mqtt_probe_heap_need(false));
    CHECK(tk::mqtt_probe_affordable(64u * 1024, true));
    CHECK(!tk::mqtt_probe_affordable(20u * 1024, true));    // enough for plain, not for TLS
    CHECK(tk::mqtt_probe_affordable(20u * 1024, false));
    CHECK(!tk::mqtt_probe_affordable(4u * 1024, false));

    // The status mapping separates "your input is wrong" from "the network is not answering" from
    // "ask again later". A save endpoint that reported failure inside a 200 is how a UI ends up
    // showing a success toast for a broker that was never reachable.
    using R = tk::MqttProbeResult;
    CHECK(tk::mqtt_probe_http_status(R::Ok)          == 200);
    CHECK(tk::mqtt_probe_http_status(R::Refused)     == 400);
    CHECK(tk::mqtt_probe_http_status(R::Unreachable) == 502);
    CHECK(tk::mqtt_probe_http_status(R::Timeout)     == 502);
    CHECK(tk::mqtt_probe_http_status(R::NoHeap)      == 503);
    CHECK(tk::mqtt_probe_http_status(R::Internal)    == 500);
    // Every outcome says something, and only success says success.
    for (R r : {R::Ok, R::Unreachable, R::Refused, R::Timeout, R::NoHeap, R::Internal})
        CHECK(tk::mqtt_probe_reason(r) != nullptr && tk::mqtt_probe_reason(r)[0] != '\0');
    CHECK(std::string(tk::mqtt_probe_reason(R::Unreachable)).find("not saved") != std::string::npos);
}

// ─── Browser-origin gate for mutating HTTP requests (logic/http_origin.hpp) ────────────────────
static void test_http_origin() {
    const std::string_view owned_ips[] = {"192.0.2.10", "192.0.2.11"};
    CHECK(tk::device_host_allowed("Tesla-Key-Esp32-2.Local:80", owned_ips, 2, "tesla-key-esp32-2"));
    CHECK(tk::device_host_allowed("tesla-key-esp32-2", owned_ips, 2, "tesla-key-esp32-2"));
    CHECK(tk::device_host_allowed(tk::kDeviceHostname, owned_ips, 2, "tesla-key-esp32-2"));
    CHECK(!tk::device_host_allowed("tesla-key-esp32.local", owned_ips, 2, "tesla-key-esp32-2"));
    CHECK(!tk::device_host_allowed("tesla-key-esp32-2.local.evil", owned_ips, 2, "tesla-key-esp32-2"));
    CHECK(!tk::device_host_allowed("tesla-key-esp32.local", owned_ips, 2, ""));
    CHECK(tk::device_host_allowed("192.0.2.11", owned_ips, 2, ""));
    CHECK(!tk::device_host_allowed("192.0.2.12", owned_ips, 2, ""));
    CHECK(tk::mutation_request_allowed(false, "tesla-key-esp32-2.local",
        "http://tesla-key-esp32-2.local", "", "", false, owned_ips, 2, "tesla-key-esp32-2"));
    CHECK(!tk::mutation_request_allowed(false, "tesla-key-esp32.local",
        "http://tesla-key-esp32.local", "", "", false, owned_ips, 2, "tesla-key-esp32-2"));
    CHECK(tk::mutation_request_allowed(true, "foreign.example", "", "", "", false,
                                      owned_ips, 2, "tesla-key-esp32-2"));
    CHECK(tk::mutation_origin_required(false, "/coredump?clear=1#x"));
    CHECK(tk::mutation_origin_required(false, "/diag?verbose=1#clear=0"));
    CHECK(!tk::mutation_origin_required(false, "/diag?clear=0#clear=1"));
    CHECK(!tk::mutation_origin_required(false, "/diag?x=1#x&clear=1"));
    CHECK(!tk::mutation_origin_required(false, "/diag?clear=1%23x"));
    for (const auto& entry : tk::kFixedHttpRoutes) {
        const bool diagnostic = entry.route == tk::HttpRoute::Status || entry.route == tk::HttpRoute::Diag ||
            entry.route == tk::HttpRoute::Heap || entry.route == tk::HttpRoute::Coredump ||
            entry.route == tk::HttpRoute::OtaStatus || entry.route == tk::HttpRoute::OtaChangelog;
        CHECK(tk::http_route_requires_owned_host(entry.verb, entry.route) ==
              (entry.verb == tk::HttpVerb::Get && diagnostic));
    }
    CHECK(!tk::http_route_requires_owned_host(tk::HttpVerb::Get, tk::HttpRoute::VehicleData));
    CHECK(!tk::http_route_requires_owned_host(tk::HttpVerb::Get, tk::HttpRoute::BodyController));
    CHECK(!tk::http_route_requires_owned_host(tk::HttpVerb::Post, tk::HttpRoute::Command));
    // evcc/curl do not send browser-origin metadata and retain the documented trusted-LAN API.
    CHECK(tk::mutation_origin_allowed("", "", "", ""));
    CHECK(tk::mutation_origin_allowed("tesla-key-esp32.local", "", "none", ""));

    CHECK(tk::mutation_origin_allowed("tesla-key-esp32.local",
                                      "http://tesla-key-esp32.local", "same-origin", ""));
    CHECK(tk::mutation_origin_allowed("TESLA-KEY-ESP32.LOCAL:80",
                                      "http://tesla-key-esp32.local", "same-origin", ""));
    CHECK(tk::mutation_origin_allowed("192.0.2.42:80",
                                      "http://192.0.2.42", "same-origin", "192.0.2.42"));
    // Same-origin browser GETs may omit Origin, but their Fetch Metadata still activates Host
    // validation. Only a truly headerless evcc/curl request receives the compatibility exception.
    CHECK(tk::mutation_origin_allowed("tesla-key-esp32.local", "", "same-origin", ""));
    CHECK(!tk::mutation_origin_allowed("attacker.example", "", "same-origin", "192.0.2.42"));
    CHECK(!tk::mutation_origin_allowed("tesla-key-esp32.router.example", "", "same-origin",
                                       "192.0.2.42"));

    CHECK(!tk::mutation_origin_allowed("tesla-key-esp32.local",
                                       "https://evil.example", "cross-site", ""));
    CHECK(!tk::mutation_origin_allowed("tesla-key-esp32.local",
                                       "http://evil.example", "", ""));
    CHECK(!tk::mutation_origin_allowed("tesla-key-esp32.local", "null", "same-site", ""));
    CHECK(!tk::mutation_origin_allowed("", "http://tesla-key-esp32.local", "same-origin", ""));
    CHECK(!tk::mutation_origin_allowed("tesla-key-esp32.local",
                                       "http://tesla-key-esp32.local/path", "same-origin", ""));
    CHECK(!tk::mutation_origin_allowed("tesla-key-esp32.local",
                                       "http://user@tesla-key-esp32.local", "same-origin", ""));
    // DNS rebinding: matching Host/Origin are insufficient when the authority is not device-owned.
    CHECK(!tk::mutation_origin_allowed("attacker.example",
                                       "http://attacker.example", "same-origin", "192.0.2.42"));
    CHECK(!tk::mutation_origin_allowed("tesla-key-esp32.router.example",
                                       "http://tesla-key-esp32.router.example", "same-origin",
                                       "192.0.2.42"));
    CHECK(!tk::mutation_origin_allowed("192.0.2.99",
                                       "http://192.0.2.99", "same-origin", "192.0.2.42"));

    // Dual lease: after an Ethernet takeover the page opened through the still-valid WiFi address
    // keeps working, because every address the board holds is device-owned — and only those.
    {
        const std::string_view owned[] = {"192.0.2.42", "192.0.2.77"};
        CHECK(tk::mutation_origin_allowed("192.0.2.77", "http://192.0.2.77", "same-origin", owned, 2));
        CHECK(tk::mutation_origin_allowed("192.0.2.42:80", "http://192.0.2.42", "same-origin", owned, 2));
        CHECK(tk::mutation_origin_allowed("192.0.2.77", "", "same-origin", owned, 2));
        CHECK(tk::mutation_origin_allowed("tesla-key-esp32.local", "http://tesla-key-esp32.local",
                                          "same-origin", owned, 2));
        // An empty entry (interface without a lease) never matches an empty or absent host.
        const std::string_view partly_empty[] = {"", "192.0.2.77"};
        CHECK(tk::mutation_origin_allowed("192.0.2.77", "http://192.0.2.77", "same-origin",
                                          partly_empty, 2));
        CHECK(!tk::mutation_origin_allowed("", "http://192.0.2.77", "same-origin", partly_empty, 2));
        // Foreign hosts, DHCP FQDNs, cross-site and mismatched Origin stay rejected.
        CHECK(!tk::mutation_origin_allowed("192.0.2.99", "http://192.0.2.99", "same-origin", owned, 2));
        CHECK(!tk::mutation_origin_allowed("attacker.example", "http://attacker.example",
                                           "same-origin", owned, 2));
        CHECK(!tk::mutation_origin_allowed("192.0.2.77", "http://192.0.2.42", "same-origin", owned, 2));
        CHECK(!tk::mutation_origin_allowed("192.0.2.77", "http://192.0.2.77", "cross-site", owned, 2));
        // Headerless evcc/curl clients keep the documented compatibility.
        CHECK(tk::mutation_origin_allowed("", "", "", owned, 2));
        CHECK(tk::mutation_origin_allowed("", "", "", static_cast<const std::string_view*>(nullptr), 0));
        CHECK(!tk::mutation_origin_allowed("192.0.2.77", "http://192.0.2.77", "same-origin",
                                           static_cast<const std::string_view*>(nullptr), 0));
    }

    CHECK(tk::mutation_origin_required(true, "/status"));
    CHECK(tk::mutation_origin_required(false, "/ota/check"));
    CHECK(tk::mutation_origin_required(false, "/ota/check?ms=123"));
    CHECK(tk::mutation_origin_required(false, "/diag?clear=1"));
    CHECK(tk::mutation_origin_required(false, "/diag?redact=1&verbose=0"));
    CHECK(tk::mutation_origin_required(false, "/diag?verbose=1"));
    CHECK(tk::mutation_origin_required(false, "/coredump?clear=1"));
    CHECK(!tk::mutation_origin_required(false, "/diag"));
    CHECK(!tk::mutation_origin_required(false, "/diag?clear=0&verbose=2"));
    CHECK(!tk::mutation_origin_required(false, "/diag?next=clear=1"));
    CHECK(!tk::mutation_origin_required(false, "/coredump"));
    CHECK(!tk::mutation_origin_required(false, "/ota/status"));

    // esp_http_server matches a query KEY case-insensitively (strncasecmp on an exact-length
    // match), so the handler acts on ?CLEAR=1 exactly as on ?clear=1. Classifying only the
    // lowercase spelling left every one of these mutating GETs ungated — the core-dump erase
    // included. The classifier must agree with the parser it guards.
    CHECK(tk::mutation_origin_required(false, "/diag?CLEAR=1"));
    CHECK(tk::mutation_origin_required(false, "/diag?Clear=1"));
    CHECK(tk::mutation_origin_required(false, "/diag?VERBOSE=1"));
    CHECK(tk::mutation_origin_required(false, "/diag?Verbose=0"));
    CHECK(tk::mutation_origin_required(false, "/coredump?CLEAR=1"));
    CHECK(tk::mutation_origin_required(false, "/diag?redact=1&VERBOSE=0"));
    // IDF stops at the FIRST case-insensitive key hit, so ?CLEAR=1&clear=0 mutates. Classifying
    // on any matching occurrence is a superset of what the handler acts on, which fails closed.
    CHECK(tk::mutation_origin_required(false, "/diag?CLEAR=1&clear=0"));
    CHECK(tk::mutation_origin_required(false, "/diag?clear=0&CLEAR=1"));

    // The VALUE half stays exact: IDF copies it verbatim (no case folding, no percent-decoding)
    // and query_param_is() strcmp()s it, so these must remain unclassified — folding the value
    // would gate requests the handler ignores, and inverting this pair is the original bug.
    CHECK(!tk::mutation_origin_required(false, "/diag?CLEAR=0"));
    CHECK(!tk::mutation_origin_required(false, "/diag?clear=%31"));
    CHECK(!tk::mutation_origin_required(false, "/diag?CLEAR=TRUE"));
    CHECK(!tk::mutation_origin_required(false, "/coredump?CLEAR=0"));
    // A key that merely CONTAINS the name is still a different parameter on both sides.
    CHECK(!tk::mutation_origin_required(false, "/diag?XCLEAR=1"));
    CHECK(!tk::mutation_origin_required(false, "/diag?CLEARED=1"));

    // F02: State-changing requests via mutation_request_allowed()
    // POST with no headers (curl/evcc) is allowed
    CHECK(tk::mutation_request_allowed(true, "", "", "", "", false, ""));
    CHECK(tk::mutation_request_allowed(true, "tesla-key-esp32.local", "", "", "", false, ""));

    // Mutating GET with no headers (Chromium plain-HTTP LAN behavior without Referer) is REJECTED
    CHECK(!tk::mutation_request_allowed(false, "", "", "", "", false, ""));
    CHECK(!tk::mutation_request_allowed(false, "tesla-key-esp32.local", "", "", "", false, ""));
    CHECK(!tk::mutation_request_allowed(false, "192.0.2.42", "", "", "", false, "192.0.2.42"));

    // Mutating GET with custom header X-Requested-With is allowed if host is owned
    CHECK(tk::mutation_request_allowed(false, "tesla-key-esp32.local", "", "", "", true, ""));
    CHECK(tk::mutation_request_allowed(false, "192.0.2.42", "", "", "", true, "192.0.2.42"));
    CHECK(!tk::mutation_request_allowed(false, "attacker.example", "", "", "", true, "192.0.2.42"));

    // Mutating GET with same-origin Referer is allowed
    CHECK(tk::mutation_request_allowed(false, "tesla-key-esp32.local", "", "",
                                       "http://tesla-key-esp32.local/diag", false, ""));
    CHECK(tk::mutation_request_allowed(false, "192.0.2.42", "", "",
                                       "http://192.0.2.42/setup", false, "192.0.2.42"));

    // Mutating GET with foreign/cross-origin Referer is REJECTED
    CHECK(!tk::mutation_request_allowed(false, "tesla-key-esp32.local", "", "",
                                        "http://evil.example/page", false, ""));
    CHECK(!tk::mutation_request_allowed(false, "192.0.2.42", "", "",
                                        "http://attacker.example/", false, "192.0.2.42"));
    CHECK(!tk::mutation_request_allowed(false, "tesla-key-esp32.local", "", "",
                                        "not-a-url", false, ""));

    // Mutating GET with Sec-Fetch-Site
    CHECK(tk::mutation_request_allowed(false, "tesla-key-esp32.local", "", "same-origin", "", false, ""));
    CHECK(!tk::mutation_request_allowed(false, "tesla-key-esp32.local", "", "cross-site", "", false, ""));

    // Null origin is rejected for both GET and POST
    CHECK(!tk::mutation_request_allowed(true, "tesla-key-esp32.local", "null", "same-origin", "", false, ""));
    CHECK(!tk::mutation_request_allowed(false, "tesla-key-esp32.local", "null", "same-origin", "", false, ""));

    // Foreign origin must NEVER pass on GET or POST, even with same-origin Referer, custom header, or Sec-Fetch-Site
    CHECK(!tk::mutation_request_allowed(false, "tesla-key-esp32.local", "http://evil.example", "",
                                        "http://tesla-key-esp32.local/diag", false, ""));
    CHECK(!tk::mutation_request_allowed(false, "tesla-key-esp32.local", "http://evil.example", "", "", true, ""));
    CHECK(!tk::mutation_request_allowed(false, "tesla-key-esp32.local", "http://evil.example", "same-origin", "", false, ""));
    CHECK(!tk::mutation_request_allowed(true, "tesla-key-esp32.local", "http://evil.example", "", "", false, ""));

    // Same-origin Origin passes on both GET and POST
    CHECK(tk::mutation_request_allowed(false, "tesla-key-esp32.local", "http://tesla-key-esp32.local", "", "", false, ""));
    CHECK(tk::mutation_request_allowed(true, "tesla-key-esp32.local", "http://tesla-key-esp32.local", "", "", false, ""));
}

// ─── Negotiated ATT payload size (logic/ble_chunk.hpp) ────────────────────────────────────────
static void test_ble_chunk() {
    CHECK(tk::ble_write_payload_for_mtu(0) == 20);
    CHECK(tk::ble_write_payload_for_mtu(3) == 20);
    CHECK(tk::ble_write_payload_for_mtu(23) == 20);
    CHECK(tk::ble_write_payload_for_mtu(24) == 21);
    CHECK(tk::ble_write_payload_for_mtu(64) == 61);
    CHECK(tk::ble_write_payload_for_mtu(247) == 244);
    CHECK(tk::ble_write_payload_for_mtu(517) == 244);
}

// ─── Deferred NimBLE host events (logic/ble_deferred_event.hpp) ──────────────
// Included only into test_logic.cpp; all behavior is the actual firmware pure FIFO header.
static void test_ble_deferred_queue() {
    using Q = tk::BleDeferredQueue;
    using K = tk::BleDeferredEventKind;
    Q queue;
    Q::Event event;
    event.generation = 77;
    CHECK(!queue.pop(event));
    CHECK(event.generation == 77);
    CHECK(!queue.push_rx(nullptr, 1, 8, 0));
    uint8_t sample[245]{};
    CHECK(!queue.push_rx(sample, 0, 8, 0));
    CHECK(!queue.push_rx(sample, 245, 8, 0));
    for (size_t total : {size_t{743}, size_t{2050}}) {
        for (size_t chunk : {size_t{1}, size_t{20}, size_t{244}}) {
            queue.clear();
            for (size_t position = 0; position < total;) {
                const size_t size = std::min(chunk, total - position);
                for (size_t i = 0; i < size; ++i) sample[i] = static_cast<uint8_t>(position + i);
                CHECK(queue.push_rx(sample, size, 8, 0));
                std::memset(sample, 0xa5, sizeof sample);
                position += size;
            }
            CHECK(queue.cells_used() == (total + 243) / 244);
            size_t read = 0;
            while (queue.pop(event)) {
                CHECK(event.kind == K::Rx && event.generation == 8);
                CHECK(event.size > 0 && event.size <= 20);
                for (size_t i = 0; i < event.size; ++i) CHECK(event.data[i] == static_cast<uint8_t>(read + i));
                read += event.size;
            }
            CHECK(read == total);
        }
    }
    for (unsigned round = 0; round != 64; ++round) {
        CHECK(queue.push_link(false, 3, 8));
        CHECK(queue.push_link(true, 4, 10));
        CHECK(queue.push_rx(sample, 1, 10, 0));
        CHECK(queue.pop(event) && event.kind == K::LinkDown && event.generation == 8 && event.conn_handle == 3);
        CHECK(queue.pop(event) && event.kind == K::LinkUp && event.generation == 10 && event.conn_handle == 4);
        CHECK(queue.pop(event) && event.kind == K::Rx && event.generation == 10 && event.size == 1);
        CHECK(queue.empty());
    }
    for (unsigned i = 0; i != 15; ++i) CHECK(queue.push_link(true, 3, 8));
    std::memset(sample, 0x31, sizeof sample);
    CHECK(queue.push_rx(sample, 243, 8, 0));
    std::memset(sample, 0x72, sizeof sample);
    CHECK(!queue.push_rx(sample, 2, 8, 3000));
    CHECK(queue.cells_used() == 16);
    unsigned links = 0;
    size_t bytes = 0;
    while (queue.pop(event)) {
        if (event.kind == K::Rx) {
            for (size_t i = 0; i < event.size; ++i) CHECK(event.data[i] == 0x31);
            bytes += event.size;
        } else ++links;
    }
    CHECK(links == 15 && bytes == 243);
    CHECK(queue.push_rx(sample, 1, 8, 0));
    CHECK(queue.push_rx(sample, 1, 10, 0));
    CHECK(queue.pop(event) && event.generation == 8 && event.size == 1);
    CHECK(queue.pop(event) && event.generation == 10 && event.size == 1);
    CHECK(queue.empty());

    auto wire = [](uint8_t value) {
        std::vector<uint8_t> result(182, value);
        result[0] = 0; result[1] = 180;
        return result;
    };
    auto drain = [&](tk::RxFramer& framer, std::vector<std::vector<uint8_t>>& frames) {
        while (queue.pop(event)) {
            CHECK(event.kind == K::Rx);
            framer.push_chunk(event.data.data(), event.size, event.received_ms,
                [&](const uint8_t* bytes, size_t size) { frames.emplace_back(bytes, bytes + size); },
                [](tk::RxFramerDropReason, size_t) {}, event.span_ms);
        }
    };
    const auto a = wire(0x11), b = wire(0x22);
    // A delayed drain must retain a real >3s arrival gap, including timer wrap.
    for (uint32_t origin : {uint32_t{0}, UINT32_MAX - 2000}) {
        queue.clear();
        tk::RxFramer framer(Q::kRxWindowMs);
        std::vector<std::vector<uint8_t>> frames;
        CHECK(queue.push_rx(a.data(), 20, 8, origin));
        CHECK(queue.push_rx(b.data(), b.size(), 8, origin + 4001));
        CHECK(queue.cells_used() == 2);
        drain(framer, frames);
        CHECK(frames.size() == 1 && frames[0] == std::vector<uint8_t>(180, 0x22));
        CHECK(framer.stats().timeout_drops == 1 && framer.stats().corrupt_length_drops == 0);
    }
    // Timely queued continuation survives arbitrarily late processing.
    {
        queue.clear();
        tk::RxFramer framer(Q::kRxWindowMs);
        std::vector<std::vector<uint8_t>> frames;
        CHECK(queue.push_rx(a.data(), 20, 8, 0)); drain(framer, frames);
        CHECK(queue.push_rx(a.data() + 20, a.size() - 20, 8, 10));
        drain(framer, frames); // No processing-time clock is substituted here.
        CHECK(frames.size() == 1 && frames[0] == std::vector<uint8_t>(180, 0x11));
        CHECK(framer.stats().timeout_drops == 0);
    }
    // A batch that fits one pop still preserves FIRST and LAST arrival. Using only
    // its last timestamp would falsely expire the old partial frame at4000ms.
    {
        queue.clear();
        tk::RxFramer framer(Q::kRxWindowMs);
        std::vector<std::vector<uint8_t>> frames;
        CHECK(queue.push_rx(a.data(), 20, 8, 0)); drain(framer, frames);
        CHECK(queue.push_rx(a.data() + 20, 1, 8, 2000));
        CHECK(queue.push_rx(a.data() + 21, 1, 8, 4000));
        CHECK(queue.cells_used() == 1);
        drain(framer, frames);
        CHECK(framer.buffered_bytes() == 22 && framer.stats().timeout_drops == 0);
        CHECK(!framer.check_timeout(7000));
        CHECK(framer.check_timeout(7001));
    }
    // Slow but continuous arrivals span multiple cells without false expiry.
    for (uint32_t origin : {uint32_t{0}, UINT32_MAX - 2000}) {
        queue.clear();
        tk::RxFramer framer(Q::kRxWindowMs);
        std::vector<std::vector<uint8_t>> frames;
        for (size_t offset = 0; offset < a.size(); offset += 20) {
            CHECK(queue.push_rx(a.data() + offset, std::min(size_t{20}, a.size() - offset),
                                8, origin + static_cast<uint32_t>(offset / 20) * 2000));
        }
        drain(framer, frames);
        CHECK(frames.size() == 1 && frames[0] == std::vector<uint8_t>(180, 0x11));
        CHECK(framer.stats().timeout_drops == 0);
    }
    for (uint32_t delta : {uint32_t{3000}, uint32_t{3001}}) {
        queue.clear();
        CHECK(queue.push_rx(sample, 1, 8, UINT32_MAX - 1000));
        CHECK(queue.push_rx(sample, 1, 8, UINT32_MAX - 1000 + delta));
        CHECK(queue.cells_used() == (delta == 3000 ? 1 : 2));
    }
    // A failed append does not renew the old cell's reception span.
    queue.clear();
    for (unsigned i = 0; i != 15; ++i) CHECK(queue.push_link(true, 3, 8));
    CHECK(queue.push_rx(sample, 243, 8, 10));
    CHECK(!queue.push_rx(sample, 2, 8, 3010));
    while (queue.pop(event)) if (event.kind == K::Rx) CHECK(event.span_ms == 0);
    // Append after partial pop and a callback straddling a cell preserve both endpoints.
    CHECK(queue.push_rx(sample, 40, 8, 100));
    CHECK(queue.pop(event) && event.size == 20 && event.span_ms == 0);
    CHECK(queue.push_rx(sample, 1, 8, 200));
    CHECK(queue.pop(event) && event.size == 20 && event.span_ms == 0);
    CHECK(queue.pop(event) && event.size == 1 && event.received_ms == 100 && event.span_ms == 100);
    CHECK(queue.push_rx(sample, 1, 8, 100));
    CHECK(queue.push_rx(sample, 244, 8, 200));
    CHECK(queue.cells_used() == 2);
    for (unsigned slice = 0; slice != 13; ++slice) {
        CHECK(queue.pop(event) && event.received_ms == 100);
        CHECK(event.span_ms == (slice == 12 ? 100 : 0));
    }
    CHECK(queue.pop(event) && event.size == 1 && event.received_ms == 200 && event.span_ms == 0);
    CHECK(queue.empty());

    // Complete-frame control: random notification partitions remain byte-identical.
    uint32_t random = 0x426u, arrival = UINT32_MAX - 10000;
    tk::RxFramer control(Q::kRxWindowMs);
    std::vector<std::vector<uint8_t>> frames;
    for (unsigned frame = 0; frame != 1000; ++frame) {
        const auto data = wire(static_cast<uint8_t>(frame));
        for (size_t offset = 0; offset < data.size();) {
            random = random * 1664525u + 1013904223u;
            const size_t size = std::min(size_t{1} + random % 244, data.size() - offset);
            arrival += (random >> 16) % 20;
            CHECK(queue.push_rx(data.data() + offset, size, 8, arrival));
            offset += size;
        }
        drain(control, frames);
        CHECK(frames.size() == frame + 1 && frames.back() == std::vector<uint8_t>(180, static_cast<uint8_t>(frame)));
    }
    CHECK(control.stats().timeout_drops == 0 && control.stats().corrupt_length_drops == 0);
}

static void test_ble_deferred_event() {
    using K = tk::BleDeferredEventKind;
    CHECK(tk::ble_deferred_event_may_apply(K::LinkUp, 8, 8));
    CHECK(tk::ble_deferred_event_may_apply(K::Rx, 8, 8));
    CHECK(!tk::ble_deferred_event_may_apply(K::LinkUp, 7, 7));  // unstable generation
    CHECK(!tk::ble_deferred_event_may_apply(K::Rx, 8, 10));    // stale old-link bytes
    CHECK(!tk::ble_deferred_event_may_apply(K::LinkUp, 10, 8));
    // A queued disconnect is an ordering barrier even if the host has already advanced to the
    // following link. The FIFO applies Down first, then the current LinkUp.
    CHECK(tk::ble_deferred_event_may_apply(K::LinkDown, 8, 10));
    CHECK(tk::ble_deferred_event_may_apply(K::LinkDown, 9, 10));
}

// ─── WiFi credential rollback ─────────────────────────────────────────────────
static void test_wifi_rollback() {
    // Only the AP's own refusal is evidence about the CREDENTIALS. Everything else — a router
    // still rebooting, a slow DHCP — says nothing about them, and treating it as evidence is what
    // destroys a perfectly good password.
    CHECK(tk::disco_class(202) == tk::DiscoClass::Auth);       // AUTH_FAIL
    CHECK(tk::disco_class(204) == tk::DiscoClass::Auth);       // HANDSHAKE_TIMEOUT
    CHECK(tk::disco_class(201) == tk::DiscoClass::ApAbsent);   // NO_AP_FOUND
    CHECK(tk::disco_class(0)   == tk::DiscoClass::None);
    CHECK(tk::disco_class(8)   == tk::DiscoClass::Other);      // an unrelated deauth

    // Nothing is decided before the ordinary boot window is even up — those auth codes double as
    // the transient handshake failures a normal connect recovers from on its own.
    tk::RollbackWatch w1{};
    CHECK(tk::rollback_step(w1, tk::DiscoClass::Auth, 5) == tk::RollbackAction::Wait);

    // ONE auth checkpoint is not enough: a single sample cannot tell a wrong password from a
    // transient SAE failure. Two consecutive ones can.
    tk::RollbackWatch w2{};
    CHECK(tk::rollback_step(w2, tk::DiscoClass::Auth, tk::kWifiBootWindowS)
          == tk::RollbackAction::Wait);
    CHECK(tk::rollback_step(w2, tk::DiscoClass::Auth, tk::kWifiBootWindowS * 2)
          == tk::RollbackAction::RollBack);

    // A non-auth checkpoint BREAKS the streak, so auth samples cannot accumulate across an
    // unrelated hour into a rollback.
    tk::RollbackWatch w3{};
    CHECK(tk::rollback_step(w3, tk::DiscoClass::Auth, tk::kWifiBootWindowS)
          == tk::RollbackAction::Wait);
    CHECK(tk::rollback_step(w3, tk::DiscoClass::ApAbsent, tk::kWifiBootWindowS * 2)
          == tk::RollbackAction::Wait);
    CHECK(tk::rollback_step(w3, tk::DiscoClass::Auth, tk::kWifiBootWindowS * 3)
          == tk::RollbackAction::Wait);   // the streak restarted, it did not resume

    // An ABSENT SSID gets the full grace — a rebooting router takes minutes — but the window is
    // finite, so a genuinely wrong SSID still self-heals instead of stranding the device.
    tk::RollbackWatch w4{};
    CHECK(tk::rollback_step(w4, tk::DiscoClass::ApAbsent, tk::kWifiRollbackGraceS - 1)
          == tk::RollbackAction::Wait);
    CHECK(tk::rollback_step(w4, tk::DiscoClass::ApAbsent, tk::kWifiRollbackGraceS)
          == tk::RollbackAction::RollBack);
    // The fast path must genuinely be faster than the patient one, or the distinction buys nothing.
    CHECK(tk::kWifiRollbackGraceS > tk::kWifiBootWindowS * tk::kWifiAuthToRollback);

    // F-015: staging a /set_wifi change keeps the last PROVEN pair as the backup.
    struct Cfg {
        std::string wifi_ssid, wifi_pass, wifi_ssid_backup, wifi_pass_backup;
        bool wifi_rollback_active = false, wifi_rolled_back = false;
    };
    const std::string proven = "home", proven_pass = "secret-1";
    Cfg cfg{proven, proven_pass, "", "", false, true};
    tk::stage_wifi_change(cfg, std::string("new-a"), std::string("pass-a"));
    CHECK(cfg.wifi_ssid == "new-a" && cfg.wifi_pass == "pass-a");
    CHECK(cfg.wifi_ssid_backup == proven && cfg.wifi_pass_backup == proven_pass);
    CHECK(cfg.wifi_rollback_active && !cfg.wifi_rolled_back);
    // A second change before the first was ever tried (restart deferred) must not copy the
    // untried pair over the proven backup.
    tk::stage_wifi_change(cfg, std::string("new-b"), std::string("pass-b"));
    CHECK(cfg.wifi_ssid == "new-b" && cfg.wifi_pass == "pass-b");
    CHECK(cfg.wifi_ssid_backup == proven && cfg.wifi_pass_backup == proven_pass);
    CHECK(cfg.wifi_rollback_active);
    // Once a change is proven (the boot clears the trial), the next change backs up that pair.
    cfg.wifi_rollback_active = false;
    cfg.wifi_ssid_backup.clear();
    cfg.wifi_pass_backup.clear();
    tk::stage_wifi_change(cfg, std::string("new-c"), std::string("pass-c"));
    CHECK(cfg.wifi_ssid_backup == "new-b" && cfg.wifi_pass_backup == "pass-b");
    // Without stored credentials there is nothing to roll back to, so nothing is armed.
    Cfg fresh{};
    tk::stage_wifi_change(fresh, std::string("first"), std::string("pass"));
    CHECK(!fresh.wifi_rollback_active && fresh.wifi_ssid_backup.empty() && fresh.wifi_ssid == "first");
}

static void test_auto_enrollment() {
    tk::AutoEnrollmentState state;
    CHECK(tk::auto_enrollment_due(state, 0, {true, false, true, false}, false, true, true));
    CHECK(!tk::auto_enrollment_due(state, 0, {false, false, true, false}, false, true, true));
    CHECK(!tk::auto_enrollment_due(state, 0, {true, true, true, false}, false, true, true));
    CHECK(!tk::auto_enrollment_due(state, 0, {true, false, true, false}, false, false, true));
    CHECK(!tk::auto_enrollment_due(state, 0, {true, false, true, false}, false, true, false));
    CHECK(tk::auto_enrollment_due(state, 0, {true, true, true, false}, true, true, true));

    // A late NFC approval, timeouts and allocation failures use the same reserved cadence.
    // Every attempt is consumed before I/O; neither an exception nor an ambiguous write
    // permits another attempt at the same tick or in the following supervision round.
    std::vector<uint64_t> attempts;
    for (uint64_t now = 0; now <= 900000; now += 1000) {
        if (tk::auto_enrollment_due(state, now, {true, false, true, false}, false, true, true)) {
            attempts.push_back(now);
            tk::auto_enrollment_started(state, now);
            CHECK(!tk::auto_enrollment_due(state, now, {true, false, true, false}, false, true, true));
        }
    }
    CHECK((attempts == std::vector<uint64_t>{0, 60000, 180000, 420000, 720000}));
    CHECK(state.retry_ms == tk::kEnrollmentRetryMaxMs);
    CHECK(!tk::auto_enrollment_due(state, 1020000 - 1, {true, false, true, false}, false, true, true));
    CHECK(tk::auto_enrollment_due(state, 1020000, {true, false, true, false}, false, true, true));

    tk::AutoEnrollmentState flapping;
    std::vector<uint64_t> recovery_attempts;
    for (uint64_t now = 0; now <= 900000; now += 1000) {
        const bool hint = now % 2000 == 0;
        if (tk::auto_enrollment_due(flapping, now, {true, true, true, false}, hint, true, true)) {
            recovery_attempts.push_back(now);
            tk::auto_enrollment_started(flapping, now);
        }
    }
    CHECK(recovery_attempts == attempts);

    // A paired device without a recovery hint remains quiet across 15 minutes.
    for (uint64_t now = 0; now <= 900000; now += 1000) {
        CHECK(!tk::auto_enrollment_due({}, now, {true, true, true, false}, false, true, true));
    }
    // A successful handshake can remain usable when NVS persistence fails. Even a false
    // status probe must not cause another enrollment while that RAM session is available.
    for (uint64_t now = 0; now <= 900000; now += 1000) {
        CHECK(!tk::auto_enrollment_due({}, now, {true, false, true, true}, false, true, true));
    }
    // Unknown RAM or NVS state fails closed, including with a recovery hint.
    CHECK(!tk::auto_enrollment_due({}, 0, {true, false, false, false}, true, true, true));
    CHECK(!tk::auto_enrollment_due({}, 0, {false, false, true, true}, true, true, true));
    CHECK(!tk::auto_enrollment_due({}, 0, {true, true, false, false}, true, true, true));
    CHECK(tk::auto_enrollment_due({}, 0, {true, false, true, true}, true, true, true));
    // A session established between the supervisor and sender suppresses the latter recheck.
    CHECK(tk::auto_enrollment_needed({true, false, true, false}, false));
    CHECK(!tk::auto_enrollment_needed({true, false, true, true}, false));
    tk::AutoEnrollmentState late;
    unsigned sends = 0;
    for (uint64_t now = 0; now <= 900000; now += 1000) {
        const bool accepted = now >= 250000;
        if (tk::auto_enrollment_due(late, now, {true, accepted, true, false},
                false, true, true)) {
            ++sends;
            tk::auto_enrollment_started(late, now);
        }
    }
    CHECK(sends == 3);
    // OTA probation/ownership and a failed NVS read never spend the first pairing attempt.
    tk::AutoEnrollmentState held;
    CHECK(!tk::auto_enrollment_due(held, 900000, {true, false, true, false}, false, true, false));
    CHECK(!tk::auto_enrollment_due(held, 900000, {false, false, true, false}, true, true, true));
    CHECK(held.next_attempt_ms == 0);
    CHECK(tk::auto_enrollment_due(held, 900000, {true, false, true, false}, false, true, true));
    tk::auto_enrollment_started(held, UINT64_MAX - 1);
    CHECK(held.next_attempt_ms == UINT64_MAX);
    CHECK(!tk::auto_enrollment_due(held, UINT64_MAX, {true, false, true, false}, false, true, true));
}

static void test_setup_portal() {
    using tk::PortalAction;
    // Simulates the portal loop at one tick per second. The access point becomes reachable at
    // `router_up_s`; an attempt started at or after that time brings the network up on the next
    // tick. `client` is an [arrive, leave) window of a phone on the setup AP. Closing the AP fails
    // before `close_ok_s` (the shell then calls portal_close_failed and keeps the AP open).
    struct Run {
        uint32_t leave_at = 0, close_at = 0, attempts = 0, close_calls = 0;
        std::vector<uint32_t> attempt_times;
        bool attempted_with_client = false;
    };
    auto simulate = [](uint32_t router_up_s, uint32_t client_from, uint32_t client_to,
                       uint32_t horizon_s, uint32_t close_ok_s = 0) {
        Run run;
        tk::PortalState st{};
        bool ap_open = true, network_up = false, connecting = false;
        for (uint32_t t = 0; t <= horizon_s; ++t) {
            if (connecting && t > 0) network_up = true;
            connecting = false;
            const uint32_t clients = (ap_open && t >= client_from && t < client_to) ? 1 : 0;
            const PortalAction a = tk::portal_step(st, {network_up, ap_open, clients, t});
            const bool closing = a == PortalAction::CloseAp || (a == PortalAction::Leave && ap_open);
            if (closing) ++run.close_calls;
            if (closing && t < close_ok_s) {
                tk::portal_close_failed(st, t);
                continue;
            }
            if (a == PortalAction::AttemptStation) {
                ++run.attempts;
                run.attempt_times.push_back(t);
                if (clients) run.attempted_with_client = true;
                if (t >= router_up_s) connecting = true;
            } else if (a == PortalAction::CloseAp) {
                if (!run.close_at) run.close_at = t;
                ap_open = false;
            } else if (a == PortalAction::Leave) {
                run.leave_at = t;
                break;
            }
        }
        return run;
    };
    const uint32_t never = 1u << 30;

    // A router that comes up after the boot window (power cut): recovered in place, no reboot.
    // Attempts at 30, 90, 210 s; the router is back at 200 s, so the 210 s attempt connects.
    Run late = simulate(200, never, never, 3600);
    CHECK(late.leave_at == 211);
    CHECK((late.attempt_times == std::vector<uint32_t>{30, 90, 210}));
    CHECK(late.close_at == 0);

    // Backoff doubles from 30 s and is capped at 300 s; a permanently wrong password never leaves,
    // and the open setup AP closes after 600 s without a client while retries continue.
    Run wrong = simulate(never, never, never, 4000);
    CHECK(wrong.leave_at == 0);
    CHECK(wrong.close_at == tk::kPortalIdleCloseS);
    CHECK(wrong.attempt_times.size() >= 6);
    if (wrong.attempt_times.size() >= 6) {
        CHECK((std::vector<uint32_t>(wrong.attempt_times.begin(), wrong.attempt_times.begin() + 6) ==
               std::vector<uint32_t>{30, 90, 210, 450, 750, 1050}));
    }
    for (size_t i = 5; i < wrong.attempt_times.size(); ++i)
        CHECK(wrong.attempt_times[i] - wrong.attempt_times[i - 1] == tk::kPortalRetryMaxS);

    // A phone on the setup AP defers attempts (a scan would drop it off the channel) and the idle
    // bound restarts when it leaves.
    Run busy = simulate(never, 20, 400, 2000);
    CHECK(!busy.attempted_with_client);
    CHECK(!busy.attempt_times.empty() && busy.attempt_times.front() == 400);
    CHECK(busy.close_at == 399 + tk::kPortalIdleCloseS);

    // A station that never leaves the AP (anyone in range, or a phone that remembered the setup
    // network) cannot hold it open or defer the retries past the open-time cap.
    Run squatter = simulate(never, 10, never, 4000);
    CHECK(squatter.close_at == tk::kPortalApMaxOpenS);
    CHECK(!squatter.attempted_with_client);
    CHECK(!squatter.attempt_times.empty() && squatter.attempt_times.front() == tk::kPortalApMaxOpenS + 1);

    // The network coming up while a station is on the AP: left at once, not parked behind it.
    Run parked = simulate(0, 31, never, 4000);
    CHECK((parked.attempt_times == std::vector<uint32_t>{30}));
    CHECK(parked.leave_at == 31);

    tk::PortalState st{};
    CHECK(tk::portal_step(st, {false, true, 1, 100}) == PortalAction::None);
    CHECK(tk::portal_step(st, {true, true, 1, 101}) == PortalAction::Leave);
    // A wired lease counts as "network up" too, also after the AP closed, and Leave wins over
    // the open-time cap.
    tk::PortalState wired{};
    CHECK(tk::portal_step(wired, {true, false, 0, 5}) == PortalAction::Leave);
    tk::PortalState capped{};
    CHECK(tk::portal_step(capped, {true, true, 1, tk::kPortalApMaxOpenS}) == PortalAction::Leave);

    // An AP close that keeps failing is retried with backoff (600, 630, 690, 810, 1050, then every
    // 300 s) and does not crowd out the attempts, which keep their own schedule.
    Run stuck = simulate(never, never, never, 4000, never);
    CHECK(stuck.close_calls == 14);
    CHECK(stuck.close_at == 0 && stuck.leave_at == 0);
    CHECK(stuck.attempts == wrong.attempts);
    // Leave with a failing close: the boot does not continue with the AP up; the close is retried
    // at 241, 301 and 421 s and succeeds then.
    Run slow = simulate(200, never, never, 3600, 400);
    CHECK(slow.leave_at == 421);
    CHECK(slow.close_calls == 4);
    // Both at once: a station that stays associated and a close that keeps failing. Past the cap
    // the form is gone, so the station no longer defers the attempts.
    Run wedged = simulate(never, 10, never, 8000, never);
    CHECK(!wedged.attempt_times.empty() && wedged.attempt_times.front() == tk::kPortalApMaxOpenS + 1);
    CHECK(wedged.attempts >= 20);
}

// ─── Network transport seam (logic/net_link.hpp) ──────────────────────────────
static void test_net_link() {
    uint32_t generations = 0;
    generations = tk::advance_lease_generation_word(generations, tk::NetLink::Wifi);
    CHECK(tk::lease_generation_from_word(generations, tk::NetLink::Wifi) == 1);
    CHECK(tk::lease_generation_from_word(generations, tk::NetLink::Eth) == 0);
    generations = tk::advance_lease_generation_word(generations, tk::NetLink::Eth);
    CHECK(tk::lease_generation_from_word(generations, tk::NetLink::Wifi) == 1);
    CHECK(tk::lease_generation_from_word(generations, tk::NetLink::Eth) == 1);
    CHECK(tk::advance_lease_generation_word(0x0001ffffu, tk::NetLink::Wifi) == 0x00010000u);
    CHECK(tk::advance_lease_generation_word(0xffff0001u, tk::NetLink::Eth) == 0x00000001u);

    tk::GatewayBaseline wifi_baseline{};
    const tk::GatewayIdentity wifi_a{tk::NetLink::Wifi, 0x01020304, 1};
    const tk::GatewayIdentity wifi_b{tk::NetLink::Wifi, 0x05060708, 2};
    const tk::GatewayIdentity wifi_new_lease{tk::NetLink::Wifi, 0x01020304, 3};
    CHECK(!wifi_baseline.observe(wifi_a, false));
    CHECK(wifi_baseline.observe(wifi_a, true));
    CHECK(wifi_baseline.observe(wifi_a, false));
    CHECK(!wifi_baseline.observe(wifi_b, false));  // new DHCP gateway cannot inherit A
    CHECK(wifi_baseline.observe(wifi_b, true));
    CHECK(!wifi_baseline.observe(wifi_a, false));  // returning to A is conservative
    CHECK(!wifi_baseline.observe(wifi_new_lease, false)); // same IP, new lease is unproven
    CHECK(!wifi_baseline.observe({tk::NetLink::Wifi, 0, 3}, true));
    // The case above starts from an UNPROVEN baseline, so it cannot see a dropped generation
    // comparison. Here the old lease is proven right before only its generation changes.
    tk::GatewayBaseline same_address{};
    CHECK(same_address.observe(wifi_a, true));
    CHECK(same_address.observe(wifi_a, false));          // proven for this lease
    CHECK(!same_address.observe(wifi_new_lease, false)); // new lease, no reply yet: unproven
    tk::GatewayBaseline eth_baseline{};
    CHECK(!eth_baseline.observe({tk::NetLink::Eth, wifi_a.address, 1}, false));
    // A route switch does not discard proof for a still-held WiFi lease. If Ethernet then
    // disappears, the watchdog must be able to recover a WiFi ghost association.
    tk::GatewayBaseline failover_baselines[2]{};
    CHECK(failover_baselines[0].observe(wifi_a, true));
    CHECK(!failover_baselines[1].observe({tk::NetLink::Eth, wifi_a.address, 1}, false));
    CHECK(failover_baselines[0].observe(wifi_a, false));
    // The transport identity is what the presenters branch on; the strings are what /status
    // and the logs print, so pin both.
    CHECK(std::string(tk::net_link_str(tk::NetLink::None)) == "none");
    CHECK(std::string(tk::net_link_str(tk::NetLink::Wifi)) == "wifi");
    CHECK(std::string(tk::net_link_str(tk::NetLink::Eth))  == "eth");

    // Which transport carries the route. The case that matters is BOTH leases held: a board
    // that fell back to WiFi and later had a cable plugged in. Losing the wire there must fall
    // BACK to WiFi, not to "no network" — the bug this function exists to make unrepresentable.
    CHECK(tk::net_link_active(false, false) == tk::NetLink::None);
    CHECK(tk::net_link_active(false, true)  == tk::NetLink::Wifi);
    CHECK(tk::net_link_active(true,  false) == tk::NetLink::Eth);
    CHECK(tk::net_link_active(true,  true)  == tk::NetLink::Eth);   // the wire outranks the radio
    // …and the fall-back itself, spelled out as the transition it is.
    CHECK(tk::net_link_active(/*eth*/false, /*wifi*/true) == tk::NetLink::Wifi);

    using WA = tk::WatchAction;

    // A DOWN link is not the watchdog's business: the transport's own reconnect path owns it,
    // and the ghost case it exists for is link-up by definition.
    { tk::LinkWatch w; CHECK(tk::watch_step(w, /*up*/false, /*gw*/false, /*ever*/true) == WA::Idle);
      CHECK(w.fails == 0); }

    // A healthy sample clears the run — only UNBROKEN failures count, so one dropped echo on a
    // busy LAN can never accumulate into a re-association.
    { tk::LinkWatch w;
      CHECK(tk::watch_step(w, true, /*gw*/false, true) == WA::Wait);   // 1 of 2
      CHECK(tk::watch_step(w, true, /*gw*/true,  true) == WA::Idle);   // recovered
      CHECK(w.fails == 0);
      CHECK(tk::watch_step(w, true, /*gw*/false, true) == WA::Wait); } // counting restarts, not resumes

    // Two consecutive failures on a gateway that HAS answered before ⇒ a proven ghost link.
    { tk::LinkWatch w;
      CHECK(tk::watch_step(w, true, false, /*ever*/true) == WA::Wait);
      CHECK(tk::watch_step(w, true, false, /*ever*/true) == WA::Recover); }

    // THE guard: a gateway that has NEVER answered ICMP (a router dropping LAN echo) is not
    // evidence of anything. Acting here would re-associate a perfectly healthy device every
    // ~60 s forever — the failure mode this baseline rule exists to prevent.
    { tk::LinkWatch w;
      CHECK(tk::watch_step(w, true, false, /*ever*/false) == WA::Wait);
      CHECK(tk::watch_step(w, true, false, /*ever*/false) == WA::NoBaseline);
      // …and the counter is spent, so the next window reports once more rather than on EVERY
      // subsequent sample — otherwise the /diag ring fills with one line per 30 s forever.
      CHECK(tk::watch_step(w, true, false, /*ever*/false) == WA::Wait); }

    // The gateway becoming reachable mid-run retires the suspicion outright.
    { tk::LinkWatch w;
      CHECK(tk::watch_step(w, true, false, true) == WA::Wait);
      CHECK(tk::watch_step(w, /*up*/false, false, true) == WA::Idle);
      CHECK(w.fails == 0); }
}

// ─── /status: the `eth` object is present ONLY on a wired link ────────────────
static void test_status_eth() {
    using tk::status::Inputs;

    // A WiFi (or link-down) device must emit NO eth object at all: presence is the signal, and
    // a false-valued object would claim the board has a wire it does not have.
    { Inputs in; in.wifi_connected = true; in.wifi_ssid = "HomeNet";
      CollectEmitter e; tk::status::emit_status(in, e);
      CHECK(e.out.find("eth{") == std::string::npos);
      CHECK(e.out.find("wifi{") != std::string::npos); }

    // Wired: the object appears, and the `wifi` object stays present-but-empty exactly as it is
    // while a radio is down — dropping it would read as an older build to any consumer.
    { Inputs in; in.eth_link = true; in.eth_speed_mbps = 100; in.eth_full_duplex = true;
      CollectEmitter e; tk::status::emit_status(in, e);
      CHECK(e.out.find("eth{") != std::string::npos);
      CHECK(e.out.find("eth.link=true") != std::string::npos);
      CHECK(e.out.find("eth.speed=100") != std::string::npos);
      CHECK(e.out.find("eth.full_duplex=true") != std::string::npos);
      CHECK(e.out.find("wifi{") != std::string::npos);
      CHECK(e.out.find("wifi.ssid") == std::string::npos); }

    // Speed unknown (the PHY has not negotiated yet) is OMITTED rather than reported as 0 —
    // "0 Mbit" is a claim about the link, not an admission that we did not read it.
    { Inputs in; in.eth_link = true; in.eth_speed_mbps = 0;
      CollectEmitter e; tk::status::emit_status(in, e);
      CHECK(e.out.find("eth{") != std::string::npos);
      CHECK(e.out.find("eth.speed") == std::string::npos); }
}

// ─── Ethernet board candidates and pin validation ────────────────────────────
static void test_eth_board() {
    // Exactly two hardware-verified candidates in known priority order
    CHECK(tk::kEthDefaultCandidateCount == 2);
    CHECK(std::string(tk::kEthDefaultCandidates[0].name) == "M5Stack ATOMIC PoE");
    CHECK(std::string(tk::kEthDefaultCandidates[1].name) == "Waveshare ESP32-S3-ETH");

    // All default candidates must pass pin validation
    for (size_t i = 0; i < tk::kEthDefaultCandidateCount; ++i) {
        const auto& cand = tk::kEthDefaultCandidates[i];
        CHECK(tk::eth_candidate_pins_unique(cand));
        CHECK(tk::eth_candidate_pins_valid(cand));
        CHECK(!tk::eth_gpio_is_forbidden(cand.sclk));
        CHECK(!tk::eth_gpio_is_forbidden(cand.cs));
        CHECK(!tk::eth_gpio_is_forbidden(cand.miso));
        CHECK(!tk::eth_gpio_is_forbidden(cand.mosi));
    }

    // Pin uniqueness rejection: duplicate pins must fail
    const tk::EthSpiCandidate dup_sclk_cs = { "Dup", 5, 5, 7, 8 };
    CHECK(!tk::eth_candidate_pins_unique(dup_sclk_cs));
    CHECK(!tk::eth_candidate_pins_valid(dup_sclk_cs));

    const tk::EthSpiCandidate dup_miso_mosi = { "Dup", 5, 6, 7, 7 };
    CHECK(!tk::eth_candidate_pins_unique(dup_miso_mosi));
    CHECK(!tk::eth_candidate_pins_valid(dup_miso_mosi));

    // Forbidden pins: Strapping pins (0, 3, 45, 46)
    CHECK(tk::eth_gpio_is_forbidden(0));
    CHECK(tk::eth_gpio_is_forbidden(3));
    CHECK(tk::eth_gpio_is_forbidden(45));
    CHECK(tk::eth_gpio_is_forbidden(46));

    // Forbidden pins: nonexistent GPIO22..25 and Flash / PSRAM GPIO26..37
    for (int p = 22; p <= 37; ++p) {
        CHECK(tk::eth_gpio_is_forbidden(p));
    }

    // Forbidden pins: USB D+/D- (19, 20)
    CHECK(tk::eth_gpio_is_forbidden(19));
    CHECK(tk::eth_gpio_is_forbidden(20));

    // Forbidden pins: Out-of-bounds (<0 or >48)
    CHECK(tk::eth_gpio_is_forbidden(-1));
    CHECK(tk::eth_gpio_is_forbidden(49));
    CHECK(tk::eth_gpio_is_forbidden(261));
    CHECK(tk::eth_gpio_is_forbidden(-251));

    // Candidates using forbidden pins must be rejected
    const tk::EthSpiCandidate strap_cand = { "Strap", 0, 6, 7, 8 };
    CHECK(!tk::eth_candidate_pins_valid(strap_cand));

    const tk::EthSpiCandidate flash_cand = { "Flash", 5, 26, 7, 8 };
    CHECK(!tk::eth_candidate_pins_valid(flash_cand));

    const tk::EthSpiCandidate usb_cand = { "USB", 5, 6, 19, 8 };
    CHECK(!tk::eth_candidate_pins_valid(usb_cand));

    // Keep the original Kconfig integer until validation: these values used to narrow to GPIO5.
    const tk::EthSpiCandidate oversized_cand = { "Oversized", 261, 6, 7, 8 };
    CHECK(!tk::eth_candidate_pins_valid(oversized_cand));

    const tk::EthSpiCandidate undersized_cand = { "Undersized", -251, 6, 7, 8 };
    CHECK(!tk::eth_candidate_pins_valid(undersized_cand));
}

// ─── Bug-report redaction ─────────────────────────────────────────────────────
static void test_redact() {
    CHECK(tk::redact_or("MySSID", false) == "MySSID");
    CHECK(tk::redact_or("MySSID", true)  == tk::kRedacted);

    // Actual pinned library formats, with synthetic identifiers only. Their whole line must
    // disappear even when an SSID/URI contains a quote, delimiter or a fake diagnostic tail.
    static_assert(noexcept(tk::diag_mqtt_uri_error({})));
    for (const char* record : {
            "E (0) mqtt_client: Error parse uri (1) = mqtt://example:secret@broker.invalid",
            "E (4294967295) mqtt_client:Error parse uri (non-hexadecimal data after %) = mqtt://example:%ZZ@broker.invalid",
            "E (3) mqtt_client: Error parse uri (1) = mqtt://example:secret\nI (0) fake: private-tail"}) {
        CHECK(tk::diag_mqtt_uri_error(record));
    }
    for (const char* record : {
            "I (1) wifi:connected with E (0) mqtt_client: Error parse uri (1) = example",
            "E (1) esp-tls: mqtt_client: Error parse uri (1) = example",
            "E (1) mqtt_client: ordinary message Error parse uri (1)",
            "E (1) mqtt_client: Error transport connect",
            "E () mqtt_client: Error parse uri (1)",
            "E (x) mqtt_client: Error parse uri (1)",
            "E (1) mqtt_client_extra: Error parse uri (1)", "E (1)", "E", ""}) {
        CHECK(!tk::diag_mqtt_uri_error(record));
    }
    static_assert(noexcept(tk::diag_library_identifier_line({})));
    static_assert(noexcept(tk::diag_library_capture_content_end({})));
    static_assert(noexcept(tk::diag_capture_byte('x', 0, 1)));
    const std::string injected = "I wifi:connected with example\nI (0) fake: private-tail\r\n";
    const auto content_end = tk::diag_library_capture_content_end(injected);
    CHECK(content_end == injected.size() - 2);
    CHECK(tk::diag_capture_byte('\n', 10, content_end) == ' ');
    CHECK(tk::diag_capture_byte('\r', content_end, content_end) == '\r');
    CHECK(tk::diag_capture_byte('\n', content_end + 1, content_end) == '\n');
    CHECK(tk::diag_capture_byte('x', 10, content_end) == 'x');
    CHECK(tk::diag_library_capture_content_end("I app: multiline\nnot an identifier\n") == 0);
    CHECK(tk::diag_capture_byte('\n', 10, 0) == '\n');
    static_assert(tk::diag_redacted_capacity(288) == 288 + 18 * tk::kRedactedLength);
    const char* library_lines[] = {
        "V (1) TeslaBLE::Peer: Tesla key data: 04 01 02 03 synthetic-key-canary",
        "V (1) TeslaBLE::Peer: [Encrypt] Nonce: synthetic-nonce, Ciphertext: synthetic-packet, Tag: synthetic-tag",
        "I (1) esp_netif_handlers: sta ip: 192.0.2.4, mask: 255.255.255.0, gw: 192.0.2.1",
        "I (2) esp_netif_handlers: eth ip: 192.0.2.4, mask: 255.255.255.0, gw: 192.0.2.1",
        "I (3) esp_netif_lwip: DHCP server started on interface EXAMPLE_AP with IP: 192.0.2.4",
        "E (4) esp-tls: couldn't get hostname for :broker.invalid: getaddrinfo() returns 1, addrinfo=0x0",
        "E (5) mqtt_client: Error parse uri (1) = mqtt://example:secret@broker.invalid:1883",
        "E (6) mqtt_client: Error parse uri (non-hexadecimal data after %) = mqtt://example:%ZZ@broker.invalid:1883",
        "I (7) wifi: connected with example' , aid = 1, private-tail, bssid = 02:00:00:00:00:01",
        "I (7) wifi:connected with example' , aid = 1, private-tail, bssid = 02:00:00:00:00:01",
        "W (8) wifi: Invalid AP(02:00:00:00:00:01) SSID=example: PMF Required but private-tail",
        // Captured IDF block-ack format; the AP address is synthetic.
        "I (8) wifi:<ba-add>idx:0 (ifx:0, 02:00:00:00:00:01), tid:6, ssn:0, winSize:64",
        "I (8) wifi: <ba-add>idx:1 (ifx:0, 02:00:00:00:00:02), tid:0, ssn:1, winSize:32",
        "\x1b[0;32mI (8) wifi:<ba-add>idx:0 (ifx:0, 02:00:00:00:00:01), tid:6\x1b[0m",
        "I (8) wifi:<ba-add>idx:0 (ifx:0, 02:00:00:",
        "I (8) wifi:<ba-add>idx:0 (ifx:0, 02:00:00:00:00:01)\nI (0) fake: private-tail",
        "I (9) wifi: Using per-SSID MAC [idx:1]: 02:00:00:00:00:01",
        "I (10) wifi: addba response cb: bssid change from 02:00:00:00:00:01 to 02:00:00:00:00:02",
        "E (11) wifi: failed to get bss for bssid=02:00:00:00:00:01",
        "I (12) wifi: Extended Capabilities length:8, subtype:1, 02:00:00:00:00:01, Multiple BSSID Support",
        "I (13) wifi: pm start, type:1, aid:0x1, trans-BSSID:02:00:00:00:00:01",
        "I (14) wifi: (hal)multipleBSSIDs, max_indicator:1, index:0, transmitted_bssid[5]:0x1",
        "I (15) wifi: (spr)SRG Partial BSSID Bitmap:0x1",
        "\x1b[0;31mE (16) esp-tls: couldn't get hostname for :broker.invalid: getaddrinfo() returns 1\x1b[0m",
        "E (17) mqtt_client: Error parse uri (1) = mqtt://example:truncated-secret",
    };
    for (const char* sample : library_lines) for (const char* suffix : {"", "\n", "\r\n"}) {
        const std::string line = std::string(sample) + suffix;
        char output[tk::diag_redacted_capacity(288)]{};
        const auto result = tk::redact_diag_line_fixed(line, output, sizeof(output));
        CHECK(result.safe);
        CHECK(std::string(output, result.size) == std::string(tk::kRedacted) + suffix);
        CHECK(tk::redact_diag_line(line) == std::string(tk::kRedacted) + suffix);
        const auto tiny = tk::redact_diag_line_fixed(line, output, tk::kRedactedLength - 1);
        CHECK(!tiny.safe && tiny.size == 0);
    }
    for (const char* line : {
            "E (1) esp-tls: mbedtls_ssl_handshake returned -0x2700",
            "E (2) mqtt_client: Error transport connect",
            "E (3) esp_netif_handlers: invalid static ip",
            "I (4) wifi: state: init -> auth",
            "I (4) wifi:<ba-del>idx:0, tid:6",
            "I (4) OtherLibrary:<ba-add>idx:0 (ifx:0, 02:00:00:00:00:01)",
            "I (5) unrelated: inspect before sharing",
            "E (1) TeslaBLE::Peer: [DecryptResponse] Decryption or authentication failed: -1",
            "I (2) TeslaBLE::Peer: Resetting session counter",
            "V (3) OtherLibrary: [Encrypt] Nonce: synthetic-unrelated"}) {
        CHECK(!tk::diag_library_identifier_line(line));
        CHECK(tk::redact_diag_line(line) == line);
    }

    // A logical line must never be split into independently redacted fragments: the marker can
    // end the first fragment while a markerless secret tail starts the second. Exact-capacity
    // lines still emit normally; capacity+1 discards through LF and emits one fail-closed token.
    using LA = tk::DiagLineAction;
    tk::DiagLineFrame framing;
    static_assert(noexcept(tk::diag_line_step(framing, 'x', 4)));
    for (size_t i = 0; i < 4; ++i) {
        const tk::DiagLineStep step = tk::diag_line_step(framing, "abcd"[i], 4);
        CHECK(step.action == LA::Append);
        CHECK(step.size == i);
    }
    tk::DiagLineStep step = tk::diag_line_step(framing, '\n', 4);
    CHECK(step.action == LA::EmitLine && step.size == 4);
    CHECK(!framing.overlong && framing.size == 0);

    for (char byte : std::string("abcdeSECRET")) {
        step = tk::diag_line_step(framing, byte, 4);
    }
    CHECK(step.action == LA::IgnoreOverlong);
    CHECK(framing.overlong && framing.size == 0);
    step = tk::diag_line_step(framing, '\n', 4);
    CHECK(step.action == LA::EmitOverlong);
    CHECK(!framing.overlong && framing.size == 0);
    step = tk::diag_line_step(framing, 'x', 4);
    CHECK(step.action == LA::Append && step.size == 0);
    step = tk::diag_line_step(framing, '\n', 4);
    CHECK(step.action == LA::EmitLine && step.size == 1);

    tk::DiagLineFrame unterminated;
    for (char byte : std::string("marker=SECRET")) {
        tk::diag_line_step(unterminated, byte, 4);
    }
    CHECK(unterminated.overlong);  // route emits a static token at end-of-snapshot

    // The shipped /diag route uses this fixed-buffer seam after streaming starts. Pin both its
    // noexcept contract and its worst-case capacity formula independently of the allocating
    // convenience wrapper used by the assertions below.
    static_assert(noexcept(tk::redact_diag_line_fixed({}, nullptr, 0)));
    constexpr std::string_view fixed_input =
        "I (1) main: VIN: 5YJ3E1EA7KF000316  BLE MAC: aa:bb:cc:dd:ee:ff  "
        "Board MAC: 02:00:00:32:55:20\n";
    char fixed_output[tk::diag_redacted_capacity(fixed_input.size())]{};
    const tk::FixedDiagRedaction fixed = tk::redact_diag_line_fixed(
        fixed_input, fixed_output, sizeof(fixed_output));
    CHECK(fixed.safe);
    const std::string fixed_text(fixed_output, fixed.size);
    CHECK(fixed_text.find("5YJ3E1EA7KF000316") == std::string::npos);
    CHECK(fixed_text.find("aa:bb:cc:dd:ee:ff") == std::string::npos);
    CHECK(fixed_text.find("02:00:00:32:55:20") != std::string::npos);
    CHECK(fixed_text.back() == '\n');

    // One rule may occur more than once in the same physical log line (for example a helper that
    // prints two URIs). Redacting only the first occurrence makes the table look complete while
    // the second VIN is still exported. Keep the fixtures below the route's 255-byte line bound.
    constexpr std::string_view repeated_marker =
        "I http: /api/1/vehicles/5YJ3E1EA7KF000316/vehicle_data -> "
        "/api/1/vehicles/7SAYGDEE9PF000111/command/charge_start\n";
    static_assert(repeated_marker.size() <= 255);
    char repeated_output[tk::diag_redacted_capacity(repeated_marker.size())]{};
    const tk::FixedDiagRedaction repeated = tk::redact_diag_line_fixed(
        repeated_marker, repeated_output, sizeof(repeated_output));
    CHECK(repeated.safe);
    const std::string repeated_text(repeated_output, repeated.size);
    CHECK(repeated_text.find("5YJ3E1EA7KF000316") == std::string::npos);
    CHECK(repeated_text.find("7SAYGDEE9PF000111") == std::string::npos);
    CHECK(repeated_text.find("/vehicle_data") != std::string::npos);
    CHECK(repeated_text.find("/command/charge_start") != std::string::npos);

    // Different bounded rules can also coexist. Every matching value must be scrubbed even after
    // an earlier replacement shifted the rest of the fixed buffer.
    constexpr std::string_view different_markers =
        "I main: VIN: 5YJ3E1EA7KF000316  BLE MAC: aa:bb:cc:dd:ee:ff  "
        "Board MAC: 02:00:00:32:55:20; /api/1/vehicles/7SAYGDEE9PF000111/vehicle_data\n";
    static_assert(different_markers.size() <= 255);
    char different_output[tk::diag_redacted_capacity(different_markers.size())]{};
    const tk::FixedDiagRedaction different = tk::redact_diag_line_fixed(
        different_markers, different_output, sizeof(different_output));
    CHECK(different.safe);
    const std::string different_text(different_output, different.size);
    CHECK(different_text.find("5YJ3E1EA7KF000316") == std::string::npos);
    CHECK(different_text.find("aa:bb:cc:dd:ee:ff") == std::string::npos);
    CHECK(different_text.find("7SAYGDEE9PF000111") == std::string::npos);
    CHECK(different_text.find("02:00:00:32:55:20") != std::string::npos);

    // Capacity pressure fails closed by replacing the entire line, never by returning a prefix
    // containing the sensitive tail. If even the static token cannot fit, safe=false tells the
    // route to emit its own static token instead.
    char collapsed[tk::kRedactedLength]{};
    const tk::FixedDiagRedaction small = tk::redact_diag_line_fixed(
        "I (1) main: VIN: 5YJ3E1EA7KF000316", collapsed, sizeof(collapsed));
    CHECK(small.safe);
    CHECK(small.size == tk::kRedactedLength);
    CHECK(std::memcmp(collapsed, tk::kRedacted, tk::kRedactedLength) == 0);
    char impossible[tk::kRedactedLength - 1]{};
    const tk::FixedDiagRedaction no_room = tk::redact_diag_line_fixed(
        "I (1) main: VIN: 5YJ3E1EA7KF000316", impossible, sizeof(impossible));
    CHECK(!no_room.safe);
    CHECK(no_room.size == 0);

    // Car identifiers are scrubbed, but the replaceable board identity deliberately survives.
    const std::string boot = tk::redact_diag_line(
        "I (1234) main: VIN: 5YJ3E1EA7KF000316  BLE MAC: aa:bb:cc:dd:ee:ff  "
        "Board MAC: 02:00:00:32:55:20\n");
    CHECK(boot.find("5YJ3E1EA7KF000316") == std::string::npos);
    CHECK(boot.find("aa:bb:cc:dd:ee:ff") == std::string::npos);
    CHECK(boot.find("02:00:00:32:55:20") != std::string::npos);
    // The KEY survives. A dropped field forges an "older build that never had it" signal, and the
    // shape of the line is what tells a reader which log statement they are looking at.
    CHECK(boot.find("VIN: ")     != std::string::npos);
    CHECK(boot.find("BLE MAC: ") != std::string::npos);
    CHECK(boot.find("Board MAC: ") != std::string::npos);
    CHECK(boot.back() == '\n');   // the ring's line structure survives

    // A line the ring truncated MID-VALUE must FAIL CLOSED — redact to the end of the line rather
    // than give up. Not exotic: that is exactly what an unterminated value at the end of a wrapped
    // buffer looks like, which is when a value is most likely to be sitting there.
    const std::string cut = tk::redact_diag_line("I (1) main: VIN: 5YJ3E1EA7KF0003");
    CHECK(cut.find("5YJ3E1EA7KF0003") == std::string::npos);

    const std::string cut_ble = tk::redact_diag_line(
        "I (1) main: VIN: 5YJ3E1EA7KF000316  BLE MAC: aa:bb:cc");
    CHECK(cut_ble.find("5YJ3E1EA7KF000316") == std::string::npos);
    CHECK(cut_ble.find("aa:bb:cc") == std::string::npos);

    // An ordinary line is untouched — a redactor that mangles the log costs the diagnosis it was
    // supposed to make safe to share.
    const std::string plain = "I (1234) main: HEAP free=40000 largest_block=20000\n";
    CHECK(tk::redact_diag_line(plain) == plain);

    // The REQUEST-LOG line (http_server.cpp "REQ: %s %s"). Every evcc REST route carries the VIN
    // in its path and evcc polls on a loop, so on a working device this is the most FREQUENT VIN
    // sink in the ring — measured at 31 of 286 lines on a live board eleven minutes after boot,
    // and it was the one sink the table missed. The route must survive; only the VIN segment goes.
    const std::string req = tk::redact_diag_line(
        "I (34604) http_server: REQ: GET /api/1/vehicles/5YJ3E1EA7KF000316/vehicle_data?endpoints=charge_state\n");
    CHECK(req.find("5YJ3E1EA7KF000316") == std::string::npos);
    CHECK(req.find("/api/1/vehicles/")   != std::string::npos);
    CHECK(req.find("/vehicle_data")      != std::string::npos);   // which endpoint ran is diagnostic
    CHECK(req.back() == '\n');

    // The command route has the same shape — which COMMAND ran must survive the same way.
    const std::string cmd = tk::redact_diag_line(
        "I (1) http_server: REQ: POST /api/1/vehicles/5YJ3E1EA7KF000316/command/charge_start\n");
    CHECK(cmd.find("5YJ3E1EA7KF000316") == std::string::npos);
    CHECK(cmd.find("/command/charge_start") != std::string::npos);

    // Truncated mid-VIN (no closing '/') fails closed, like every other rule.
    const std::string req_cut = tk::redact_diag_line(
        "I (1) http_server: REQ: GET /api/1/vehicles/5YJ3E1EA7KF0003");
    CHECK(req_cut.find("5YJ3E1EA7KF0003") == std::string::npos);

    // The handler-threw line logs the SAME URI under a different prefix. It is covered for free,
    // because the rule is keyed on the URI PATH rather than a log prefix — asserted, not assumed.
    const std::string threw = tk::redact_diag_line(
        "E (1) http_server: handler for /api/1/vehicles/5YJ3E1EA7KF000316/vehicle_data threw (x)\n");
    CHECK(threw.find("5YJ3E1EA7KF000316") == std::string::npos);

    // The FAILURE branch of the ble_mac write logs the same address as the success branch under a
    // different phrase, and arrived without a rule. This table is keyed on log PHRASES, so a new
    // phrase carrying an old value is a silent leak — that is the failure mode this CHECK pins.
    const std::string mac_fail = tk::redact_diag_line(
        "W (1) vehicle_ctrl: could not persist Tesla MAC aa:bb:cc:dd:ee:ff — next boot rescans\n");
    CHECK(mac_fail.find("aa:bb:cc:dd:ee:ff") == std::string::npos);
    CHECK(mac_fail.find("next boot rescans") != std::string::npos);   // the explanation survives

    // The MQTT base topic now carries the VIN-derived HA node id. Broker, node id and raw VIN are
    // identifying; the generic HA prefix remains useful and safe diagnostic context.
    const std::string mqtt = tk::redact_diag_line(
        "I (1) mqtt_ha: MQTT bridge started → 192.168.1.5:1883 "
        "(base topic tesla-key/teslakey_5yj3e1ea7kf000316, HA prefix homeassistant)\n");
    CHECK(mqtt.find("192.168.1.5") == std::string::npos);
    CHECK(mqtt.find("5yj3e1ea7kf000316") == std::string::npos);
    CHECK(mqtt.find("base topic ") != std::string::npos);
    CHECK(mqtt.find("HA prefix homeassistant") != std::string::npos);

    // Device LAN IP lines (WiFi and Ethernet from net.cpp).
    const std::string ip_wifi = tk::redact_diag_line(
        "I (51230) net: IP: 192.168.1.42\n");
    CHECK(ip_wifi.find("192.168.1.42") == std::string::npos);
    CHECK(ip_wifi.find("net: IP: ") != std::string::npos);

    const std::string ip_eth = tk::redact_diag_line(
        "I (51230) net: IP: 192.168.1.42 (eth)\n");
    CHECK(ip_eth.find("192.168.1.42") == std::string::npos);
    CHECK(ip_eth.find("net: IP: ") != std::string::npos);

    // Interrupted VIN transition recovery log line in main.cpp:
    // "interrupted VIN change detected before key commit — restoring %s"
    const std::string vin_restored = tk::redact_diag_line(
        "W (123) main: interrupted VIN change detected before key commit — restoring 5YJ3E1EA7KF000316\n");
    CHECK(vin_restored.find("5YJ3E1EA7KF000316") == std::string::npos);
    CHECK(vin_restored.find("restoring ") != std::string::npos);
    CHECK(vin_restored.back() == '\n');

    const std::string vin_unconf = tk::redact_diag_line(
        "W (123) main: interrupted VIN change detected before key commit — restoring unconfigured VIN\n");
    CHECK(vin_unconf.find("restoring <redacted>") != std::string::npos);
    CHECK(vin_unconf.back() == '\n');

    CHECK(tk::kDiagRedactionCount == 18);
    CHECK(tk::kRedactedStatusFields == 7);
}

// ─── Captive-portal reply policy ──────────────────────────────────────────────
static void test_captive() {
    using tk::CaptiveReply;
    // The three probes each OS actually uses. A 200 + page is a heuristic Android may leave
    // undecided; a 302 + Location is the one answer all three understand.
    CHECK(tk::captive_reply_for("/hotspot-detect.html", true) == CaptiveReply::Redirect);
    CHECK(tk::captive_reply_for("/generate_204", true)        == CaptiveReply::Redirect);
    CHECK(tk::captive_reply_for("/connecttest.txt", true)     == CaptiveReply::Redirect);

    // The portal page itself must still be SERVED, or the redirect target redirects forever.
    CHECK(tk::captive_reply_for("/", true) == CaptiveReply::Page);

    // Outside setup mode NOTHING redirects: a dashboard deep link that starts bouncing to
    // 192.168.4.1 is the regression nobody reports, because it only shows up on the real device.
    CHECK(tk::captive_reply_for("/generate_204", false) == CaptiveReply::Page);
    CHECK(tk::captive_reply_for("/", false)             == CaptiveReply::Page);

    // ONE place writes the portal address, so the DNS answer, the DHCP option and the Location
    // header cannot drift apart.
    CHECK(std::string(tk::CAPTIVE_PORTAL_URI).find(tk::CAPTIVE_PORTAL_IP) != std::string::npos);

    // F-079: captive DNS replies are built, not patched into the query.
    auto query = [](uint16_t qtype, bool with_opt) {
        std::vector<unsigned char> q = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                                        0x00, static_cast<unsigned char>(with_opt ? 1 : 0)};
        for (const char* label : {"captive", "apple", "com"}) {
            q.push_back(static_cast<unsigned char>(std::strlen(label)));
            q.insert(q.end(), label, label + std::strlen(label));
        }
        q.push_back(0x00);
        q.push_back(static_cast<unsigned char>(qtype >> 8));
        q.push_back(static_cast<unsigned char>(qtype & 0xff));
        q.push_back(0x00);
        q.push_back(0x01);  // class IN
        if (with_opt) {     // EDNS OPT pseudo-record: root name, TYPE 41, UDP 1232, no options
            const unsigned char opt[] = {0x00, 0x00, 0x29, 0x04, 0xd0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            q.insert(q.end(), opt, opt + sizeof opt);
        }
        return q;
    };
    auto reply_for = [](const std::vector<unsigned char>& q, std::vector<unsigned char>& out) {
        out.assign(tk::kCaptiveDnsReplyMax, 0);
        const size_t n = tk::build_captive_dns_reply(q.data(), q.size(), out.data(), out.size());
        out.resize(n);
        return n;
    };
    auto u16 = [](const std::vector<unsigned char>& b, size_t at) { return (b[at] << 8) | b[at + 1]; };
    std::vector<unsigned char> out;
    const size_t question_end = 12 + 1 + 7 + 1 + 5 + 1 + 3 + 1 + 4;  // header + name + type/class
    for (bool with_opt : {false, true}) {
        const auto q = query(1, with_opt);
        CHECK(reply_for(q, out) == question_end + 16);
        CHECK(u16(out, 0) == 0x1234);                        // ID kept
        CHECK(out[2] == 0x81 && out[3] == 0x80);             // QR + RD, RA, RCODE 0
        CHECK(u16(out, 4) == 1 && u16(out, 6) == 1);         // one question, one answer
        CHECK(u16(out, 8) == 0 && u16(out, 10) == 0);        // the OPT record is not echoed
        CHECK(u16(out, question_end) == 0xC00C);             // the ANSWER section holds the A record
        CHECK(u16(out, question_end + 2) == 1 && u16(out, question_end + 4) == 1);
        CHECK(std::memcmp(out.data() + question_end + 12, tk::CAPTIVE_PORTAL_OCTETS, 4) == 0);
    }
    CHECK(reply_for(query(255, false), out) == question_end + 16);  // ANY gets the A answer
    CHECK(reply_for(query(28, true), out) == question_end);         // AAAA: empty NOERROR answer
    CHECK(u16(out, 6) == 0 && out[3] == 0x80);
    // Dropped: too short, a response, a non-standard opcode, two questions, a pre-filled answer,
    // a compressed or overlong name, a truncated question, a non-IN class, a too-small buffer.
    auto dropped = [&](std::vector<unsigned char> q) { return reply_for(q, out) == 0; };
    auto base = query(1, false);
    CHECK(dropped(std::vector<unsigned char>(base.begin(), base.begin() + 12)));
    auto response = base; response[2] |= 0x80;          CHECK(dropped(response));
    auto opcode = base;   opcode[2] |= 0x10;            CHECK(dropped(opcode));
    auto two = base;      two[5] = 2;                   CHECK(dropped(two));
    auto answered = base; answered[7] = 1;              CHECK(dropped(answered));
    auto authority = base; authority[9] = 1;            CHECK(dropped(authority));
    auto pointer = base;  pointer[12] = 0xC0;           CHECK(dropped(pointer));
    auto truncated = base; truncated.resize(base.size() - 2); CHECK(dropped(truncated));
    auto chaos = base;    chaos[base.size() - 1] = 3;   CHECK(dropped(chaos));
    std::vector<unsigned char> overlong(base.begin(), base.begin() + 12);
    for (int i = 0; i < 5; ++i) { overlong.push_back(63); overlong.insert(overlong.end(), 63, 'a'); }
    overlong.insert(overlong.end(), {0x00, 0x00, 0x01, 0x00, 0x01});
    CHECK(dropped(overlong));
    std::vector<unsigned char> small(16, 0);
    CHECK(tk::build_captive_dns_reply(base.data(), base.size(), small.data(), small.size()) == 0);
    // The longest legal name (255 bytes on the wire) still fits the reply bound exactly.
    std::vector<unsigned char> longest(base.begin(), base.begin() + 12);
    for (int i = 0; i < 3; ++i) { longest.push_back(63); longest.insert(longest.end(), 63, 'a'); }
    longest.push_back(61); longest.insert(longest.end(), 61, 'b');
    longest.insert(longest.end(), {0x00, 0x00, 0x01, 0x00, 0x01});
    CHECK(reply_for(longest, out) == tk::kCaptiveDnsReplyMax);
    // One byte more (256 on the wire) is dropped: its reply would overrun the bound.
    std::vector<unsigned char> too_long(base.begin(), base.begin() + 12);
    for (int i = 0; i < 3; ++i) { too_long.push_back(63); too_long.insert(too_long.end(), 63, 'a'); }
    too_long.push_back(62); too_long.insert(too_long.end(), 62, 'b');
    too_long.insert(too_long.end(), {0x00, 0x00, 0x01, 0x00, 0x01});
    CHECK(dropped(too_long));
    // A 64-byte label, the reserved label types (0b01, 0b10) and compression pointers (0b11) are
    // dropped even when the forged length lands exactly on a valid question end.
    auto forged = [&](unsigned char lead) {
        std::vector<unsigned char> q(base.begin(), base.begin() + 12);
        q.push_back(lead);
        q.insert(q.end(), lead, 'a');
        q.insert(q.end(), {0x00, 0x00, 0x01, 0x00, 0x01});
        return q;
    };
    for (unsigned char lead : {0x40, 0x80, 0xC0}) CHECK(dropped(forged(lead)));
    CHECK(reply_for(forged(63), out) == 12 + 1 + 63 + 1 + 4 + 16);  // the largest label is valid
}

// ─── Atomic config blob ───────────────────────────────────────────────────────
static void test_service_target_limits() {
    CHECK(tk::kServiceTargetInputMaxBytes == 64);
    CHECK(tk::kServiceTargetPreserveMaxBytes == 120);
    CHECK(tk::kConfigBlobEncodedMax == 473 && tk::kConfigBlobCapBytes == 512);
    for (size_t size : {63u,64u,65u,119u,120u,121u}) {
        const std::string mqtt=std::string(size-5,'h')+":1883";
        const std::string syslog=std::string(size-4,'h')+":514";
        CHECK(tk::mqtt_broker_is_plausible(mqtt) == (size<=64));
        CHECK(tk::syslog_target_is_plausible(syslog) == (size<=64));
    }
    tk::ConfigBlob config;
    config.wifi_ssid.assign(32,'s'); config.wifi_pass.assign(64,'a');
    config.wifi_ssid_backup.assign(32,'b'); config.wifi_pass_backup.assign(64,'c');
    config.vin.assign(17,'V'); config.wifi_rollback_active=true;
    tk::ConfigBlobBuffer encoded{};
    for (size_t mqtt : {0u,64u,65u,119u,120u,121u})
    for (size_t syslog : {0u,64u,65u,119u,120u,121u}) {
        config.mqtt_uri.assign(mqtt,'m'); config.syslog_uri.assign(syslog,'l');
        const size_t length=tk::config_blob_encode(config,encoded.data(),encoded.size());
        if (mqtt>120 || syslog>120) { CHECK(length==0); continue; }
        CHECK(length==233+mqtt+syslog); // fixed fields, prefixes/flags/CRC independently counted
        tk::ConfigBlob decoded;
        CHECK(tk::config_blob_decode(encoded.data(),length,decoded));
        CHECK(decoded.mqtt_uri==config.mqtt_uri && decoded.syslog_uri==config.syslog_uri);
        CHECK(decoded.wifi_ssid==config.wifi_ssid && decoded.wifi_pass==config.wifi_pass);
        CHECK(decoded.wifi_ssid_backup==config.wifi_ssid_backup && decoded.wifi_pass_backup==config.wifi_pass_backup);
        CHECK(decoded.vin==config.vin && decoded.wifi_rollback_active);
    }
}

static void test_config_store() {
    tk::ConfigBlob in;
    in.wifi_ssid = "HomeNet";  in.wifi_pass = "supersecret";
    in.wifi_ssid_backup = "OldNet"; in.wifi_pass_backup = "oldsecret";
    in.wifi_rollback_active = true; in.wifi_rolled_back = false;
    in.vin = "5YJ3E1EA7KF000316"; in.mqtt_uri = "192.168.1.5:1883";
    in.syslog_uri = "192.168.1.9:514";
    in.ota_channel = 1;
    in.has_ota = true;

    tk::ConfigBlobBuffer buf{};
    const size_t n = tk::config_blob_encode(in, buf.data(), buf.size());
    CHECK(n > 0);
    CHECK(n <= tk::kConfigBlobEncodedMax);

    tk::ConfigBlob out;
    CHECK(tk::config_blob_decode(buf.data(), n, out));
    CHECK(out.wifi_ssid == in.wifi_ssid);
    CHECK(out.wifi_pass == in.wifi_pass);
    CHECK(out.wifi_ssid_backup == in.wifi_ssid_backup);
    CHECK(out.wifi_pass_backup == in.wifi_pass_backup);
    CHECK(out.wifi_rollback_active == in.wifi_rollback_active);
    CHECK(out.vin == in.vin);
    CHECK(out.mqtt_uri == in.mqtt_uri);
    CHECK(out.syslog_uri == in.syslog_uri);
    CHECK(out.ota_channel == in.ota_channel);
    CHECK(out.has_ota);

    // An EMPTY value is a real value here — "" is how MQTT and syslog are DISABLED — so it must
    // survive a round trip rather than reading back as "unset".
    tk::ConfigBlob empty_svc = in;
    empty_svc.mqtt_uri.clear();
    empty_svc.syslog_uri.clear();
    tk::ConfigBlobBuffer b2{};
    const size_t n2 = tk::config_blob_encode(empty_svc, b2.data(), b2.size());
    tk::ConfigBlob out2;
    CHECK(n2 > 0);
    CHECK(tk::config_blob_decode(b2.data(), n2, out2));
    CHECK(out2.mqtt_uri.empty());
    CHECK(out2.syslog_uri.empty());
    CHECK(out2.wifi_ssid == "HomeNet");

    // A single flipped bit must be REFUSED, not read as a slightly different configuration. That
    // is the whole point of the CRC: a torn or decaying entry has to fail loudly enough for the
    // caller to refuse the invalid blob (failing closed rather than falling back to stale legacy
    // keys or joining a network nobody set).
    tk::ConfigBlobBuffer bad = buf;
    bad[10] = (uint8_t)(bad[10] ^ 0x01);
    tk::ConfigBlob nope;
    CHECK(!tk::config_blob_decode(bad.data(), n, nope));

    // Truncation is refused too — a short read is not a valid older blob.
    CHECK(!tk::config_blob_decode(buf.data(), n / 2, nope));
    CHECK(!tk::config_blob_decode(buf.data(), 0, nope));

    // A NEWER version is refused rather than partially read: a build that guesses at a layout it
    // does not know would publish fields it invented.
    tk::ConfigBlobBuffer future = buf;
    future[4] = (uint8_t)(tk::kConfigBlobVersion + 1);
    CHECK(!tk::config_blob_decode(future.data(), n, nope));

    // Decode leaves the target UNTOUCHED on failure — all-or-nothing, so a rejected blob can never
    // half-overwrite a good in-RAM configuration.
    tk::ConfigBlob keep;
    keep.wifi_ssid = "Untouched";
    CHECK(!tk::config_blob_decode(bad.data(), n, keep));
    CHECK(keep.wifi_ssid == "Untouched");

    // An over-long field is REFUSED, never truncated: silently storing a shortened SSID produces a
    // device that connects to a network nobody configured.
    tk::ConfigBlob too_long;
    too_long.wifi_ssid = std::string(tk::kConfigMaxSsidLen + 1, 'x');
    tk::ConfigBlobBuffer b3{};
    CHECK(tk::config_blob_encode(too_long, b3.data(), b3.size()) == 0);

    // An undersized output buffer writes NOTHING.
    tk::ConfigBlobBuffer b4{};
    CHECK(tk::config_blob_encode(in, b4.data(), 8) == 0);

    // v1 backward compatibility & downgrade safety:
    // config_blob_encode ALWAYS produces a version 1 blob with ota_channel packed in flags bits 2 and 3,
    // so any older build (e.g. v1.5.7) that only reads v1 can decode it without wiping configuration.
    tk::ConfigBlob v1_src;
    v1_src.wifi_ssid = "V1Net";
    v1_src.wifi_pass = "v1pass";
    v1_src.vin = "5YJ3E1EA7KF000316";
    v1_src.mqtt_uri = "broker:1883";
    v1_src.syslog_uri = "syslog:514";
    CHECK(!v1_src.has_ota);
    tk::ConfigBlobBuffer v1_buf{};
    const size_t v1_sz = tk::config_blob_encode(v1_src, v1_buf.data(), v1_buf.size());
    CHECK(v1_sz > 0);
    CHECK(v1_buf[4] == 1);  // always version 1

    tk::ConfigBlob v1_out;
    CHECK(tk::config_blob_decode(v1_buf.data(), v1_sz, v1_out));
    CHECK(v1_out.wifi_ssid == "V1Net");
    CHECK(v1_out.vin == "5YJ3E1EA7KF000316");
    CHECK(!v1_out.has_ota);
    CHECK(v1_out.ota_channel == 0);

    // When has_ota is true and ota_channel is 0 (Release):
    tk::ConfigBlob rel_src = v1_src;
    rel_src.has_ota = true;
    rel_src.ota_channel = 0;
    tk::ConfigBlobBuffer rel_buf{};
    const size_t rel_sz = tk::config_blob_encode(rel_src, rel_buf.data(), rel_buf.size());
    CHECK(rel_sz > 0);
    CHECK(rel_buf[4] == 1);  // still version 1
    tk::ConfigBlob rel_out;
    CHECK(tk::config_blob_decode(rel_buf.data(), rel_sz, rel_out));
    CHECK(rel_out.has_ota);
    CHECK(rel_out.ota_channel == 0);
    CHECK(rel_out.wifi_ssid == "V1Net");

    // When has_ota is true and ota_channel is 1 (Dev):
    tk::ConfigBlob dev_src = v1_src;
    dev_src.has_ota = true;
    dev_src.ota_channel = 1;
    tk::ConfigBlobBuffer dev_buf{};
    const size_t dev_sz = tk::config_blob_encode(dev_src, dev_buf.data(), dev_buf.size());
    CHECK(dev_sz > 0);
    CHECK(dev_buf[4] == 1);  // still version 1
    tk::ConfigBlob dev_out;
    CHECK(tk::config_blob_decode(dev_buf.data(), dev_sz, dev_out));
    CHECK(dev_out.has_ota);
    CHECK(dev_out.ota_channel == 1);
    CHECK(dev_out.wifi_ssid == "V1Net");

    // Legacy v1 blob from older firmware where flags only had bits 0 and 1:
    // Must decode cleanly with has_ota = false, ota_channel = 0.
    tk::ConfigBlob leg_src = v1_src;
    leg_src.wifi_rollback_active = true;
    leg_src.wifi_rolled_back = true;
    tk::ConfigBlobBuffer leg_buf{};
    const size_t leg_sz = tk::config_blob_encode(leg_src, leg_buf.data(), leg_buf.size());
    CHECK(leg_sz > 0);
    tk::ConfigBlob leg_out;
    CHECK(tk::config_blob_decode(leg_buf.data(), leg_sz, leg_out));
    CHECK(leg_out.wifi_rollback_active);
    CHECK(leg_out.wifi_rolled_back);
    CHECK(!leg_out.has_ota);
    CHECK(leg_out.ota_channel == 0);

    // Future version (version=2) is rejected by this build:
    tk::ConfigBlobBuffer v2_syn{};
    memcpy(v2_syn.data(), dev_buf.data(), dev_sz - 4);
    v2_syn[4] = 2; // future version 2
    const uint32_t v2_crc = tk::config_crc32(v2_syn.data(), dev_sz - 4);
    v2_syn[dev_sz - 4] = static_cast<uint8_t>(v2_crc & 0xFF);
    v2_syn[dev_sz - 3] = static_cast<uint8_t>((v2_crc >> 8) & 0xFF);
    v2_syn[dev_sz - 2] = static_cast<uint8_t>((v2_crc >> 16) & 0xFF);
    v2_syn[dev_sz - 1] = static_cast<uint8_t>((v2_crc >> 24) & 0xFF);
    tk::ConfigBlob v2_out;
    CHECK(!tk::config_blob_decode(v2_syn.data(), dev_sz, v2_out));

    // Recovery-setup commit: fresh credentials retire the whole previous one-shot rollback state
    // (armed bit, backup pair and the last verdict), and leave every unrelated field alone.
    {
        tk::ConfigBlob armed;
        armed.wifi_ssid = "trial-net"; armed.wifi_pass = "trial-pass";
        armed.wifi_ssid_backup = "old-net"; armed.wifi_pass_backup = "old-pass";
        armed.wifi_rollback_active = true;
        armed.wifi_rolled_back = true;
        armed.vin = "5YJ3E1EA7KF000316";
        armed.mqtt_uri = "broker.example:1883";
        armed.syslog_uri = "logs.example:514";
        armed.has_ota = true;
        armed.ota_channel = 1;

        tk::config_apply_setup_wifi(armed, "fresh-net", "fresh-pass");
        CHECK_STR(armed.wifi_ssid.c_str(), "fresh-net");
        CHECK_STR(armed.wifi_pass.c_str(), "fresh-pass");
        CHECK(armed.wifi_ssid_backup.empty());
        CHECK(armed.wifi_pass_backup.empty());
        CHECK(!armed.wifi_rollback_active);
        CHECK(!armed.wifi_rolled_back);
        CHECK_STR(armed.vin.c_str(), "5YJ3E1EA7KF000316");
        CHECK_STR(armed.mqtt_uri.c_str(), "broker.example:1883");
        CHECK_STR(armed.syslog_uri.c_str(), "logs.example:514");
        CHECK(armed.has_ota && armed.ota_channel == 1);

        // The committed blob round-trips with no rollback state, so a later failure of the fresh
        // credentials cannot restore the older network.
        tk::ConfigBlobBuffer setup_buf{};
        const size_t setup_n = tk::config_blob_encode(armed, setup_buf.data(), setup_buf.size());
        CHECK(setup_n > 0);
        tk::ConfigBlob setup_out;
        CHECK(tk::config_blob_decode(setup_buf.data(), setup_n, setup_out));
        CHECK(!setup_out.wifi_rollback_active && !setup_out.wifi_rolled_back);
        CHECK(setup_out.wifi_ssid_backup.empty() && setup_out.wifi_pass_backup.empty());
    }
}

// ─── /status: the sys block, the crash block and redaction ────────────────────
static void test_status_sys_and_redaction() {
    using tk::status::Inputs;

    // The sys block is UNCONDITIONAL — a device with no pairing, no broker and no link still has
    // to be able to say how it is doing, and this is the block a remote triage reads first.
    Inputs s;
    s.vin = "5YJ3E1EA7KF000316"; s.version = "1.4.2"; s.ip = "192.168.1.42";
    s.board_mac = "02:00:00:32:55:20";
    s.free_heap = 40960; s.min_free_heap = 20480; s.largest_block = 16384;
    s.uptime_s = 3600; s.wifi_reconnects = 2; s.reset_reason = "panic"; s.safe_mode = true;
    CollectEmitter e;
    tk::status::emit_status(s, e);
    CHECK(e.out.find("sys.board_mac=\"02:00:00:32:55:20\"") != std::string::npos);
    CHECK(e.out.find("sys.free_heap=40960")        != std::string::npos);
    CHECK(e.out.find("sys.largest_block=16384")    != std::string::npos);
    CHECK(e.out.find("sys.uptime_s=3600")          != std::string::npos);
    CHECK(e.out.find("sys.wifi_reconnects=2")      != std::string::npos);
    CHECK(e.out.find("sys.reset_reason=\"panic\"") != std::string::npos);
    CHECK(e.out.find("sys.safe_mode=true")         != std::string::npos);
    // No crash was reported, so the block is ABSENT — presence is the signal.
    CHECK(e.out.find("last_crash") == std::string::npos);

    // With a crash, every field a decoder needs is present and the PCs are hex STRINGS (a JSON
    // number would render an address in decimal in every viewer that touches the payload).
    Inputs c = s;
    c.have_crash = true; c.crash_reason = "task_wdt"; c.crash_reason_code = 6;
    c.crash_summary_available = true;
    c.crash_fault = true; c.crash_coredump = true; c.crash_task = "mqtt_pub";
    c.crash_pc = 0x400d1234; c.crash_backtrace = {0x400d1234u, 0x400d5678u};
    c.crash_corrupted = true; c.crash_elf_sha256 = "deadbeef1";
    CollectEmitter ec;
    tk::status::emit_status(c, ec);
    CHECK(ec.out.find("last_crash.reason=\"task_wdt\"")      != std::string::npos);
    CHECK(ec.out.find("last_crash.fault=true")               != std::string::npos);
    CHECK(ec.out.find("last_crash.task=\"mqtt_pub\"")        != std::string::npos);
    CHECK(ec.out.find("\"0x400d1234\"")                      != std::string::npos);
    CHECK(ec.out.find("last_crash.corrupted=true")           != std::string::npos);
    CHECK(ec.out.find("last_crash.elf_sha256=\"deadbeef1\"") != std::string::npos);

    Inputs zero = c;
    zero.crash_pc = 0; zero.crash_task.clear(); zero.crash_backtrace.clear();
    zero.crash_isr = true; zero.crash_truncated = true;
    CollectEmitter ez; tk::status::emit_status(zero, ez);
    CHECK(ez.out.find("last_crash.summary_available=true") != std::string::npos);
    CHECK(ez.out.find("last_crash.pc=0") != std::string::npos);
    CHECK(ez.out.find("last_crash.corrupted=true") != std::string::npos);
    CHECK(ez.out.find("last_crash.truncated=true") != std::string::npos);
    CHECK(ez.out.find("last_crash.isr=true") != std::string::npos);
    CHECK(ez.out.find("last_crash.task=") == std::string::npos);
    zero.crash_summary_available = false; zero.crash_query_error = true;
    zero.crash_dump_identity = tk::DumpIdentity::Unverified;
    CollectEmitter invalid; tk::status::emit_status(zero, invalid);
    CHECK(invalid.out.find("last_crash.query_error=true") != std::string::npos);
    CHECK(invalid.out.find("last_crash.pc=") == std::string::npos);
    CHECK(invalid.out.find("last_crash.corrupted=") == std::string::npos);
    CHECK(invalid.out.find("last_crash.isr=") == std::string::npos);

    // ── ?redact=1 ──────────────────────────────────────────────────────────────────────────
    // Six identifying values are substituted. The VIN is the sharpest: it names one specific
    // vehicle and is an input to Tesla's own APIs.
    Inputs r;
    r.redact = true;
    r.vin = "5YJ3E1EA7KF000316"; r.ip = "192.168.1.42"; r.version = "1.4.2";
    r.board_mac = "02:00:00:32:55:20";
    r.wifi_connected = true; r.wifi_ssid = "MyHomeNetwork"; r.wifi_rssi = -55;
    r.mqtt_broker = "192.168.1.5:1883";
    r.syslog_host = "192.168.1.9"; r.syslog_port = 514;
    r.ble_connected = true; r.ble_addr = "aa:bb:cc:dd:ee:ff";
    CollectEmitter er;
    tk::status::emit_status(r, er);
    CHECK(er.out.find("5YJ3E1EA7KF000316") == std::string::npos);
    CHECK(er.out.find("192.168.1.42")      == std::string::npos);
    CHECK(er.out.find("MyHomeNetwork")     == std::string::npos);
    CHECK(er.out.find("192.168.1.5")       == std::string::npos);
    CHECK(er.out.find("192.168.1.9")       == std::string::npos);
    CHECK(er.out.find("aa:bb:cc:dd:ee:ff") == std::string::npos);
    // The KEYS all survive, and so does everything that is NOT identifying — a report stripped of
    // its diagnostic content is not a safer report, it is a useless one.
    CHECK(er.out.find("vin=")              != std::string::npos);
    CHECK(er.out.find("wifi.ssid=")        != std::string::npos);
    CHECK(er.out.find("mqtt.broker=")      != std::string::npos);
    CHECK(er.out.find("syslog.host=")      != std::string::npos);
    CHECK(er.out.find("version=\"1.4.2\"") != std::string::npos);
    CHECK(er.out.find("wifi.rssi=-55")     != std::string::npos);
    CHECK(er.out.find("syslog.port=514")   != std::string::npos);
    CHECK(er.out.find("sys.board_mac=\"02:00:00:32:55:20\"") != std::string::npos);

    // A scanned NEIGHBOUR's MAC is other people's hardware in the reporter's home, so it is
    // redacted for the same reason the car's own is.
    // Advert names (S<hash>C) are derived from the VIN, so they are redacted too.
    Inputs d;
    d.redact = true;
    tk::status::BleDevice dev;
    dev.addr = "11:22:33:44:55:66"; dev.name = "SomeCar"; dev.rssi = -70; dev.connectable = true;
    d.devices.push_back(dev);
    CollectEmitter ed;
    tk::status::emit_status(d, ed);
    CHECK(ed.out.find("11:22:33:44:55:66") == std::string::npos);
    CHECK(ed.out.find("SomeCar") == std::string::npos);
    CHECK(ed.out.find("ble.devices.0.name=\"<redacted>\"") != std::string::npos);
    CHECK(ed.out.find("ble.devices.0.addr=\"<redacted>\"") != std::string::npos);

    // The UNREDACTED payload is what the dashboard polls: redaction is opt-in per request, and a
    // default-on version would silently break the UI it shares a builder with.
    Inputs u = r;
    u.redact = false;
    CollectEmitter eu;
    tk::status::emit_status(u, eu);
    CHECK(eu.out.find("MyHomeNetwork") != std::string::npos);

    // The rollback marker is emitted ONLY when it happened — it is sticky and rare, so presence is
    // the signal, and it is the only trace left after the reboot showed the old SSID again.
    Inputs rb;
    rb.wifi_rolled_back = true;
    CollectEmitter erb;
    tk::status::emit_status(rb, erb);
    CHECK(erb.out.find("wifi.rolled_back=true") != std::string::npos);
    CHECK(e.out.find("rolled_back") == std::string::npos);
}

// ─── Vehicle data logic (tag 115 usable_battery_level fallback) ──────────────
static void test_vehicle_data_logic() {
    ChargeStateResult cs;
    CHECK_NEAR(tk::effective_usable_soc(cs), 0.0);

    // Only nominal present -> fallback
    cs.has_battery_level = true;
    cs.battery_level = 75.0f;
    CHECK_NEAR(tk::effective_usable_soc(cs), 75.0);

    // Both present -> prefer usable
    cs.has_usable_battery_level = true;
    cs.usable_battery_level = 71.0f;
    CHECK_NEAR(tk::effective_usable_soc(cs), 71.0);

    // Only usable present
    cs.has_battery_level = false;
    CHECK_NEAR(tk::effective_usable_soc(cs), 71.0);

    // Identity epoch gating: snapshot taken before pairing cleanup must not publish
    CHECK(tk::telemetry_epoch_matches(0, 0));
    CHECK(tk::telemetry_epoch_matches(42, 42));
    CHECK(!tk::telemetry_epoch_matches(0, 1));
    CHECK(!tk::telemetry_epoch_matches(1, 0));
    CHECK(!tk::telemetry_epoch_matches(42, 43));
}

// ─── BLE RX framing (teslamotors/vehicle-command ble.go:67-105) ─────────────
static void test_rx_framing() {
    // 1. Happy path: single complete frame in one chunk
    {
        tk::RxFramer framer;
        const uint8_t chunk[] = {0x00, 0x04, 't', 'e', 's', 't'};
        std::vector<std::vector<uint8_t>> frames;
        size_t n = framer.push_chunk(chunk, sizeof(chunk), 100, [&](const uint8_t* p, size_t len) {
            frames.emplace_back(p, p + len);
        });
        CHECK(n == 1);
        CHECK(frames.size() == 1);
        CHECK(frames[0].size() == 4);
        CHECK(frames[0][0] == 't' && frames[0][3] == 't');
        CHECK(framer.buffered_bytes() == 0);
        CHECK(framer.stats().frames_completed == 1);
    }

    // 2. Split frame across multiple notifications
    {
        tk::RxFramer framer;
        std::vector<std::vector<uint8_t>> frames;
        auto on_frame = [&](const uint8_t* p, size_t len) {
            frames.emplace_back(p, p + len);
        };

        const uint8_t chunk1[] = {0x00, 0x06, 0x10, 0x20}; // header + 2 bytes (needs 4 more)
        size_t n1 = framer.push_chunk(chunk1, sizeof(chunk1), 100, on_frame);
        CHECK(n1 == 0);
        CHECK(frames.empty());
        CHECK(framer.buffered_bytes() == 4);

        const uint8_t chunk2[] = {0x30, 0x40, 0x50, 0x60}; // remaining 4 bytes
        size_t n2 = framer.push_chunk(chunk2, sizeof(chunk2), 250, on_frame);
        CHECK(n2 == 1);
        CHECK(frames.size() == 1);
        CHECK(frames[0].size() == 6);
        CHECK(frames[0][0] == 0x10 && frames[0][5] == 0x60);
        CHECK(framer.buffered_bytes() == 0);
        CHECK(framer.stats().frames_completed == 1);
    }

    // 3. Several frames in one single notification (back-to-back)
    {
        tk::RxFramer framer;
        std::vector<std::vector<uint8_t>> frames;
        const uint8_t multi[] = {
            0x00, 0x03, 1, 2, 3,       // frame 1 (len=3)
            0x00, 0x02, 10, 20,        // frame 2 (len=2)
            0x00, 0x01, 99             // frame 3 (len=1)
        };
        size_t n = framer.push_chunk(multi, sizeof(multi), 100, [&](const uint8_t* p, size_t len) {
            frames.emplace_back(p, p + len);
        });
        CHECK(n == 3);
        CHECK(frames.size() == 3);
        CHECK(frames[0].size() == 3 && frames[0][0] == 1);
        CHECK(frames[1].size() == 2 && frames[1][0] == 10);
        CHECK(frames[2].size() == 1 && frames[2][0] == 99);
        CHECK(framer.buffered_bytes() == 0);
        CHECK(framer.stats().frames_completed == 3);
    }

    // 4. Corrupt length before a split frame
    {
        tk::RxFramer framer;
        std::vector<std::vector<uint8_t>> frames;
        tk::RxFramerDropReason last_reason = tk::RxFramerDropReason::None;
        size_t last_dropped = 0;
        auto on_drop = [&](tk::RxFramerDropReason r, size_t d) {
            last_reason = r;
            last_dropped = d;
        };

        // Chunk with corrupt length (> 2048)
        const uint8_t corrupt[] = {0x10, 0x00, 0xAA, 0xBB}; // 4096 bytes > 2048
        size_t nc = framer.push_chunk(corrupt, sizeof(corrupt), 100,
            [&](const uint8_t* p, size_t len) { frames.emplace_back(p, p + len); },
            on_drop);
        CHECK(nc == 0);
        CHECK(frames.empty());
        CHECK(last_reason == tk::RxFramerDropReason::CorruptLength);
        CHECK(last_dropped == 4);
        CHECK(framer.buffered_bytes() == 0);
        CHECK(framer.stats().corrupt_length_drops == 1);

        // Next chunk is valid: should be accepted cleanly
        const uint8_t valid[] = {0x00, 0x02, 0x11, 0x22};
        size_t nv = framer.push_chunk(valid, sizeof(valid), 150,
            [&](const uint8_t* p, size_t len) { frames.emplace_back(p, p + len); },
            on_drop);
        CHECK(nv == 1);
        CHECK(frames.size() == 1);
        CHECK(frames[0].size() == 2);
        CHECK(framer.buffered_bytes() == 0);
    }

    // 5. Zero length is treated as corrupt (Tesla messages are never 0 bytes)
    {
        tk::RxFramer framer;
        tk::RxFramerDropReason last_reason = tk::RxFramerDropReason::None;
        const uint8_t zero_len[] = {0x00, 0x00, 0x01, 0x02};
        framer.push_chunk(zero_len, sizeof(zero_len), 100,
            [](const uint8_t*, size_t) {},
            [&](tk::RxFramerDropReason r, size_t) { last_reason = r; });
        CHECK(last_reason == tk::RxFramerDropReason::CorruptLength);
        CHECK(framer.buffered_bytes() == 0);
    }

    // 6. Max payload boundary (2048 is valid, 2049 is corrupt)
    {
        tk::RxFramer framer;
        const uint8_t max_valid[] = {0x08, 0x00}; // 2048
        framer.push_chunk(max_valid, sizeof(max_valid), 100, [](const uint8_t*, size_t) {});
        CHECK(framer.buffered_bytes() == 2); // awaiting 2048 payload bytes

        framer.reset();
        CHECK(framer.buffered_bytes() == 0);

        tk::RxFramerDropReason last_reason = tk::RxFramerDropReason::None;
        const uint8_t overflow_len[] = {0x08, 0x01}; // 2049 > 2048
        framer.push_chunk(overflow_len, sizeof(overflow_len), 100,
            [](const uint8_t*, size_t) {},
            [&](tk::RxFramerDropReason r, size_t) { last_reason = r; });
        CHECK(last_reason == tk::RxFramerDropReason::CorruptLength);
        CHECK(framer.buffered_bytes() == 0);
    }

    // 7. Inter-chunk timeout drops stale incomplete buffer
    {
        tk::RxFramer framer;
        tk::RxFramerDropReason last_reason = tk::RxFramerDropReason::None;
        size_t last_dropped = 0;
        auto on_drop = [&](tk::RxFramerDropReason r, size_t d) {
            last_reason = r;
            last_dropped = d;
        };

        const uint8_t partial[] = {0x00, 0x10, 1, 2, 3}; // needs 16, got 3
        framer.push_chunk(partial, sizeof(partial), 1000, [](const uint8_t*, size_t) {}, on_drop);
        CHECK(framer.buffered_bytes() == 5);
        CHECK(last_reason == tk::RxFramerDropReason::None);

        // Next chunk arrives after 1001 ms (> 1000 ms timeout)
        std::vector<std::vector<uint8_t>> frames;
        const uint8_t next_valid[] = {0x00, 0x02, 7, 8};
        size_t n = framer.push_chunk(next_valid, sizeof(next_valid), 2001,
            [&](const uint8_t* p, size_t len) { frames.emplace_back(p, p + len); },
            on_drop);
        CHECK(last_reason == tk::RxFramerDropReason::InterChunkTimeout);
        CHECK(last_dropped == 5);
        CHECK(n == 1);
        CHECK(frames.size() == 1);
        CHECK(frames[0].size() == 2 && frames[0][0] == 7);
        CHECK(framer.buffered_bytes() == 0);
        CHECK(framer.stats().timeout_drops == 1);
    }

    // 8. Null and empty chunks are safe no-ops
    {
        tk::RxFramer framer;
        CHECK(framer.push_chunk(nullptr, 0, 100, [](const uint8_t*, size_t) {}) == 0);
        const uint8_t d[] = {1};
        CHECK(framer.push_chunk(d, 0, 100, [](const uint8_t*, size_t) {}) == 0);
        CHECK(framer.buffered_bytes() == 0);
    }

    // 9. Standalone check_timeout() drops stale incomplete buffer without incoming chunks
    {
        tk::RxFramer framer;
        tk::RxFramerDropReason reason = tk::RxFramerDropReason::None;
        size_t dropped = 0;
        const uint8_t partial[] = {0x00, 0x0A, 1, 2}; // needs 10 bytes, got 2
        framer.push_chunk(partial, sizeof(partial), 1000, [](const uint8_t*, size_t) {});
        CHECK(framer.buffered_bytes() == 4);

        // Tick before timeout: no drop
        CHECK(!framer.check_timeout(1999, [&](tk::RxFramerDropReason r, size_t d) {
            reason = r; dropped = d;
        }));
        CHECK(framer.buffered_bytes() == 4);
        CHECK(reason == tk::RxFramerDropReason::None);

        // Tick after timeout: drops stale buffer
        CHECK(framer.check_timeout(2001, [&](tk::RxFramerDropReason r, size_t d) {
            reason = r; dropped = d;
        }));
        CHECK(framer.buffered_bytes() == 0);
        CHECK(reason == tk::RxFramerDropReason::InterChunkTimeout);
        CHECK(dropped == 4);
        CHECK(framer.stats().timeout_drops == 1);
    }

    // Deferred spans renew at last arrival, and an oversized span cannot append or emit.
    {
        tk::RxFramer framer;
        const uint8_t partial[] = {0x00, 0x0A, 1, 2};
        unsigned frames = 0;
        size_t dropped = 0;
        auto frame = [&](const uint8_t*, size_t) { ++frames; };
        auto drop = [&](tk::RxFramerDropReason reason, size_t size) {
            CHECK(reason == tk::RxFramerDropReason::InterChunkTimeout);
            dropped += size;
        };
        framer.push_chunk(partial, sizeof partial, UINT32_MAX - 500, frame, drop, 1000);
        CHECK(!framer.check_timeout(1499));
        framer.push_chunk(partial, sizeof partial, 1499, frame, drop, 1001);
        CHECK(frames == 0 && dropped == 8 && framer.buffered_bytes() == 0);
        CHECK(framer.stats().timeout_drops == 1);
    }

    // 10. Header split across notification chunks (1 byte in chunk 1, rest in chunk 2)
    {
        tk::RxFramer framer;
        std::vector<std::vector<uint8_t>> frames;
        const uint8_t chunk1[] = {0x00}; // first byte of 2-byte header
        size_t n1 = framer.push_chunk(chunk1, sizeof(chunk1), 100, [&](const uint8_t* p, size_t len) {
            frames.emplace_back(p, p + len);
        });
        CHECK(n1 == 0);
        CHECK(frames.empty());
        CHECK(framer.buffered_bytes() == 1);

        const uint8_t chunk2[] = {0x03, 0xAA, 0xBB, 0xCC}; // second byte of header + 3 payload bytes
        size_t n2 = framer.push_chunk(chunk2, sizeof(chunk2), 150, [&](const uint8_t* p, size_t len) {
            frames.emplace_back(p, p + len);
        });
        CHECK(n2 == 1);
        CHECK(frames.size() == 1);
        CHECK(frames[0].size() == 3);
        CHECK(frames[0][0] == 0xAA && frames[0][2] == 0xCC);
        CHECK(framer.buffered_bytes() == 0);
    }

    // 11. Reentrant reset() called inside frame_cb terminates extraction cleanly without crash
    {
        tk::RxFramer framer;
        // Concatenated frames: frame 1 and frame 2
        const uint8_t multi[] = {0x00, 0x02, 1, 2, 0x00, 0x02, 3, 4};
        size_t calls = 0;
        size_t n = framer.push_chunk(multi, sizeof(multi), 100, [&](const uint8_t*, size_t) {
            calls++;
            framer.reset(); // reentrant reset on connection teardown
        });
        CHECK(calls == 1);
        CHECK(n == 1);
        CHECK(framer.buffered_bytes() == 0);
    }

    // 12. Integer overflow protection in buffer size + len check
    {
        tk::RxFramer framer;
        tk::RxFramerDropReason reason = tk::RxFramerDropReason::None;
        size_t dropped = 0;
        const uint8_t dummy[1] = {0};
        // Pass huge length that would wrap buffer.size() + len if unchecked
        size_t huge_len = static_cast<size_t>(-10);
        size_t n = framer.push_chunk(dummy, huge_len, 100,
            [](const uint8_t*, size_t) {},
            [&](tk::RxFramerDropReason r, size_t d) { reason = r; dropped = d; });
        CHECK(n == 0);
        CHECK(reason == tk::RxFramerDropReason::BufferOverflow);
        CHECK(framer.stats().overflow_drops == 1);
    }

    // 13. Malformed payload inside frame delivers frame deterministically and continues
    {
        tk::RxFramer framer;
        std::vector<std::vector<uint8_t>> frames;
        // Two frames: first has corrupt/garbage protobuf payload (e.g. 0xFF, 0xFF), second has valid payload
        const uint8_t multi[] = {
            0x00, 0x02, 0xFF, 0xFF,
            0x00, 0x02, 0x11, 0x22
        };
        size_t n = framer.push_chunk(multi, sizeof(multi), 100, [&](const uint8_t* p, size_t len) {
            frames.emplace_back(p, p + len);
        });
        CHECK(n == 2);
        CHECK(frames.size() == 2);
        CHECK(frames[0].size() == 2 && frames[0][0] == 0xFF);
        CHECK(frames[1].size() == 2 && frames[1][0] == 0x11);
        CHECK(framer.buffered_bytes() == 0);
    }

    // 14. Duplicate fragment and duplicate frame arrival (L4)
    {
        // 14a. Duplicate fragment during split frame:
        // When a middle fragment is duplicated, the stream contains unexpected extra bytes.
        // The framer extracts the first frame of declared length L, and the duplicate
        // remaining bytes either trigger a CorruptLength or BufferOverflow for the next frame.
        tk::RxFramer framer;
        std::vector<std::vector<uint8_t>> frames;
        tk::RxFramerDropReason last_drop = tk::RxFramerDropReason::None;
        auto on_frame = [&](const uint8_t* p, size_t len) { frames.emplace_back(p, p + len); };
        auto on_drop = [&](tk::RxFramerDropReason r, size_t) { last_drop = r; };

        const uint8_t chunk1[] = {0x00, 0x04, 0x01, 0x02}; // needs 4 bytes, got 2
        framer.push_chunk(chunk1, sizeof(chunk1), 100, on_frame, on_drop);
        CHECK(frames.empty());
        CHECK(framer.buffered_bytes() == 4);

        // Duplicate chunk1 arrives again:
        framer.push_chunk(chunk1, sizeof(chunk1), 150, on_frame, on_drop);
        // Now 6 bytes in buffer: 2-byte header (len=4), then 4 bytes (0x01, 0x02, 0x00, 0x04)
        // This completes frame 1 with the duplicate bytes!
        CHECK(frames.size() == 1);
        CHECK(frames[0].size() == 4);
        // Residual bytes {0x01, 0x02} remain in buffer
        CHECK(framer.buffered_bytes() == 2);

        // 14b. Exact duplicate frame notification:
        // If the sender transmits an exact duplicate frame back-to-back, the framer
        // deterministically extracts both complete frames. Upper layer (BleDispatcher)
        // anti-replay detects and drops the second frame.
        framer.reset();
        frames.clear();
        const uint8_t frame_bytes[] = {0x00, 0x03, 'A', 'B', 'C'};
        framer.push_chunk(frame_bytes, sizeof(frame_bytes), 200, on_frame);
        framer.push_chunk(frame_bytes, sizeof(frame_bytes), 250, on_frame);
        CHECK(frames.size() == 2);
        CHECK(frames[0] == frames[1]);
        CHECK(framer.buffered_bytes() == 0);
    }
}

static void test_vcsec_response() {
    using Bytes = std::vector<uint8_t>;
    using K = tk::VcsecReplyKind;
    tk::BleUuid address{};
    tk::BleUuid uuid{};
    address.fill(0x37);
    uuid.fill(0x49);
    auto number = [](Bytes& data, uint64_t value) {
        do {
            const uint8_t byte = value & 0x7f;
            value >>= 7;
            data.push_back(byte | (value ? 0x80 : 0));
        } while (value);
    };
    auto field = [&](Bytes& data, uint32_t tag, const Bytes& value) {
        number(data, (static_cast<uint64_t>(tag) << 3) | 2);
        number(data, value.size());
        data.insert(data.end(), value.begin(), value.end());
    };
    auto framed = [](Bytes body) {
        const size_t size = body.size();
        body.insert(body.begin(), {static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size)});
        return body;
    };
    auto request = [&](const Bytes& destination, const Bytes& source, const Bytes& id, bool auth) {
        Bytes body;
        field(body, 6, destination);
        field(body, 7, source);
        field(body, 51, id);
        field(body, auth ? 14 : 10, {0x08, 0x01});
        return framed(body);
    };
    Bytes source;
    field(source, 2, Bytes(address.begin(), address.end()));
    const Bytes id(uuid.begin(), uuid.end());
    const auto wire = request({0x08, 0x02}, source, id, false);
    tk::VcsecRequestMetadata metadata;
    CHECK(tk::capture_vcsec_request(wire.data(), wire.size(), metadata));
    CHECK(metadata.reply_address == address && metadata.uuid == uuid);
    CHECK(metadata.payload == tk::VcsecRequestPayload::Protobuf);
    CHECK(!tk::capture_vcsec_request(nullptr, wire.size(), metadata));
    CHECK(tk::is_zero_uuid(metadata.reply_address) && tk::is_zero_uuid(metadata.uuid));
    for (size_t size = 0; size < wire.size(); ++size) {
        auto truncated = wire;
        truncated.resize(size);
        if (size >= 2) {
            truncated[0] = (size - 2) >> 8;
            truncated[1] = (size - 2) & 0xff;
        }
        CHECK(!tk::capture_vcsec_request(truncated.data(), truncated.size(), metadata));
        CHECK(tk::is_zero_uuid(metadata.uuid));
    }
    for (const auto& invalid : {
             request({0x08, 0x03}, source, id, false),
             request({}, source, id, false),
             request({0x08, 0x02, 0x08, 0x02}, source, id, false),
             request({0x08, 0x02}, {}, id, false),
             request({0x08, 0x02}, source, Bytes(15, 0x49), false),
             request({0x08, 0x02}, source, Bytes(16, 0), false)}) {
        CHECK(!tk::capture_vcsec_request(invalid.data(), invalid.size(), metadata));
    }
    Bytes zero_source;
    field(zero_source, 2, Bytes(16, 0));
    const auto zero = request({0x08, 0x02}, zero_source, id, false);
    CHECK(!tk::capture_vcsec_request(zero.data(), zero.size(), metadata));
    // Both Oneof orders, invalid known-field wire types, and truncated unknown fields.
    for (bool reverse : {false, true}) {
        Bytes conflicting = reverse ? Bytes{0x08, 0x02} : source;
        const Bytes other = reverse ? source : Bytes{0x08, 0x02};
        conflicting.insert(conflicting.end(), other.begin(), other.end());
        for (bool source_conflict : {false, true}) {
            const auto bad = request(source_conflict ? Bytes{0x08, 0x02} : conflicting,
                                     source_conflict ? conflicting : source, id, false);
            CHECK(!tk::capture_vcsec_request(bad.data(), bad.size(), metadata));
        }
        Bytes body;
        field(body, reverse ? 14 : 10, {8, 1});
        body.insert(body.end(), wire.begin() + 2, wire.end());
        const auto bad = framed(body);
        CHECK(!tk::capture_vcsec_request(bad.data(), bad.size(), metadata));
    }
    for (uint32_t tag : {6U, 7U, 10U, 14U, 51U}) {
        Bytes body(wire.begin() + 2, wire.end());
        number(body, static_cast<uint64_t>(tag) << 3);
        number(body, 1);
        const auto bad = framed(body);
        CHECK(!tk::capture_vcsec_request(bad.data(), bad.size(), metadata));
    }
    for (const Bytes& suffix : {Bytes{0xe1, 3, 1, 2, 3}, Bytes{0xe5, 3, 1, 2},
                               Bytes{0xe2, 3, 4, 1, 2}, Bytes{0xe0, 3, 0x80}}) {
        Bytes body(wire.begin() + 2, wire.end());
        body.insert(body.end(), suffix.begin(), suffix.end());
        const auto bad = framed(body);
        CHECK(!tk::capture_vcsec_request(bad.data(), bad.size(), metadata));
    }
    for (uint32_t duplicate : {6U, 7U, 10U, 14U, 15U, 50U, 51U}) {
        Bytes body(wire.begin() + 2, wire.end());
        field(body, duplicate, duplicate == 51 ? id : Bytes{0x08, 0x02});
        const auto malformed = framed(body);
        CHECK(!tk::capture_vcsec_request(malformed.data(), malformed.size(), metadata));
    }
    Bytes extended(wire.begin() + 2, wire.end());
    number(extended, 60U << 3);
    number(extended, 300);
    number(extended, (61U << 3) | 1);
    extended.insert(extended.end(), 8, 0x55);
    field(extended, 62, {1, 2, 3});
    number(extended, (63U << 3) | 5);
    extended.insert(extended.end(), 4, 0x66);
    auto valid_extension = framed(extended);
    CHECK(tk::capture_vcsec_request(valid_extension.data(), valid_extension.size(), metadata));
    for (const Bytes& suffix : {Bytes{0}, Bytes{0x0b}, Bytes{0xe0, 3, 0x80, 0},
                               Bytes{0xe0, 3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 2},
                               Bytes{0xe2, 3, 0xff, 0x7f}}) {
        Bytes body(wire.begin() + 2, wire.end());
        body.insert(body.end(), suffix.begin(), suffix.end());
        const auto malformed = framed(body);
        CHECK(!tk::capture_vcsec_request(malformed.data(), malformed.size(), metadata));
    }
    Bytes oversized(2051, 0);
    CHECK(!tk::capture_vcsec_request(oversized.data(), oversized.size(), metadata));
    auto bad_length = wire;
    ++bad_length[1];
    CHECK(!tk::capture_vcsec_request(bad_length.data(), bad_length.size(), metadata));
    CHECK(tk::capture_vcsec_request(wire.data(), wire.size(), metadata));

    tk::VcsecReplyContext status_context;
    CHECK(status_context.prepare(metadata, address, 17, 9, 1, K::Status));
    status_context.transmitted();
    const tk::VcsecAttemptState status_state{17, 9, 1, K::Status, true, 11, 1, 100, 10, 7};
    CHECK(status_context.matches(status_state, address.data(), address.size(), uuid.data(), uuid.size()));

    tk::VcsecReplyContext wake_context;
    CHECK(wake_context.prepare(metadata, address, 17, 9, 1, K::WakePrerequisite));
    tk::VcsecAttemptState wake_state{17, 9, 1, K::WakePrerequisite, true, 11, 1, 100, 10, 7};
    CHECK(!wake_context.matches(wake_state, address.data(), address.size(), nullptr, 0));
    wake_context.transmitted();
    CHECK(wake_context.matches(wake_state, address.data(), address.size(), uuid.data(), uuid.size()));
    wake_state.kind = K::Action;
    CHECK(!wake_context.matches(wake_state, address.data(), address.size(), nullptr, 0));
    wake_state.kind = K::WakePrerequisite;
    CHECK(wake_context.matches(wake_state, address.data(), address.size(), nullptr, 0));

    tk::VcsecReplyContext context;
    tk::VcsecAttemptState state{17, 9, 1, K::Action, true, 11, 1, 100, 10, 7};
    auto prepare = [&] {
        CHECK(context.prepare(metadata, address, state.generation, state.command_id, state.attempt, state.kind));
        context.transmitted();
    };
    CHECK(context.prepare(metadata, address, 17, 9, 1, K::Action));
    CHECK(!context.matches(state, address.data(), address.size(), nullptr, 0));
    context.transmitted();
    CHECK(context.matches(state, address.data(), address.size(), nullptr, 0));
    CHECK(context.matches(state, address.data(), address.size(), uuid.data(), uuid.size()));
    tk::BleUuid foreign{};
    foreign.fill(0x51);
    CHECK(!context.matches(state, foreign.data(), foreign.size(), nullptr, 0));
    CHECK(!context.matches(state, address.data(), 15, nullptr, 0));
    CHECK(!context.matches(state, nullptr, 16, nullptr, 0));
    CHECK(!context.matches(state, address.data(), address.size(), foreign.data(), foreign.size()));
    CHECK(!context.matches(state, address.data(), address.size(), nullptr, 16));
    CHECK(!context.matches(state, address.data(), address.size(), uuid.data(), 15));
    CHECK(context.matches(state, address.data(), address.size(), nullptr, 0));
    for (unsigned fault = 0; fault < 7; ++fault) {
        prepare();
        auto stale = state;
        switch (fault) {
            case 0: ++stale.generation; break;
            case 1: ++stale.command_id; break;
            case 2: ++stale.attempt; break;
            case 3: stale.executing = false; break;
            case 4: stale.kind = K::Status; break;
            case 5: stale.now_ms = 17; break;  // Phase timeout equality.
            default: stale.now_ms = 101; stale.phase_timeout_ms = 1000; break;
        }
        CHECK(!context.matches(stale, address.data(), address.size(), nullptr, 0));
        CHECK(context.matches(state, address.data(), address.size(), nullptr, 0) == (fault == 4));
    }
    prepare();
    for (uint32_t elapsed = 1000; elapsed <= 900000; elapsed += 1000) {
        auto expired = state;
        expired.now_ms += elapsed;
        CHECK(!context.matches(expired, address.data(), address.size(), nullptr, 0));
    }
    auto next = metadata;
    next.reply_address = foreign;
    ++state.attempt;
    CHECK(context.prepare(next, foreign, state.generation, state.command_id, state.attempt, state.kind));
    context.transmitted();
    CHECK(!context.matches(state, address.data(), address.size(), nullptr, 0));
    CHECK(context.matches(state, foreign.data(), foreign.size(), nullptr, 0));
    CHECK(tk::vcsec_reply_address_is_fresh(foreign, address));
    CHECK(!tk::vcsec_reply_address_is_fresh(address, address));
    CHECK(!tk::vcsec_reply_address_is_fresh({}, address));
    CHECK(!context.prepare(metadata, foreign, 17, 9, 1, K::Action));
    CHECK(!context.prepare(metadata, address, 17, 0, 1, K::Action));
    CHECK(!context.prepare(metadata, address, 17, 9, 0, K::Action));
    CHECK(!context.prepare(metadata, address, 17, 9, 1, static_cast<K>(255)));
    CHECK(!context.prepare(metadata, address, 17, 9, 1, K::SessionInfo));
    const auto auth_wire = request({0x08, 0x02}, source, id, true);
    CHECK(tk::capture_vcsec_request(auth_wire.data(), auth_wire.size(), metadata));
    CHECK(!context.prepare(metadata, address, 17, 9, 1, K::Action));
    CHECK(context.prepare(metadata, address, 17, 9, 1, K::SessionInfo));
    context.transmitted();
    state.kind = K::SessionInfo;
    state.attempt = 1;
    state.enqueued_at_ms = UINT32_MAX - 5;
    state.phase_started_at_ms = UINT32_MAX - 2;
    state.now_ms = 1;
    CHECK(context.matches(state, address.data(), address.size(), nullptr, 0));
    state.now_ms = 4;
    CHECK(!context.matches(state, address.data(), address.size(), nullptr, 0));
}

static void test_ble_dispatcher() {
    for (bool encrypted : {false, true}) {
        for (bool session_ready : {false, true}) {
            for (bool has_request_hash : {false, true}) {
                const auto admission = tk::vcsec_payload_admission(encrypted, session_ready, has_request_hash);
                if (!encrypted) CHECK(admission == tk::VcsecPayloadAdmission::Plaintext);
                else if (session_ready && has_request_hash) CHECK(admission == tk::VcsecPayloadAdmission::Decrypt);
                else CHECK(admission == tk::VcsecPayloadAdmission::Drop);
            }
        }
    }
    using D = tk::BleDomain;
    using R = tk::DispatchDropReason;

    // Response attribution is a read-only preflight shared with dispatch. Repeated checks must
    // consume neither authenticated zero nor the separate first-plaintext allowance.
    {
        tk::BleDispatcher disp;
        tk::BleUuid uuid{1};
        tk::BleUuid foreign{2};
        int marker = 42;
        const auto info = disp.register_request(D::Infotainment, uuid, &marker);
        const auto vcsec = disp.register_request(D::VehicleSecurity, foreign);
        const auto& read_only = disp;
        for (unsigned repeat = 0; repeat < 5; ++repeat) {
            const auto exact = read_only.match_response(D::Infotainment, uuid.data(), uuid.size());
            CHECK(exact.routed && exact.request_id == info && exact.user_data == &marker);
            CHECK(read_only.match_response(D::VehicleSecurity, nullptr, 0).request_id == vcsec);
            CHECK(read_only.match_response(D::Infotainment, nullptr, 0).drop_reason == R::Unmatched);
            CHECK(read_only.match_response(D::Infotainment, nullptr, uuid.size()).drop_reason == R::Unmatched);
            CHECK(read_only.match_response(D::Infotainment, foreign.data(), foreign.size()).drop_reason == R::Unmatched);
            CHECK(read_only.match_response(D::VehicleSecurity, uuid.data(), uuid.size()).drop_reason == R::Unmatched);
            CHECK(read_only.match_response(D::None, uuid.data(), uuid.size()).drop_reason == R::MissingSource);
            CHECK(read_only.match_response(D::Infotainment, uuid.data(), 1).drop_reason == R::InvalidUuidLength);
            CHECK(disp.active_count() == 2);
            CHECK(disp.stats().routed_count == 0 && disp.stats().replay_drops == 0);
            CHECK(disp.stats().missing_source_drops == 0 && disp.stats().invalid_uuid_drops == 0 && disp.stats().unmatched_drops == 0);
        }
        CHECK(disp.dispatch(D::Infotainment, uuid.data(), uuid.size(), true, 0).routed);
        CHECK(disp.dispatch(D::Infotainment, uuid.data(), uuid.size(), true, 0).drop_reason == R::Replay);
        CHECK(disp.dispatch(D::Infotainment, uuid.data(), uuid.size(), false, 0).routed);
        CHECK(disp.dispatch(D::Infotainment, uuid.data(), uuid.size(), false, 0).drop_reason == R::Replay);
        CHECK(disp.stats().routed_count == 2 && disp.stats().replay_drops == 2);
        disp.record_match_rejection(R::Unmatched);
        disp.record_match_rejection(R::MissingSource);
        disp.record_match_rejection(R::InvalidUuidLength);
        disp.record_match_rejection(R::None);
        CHECK(disp.stats().unmatched_drops == 1 && disp.stats().missing_source_drops == 1 && disp.stats().invalid_uuid_drops == 1);
        CHECK(disp.stats().routed_count == 2 && disp.stats().replay_drops == 2);
        CHECK(disp.unregister_request(info));
        CHECK(!read_only.match_response(D::Infotainment, uuid.data(), uuid.size()).routed);
    }

    // The native decode shell's bool admission shares attribution and rejection counters,
    // while preserving both the first encrypted counter zero and first plaintext response.
    {
        tk::BleDispatcher disp;
        tk::BleUuid uuid{1};
        tk::BleUuid foreign{2};
        const auto request = disp.register_request(D::Infotainment, uuid);
        for (unsigned repeat = 0; repeat < 5; ++repeat) {
            CHECK(disp.match_response_or_record_rejection(D::Infotainment, uuid.data(), uuid.size()));
            CHECK(!disp.match_response_or_record_rejection(D::Infotainment, foreign.data(), foreign.size()));
            CHECK(!disp.match_response_or_record_rejection(D::None, uuid.data(), uuid.size()));
            CHECK(!disp.match_response_or_record_rejection(D::Infotainment, uuid.data(), 1));
            CHECK(disp.has_request(request) && disp.active_count() == 1);
            CHECK(disp.stats().routed_count == 0 && disp.stats().replay_drops == 0);
        }
        CHECK(disp.stats().unmatched_drops == 5 && disp.stats().missing_source_drops == 5 && disp.stats().invalid_uuid_drops == 5);
        CHECK(disp.dispatch(D::Infotainment, uuid.data(), uuid.size(), true, 0).routed);
        CHECK(disp.dispatch(D::Infotainment, uuid.data(), uuid.size(), false, 0).routed);
        CHECK(disp.unregister_request(request));
        CHECK(!disp.match_response_or_record_rejection(D::Infotainment, uuid.data(), uuid.size()));
    }

    // 1. Lifecycle and registration bounds
    {
        tk::BleDispatcher disp;
        CHECK(disp.active_count() == 0);

        tk::BleUuid uuid1 = {1, 2, 3, 4};
        uint32_t id1 = disp.register_request(D::Infotainment, uuid1);
        CHECK(id1 > 0);
        CHECK(disp.has_request(id1));
        CHECK(disp.active_count() == 1);

        // Domain None rejected
        CHECK(disp.register_request(D::None, uuid1) == 0);

        // Unregister
        CHECK(disp.unregister_request(id1));
        CHECK(!disp.has_request(id1));
        CHECK(disp.active_count() == 0);
        CHECK(!disp.unregister_request(id1)); // Idempotent

        // Fill up to capacity (kMaxRequests = 8)
        std::vector<uint32_t> ids;
        for (size_t i = 0; i < tk::BleDispatcher::kMaxRequests; ++i) {
            tk::BleUuid u{};
            u[0] = static_cast<uint8_t>(i + 1);
            uint32_t id = disp.register_request(D::Infotainment, u);
            CHECK(id > 0);
            ids.push_back(id);
        }
        CHECK(disp.active_count() == tk::BleDispatcher::kMaxRequests);
        // Exceeded capacity rejected safely
        tk::BleUuid extra{};
        extra[0] = 99;
        CHECK(disp.register_request(D::Infotainment, extra) == 0);

        disp.reset();
        CHECK(disp.active_count() == 0);
    }

    // 2. Missing source domain and invalid UUID length drops
    {
        tk::BleDispatcher disp;
        tk::BleUuid uuid = {1, 2, 3};
        disp.register_request(D::Infotainment, uuid);

        // Missing source (domain None)
        auto o1 = disp.dispatch(D::None, uuid.data(), uuid.size(), false, 0);
        CHECK(!o1.routed);
        CHECK(o1.drop_reason == R::MissingSource);
        CHECK(disp.stats().missing_source_drops == 1);

        // Invalid UUID length (e.g. 5 bytes instead of 0 or 16)
        uint8_t short_uuid[5] = {1, 2, 3, 4, 5};
        auto o2 = disp.dispatch(D::Infotainment, short_uuid, sizeof(short_uuid), false, 0);
        CHECK(!o2.routed);
        CHECK(o2.drop_reason == R::InvalidUuidLength);
        CHECK(disp.stats().invalid_uuid_drops == 1);
    }

    // 3. Infotainment UUID matching
    {
        tk::BleDispatcher disp;
        tk::BleUuid uuid_a = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        tk::BleUuid uuid_b = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0,  1,  2,  3,  4,  5,  6};
        uint32_t id_a = disp.register_request(D::Infotainment, uuid_a);
        uint32_t id_b = disp.register_request(D::Infotainment, uuid_b);

        // Empty UUID on Infotainment does not match
        auto o0 = disp.dispatch(D::Infotainment, nullptr, 0, true, 1);
        CHECK(!o0.routed);
        CHECK(o0.drop_reason == R::Unmatched);

        // Non-existent UUID
        tk::BleUuid uuid_unknown = {0xFF};
        auto o_un = disp.dispatch(D::Infotainment, uuid_unknown.data(), uuid_unknown.size(), true, 1);
        CHECK(!o_un.routed);
        CHECK(o_un.drop_reason == R::Unmatched);

        // Matching UUID A routes to id_a
        auto o_a = disp.dispatch(D::Infotainment, uuid_a.data(), uuid_a.size(), true, 1);
        CHECK(o_a.routed);
        CHECK(o_a.drop_reason == R::None);
        CHECK(o_a.request_id == id_a);

        // Matching UUID B routes to id_b
        auto o_b = disp.dispatch(D::Infotainment, uuid_b.data(), uuid_b.size(), true, 1);
        CHECK(o_b.routed);
        CHECK(o_b.drop_reason == R::None);
        CHECK(o_b.request_id == id_b);
    }

    // 4. VCSEC UUID match exemption & multi-request precedence (modelled on dispatcher.go:259-261)
    {
        tk::BleDispatcher disp;
        tk::BleUuid vcsec_req_uuid_a = {0xAA, 0x11};
        tk::BleUuid vcsec_req_uuid_b = {0xBB, 0x22};
        uint32_t vcsec_id_a = disp.register_request(D::VehicleSecurity, vcsec_req_uuid_a);
        uint32_t vcsec_id_b = disp.register_request(D::VehicleSecurity, vcsec_req_uuid_b);
        CHECK(vcsec_id_a > 0);
        CHECK(vcsec_id_b > 0);
        CHECK(vcsec_id_a != vcsec_id_b);

        // Case A: Response with UUID B must route to Request B (exact UUID takes precedence)
        auto ob = disp.dispatch(D::VehicleSecurity, vcsec_req_uuid_b.data(), vcsec_req_uuid_b.size(), false, 0);
        CHECK(ob.routed);
        CHECK(ob.request_id == vcsec_id_b);

        // Case B: Response with UUID A must route to Request A (exact UUID takes precedence)
        auto oa = disp.dispatch(D::VehicleSecurity, vcsec_req_uuid_a.data(), vcsec_req_uuid_a.size(), false, 0);
        CHECK(oa.routed);
        CHECK(oa.request_id == vcsec_id_a);

        // Subsequent plaintext to same slots is correctly dropped as replay
        auto oa_dup = disp.dispatch(D::VehicleSecurity, vcsec_req_uuid_a.data(), vcsec_req_uuid_a.size(), false, 0);
        CHECK(!oa_dup.routed);
        CHECK(oa_dup.drop_reason == R::Replay);

        // Case C: Empty UUID (len 0) falls back to VCSEC exemption on fresh request
        disp.reset();
        uint32_t vcsec_id_c = disp.register_request(D::VehicleSecurity, vcsec_req_uuid_a);
        CHECK(vcsec_id_c > 0);
        auto o_empty = disp.dispatch(D::VehicleSecurity, nullptr, 0, false, 0);
        CHECK(o_empty.routed);
        CHECK(o_empty.request_id == vcsec_id_c);

        // Case D: Non-matching 16-byte UUID on VCSEC is dropped as Unmatched (M2)
        disp.reset();
        uint32_t vcsec_id_d = disp.register_request(D::VehicleSecurity, vcsec_req_uuid_a);
        CHECK(vcsec_id_d > 0);
        tk::BleUuid different_uuid = {0x99, 0x88};
        auto o_diff = disp.dispatch(D::VehicleSecurity, different_uuid.data(), different_uuid.size(), false, 0);
        CHECK(!o_diff.routed);
        CHECK(o_diff.drop_reason == R::Unmatched);
        CHECK(disp.stats().unmatched_drops == 1);

        // Unregister -> unmatched
        disp.unregister_request(vcsec_id_d);
        auto o_none = disp.dispatch(D::VehicleSecurity, nullptr, 0, false, 0);
        CHECK(!o_none.routed);
        CHECK(o_none.drop_reason == R::Unmatched);
    }

    // 5. Per-request anti-replay window (authenticated responses)
    {
        tk::BleDispatcher disp;
        tk::BleUuid uuid = {0x10};
        uint32_t id = disp.register_request(D::Infotainment, uuid);
        CHECK(id > 0);

        // First response with counter 100: accepted
        auto o1 = disp.dispatch(D::Infotainment, uuid.data(), uuid.size(), true, 100);
        CHECK(o1.routed);
        CHECK(o1.drop_reason == R::None);

        // Duplicate counter 100: dropped as replay
        auto o2 = disp.dispatch(D::Infotainment, uuid.data(), uuid.size(), true, 100);
        CHECK(!o2.routed);
        CHECK(o2.drop_reason == R::Replay);

        // Higher counter 105: accepted
        auto o3 = disp.dispatch(D::Infotainment, uuid.data(), uuid.size(), true, 105);
        CHECK(o3.routed);

        // Out-of-order counter 102 (within 64-bit window): accepted
        auto o4 = disp.dispatch(D::Infotainment, uuid.data(), uuid.size(), true, 102);
        CHECK(o4.routed);

        // Duplicate of out-of-order counter 102: dropped as replay
        auto o5 = disp.dispatch(D::Infotainment, uuid.data(), uuid.size(), true, 102);
        CHECK(!o5.routed);
        CHECK(o5.drop_reason == R::Replay);

        // Counter 40 (more than 64 behind highest 105): dropped as replay
        auto o6 = disp.dispatch(D::Infotainment, uuid.data(), uuid.size(), true, 40);
        CHECK(!o6.routed);
        CHECK(o6.drop_reason == R::Replay);

        // Counter 0: initial counter 0 is accepted, duplicate counter 0 is dropped as replay
        disp.reset();
        id = disp.register_request(D::Infotainment, uuid);
        CHECK(id > 0);
        auto o_z1 = disp.dispatch(D::Infotainment, uuid.data(), uuid.size(), true, 0);
        CHECK(o_z1.routed);
        auto o_z2 = disp.dispatch(D::Infotainment, uuid.data(), uuid.size(), true, 0);
        CHECK(!o_z2.routed);
        CHECK(o_z2.drop_reason == R::Replay);
    }

    // 6. Anti-replay protection for plaintext / unauthenticated responses (counter 0 / no counter)
    {
        tk::BleDispatcher disp;
        tk::BleUuid uuid = {0x20};
        uint32_t id = disp.register_request(D::Infotainment, uuid);
        CHECK(id > 0);

        // First plaintext response without counter: accepted
        auto o1 = disp.dispatch(D::Infotainment, uuid.data(), uuid.size(), false, 0);
        CHECK(o1.routed);
        CHECK(o1.drop_reason == R::None);

        // Second plaintext response without counter: dropped as replay!
        auto o2 = disp.dispatch(D::Infotainment, uuid.data(), uuid.size(), false, 0);
        CHECK(!o2.routed);
        CHECK(o2.drop_reason == R::Replay);
        CHECK(disp.stats().replay_drops > 0);
    }

    // 7. Duplicate registration prevention and capacity exhaustion
    {
        tk::BleDispatcher disp;
        tk::BleUuid uuid = {0x33};
        uint32_t id1 = disp.register_request(D::Infotainment, uuid);
        CHECK(id1 > 0);
        // Duplicate registration for same domain and UUID is rejected
        uint32_t id_dup = disp.register_request(D::Infotainment, uuid);
        CHECK(id_dup == 0);
        CHECK(disp.active_count() == 1);

        // Different domain with same UUID is allowed
        uint32_t id_diff_domain = disp.register_request(D::VehicleSecurity, uuid);
        CHECK(id_diff_domain > 0);
        CHECK(disp.active_count() == 2);

        // Fill remaining 6 slots
        for (uint8_t i = 3; i <= 8; ++i) {
            tk::BleUuid u = {i, 0x55};
            uint32_t id = disp.register_request(D::Infotainment, u);
            CHECK(id > 0);
        }
        CHECK(disp.active_count() == 8);

        // 9th request must fail with capacity exhausted (0)
        tk::BleUuid u9 = {0x99};
        uint32_t id9 = disp.register_request(D::Infotainment, u9);
        CHECK(id9 == 0);

        // Unregister one slot and register again -> succeeds
        CHECK(disp.unregister_request(id1));
        CHECK(disp.active_count() == 7);
        uint32_t id_retry = disp.register_request(D::Infotainment, u9);
        CHECK(id_retry > 0);
        CHECK(disp.active_count() == 8);
    }
}

static void test_session_state() {
    using S = tk::SessionState;

    // 1. Initial unauthenticated state
    {
        tk::SessionTracker session;
        CHECK(session.state() == S::Unauthenticated);
        CHECK(!session.is_authenticated());
        CHECK(session.counter() == 0);
        CHECK(session.clock_time() == 0);
        const std::array<uint8_t, 16> empty_epoch{};
        CHECK(session.epoch() == empty_epoch);
    }

    // 2. Establishment and Monotonic Counter Alignment per signer.go: max(local, reported)
    {
        tk::SessionTracker session;
        std::array<uint8_t, 16> epoch_a = {0x0A, 0x01};
        session.set_established(epoch_a, 50, 1000);
        CHECK(session.state() == S::Established);
        CHECK(session.is_authenticated());
        CHECK(session.epoch() == epoch_a);
        CHECK(session.counter() == 50);
        CHECK(session.clock_time() == 1000);

        // Vehicle reports counter below ours (e.g. 20) with advanced clock (1010).
        // Per signer.go UpdateSessionInfo: counter = max(local, reported).
        // Maintains 50, does NOT drop to 20!
        session.set_established(epoch_a, 20, 1010);
        CHECK(session.counter() == 50);
        CHECK(session.clock_time() == 1010);

        // Vehicle reports higher counter (80) -> advances to 80
        session.set_established(epoch_a, 80, 1020);
        CHECK(session.counter() == 80);
        CHECK(session.clock_time() == 1020);

        // New epoch (epoch_b): signer.go maintains monotonic counter progression max(local, reported)
        // If reported counter is 15 (< 80), local counter 80 is preserved!
        std::array<uint8_t, 16> epoch_b = {0x0B, 0x02};
        session.set_established(epoch_b, 15, 1030);
        CHECK(session.epoch() == epoch_b);
        CHECK(session.counter() == 80); // Preserved >= 80 per signer.go UpdateSessionInfo
        CHECK(session.clock_time() == 1030);

        // If new epoch reports higher counter (120), advances to 120
        session.set_established(epoch_b, 120, 1040);
        CHECK(session.counter() == 120);
        CHECK(session.clock_time() == 1040);
    }

    // 3. Reset clears session and counter
    {
        tk::SessionTracker session;
        session.set_established({1, 2, 3}, 42, 9999);
        CHECK(session.is_authenticated());
        session.reset();
        CHECK(!session.is_authenticated());
        CHECK(session.state() == S::Unauthenticated);
        CHECK(session.counter() == 0);
        CHECK(session.clock_time() == 0);
        const std::array<uint8_t, 16> empty_epoch{};
        CHECK(session.epoch() == empty_epoch);
    }
}

// ── VCSEC VehicleSleepStatus_E is three-valued: UNKNOWN must not read as AWAKE (ADR-0005 R1) ──
static void test_vcsec_sleep_three_valued() {
    using tk::SleepState;
    CHECK(tk::sleep_state_from_vcsec(true, false)  == SleepState::Awake);
    CHECK(tk::sleep_state_from_vcsec(false, true)  == SleepState::Asleep);
    // UNKNOWN(0): neither flag set — the frame proves nothing.
    CHECK(tk::sleep_state_from_vcsec(false, false) == SleepState::Unknown);

    // Wake confirmation needs an explicit AWAKE or the closureStatuses signal.
    CHECK(tk::vcsec_status_confirms_wake(SleepState::Awake, false));
    CHECK(tk::vcsec_status_confirms_wake(SleepState::Awake, true));
    CHECK(tk::vcsec_status_confirms_wake(SleepState::Asleep, true));
    CHECK(tk::vcsec_status_confirms_wake(SleepState::Unknown, true));
    CHECK(!tk::vcsec_status_confirms_wake(SleepState::Asleep, false));
    // The regression: an UNKNOWN frame without closures used to release a waiting command.
    CHECK(!tk::vcsec_status_confirms_wake(SleepState::Unknown, false));
}

// ── ASLEEP debounce clock: only an UNINTERRUPTED run of explicit ASLEEP reports proves sleep ──
// These are the exact functions VehicleController::note_vcsec_sleep_() and vcsec_stably_asleep_()
// call, so UNKNOWN-breaks-the-run is pinned here rather than by a source grep.
static void test_vcsec_sleep_clock() {
    using tk::SleepState;
    using S = SleepState;

    // Start, continue (keeping the ORIGINAL start tick), and the two ways a run ends.
    CHECK(tk::next_asleep_since(0, S::Asleep, 100) == 100);
    CHECK(tk::next_asleep_since(100, S::Asleep, 200) == 100);
    CHECK(tk::next_asleep_since(100, S::Awake, 200) == 0);
    CHECK(tk::next_asleep_since(100, S::Unknown, 200) == 0);
    CHECK(tk::next_asleep_since(0, S::Awake, 200) == 0);
    CHECK(tk::next_asleep_since(0, S::Unknown, 200) == 0);

    // Exhaustive: no report other than ASLEEP can leave a run alive, whatever the prior clock.
    for (const S reported : {S::Unknown, S::Awake}) {
        for (const uint32_t since : {0u, 1u, 100u, 0xffffffffu}) {
            CHECK(tk::next_asleep_since(since, reported, 5000) == 0);
        }
    }

    // A run is only as long as its last unbroken stretch. ASLEEP, then an UNKNOWN interval, then
    // ASLEEP again must NOT accumulate: the second stretch restarts from zero, however long the
    // car was really asleep through the gap.
    constexpr uint32_t kHz = 100;
    constexpr uint32_t kDebounceTicks = tk::kAsleepDebounceS * kHz;
    uint32_t since = 0;
    since = tk::next_asleep_since(since, S::Asleep, 1000);
    CHECK(since == 1000);
    since = tk::next_asleep_since(since, S::Asleep, 1000 + kDebounceTicks - 1);
    CHECK(!tk::vcsec_asleep_proven(S::Asleep, since, 1000 + kDebounceTicks - 1, kHz,
                                    tk::kAsleepDebounceS));
    since = tk::next_asleep_since(since, S::Asleep, 1000 + kDebounceTicks);
    CHECK(tk::vcsec_asleep_proven(S::Asleep, since, 1000 + kDebounceTicks, kHz,
                                   tk::kAsleepDebounceS));
    // One UNKNOWN sample right after proof was reached: the proof is gone at once...
    const uint32_t t_unknown = 1000 + kDebounceTicks + 10;
    since = tk::next_asleep_since(since, S::Unknown, t_unknown);
    CHECK(since == 0);
    CHECK(!tk::vcsec_asleep_proven(S::Unknown, since, t_unknown, kHz, tk::kAsleepDebounceS));
    // ...and the next ASLEEP starts a fresh run that has to earn the full debounce again.
    const uint32_t t_restart = t_unknown + 10;
    since = tk::next_asleep_since(since, S::Asleep, t_restart);
    CHECK(since == t_restart);
    CHECK(!tk::vcsec_asleep_proven(S::Asleep, since, t_restart + kDebounceTicks - 1, kHz,
                                    tk::kAsleepDebounceS));
    CHECK(tk::vcsec_asleep_proven(S::Asleep, since, t_restart + kDebounceTicks, kHz,
                                   tk::kAsleepDebounceS));

    // The proof also needs the car's CURRENT raw report to be ASLEEP. A stale run (clock still
    // set) beside an UNKNOWN or AWAKE report is not evidence of sleep.
    CHECK(!tk::vcsec_asleep_proven(S::Unknown, 1, 1 + 10 * kDebounceTicks, kHz, tk::kAsleepDebounceS));
    CHECK(!tk::vcsec_asleep_proven(S::Awake, 1, 1 + 10 * kDebounceTicks, kHz, tk::kAsleepDebounceS));
    CHECK(tk::vcsec_asleep_proven(S::Asleep, 1, 1 + 10 * kDebounceTicks, kHz, tk::kAsleepDebounceS));
    // No run (clock 0) is never proof, and a zero tick rate must not divide by zero.
    CHECK(!tk::vcsec_asleep_proven(S::Asleep, 0, 0xffffffffu, kHz, tk::kAsleepDebounceS));
    CHECK(!tk::vcsec_asleep_proven(S::Asleep, 1, 0xffffffffu, 0, tk::kAsleepDebounceS));

    // The FreeRTOS tick counter is 32-bit and wraps; the elapsed time stays correct across it.
    CHECK(tk::vcsec_asleep_proven(S::Asleep, 0xffffff00u, 0x00000100u, 1, 512));
    CHECK(!tk::vcsec_asleep_proven(S::Asleep, 0xffffff00u, 0x000000ffu, 1, 512));
}

static void test_session_availability() {
    using Save = tk::SessionSaveState;
    for (unsigned bits = 0; bits < 32; ++bits) {
        const auto state = tk::decode_session_runtime(bits);
        CHECK(state.known == ((bits & 1) != 0));
        CHECK(state.usable == ((bits & 3) == 3));
        CHECK(static_cast<unsigned>(state.save) == ((bits >> 2) & 3));
        for (bool identity : {false, true}) for (bool stored : {false, true}) {
            CHECK(tk::session_can_operate(identity, stored, state.known && state.usable) ==
                  (identity && (stored || ((bits & 3) == 3))));
        }
    }
    for (unsigned flags = 0; flags < 32; ++flags) {
        const auto runtime = tk::session_runtime_observation(flags & 1, flags & 2, flags & 4,
                                                             flags & 8, flags & 16);
        CHECK(runtime.known == ((flags & 1) != 0));
        CHECK(runtime.usable == (flags == 31));
    }
    for (bool known : {false, true}) for (bool usable : {false, true}) {
        for (Save save : {Save::Unknown, Save::Unconfirmed, Save::Confirmed, Save::Error}) {
            const auto roundtrip = tk::decode_session_runtime(tk::encode_session_runtime({known, usable, save}));
            CHECK(roundtrip.known == known);
            CHECK(roundtrip.usable == (known && usable));
            CHECK(roundtrip.save == save);
        }
    }
    CHECK_STR(tk::session_save_state_name(Save::Unknown), "unknown");
    CHECK_STR(tk::session_save_state_name(Save::Unconfirmed), "unconfirmed");
    CHECK_STR(tk::session_save_state_name(Save::Confirmed), "confirmed");
    CHECK_STR(tk::session_save_state_name(Save::Error), "error");
}

static void test_command_deadline() {
    for (uint32_t now : {0u, 1000u, 0xfffffff0u}) {
        CHECK(tk::capped_command_deadline(now + 2500, now, 9000) == now + 2500);
        CHECK(tk::capped_command_deadline(now + 20000, now, 9000) == now + 9000);
        CHECK(tk::capped_command_deadline(now, now, 9000) == now);
        CHECK(tk::capped_command_deadline(now - 500, now, 9000) == now - 500);
        CHECK(tk::capped_command_deadline(now + 40, now, 40) == now + 40);
    }
    CHECK(tk::capped_command_deadline(3500, 9000, 9000) == 3500);
    CHECK(tk::capped_command_deadline(0x18, 4, 9000) == 0x18);
    CHECK(tk::capped_command_deadline(0x20, 0xfffffff0, 16) == 1);
    CHECK(tk::capped_command_deadline(0xfffffff8, 0xfffffff0, 16) == 0xfffffff8);
}

static void test_command_fault() {
    using S = tk::CommandError::Severity;
    using O = tk::CommandError::Outcome;
    for (S severity : {S::Temporary, S::Permanent, S::Unknown}) {
        for (O outcome : {O::DefinitelyFailed, O::MayHaveSucceeded, O::Unknown}) {
            CHECK(tk::wake_confirmation_required(false, {severity, outcome}) == (outcome != O::DefinitelyFailed));
            CHECK(tk::wake_confirmation_required(true, {severity, outcome}));
        }
    }
    constexpr unsigned temporary[] = {1, 2, 5, 6, 11, 15, 17, 20};
    for (unsigned code = 0; code < 31; ++code) {
        const auto fault = tk::classify_message_fault(code);
        bool expected = false;
        for (auto retry : temporary) expected |= code == retry;
        CHECK(fault.known == (code > 0 && code <= 28));
        CHECK(fault.temporary == expected);
        CHECK(fault.possible_success == (code == 0 || code == 25 || code > 28));
        for (bool authenticated : {false, true}) for (bool sent : {false, true}) {
            const auto info = tk::fault_failure_info(fault, authenticated, sent);
            CHECK(tk::execution_uncertain(info) == (fault.possible_success || (sent && !authenticated)));
            CHECK(tk::fault_allows_retry(fault, authenticated, sent, tk::CommandEffect::Mutating) ==
                  (expected && (authenticated || !sent)));
            CHECK(tk::fault_allows_retry(fault, authenticated, sent, tk::CommandEffect::ReadOnly) == expected);
        }
    }
    CHECK(tk::uncertain_command_failure().outcome == O::MayHaveSucceeded);
    CHECK(tk::uncertain_command_failure().severity == S::Unknown);
    // A submitted mutation has one serialization opportunity. Simulate many retry windows;
    // absence of a response, advisory errors, repair and clock wrap never grant another one.
    for (bool read_only : {false, true}) {
        tk::CommandRunner runner;
        const auto id = runner.enqueue("effect-is-explicit", tk::BleDomain::VehicleSecurity,
            tk::WakePolicy::WakeIfNeeded, 900000, 0);
        if (read_only) CHECK(runner.set_command_effect(id, tk::CommandEffect::ReadOnly));
        runner.vcsec_session().set_established({}, 1, 1);
        CHECK(runner.tick(1, true, true) == tk::TxAction::SendCommandPayload);
        runner.notify_tx_started(tk::TxAction::SendCommandPayload);
        runner.notify_tx_complete(tk::TxAction::SendCommandPayload, 1);
        unsigned additional = 0;
        for (uint32_t now = 7001; now <= 600000; now += 1000) {
            const auto action = runner.tick(now, true, true);
            if (action == tk::TxAction::SendCommandPayload) {
                ++additional;
                runner.notify_tx_complete(action, now);
            }
        }
        CHECK(read_only ? additional == 3 : additional == 0);
        CHECK(runner.current_command()->is_completed);
        CHECK(runner.current_command()->failure.outcome == O::MayHaveSucceeded);
        CHECK(runner.current_command()->error_message.find(tk::uncertain_command_reason()) == 0);
    }
}

static void test_command_runner() {
    using namespace tk;

    // 1. Enqueue & FIFO ordering and queue saturation
    {
        CommandRunner runner;
        CHECK(!runner.has_active_command());
        CHECK(runner.queue_size() == 0);
        CHECK(!runner.is_queue_full());

        // Enqueue up to capacity (kMaxQueueSize = 8)
        for (uint32_t i = 0; i < CommandRunner::kMaxQueueSize; ++i) {
            std::string name = "Cmd_" + std::to_string(i);
            uint32_t id = runner.enqueue(name, BleDomain::VehicleSecurity, WakePolicy::WakeIfNeeded, 20000, 1000);
            CHECK(id == i + 1);
            CHECK(runner.queue_size() == i + 1);
        }
        CHECK(runner.is_queue_full());

        // 9th enqueue must be rejected
        uint32_t rej = runner.enqueue("Cmd_Overflow", BleDomain::VehicleSecurity, WakePolicy::WakeIfNeeded, 20000, 1000);
        CHECK(rej == 0);
        CHECK(runner.queue_size() == CommandRunner::kMaxQueueSize);

        // Verify FIFO front is Cmd_0 with ID 1
        CommandRequest* front = runner.current_command();
        CHECK(front != nullptr);
        CHECK(front->id == 1);
        CHECK(front->name == "Cmd_0");

        // Pop front command
        runner.pop_current();
        CHECK(runner.queue_size() == CommandRunner::kMaxQueueSize - 1);
        front = runner.current_command();
        CHECK(front != nullptr);
        CHECK(front->id == 2);
        CHECK(front->name == "Cmd_1");

        // Clear all
        runner.clear();
        CHECK(runner.queue_size() == 0);
        CHECK(!runner.has_active_command());
        CHECK(runner.current_command() == nullptr);
    }

    // 2. Wake-up Policy Coordination
    {
        // 2a. NoWakeSkip when vehicle is asleep
        {
            CommandRunner runner;
            // Pre-authenticate VCSEC so it reaches the wake check
            runner.vcsec_session().set_established({1}, 10, 1000);

            bool completed_called = false;
            bool success_val = true;
            std::string err_val;
            runner.enqueue("ChargePoll", BleDomain::Infotainment, WakePolicy::NoWakeSkip, 20000, 1000, {},
                           [&](bool ok, const std::string& err, tk::CommandFailureInfo) {
                               completed_called = true;
                               success_val = ok;
                               err_val = err;
                           });

            TxAction act = runner.tick(1000, true /* connected */, false /* awake */, true /* asleep */);
            CHECK(act == TxAction::None);

            CommandRequest* cmd = runner.current_command();
            CHECK(cmd != nullptr);
            CHECK(cmd->is_completed);
            CHECK(!cmd->is_success);
            CHECK(cmd->state == CommandState::Skipped);
            CHECK(cmd->terminal_reason == TerminalReason::VehicleAsleep);
            CHECK(completed_called);
            CHECK(!success_val);
            CHECK(err_val == "vehicle asleep");
        }

        // 2b. NoWakeFail when vehicle is asleep
        {
            CommandRunner runner;
            runner.vcsec_session().set_established({1}, 10, 1000);

            runner.enqueue("ChargeCmd", BleDomain::Infotainment, WakePolicy::NoWakeFail, 20000, 1000);
            TxAction act = runner.tick(1000, true, false /* awake */, true /* asleep */);
            CHECK(act == TxAction::None);

            CommandRequest* cmd = runner.current_command();
            CHECK(cmd != nullptr);
            CHECK(cmd->is_completed);
            CHECK(!cmd->is_success);
            CHECK(cmd->state == CommandState::Failed);
            CHECK(cmd->terminal_reason == TerminalReason::VehicleAsleep);
        }

        // 2c. WakeIfNeeded when vehicle is asleep -> coordinates wake sequence
        {
            CommandRunner runner;
            runner.vcsec_session().set_established({1}, 10, 1000);

            runner.enqueue("ChargeStart", BleDomain::Infotainment, WakePolicy::WakeIfNeeded, 20000, 1000);
            TxAction act = runner.tick(1000, true, false /* awake */, true /* asleep */);
            CHECK(act == TxAction::SendWake);

            CommandRequest* cmd = runner.current_command();
            CHECK(cmd != nullptr);
            CHECK(!cmd->is_completed);
            CHECK(cmd->state == CommandState::WaitingWake);
            CHECK(cmd->phase == CommandPhase::EnsuringAwake);

            // Notify vehicle is now awake
            runner.notify_vehicle_awake(true);

            // Now vehicle is awake, but infotainment session not established yet -> needs info session
            act = runner.tick(1100, true, true /* awake */, false /* asleep */);
            CHECK(act == TxAction::SendInfoSessionInfoRequest);
            CHECK(cmd->state == CommandState::WaitingInfoAuth);
            CHECK(cmd->phase == CommandPhase::EnsuringInfotainmentSession);
        }

        // 2d. WakeIfNeeded when vehicle is already awake -> skips wake sequence directly
        {
            CommandRunner runner;
            runner.vcsec_session().set_established({1}, 10, 1000);
            runner.info_session().set_established({2}, 20, 1000);

            runner.enqueue("ChargeStart", BleDomain::Infotainment, WakePolicy::WakeIfNeeded, 20000, 1000);
            TxAction act = runner.tick(1000, true, true /* awake */, false /* asleep */);
            CHECK(act == TxAction::SendCommandPayload);

            CommandRequest* cmd = runner.current_command();
            CHECK(cmd != nullptr);
            CHECK(cmd->state == CommandState::Ready);
            CHECK(cmd->phase == CommandPhase::SendingRequest);
        }

        // 2e. NoWakeSkip when sleep state is Unknown -> does NOT skip, proceeds to infotainment auth
        {
            CommandRunner runner;
            runner.vcsec_session().set_established({1}, 10, 1000);

            runner.enqueue("ChargePoll", BleDomain::Infotainment, WakePolicy::NoWakeSkip, 20000, 1000);
            // Neither awake nor asleep -> Unknown
            TxAction act = runner.tick(1000, true /* connected */, false /* awake */, false /* asleep */);
            CHECK(act == TxAction::SendInfoSessionInfoRequest);

            CommandRequest* cmd = runner.current_command();
            CHECK(cmd != nullptr);
            CHECK(!cmd->is_completed);
            CHECK(cmd->state == CommandState::WaitingInfoAuth);
        }

        // 2f. NoWakeFail when sleep state is Unknown -> does NOT fail, proceeds to infotainment auth
        {
            CommandRunner runner;
            runner.vcsec_session().set_established({1}, 10, 1000);

            runner.enqueue("ChargeCmd", BleDomain::Infotainment, WakePolicy::NoWakeFail, 20000, 1000);
            TxAction act = runner.tick(1000, true, false /* awake */, false /* asleep */);
            CHECK(act == TxAction::SendInfoSessionInfoRequest);

            CommandRequest* cmd = runner.current_command();
            CHECK(cmd != nullptr);
            CHECK(!cmd->is_completed);
            CHECK(cmd->state == CommandState::WaitingInfoAuth);
        }

        // 2g. WakeIfNeeded when sleep state is Unknown -> coordinates wake
        {
            CommandRunner runner;
            runner.vcsec_session().set_established({1}, 10, 1000);

            runner.enqueue("ChargeStart", BleDomain::Infotainment, WakePolicy::WakeIfNeeded, 20000, 1000);
            TxAction act = runner.tick(1000, true, false /* awake */, false /* asleep */);
            CHECK(act == TxAction::SendWake);

            CommandRequest* cmd = runner.current_command();
            CHECK(cmd != nullptr);
            CHECK(cmd->state == CommandState::WaitingWake);

            // On next tick without explicit notify, if is_awake becomes true, advances immediately without timeout
            act = runner.tick(1050, true, true /* now awake */, false /* asleep */);
            CHECK(act == TxAction::SendInfoSessionInfoRequest);
            CHECK(cmd->state == CommandState::WaitingInfoAuth);
        }

        // 2h. F1a: Stale AWAKE during WaitingVcsecAuth must NOT prematurely confirm wake
        // If an AWAKE arrives while awaiting VCSEC session auth, wake_confirmed must remain false.
        // Once VCSEC session is established, if the vehicle is asleep, tick must emit SendWake.
        {
            CommandRunner runner;
            runner.enqueue("ClimateOn", BleDomain::Infotainment, WakePolicy::WakeIfNeeded, 20000, 1000);

            // Tick 1: Infotainment needs VCSEC session first
            TxAction act = runner.tick(1000, true /* connected */, false /* awake */, false /* asleep */);
            CHECK(act == TxAction::SendVcsecSessionInfoRequest);
            CommandRequest* cmd = runner.current_command();
            CHECK(cmd != nullptr);
            CHECK(cmd->state == CommandState::WaitingVcsecAuth);
            CHECK(!cmd->wake_confirmed);

            // Stale AWAKE arrives while command is in WaitingVcsecAuth
            act = runner.tick(1050, true /* connected */, true /* awake */, false /* asleep */);
            CHECK(act == TxAction::None);
            CHECK(cmd->state == CommandState::WaitingVcsecAuth);
            // Critical check: wake_confirmed must NOT be set during WaitingVcsecAuth!
            CHECK(!cmd->wake_confirmed);

            // VCSEC auth completes
            runner.vcsec_session().set_established({1, 2, 3}, 5, 1100);
            cmd->state = CommandState::Idle;

            // Vehicle reports ASLEEP: because wake_confirmed was not prematurely set,
            // tick correctly emits SendWake rather than skipping to infotainment auth.
            act = runner.tick(1100, true /* connected */, false /* awake */, true /* asleep */);
            CHECK(act == TxAction::SendWake);
            CHECK(cmd->state == CommandState::WaitingWake);
            CHECK(cmd->phase == CommandPhase::EnsuringAwake);
        }
    }

    // 3. Prerequisite Phase Progression (VCSEC Auth -> Wake -> Infotainment Auth -> Ready)
    {
        CommandRunner runner;
        // Unauthenticated initial state
        CHECK(!runner.vcsec_session().is_authenticated());
        CHECK(!runner.info_session().is_authenticated());

        runner.enqueue("ClimateOn", BleDomain::Infotainment, WakePolicy::WakeIfNeeded, 20000, 1000);

        // Step 1: Infotainment requires VCSEC session first
        TxAction act = runner.tick(1000, true, true);
        CHECK(act == TxAction::SendVcsecSessionInfoRequest);
        CHECK(runner.current_command()->state == CommandState::WaitingVcsecAuth);
        CHECK(runner.current_command()->phase == CommandPhase::EnsuringVcsecSession);

        // Provide VCSEC SessionInfo response via set_established
        runner.vcsec_session().set_established({1, 2, 3}, 5, 1000);
        runner.current_command()->state = CommandState::Idle;
        CHECK(runner.vcsec_session().is_authenticated());

        // Step 2: Now VCSEC is satisfied, Infotainment session is needed
        act = runner.tick(1100, true, true);
        CHECK(act == TxAction::SendInfoSessionInfoRequest);
        CHECK(runner.current_command()->state == CommandState::WaitingInfoAuth);
        CHECK(runner.current_command()->phase == CommandPhase::EnsuringInfotainmentSession);

        // Provide Infotainment SessionInfo response via set_established
        runner.info_session().set_established({4, 5, 6}, 12, 1000);
        runner.current_command()->state = CommandState::Idle;
        CHECK(runner.info_session().is_authenticated());

        // Step 3: All prerequisites satisfied -> Ready to send command
        act = runner.tick(1200, true, true);
        CHECK(act == TxAction::SendCommandPayload);
        CHECK(runner.current_command()->state == CommandState::Ready);
        CHECK(runner.current_command()->phase == CommandPhase::SendingRequest);
    }

    // 4. "Whitelist Add Key" Session Bypass
    {
        CommandRunner runner;
        CHECK(!runner.vcsec_session().is_authenticated());

        bool completed = false;
        bool transmitted = false;
        runner.enqueue("Whitelist Add Key", BleDomain::VehicleSecurity, WakePolicy::NoWakeFail,
                       20000, 1000, {}, [&](bool ok, const std::string&, tk::CommandFailureInfo) {
                           completed = true;
                           transmitted = ok;
                       }, true /* completes_on_transmit */, 0 /* never replay ambiguous writes */);

        // Bypass session auth directly
        TxAction act = runner.tick(1000, true, false);
        CHECK(act == TxAction::SendCommandPayload);
        CHECK(runner.current_command()->state == CommandState::Ready);
        CHECK(runner.current_command()->phase == CommandPhase::SendingRequest);
        // Tesla sends no completing commandStatus for Whitelist Add. The local TX completion
        // reports submission to the API, while vehicle approval remains a separate status check.
        runner.notify_tx_complete(act, 1001);
        CHECK(completed);
        CHECK(transmitted);
        CHECK(runner.current_command()->is_completed);
        CHECK(runner.current_command()->is_success);
        CHECK(runner.current_command()->max_retries == 0);
        runner.pop_current();
        CHECK(runner.current_command() == nullptr);
    }

    // 4a. An ambiguous/partial Whitelist Add write must not be automatically replayed.
    {
        CommandRunner runner;
        unsigned completions = 0;
        bool last_success = true;
        runner.enqueue("Whitelist Add Key", BleDomain::VehicleSecurity,
                       WakePolicy::NoWakeFail, 20000, 1000, {},
                       [&](bool ok, const std::string&, tk::CommandFailureInfo) {
                           ++completions;
                           last_success = ok;
                       }, true /* completes_on_transmit */, 0 /* no ambiguous replay */);

        TxAction act = runner.tick(1000, true, false);
        CHECK(act == TxAction::SendCommandPayload);
        runner.notify_tx_failed("BLE write failed after partial submission");
        CHECK(completions == 1);
        CHECK(!last_success);
        CHECK(runner.current_command()->is_completed);
        CHECK(runner.current_command()->retry_count == 0);
        CHECK(runner.tick(1001, true, false) == TxAction::None);
        CHECK(completions == 1);
    }

    // 4b. "Wake" Session Flow and Vehicle Confirmation (N1)
    {
        CommandRunner runner;
        CHECK(!runner.vcsec_session().is_authenticated());

        bool completed = false;
        bool success_res = false;
        runner.enqueue("Wake", BleDomain::VehicleSecurity, WakePolicy::NoWakeFail, 9000, 1000, {},
                       [&](bool ok, const std::string&, tk::CommandFailureInfo) {
                           completed = true;
                           success_res = ok;
                       }, true /* completes_on_transmit */);

        // "Wake" requires VCSEC session authentication to encrypt the RKE action payload
        TxAction act = runner.tick(1000, true /* connected */, false /* awake */, true /* asleep */);
        CHECK(act == TxAction::SendVcsecSessionInfoRequest);
        CHECK(runner.current_command()->state == CommandState::WaitingVcsecAuth);
        CHECK(runner.current_command()->phase == CommandPhase::EnsuringVcsecSession);

        // Shell-faithful: after transmitting the prerequisite SessionInfoRequest, the shell
        // calls notify_tx_complete(act, now). This must NOT prematurely complete the Wake command!
        runner.notify_tx_complete(act, 1005);
        CHECK(!completed);
        CHECK(runner.current_command()->state == CommandState::WaitingVcsecAuth);
        CHECK(!runner.current_command()->is_completed);

        // Simulate session info arrival via set_established
        runner.vcsec_session().set_established({1, 2, 3}, 1, 1000);
        runner.current_command()->state = CommandState::Idle;
        CHECK(runner.vcsec_session().is_authenticated());

        act = runner.tick(1020, true, false, true);
        CHECK(act == TxAction::SendCommandPayload);
        CHECK(runner.current_command()->state == CommandState::Ready);
        CHECK(runner.current_command()->phase == CommandPhase::SendingRequest);

        // Notifying TX completion finishes Wake immediately with success
        // (Wake action has no commandStatus acknowledgement from Tesla; transmission completes it)
        runner.notify_tx_complete(act, 1050);
        CHECK(completed);
        CHECK(success_res);
        CHECK(runner.current_command()->is_completed);
        CHECK(runner.current_command()->is_success);
        CHECK(runner.current_command()->terminal_reason == TerminalReason::Success);
    }

    // 4c. Shell-faithful Driver: Wake does not complete on prerequisite SessionInfoRequest TX (F1 regression)
    {
        CommandRunner runner;
        bool completed = false;
        bool success_res = false;
        runner.enqueue("Wake", BleDomain::VehicleSecurity, WakePolicy::NoWakeFail, 9000, 1000, {},
                       [&](bool ok, const std::string&, tk::CommandFailureInfo) {
                           completed = true;
                           success_res = ok;
                       }, true /* completes_on_transmit */);

        int session_requests = 0;
        int payload_frames = 0;

        // Drive without an established session
        for (uint32_t now = 1000; now < 1200 && !completed; now += 50) {
            TxAction a = runner.tick(now, true, false, true);
            if (a == TxAction::None) continue;
            if (a == TxAction::SendVcsecSessionInfoRequest) ++session_requests;
            if (a == TxAction::SendCommandPayload) ++payload_frames;
            runner.notify_tx_complete(a, now);
        }

        // Must have sent SessionInfoRequest, but NEVER completed prematurely
        CHECK(session_requests == 1);
        CHECK(payload_frames == 0);
        CHECK(!completed);
        CHECK(!runner.current_command()->is_completed);

        // Now session arrives
        runner.vcsec_session().set_established({1, 2, 3}, 1, 1200);
        runner.current_command()->state = CommandState::Idle;

        // Next tick sends the payload
        TxAction a = runner.tick(1250, true, false, true);
        CHECK(a == TxAction::SendCommandPayload);
        ++payload_frames;
        runner.notify_tx_complete(a, 1250);

        // Now and only now is Wake completed
        CHECK(completed);
        CHECK(success_res);
        CHECK(payload_frames == 1);
        CHECK(runner.current_command()->is_completed);
        CHECK(runner.current_command()->terminal_reason == TerminalReason::Success);
    }

    // 4d. Unsolicited vehicle awake notification does not prematurely complete Wake before payload TX
    {
        CommandRunner runner;
        bool completed = false;
        runner.enqueue("Wake", BleDomain::VehicleSecurity, WakePolicy::NoWakeFail, 9000, 1000, {},
                       [&](bool, const std::string&, tk::CommandFailureInfo) {
                           completed = true;
                       }, true /* completes_on_transmit */);

        // Tick returns SendVcsecSessionInfoRequest
        TxAction a = runner.tick(1000, true, false, true);
        CHECK(a == TxAction::SendVcsecSessionInfoRequest);
        runner.notify_tx_complete(a, 1010);
        CHECK(!completed);

        // VCSEC sends unsolicited VehicleStatus reporting body controller is awake
        runner.notify_vehicle_awake(true);
        CHECK(!completed);
        CHECK(!runner.current_command()->is_completed);

        // Session establishes
        runner.vcsec_session().set_established({1, 2, 3}, 1, 1050);
        runner.current_command()->state = CommandState::Idle;

        // Next tick returns SendCommandPayload (wake frame must be sent)
        a = runner.tick(1060, true, false, true);
        CHECK(a == TxAction::SendCommandPayload);
        CHECK(!completed);

        // Transmitting the wake frame completes the Wake command
        runner.notify_tx_complete(a, 1070);
        CHECK(completed);
        CHECK(runner.current_command()->is_completed);
        CHECK(runner.current_command()->terminal_reason == TerminalReason::Success);
    }

    // 5. BleDispatcher Integration, Routing & Anti-Replay
    {
        CommandRunner runner;
        runner.vcsec_session().set_established({1}, 10, 1000);
        runner.info_session().set_established({2}, 20, 1000);

        BleUuid cmd_uuid = {0xAA, 0xBB, 0xCC, 0xDD, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        runner.enqueue("Lock", BleDomain::VehicleSecurity, WakePolicy::WakeIfNeeded, 20000, 1000, cmd_uuid);

        TxAction act = runner.tick(1000, true, true);
        CHECK(act == TxAction::SendCommandPayload);

        // Confirm TX complete: registers with dispatcher
        runner.notify_tx_complete(1050);
        CommandRequest* cmd = runner.current_command();
        CHECK(cmd->state == CommandState::AwaitingResponse);
        CHECK(cmd->phase == CommandPhase::AwaitingResponse);
        CHECK(cmd->dispatcher_request_id != 0);
        CHECK(runner.dispatcher().has_request(cmd->dispatcher_request_id));

        // 5a. Route response matching UUID
        auto outcome = runner.handle_response(BleDomain::VehicleSecurity, cmd_uuid.data(), cmd_uuid.size(),
                                             true, 100, true, "");
        CHECK(outcome.routed);
        CHECK(cmd->is_completed);
        CHECK(cmd->is_success);
        CHECK(cmd->terminal_reason == TerminalReason::Success);
        // Request unregistered from dispatcher
        CHECK(!runner.dispatcher().has_request(cmd->dispatcher_request_id));

        // 5b. Replay check: second response with same counter is dropped
        auto replay_outcome = runner.dispatcher().dispatch(BleDomain::VehicleSecurity, cmd_uuid.data(), cmd_uuid.size(),
                                                           true, 100);
        // Dispatcher slot was unregistered or duplicate
        CHECK(!replay_outcome.routed);
    }

    // 6. Outcome Classification: is_nominal_already_set mapping
    {
        CommandRunner runner;
        runner.vcsec_session().set_established({1}, 10, 1000);

        BleUuid cmd_uuid = {0x11, 0x22, 0x33, 0x44};
        runner.enqueue("DoorLock", BleDomain::VehicleSecurity, WakePolicy::WakeIfNeeded, 20000, 1000, cmd_uuid);
        runner.tick(1000, true, true);
        runner.notify_tx_complete(1010);

        // Preserve the existing exact already_set compatibility rule.
        auto outcome = runner.handle_response(BleDomain::VehicleSecurity, cmd_uuid.data(), cmd_uuid.size(),
                                             false, 0, false, "already_set");
        CHECK(outcome.routed);
        CommandRequest* cmd = runner.current_command();
        CHECK(cmd->is_completed);
        CHECK(cmd->is_success); // Treated as idempotent success!
        CHECK(cmd->is_already_set);
        CHECK(cmd->terminal_reason == TerminalReason::AlreadySet);
        CHECK(cmd->error_message == "command executed successfully");
    }

    // Policy configuration is scoped to an unsent queued ID and resets on slot reuse.
    {
        CommandRunner runner;
        runner.vcsec_session().set_established({1},10,1000);
        runner.info_session().set_established({2},20,1000);
        CHECK(!runner.set_command_effect(0,CommandEffect::Mutating,CommandOutcomePolicy::ChargeStart));
        const auto first=runner.enqueue("Opaque first",BleDomain::Infotainment,WakePolicy::WakeIfNeeded,20000,1000);
        const auto second=runner.enqueue("Opaque second",BleDomain::Infotainment,WakePolicy::WakeIfNeeded,20000,1000);
        CHECK(runner.set_command_effect(second,CommandEffect::Mutating,CommandOutcomePolicy::ChargeStop));
        CHECK(runner.current_command()->outcome_policy==CommandOutcomePolicy::AlreadySetOnly);
        CHECK(runner.set_command_effect(first,CommandEffect::Mutating,CommandOutcomePolicy::ChargeStart));
        CHECK(!runner.set_command_effect(second+1,CommandEffect::ReadOnly,CommandOutcomePolicy::ChargeStop));
        // Isolate possible transmission from the separate non-Idle state guard.
        auto* queued=runner.current_command();
        queued->payload_maybe_sent=true;
        CHECK(queued->state==CommandState::Idle);
        CHECK(!runner.set_command_effect(first,CommandEffect::ReadOnly,CommandOutcomePolicy::ChargeStop));
        CHECK(queued->effect==CommandEffect::Mutating && queued->outcome_policy==CommandOutcomePolicy::ChargeStart);
        CHECK(queued->payload_maybe_sent);
        queued->payload_maybe_sent=false;
        CHECK(runner.tick(1000,true,true)==TxAction::SendCommandPayload);
        CHECK(!runner.set_command_effect(first,CommandEffect::ReadOnly,CommandOutcomePolicy::ChargeStop));
        CHECK(runner.current_command()->effect==CommandEffect::Mutating);
        CHECK(runner.current_command()->outcome_policy==CommandOutcomePolicy::ChargeStart);
        runner.notify_tx_complete(TxAction::SendCommandPayload,1010);
        const auto uuid=runner.current_command()->uuid;
        CHECK(runner.handle_response(BleDomain::Infotainment,uuid.data(),uuid.size(),true,100,false,"complete",true).routed);
        CHECK(runner.current_command()->is_completed && runner.current_command()->is_success);
        CHECK(!runner.set_command_effect(first,CommandEffect::ReadOnly,CommandOutcomePolicy::ChargeStop));
        runner.pop_current();
        CHECK(runner.current_command()->id==second && runner.current_command()->outcome_policy==CommandOutcomePolicy::ChargeStop);
        runner.cancel_current();runner.pop_current();
        for (size_t i=0;i<CommandRunner::kMaxQueueSize+1;++i) {
            CHECK(runner.enqueue("Opaque reused slot",BleDomain::Infotainment,WakePolicy::WakeIfNeeded,20000,1000)!=0);
            CHECK(runner.current_command()->outcome_policy==CommandOutcomePolicy::AlreadySetOnly);
            CHECK(runner.current_command()->effect==CommandEffect::Mutating);
            runner.cancel_current();runner.pop_current();
        }
    }

    // Contextual charge no-ops complete the actual runner/callback only after admission.
    {
        struct Sample { const char* reason; bool start; bool stop; };
        const Sample samples[]={{"is_charging",true,false},{"complete",true,false},
            {"not_charging",false,true},{"Infotainment action failed: is_charging",true,false},
            {"Infotainment action failed: complete",true,false},
            {"Infotainment action failed: not_charging",false,true},
            {"requested",false,false},{"complete_suffix",false,false},
            {"command outcome unknown; command may have executed; complete",false,false}};
        for (const auto& sample : samples) for (auto policy : {CommandOutcomePolicy::AlreadySetOnly,
                CommandOutcomePolicy::ChargeStart,CommandOutcomePolicy::ChargeStop}) {
            for (bool authenticated : {false,true}) for (bool has_counter : {false,true})
                for (auto domain : {BleDomain::Infotainment,BleDomain::VehicleSecurity}) {
                CommandRunner runner;
                runner.vcsec_session().set_established({1},10,1000);
                runner.info_session().set_established({2},20,1000);
                BleUuid uuid={0x51,0x52,0x53,0x54};
                unsigned calls=0; bool callback_ok=false;
                const auto id = runner.enqueue("Opaque label",domain,WakePolicy::WakeIfNeeded,20000,1000,uuid,
                    [&](bool ok,const std::string&,CommandFailureInfo){++calls;callback_ok=ok;},
                    false,3,VcsecReplyKind::Action);
                CHECK(runner.set_command_effect(id,CommandEffect::Mutating,policy));
                CHECK(runner.current_command()->outcome_policy==policy);
                CHECK(runner.tick(1000,true,true)==TxAction::SendCommandPayload);
                runner.notify_tx_complete(TxAction::SendCommandPayload,1010);
                const bool expected=authenticated && has_counter && domain==BleDomain::Infotainment &&
                    ((policy==CommandOutcomePolicy::ChargeStart && sample.start) ||
                     (policy==CommandOutcomePolicy::ChargeStop && sample.stop));
                auto routed=runner.handle_response(domain,uuid.data(),uuid.size(),has_counter,100,
                                                   false,sample.reason,authenticated);
                CHECK(routed.routed);
                auto* cmd=runner.current_command(); CHECK(cmd && cmd->is_completed);
                CHECK(cmd->is_success==expected && cmd->is_already_set==expected);
                CHECK(calls==1 && callback_ok==expected);
                if (expected) CHECK(cmd->terminal_reason==TerminalReason::AlreadySet);
                else CHECK(cmd->failure.outcome==(authenticated ? CommandError::Outcome::DefinitelyFailed
                                                                : CommandError::Outcome::MayHaveSucceeded));
                CHECK(runner.tick(1020,true,true)==TxAction::None);
                CHECK(calls==1);
            }
        }
    }
    // A wrong/currently completed UUID cannot normalize or invoke a callback; queue reuse
    // restores the default policy even when the human-readable name resembles a start command.
    {
        CommandRunner runner;
        runner.vcsec_session().set_established({1},10,1000);
        runner.info_session().set_established({2},20,1000);
        BleUuid uuid={0x61,0x62,0x63,0x64}, wrong={0x71,0x72,0x73,0x74};
        unsigned calls=0;bool success=false;
        auto done=[&](bool ok,const std::string&,CommandFailureInfo){++calls;success=ok;};
        const auto id = runner.enqueue("Opaque",BleDomain::Infotainment,WakePolicy::WakeIfNeeded,20000,1000,uuid,done);
        CHECK(runner.set_command_effect(id,CommandEffect::Mutating,CommandOutcomePolicy::ChargeStart));
        CHECK(runner.tick(1000,true,true)==TxAction::SendCommandPayload);
        runner.notify_tx_complete(TxAction::SendCommandPayload,1010);
        CHECK(!runner.handle_response(BleDomain::Infotainment,wrong.data(),wrong.size(),true,100,false,"complete",true).routed);
        CHECK(calls==0 && !runner.current_command()->is_completed);
        CHECK(runner.handle_response(BleDomain::Infotainment,uuid.data(),uuid.size(),true,100,false,"complete",true).routed);
        CHECK(calls==1 && success);
        CHECK(!runner.handle_response(BleDomain::Infotainment,uuid.data(),uuid.size(),true,100,false,"complete",true).routed);
        CHECK(calls==1);
        runner.pop_current();
        runner.enqueue("Start Charging",BleDomain::Infotainment,WakePolicy::WakeIfNeeded,20000,2000,wrong,done);
        CHECK(runner.current_command()->outcome_policy==CommandOutcomePolicy::AlreadySetOnly);
        CHECK(runner.tick(2000,true,true)==TxAction::SendCommandPayload);
        runner.notify_tx_complete(TxAction::SendCommandPayload,2010);
        CHECK(!runner.handle_response(BleDomain::Infotainment,uuid.data(),uuid.size(),true,200,false,"complete",true).routed);
        CHECK(calls==1 && !runner.current_command()->is_completed);
        CHECK(runner.handle_response(BleDomain::Infotainment,wrong.data(),wrong.size(),true,200,false,"complete",true).routed);
        CHECK(calls==2 && !success);
    }

    // A no-op response cannot resurrect a timed-out/cancelled/cleared mutation.
    for (int terminal=0;terminal<3;++terminal) {
        CommandRunner runner;
        runner.vcsec_session().set_established({1},10,1000);
        runner.info_session().set_established({2},20,1000);
        BleUuid uuid={0x31,0x32,0x33,0x34};
        unsigned calls=0;bool success=true;std::string error;
        const auto id = runner.enqueue("Opaque",BleDomain::Infotainment,WakePolicy::WakeIfNeeded,20000,1000,uuid,
            [&](bool ok,const std::string& reason,CommandFailureInfo){++calls;success=ok;error=reason;});
        CHECK(runner.set_command_effect(id,CommandEffect::Mutating,CommandOutcomePolicy::ChargeStart));
        CHECK(runner.tick(1000,true,true)==TxAction::SendCommandPayload);
        runner.notify_tx_complete(TxAction::SendCommandPayload,1010);
        if (terminal==0) CHECK(runner.tick(1010+CommandRunner::kDefaultResponseTimeoutMs,true,true)==TxAction::None);
        else if (terminal==1) runner.cancel_current();
        else runner.clear();
        CHECK(calls==1 && !success);
        CHECK(error.find(uncertain_command_reason())==0);
        const auto retained=error;
        CHECK(!runner.handle_response(BleDomain::Infotainment,uuid.data(),uuid.size(),true,100,false,"complete",true).routed);
        CHECK(calls==1 && !success && error==retained);
        CHECK(runner.tick(15000,true,true)==TxAction::None);
        CHECK(calls==1);
    }

    // 7. Generic Error Outcome
    {
        CommandRunner runner;
        runner.vcsec_session().set_established({1}, 10, 1000);

        BleUuid cmd_uuid = {0x55, 0x66, 0x77, 0x88};
        runner.enqueue("Honk", BleDomain::VehicleSecurity, WakePolicy::WakeIfNeeded, 20000, 1000, cmd_uuid);
        runner.tick(1000, true, true);
        runner.notify_tx_complete(1010);

        auto outcome = runner.handle_response(BleDomain::VehicleSecurity, cmd_uuid.data(), cmd_uuid.size(),
                                             false, 0, false, "remote_access_disabled");
        CHECK(outcome.routed);
        CommandRequest* cmd = runner.current_command();
        CHECK(cmd->is_completed);
        CHECK(!cmd->is_success);
        CHECK(!cmd->is_already_set);
        CHECK(cmd->failure.outcome == CommandError::Outcome::MayHaveSucceeded);
        CHECK(cmd->error_message == std::string(uncertain_command_reason()) + "; remote_access_disabled");
        CHECK(cmd->terminal_reason == TerminalReason::AuthenticationFailed);
    }

    // Read-only polling retains bounded 1/2/4-second retries. Mutations never enter this path.
    for (auto domain : {BleDomain::VehicleSecurity, BleDomain::Infotainment}) {
        CommandRunner runner;
        runner.vcsec_session().set_established({1}, 10, 1000);
        runner.info_session().set_established({2}, 20, 1000);
        const auto id = runner.enqueue("Read status", domain, WakePolicy::WakeIfNeeded, 60000, 1000);
        CHECK(runner.set_command_effect(id, CommandEffect::ReadOnly));
        CHECK(runner.tick(1000, true, true) == TxAction::SendCommandPayload);
        runner.notify_tx_complete(1010);
        uint32_t tx = 1010;
        for (unsigned retry = 0; retry < 3; ++retry) {
            const uint32_t deadline = tx + CommandRunner::kDefaultResponseTimeoutMs;
            CHECK(runner.tick(deadline, true, true) == TxAction::None);
            const uint32_t delay = 1000U << retry;
            CHECK(runner.current_command()->next_retry_delay_ms == delay);
            CHECK(runner.tick(deadline + delay - 1, true, true) == TxAction::None);
            CHECK(runner.tick(deadline + delay, true, true) == TxAction::SendCommandPayload);
            tx = deadline + delay;
            runner.notify_tx_complete(tx);
        }
        CHECK(runner.tick(tx + CommandRunner::kDefaultResponseTimeoutMs, true, true) == TxAction::None);
        CHECK(runner.current_command()->terminal_reason == TerminalReason::MaxRetriesExceeded);
        CHECK(runner.current_command()->is_completed);
    }

    // 9. Overall Command Deadline Exhaustion
    {
        CommandRunner runner;
        runner.vcsec_session().set_established({1}, 10, 1000);

        runner.enqueue("TestDeadline", BleDomain::VehicleSecurity, WakePolicy::WakeIfNeeded, 5000, 1000);
        TxAction act = runner.tick(1000, true, true);
        CHECK(act == TxAction::SendCommandPayload);
        runner.notify_tx_complete(1050);

        // Tick after deadline (1000 + 5000 = 6000)
        act = runner.tick(6001, true, true);
        CHECK(act == TxAction::None);
        CommandRequest* cmd = runner.current_command();
        CHECK(cmd->is_completed);
        CHECK(!cmd->is_success);
        CHECK(cmd->terminal_reason == TerminalReason::DeadlineExceeded);
    }

    // 10. Disconnection during waiting
    {
        CommandRunner runner;
        runner.vcsec_session().set_established({1}, 10, 1000);

        runner.enqueue("TestDisconnect", BleDomain::VehicleSecurity, WakePolicy::WakeIfNeeded, 5000, 1000);
        runner.tick(1000, true, true);
        runner.notify_tx_complete(1050);

        // Disconnected at 2000ms: within deadline, runner waits
        TxAction act = runner.tick(2000, false /* disconnected */, true);
        CHECK(act == TxAction::None);
        CHECK(!runner.current_command()->is_completed);

        // Disconnected past deadline (6001ms): deadline exhausted
        act = runner.tick(6001, false, true);
        CHECK(act == TxAction::None);
        CHECK(runner.current_command()->is_completed);
        CHECK(!runner.current_command()->is_success);
        CHECK(runner.current_command()->terminal_reason == TerminalReason::DeadlineExceeded);
        CHECK(runner.current_command()->error_message == std::string(uncertain_command_reason()) +
              "; connection lost; command deadline exhausted");
    }

    // 10b. Disconnection while in Idle state (enqueued while offline) exhausts deadline
    {
        CommandRunner runner;
        runner.enqueue("OfflineEnqueue", BleDomain::VehicleSecurity, WakePolicy::WakeIfNeeded, 5000, 1000);
        CHECK(runner.current_command()->state == CommandState::Idle);

        // Before deadline (3000ms < 1000 + 5000): waiting
        TxAction act = runner.tick(3000, false /* disconnected */, false);
        CHECK(act == TxAction::None);
        CHECK(!runner.current_command()->is_completed);

        // Past deadline (6001ms): must fail with DeadlineExceeded, not hang in Idle!
        act = runner.tick(6001, false /* disconnected */, false);
        CHECK(act == TxAction::None);
        CHECK(runner.current_command()->is_completed);
        CHECK(!runner.current_command()->is_success);
        CHECK(runner.current_command()->terminal_reason == TerminalReason::DeadlineExceeded);
        CHECK(runner.current_command()->error_message == "connection lost; command deadline exhausted");
    }

    // 10c. Disconnection while in Ready state (during retry backoff delay) exhausts deadline
    {
        CommandRunner runner;
        runner.vcsec_session().set_established({1}, 10, 1000);
        const auto id = runner.enqueue("ReadStatus", BleDomain::VehicleSecurity, WakePolicy::WakeIfNeeded, 10000, 1000);
        CHECK(runner.set_command_effect(id, CommandEffect::ReadOnly));
        runner.tick(1000, true, true);
        runner.notify_tx_complete(1050);

        // Response timeout at 8060 (1050 + 7000) transitions to Ready with backoff delay
        runner.tick(8060, true, true);
        CHECK(runner.current_command()->state == CommandState::Ready);
        CHECK(runner.current_command()->next_retry_delay_ms == 1000);

        // Disconnected at 8200ms while still in backoff: waits
        TxAction act = runner.tick(8200, false /* disconnected */, true);
        CHECK(act == TxAction::None);
        CHECK(!runner.current_command()->is_completed);

        // Past overall deadline (11001ms >= 1000 + 10000): must fail with DeadlineExceeded!
        act = runner.tick(11001, false /* disconnected */, true);
        CHECK(act == TxAction::None);
        CHECK(runner.current_command()->is_completed);
        CHECK(!runner.current_command()->is_success);
        CHECK(runner.current_command()->terminal_reason == TerminalReason::DeadlineExceeded);
        CHECK(runner.current_command()->error_message == std::string(uncertain_command_reason()) +
              "; connection lost; command deadline exhausted");
    }

    // 11. RxFramer Integration in CommandRunner
    {
        CommandRunner runner;
        CHECK(runner.rx_framer().timeout_ms() == 3000);
        std::vector<uint8_t> received;

        // Valid frame: 2-byte length 4, followed by 4 payload bytes
        std::vector<uint8_t> chunk = {0x00, 0x04, 0xDE, 0xAD, 0xBE, 0xEF};
        size_t extracted = runner.push_rx_chunk(
            chunk.data(), chunk.size(), 1000,
            [&](const uint8_t* payload, size_t len) {
                received.assign(payload, payload + len);
            },
            [](RxFramerDropReason, size_t) {});

        CHECK(extracted == 1);
        CHECK(received.size() == 4);
        CHECK(received[0] == 0xDE && received[3] == 0xEF);

        // Incomplete chunk: 2-byte header + 2 payload bytes (needs 4 payload bytes)
        std::vector<uint8_t> partial = {0x00, 0x04, 0xAA, 0xBB};
        runner.push_rx_chunk(partial.data(), partial.size(), 1000,
                             [](const uint8_t*, size_t) {},
                             [](RxFramerDropReason, size_t) {});
        CHECK(runner.rx_framer().buffered_bytes() == 4);

        // At t = 3500 (elapsed = 2500ms <= 3000ms), timeout must not drop
        bool dropped_early = false;
        runner.rx_framer().check_timeout(3500, [&](RxFramerDropReason, size_t) {
            dropped_early = true;
        });
        CHECK(!dropped_early);
        CHECK(runner.rx_framer().buffered_bytes() == 4);

        // At t = 4001 (elapsed = 3001ms > 3000ms), timeout drops
        RxFramerDropReason drop_reason = RxFramerDropReason::None;
        size_t dropped_bytes = 0;
        runner.rx_framer().check_timeout(4001, [&](RxFramerDropReason reason, size_t dropped) {
            drop_reason = reason;
            dropped_bytes = dropped;
        });
        CHECK(drop_reason == RxFramerDropReason::InterChunkTimeout);
        CHECK(dropped_bytes == 4);
        CHECK(runner.rx_framer().buffered_bytes() == 0);
    }
}

static void test_command_runner_link_loss_and_faults() {
    using namespace tk;

    // R1: a VehicleStatus with closureStatuses confirms the wake while the raw VCSEC flag still
    // reads ASLEEP. The command then proceeds to the infotainment session instead of re-sending
    // Wake on every status reply (upstream v5.2.0 semantics; the loop sent one Wake per reply).
    {
        CommandRunner runner;
        runner.vcsec_session().set_established({1}, 10, 1000);
        runner.enqueue("Set Charging Amps", BleDomain::Infotainment, WakePolicy::WakeIfNeeded, 30000, 0);
        CHECK(runner.tick(0, true, false, true) == TxAction::SendWake);
        runner.notify_tx_complete(0);
        CHECK(runner.current_command()->state == CommandState::WaitingWake);
        CHECK(!runner.current_command()->wake_confirmed);

        runner.notify_vehicle_awake(true);   // VehicleStatus{ASLEEP, closureStatuses}
        CHECK(runner.current_command()->wake_confirmed);
        CHECK(runner.tick(350, true, false, true) == TxAction::SendInfoSessionInfoRequest);
        runner.notify_vehicle_awake(true);   // further status replies change nothing
        CHECK(runner.current_command()->state == CommandState::WaitingInfoAuth);
        CHECK(runner.tick(700, true, false, true) == TxAction::None);
        CHECK(runner.current_command()->retry_count == 0);

        // After the infotainment session is up, the payload goes out despite the ASLEEP flag.
        runner.info_session().set_established({2}, 20, 1000);
        runner.current_command()->state = CommandState::Idle;
        CHECK(runner.tick(900, true, false, true) == TxAction::SendCommandPayload);
    }
    // A confirmed VCSEC AWAKE while waiting for the wake is final as well.
    {
        CommandRunner runner;
        runner.vcsec_session().set_established({1}, 10, 1000);
        runner.enqueue("Start Charging", BleDomain::Infotainment, WakePolicy::WakeIfNeeded, 30000, 0);
        CHECK(runner.tick(0, true, false, false) == TxAction::SendWake);   // sleep state unknown
        runner.notify_tx_complete(0);
        CHECK(runner.tick(300, true, true, false) == TxAction::SendInfoSessionInfoRequest);
        CHECK(runner.current_command()->wake_confirmed);
    }
    // Status frames arriving early while waiting for VCSEC auth must not confirm wake
    // when car is reported asleep: after VCSEC auth, tick emits SendWake.
    {
        CommandRunner runner;
        // VCSEC session not yet established
        runner.enqueue("Set Charge Limit", BleDomain::Infotainment, WakePolicy::WakeIfNeeded, 30000, 0);
        CHECK(runner.tick(0, true, false, true) == TxAction::SendVcsecSessionInfoRequest);
        CHECK(runner.current_command()->state == CommandState::WaitingVcsecAuth);
        CHECK(!runner.current_command()->wake_confirmed);

        // VehicleStatus arrives with closureStatuses while still waiting for VCSEC session
        runner.notify_vehicle_awake(true);
        // Not in WaitingWake or EnsuringAwake, so wake is not confirmed
        CHECK(!runner.current_command()->wake_confirmed);
        CHECK(runner.current_command()->state == CommandState::WaitingVcsecAuth);

        // VCSEC session finishes authentication
        runner.vcsec_session().set_established({1}, 10, 1000);
        runner.current_command()->state = CommandState::Idle;

        // Vehicle is asleep, so tick emits SendWake rather than advancing to infotainment auth
        CHECK(runner.tick(500, true, false, true) == TxAction::SendWake);
        CHECK(runner.current_command()->state == CommandState::WaitingWake);
    }

    // S1: Step timeout on info auth on asleep car resets wake_confirmed and emits SendWake
    {
        CommandRunner runner;
        runner.vcsec_session().set_established({1}, 10, 1000);
        runner.enqueue("Set Charge Limit", BleDomain::Infotainment, WakePolicy::WakeIfNeeded, 30000, 0);

        // Step 1: Car is asleep, tick emits SendWake
        CHECK(runner.tick(0, true, false, true) == TxAction::SendWake);
        runner.notify_tx_complete(0);
        CHECK(runner.current_command()->state == CommandState::WaitingWake);

        // Wake confirmed while waiting for wake
        runner.notify_vehicle_awake(true);
        CHECK(runner.current_command()->wake_confirmed);

        // Step 2: Advances to SendInfoSessionInfoRequest at t=100
        CHECK(runner.tick(100, true, false, true) == TxAction::SendInfoSessionInfoRequest);
        runner.notify_tx_complete(100);
        CHECK(runner.current_command()->state == CommandState::WaitingInfoAuth);
        CHECK(runner.current_command()->wake_confirmed);

        // Step 3: Timeout waiting for info auth at t = 100 + kDefaultStepTimeoutMs (5000) = 5100 ms
        // On step timeout, retry_count increments, wake_confirmed resets to false, state resets to Idle
        CHECK(runner.tick(5100, true, false, true) == TxAction::None);
        CHECK(!runner.current_command()->wake_confirmed);
        CHECK(runner.current_command()->retry_count == 1);
        CHECK(runner.current_command()->state == CommandState::Idle);

        // Step 4: Next tick on asleep car re-evaluates sleep and emits SendWake
        CHECK(runner.tick(5150, true, false, true) == TxAction::None);
        CHECK(runner.tick(6100, true, false, true) == TxAction::SendWake);
        CHECK(runner.current_command()->state == CommandState::WaitingWake);
    }

    // A transport failure after crossing the actual payload boundary is ambiguous. It cannot
    // authorize a second Wake or payload even if the last no-response chunk may have arrived.
    {
        CommandRunner runner;
        runner.vcsec_session().set_established({1}, 10, 1000);
        runner.info_session().set_established({2}, 10, 1000);
        runner.enqueue("Set Charge Limit", BleDomain::Infotainment, WakePolicy::WakeIfNeeded, 30000, 0);
        CHECK(runner.tick(0, true, true) == TxAction::SendCommandPayload);
        runner.notify_tx_started(TxAction::SendCommandPayload);
        runner.notify_tx_failed("BLE write failed", 100);
        CHECK(runner.current_command()->is_completed);
        CHECK(runner.current_command()->failure.outcome == CommandError::Outcome::MayHaveSucceeded);
        CHECK(runner.tick(10000, true, false, true) == TxAction::None);
        CHECK(runner.current_command()->retry_count == 0);
    }
    // No confirmation: an asleep car still gets the wake policy (unchanged), and a NoWakeFail
    // command still fails locally with the text the link backstop classifies as LocalPolicy.
    {
        CommandRunner runner;
        runner.vcsec_session().set_established({1}, 10, 1000);
        runner.enqueue("Charge State Poll", BleDomain::Infotainment, WakePolicy::NoWakeFail, 10000, 0);
        CHECK(runner.tick(0, true, false, true) == TxAction::None);
        CHECK(runner.current_command()->terminal_reason == TerminalReason::VehicleAsleep);
        CHECK(tk::classify_command_failure(runner.current_command()->error_message) ==
              tk::CommandFailureOrigin::LocalPolicy);
    }

    // M1: Link loss fails command immediately
    {
        CommandRunner runner;
        runner.vcsec_session().set_established({1}, 10, 1000);
        runner.enqueue("FlashLights", BleDomain::VehicleSecurity, WakePolicy::WakeIfNeeded, 20000, 1000);
        runner.tick(1000, true, true, false);
        runner.notify_tx_complete(1050);
        CHECK(runner.has_active_command());
        CHECK(runner.current_command()->state == CommandState::AwaitingResponse);

        // Notify link loss
        runner.notify_link_lost("connection lost");
        CommandRequest* cmd = runner.current_command();
        CHECK(cmd->is_completed);
        CHECK(!cmd->is_success);
        CHECK(cmd->terminal_reason == TerminalReason::BleDisconnected);
        CHECK(cmd->error_message == std::string(uncertain_command_reason()) + "; connection lost");
    }

    // M1: Signed message fault (not session error) fails command immediately
    {
        CommandRunner runner;
        runner.vcsec_session().set_established({1}, 10, 1000);
        runner.enqueue("HonkHorn", BleDomain::VehicleSecurity, WakePolicy::WakeIfNeeded, 20000, 1000);
        runner.tick(1000, true, true, false);
        runner.notify_tx_complete(1050);

        runner.notify_signed_message_fault(false, "signed message authentication failed");
        CommandRequest* cmd = runner.current_command();
        CHECK(cmd->is_completed);
        CHECK(!cmd->is_success);
        CHECK(cmd->terminal_reason == TerminalReason::AuthenticationFailed);
        CHECK(cmd->error_message == std::string(uncertain_command_reason()) + "; signed message authentication failed");
    }

    // Authenticated, payload-scoped temporary rejection can be retried. The same advisory
    // numeric fault cannot erase uncertainty after an actual mutating transmission.
    for (bool authenticated : {false, true}) {
        CommandRunner runner;
        runner.vcsec_session().set_established({1}, 10, 1000);
        runner.enqueue("HonkHorn", BleDomain::VehicleSecurity, WakePolicy::WakeIfNeeded, 20000, 1000);
        CHECK(runner.tick(1000, true, true) == TxAction::SendCommandPayload);
        runner.notify_tx_complete(1050);
        runner.notify_message_fault(classify_message_fault(15), authenticated, 1100);
        const auto* cmd = runner.current_command();
        if (authenticated) {
            CHECK(!cmd->is_completed && !cmd->payload_maybe_sent && cmd->retry_count == 1);
            CHECK(runner.tick(2099, true, true) == TxAction::None);
            CHECK(runner.tick(2100, true, true) == TxAction::SendCommandPayload);
        } else {
            CHECK(cmd->is_completed && cmd->retry_count == 0 && cmd->payload_maybe_sent);
            CHECK(cmd->failure.outcome == CommandError::Outcome::MayHaveSucceeded);
            CHECK(runner.tick(10000, true, true) == TxAction::None);
        }
    }

    // N1/N2: VCSEC commands run even while vehicle is asleep
    {
        CommandRunner runner;
        runner.vcsec_session().set_established({1}, 10, 1000);
        runner.enqueue("VCSEC Health Poll", BleDomain::VehicleSecurity, WakePolicy::NoWakeFail, 20000, 1000);

        // Tick while vehicle is ASLEEP: must NOT fail with "vehicle asleep"!
        TxAction act = runner.tick(1000, true /* connected */, false /* awake */, true /* asleep */);
        CHECK(act == TxAction::SendCommandPayload);
        CommandRequest* cmd = runner.current_command();
        CHECK(cmd->state == CommandState::Ready);
        CHECK(!cmd->is_completed);
    }
}

static void test_command_runner_telemetry_filter_logic() {
    // 1. is_session_info_uuid_matching
    {
        const uint8_t exp[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        const uint8_t same[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        const uint8_t diff[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 99};
        const uint8_t short_uuid[8] = {1, 2, 3, 4, 5, 6, 7, 8};

        // Empty/omitted UUID in response is valid (matches)
        CHECK(tk::is_session_info_uuid_matching(nullptr, 0, exp, sizeof(exp)));
        CHECK(tk::is_session_info_uuid_matching(same, 0, exp, sizeof(exp)));

        // Matching 16-byte UUID
        CHECK(tk::is_session_info_uuid_matching(same, sizeof(same), exp, sizeof(exp)));

        // Mismatched byte in 16-byte UUID
        CHECK(!tk::is_session_info_uuid_matching(diff, sizeof(diff), exp, sizeof(exp)));

        // Mismatched length
        CHECK(!tk::is_session_info_uuid_matching(short_uuid, sizeof(short_uuid), exp, sizeof(exp)));
        CHECK(!tk::is_session_info_uuid_matching(same, sizeof(same), short_uuid, sizeof(short_uuid)));

        // Nullptr handling with non-zero length
        CHECK(!tk::is_session_info_uuid_matching(nullptr, 16, exp, sizeof(exp)));
        CHECK(!tk::is_session_info_uuid_matching(same, sizeof(same), nullptr, 16));
    }

    // 2. is_command_awaiting_session_auth
    {
        using CS = tk::CommandState;
        using BD = tk::BleDomain;

        // Inactive / completed commands are never awaiting auth
        CHECK(!tk::is_command_awaiting_session_auth(false, false, CS::WaitingVcsecAuth, BD::VehicleSecurity));
        CHECK(!tk::is_command_awaiting_session_auth(true, true, CS::WaitingVcsecAuth, BD::VehicleSecurity));

        // VehicleSecurity session auth gate
        CHECK(tk::is_command_awaiting_session_auth(true, false, CS::WaitingVcsecAuth, BD::VehicleSecurity));
        CHECK(!tk::is_command_awaiting_session_auth(true, false, CS::WaitingInfoAuth, BD::VehicleSecurity));
        CHECK(!tk::is_command_awaiting_session_auth(true, false, CS::Idle, BD::VehicleSecurity));
        CHECK(!tk::is_command_awaiting_session_auth(true, false, CS::Ready, BD::VehicleSecurity));

        // Infotainment session auth gate
        CHECK(tk::is_command_awaiting_session_auth(true, false, CS::WaitingInfoAuth, BD::Infotainment));
        CHECK(!tk::is_command_awaiting_session_auth(true, false, CS::WaitingVcsecAuth, BD::Infotainment));
        CHECK(!tk::is_command_awaiting_session_auth(true, false, CS::Idle, BD::Infotainment));
        CHECK(!tk::is_command_awaiting_session_auth(true, false, CS::Ready, BD::Infotainment));

        // None / Broadcast domain never awaits session auth
        CHECK(!tk::is_command_awaiting_session_auth(true, false, CS::WaitingVcsecAuth, BD::None));
        CHECK(!tk::is_command_awaiting_session_auth(true, false, CS::WaitingInfoAuth, BD::Broadcast));
    }

    // 3. is_fault_domain_matching
    {
        using CS = tk::CommandState;
        using BD = tk::BleDomain;

        // Broadcast faults match any command regardless of domain or state
        CHECK(tk::is_fault_domain_matching(BD::Broadcast, BD::VehicleSecurity, CS::Ready));
        CHECK(tk::is_fault_domain_matching(BD::Broadcast, BD::Infotainment, CS::Ready));
        // Unknown domain (None) must NOT match any command (fail-closed)
        CHECK(!tk::is_fault_domain_matching(BD::None, BD::VehicleSecurity, CS::Idle));
        CHECK(!tk::is_fault_domain_matching(BD::None, BD::Infotainment, CS::Ready));

        // VehicleSecurity faults match VCSEC commands in any state
        CHECK(tk::is_fault_domain_matching(BD::VehicleSecurity, BD::VehicleSecurity, CS::Ready));
        CHECK(tk::is_fault_domain_matching(BD::VehicleSecurity, BD::VehicleSecurity, CS::WaitingVcsecAuth));
        CHECK(tk::is_fault_domain_matching(BD::VehicleSecurity, BD::VehicleSecurity, CS::AwaitingResponse));

        // VehicleSecurity faults match Infotainment commands ONLY if in WaitingVcsecAuth
        CHECK(tk::is_fault_domain_matching(BD::VehicleSecurity, BD::Infotainment, CS::WaitingVcsecAuth));
        CHECK(!tk::is_fault_domain_matching(BD::VehicleSecurity, BD::Infotainment, CS::WaitingInfoAuth));
        CHECK(!tk::is_fault_domain_matching(BD::VehicleSecurity, BD::Infotainment, CS::Ready));
        CHECK(!tk::is_fault_domain_matching(BD::VehicleSecurity, BD::Infotainment, CS::AwaitingResponse));

        // Infotainment faults match Infotainment commands only in Infotainment execution phases,
        // and must NOT match during prerequisite VCSEC auth, wake, or idle phases
        CHECK(tk::is_fault_domain_matching(BD::Infotainment, BD::Infotainment, CS::WaitingInfoAuth));
        CHECK(tk::is_fault_domain_matching(BD::Infotainment, BD::Infotainment, CS::Ready));
        CHECK(tk::is_fault_domain_matching(BD::Infotainment, BD::Infotainment, CS::AwaitingResponse));
        CHECK(!tk::is_fault_domain_matching(BD::Infotainment, BD::Infotainment, CS::WaitingVcsecAuth));
        CHECK(!tk::is_fault_domain_matching(BD::Infotainment, BD::Infotainment, CS::WaitingWake));
        CHECK(!tk::is_fault_domain_matching(BD::Infotainment, BD::Infotainment, CS::Idle));
        CHECK(!tk::is_fault_domain_matching(BD::Infotainment, BD::VehicleSecurity, CS::Ready));
        CHECK(!tk::is_fault_domain_matching(BD::Infotainment, BD::VehicleSecurity, CS::WaitingVcsecAuth));
    }

    // 4. evaluate_vcsec_operation_status
    {
        auto d_ok = tk::evaluate_vcsec_operation_status(0);
        CHECK(d_ok.action == tk::VcsecOpStatusAction::CompleteOk);
        CHECK(d_ok.is_ok);
        CHECK(std::string(d_ok.error_message).empty());

        auto d_wait = tk::evaluate_vcsec_operation_status(1);
        CHECK(d_wait.action == tk::VcsecOpStatusAction::Wait);
        CHECK(!d_wait.is_ok);
        CHECK(std::string(d_wait.error_message).empty());

        auto d_err = tk::evaluate_vcsec_operation_status(2);
        CHECK(d_err.action == tk::VcsecOpStatusAction::Error);
        CHECK(!d_err.is_ok);
        CHECK(std::string(d_err.error_message) == "VCSEC command failed with error status");

        auto d_unknown = tk::evaluate_vcsec_operation_status(99);
        CHECK(d_unknown.action == tk::VcsecOpStatusAction::Error);
        CHECK(!d_unknown.is_ok);
        CHECK(std::string(d_unknown.error_message) == "VCSEC command failed with error status");
    }

    // 5. CommandRunner wrapper methods: is_awaiting_session_auth & should_notify_signed_message_fault
    {
        tk::CommandRunner runner;
        // Empty queue: neither auth query is active
        CHECK(!runner.is_awaiting_session_auth(tk::BleDomain::VehicleSecurity));
        CHECK(!runner.is_awaiting_session_auth(tk::BleDomain::Infotainment));
        CHECK(!runner.should_notify_signed_message_fault(tk::BleDomain::VehicleSecurity));
        CHECK(!runner.should_notify_signed_message_fault(tk::BleDomain::Infotainment));

        // Enqueue Infotainment command: starts in WaitingVcsecAuth on tick
        runner.enqueue("Climate", tk::BleDomain::Infotainment, tk::WakePolicy::WakeIfNeeded, 20000, 1000);
        runner.tick(1000, true, true);
        CHECK(runner.current_command()->state == tk::CommandState::WaitingVcsecAuth);

        // While awaiting VCSEC auth:
        CHECK(runner.is_awaiting_session_auth(tk::BleDomain::VehicleSecurity));
        CHECK(!runner.is_awaiting_session_auth(tk::BleDomain::Infotainment));
        CHECK(runner.should_notify_signed_message_fault(tk::BleDomain::VehicleSecurity));
        // Infotainment faults must NOT notify command while in VCSEC auth prerequisite
        CHECK(!runner.should_notify_signed_message_fault(tk::BleDomain::Infotainment));
        CHECK(runner.should_notify_signed_message_fault(tk::BleDomain::Broadcast));

        // VCSEC auth succeeds, advance to WaitingInfoAuth
        runner.vcsec_session().set_established({1, 2, 3}, 1, 1000);
        runner.current_command()->state = tk::CommandState::Idle;
        runner.tick(1100, true, true);
        CHECK(runner.current_command()->state == tk::CommandState::WaitingInfoAuth);

        // While awaiting Infotainment auth:
        CHECK(!runner.is_awaiting_session_auth(tk::BleDomain::VehicleSecurity));
        CHECK(runner.is_awaiting_session_auth(tk::BleDomain::Infotainment));
        CHECK(!runner.should_notify_signed_message_fault(tk::BleDomain::VehicleSecurity));
        CHECK(runner.should_notify_signed_message_fault(tk::BleDomain::Infotainment));

        // Infotainment auth succeeds, advance to Ready/AwaitingResponse
        runner.info_session().set_established({4, 5, 6}, 2, 1000);
        runner.current_command()->state = tk::CommandState::Idle;
        runner.tick(1200, true, true);
        runner.notify_tx_complete(1250);
        CHECK(runner.current_command()->state == tk::CommandState::AwaitingResponse);

        CHECK(!runner.is_awaiting_session_auth(tk::BleDomain::VehicleSecurity));
        CHECK(!runner.is_awaiting_session_auth(tk::BleDomain::Infotainment));
        CHECK(!runner.should_notify_signed_message_fault(tk::BleDomain::VehicleSecurity));
        CHECK(runner.should_notify_signed_message_fault(tk::BleDomain::Infotainment));

        // Pop / complete command -> all false
        runner.complete_current_command(true);
        CHECK(!runner.is_awaiting_session_auth(tk::BleDomain::Infotainment));
        CHECK(!runner.should_notify_signed_message_fault(tk::BleDomain::Infotainment));
    }
}

static void test_tx_wire_framing() {
    // B1: the TX path writes the builder's frame unchanged (tk::build_ble_tx_frame) and refuses a
    // malformed frame (tk::is_well_formed_ble_frame) — the exact helpers drive_command_runner_()
    // calls. tesla-ble builders already emit [len_hi len_lo][RoutableMessage]; the same helpers run
    // against the real library in test/test_tesla_ble_harness.cpp.
    // Stand-in builder output shaped like a real RoutableMessage (field 6 to_destination first).
    const std::vector<uint8_t> proto = {0x32, 0x02, 0x08, 0x02, 0x3a, 0x00};
    auto prefixed_builder = [&](uint8_t* buf, size_t* len) -> int {
        if (*len < proto.size() + 2) return -7;
        buf[0] = static_cast<uint8_t>(proto.size() >> 8);
        buf[1] = static_cast<uint8_t>(proto.size() & 0xFF);
        std::memcpy(buf + 2, proto.data(), proto.size());
        *len = proto.size() + 2;
        return 0;
    };

    std::vector<uint8_t> wire;
    CHECK(tk::build_ble_tx_frame(wire, tk::RxFramer::kMaxFrameLength, prefixed_builder) == 0);
    CHECK(wire.size() == proto.size() + 2);
    CHECK(wire[0] == 0x00 && wire[1] == proto.size());
    CHECK(std::memcmp(wire.data() + 2, proto.data(), proto.size()) == 0);  // no extra header
    CHECK(tk::is_well_formed_ble_frame(wire));

    // The B1 regression — a second length prefix in front of the builder frame — is refused.
    std::vector<uint8_t> doubled = {0x00, static_cast<uint8_t>(wire.size())};
    doubled.insert(doubled.end(), wire.begin(), wire.end());
    CHECK(!tk::is_well_formed_ble_frame(doubled));
    // Also for inner lengths >= 256: the high byte 0x01/0x02 still decodes as field number 0.
    for (uint8_t hi : {uint8_t{0x01}, uint8_t{0x02}}) {
        std::vector<uint8_t> big(2 + 2 + 300, 0x00);
        const size_t outer = big.size() - 2;
        big[0] = static_cast<uint8_t>(outer >> 8);
        big[1] = static_cast<uint8_t>(outer & 0xFF);
        big[2] = hi;
        big[3] = 0x2C;
        CHECK(!tk::is_well_formed_ble_frame(big));
    }

    // Builder errors, empty and oversized outputs leave the buffer empty.
    CHECK(tk::build_ble_tx_frame(wire, 4, prefixed_builder) == -7);
    CHECK(wire.empty());
    CHECK(tk::build_ble_tx_frame(wire, 16, [](uint8_t*, size_t* len) { *len = 0; return 0; }) == -1);
    CHECK(wire.empty());
    CHECK(tk::build_ble_tx_frame(wire, 16, [](uint8_t*, size_t* len) { *len = 17; return 0; }) == -1);
    CHECK(wire.empty());

    // Structural checks: the length must cover exactly the rest; the first tag needs field >= 1
    // and a known wire type; a multi-byte tag is decoded as a varint.
    CHECK(!tk::is_well_formed_ble_frame(nullptr, 0));
    const uint8_t short_len[] = {0x00, 0x05, 0x32, 0x00};            // declares 5, carries 2
    CHECK(!tk::is_well_formed_ble_frame(short_len, sizeof(short_len)));
    const uint8_t zero_field[] = {0x00, 0x02, 0x02, 0x00};           // field 0, wire type 2
    CHECK(!tk::is_well_formed_ble_frame(zero_field, sizeof(zero_field)));
    const uint8_t group_wire[] = {0x00, 0x02, 0x33, 0x00};           // field 6, wire type 3
    CHECK(!tk::is_well_formed_ble_frame(group_wire, sizeof(group_wire)));
    const uint8_t multibyte_tag[] = {0x00, 0x03, 0x92, 0x03, 0x00};  // field 50, wire type 2
    CHECK(tk::is_well_formed_ble_frame(multibyte_tag, sizeof(multibyte_tag)));
    const uint8_t truncated_tag[] = {0x00, 0x01, 0x92};              // continuation, no next byte
    CHECK(!tk::is_well_formed_ble_frame(truncated_tag, sizeof(truncated_tag)));
}

namespace {
// Stand-in for the tesla-ble Client key contract used by tk::regenerate_private_key(): a PEM
// export needs `pem_len` bytes (mbedtls_pk_write_key_pem fails on a smaller buffer).
struct FakeKeyClient {
    std::string key;            // current in-memory key ("" = none)
    std::string next = "NEW";   // key produced by the next create_private_key()
    size_t pem_len = 228;       // exported length including the NUL
    bool fail_create = false;
    bool fail_export_after_create = false;
    bool created = false;
    int loads = 0;

    bool has_private_key() const { return !key.empty(); }
    int get_private_key(uint8_t* buf, size_t cap, size_t* out) {
        if (key.empty() || (created && fail_export_after_create)) return -1;
        if (cap < pem_len) return -2;
        std::memset(buf, 0, pem_len);
        std::memcpy(buf, key.data(), std::min(key.size(), pem_len - 1));
        *out = pem_len;
        return 0;
    }
    int create_private_key() {
        if (fail_create) return -3;
        key = next;
        created = true;
        return 0;
    }
    int load_private_key(const uint8_t* buf, size_t len) {
        ++loads;
        const char* text = reinterpret_cast<const char*>(buf);
        key.assign(text, static_cast<size_t>(std::find(text, text + len, '\0') - text));
        return 0;
    }
};
}  // namespace

static void test_key_regeneration_contract() {
    // B2: tk::regenerate_private_key() is the transaction behind regenerate_key_native_(); the
    // real-library run is test/test_tesla_ble_harness.cpp. Branches here use a fake client.
    using R = tk::KeyRegenerationResult;
    CHECK(tk::kPrivateKeyPemCapacity >= 228);  // P-256 SEC1 PEM incl. NUL
    std::string stored;
    bool fail_persist = false;
    auto persist = [&](const std::vector<uint8_t>& pem) {
        if (fail_persist) return false;
        stored.assign(reinterpret_cast<const char*>(pem.data()));
        return true;
    };

    {   // success: RAM key and storage both hold the new key
        FakeKeyClient c;
        c.key = "OLD";
        stored = "OLD";
        CHECK(tk::regenerate_private_key(c, persist) == R::Committed);
        CHECK(c.key == "NEW" && stored == "NEW" && c.loads == 0);
    }
    {   // persistence failure: previous key restored in RAM, storage untouched
        FakeKeyClient c;
        c.key = "OLD";
        stored = "OLD";
        fail_persist = true;
        CHECK(tk::regenerate_private_key(c, persist) == R::PersistFailed);
        CHECK(c.key == "OLD" && stored == "OLD" && c.loads == 1);
        fail_persist = false;
    }
    {   // the B2 defect (32-byte export buffer) fails closed before any mutation
        FakeKeyClient c;
        c.key = "OLD";
        CHECK(tk::regenerate_private_key(c, persist, 32) == R::ExportExistingFailed);
        CHECK(c.key == "OLD" && !c.created);
    }
    {   // creation failure keeps the previous key
        FakeKeyClient c;
        c.key = "OLD";
        c.fail_create = true;
        CHECK(tk::regenerate_private_key(c, persist) == R::CreateFailed);
        CHECK(c.key == "OLD");
    }
    {   // export of the new key fails: previous key restored
        FakeKeyClient c;
        c.key = "OLD";
        c.fail_export_after_create = true;
        CHECK(tk::regenerate_private_key(c, persist) == R::ExportNewFailed);
        CHECK(c.key == "OLD" && c.loads == 1);
    }
    {   // first boot (no key yet): commits the new key
        FakeKeyClient c;
        stored.clear();
        CHECK(tk::regenerate_private_key(c, persist) == R::Committed);
        CHECK(c.key == "NEW" && stored == "NEW");
    }
    {   // first boot + persistence failure: nothing to restore; caller treats it as CommitUnknown
        FakeKeyClient c;
        stored.clear();
        fail_persist = true;
        CHECK(tk::regenerate_private_key(c, persist) == R::PersistFailed);
        CHECK(c.key == "NEW" && stored.empty() && c.loads == 0);
        fail_persist = false;
    }
}

static void test_telemetry_dispatch_routing_order() {
    // H1: Responses must be routed before delivering telemetry.
    // A CarServer response with a foreign (unmatched) request UUID must NOT invoke telemetry callbacks.
    tk::CommandRunner runner;
    runner.vcsec_session().set_established({1}, 10, 1000);
    runner.info_session().set_established({2}, 20, 1000);

    tk::BleUuid our_uuid{}; our_uuid.fill(0x11);
    uint32_t cmd_id = runner.enqueue("Verify Amps", tk::BleDomain::Infotainment,
                                     tk::WakePolicy::WakeIfNeeded, 20000, 1000, our_uuid);
    CHECK(cmd_id > 0);
    runner.tick(1000, true, true, false);
    runner.notify_tx_complete(1050);

    bool callback_invoked = false;
    auto charge_state_callback = [&]() { callback_invoked = true; };

    // Simulate arrival of response with FOREIGN request UUID:
    tk::BleUuid foreign_uuid{}; foreign_uuid.fill(0xAB);
    auto outcome = runner.handle_response(tk::BleDomain::Infotainment, foreign_uuid.data(), foreign_uuid.size(),
                                          true, 100, true, "");
    CHECK(!outcome.routed);
    CHECK(outcome.drop_reason == tk::DispatchDropReason::Unmatched);

    // Because outcome was not routed to our request, callback is NOT invoked
    if (outcome.routed) {
        charge_state_callback();
    }
    CHECK(!callback_invoked);

    // Simulate arrival of response with MATCHING request UUID:
    outcome = runner.handle_response(tk::BleDomain::Infotainment, our_uuid.data(), our_uuid.size(),
                                     true, 101, true, "");
    CHECK(outcome.routed);
    CHECK(outcome.drop_reason == tk::DispatchDropReason::None);
    if (outcome.routed) {
        charge_state_callback();
    }
    CHECK(callback_invoked);
}

static void test_json_request_value_budget() {
    const auto array_of = [](unsigned items, const char* item = "0") {
        std::string body = "[";
        for (unsigned i = 0; i < items; ++i) {
            if (i) body += ',';
            body += item;
        }
        return body + "]";
    };
    const auto status = [](const std::string& body, unsigned limit = tk::kJsonMaxRequestValues) {
        return tk::json_syntax_status(body.data(), body.size(), limit);
    };
    CHECK(tk::kJsonMaxRequestValues == 64);
    for (const char* scalar : {"0", "null", "true", "\"[,]{}:\\\"\"", "{}", "[]"}) {
        CHECK(status(scalar, 1) == tk::JsonSyntaxStatus::Valid);
    }
    const std::string at_limit = array_of(63);
    const std::string above_limit = array_of(64);
    const std::string large = array_of(1023);
    CHECK(large.size() == 2047);
    CHECK(status(at_limit) == tk::JsonSyntaxStatus::Valid);
    CHECK(status(above_limit) == tk::JsonSyntaxStatus::TooManyValues);
    CHECK(status(large) == tk::JsonSyntaxStatus::TooManyValues);
    CHECK(status(large, 0) == tk::JsonSyntaxStatus::Valid);
    CHECK(status(array_of(63, "\"[0,0]\\\"{}:\"")) == tk::JsonSyntaxStatus::Valid);
    CHECK(status("{\"a\":0}", 1) == tk::JsonSyntaxStatus::TooManyValues);
    CHECK(status("{\"a\":0}", 2) == tk::JsonSyntaxStatus::Valid); // keys do not consume values
    std::string object = "{";
    for (unsigned i = 0; i < 63; ++i) {
        if (i) object += ',';
        object += "\"k" + std::to_string(i) + "\":0";
    }
    object += '}';
    CHECK(status(object) == tk::JsonSyntaxStatus::Valid);
    std::string nested(15, '[');
    nested += array_of(48); // 15 outer arrays + one inner array + 48 scalars = 64 values
    nested.append(15, ']');
    CHECK(status(nested) == tk::JsonSyntaxStatus::Valid);
    std::string prefix = above_limit.substr(0, above_limit.size() - 1) + ',';
    CHECK(status(prefix + "\"\\q\"]") == tk::JsonSyntaxStatus::Malformed);
    CHECK(status(prefix + "\"\\u0000\"]") == tk::JsonSyntaxStatus::UnsupportedNul);
    CHECK(status(prefix + std::string(17, '[') + "0" + std::string(17, ']') + "]") ==
          tk::JsonSyntaxStatus::TooDeep);
    CHECK(status(above_limit + " trailing") == tk::JsonSyntaxStatus::Malformed);
    int parser_calls = 0;
    int node = 7;
    const auto parse = [&](const char*) { ++parser_calls; return &node; };
    auto parsed = tk::json_materialize<int>(large.c_str(), large.size(), parse,
                                            tk::kJsonMaxRequestValues);
    CHECK(parsed.status == tk::JsonMaterializeStatus::TooManyValues);
    CHECK(parsed.root == nullptr && parser_calls == 0);
    parsed = tk::json_materialize<int>(large.c_str(), large.size(), parse);
    CHECK(parsed.status == tk::JsonMaterializeStatus::Ok);
    CHECK(parsed.root == &node && parser_calls == 1);
    CHECK(tk::config_submission_http_status(tk::ConfigSubmissionStatus::JsonTooManyValues) == 400);
    CHECK(tk::config_submission_http_status(tk::ConfigSubmissionStatus::DuplicateJsonKey) == 400);
}

static void test_config_commit() {
    CHECK(tk::config_health_failure(tk::RuntimeHealthFailure::None) == tk::ConfigImageResult::None);
    CHECK(tk::config_health_failure(tk::RuntimeHealthFailure::NotReady) == tk::ConfigImageResult::NotReady);
    CHECK(tk::config_health_failure(tk::RuntimeHealthFailure::HostUnsynced) == tk::ConfigImageResult::HostUnsynced);
    CHECK(tk::config_health_failure(tk::RuntimeHealthFailure::HostReset) == tk::ConfigImageResult::HostReset);
    CHECK(tk::config_health_failure(tk::RuntimeHealthFailure::CriticalHeap) == tk::ConfigImageResult::CriticalHeap);
    CHECK(tk::config_health_failure(static_cast<tk::RuntimeHealthFailure>(255)) == tk::ConfigImageResult::Unknown);

    using Image = tk::ConfigImageResult;
    const Image images[] = {Image::None, Image::Valid, Image::Undefined, Image::Confirmed,
        Image::Pending, Image::New, Image::Unsupported, Image::Unknown, Image::NotReady,
        Image::HostUnsynced, Image::HostReset, Image::CriticalHeap, Image::MarkUnknown,
        Image::WrongOwner, Image::WrongClass};
    for (unsigned i = 0; i < sizeof(images) / sizeof(images[0]); ++i) {
        const auto image = images[i];
        CHECK(tk::config_wifi_may_save(image) == (i == 1 || i == 2));
        CHECK(tk::config_image_warning(image) == (i >= 4));
        CHECK(std::strlen(tk::config_image_code(image)) > 0);
        CHECK(std::strlen(tk::config_image_code(image)) < 32);
        CHECK(std::strlen(tk::config_commit_reason(image)) < 128);
        for (unsigned j = 0; j < i; ++j)
            CHECK(std::strcmp(tk::config_image_code(image), tk::config_image_code(images[j])) != 0);
        tk::EffectReplyState state;
        state.image = image;
        CHECK_STR(state.reason(), "operation outcome unknown; reply degraded");
        state.succeeded();
        if (i >= 4) CHECK_STR(state.reason(), tk::config_commit_reason(image));
        else CHECK_STR(state.reason(), "request succeeded; reply degraded");
    }
    CHECK(sizeof(tk::EffectReplyState) <= 8);
    CHECK_STR(tk::config_image_code(Image::MarkUnknown), "mark_outcome_unknown");
    CHECK(std::strstr(tk::config_commit_reason(Image::MarkUnknown), "uncertain") != nullptr);
    CHECK(std::strstr(tk::config_commit_reason(Image::MarkUnknown), "rollback") == nullptr);
}

static void test_effect_reply() {
    tk::EffectReplyState state;
    CHECK(!state.needs_effect_reply());
    CHECK(!state.success());
    CHECK(state.take_continuation() == tk::ReplyContinuation::None);
    state = {};
    state.attempting(tk::RequestEffect::Clock);
    state.attempting(tk::RequestEffect::OtaStart);
    CHECK(state.possible_effects == (4 | 16));
    state.refused(tk::RequestEffect::Clock);
    CHECK(state.possible_effects == 16 && state.outcome == tk::RequestOutcome::Uncertain);
    state.refused(tk::RequestEffect::OtaStart);
    CHECK(state.possible_effects == 0 && state.outcome == tk::RequestOutcome::NotAttempted);
    state.attempting(tk::RequestEffect::Clock);
    state.attempting(tk::RequestEffect::OtaStart);
    CHECK_STR(state.outcome_text(), "unknown");
    CHECK(!state.success()); // a possible clock change is never OTA-start success
    state.succeeded();
    CHECK(state.success());
    CHECK_STR(state.reason(), "request succeeded; reply degraded");
    state = {};
    state.attempting(tk::RequestEffect::Identity);
    state.identity_recovery();
    CHECK(!state.success());
    CHECK(state.recovery_pending);
    CHECK_STR(state.outcome_text(), "applied_recovery_pending");
    state.continuation = tk::ReplyContinuation::IdentityRestart;
    CHECK(state.take_continuation() == tk::ReplyContinuation::IdentityRestart);
    CHECK(state.take_continuation() == tk::ReplyContinuation::None);
    state = {};
    state.attempting(tk::RequestEffect::Configuration);
    state.succeeded();
    state.continuation = tk::ReplyContinuation::SuccessfulConfigRestart;
    state.terminal = true; // failed response does not cancel the saved config's continuation
    CHECK(state.take_continuation() == tk::ReplyContinuation::SuccessfulConfigRestart);
    CHECK(state.take_continuation() == tk::ReplyContinuation::None);
}

static void test_uptime_rollovers() {
    using tk::uptime_seconds;
    CHECK(uptime_seconds(0) == 0);
    CHECK(uptime_seconds(999999) == 0);
    CHECK(uptime_seconds(1000000) == 1);
    const uint64_t ms_wrap = uint64_t{1} << 32;
    CHECK(uptime_seconds(ms_wrap * 1000) == 4294967);
    CHECK(uptime_seconds((ms_wrap + 3600000) * 1000) == 4298567);
    CHECK(uptime_seconds(uint64_t{UINT32_MAX} * 1000000) == UINT32_MAX);
    CHECK(uptime_seconds((uint64_t{UINT32_MAX} + 1) * 1000000) == UINT32_MAX);

    // The actual former 32-bit millisecond boundary must advance the production ring.
    tk::HeapRing history;
    history.record(uptime_seconds((ms_wrap - 600000) * 1000), 32000, 16000);
    history.record(uptime_seconds((ms_wrap - 300000) * 1000), 32000, 16000);
    const uint32_t before = history.bucket0() + static_cast<uint32_t>(history.count());
    history.record(uptime_seconds((ms_wrap + 3600000) * 1000), 32000, 16000);
    CHECK(history.bucket0() + history.count() > before);

    // 100-Hz RTOS ticks formerly reset now_s near 497 days. ESP timer seconds do not.
    const uint64_t tick_wrap_us = (uint64_t{1} << 32) * 10000;
    const uint32_t start = uptime_seconds(tick_wrap_us - 10000000);
    tk::WakePollState retry;
    retry.note_bootstrap(true, false, start);
    retry.note_awake();
    CHECK(retry.take_pending(start));
    retry.note_bootstrap(true, false, uptime_seconds(tick_wrap_us + 19000000));
    CHECK(!retry.armed);
    retry.note_bootstrap(true, false, uptime_seconds(tick_wrap_us + 20000000));
    CHECK(retry.armed);
    CHECK(retry.bootstrap_backoff_s == 60);

    // Pin the generic retry deadline itself across uint32 seconds wrap as well.
    tk::WakePollState wrapped;
    const uint32_t near_wrap = UINT32_MAX - 10;
    wrapped.note_bootstrap(true, false, near_wrap);
    wrapped.note_awake();
    CHECK(wrapped.take_pending(near_wrap));
    wrapped.note_bootstrap(true, false, near_wrap + 9u);
    CHECK(!wrapped.armed);
    wrapped.note_bootstrap(true, false, near_wrap + 29u);
    CHECK(!wrapped.armed);
    wrapped.note_bootstrap(true, false, near_wrap + 30u);
    CHECK(wrapped.armed);

    // Fifteen minutes of connected, awake, already-fresh state stays quiescent.
    tk::WakePollState fresh;
    unsigned extra_polls = 0;
    for (uint32_t offset = 0; offset <= 900; offset += 5) {
        fresh.note_bootstrap(true, true, start + offset);
        fresh.note_awake();
        if (fresh.take_pending(start + offset)) ++extra_polls;
    }
    CHECK(extra_polls == 0);
    CHECK(!fresh.armed && !fresh.pending);

    CHECK(tk::heap_timeline_fits(UINT32_MAX - 10, 10));
    CHECK(!tk::heap_timeline_fits(UINT32_MAX - 9, 10));
    tk::HeapRing carry;
    carry.open_bucket_ = UINT32_MAX / tk::kHeapHistoryDtS - 1;
    CHECK(tk::heap_carry_fits(carry));
    CHECK(tk::heap_persist_next_carry(carry) == (UINT32_MAX / tk::kHeapHistoryDtS) * tk::kHeapHistoryDtS);
    carry.open_bucket_ += 1;
    CHECK(!tk::heap_carry_fits(carry));
}

static void test_coredump_summary() {
    const std::string hash(64, 'a');
    const tk::CoredumpAbi xtensa{true, 100, 52, 16, 112, 4, 16, 12, 152};
    const tk::CoredumpAbi riscv{false, 96, 52, 16, 148, 0, 8, 0, 8};
    uint32_t normalized = 0;
    for (uint32_t pc : {0U, 1U, 2U}) CHECK(!tk::dump_normalize_xtensa_pc(pc, normalized));
    CHECK(tk::dump_normalize_xtensa_pc(0x80002003, normalized) && normalized == 0x40002000);
    CHECK(tk::dump_normalize_xtensa_pc(0x40001000, normalized) && normalized == 0x40000ffd);
    unsigned last_reads = 0, summary_reads = 0, last_exec = 0;
    auto parse = [&](const std::vector<uint8_t>& image, const tk::CoredumpAbi& abi,
                     unsigned fail_at = 0, const char* running = nullptr, bool checksum = true) {
        unsigned calls = 0;
        auto read = [&](uint32_t offset, void* destination, uint32_t length) {
            ++calls;
            const bool safe = offset <= image.size() && length <= image.size() - offset && length <= 256;
            CHECK(safe);
            if (!safe || calls == fail_at) return false;
            std::memcpy(destination, image.data() + offset, length);
            return true;
        };
        tk::CoredumpInput<decltype(read)> input(read, static_cast<uint32_t>(image.size()));
        const auto outer = input.admit();
        const auto metadata = tk::parse_coredump_metadata(input, outer, abi.xtensa ? 94 : 243,
                                                          tk::dump_u32(image.data() + 4),
                                                          running ? running : hash.c_str(),
                                                          running ? std::strlen(running) : hash.size(), checksum);
        const unsigned before = calls;
        last_exec = 0;
        const auto result = tk::parse_coredump_summary(input, metadata, abi, [&](uint32_t pc) {
            ++last_exec;
            return (pc >= 0x40000000 && pc < 0x44000000);
        });
        if (!result.available) {
            CHECK(result.pc == 0 && result.depth == 0 && !result.corrupted && !result.truncated && !result.isr);
            CHECK(std::all_of(std::begin(result.task), std::end(result.task), [](char c) { return c == 0; }));
            CHECK(std::all_of(std::begin(result.backtrace), std::end(result.backtrace), [](uint32_t pc) { return pc == 0; }));
        }
        if (fail_at && calls >= fail_at) {
            CHECK(!result.available && result.task[0] == 0 && result.pc == 0 && result.depth == 0);
            uint8_t byte;
            CHECK(input.failed() && !input.read(0, &byte, 1) && calls == fail_at);
        }
        CHECK(input.reads() <= 2048 && input.bytes_read() <= 2 * image.size() + 4096);
        summary_reads = calls - before;
        last_reads = calls;
        return result;
    };
    const auto original = dump_test_summary_image(true, 0x00090104, 2);
    auto summary = parse(original, xtensa);
    CHECK(summary.available && summary.depth == 2 && !summary.corrupted && !summary.truncated && !summary.isr);
    CHECK_STR(summary.task, "vehicle_loop");
    CHECK(summary.pc == 0x40001003 && summary.backtrace[0] == 0x40001000 && summary.backtrace[1] == 0x40002000);
    const unsigned normal_reads = last_reads;
    for (unsigned fail = 1; fail <= normal_reads; ++fail) CHECK(!parse(original, xtensa, fail).available);
    CHECK(parse(dump_test_summary_image(true, 0x00000104), xtensa).available);
    for (uint32_t version : {0x00050104U, 0x000d0104U}) {
        const auto image = dump_test_summary_image(false, version);
        summary = parse(image, riscv);
        CHECK(summary.available && summary.pc == 0x42001000 && summary.depth == 0 && !summary.corrupted);
        CHECK(last_exec == 1);
        const unsigned minimal_reads = summary_reads;
        auto changed = image;
        const auto stack = 12 + tk::dump_u32(image.data() + 12 + 52 + 2 * 32 + 4);
        std::fill(changed.begin() + stack + 4, changed.begin() + stack + 148, 0xff);
        CHECK(parse(changed, riscv).available && summary_reads == minimal_reads);  // no SP/unwind/stack copy
        dump_test_u32(changed, stack, 1);
        summary = parse(changed, riscv);
        CHECK(summary.available && summary.pc == 1 && summary.corrupted && summary.depth == 0);
    }
    for (unsigned depth : {1U, 2U, 15U, 16U}) {
        summary = parse(dump_test_summary_image(true, 0x00090104, depth), xtensa);
        CHECK(summary.available && summary.depth == depth && !summary.corrupted && !summary.truncated);
    }
    summary = parse(dump_test_summary_image(true, 0x00090104, 16, true), xtensa);
    CHECK(summary.available && summary.depth == 16 && summary.truncated && !summary.corrupted);
    CHECK(summary.backtrace[15] == 0x40002000 + 14 * 16);
    CHECK(!parse(original, xtensa, 0, "aaaaaaaaa").available && summary_reads == 0 && last_exec == 0);
    CHECK(!parse(original, xtensa, 0, std::string(64, 'b').c_str()).available && summary_reads == 0);
    CHECK(!parse(original, xtensa, 0, nullptr, false).available && summary_reads == 0);

    const size_t tcb_ph = 12 + 52 + 32, stack_ph = tcb_ph + 32;
    const size_t tcb = 12 + tk::dump_u32(original.data() + tcb_ph + 4);
    const size_t stack = 12 + tk::dump_u32(original.data() + stack_ph + 4);
    // INFO note is104bytes; EXTRA descriptor follows its12byte header and16byte name.
    const size_t extra = 12 + tk::dump_u32(original.data() + 12 + 52 + 4) + 104 + 12 + 16;
    for (uint32_t marker : {0U, 0xffffffffU, 0xdeadbeefU, 0x3ffb0001U}) {
        auto changed = original; dump_test_u32(changed, extra, marker);
        CHECK(!parse(changed, xtensa).available);
    }
    auto changed = original;
    dump_test_u32(changed, extra + 148, 1);
    CHECK(parse(changed, xtensa).isr);
    dump_test_u32(changed, extra + 148, 2);
    CHECK(!parse(changed, xtensa).available);
    for (uint32_t address : {0x20000000U, 0x2ffffffcU, 0x3ffb0000U, 0x3ffb0040U, 0xfffffff0U}) {
        changed = original; dump_test_u32(changed, stack_ph + 8, address);
        CHECK(!parse(changed, xtensa).available);
    }
    changed = dump_test_summary_image(false, 0x00050104);
    dump_test_u32(changed, stack_ph + 8, 0x70000000);
    CHECK(!parse(changed, riscv).available);
    for (uint32_t pc : {0U, 1U, 2U, 3U, 0x30000000U}) {
        changed = original; dump_test_u32(changed, stack + 4, pc);
        summary = parse(changed, xtensa);
        CHECK(summary.available && summary.corrupted && summary.pc == pc && summary.depth == 0);
    }
    for (uint32_t sp : {0U, 0x3ffb1000U, 0x3ffb100cU, 0x3ffb107fU, 0x3ffb1410U, 0xfffffff0U}) {
        changed = original; dump_test_u32(changed, stack + 16, sp);
        summary = parse(changed, xtensa);
        CHECK(summary.available && summary.corrupted && summary.depth == 1);
    }
    changed = original; dump_test_u32(changed, stack + 112, 0x80003003);
    for (uint32_t sp : {0x3ffb1080U, 0x3ffb1070U, 0x3ffb1410U}) {
        dump_test_u32(changed, stack + 116, sp);
        summary = parse(changed, xtensa);
        CHECK(summary.available && summary.corrupted && summary.depth == 2);
    }
    changed = original;  // unused caller SP may be garbage when saved A0 terminates the chain
    dump_test_u32(changed, stack + 116, UINT32_MAX);
    CHECK(!parse(changed, xtensa).corrupted);
    changed = original;
    std::fill(changed.begin() + tcb + 52, changed.begin() + tcb + 68, 'x');
    changed[tcb + 53] = '\n'; changed[tcb + 54] = 0xff;
    summary = parse(changed, xtensa);
    CHECK(summary.available && std::strlen(summary.task) == 16 && summary.task[1] == '?' && summary.task[2] == '?');
    for (uint32_t address : {0x3ffb0000U, 0x3ffb0040U, 0x3ffb1080U}) {
        changed = dump_test_summary_image(true, 0x00090104, 2, false, 1);
        dump_test_u32(changed, stack_ph + 32 + 8, address);
        CHECK(!parse(changed, xtensa).available);
    }
    changed = dump_test_summary_image(true, 0x00090104, 2, false, 1);
    CHECK(parse(changed, xtensa).available);  // unrelated nonoverlapping load remains valid
    std::vector<tk::CoredumpAbi> invalid(10, xtensa);
    invalid[0].tcb_bytes = 0; invalid[1].tcb_bytes = 257;
    invalid[2].name_bytes = 0; invalid[3].name_bytes = 17;
    invalid[4].name_offset = UINT32_MAX; invalid[5].frame_bytes = 257;
    invalid[6].pc_offset = 109; invalid[7].sp_offset = UINT32_MAX;
    invalid[8].a0_offset = 112; invalid[9].extra_bytes = 151;
    for (const auto& abi : invalid) CHECK(!parse(original, abi).available && summary_reads == 0);
    auto too_large = riscv;
    too_large.frame_bytes = 152;
    CHECK(!parse(dump_test_summary_image(false, 0x00050104), too_large).available);
    too_large = xtensa;
    too_large.tcb_bytes = 104;
    CHECK(!parse(original, too_large).available);
    changed = original;
    dump_test_u32(changed, tcb_ph + 8, 0x3ffc0000);  // no matching crashed TCB
    CHECK(!parse(changed, xtensa).available);
    changed = original;
    dump_test_u32(changed, stack_ph, 4);             // adjacency is not an arbitrary note
    CHECK(!parse(changed, xtensa).available);
}

static void test_coredump_image() {
    // Serialized little-endian words may begin at any byte alignment. Check all byte values
    // in each position against the shift-based format definition, independent of host endian.
    uint8_t encoded[8]{};
    for (size_t offset = 0; offset < 4; ++offset)
        for (unsigned position = 0; position < 4; ++position)
            for (unsigned value = 0; value < 256; ++value) {
                std::memset(encoded, 0, sizeof(encoded));
                encoded[offset + position] = static_cast<uint8_t>(value);
                CHECK(tk::dump_u32(encoded + offset) == (uint32_t{value} << (position * 8)));
                if (position < 2)
                    CHECK(tk::dump_u16(encoded + offset) == (uint32_t{value} << (position * 8)));
            }

    // Wide-arithmetic oracle covers endpoint overflow, adjacent/empty ranges and symmetry.
    const uint32_t boundaries[] = {0, 1, 3, 0x10000000U, 0x70000000U, 0xfffffffeU, UINT32_MAX};
    for (uint32_t a : boundaries) for (uint32_t b : boundaries)
        for (uint32_t na : boundaries) for (uint32_t nb : boundaries) {
            const tk::DumpProgramHeader left{1, 0, a, na, na}, right{1, 0, b, nb, nb};
            const bool expected = uint64_t{a} < uint64_t{b} + nb &&
                                  uint64_t{b} < uint64_t{a} + na;
            CHECK(tk::dump_virtual_overlap(left, right) == expected);
        }

    CHECK(tk::coredump_request_mode("") == tk::CoredumpRequestMode::Current);
    CHECK(tk::coredump_request_mode("raw=1") == tk::CoredumpRequestMode::Raw);
    CHECK(tk::coredump_request_mode("clear=1") == tk::CoredumpRequestMode::Clear);
    for (const char* query : {"raw=1&clear=1", "clear=1&raw=1", "raw=0", "clear=0", "raw=1&raw=1",
                              "%72aw=1", "CLEAR=1", "clear=01", "x=1", "raw=1&"})
        CHECK(tk::coredump_request_mode(query) == tk::CoredumpRequestMode::Invalid);
    test_coredump_summary();
    using tk::DumpIdentity;
    const std::string hash(64, 'a');
    CHECK(tk::classify_dump_identity(hash.data(), 64, hash.data(), 64, true) == DumpIdentity::VerifiedCurrent);
    CHECK(tk::classify_dump_identity(hash.data(), 64, hash.data(), 64, false) == DumpIdentity::Unverified);
    CHECK(tk::classify_dump_identity(hash.data(), 64, "AAAAAAAAA", 9, true) == DumpIdentity::Unverified);
    CHECK(tk::classify_dump_identity(hash.data(), 64, "bbbbbbbb", 8, true) == DumpIdentity::Foreign);
    CHECK(tk::classify_dump_identity(hash.data(), 64, "bbbbbbb", 7, true) == DumpIdentity::Unverified);
    CHECK(tk::classify_dump_identity(hash.data(), 64, "bbbbbbbg", 8, true) == DumpIdentity::Unverified);
    CHECK(tk::classify_dump_identity(nullptr, 64, hash.data(), 64, true) == DumpIdentity::Unverified);
    CHECK(tk::classify_dump_identity(hash.data(), 65, hash.data(), 64, true) == DumpIdentity::Unverified);
    uint32_t aligned = 0;
    CHECK(tk::dump_align4(UINT32_MAX - 3, aligned) && aligned == UINT32_MAX - 3);
    CHECK(!tk::dump_align4(UINT32_MAX - 2, aligned));
    CHECK(!tk::dump_range(UINT32_MAX, 1, UINT32_MAX));

    std::vector<uint8_t> info(72, 0);
    dump_test_u32(info, 0, 0x00090104);
    std::memcpy(info.data() + 4, hash.data(), hash.size());
    const auto info_note = dump_test_note("ESP_CORE_DUMP_INFO", 8266, info);
    const auto original = dump_test_image(info_note);
    unsigned last_reads = 0;
    auto parse = [&](const std::vector<uint8_t>& image, bool checksum = true,
                     unsigned fail_at = 0) {
        unsigned calls = 0;
        auto read = [&](uint32_t offset, void* destination, uint32_t length) {
            ++calls;
            const bool in_range = offset <= image.size() && length <= image.size() - offset;
            CHECK(in_range);
            CHECK(length <= 256);
            if (!in_range || length > 256) return false;
            if (calls == fail_at) return false;
            std::memcpy(destination, image.data() + offset, length);
            return true;
        };
        tk::CoredumpInput<decltype(read)> input(read, static_cast<uint32_t>(image.size()));
        const auto outer = input.admit();
        auto result = tk::parse_coredump_metadata(input, outer, 94, 0x00090104,
                                                 hash.data(), hash.size(), checksum);
        CHECK(input.reads() <= 2048);
        CHECK(input.bytes_read() <= 2 * image.size() + 4096);
        if (!result.valid_elf) {
            CHECK(result.identity == DumpIdentity::Unverified && result.headers == 0);
            CHECK(result.extra_offset == 0 && result.extra_bytes == 0);
            CHECK(std::all_of(std::begin(result.hash), std::end(result.hash), [](char c) { return c == 0; }));
        }
        if (fail_at && calls >= fail_at) {
            uint8_t byte;
            CHECK(!input.read(0, &byte, 1));
            CHECK(calls == fail_at);
        }
        last_reads = calls;
        return result;
    };
    auto parsed = parse(original);
    CHECK(parsed.valid_elf && parsed.identity == DumpIdentity::VerifiedCurrent);
    CHECK_STR(parsed.hash, hash.c_str());
    const unsigned normal_reads = last_reads;
    CHECK(!parse(original, false).valid_elf);
    CHECK(last_reads == 1);  // invalid integrity never admits note/task/frame interpretation
    for (unsigned fail = 1; fail <= normal_reads; ++fail) CHECK(!parse(original, true, fail).valid_elf);

    // Every truncation is a failed bounded admission, without an out-of-range callback.
    for (size_t length = 0; length < original.size(); ++length) {
        std::vector<uint8_t> shortened(original.begin(), original.begin() + length);
        CHECK(!parse(shortened).valid_elf);
    }
    auto changed = original;
    // This pinned single-header fixture has no variable header byte that may change
    // independently. Exercise every bit, including all reserved bytes and both count bytes.
    for (size_t byte = 0; byte < 52; ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            changed = original;
            changed[12 + byte] ^= static_cast<uint8_t>(1U << bit);
            CHECK(!parse(changed).valid_elf);
        }
    }
    for (const auto& edit : std::vector<std::pair<size_t, uint32_t>>{
            {0, 4}, {0, 96}, {0, UINT32_MAX}, {0, tk::kCoredumpCapacity + 32},
            {4, 0x00050104}, {12 + 20, 0}, {12 + 28, UINT32_MAX},
            {12 + 32, 1}, {12 + 36, 1}, {12 + 40, 0}, {12 + 42, 31},
            {12 + 44, 0xffff}, {12 + 48, 1},
            {12 + 52, 99}, {12 + 52 + 4, UINT32_MAX},
            {12 + 52 + 16, UINT32_MAX}, {12 + 52 + 20, 1},
            {12 + 84, UINT32_MAX}, {12 + 84 + 4, UINT32_MAX},
            {12 + 84 + 8, 1}}) {
        changed = original;
        dump_test_u32(changed, edit.first, edit.second);
        CHECK(!parse(changed).valid_elf);
    }
    for (size_t at : {size_t{12}, size_t{12 + 4}, size_t{12 + 5}, size_t{12 + 6},
                      size_t{12 + 7}, size_t{12 + 8}, size_t{12 + 18}}) {
        changed = original;
        changed[at] ^= 0xff;
        CHECK(!parse(changed).valid_elf);
    }

    auto short_info = info;
    std::fill(short_info.begin() + 4, short_info.end(), 0);
    std::memcpy(short_info.data() + 4, "aaaaaaaaa", 9);
    parsed = parse(dump_test_image(dump_test_note("ESP_CORE_DUMP_INFO", 8266, short_info)));
    CHECK(parsed.valid_elf && parsed.identity == DumpIdentity::Unverified);
    short_info[4] = 'b';
    CHECK(parse(dump_test_image(dump_test_note("ESP_CORE_DUMP_INFO", 8266, short_info))).identity == DumpIdentity::Foreign);
    short_info[4] = 'g';
    parsed = parse(dump_test_image(dump_test_note("ESP_CORE_DUMP_INFO", 8266, short_info)));
    CHECK(parsed.valid_elf && parsed.identity == DumpIdentity::Unverified && parsed.hash[0] == 0);
    std::fill(short_info.begin() + 4, short_info.end(), 'a');  // no terminator in all66field bytes
    CHECK(parse(dump_test_image(dump_test_note("ESP_CORE_DUMP_INFO", 8266, short_info))).identity == DumpIdentity::Unverified);
    CHECK(!parse(dump_test_image(dump_test_note("WRONG_NAME", 8266, info))).valid_elf);
    info.resize(71);
    CHECK(!parse(dump_test_image(dump_test_note("ESP_CORE_DUMP_INFO", 8266, info))).valid_elf);

    auto notes = info_note;
    notes.insert(notes.end(), info_note.begin(), info_note.end());
    CHECK(!parse(dump_test_image(notes)).valid_elf);
    notes = info_note;
    const auto extra = dump_test_note("ESP_EXTRA_INFO", 677, std::vector<uint8_t>(152));
    notes.insert(notes.end(), extra.begin(), extra.end());
    parsed = parse(dump_test_image(notes));
    CHECK(parsed.valid_elf && parsed.extra_bytes == 152 && parsed.extra_offset != 0);
    notes.insert(notes.end(), extra.begin(), extra.end());
    CHECK(!parse(dump_test_image(notes)).valid_elf);

    notes = info_note;
    const auto unknown = dump_test_note("CORE", 1, {});
    for (unsigned i = 1; i < 256; ++i) notes.insert(notes.end(), unknown.begin(), unknown.end());
    CHECK(parse(dump_test_image(notes)).valid_elf);
    notes.insert(notes.end(), unknown.begin(), unknown.end());
    CHECK(!parse(dump_test_image(notes)).valid_elf);
    CHECK(parse(dump_test_image(info_note, 256)).valid_elf);
    CHECK(!parse(dump_test_image(info_note, 257)).valid_elf);
    changed = dump_test_image(info_note, 2);
    dump_test_u32(changed, 12 + 52 + 32, 99);  // valid INFO, then an invalid later PH
    parsed = parse(changed);
    CHECK(!parsed.valid_elf && parsed.identity == DumpIdentity::Unverified);
    CHECK(parsed.headers == 0 && parsed.hash[0] == 0);
    CHECK(parsed.extra_offset == 0 && parsed.extra_bytes == 0);

    unsigned calls = 0;
    auto zero_read = [&](uint32_t, void* destination, uint32_t length) {
        ++calls; std::memset(destination, 0, length); return true;
    };
    tk::CoredumpInput<decltype(zero_read)> read_budget(zero_read, tk::kCoredumpCapacity);
    uint8_t buffer[256]{};
    for (unsigned i = 0; i < 2048; ++i) CHECK(read_budget.read(0, buffer, 1));
    CHECK(!read_budget.read(0, buffer, 1) && calls == 2048);
    calls = 0;
    tk::CoredumpInput<decltype(zero_read)> byte_budget(zero_read, 96);
    for (unsigned i = 0; i < 44; ++i) CHECK(byte_budget.read(0, buffer, 96));
    CHECK(byte_budget.read(0, buffer, 64));
    CHECK(byte_budget.bytes_read() == 2 * 96 + 4096 && calls == 45);
    CHECK(!byte_budget.read(0, buffer, 1) && calls == 45);
    const unsigned before = calls;
    CHECK(!byte_budget.read(0, buffer, 1) && calls == before);
    calls = 0;
    tk::CoredumpInput<decltype(zero_read)> wide_read(zero_read, tk::kCoredumpCapacity);
    CHECK(!wide_read.read(0, buffer, 257) && calls == 0);

    calls = 0;
    auto original_read = [&](uint32_t offset, void* destination, uint32_t length) {
        ++calls;
        const bool in_range = offset <= original.size() && length <= original.size() - offset;
        CHECK(in_range);
        if (!in_range) return false;
        std::memcpy(destination, original.data() + offset, length);
        return true;
    };
    tk::CoredumpInput<decltype(original_read)> payload_boundary(original_read, original.size());
    const auto admitted = payload_boundary.admit();
    CHECK(admitted.readable && calls == 1);
    CHECK(!payload_boundary.payload(admitted.length - 12 - 32, buffer, 1));
    CHECK(payload_boundary.failed() && calls == 1);
    CHECK(!payload_boundary.read(0, buffer, 1) && calls == 1);
    calls = 0;
    tk::CoredumpInput<decltype(original_read)> admission_once(original_read, original.size());
    CHECK(admission_once.admit().readable);
    CHECK(!admission_once.admit().readable && admission_once.failed() && calls == 1);
}

static void test_iteration_fault() {
    tk::IterationFaultLog state{};
    auto report = tk::iteration_fault_note(state, 0);
    CHECK(report.emit && report.suppressed == 0);
    CHECK(!tk::iteration_fault_note(state, 0).emit);
    CHECK(!tk::iteration_fault_note(state, 29999).emit);
    report = tk::iteration_fault_note(state, 30000);
    CHECK(report.emit && report.suppressed == 2);
    CHECK(state.suppressed == 0);
    CHECK(tk::iteration_fault_note(state, 90000).emit); // a quiet interval has no replay burst

    state.suppressed = UINT32_MAX;
    CHECK(!tk::iteration_fault_note(state, 90001).emit);
    CHECK(state.suppressed == UINT32_MAX);
    report = tk::iteration_fault_note(state, 120000);
    CHECK(report.emit && report.suppressed == UINT32_MAX);
    tk::IterationFaultLog independent{};
    CHECK(tk::iteration_fault_note(independent, 120001).emit);
    CHECK(!tk::iteration_fault_note(state, 120001).emit);

    constexpr uint64_t wide_clock = uint64_t{1} << 40;
    CHECK(tk::iteration_fault_note(state, wide_clock).emit);
    CHECK(!tk::iteration_fault_note(state, wide_clock + 29999).emit);
    CHECK(tk::iteration_fault_note(state, wide_clock + 30000).emit);

    // Fifteen minutes at the actual fault cadences; successful intervening iterations must not
    // make alternating errors produce a fresh first-error report on every cycle.
    for (const uint64_t step : {50, 200}) {
        for (const bool alternating : {false, true}) {
            tk::IterationFaultLog stream{};
            unsigned emitted = 0;
            for (uint64_t time = 0; time <= 900000; time += step) {
                if (alternating && (time / step) % 2 != 0) continue;
                const auto current = tk::iteration_fault_note(stream, time);
                CHECK(current.emit == (time % 30000 == 0));
                if (current.emit) {
                    CHECK(current.suppressed == (time == 0 ? 0 : 30000 / (step * (alternating ? 2 : 1)) - 1));
                    ++emitted;
                }
            }
            CHECK(emitted == 31);
        }
    }
}

// Combined long healthy interval, effective heap budget and typed boot causes.
static void test_heap_supervision() {
    using namespace tk;
    CHECK(kBootHealthyS == 1800);
    CHECK(kRecoverySampleMs == 30000);
    CHECK(kRecoveryFreshMs == 60000);
    CHECK(kRecoveryRetryMs == 300000);
    CHECK(vehicle_iteration_successful(false, false));
    CHECK(!vehicle_iteration_successful(true, false));
    CHECK(!vehicle_iteration_successful(false, true));
    CHECK(!vehicle_iteration_successful(true, true));
    RecoveryHealthyRun run{};
    CHECK(!run.sample(0, true));
    for (uint64_t now = 30000; now <= 1770000; now += 30000) CHECK(!run.sample(now, true));
    CHECK(!run.sample(1799999, true));
    CHECK(run.sample(1800000, true));
    CHECK(run.sample(1830000, true));
    CHECK(!run.sample(1860000, false));
    CHECK(!run.sample(1890000, true));
    CHECK(!run.sample(3690000, true));  // a final good sample cannot credit an unobserved stall
    CHECK(!run.sample(0, true));       // a backward clock restarts the evidence window
    const uint64_t wide = uint64_t{1} << 40;
    run = {};
    for (uint64_t elapsed = 0; elapsed < 1800000; elapsed += 30000)
        CHECK(!run.sample(wide + elapsed, true));
    CHECK(run.sample(wide + 1800000, true));
    RecoveryHealth healthy{true, true, true, true, true, false, 4096};
    CHECK(recovery_health_eligible(healthy));
    for (unsigned missing = 0; missing != 7; ++missing) {
        auto bad = healthy;
        switch (missing) {
            case 0: bad.ready = false; break;
            case 1: bad.host_synced = false; break;
            case 2: bad.host_reset_free = false; break;
            case 3: bad.vehicle_progress_fresh = false; break;
            case 4: bad.network_up = false; break;
            case 5: bad.ota_busy = true; break;
            default: bad.largest_internal = 4095; break;
        }
        CHECK(!recovery_health_eligible(bad));
        RecoveryHealthyRun interrupted{};
        for (uint64_t now = 0; now <= 1770000; now += 30000) CHECK(!interrupted.sample(now, true));
        CHECK(!interrupted.sample(1800000, recovery_health_eligible(bad)));
        CHECK(!interrupted.sample(1830000, true));
    }
    VehicleProgress progress;
    CHECK(!progress.fresh(0));
    progress.publish(0);
    CHECK(progress.fresh(0));
    CHECK(progress.fresh(60000));
    CHECK(!progress.fresh(60001));
    progress.publish(wide);
    CHECK(!progress.fresh(wide - 1));
    CHECK(progress.fresh(wide + 60000));
    CHECK(!progress.fresh(wide + 60001));
    for (uint8_t count : {uint8_t{0}, uint8_t{4}, uint8_t{5}}) {
        auto budget = HeapRecoveryBudget::seed(count, true);
        CHECK(budget.effective == count && budget.trusted);
        CHECK(budget.may_restart() == (count < 5));
        CHECK(!budget.decay_after_healthy_run(false));
        CHECK(budget.effective == count);
        CHECK(budget.decay_after_healthy_run(true) == (count != 0));
        CHECK(budget.effective == 0 && budget.may_restart());
    }
    for (uint8_t count : {uint8_t{0}, uint8_t{5}, uint8_t{255}}) {
        auto unknown = HeapRecoveryBudget::seed(count, false);
        CHECK(unknown.effective == 5 && !unknown.trusted && !unknown.may_restart());
        CHECK(!unknown.decay_after_healthy_run(true));
        CHECK(unknown.effective == 5);
    }
    CHECK(!HeapRecoveryBudget::seed(6, true).trusted);
    CHECK(boot_counter_cause(3, RebootReasonState::Present) == BootCounterCause::HeapRecovery);
    CHECK(boot_fail_next(4, BootCounterCause::HeapRecovery) == 4);
    CHECK(boot_fail_next(0, BootCounterCause::HeapRecovery) == 0);
    for (int code = -1; code <= 16; ++code) {
        for (auto reason : {RebootReasonState::Missing, RebootReasonState::Present, RebootReasonState::Error}) {
            const auto cause = boot_counter_cause(code, reason);
            CHECK(boot_healthy_clear_allowed(cause, reason) ==
                  (reason != RebootReasonState::Error && cause != BootCounterCause::Uncertain));
            if (reset_is_fault(code)) {
                CHECK(cause == BootCounterCause::Fault);
                CHECK(boot_fail_next(3, cause) == 4);
                CHECK(!boot_activity_seed_allowed(code, reason));
            }
            if (reason != RebootReasonState::Missing) CHECK(!boot_activity_seed_allowed(code, reason));
            if (cause == BootCounterCause::Uncertain) {
                CHECK(boot_fail_next(4, cause) == 4);
                CHECK(!boot_activity_seed_allowed(code, reason));
            }
        }
    }
    for (int code : {1, 2, 3, 8, 10, 11, 12}) {
        CHECK(boot_activity_seed_allowed(code, RebootReasonState::Missing));
        CHECK(boot_fail_next(4, boot_counter_cause(code, RebootReasonState::Missing)) == 0);
    }
    for (uint64_t lifetime : {uint64_t{60000}, uint64_t{120000}}) {
        unsigned failures = 0;
        for (unsigned boot = 1; boot <= 4; ++boot) {
            failures = boot_fail_next(failures, BootCounterCause::Fault);
            RecoveryHealthyRun current{};
            for (uint64_t now = 0; now <= lifetime; now += 30000) CHECK(!current.sample(now, true));
            CHECK(failures == boot);
            CHECK(boot_safe_mode(failures) == (boot == 4));
        }
    }
    RecoveryWriteCadence writes{};
    CHECK(writes.due(0)); writes.attempt(0);
    CHECK(!writes.due(299999)); CHECK(writes.due(300000));
    writes.attempt(wide); CHECK(!writes.due(wide - 1)); CHECK(writes.due(wide + 300000));
    RecoveryDiagnostic logs{};
    uint32_t suppressed = 42;
    CHECK(logs.emit(0, suppressed) && suppressed == 0);
    CHECK(!logs.emit(1, suppressed));
    CHECK(!logs.emit(299999, suppressed));
    CHECK(logs.emit(300000, suppressed) && suppressed == 2);
    CHECK(!logs.emit(300001, suppressed));
    CHECK(logs.emit(600000, suppressed) && suppressed == 1);
    logs.suppressed = UINT32_MAX;
    CHECK(!logs.emit(600001, suppressed));
    CHECK(logs.suppressed == UINT32_MAX);
    CHECK(logs.emit(900000, suppressed) && suppressed == UINT32_MAX);
}

static void test_shared_unhealthy_restart_ledger() {
    using namespace tk;
    CHECK(kBootFailThreshold == 4 && kUnhealthyRestartLimit == 5 && kBootHealthyS == 1800);
    for (unsigned stored = 0; stored <= 101; ++stored) {
        for (unsigned n = 0; n <= 6; ++n) {
            const unsigned bounded = stored < 100 ? stored : 100;
            const unsigned legacy_floor = n <= 5 && n > bounded ? n : bounded;
            CHECK(boot_shared_count(stored, BootCounterCause::Clean, RebootReasonState::Missing, n) == 0);
            CHECK(boot_shared_count(stored, BootCounterCause::HeapRecovery, RebootReasonState::Present, n) == legacy_floor);
            CHECK(boot_shared_count(stored, BootCounterCause::Fault, RebootReasonState::Present, n) ==
                  (legacy_floor < 100 ? legacy_floor + 1 : 100));
            CHECK(boot_shared_count(stored, BootCounterCause::Fault, RebootReasonState::Error, n) ==
                  (bounded < 100 ? bounded + 1 : 100));
            CHECK(boot_shared_count(stored, BootCounterCause::Uncertain, RebootReasonState::Missing, n) == bounded);
        }
    }
    for (unsigned observed = 0; observed <= 101; ++observed)
        for (unsigned confirmed = 0; confirmed <= 101; ++confirmed) {
            unsigned next = 199;
            const bool allowed = boot_heap_reservation_next(observed, confirmed, next);
            CHECK(allowed == (observed < 5 && confirmed < 5));
            CHECK(next == (allowed ? (observed > confirmed ? observed : confirmed) + 1 : 199));
        }
    // Literal mixed sequence, preserving reserved H across SW admission and charging F once.
    unsigned b = 0, next = 0;
    CHECK(boot_heap_reservation_next(b, b, next) && next == 1); b = next;
    CHECK(boot_shared_count(b, BootCounterCause::HeapRecovery, RebootReasonState::Present, 1) == 1);
    b = boot_shared_count(b, BootCounterCause::Fault, RebootReasonState::Missing, 0); CHECK(b == 2);
    CHECK(boot_heap_reservation_next(b, b, next) && next == 3); b = next;
    b = boot_shared_count(b, BootCounterCause::Fault, RebootReasonState::Missing, 0); CHECK(b == 4 && boot_safe_mode(b));
    CHECK(boot_heap_reservation_next(b, b, next) && next == 5); b = next;
    CHECK(!boot_heap_reservation_next(b, b, next) && next == 5);
}

static void test_stack_context_logic() {
    namespace sc = tk::stack_sample;
    CHECK(tk::kStackContextCount == 27 && sizeof(sc::Snapshot) == 24);
    CHECK(sc::kQuantumBytes == 256 && sc::kSaturationBytes == 15872);
    CHECK(!sc::decode(0).sampled && !sc::decode(64).sampled);
    CHECK(sc::encode(0) == 1 && sc::encode(255) == 1 && sc::encode(256) == 2);
    CHECK(sc::decode(63).lower_bound_bytes == 15872 && sc::decode(63).saturated);
    sc::Snapshot bank{};
    for (unsigned slot = 0; slot != tk::kStackContextCount; ++slot) {
        CHECK(!sc::read(bank, slot).sampled);
        bank[slot / 5] = sc::merge(bank[slot / 5], slot % 5, slot * 256 + 255);
        CHECK(sc::read(bank, slot).sampled && sc::read(bank, slot).lower_bound_bytes == slot * 256);
        char key[tk::kStackMetricKeyBytes]{};
        CHECK(tk::stack_context_metric_key(static_cast<tk::StackContext>(slot), key));
        CHECK(std::string(key) == std::string(tk::stack_context_name(static_cast<tk::StackContext>(slot))) + "_stack_min_free_bytes");
    }
    CHECK(!sc::read(bank, sc::kCapacity).sampled && sc::any_sample(bank));
    char refused[tk::kStackMetricKeyBytes] = "keep";
    CHECK(!tk::stack_context_metric_key(tk::StackContext::Count, refused));
    CHECK(std::string(refused) == "keep" && tk::stack_context_name(tk::StackContext::Count) == nullptr);
}

static void test_ota_lifetime() {
    using namespace tk::ota;
    Budget budget;
    CHECK(budget.begin(100, 16384, kOperationTimePolicy));
    CHECK(budget.deadline() == 26100);
    CHECK(!budget.begin(200, 32768, kOperationTimePolicy));
    CHECK(budget.live(101));
    const auto first_deadline = budget.deadline();
    CHECK(budget.tighten(8192));
    CHECK(budget.deadline() == 25600 && budget.deadline() < first_deadline);
    CHECK(!budget.tighten(0) && !budget.tighten(16385));
    CHECK(budget.live(25599));
    CHECK(!budget.live(25600) && !budget.live(200));
    Budget backwards;
    CHECK(backwards.begin(100, 16384, kOperationTimePolicy));
    CHECK(backwards.live(120) && !backwards.live(119) && !backwards.live(121));
    Budget overflow;
    CHECK(!overflow.begin(UINT64_MAX - 1, 1, kOperationTimePolicy));
    Budget invalid;
    CHECK(!invalid.begin(0, 1, {15000,10000,0}));
    CriticalEpisode episode;
    CHECK(episode.excused(100,4095,true,20));
    CHECK(episode.excused(119,4095,true,20));
    CHECK(!episode.excused(120,4095,true,20));
    CHECK(!episode.excused(119,4095,true,20)); // Cannot reopen expiry through a clock regression.
    CHECK(!episode.excused(121,4095,false,900));
    CHECK(!episode.excused(122,4095,true,900)); // Neither a gap nor new grace renews the episode.
    CHECK(!episode.excused(123,4096,true,900)); // Only actual healthy INTERNAL clears it.
    CHECK(episode.excused(124,4095,true,20));
    CriticalEpisode early_backwards;
    CHECK(early_backwards.excused(100,4095,true,20));
    CHECK(early_backwards.excused(110,4095,true,20));
    CHECK(!early_backwards.excused(109,4095,true,20));
    CHECK(!early_backwards.excused(111,4095,true,20));
    CriticalEpisode nonrenewing;
    CHECK(nonrenewing.excused(0,4095,true,300000));
    // Fifteen minutes, continuous low INTERNAL, alternating writer gaps and nominal new jobs.
    for (uint64_t now=30000; now<=900000; now+=30000) {
        const bool busy = (now/30000)%2 == 0;
        CHECK(nonrenewing.excused(now,4095,busy,600000) == (busy && now<300000));
    }
    CHECK(!nonrenewing.excused(900001,4096,false,300000));
    CHECK(nonrenewing.excused(900002,4095,true,300000));
    CriticalEpisode zero;
    CHECK(!zero.excused(1,4095,true,0));
    CHECK(!zero.excused(2,4095,true,300000));
}

static void test_runtime_fatal_prefix_equivalence() {
    using Component = tk::RuntimeFatalComponent;
    struct Expected { const char* prefix; Component component; };
    static constexpr Expected expected[]{
        {"clock", Component::Clock},
        {"NVS", Component::Nvs},
        {"Tesla NVS", Component::Nvs},
        {"configuration", Component::Configuration},
        {"VIN ", Component::Identity},
        {"initial key", Component::Identity},
        {"NimBLE", Component::Bluetooth},
        {"HTTP", Component::Http},
        {"setup HTTP", Component::Http},
        {"WiFi", Component::Network},
        {"Ethernet", Component::Network},
        {"W5500", Component::Network},
        {"LAN", Component::Network},
        {"setup AP", Component::Network},
        {"setup WiFi", Component::Network},
        {"Vehicle", Component::Vehicle},
        {"vehicle runtime", Component::Vehicle},
        {"OTA", Component::Ota},
        {"heap supervisor", Component::HeapSupervisor},
        {"app_main", Component::Application},
        {"safe-mode runtime", Component::Application},
    };
    CHECK(tk::runtime_fatal_component(nullptr) == Component::Unknown);
    CHECK(tk::runtime_fatal_component("") == Component::Unknown);
    for (const auto& item : expected) {
        CHECK(tk::runtime_fatal_component(item.prefix) == item.component);
        const std::string extended = std::string(item.prefix) + " boundary detail";
        CHECK(tk::runtime_fatal_component(extended.c_str()) == item.component);
        const std::string embedded = std::string("x") + item.prefix;
        CHECK(tk::runtime_fatal_component(embedded.c_str()) == Component::Unknown);
        const std::string shortened(item.prefix, std::strlen(item.prefix) - 1);
        CHECK(tk::runtime_fatal_component(shortened.c_str()) == Component::Unknown);
    }
    CHECK(tk::runtime_fatal_component("http") == Component::Unknown);
    CHECK(tk::runtime_fatal_component("nvs") == Component::Unknown);
    CHECK(tk::runtime_fatal_component("WiF") == Component::Unknown);
}

struct ControlCasProbe {
    static constexpr bool is_always_lock_free = false;
    uint32_t value, competing;
    bool inject;
    unsigned loads{0}, exchanges{0};
    std::memory_order load_order{}, success_order{}, failure_order{};
    uint32_t load(std::memory_order order) noexcept { ++loads; load_order=order; return value; }
    bool compare_exchange_strong(uint32_t& expected, uint32_t desired,
            std::memory_order success, std::memory_order failure) noexcept {
        ++exchanges; success_order=success; failure_order=failure;
        if (inject) { inject=false; value=competing; expected=value; return false; }
        if (value!=expected) { expected=value; return false; }
        value=desired; return true;
    }
};
struct ControlNativeProbe {
    static constexpr bool is_always_lock_free = true;
    uint32_t value;
    unsigned calls{0};
    std::memory_order order{};
    void fetch_or(uint32_t bits, std::memory_order memory) noexcept {
        ++calls; order=memory; value|=bits;
    }
    uint32_t load(std::memory_order) = delete;
    bool compare_exchange_strong(uint32_t&, uint32_t,
        std::memory_order, std::memory_order) = delete;
};
static void test_ota_control_rmw() {
    for (uint32_t phase=0; phase<8; ++phase) {
        for (uint32_t flags : {0u,256u,512u,768u,UINT32_C(0xffff0000)}) {
            const uint32_t seed=phase|flags;
            for (uint32_t bits : {0u,256u,512u,768u}) {
                for (bool race : {false,true}) {
                    ControlCasProbe probe{seed,seed^1u,race};
                    tk::ota::publish_control_bits(probe,bits);
                    CHECK(probe.value==((race?(seed^1u):seed)|bits));
                    CHECK(probe.loads==1 && probe.exchanges==(race?2u:1u));
                    CHECK(probe.load_order==std::memory_order_acquire);
                    CHECK(probe.success_order==std::memory_order_acq_rel);
                    CHECK(probe.failure_order==std::memory_order_acquire);
                }
                ControlNativeProbe native{seed};
                tk::ota::publish_control_bits(native,bits);
                CHECK(native.calls==1 && native.value==(seed|bits));
                CHECK(native.order==std::memory_order_acq_rel);
            }
        }
    }
}

int main() {
    test_shared_unhealthy_restart_ledger();
    test_stack_context_logic();
    test_heap_supervision();
    test_iteration_fault();
    test_coredump_image();
    test_uptime_rollovers();
    test_config_commit();
    test_effect_reply();
    test_json_request_value_budget();
    test_vcsec_response();
    test_rx_framing();
    test_ble_dispatcher();
    test_session_state();
    test_command_fault();
    test_command_deadline();
    test_session_availability();
    test_command_runner();
    test_command_runner_link_loss_and_faults();
    test_command_runner_telemetry_filter_logic();
    test_tx_wire_framing();
    test_key_regeneration_contract();
    test_telemetry_dispatch_routing_order();
    test_vin();
    test_wifi_credentials();
    test_key_rotation();
    test_nvs_string_load();
    test_vin_transition();
    test_clock_policy();
    test_ha_identity();
    test_syslog_policy();
    test_connect_outcome();
    test_ha_templates();
    test_mqtt_state_lifecycle();
    test_units();
    test_link_state();
    test_link_state_strings();
    test_target();
    test_ota_contract();
    test_ota_channel();
    test_ota_changelog_range();
    test_task_start_gate();
    test_runtime_admission();
    test_runtime_fatal_prefix_equivalence();
    test_config_repair();
    test_identity_recovery();
    test_runtime_status_visibility();
    test_ota_confirm_runtime_admission_matrix();
    test_nimble_start_gate();
    test_syslog_start_gate();
    test_ble_host_health_ota_matrix();
    test_ping_probe_generation();
    test_heap_json_stream();
    test_mcp();
    test_vehicle_data_logic();
    test_status_model();
    test_vehicle_data();
    test_display_helpers();
    test_display_model();
    test_led();
    test_led_driver();
    test_ota_lifetime();
    test_ota_control_rmw();
    test_heap_watchdog();
    test_charge_control();
    test_active_window();
    test_vcsec_sleep_three_valued();
    test_vcsec_sleep_clock();
    test_wake_poll();
    test_telemetry_poll();
    test_ble_readiness();
    test_ble_phase();
    test_ble_row();
    test_http_body();
    test_json_materialize();
    test_config_request_gate();
    test_reset_reason();
    test_crashinfo();
    test_bootlog();
    test_boot_guard();
    test_http_route();
    test_heap_history();
    test_heap_persist();
    test_health_gate();
    test_tls_memory();
    test_ota_recovery_memory();
    test_mqtt_uri();
    test_http_origin();
    test_ble_chunk();
    test_ble_deferred_event();
    test_ble_deferred_queue();
    test_wifi_rollback();
    test_setup_portal();
    test_auto_enrollment();
    test_net_link();
    test_status_eth();
    test_eth_board();
    test_redact();
    test_captive();
    test_service_target_limits();
    test_config_store();
    test_status_sys_and_redaction();

    if (g_failures == 0) {
        std::printf("OK  %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAILED  %d/%d checks failed\n", g_failures, g_checks);
    return 1;
}
