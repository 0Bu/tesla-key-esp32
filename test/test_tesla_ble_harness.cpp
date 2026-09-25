// Host integration harness against the real yoziru/tesla-ble v5.2.0 (with the repository patch
// series, including the PSA port), Nanopb, and the Mbed TLS 4.1 / TF-PSA-Crypto commit that the
// pinned ESP-IDF v6.1 builds into the firmware.
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
// Part D (V1): protocol-vector known answers from teslamotors/vehicle-command pkg/protocol
//              protocol.md, run through the PSA-ported crypto bindings the firmware links: P-256
//              key import, ECDH session key, session-info HMAC and tag, AES-GCM request/response
//              (response metadata bound to the response's own counter), the VIN BLE name, PEM
//              round trips (the NVS key format) and the firmware key fingerprint. The fingerprint
//              runs through a mirror of VehicleController::compute_key_fingerprint_(), which is IDF
//              code; the helpers listed above for parts A and B are the production ones.
//              test/tesla_protocol_vectors.test.mjs pins the same vectors independently of the
//              library; this part proves the patched library reproduces them.

#include <client.h>
#include <adapters.h>
#include <crypto_context.h>
#include <peer.h>
#include <vin_utils.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <car_server.pb.h>
#include <vcsec.pb.h>
#include <universal_message.pb.h>

#include <mbedtls/pk.h>
#include <psa/crypto.h>

// Official protocol test keys (never production): tesla-ble tests/test_constants.h, which copies
// them from vehicle-command protocol.md.
#include "test_constants.h"

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

// ---- Part D helpers: official vectors and an independent PSA reference -------------------------

namespace kat {

namespace tc = TeslaBLE::TestConstants;

// vehicle-command protocol.md, "AES-GCM" command example: sorted metadata TLVs, nonce, plaintext,
// ciphertext and tag for the protocol session key; and the session-info HMAC key it derives.
constexpr const char* kMetadata =
    "000105010103021135594a333031323334353637383941424303104c463f9cc0d3d26906e982ed224adde6040400000a5f050400000007"
    "070400000002ff";
constexpr const char* kNonce = "dbf79447fa156674dae1caed";
constexpr const char* kPlaintext = "120452020801";
constexpr const char* kCiphertext = "38038e8c0f2e";
constexpr const char* kTag = "c228e0ff64991481db3a7bbc133696c5";
constexpr const char* kSessionInfoKey = "fceb679ee7bca756fcd441bf238bf2f338629b41d9eb9c67be1b32c9672ce300";
// protocol.md, "Session info" example response: the serialized SessionInfo, the request UUID it
// answers, and the HMAC-SHA256 session_info_tag the vehicle sends with it.
constexpr const char* kSessionInfo =
    "0806124104c7a1f47138486aa4729971494878d33b1a24e39571f748a6e16c5955b3d877d3a6aaa0e955166474af5d32c410f439a22341"
    "37ad1bb085fd4e8813c958f11d971a104c463f9cc0d3d26906e982ed224adde6255a0a0000";
constexpr const char* kSessionInfoRequestUuid = "1588d5a30eabc6f8fc9a951b11f6fd11";
constexpr const char* kSessionInfoTag = "996c1fe38331be138f8039c194b14db2198846ed7d8251e6749284d7b32ea002";

std::vector<uint8_t> unhex(const char* text) {
    std::vector<uint8_t> out;
    for (size_t i = 0; text[i] && text[i + 1]; i += 2) {
        unsigned value = 0;
        std::sscanf(text + i, "%2x", &value);
        out.push_back(static_cast<uint8_t>(value));
    }
    return out;
}

std::string hex(const uint8_t* bytes, size_t length) {
    std::string out;
    char pair[3];
    for (size_t i = 0; i < length; ++i) {
        std::snprintf(pair, sizeof pair, "%02x", bytes[i]);
        out += pair;
    }
    return out;
}

std::vector<uint8_t> sha256(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out(32);
    size_t length = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, input.data(), input.size(), out.data(), out.size(), &length) !=
            PSA_SUCCESS ||
        length != out.size()) {
        return {};
    }
    return out;
}

