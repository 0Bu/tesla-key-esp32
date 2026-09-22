// Host integration harness against the real yoziru/tesla-ble v5.2.0, Nanopb and Mbed TLS.
//
// It runs the PRODUCTION helpers from main/logic/ that the firmware itself calls, so a regression
// in them fails here (not a copy of them):
// Part A (B1): tk::build_ble_tx_frame() + tk::is_well_formed_ble_frame(), the TX path of
//              VehicleController::drive_command_runner_(), with real Client builders; every frame
//              is decoded vehicle-side exactly like teslamotors/vehicle-command ble.go flush().
// Part B (B2): tk::regenerate_private_key(), the transaction behind
//              VehicleController::regenerate_key_native_(), with a real Client and PEM export.
// Part C (H1): the tk::CommandRunner / BleDispatcher routing contract for a CarServer response
//              with a foreign request UUID. The order "route, then deliver telemetry" inside
//              VehicleController::handle_carserver_frame_() is IDF code; it is pinned separately
//              by test/tesla_protocol_vectors.test.mjs.

#include <client.h>
#include <adapters.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <car_server.pb.h>
#include <vcsec.pb.h>
#include <universal_message.pb.h>

#include "logic/command_runner.hpp"
#include "logic/key_rotation.hpp"
#include "logic/rx_framing.hpp"

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace TeslaBLE;

static const char* kVin = "TESTVIN0000000001";
static const char* kPrivateKeyKey = "private_key";

struct MemStorage : StorageAdapter {
    std::map<std::string, std::vector<uint8_t>> kv;
    bool fail_save = false;
    bool load(const std::string& k, std::vector<uint8_t>& v) override {
        auto it = kv.find(k);
        if (it == kv.end()) return false;
        v = it->second;
        return true;
    }
    bool save(const std::string& k, const std::vector<uint8_t>& v) override {
        if (fail_save) return false;
        kv[k] = v;
        return true;
    }
    bool remove(const std::string& k) override {
        return kv.erase(k) > 0;
    }
};

static std::string export_pem(Client& c) {
    std::vector<uint8_t> buf(2048);
    size_t len = 0;
    if (c.get_private_key(buf.data(), buf.size(), &len) != 0) return {};
    return std::string(reinterpret_cast<const char*>(buf.data()));
}

static std::string stored_pem(MemStorage& storage) {
    auto it = storage.kv.find(kPrivateKeyKey);
    if (it == storage.kv.end() || it->second.empty()) return {};
    return std::string(reinterpret_cast<const char*>(it->second.data()));
}

// Vehicle-side view per teslamotors/vehicle-command pkg/connector/ble/ble.go flush(): read the
// 2-byte big-endian length L, then decode the next L bytes as a RoutableMessage.
static bool car_side_decode(const std::vector<uint8_t>& wire, Client& decoder,
                            UniversalMessage_RoutableMessage& out) {
    if (wire.size() < 2) return false;
    const size_t len = (static_cast<size_t>(wire[0]) << 8) | wire[1];
    if (wire.size() < 2 + len) return false;
    std::vector<uint8_t> msg(wire.begin() + 2, wire.begin() + 2 + static_cast<std::ptrdiff_t>(len));
    out = UniversalMessage_RoutableMessage_init_default;
    return decoder.parse_universal_message(msg.data(), msg.size(), &out) == 0;
}

