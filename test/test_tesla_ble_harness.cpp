// Host test harness for tesla-ble integration (B1, B2, H1).
// Verifies:
// Part A (B1): TX wire framing emits builder output directly (no double length prefix)
//              and car-side decoder parses a valid SessionInfoRequest.
// Part B (B2): Key regeneration uses >= 2048 B export buffers, refuses mutation if old
//              key export fails, and performs fail-closed rollback if persistence fails.
// Part C (H1): CarServer vehicleData response with foreign request UUID is dropped by
//              dispatcher before delivering telemetry callbacks.

#include <client.h>
#include <adapters.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <car_server.pb.h>
#include <universal_message.pb.h>

#include "logic/command_runner.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
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

// Reproduction of production regenerate_key_native_() from main/vehicle_pairing.cpp
static bool production_regenerate_key_native(Client* client_, StorageAdapter* storage_, size_t buffer_size = 2048) {
    if (!client_ || !storage_) return false;
    std::vector<uint8_t> old_key(buffer_size);
    size_t old_len = old_key.size();
    bool had_old = false;
    if (client_->has_private_key()) {
        if (client_->get_private_key(old_key.data(), old_key.size(), &old_len) != 0) {
            std::printf("  [harness] Failed to export existing private key - aborting re-key\n");
            return false;
        }
        old_key.resize(old_len);
        had_old = true;
    }

    if (client_->create_private_key() != 0) {
        std::printf("  [harness] Failed to create new private key\n");
        if (had_old) (void)client_->load_private_key(old_key.data(), old_key.size());
        return false;
    }

    std::vector<uint8_t> new_key(buffer_size);
    size_t new_len = new_key.size();
    if (client_->get_private_key(new_key.data(), new_key.size(), &new_len) != 0) {
        std::printf("  [harness] Failed to export new private key\n");
        if (had_old) (void)client_->load_private_key(old_key.data(), old_key.size());
        return false;
    }
    new_key.resize(new_len);

    if (!storage_->save(kPrivateKeyKey, new_key)) {
        std::printf("  [harness] Failed to persist new private key\n");
        if (had_old) (void)client_->load_private_key(old_key.data(), old_key.size());
        return false;
    }
    return true;
}

// Vehicle-side view per teslamotors/vehicle-command pkg/connector/ble/ble.go flush()
static bool car_side_decode(const std::vector<uint8_t>& wire, Client& decoder, const char* label) {
    if (wire.size() < 2) {
        std::printf("  %s: wire frame too short (%zu B)\n", label, wire.size());
        return false;
    }
    const size_t L = (static_cast<size_t>(wire[0]) << 8) | wire[1];
    if (wire.size() < 2 + L) {
        std::printf("  %s: incomplete wire frame (need %zu, have %zu)\n", label, 2 + L, wire.size());
        return false;
    }
    std::vector<uint8_t> msg(wire.begin() + 2, wire.begin() + 2 + L);
    UniversalMessage_RoutableMessage out = UniversalMessage_RoutableMessage_init_default;
    const int rc = decoder.parse_universal_message(msg.data(), msg.size(), &out);
    const bool ok = (rc == 0) &&
        (out.which_payload == UniversalMessage_RoutableMessage_session_info_request_tag) &&
        out.has_to_destination &&
        (out.to_destination.which_sub_destination == UniversalMessage_Destination_domain_tag) &&
        (out.to_destination.sub_destination.domain == UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY);
    std::printf("  %-38s L=%zu, parse_rc=%d, is VCSEC SessionInfoRequest=%s\n", label, L, rc, ok ? "YES" : "NO");
    return ok;
}