// One-shot AES-128-GCM through PSA: an independent reference for the ported multipart code.
std::vector<uint8_t> gcm_encrypt(const uint8_t* key, const std::vector<uint8_t>& nonce,
                                 const std::vector<uint8_t>& aad, const std::vector<uint8_t>& plaintext) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    std::vector<uint8_t> out(plaintext.size() + 16);
    size_t length = 0;
    const bool ok = psa_import_key(&attributes, key, 16, &id) == PSA_SUCCESS &&
                    psa_aead_encrypt(id, PSA_ALG_GCM, nonce.data(), nonce.size(), aad.data(), aad.size(),
                                     plaintext.data(), plaintext.size(), out.data(), out.size(),
                                     &length) == PSA_SUCCESS;
    psa_destroy_key(id);
    psa_reset_key_attributes(&attributes);
    if (!ok) return {};
    out.resize(length);
    return out;  // ciphertext || 16-byte tag
}

const uint8_t* client_pem() { return reinterpret_cast<const uint8_t*>(tc::CLIENT_PRIVATE_KEY_PEM); }
size_t client_pem_size() { return std::strlen(tc::CLIENT_PRIVATE_KEY_PEM) + 1; }  // stored with NUL

std::shared_ptr<CryptoContext> loaded_client_context() {
    auto context = std::make_shared<CryptoContext>();
    if (context->load_private_key(client_pem(), client_pem_size()) != TeslaBLE_Status_E_OK) return nullptr;
    return context;
}

// Mirrors VehicleController::compute_key_fingerprint_() (main/vehicle_pairing.cpp): PK parses the
// stored PEM, the key moves into a volatile PSA key, and SHA-1 over the exported point gives the id.
std::string firmware_fingerprint(const uint8_t* pem, size_t pem_size) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    std::string fingerprint;
    if (mbedtls_pk_parse_key(&pk, pem, pem_size, nullptr, 0) == 0) {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        uint8_t point[65];
        size_t point_size = 0;
        uint8_t digest[20];
        size_t digest_size = 0;
        if (mbedtls_pk_get_psa_attributes(&pk, PSA_KEY_USAGE_DERIVE, &attributes) == 0 &&
            psa_get_key_type(&attributes) == PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1) &&
            psa_get_key_bits(&attributes) == 256 && mbedtls_pk_import_into_psa(&pk, &attributes, &id) == 0 &&
            psa_export_public_key(id, point, sizeof point, &point_size) == PSA_SUCCESS &&
            psa_hash_compute(PSA_ALG_SHA_1, point, point_size, digest, sizeof digest, &digest_size) ==
                PSA_SUCCESS &&
            digest_size == sizeof digest) {
            fingerprint = hex(digest, 4);
        }
        psa_reset_key_attributes(&attributes);
    }
    psa_destroy_key(id);
    mbedtls_pk_free(&pk);
    return fingerprint;
}

// A genuine secp384r1 key in the SEC1 PEM shape a stored key has, generated here.
std::vector<uint8_t> p384_pem() {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 384);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT);
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    std::vector<uint8_t> pem(2048);
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    const bool ok = psa_generate_key(&attributes, &id) == PSA_SUCCESS && mbedtls_pk_copy_from_psa(id, &pk) == 0 &&
                    mbedtls_pk_write_key_pem(&pk, pem.data(), pem.size()) == 0;
    psa_destroy_key(id);
    psa_reset_key_attributes(&attributes);
    mbedtls_pk_free(&pk);
    if (!ok) return {};
    pem.resize(std::strlen(reinterpret_cast<const char*>(pem.data())) + 1);
    return pem;
}

}  // namespace kat