// Plaintext CarServer response {vehicleData{charge_state{charging_amps=7}}} from INFOTAINMENT.
static std::vector<uint8_t> make_carserver_frame(const uint8_t* uuid_bytes) {
    CarServer_Response resp = CarServer_Response_init_default;
    resp.which_response_msg = CarServer_Response_vehicleData_tag;
    resp.response_msg.vehicleData.has_charge_state = true;
    resp.response_msg.vehicleData.charge_state.which_optional_charging_amps =
        CarServer_ChargeState_charging_amps_tag;
    resp.response_msg.vehicleData.charge_state.optional_charging_amps.charging_amps = 7;
    uint8_t inner[1024];
    pb_ostream_t is = pb_ostream_from_buffer(inner, sizeof(inner));
    if (!pb_encode(&is, CarServer_Response_fields, &resp)) {
        std::printf("  [harness] encode inner failed\n");
    }

    UniversalMessage_RoutableMessage msg = UniversalMessage_RoutableMessage_init_default;
    msg.has_from_destination = true;
    msg.from_destination.which_sub_destination = UniversalMessage_Destination_domain_tag;
    msg.from_destination.sub_destination.domain = UniversalMessage_Domain_DOMAIN_INFOTAINMENT;
    msg.which_payload = UniversalMessage_RoutableMessage_protobuf_message_as_bytes_tag;
    std::memcpy(msg.payload.protobuf_message_as_bytes.bytes, inner, is.bytes_written);
    msg.payload.protobuf_message_as_bytes.size = is.bytes_written;
    msg.request_uuid.size = 16;
    std::memcpy(msg.request_uuid.bytes, uuid_bytes, 16);
    uint8_t outer[2048];
    pb_ostream_t os = pb_ostream_from_buffer(outer, sizeof(outer));
    if (!pb_encode(&os, UniversalMessage_RoutableMessage_fields, &msg)) {
        std::printf("  [harness] encode outer failed\n");
    }
    std::vector<uint8_t> frame{static_cast<uint8_t>(os.bytes_written >> 8),
                               static_cast<uint8_t>(os.bytes_written & 0xFF)};
    frame.insert(frame.end(), outer, outer + os.bytes_written);
    return frame;
}

static int failures = 0;

static void expect(bool ok, const char* pass_text, const char* fail_text) {
    if (ok) {
        std::printf("PASS: %s\n", pass_text);
    } else {
        std::printf("FAIL: %s\n", fail_text);
        ++failures;
    }
}