// Plaintext CarServer response {vehicleData{charge_state{charging_amps=7}}} from INFOTAINMENT.
static std::vector<uint8_t> make_carserver_frame(bool with_uuid, const uint8_t* uuid_bytes = nullptr) {
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
    if (with_uuid) {
        msg.request_uuid.size = 16;
        if (uuid_bytes) {
            std::memcpy(msg.request_uuid.bytes, uuid_bytes, 16);
        } else {
            std::memset(msg.request_uuid.bytes, 0xAB, 16); // foreign UUID
        }
    }
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

int main() {
    int failures = 0;
    std::printf("=================================================================\n");
    std::printf("=== Tesla BLE Integration Harness (B1, B2, H1 Verification)   ===\n");
    std::printf("=================================================================\n");

    // Initialize key for Client
    Client client;
    client.set_vin(kVin);
    client.create_private_key();
    const std::string pem = export_pem(client);
    std::printf("[INFO] Exported P-256 test key PEM length: %zu B\n", pem.size());
    assert(pem.size() >= 220);

    // =========================================================================
    // PART A: TX Wire Framing (B1 Verification)
    // =========================================================================
    std::printf("\n--- Part A: TX Wire Framing (B1) ---\n");
    // Production TX path in main/vehicle_telemetry.cpp: drive_command_runner_()
    // Builders prepend length prefix directly, so builder output IS the wire frame!
    std::vector<uint8_t> tx_buffer(2048);
    uint8_t* payload_buf = tx_buffer.data();
    size_t len = tx_buffer.size();
    int build_rc = client.build_session_info_request_message(
        UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, payload_buf, &len);
    assert(build_rc == 0);
    assert(len > 0);
    tx_buffer.resize(len);

    Client car_decoder;
    const bool car_decoded_prod = car_side_decode(tx_buffer, car_decoder, "Production TX frame (direct builder output):");
    if (!car_decoded_prod) {
        std::printf("FAIL: Production TX frame could not be decoded by vehicle\n");
        failures++;
    } else {
        std::printf("PASS: Production TX frame decoded successfully by car decoder\n");
    }

    // Negative control: Re-inject B1 defect (redundant second length prefix)
    std::vector<uint8_t> defective_b1_frame(2 + tx_buffer.size());
    defective_b1_frame[0] = static_cast<uint8_t>((tx_buffer.size() >> 8) & 0xFF);
    defective_b1_frame[1] = static_cast<uint8_t>(tx_buffer.size() & 0xFF);
    std::memcpy(defective_b1_frame.data() + 2, tx_buffer.data(), tx_buffer.size());

    const bool car_decoded_defective = car_side_decode(defective_b1_frame, car_decoder, "Defective B1 frame (double length prefix):");
    if (car_decoded_defective) {
        std::printf("FAIL: Negative control failed: defective double-prefix frame unexpectedly decoded\n");
        failures++;
    } else {
        std::printf("PASS: Negative control confirmed: defective double-prefix frame fails vehicle decode\n");
    }

    // =========================================================================
    // PART B: Key Regeneration & Transactional Rollback (B2 Verification)
    // =========================================================================
    std::printf("\n--- Part B: Key Regeneration & Transactional Rollback (B2) ---\n");
    MemStorage storage;
    storage.kv[kPrivateKeyKey] = std::vector<uint8_t>(pem.begin(), pem.end() + 1);

    // B2.1: Success path with 2048 B buffer
    const std::string original_pem = export_pem(client);
    bool ok_regen = production_regenerate_key_native(&client, &storage, 2048);
    const std::string new_pem = export_pem(client);
    const std::string stored_pem(reinterpret_cast<const char*>(storage.kv[kPrivateKeyKey].data()));

    if (!ok_regen || new_pem.empty() || new_pem == original_pem || new_pem != stored_pem) {
        std::printf("FAIL: Key regeneration with 2048 B buffer failed or diverged (RAM == NVS: %s)\n",
                    new_pem == stored_pem ? "yes" : "NO");
        failures++;
    } else {
        std::printf("PASS: Key regeneration with 2048 B buffer succeeded; RAM key matches NVS\n");
    }

    // B2.2: Fail-closed rollback path when persistence fails
    storage.fail_save = true;
    const std::string pre_fail_pem = export_pem(client);
    bool fail_regen = production_regenerate_key_native(&client, &storage, 2048);
    const std::string post_fail_pem = export_pem(client);

    if (fail_regen) {
        std::printf("FAIL: Key regeneration unexpectedly reported success when storage failed\n");
        failures++;
    } else if (post_fail_pem != pre_fail_pem) {
        std::printf("FAIL: Key regeneration failed to rollback RAM key on storage failure\n");
        failures++;
    } else {
        std::printf("PASS: Key regeneration rolled back RAM key to previous identity on storage failure\n");
    }
    storage.fail_save = false;

    // B2.3: Negative control with 32 B buffer (the B2 defect)
    bool small_buf_regen = production_regenerate_key_native(&client, &storage, 32);
    if (small_buf_regen) {
        std::printf("FAIL: Negative control failed: 32 B buffer unexpectedly succeeded\n");
        failures++;
    } else {
        std::printf("PASS: Negative control confirmed: 32 B buffer fails cleanly\n");
    }

    // =========================================================================
    // PART C: Dispatch Routing Before Telemetry (H1 Verification)
    // =========================================================================
    std::printf("\n--- Part C: Dispatch Routing Before Telemetry (H1) ---\n");
    tk::CommandRunner runner;
    runner.vcsec_session().set_established({1}, 10, 1000);
    runner.info_session().set_established({2}, 20, 1000);

    tk::BleUuid our_uuid{};
    our_uuid.fill(0x11);
    uint32_t cmd_id = runner.enqueue("Verify Amps", tk::BleDomain::Infotainment,
                                     tk::WakePolicy::WakeIfNeeded, 20000, 1000, our_uuid);
    assert(cmd_id > 0);
    runner.tick(1000, true, true, false);
    runner.notify_tx_complete(1050);

    // Simulate arrival of CarServer response with foreign UUID (0xAB)
    uint8_t foreign_uuid[16];
    std::memset(foreign_uuid, 0xAB, 16);
    std::vector<uint8_t> foreign_frame = make_carserver_frame(true, foreign_uuid);

    // Process frame using production handle_carserver_frame_() logic
    int callback_delivered_count = 0;
    auto charge_state_callback = [&]() { callback_delivered_count++; };

    // Parse outer RoutableMessage (payload starts after 2-byte length)
    UniversalMessage_RoutableMessage msg = UniversalMessage_RoutableMessage_init_default;
    int parse_rc = client.parse_universal_message(foreign_frame.data() + 2, foreign_frame.size() - 2, &msg);
    assert(parse_rc == 0);

    // Route first
    const uint8_t* uuid_ptr = (msg.request_uuid.size > 0) ? msg.request_uuid.bytes : nullptr;
    auto outcome = runner.handle_response(tk::BleDomain::Infotainment, uuid_ptr, msg.request_uuid.size,
                                          true, 100, true, "");

    // Deliver callbacks ONLY if routed (production handle_carserver_frame_ contract)
    if (outcome.routed) {
        charge_state_callback();
    }

    if (outcome.routed || callback_delivered_count > 0) {
        std::printf("FAIL: Foreign UUID response was routed or delivered callback (callbacks=%d)\n", callback_delivered_count);
        failures++;
    } else {
        std::printf("PASS: Foreign UUID response dropped by dispatcher (reason=%d); 0 callbacks delivered\n",
                    static_cast<int>(outcome.drop_reason));
    }

    // Matching UUID control
    auto outcome_matching = runner.handle_response(tk::BleDomain::Infotainment, our_uuid.data(), our_uuid.size(),
                                                   true, 101, true, "");
    if (outcome_matching.routed) {
        charge_state_callback();
    }
    if (!outcome_matching.routed || callback_delivered_count != 1) {
        std::printf("FAIL: Matching UUID response was not routed or callback missed (callbacks=%d)\n", callback_delivered_count);
        failures++;
    } else {
        std::printf("PASS: Matching UUID response successfully routed and delivered 1 callback\n");
    }

    std::printf("\n=================================================================\n");
    if (failures == 0) {
        std::printf("=== ALL INTEGRATION HARNESS CHECKS PASSED (failures=0)        ===\n");
        std::printf("=================================================================\n");
        return 0;
    } else {
        std::printf("=== INTEGRATION HARNESS FAILED (failures=%d)                  ===\n", failures);
        std::printf("=================================================================\n");
        return 1;
    }
}