int main() {
    std::printf("=================================================================\n");
    std::printf("=== Tesla BLE Integration Harness (B1, B2, H1, V1)            ===\n");
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

    // =========================================================================
    // PART D: protocol-vector known answers through the PSA crypto bindings (V1)
    // =========================================================================
    std::printf("\n--- Part D: protocol-vector known answers (V1) ---\n");
    namespace tc = TeslaBLE::TestConstants;
    expect(psa_crypto_init() == PSA_SUCCESS, "PSA Crypto initialised", "PSA Crypto failed to initialise");
    {
        auto context = kat::loaded_client_context();
        uint8_t point[65];
        size_t point_size = sizeof point;
        expect(context && context->generate_public_key(point, &point_size) == TeslaBLE_Status_E_OK &&
                   point_size == 65 && std::memcmp(point, tc::EXPECTED_CLIENT_PUBLIC_KEY, 65) == 0,
               "PEM key import yields the official client public key",
               "PEM key import does not yield the official client public key");
    }
    {
        auto context = kat::loaded_client_context();
        Peer peer(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, context, tc::TEST_VIN);
        expect(context && peer.load_tesla_key(tc::EXPECTED_VEHICLE_PUBLIC_KEY, 65) == TeslaBLE_Status_E_OK &&
                   std::memcmp(peer.get_shared_secret(), tc::EXPECTED_SESSION_KEY, 16) == 0,
               "ECDH through Peer yields the official session key SHA1(X)[:16]",
               "ECDH through Peer does not yield the official session key");
    }
    {
        uint8_t key[32];
        expect(CryptoUtils::derive_session_info_key(tc::EXPECTED_SESSION_KEY, 16, key, sizeof key) ==
                       TeslaBLE_Status_E_OK &&
                   kat::hex(key, sizeof key) == kat::kSessionInfoKey,
               "session-info key matches the official HMAC-SHA256 vector",
               "session-info key does not match the official HMAC-SHA256 vector");
    }
    const std::vector<uint8_t> metadata = kat::unhex(kat::kMetadata);
    const std::vector<uint8_t> plaintext = kat::unhex(kat::kPlaintext);
    {
        const auto reference = kat::gcm_encrypt(tc::EXPECTED_SESSION_KEY, kat::unhex(kat::kNonce),
                                                kat::sha256(metadata), plaintext);
        expect(reference.size() == plaintext.size() + 16 &&
                   kat::hex(reference.data(), plaintext.size()) == kat::kCiphertext &&
                   kat::hex(reference.data() + plaintext.size(), 16) == kat::kTag,
               "reference AES-GCM reproduces the official command ciphertext and tag",
               "reference AES-GCM does not reproduce the official command vector");
    }
    {
        auto context = kat::loaded_client_context();
        Peer peer(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, context, tc::TEST_VIN);
        std::vector<uint8_t> input = plaintext;
        std::vector<uint8_t> ad = metadata;
        uint8_t ciphertext[64];
        size_t ciphertext_size = 0;
        uint8_t tag[16];
        uint8_t nonce[12];
        const bool encrypted =
            context && peer.load_tesla_key(tc::EXPECTED_VEHICLE_PUBLIC_KEY, 65) == TeslaBLE_Status_E_OK &&
            peer.encrypt(input.data(), input.size(), ciphertext, sizeof ciphertext, &ciphertext_size, tag,
                         ad.data(), ad.size(), nonce) == TeslaBLE_Status_E_OK &&
            ciphertext_size == plaintext.size();
        const auto reference = kat::gcm_encrypt(tc::EXPECTED_SESSION_KEY, std::vector<uint8_t>(nonce, nonce + 12),
                                                kat::sha256(metadata), plaintext);
        expect(encrypted && reference.size() == plaintext.size() + 16 &&
                   std::memcmp(ciphertext, reference.data(), plaintext.size()) == 0 &&
                   std::memcmp(tag, reference.data() + plaintext.size(), 16) == 0,
               "Peer::encrypt matches reference AES-GCM bit for bit (nonce, AAD=SHA256(metadata), tag)",
               "Peer::encrypt diverges from reference AES-GCM");
    }
    {
        // A response sealed the way the vehicle seals it: the metadata of protocol.md "Response
        // metadata", serialized here independently of the library, carries the RESPONSE's counter.
        // The vehicle counts responses per request (VCSEC sends up to three), so that counter need not
        // equal the request counter the peer holds; vehicle-command's Signer.Decrypt binds the AAD to
        // the counter the response carries. Response 4 answers while the peer's request counter is 7.
        auto context = kat::loaded_client_context();
        Peer peer(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, context, tc::TEST_VIN);
        const uint8_t request_hash[17] = {0x05, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        const uint32_t flags = 1;
        const uint32_t fault = 0;
        const uint32_t response_counter = 4;
        const uint32_t request_counter = 7;
        const char* const expected_metadata =
            "000109"                                  // TAG_SIGNATURE_TYPE: SIGNATURE_TYPE_AES_GCM_RESPONSE
            "010103"                                  // TAG_DOMAIN: DOMAIN_INFOTAINMENT
            "021135594a3330313233343536373839414243"  // TAG_PERSONALIZATION: the VIN
            "050400000004"                            // TAG_COUNTER: the response's own counter
            "070400000001"                            // TAG_FLAGS: the response's flags, always present
            "081105010203040506070809"                // TAG_REQUEST_HASH: method byte + request tag
            "0a0b0c0d0e0f10"
            "090400000000"                            // TAG_FAULT
            "ff";                                     // TAG_END
        bool ready = context && peer.load_tesla_key(tc::EXPECTED_VEHICLE_PUBLIC_KEY, 65) == TeslaBLE_Status_E_OK;
        peer.set_counter(request_counter);
        uint8_t ad[80];
        size_t ad_size = 0;
        ready = ready && peer.get_counter() == request_counter &&
                peer.construct_response_ad_buffer(response_counter, ad, &ad_size, flags, request_hash,
                                                  sizeof request_hash, fault) == TeslaBLE_Status_E_OK;
        expect(ready && kat::hex(ad, ad_size) == expected_metadata,
               "response metadata matches protocol.md and carries the response's own counter",
               "response metadata diverges from protocol.md");
        expect(peer.construct_ad_buffer(Signatures_SignatureType_SIGNATURE_TYPE_AES_GCM_RESPONSE, tc::TEST_VIN, 0,
                                        ad, &ad_size, flags, request_hash, sizeof request_hash,
                                        fault) == TeslaBLE_Status_E_ERROR_INVALID_PARAMS,
               "request metadata builder refuses the response type (no request counter in a response AAD)",
               "request metadata builder still builds response metadata from the request counter");

        const auto nonce = kat::unhex("0102030405060708090a0b0c");
        const auto response = kat::unhex("0a0548656c6c6f1001");
        const auto sealed =
            kat::gcm_encrypt(tc::EXPECTED_SESSION_KEY, nonce, kat::sha256(kat::unhex(expected_metadata)), response);
        uint8_t out[64];
        size_t out_size = 0;
        const bool opened =
            ready && sealed.size() == response.size() + 16 &&
            peer.decrypt_response(sealed.data(), response.size(), nonce.data(), sealed.data() + response.size(),
                                  request_hash, sizeof request_hash, flags, fault, response_counter, out, sizeof out,
                                  &out_size) == TeslaBLE_Status_E_OK &&
            out_size == response.size() && std::memcmp(out, response.data(), response.size()) == 0;
        expect(opened, "decrypt_response opens a vehicle-sealed response whose counter differs from the request's",
               "decrypt_response rejected a vehicle-sealed response");

        // The unported library built this AAD from its own request counter. With the tag now
        // verified, that choice refuses the same authentic response.
        const bool request_counter_refused =
            ready && peer.decrypt_response(sealed.data(), response.size(), nonce.data(),
                                           sealed.data() + response.size(), request_hash, sizeof request_hash, flags,
                                           fault, request_counter, out, sizeof out,
                                           &out_size) == TeslaBLE_Status_E_ERROR_DECRYPT;
        expect(request_counter_refused, "an AAD built from the request counter does not verify that response",
               "an AAD built from the request counter verified a response sealed with another counter");

        std::vector<uint8_t> bad_tag(sealed.begin() + static_cast<std::ptrdiff_t>(response.size()), sealed.end());
        if (!bad_tag.empty()) bad_tag[0] ^= 0x01;
        std::memset(out, 0xAA, sizeof out);
        const bool refused =
            ready && bad_tag.size() == 16 &&
            peer.decrypt_response(sealed.data(), response.size(), nonce.data(), bad_tag.data(), request_hash,
                                  sizeof request_hash, flags, fault, response_counter, out, sizeof out,
                                  &out_size) == TeslaBLE_Status_E_ERROR_DECRYPT;
        bool wiped = true;
        for (size_t i = 0; i < response.size(); ++i) wiped = wiped && out[i] == 0;
        expect(refused && wiped, "tampered tag is refused and no unauthenticated plaintext is left behind",
               "tampered tag was accepted or left plaintext in the output buffer");

        const bool invalid_params_refused =
            ready &&
            peer.decrypt_response(nullptr, response.size(), nonce.data(), sealed.data() + response.size(),
                                  request_hash, sizeof request_hash, flags, fault, response_counter, out, sizeof out,
                                  &out_size) == TeslaBLE_Status_E_ERROR_INVALID_PARAMS &&
            peer.decrypt_response(sealed.data(), response.size(), nullptr, sealed.data() + response.size(),
                                  request_hash, sizeof request_hash, flags, fault, response_counter, out, sizeof out,
                                  &out_size) == TeslaBLE_Status_E_ERROR_INVALID_PARAMS &&
            peer.decrypt_response(sealed.data(), response.size(), nonce.data(), nullptr,
                                  request_hash, sizeof request_hash, flags, fault, response_counter, out, sizeof out,
                                  &out_size) == TeslaBLE_Status_E_ERROR_INVALID_PARAMS &&
            peer.decrypt_response(sealed.data(), response.size(), nonce.data(), sealed.data() + response.size(),
                                  request_hash, sizeof request_hash, flags, fault, response_counter, nullptr, sizeof out,
                                  &out_size) == TeslaBLE_Status_E_ERROR_INVALID_PARAMS &&
            peer.decrypt_response(sealed.data(), response.size(), nonce.data(), sealed.data() + response.size(),
                                  request_hash, sizeof request_hash, flags, fault, response_counter, out, sizeof out,
                                  nullptr) == TeslaBLE_Status_E_ERROR_INVALID_PARAMS &&
            peer.decrypt_response(sealed.data(), response.size(), nonce.data(), sealed.data() + response.size(),
                                  request_hash, sizeof request_hash, flags, fault, response_counter, out,
                                  response.size() - 1, &out_size) == TeslaBLE_Status_E_ERROR_INVALID_PARAMS;
        expect(invalid_params_refused,
               "null pointers and undersized output buffer in decrypt_response are refused with INVALID_PARAMS",
               "decrypt_response accepted null pointer or undersized buffer or returned wrong error code");
    }
    {
        // protocol.md's session-info example through the library's own verification path: ECDH with
        // the vehicle key in SessionInfo, the session-info key, and HMAC-SHA256 over the HMAC
        // metadata (VIN, request UUID as challenge) followed by the serialized SessionInfo.
        Client client;
        client.set_vin(tc::TEST_VIN);
        const auto info_bytes = kat::unhex(kat::kSessionInfo);
        const auto uuid = kat::unhex(kat::kSessionInfoRequestUuid);
        auto tag = kat::unhex(kat::kSessionInfoTag);
        Signatures_SessionInfo info = Signatures_SessionInfo_init_default;
        pb_istream_t in = pb_istream_from_buffer(info_bytes.data(), info_bytes.size());
        const bool decoded = client.load_private_key(kat::client_pem(), kat::client_pem_size()) == 0 &&
                             pb_decode(&in, Signatures_SessionInfo_fields, &info);
        const bool accepted = decoded && client.verify_session_info_tag(info, info_bytes.data(), info_bytes.size(),
                                                                        uuid.data(), uuid.size(), tag.data(),
                                                                        tag.size());
        tag[0] ^= 0x01;
        const bool refused = decoded && !client.verify_session_info_tag(info, info_bytes.data(), info_bytes.size(),
                                                                        uuid.data(), uuid.size(), tag.data(),
                                                                        tag.size());
        expect(accepted && refused, "session-info tag verifies against the official vector; a flipped bit is refused",
               "session-info tag verification diverges from the official vector");
    }
    expect(get_vin_advertisement_name("5YJS0000000000000") == "S1a87a5a75f3df858C",
           "VIN BLE name matches the official vector", "VIN BLE name does not match the official vector");
    {
        Client pem_client;
        uint8_t pem_out[2048];
        size_t pem_size = 0;
        auto reloaded = std::make_shared<CryptoContext>();
        uint8_t point[65];
        size_t point_size = sizeof point;
        // The unported library on Mbed TLS 3.6 (ESP-IDF 5.5.5 firmware) exported this key as the
        // SEC1 PEM text plus a final newline and NUL (228 B); the NVS key format must not change.
        const std::string legacy_export = std::string(tc::CLIENT_PRIVATE_KEY_PEM) + "\n";
        const bool ok = pem_client.load_private_key(kat::client_pem(), kat::client_pem_size()) == 0 &&
                        pem_client.get_private_key(pem_out, sizeof pem_out, &pem_size) == 0 &&
                        pem_size == legacy_export.size() + 1 && pem_out[pem_size - 1] == 0 &&
                        std::memcmp(pem_out, legacy_export.data(), legacy_export.size()) == 0 &&
                        reloaded->load_private_key(pem_out, pem_size) == TeslaBLE_Status_E_OK &&
                        reloaded->generate_public_key(point, &point_size) == TeslaBLE_Status_E_OK &&
                        point_size == 65 && std::memcmp(point, tc::EXPECTED_CLIENT_PUBLIC_KEY, 65) == 0;
        expect(ok, "PEM export is byte-identical to the Mbed TLS 3.6 export (NVS format) and reloads",
               "PEM export differs from the Mbed TLS 3.6 export or does not reload");
    }
    {
        CryptoContext generated;
        uint8_t pem_out[2048];
        size_t pem_size = 0;
        uint8_t point_a[65];
        uint8_t point_b[65];
        size_t size_a = sizeof point_a;
        size_t size_b = sizeof point_b;
        uint8_t key_a[16];
        uint8_t key_b[16];
        CryptoContext reloaded;
        const bool ok =
            generated.create_private_key() == TeslaBLE_Status_E_OK &&
            generated.get_private_key(pem_out, sizeof pem_out, &pem_size) == TeslaBLE_Status_E_OK &&
            std::strstr(reinterpret_cast<const char*>(pem_out), "-----BEGIN EC PRIVATE KEY-----") != nullptr &&
            generated.generate_public_key(point_a, &size_a) == TeslaBLE_Status_E_OK &&
            reloaded.load_private_key(pem_out, pem_size) == TeslaBLE_Status_E_OK &&
            reloaded.generate_public_key(point_b, &size_b) == TeslaBLE_Status_E_OK && size_a == size_b &&
            std::memcmp(point_a, point_b, size_a) == 0 &&
            generated.perform_tesla_ecdh(tc::EXPECTED_VEHICLE_PUBLIC_KEY, 65, key_a) == TeslaBLE_Status_E_OK &&
            reloaded.perform_tesla_ecdh(tc::EXPECTED_VEHICLE_PUBLIC_KEY, 65, key_b) == TeslaBLE_Status_E_OK &&
            std::memcmp(key_a, key_b, 16) == 0;
        expect(ok, "generated key survives the SEC1 PEM round trip and agrees with the vehicle",
               "generated key does not survive the PEM round trip");
    }
    {
        const auto wrong_curve = kat::p384_pem();
        CryptoContext context;
        static const char kGarbage[] = "invalid_key_data_that_should_fail";
        const bool p384_refused = !wrong_curve.empty() &&
                                  context.load_private_key(wrong_curve.data(), wrong_curve.size()) ==
                                      TeslaBLE_Status_E_ERROR_INVALID_PARAMS &&
                                  !context.is_private_key_initialized();
        const bool garbage_refused =
            context.load_private_key(reinterpret_cast<const uint8_t*>(kGarbage), sizeof kGarbage) !=
                TeslaBLE_Status_E_OK &&
            !context.is_private_key_initialized();
        expect(p384_refused && garbage_refused, "non-P-256 and malformed keys are refused",
               "a non-P-256 or malformed key was accepted");
    }
    {
        auto context = kat::loaded_client_context();
        uint8_t point[65];
        size_t point_size = sizeof point;
        uint8_t key_id[4];
        const bool ok = context && context->generate_public_key(point, &point_size) == TeslaBLE_Status_E_OK &&
                        context->generate_key_id(point, point_size, key_id) == TeslaBLE_Status_E_OK &&
                        kat::firmware_fingerprint(kat::client_pem(), kat::client_pem_size()) ==
                            kat::hex(key_id, sizeof key_id);
        expect(ok, "firmware key fingerprint (PK -> PSA -> SHA-1) equals tesla-ble's key id",
               "firmware key fingerprint diverges from tesla-ble's key id");
    }

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