int main() {
    std::printf("=================================================================\n");
    std::printf("=== Tesla BLE Integration Harness (B1, B2, H1 Verification)   ===\n");
    std::printf("=================================================================\n");

    Client client;
    client.set_vin(kVin);
    if (client.create_private_key() != 0) {
        std::printf("FAIL: tesla-ble could not create a P-256 key\n");
        return 1;
    }
    const std::string pem = export_pem(client);
    std::printf("[INFO] Exported P-256 test key PEM length: %zu B (+1 NUL)\n", pem.size());
    expect(!pem.empty() && pem.size() + 1 <= tk::kPrivateKeyPemCapacity,
           "tk::kPrivateKeyPemCapacity holds the real tesla-ble PEM export",
           "tk::kPrivateKeyPemCapacity is smaller than the real tesla-ble PEM export");

    // =========================================================================
    // PART A: TX wire framing (B1) through the production helpers
    // =========================================================================
    std::printf("\n--- Part A: TX wire framing (B1) ---\n");
    struct TxCase {
        const char* name;
        std::function<int(uint8_t*, size_t*)> build;
        bool routable;  // false: bare VCSEC ToVCSECMessage (the pairing whitelist request)
        UniversalMessage_Domain to_domain;
        pb_size_t payload_tag;
    };
    const std::vector<TxCase> tx_cases = {
        {"VCSEC SessionInfoRequest",
         [&](uint8_t* b, size_t* l) {
             return client.build_session_info_request_message(
                 UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, b, l);
         },
         true, UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY,
         UniversalMessage_RoutableMessage_session_info_request_tag},
        {"Infotainment SessionInfoRequest",
         [&](uint8_t* b, size_t* l) {
             return client.build_session_info_request_message(
                 UniversalMessage_Domain_DOMAIN_INFOTAINMENT, b, l);
         },
         true, UniversalMessage_Domain_DOMAIN_INFOTAINMENT,
         UniversalMessage_RoutableMessage_session_info_request_tag},
        {"Whitelist Add Key (Charging Manager)",
         [&](uint8_t* b, size_t* l) {
             return client.build_white_list_message(
                 Keys_Role_ROLE_CHARGING_MANAGER, VCSEC_KeyFormFactor_KEY_FORM_FACTOR_CLOUD_KEY, b, l);
         },
         false, UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, 0},
    };

    Client car_decoder;
    for (const TxCase& tc : tx_cases) {
        std::vector<uint8_t> wire;
        const int rc = tk::build_ble_tx_frame(wire, tk::RxFramer::kMaxFrameLength, tc.build);
        bool decoded = false;
        bool content_ok = false;
        if (rc == 0 && tc.routable) {
            UniversalMessage_RoutableMessage out = UniversalMessage_RoutableMessage_init_default;
            decoded = car_side_decode(wire, car_decoder, out);
            content_ok = decoded && out.has_to_destination &&
                         out.to_destination.which_sub_destination == UniversalMessage_Destination_domain_tag &&
                         out.to_destination.sub_destination.domain == tc.to_domain &&
                         out.which_payload == tc.payload_tag;
        } else if (rc == 0 && wire.size() >= 2) {
            const size_t len = (static_cast<size_t>(wire[0]) << 8) | wire[1];
            VCSEC_ToVCSECMessage out = VCSEC_ToVCSECMessage_init_default;
            pb_istream_t in = pb_istream_from_buffer(wire.data() + 2, wire.size() - 2);
            decoded = len == wire.size() - 2 && pb_decode(&in, VCSEC_ToVCSECMessage_fields, &out);
            content_ok = decoded && out.has_signedMessage;
        }
        const bool ok = decoded && content_ok && tk::is_well_formed_ble_frame(wire);
        std::printf("  %-38s rc=%d wire=%zu B well_formed=%s vehicle_decode=%s\n", tc.name, rc,
                    wire.size(), tk::is_well_formed_ble_frame(wire) ? "yes" : "NO",
                    decoded ? "yes" : "NO");
        expect(ok, "production TX frame is well-formed and decodes vehicle-side",
               "production TX frame is malformed or not decodable vehicle-side");
    }

    // Negative control: a builder chain that re-adds the B1 second length prefix. The production
    // guard must refuse the frame, and the vehicle-side decoder must reject it as well.
    {
        const auto& real = tx_cases.front().build;
        std::vector<uint8_t> wire;
        const int rc = tk::build_ble_tx_frame(
            wire, tk::RxFramer::kMaxFrameLength, [&](uint8_t* b, size_t* l) -> int {
                size_t inner = *l - 2;
                const int brc = real(b + 2, &inner);
                if (brc != 0) return brc;
                b[0] = static_cast<uint8_t>(inner >> 8);
                b[1] = static_cast<uint8_t>(inner & 0xFF);
                *l = inner + 2;
                return 0;
            });
        UniversalMessage_RoutableMessage out = UniversalMessage_RoutableMessage_init_default;
        const bool decoded = rc == 0 && car_side_decode(wire, car_decoder, out);
        expect(rc == 0 && !tk::is_well_formed_ble_frame(wire) && !decoded,
               "negative control: double-prefixed frame is refused by the guard and the vehicle",
               "negative control: double-prefixed frame passed the guard or decoded vehicle-side");
    }

    // =========================================================================
    // PART B: key regeneration (B2) through tk::regenerate_private_key()
    // =========================================================================
    std::printf("\n--- Part B: key regeneration and rollback (B2) ---\n");
    MemStorage storage;
    storage.kv[kPrivateKeyKey] = std::vector<uint8_t>(pem.begin(), pem.end() + 1);
    auto persist = [&](const std::vector<uint8_t>& key) { return storage.save(kPrivateKeyKey, key); };

    {
        const std::string before = export_pem(client);
        const auto result = tk::regenerate_private_key(client, persist);
        const std::string after = export_pem(client);
        expect(result == tk::KeyRegenerationResult::Committed && !after.empty() && after != before &&
                   after == stored_pem(storage),
               "regeneration committed a new key; RAM key matches storage",
               "regeneration failed or RAM key diverged from storage");
    }
    {
        storage.fail_save = true;
        const std::string before = export_pem(client);
        const std::string stored_before = stored_pem(storage);
        const auto result = tk::regenerate_private_key(client, persist);
        expect(result == tk::KeyRegenerationResult::PersistFailed && export_pem(client) == before &&
                   stored_pem(storage) == stored_before,
               "persistence failure restored the previous RAM key; storage untouched",
               "persistence failure did not roll back the RAM key");
        storage.fail_save = false;
    }
    {
        // Negative control: the B2 defect (a 32-byte export buffer) must fail closed.
        const std::string before = export_pem(client);
        const std::string stored_before = stored_pem(storage);
        const auto result = tk::regenerate_private_key(client, persist, 32);
        expect(result == tk::KeyRegenerationResult::ExportExistingFailed &&
                   export_pem(client) == before && stored_pem(storage) == stored_before,
               "negative control: 32 B export buffer fails closed without touching the key",
               "negative control: 32 B export buffer mutated the key or succeeded");
    }
    {
        // First-boot path: no key in RAM yet.
        Client fresh;
        fresh.set_vin(kVin);
        MemStorage empty_storage;
        const auto result = tk::regenerate_private_key(
            fresh, [&](const std::vector<uint8_t>& key) { return empty_storage.save(kPrivateKeyKey, key); });
        expect(result == tk::KeyRegenerationResult::Committed && fresh.has_private_key() &&
                   export_pem(fresh) == stored_pem(empty_storage),
               "first-boot regeneration committed a key; RAM key matches storage",
               "first-boot regeneration failed or diverged from storage");
    }

    // =========================================================================
    // PART C: routing contract before telemetry (H1)
    // =========================================================================
    std::printf("\n--- Part C: CarServer routing contract (H1) ---\n");
    tk::CommandRunner runner;
    runner.vcsec_session().set_established({1}, 10, 1000);
    runner.info_session().set_established({2}, 20, 1000);

    tk::BleUuid our_uuid{};
    our_uuid.fill(0x11);
    const uint32_t cmd_id = runner.enqueue("Verify Amps", tk::BleDomain::Infotainment,
                                           tk::WakePolicy::WakeIfNeeded, 20000, 1000, our_uuid);
    expect(cmd_id > 0, "command enqueued", "command could not be enqueued");
    runner.tick(1000, true, true, false);
    runner.notify_tx_complete(1050);

    uint8_t foreign_uuid[16];
    std::memset(foreign_uuid, 0xAB, sizeof(foreign_uuid));
    const std::vector<uint8_t> foreign_frame = make_carserver_frame(foreign_uuid);
    UniversalMessage_RoutableMessage msg = UniversalMessage_RoutableMessage_init_default;
    const bool parsed = car_side_decode(foreign_frame, client, msg);
    expect(parsed, "foreign CarServer frame parsed", "foreign CarServer frame did not parse");

    const uint8_t* uuid_ptr = (msg.request_uuid.size > 0) ? msg.request_uuid.bytes : nullptr;
    const auto foreign = runner.handle_response(tk::BleDomain::Infotainment, uuid_ptr, msg.request_uuid.size,
                                                true, 100, true, "");
    expect(!foreign.routed && foreign.drop_reason == tk::DispatchDropReason::Unmatched,
           "foreign-UUID response is not routed (no telemetry may be delivered for it)",
           "foreign-UUID response was routed");
    const auto matching = runner.handle_response(tk::BleDomain::Infotainment, our_uuid.data(), our_uuid.size(),
                                                 true, 101, true, "");
    expect(matching.routed, "matching-UUID response is routed", "matching-UUID response was not routed");

    std::printf("\n=================================================================\n");
    if (failures == 0) {
        std::printf("=== ALL INTEGRATION HARNESS CHECKS PASSED (failures=0)        ===\n");
        std::printf("=================================================================\n");
        return 0;
    }
    std::printf("=== INTEGRATION HARNESS FAILED (failures=%d)                  ===\n", failures);
    std::printf("=================================================================\n");
    return 1;
}
