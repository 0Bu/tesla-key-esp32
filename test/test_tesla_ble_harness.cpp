// Host integration harness against the real yoziru/tesla-ble v6.3.0 (with the repository patch
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
//              VehicleController::handle_carserver_frame_() is also compiled directly below against
//              real Client/Peer crypto and replay windows, substituting only the IDF facade/sinks.
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
#include <pb_common.h>
#include <car_server.pb.h>
#include <vcsec.pb.h>
#include <universal_message.pb.h>

#include <mbedtls/pk.h>
#include <psa/crypto.h>
#include "coredump_hash_vectors.hpp"
#include <psa/crypto_extra.h>

// Official protocol test keys (never production): tesla-ble tests/test_constants.h, which copies
// them from vehicle-command protocol.md.
#include "test_constants.h"

#include "logic/command_runner.hpp"
#include "logic/session_availability.hpp"
#include "rtos_guard.hpp"
#include "logic/key_rotation.hpp"
#include "logic/rx_framing.hpp"
#include "logic/link_state.hpp"
#include "logic/ble_deferred_queue.hpp"

#include <algorithm>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

static bool fail_next_cpp_allocation = false;
void* operator new(std::size_t size) {
    if (fail_next_cpp_allocation) { fail_next_cpp_allocation = false; throw std::bad_alloc(); }
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

using namespace TeslaBLE;

#include "ble_deferred_test_hooks.hpp"

static uint32_t native_ticks = 0;
static int64_t esp_timer_get_time() { return static_cast<int64_t>(native_ticks) * 1000; }
static unsigned native_errors = 0, native_warnings = 0;
struct NativeClientRef {
    Client* value;
    Client* operator->() const { return value; }
    Client& operator*() const { return *value; }
    Client* get() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};
struct NativeBleSink {
    bool connected{true};
    bool write_ok{true}, submit_then_fail{false};
    unsigned writes{0};
    uint32_t generation{8};
    uint16_t handle{0x22};
    bool ready_ok{true};
    unsigned disconnects{0}, ready_calls{0};
    NativeLifecycleTrace lifecycle_trace;
    std::function<void()> during_generation, during_ready;
    uint32_t lifecycle_generation() {
        native_require_unlocked();
        if (during_generation) during_generation();
        return generation;
    }
    bool complete_ready(uint16_t candidate, uint32_t epoch) {
        native_require_unlocked();
        ++ready_calls;
        lifecycle_trace.push_back('U');
        if (during_ready) during_ready();
        return ready_ok && candidate == handle && epoch == generation && (epoch & 1u) == 0;
    }
    void disconnect() {
        native_require_unlocked();
        ++disconnects;
        connected = false;
        lifecycle_trace.push_back('X');
    }
    std::vector<std::vector<uint8_t>> frames;
    bool write(const std::vector<uint8_t>& frame) {
        native_require_unlocked();
        ++writes;
        if (write_ok || submit_then_fail) frames.push_back(frame);
        return write_ok;
    }
};

struct HostSemaphore { bool available{false}, fail{false}; unsigned gives{0}; };
extern "C" SemaphoreHandle_t xSemaphoreCreateMutex() { return new HostSemaphore{true}; }
extern "C" SemaphoreHandle_t xSemaphoreCreateBinary() { return new HostSemaphore{}; }
extern "C" BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t) {
    native_require_unlocked();
    if (!sem || sem->fail || !sem->available) return pdFALSE;
    sem->available = false; return pdTRUE;
}
extern "C" BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    sem->available = true; ++sem->gives; return pdTRUE;
}
extern "C" void vSemaphoreDelete(SemaphoreHandle_t sem) { delete sem; }

struct NativeSessionStorage {
    bool save_ok{true}, throw_save{false};
    unsigned saves{0};
    std::map<std::string, std::vector<uint8_t>> records;
    std::function<void()> during_save;
    bool save(const std::string& key, const std::vector<uint8_t>& data) {
        native_require_unlocked();
        ++saves;
        if (during_save) during_save();
        if (throw_save) throw std::bad_alloc();
        if (!save_ok) return false;
        records[key] = data;
        return true;
    }
    bool load(const std::string& key, std::vector<uint8_t>& data) {
        native_require_unlocked();
        const auto found = records.find(key);
        if (found == records.end()) return false;
        data = found->second; return true;
    }
};
static time_t native_session_wall_seconds = 1234;
static time_t native_session_time(time_t*) { return native_session_wall_seconds; }

// The real firmware method below owns authentication/counter classification and callback order.
// Hardware/clock/logging and storage/cache/hint sinks are substituted; protocol decisions are not.
class VehicleController {
public:
#include "ble-deferred-types.inc"
    unsigned deferred_mac_persists{0};
    std::function<void()> during_sleep;
    void persist_discovered_mac_after_ready_() {
        native_require_unlocked();
        ++deferred_mac_persists;
        ble.lifecycle_trace.push_back('P');
    }
    NativeClientRef client_;
    tk::CommandRunner command_runner_;
    std::array<std::function<int(Client*, uint8_t*, size_t*)>, tk::CommandRunner::kMaxQueueSize> command_builders_{};
    std::atomic<uint32_t> command_generation_{0};
    std::atomic<int> vcsec_sleep_state_{static_cast<int>(tk::SleepState::Awake)};
    tk::BleUuid ordinary_routing_address_{}, previous_vcsec_reply_address_{};
    bool ordinary_routing_address_ready_{false};
    tk::VcsecReplyContext vcsec_reply_context_;
    uint32_t vcsec_attempt_counter_{0};
    std::vector<uint8_t> tx_buffer_;
    NativeBleSink ble;
    NativeBleSink* ble_{&ble};
    bool identity_ready{true};
    unsigned telemetry_deliveries{0};
    unsigned sleep_deliveries{0}, recovery_deliveries{0};
    NativeSessionStorage session_storage;
    NativeSessionStorage* storage_{&session_storage};
    unsigned& session_persists{session_storage.saves};
    SemaphoreHandle_t vehicle_mutex_{xSemaphoreCreateMutex()};
    std::atomic<uint32_t> identity_epoch_{0}, session_runtime_bits_{0}, session_runtime_epoch_{0};
    std::function<void(const VCSEC_VehicleStatus&)> vehicle_status_callback_;
    explicit VehicleController(Client& client) : client_{&client} {}
    bool ble_connected() const { return ble_ && ble_->connected; }
    bool command_identity_ready_() const { return identity_ready; }
    void note_vcsec_sleep_(tk::SleepState) {
        native_require_unlocked();
        ++sleep_deliveries;
        if (during_sleep) during_sleep();
    }
    void on_vehicle_message_(const UniversalMessage_RoutableMessage&) { ++recovery_deliveries; }
    ~VehicleController() { vSemaphoreDelete(vehicle_mutex_); }
    void mark_vcsec_unknown_() {
        native_require_unlocked();
        vcsec_sleep_state_.store(static_cast<int>(tk::SleepState::Unknown));
        ble.lifecycle_trace.push_back('D');
    }
    bool apply_ble_link_state_(bool);
    bool persist_session_(UniversalMessage_Domain, const UniversalMessage_RoutableMessage_session_info_t&);
    bool save_session_runtime_locked_(UniversalMessage_Domain,
        const UniversalMessage_RoutableMessage_session_info_t&, uint32_t) noexcept;
    void establish_verified_session_locked_(UniversalMessage_Domain, Signatures_SessionInfo&,
        const UniversalMessage_RoutableMessage_session_info_t&);
    bool load_nvs_sessions_();
    tk::SessionRuntimeState runtime_session_observation_locked_(UniversalMessage_Domain) noexcept;
    void publish_session_runtime_locked_(UniversalMessage_Domain, tk::SessionSaveState, uint32_t) noexcept;
    void invalidate_session_runtime_locked_(UniversalMessage_Domain) noexcept;
    tk::SessionAvailability session_availability() const noexcept;
    bool session_available() const noexcept;
    template <typename T> void on_charge_state_(const T&) { native_require_unlocked(); ++telemetry_deliveries; }
    template <typename T> void on_climate_state_(const T&) { native_require_unlocked(); ++telemetry_deliveries; }
    template <typename T> void on_drive_state_(const T&) { native_require_unlocked(); ++telemetry_deliveries; }
    template <typename T> void on_tire_pressure_state_(const T&) { native_require_unlocked(); ++telemetry_deliveries; }
    template <typename T> void on_closures_state_(const T&) { native_require_unlocked(); ++telemetry_deliveries; }
    void handle_carserver_frame_(const UniversalMessage_RoutableMessage& msg);
    void retire_vcsec_contexts_() noexcept;
    bool initialize_routing_address_();
    int build_vcsec_attempt_(tk::VcsecReplyKind kind, uint8_t* buffer, size_t* length);
    bool vcsec_response_matches_(const UniversalMessage_RoutableMessage& msg, tk::VcsecReplyKind kind);
    bool deliver_vcsec_response_(const UniversalMessage_RoutableMessage& msg,
                                 bool success, const char* error = "", bool authenticated_rejection = false);
    void handle_empty_vcsec_reply_(const UniversalMessage_RoutableMessage& msg);
    void handle_vcsec_frame_(const UniversalMessage_RoutableMessage& msg);
    bool response_matches_current_(const UniversalMessage_RoutableMessage& msg);
    bool authenticate_fault_reply_(UniversalMessage_RoutableMessage& msg);
    void handle_signed_message_fault_(const UniversalMessage_RoutableMessage& msg, bool authenticated);
    void handle_session_info_frame_(const UniversalMessage_RoutableMessage& msg);
    void process_rx_frame_(const uint8_t* data, size_t size);
    void drive_command_runner_();
};
#define ESP_LOGE(...) (native_require_unlocked(), ++native_errors)
#define ESP_LOGW(...) (native_require_unlocked(), ++native_warnings)
#define ESP_LOGD(...) (native_require_unlocked())
#define ESP_LOGI(...) (native_require_unlocked())
#define xTaskGetTickCount() (native_ticks)
#define pdTICKS_TO_MS(value) (value)
#include "carserver-handler.inc"
// Only the native routing-address RNG call can be fault-injected. Its normal path delegates
// to the real public library implementation; this does not simulate a PSA entropy failure.
namespace TeslaBLE {
struct NativeRoutingRandom {
    inline static bool fail = false;
    static TeslaBLE_Status_E generate_random_bytes(pb_byte_t* output, size_t length) {
        return fail ? TeslaBLE_Status_E_ERROR_INTERNAL
                    : CryptoUtils::generate_random_bytes(output, length);
    }
};
}
#define CryptoUtils NativeRoutingRandom
#define time native_session_time
#include "vcsec-native.inc"
#define portENTER_CRITICAL(mux) native_mailbox_enter(mux)
#define portEXIT_CRITICAL(mux) native_mailbox_exit(mux)
#include "ble-deferred-native.inc"
#undef portEXIT_CRITICAL
#undef portENTER_CRITICAL
#undef time
#undef CryptoUtils
#undef pdTICKS_TO_MS
#undef xTaskGetTickCount
#undef ESP_LOGI
#undef ESP_LOGE
#undef ESP_LOGW
#undef ESP_LOGD

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

// This facade supplies hardware/storage/cache effects for the native identity cleanup below.
// Client, both Peer objects, CommandRunner and its dispatcher/framer remain the real code.
struct PairingFixtureMutex {
    bool fail{false};
    bool throw_on_take{false};
    unsigned held{0};
};
namespace tk {
class PairingFixtureGuard {
public:
    explicit PairingFixtureGuard(PairingFixtureMutex& mutex) : mutex_(mutex) {
        if (mutex_.throw_on_take) throw std::runtime_error("injected cleanup lock exception");
        held_ = !mutex_.fail;
        if (held_) ++mutex_.held;
    }
    ~PairingFixtureGuard() { if (held_) --mutex_.held; }
    explicit operator bool() const { return held_; }
private:
    PairingFixtureMutex& mutex_;
    bool held_{false};
};
}  // namespace tk

struct PairingFixtureStorage {
    std::map<std::string, bool> present;
    std::string failed_remove;
    std::vector<std::string> removals;
    bool remove(const char* key) {
        removals.emplace_back(key);
        if (failed_remove == key) return false;
        present.erase(key);  // NvsStorage treats an already absent entry as success.
        return true;
    }
};
struct PairingFixtureBle {
    bool connected{false};
    unsigned disconnects{0};
    bool is_connected() const { return connected; }
    void disconnect() { ++disconnects; connected = false; }
};
struct PairingFixtureCache { bool valid{false}; };

class PairingCleanupController {
public:
    Client* client_;
    PairingFixtureStorage storage;
    PairingFixtureStorage* storage_{&storage};
    PairingFixtureBle ble;
    PairingFixtureBle* ble_{&ble};
    PairingFixtureMutex vehicle_mutex_, cache_mutex_;
    tk::CommandRunner command_runner_;
    tk::VcsecReplyContext vcsec_reply_context_;
    std::array<std::function<void()>, tk::CommandRunner::kMaxQueueSize> command_builders_{};
    std::atomic<uint32_t> command_generation_{7}, identity_epoch_{11}, session_runtime_bits_{0xff};
    unsigned telemetry_pending_mux_{0}, telemetry_pending_mask_{31};
    PairingFixtureCache telemetry_pending_charge_{true}, telemetry_pending_climate_{true},
        telemetry_pending_drive_{true}, telemetry_pending_tires_{true}, telemetry_pending_closures_{true},
        charging_amps_feedback_{true}, last_known_charge_{true}, last_known_status_{true},
        last_known_climate_{true}, last_known_drive_{true}, last_known_tires_{true}, last_known_closures_{true};
    std::atomic<uint32_t> last_contact_ticks_{10}, last_charge_ticks_{10}, charge_state_generation_{10},
        last_climate_ticks_{10}, climate_state_generation_{10}, last_reachable_ticks_{10};
    std::atomic<bool> charge_cache_stale_reported_{true}, key_created_untrusted_{false},
        pairing_cleanup_pending_{true}, key_reload_required_{false}, key_runtime_safe_{false},
        pairing_recovery_hint_{true};
    std::atomic<unsigned> auth_fail_streak_{3};
    bool framer_reset_requested{false}, sleep_unknown{false};
    explicit PairingCleanupController(Client& client) : client_(&client) {
        storage.present[tk::kKeyRotationMarker] = true;
    }
    bool has_session() const { return storage.present.count(tk::nvs_contract::kSessionVcsec) != 0; }
    void request_framer_reset_() { framer_reset_requested = true; }
    void mark_vcsec_unknown_() { sleep_unknown = true; }
    bool clear_session_and_cache_();
    bool finish_key_rotation_cleanup_();
    void retire_vcsec_contexts_() noexcept;
};
#define SemGuard PairingFixtureGuard
#define MutexGuard PairingFixtureGuard
#define portENTER_CRITICAL(mux) (++*(mux))
#define portEXIT_CRITICAL(mux) (--*(mux))
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#include "pairing-cleanup.inc"
#undef ESP_LOGI
#undef ESP_LOGE
#undef portEXIT_CRITICAL
#undef portENTER_CRITICAL
#undef MutexGuard
#undef SemGuard

static void check_native_pairing_cleanup() {
    namespace tc = TeslaBLE::TestConstants;
    // No NVS session blobs and no link is the defect trigger. Add durable-session and connected
    // controls, each cleanup erase failure, a journal failure, and the native reset exception path.
    for (unsigned scenario = 0; scenario < 12; ++scenario) {
        Client client;
        client.set_vin(tc::TEST_VIN);
        bool ready = client.load_private_key(kat::client_pem(), kat::client_pem_size()) == 0;
        Signatures_SessionInfo info = Signatures_SessionInfo_init_zero;
        std::memcpy(info.epoch, tc::TEST_EPOCH, sizeof info.epoch);
        std::memcpy(info.publicKey.bytes, tc::EXPECTED_VEHICLE_PUBLIC_KEY, 65);
        info.publicKey.size = 65;
        info.clock_time = 1234;
        for (auto domain : {UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY,
                            UniversalMessage_Domain_DOMAIN_INFOTAINMENT}) {
            auto* peer = client.get_peer(domain);
            ready = ready && peer && peer->update_session(&info) == 0 && peer->is_initialized() &&
                    peer->validate_response_counter(0);
        }
        PairingCleanupController controller(client);
        auto& runner = controller.command_runner_;
        std::array<uint8_t, 16> epoch{};
        std::memcpy(epoch.data(), info.epoch, epoch.size());
        runner.vcsec_session().set_established(epoch, 0, info.clock_time);
        runner.info_session().set_established(epoch, 0, info.clock_time);
        std::function<void(bool, const std::string&, tk::CommandFailureInfo)> on_complete;
        if (scenario == 10) {
            on_complete = [](bool, const std::string&, tk::CommandFailureInfo) {
                throw std::runtime_error("injected command completion exception");
            };
        }
        const auto id = runner.enqueue("old identity action", tk::BleDomain::VehicleSecurity,
                                       tk::WakePolicy::NoWakeFail, 5000, 1, {}, std::move(on_complete));
        if (auto* command = runner.current_command()) {
            command->state = tk::CommandState::Ready;
            command->uuid.fill(0x37);
            runner.notify_tx_complete(tk::TxAction::SendCommandPayload, 2);
        }
        const uint8_t partial[] = {0, 4, 0x12};
        runner.push_rx_chunk(partial, sizeof partial, 2,
                             [](const uint8_t*, size_t) {}, [](const auto&, auto) {});
        controller.command_builders_[id % tk::CommandRunner::kMaxQueueSize] = [] {};
        if (scenario == 1) controller.storage.present[tk::nvs_contract::kSessionVcsec] = true;
        if (scenario == 2) controller.ble.connected = true;
        if (scenario >= 3 && scenario <= 6) {
            const char* failures[] = {tk::nvs_contract::kSessionVcsec,
                tk::nvs_contract::kSessionInfotainment, tk::nvs_contract::kPairedAt,
                tk::kKeyRotationMarker};
            controller.storage.failed_remove = failures[scenario - 3];
        }
        if (scenario == 7) controller.vehicle_mutex_.fail = true;
        if (scenario == 9) controller.vehicle_mutex_.throw_on_take = true;
        if (scenario == 11) controller.storage_ = nullptr;
        if (scenario == 8) {
            controller.key_created_untrusted_.store(true);
            controller.storage.failed_remove = tk::nvs_contract::kKeyCreated;
        }
        // Native key creation changes the shared crypto identity but does not clear the old peers.
        ready = ready && tk::regenerate_private_key(client, [](const auto&) { return true; }) ==
                         tk::KeyRegenerationResult::Committed;
        expect(ready && id != 0 && runner.dispatcher().active_count() == 1 &&
                   runner.rx_framer().buffered_bytes() == sizeof partial,
               "native identity cleanup fixture has two real RAM sessions and pending command/RX",
               "native identity cleanup fixture could not prepare real sessions/command/RX");
        if (!ready) continue;
        const bool complete = controller.finish_key_rotation_cleanup_();
        expect(controller.session_runtime_bits_.load() == 0, "identity cleanup invalidates availability before fallible reset", "identity cleanup retained RAM availability");
        const bool should_complete = scenario < 3;
        const bool reset_failed = scenario == 7 || scenario == 9 || scenario == 10;
        auto* vcsec = client.get_peer(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY);
        auto* infotainment = client.get_peer(UniversalMessage_Domain_DOMAIN_INFOTAINMENT);
        expect(complete == should_complete && controller.key_runtime_safe_.load() == should_complete &&
                   controller.pairing_cleanup_pending_.load() == !should_complete &&
                   controller.storage.present.count(tk::kKeyRotationMarker) == !should_complete,
               "cleanup retires the journal and reopens signing only after complete native cleanup",
               "cleanup retired the journal or reopened signing after incomplete cleanup");
        expect(controller.command_generation_.load() == 8 && controller.identity_epoch_.load() == 12 &&
                   controller.ble.disconnects == 1 && controller.vehicle_mutex_.held == 0 &&
                   controller.cache_mutex_.held == 0 && controller.telemetry_pending_mux_ == 0,
               "native cleanup advances identity generations and releases every lock",
               "native cleanup retained an identity generation or lock");
        expect(reset_failed || (!vcsec->is_valid() && !infotainment->is_valid() &&
                   !vcsec->has_shared_secret() && !infotainment->has_shared_secret() &&
                   !vcsec->has_valid_epoch() && !infotainment->has_valid_epoch() &&
                   vcsec->get_counter() == 0 && infotainment->get_counter() == 0 &&
                   vcsec->validate_response_counter(0) && infotainment->validate_response_counter(0) &&
                   !runner.vcsec_session().is_authenticated() && !runner.info_session().is_authenticated() &&
                   runner.current_command() == nullptr && runner.dispatcher().active_count() == 0 &&
                   controller.framer_reset_requested &&
                   !controller.command_builders_[id % tk::CommandRunner::kMaxQueueSize]),
               "RAM-only sessions and pending old-identity commands are invalidated",
               "RAM-only session or old-identity command survived native cleanup");
        expect(runner.rx_framer().buffered_bytes() == sizeof partial,
               "external cleanup defers RX-buffer mutation to its loop owner",
               "external cleanup mutated the loop-owned RX buffer");
        expect(!controller.last_known_charge_.valid && !controller.last_known_status_.valid &&
                   !controller.last_known_climate_.valid && !controller.last_known_drive_.valid &&
                   !controller.last_known_tires_.valid && !controller.last_known_closures_.valid &&
                   !controller.telemetry_pending_charge_.valid && !controller.telemetry_pending_climate_.valid &&
                   !controller.telemetry_pending_drive_.valid && !controller.telemetry_pending_tires_.valid &&
                   !controller.telemetry_pending_closures_.valid && !controller.charging_amps_feedback_.valid &&
                   controller.telemetry_pending_mask_ == 0 && controller.sleep_unknown &&
                   controller.last_contact_ticks_ == 0 && controller.last_reachable_ticks_ == 0,
               "native cleanup discards old-identity cached and pending telemetry",
               "native cleanup retained old-identity telemetry");
        if (should_complete) {
            expect(!controller.storage.removals.empty() &&
                       controller.storage.removals.back() == tk::kKeyRotationMarker &&
                       controller.finish_key_rotation_cleanup_(),
                   "native journal retirement is last and cleanup is idempotent",
                   "native journal retirement preceded cleanup or repeat cleanup failed");
        } else {
            controller.vehicle_mutex_.fail = false;
            controller.vehicle_mutex_.throw_on_take = false;
            controller.storage.failed_remove.clear();
            controller.storage_ = &controller.storage;
            expect(controller.finish_key_rotation_cleanup_() && !vcsec->is_valid() && !infotainment->is_valid(),
                   "a later successful cleanup completes the retained journal safely",
                   "cleanup could not recover safely after its injected failure");
        }
    }
    Client fresh;
    PairingCleanupController first_boot(fresh);
    expect(first_boot.finish_key_rotation_cleanup_() && !first_boot.has_session() &&
               first_boot.command_generation_ == 8 && first_boot.framer_reset_requested &&
               first_boot.key_runtime_safe_ && !first_boot.pairing_cleanup_pending_,
           "first-boot cleanup succeeds with no previous link, key, session or command",
           "first-boot empty-state cleanup failed");
}

static void check_native_carserver_replay() {
    namespace tc = TeslaBLE::TestConstants;
    Client client;
    client.set_vin(tc::TEST_VIN);
    bool ready = client.load_private_key(kat::client_pem(), kat::client_pem_size()) == 0;
    Signatures_SessionInfo info = Signatures_SessionInfo_init_zero;
    std::memcpy(info.epoch, tc::TEST_EPOCH, sizeof info.epoch);
    std::memcpy(info.publicKey.bytes, tc::EXPECTED_VEHICLE_PUBLIC_KEY, 65);
    info.publicKey.size = 65;
    info.clock_time = 1234;
    info.counter = 7;
    auto* peer = client.get_peer(UniversalMessage_Domain_DOMAIN_INFOTAINMENT);
    ready = ready && peer && peer->update_session(&info) == 0 && peer->is_initialized();
    uint8_t wire[2048];
    size_t wire_size = sizeof wire;
    ready = ready && client.build_car_server_get_vehicle_data_message(
        wire, &wire_size, CarServer_GetVehicleData_getChargeState_tag) == 0;
    tk::BleUuid uuid{};
    size_t uuid_size = uuid.size();
    ready = ready && client.get_last_request_uuid(
        UniversalMessage_Domain_DOMAIN_INFOTAINMENT, uuid.data(), &uuid_size) && uuid_size == uuid.size();
    size_t request_hash_size = 0;
    const auto* request_hash = client.get_last_request_hash(
        UniversalMessage_Domain_DOMAIN_INFOTAINMENT, &request_hash_size);
    ready = ready && request_hash && request_hash_size == 17;
    UniversalMessage_RoutableMessage plain = UniversalMessage_RoutableMessage_init_default;
    ready = ready && car_side_decode(make_carserver_frame(uuid.data()), client, plain);
    expect(ready, "native CarServer adapter fixture has a real signed request and session",
           "native CarServer adapter fixture could not prepare a real request");
    if (!ready) return;

    const auto& payload = plain.payload.protobuf_message_as_bytes;
    const std::vector<uint8_t> plaintext(payload.bytes, payload.bytes + payload.size);
    const std::string request_hash_hex = kat::hex(request_hash, request_hash_size);
    auto seal = [&](uint32_t counter) {
        auto message = plain;
        message.flags = 1;
        message.which_sub_sigData = UniversalMessage_RoutableMessage_signature_data_tag;
        auto& signature = message.sub_sigData.signature_data;
        signature.which_sig_type = Signatures_SignatureData_AES_GCM_Response_data_tag;
        auto& encrypted = signature.sig_type.AES_GCM_Response_data;
        encrypted.counter = counter;
        char counter_hex[9], fault_hex[9];
        std::snprintf(fault_hex, sizeof fault_hex, "%08x", static_cast<unsigned>(
            message.has_signedMessageStatus ? message.signedMessageStatus.signed_message_fault : 0));
        std::snprintf(counter_hex, sizeof counter_hex, "%08x", static_cast<unsigned>(counter));
        // Independent vehicle-side metadata from protocol.md. Counter zero remains an authenticated
        // AES-GCM response; the signature variant determines that property, never its integer value.
        const std::string metadata_hex = std::string("0001090101030211") +
            kat::hex(reinterpret_cast<const uint8_t*>(tc::TEST_VIN), std::strlen(tc::TEST_VIN)) +
            "0504" + counter_hex + "0704000000010811" + request_hash_hex + "090400000000ff";
        const auto metadata = kat::unhex(metadata_hex.c_str());
        const auto nonce = kat::unhex("0102030405060708090a0b0c");
        const auto ciphertext = kat::gcm_encrypt(tc::EXPECTED_SESSION_KEY, nonce, kat::sha256(metadata), plaintext);
        if (ciphertext.size() != plaintext.size() + 16) {
            expect(false, "", "vehicle-side response sealing failed");
            return UniversalMessage_RoutableMessage{};
        }
        std::memcpy(encrypted.nonce, nonce.data(), nonce.size());
        std::memcpy(encrypted.tag, ciphertext.data() + plaintext.size(), 16);
        auto& output = message.payload.protobuf_message_as_bytes;
        output.size = plaintext.size();
        std::memcpy(output.bytes, ciphertext.data(), plaintext.size());
        return message;
    };

    VehicleController controller(client);
    const auto request_id = controller.command_runner_.dispatcher().register_request(tk::BleDomain::Infotainment, uuid);
    expect(request_id != 0, "native adapter request is registered", "native adapter request registration failed");
    auto zero = seal(0);
    auto foreign_zero = zero;
    foreign_zero.request_uuid.bytes[0] ^= 1;
    controller.handle_carserver_frame_(foreign_zero);
    expect(controller.telemetry_deliveries == 0, "foreign zero drops before native telemetry",
           "foreign zero delivered native telemetry");
    auto bad_tag = zero;
    bad_tag.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.tag[0] ^= 1;
    controller.handle_carserver_frame_(bad_tag);
    expect(controller.telemetry_deliveries == 0, "bad tag produces no native telemetry",
           "bad tag reached native telemetry");
    controller.handle_carserver_frame_(zero);
    expect(controller.telemetry_deliveries == 1, "authenticated zero is accepted after foreign UUID and bad tag",
           "authenticated zero was rejected or rejected input consumed replay state");
    controller.handle_carserver_frame_(zero);
    expect(controller.telemetry_deliveries == 1, "repeated authenticated zero is dropped",
           "repeated authenticated zero delivered telemetry");
    controller.handle_carserver_frame_(seal(1));
    expect(controller.telemetry_deliveries == 2, "nonzero multipart response is retained",
           "nonzero multipart response was dropped");
    auto foreign = seal(2);
    foreign.request_uuid.bytes[0] ^= 1;
    controller.handle_carserver_frame_(foreign);
    expect(controller.telemetry_deliveries == 2, "foreign UUID drops before native telemetry",
           "foreign UUID delivered native telemetry");
    controller.handle_carserver_frame_(seal(2));
    expect(controller.telemetry_deliveries == 3, "foreign UUID did not consume the authentic response counter",
           "foreign UUID poisoned the peer replay window");
    controller.handle_carserver_frame_(plain);
    expect(controller.telemetry_deliveries == 4, "plaintext keeps its separate one-response rule",
           "authenticated zero was misclassified as plaintext");
    controller.handle_carserver_frame_(plain);
    expect(controller.telemetry_deliveries == 4, "repeated plaintext is dropped",
           "repeated plaintext delivered native telemetry");
    controller.handle_carserver_frame_(seal(4));
    controller.handle_carserver_frame_(seal(3));
    expect(controller.telemetry_deliveries == 6, "fresh out-of-order authenticated replies are accepted",
           "fresh out-of-order authenticated replies were dropped");
    expect(!peer->validate_response_counter(0), "native adapter recorded zero in the real peer replay window",
           "native adapter bypassed the real peer replay window for zero");
    const auto replay = controller.command_runner_.dispatcher().dispatch(
        tk::BleDomain::Infotainment, uuid.data(), uuid.size(), true, 0);
    expect(!replay.routed && replay.drop_reason == tk::DispatchDropReason::Replay,
           "native adapter recorded zero in the per-request authenticated window",
           "native adapter bypassed the per-request authenticated window for zero");
}

namespace vcsec_native {
using Message = UniversalMessage_RoutableMessage;
using Kind = tk::VcsecReplyKind;
namespace tc = TeslaBLE::TestConstants;
static_assert(UniversalMessage_RoutableMessage_to_destination_tag == 6 &&
              UniversalMessage_RoutableMessage_from_destination_tag == 7 &&
              UniversalMessage_RoutableMessage_protobuf_message_as_bytes_tag == 10 &&
              UniversalMessage_RoutableMessage_session_info_request_tag == 14 &&
              UniversalMessage_RoutableMessage_session_info_tag == 15 &&
              UniversalMessage_RoutableMessage_request_uuid_tag == 50 &&
              UniversalMessage_RoutableMessage_uuid_tag == 51 &&
              UniversalMessage_Destination_domain_tag == 1 &&
              UniversalMessage_Destination_routing_address_tag == 2 &&
              UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY == 2,
              "bounded request scanner must match the pinned generated protocol tags");

static void send(VehicleController& controller, const Message& message) {
    uint8_t bytes[2048];
    auto out = pb_ostream_from_buffer(bytes, sizeof bytes);
    const bool encoded = pb_encode(&out, UniversalMessage_RoutableMessage_fields, &message);
    expect(encoded, "vehicle response encodes with actual Nanopb", "vehicle response could not encode");
    if (encoded) controller.process_rx_frame_(bytes, out.bytes_written);
}
static Message envelope(const tk::VcsecRequestMetadata& request, bool with_uuid = false) {
    Message reply = UniversalMessage_RoutableMessage_init_default;
    reply.has_from_destination = true;
    reply.from_destination.which_sub_destination = UniversalMessage_Destination_domain_tag;
    reply.from_destination.sub_destination.domain = UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY;
    reply.has_to_destination = true;
    reply.to_destination.which_sub_destination = UniversalMessage_Destination_routing_address_tag;
    auto& address = reply.to_destination.sub_destination.routing_address;
    address.size = request.reply_address.size();
    std::memcpy(address.bytes, request.reply_address.data(), address.size);
    if (with_uuid) {
        reply.request_uuid.size = request.uuid.size();
        std::memcpy(reply.request_uuid.bytes, request.uuid.data(), request.uuid.size());
    }
    return reply;
}
static Message payload(Message message, const VCSEC_FromVCSECMessage& vcsec) {
    message.which_payload = UniversalMessage_RoutableMessage_protobuf_message_as_bytes_tag;
    auto& bytes = message.payload.protobuf_message_as_bytes;
    auto out = pb_ostream_from_buffer(bytes.bytes, sizeof bytes.bytes);
    expect(pb_encode(&out, VCSEC_FromVCSECMessage_fields, &vcsec),
           "VCSEC fixture encodes", "VCSEC fixture could not encode");
    bytes.size = out.bytes_written;
    return message;
}
static Message status(Message message) {
    VCSEC_FromVCSECMessage vcsec = VCSEC_FromVCSECMessage_init_default;
    vcsec.which_sub_message = VCSEC_FromVCSECMessage_vehicleStatus_tag;
    auto& sample = vcsec.sub_message.vehicleStatus;
    sample.vehicleSleepStatus = VCSEC_VehicleSleepStatus_E_VEHICLE_SLEEP_STATUS_AWAKE;
    sample.has_closureStatuses = true;
    return payload(message, vcsec);
}
static Message command_status(Message message, VCSEC_OperationStatus_E operation) {
    VCSEC_FromVCSECMessage vcsec = VCSEC_FromVCSECMessage_init_default;
    vcsec.which_sub_message = VCSEC_FromVCSECMessage_commandStatus_tag;
    vcsec.sub_message.commandStatus.operationStatus = operation;
    return payload(message, vcsec);
}
static Message session_info(Message message, const tk::BleUuid& challenge) {
    const auto bytes = kat::unhex(kat::kSessionInfo);
    message.which_payload = UniversalMessage_RoutableMessage_session_info_tag;
    message.payload.session_info.size = bytes.size();
    std::memcpy(message.payload.session_info.bytes, bytes.data(), bytes.size());
    message.which_sub_sigData = UniversalMessage_RoutableMessage_signature_data_tag;
    auto& signature = message.sub_sigData.signature_data;
    signature.which_sig_type = Signatures_SignatureData_session_info_tag_tag;
    // Independent HMAC fixture: reference TLVs, official derived key, and PSA one-shot MAC.
    auto input = kat::unhex((std::string("0001060211") +
        kat::hex(reinterpret_cast<const uint8_t*>(tc::TEST_VIN), std::strlen(tc::TEST_VIN)) +
        "0610" + kat::hex(challenge.data(), challenge.size()) + "ff").c_str());
    input.insert(input.end(), bytes.begin(), bytes.end());
    const auto key = kat::unhex(kat::kSessionInfoKey);
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attrs, 256);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attrs, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    size_t length = 0;
    auto& tag = signature.sig_type.session_info_tag.tag;
    const bool sealed = psa_import_key(&attrs, key.data(), key.size(), &id) == PSA_SUCCESS &&
        psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), input.data(), input.size(),
                        tag.bytes, sizeof tag.bytes, &length) == PSA_SUCCESS;
    psa_destroy_key(id);
    psa_reset_key_attributes(&attrs);
    tag.size = length;
    expect(sealed && length == 32, "independent SessionInfo HMAC seals", "SessionInfo HMAC sealing failed");
    return message;
}
struct Fixture {
    Client client;
    VehicleController controller{client};
    unsigned completions{0}, samples{0};
    bool success{false};
    tk::CommandFailureInfo last_failure{};
    std::string last_reason;
    Fixture() {
        native_ticks = 100;
        client.set_vin(tc::TEST_VIN);
        bool ready = client.load_private_key(kat::client_pem(), kat::client_pem_size()) == 0;
        Signatures_SessionInfo info = Signatures_SessionInfo_init_zero;
        std::memcpy(info.epoch, tc::TEST_EPOCH, 16);
        std::memcpy(info.publicKey.bytes, tc::EXPECTED_VEHICLE_PUBLIC_KEY, 65);
        info.publicKey.size = 65;
        info.clock_time = 1234;
        info.counter = 7;
        for (auto domain : {UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY,
                            UniversalMessage_Domain_DOMAIN_INFOTAINMENT}) {
            auto* peer = client.get_peer(domain);
            ready = ready && peer && peer->update_session(&info) == 0 && peer->is_initialized();
        }
        std::array<uint8_t, 16> epoch{};
        std::memcpy(epoch.data(), tc::TEST_EPOCH, 16);
        controller.command_runner_.vcsec_session().set_established(epoch, 7, 1234);
        controller.command_runner_.info_session().set_established(epoch, 7, 1234);
        controller.vehicle_status_callback_ = [&](const VCSEC_VehicleStatus&) { ++samples; };
        expect(ready, "native VCSEC fixture has actual authenticated peers", "native VCSEC fixture setup failed");
    }
    uint32_t enqueue(Kind kind = Kind::Action, const char* name = "Action", bool tx_only = false,
                     tk::BleDomain domain = tk::BleDomain::VehicleSecurity,
                     tk::CommandEffect effect = tk::CommandEffect::Mutating) {
        auto& runner = controller.command_runner_;
        const auto id = runner.enqueue(name, domain, tk::WakePolicy::WakeIfNeeded, 60000, native_ticks,
            {}, [&](bool ok, const std::string& reason, tk::CommandFailureInfo failure) {
                ++completions; success = ok; last_failure = failure; last_reason = reason;
            }, tx_only, 3, kind);
        runner.set_command_effect(id, kind == Kind::Status ? tk::CommandEffect::ReadOnly : effect);
        controller.command_builders_[id % tk::CommandRunner::kMaxQueueSize] =
            [kind](Client* client, uint8_t* bytes, size_t* length) {
                return kind == Kind::Status ? client->build_vcsec_information_request_message(
                    VCSEC_InformationRequestType_INFORMATION_REQUEST_TYPE_GET_STATUS, bytes, length)
                    : client->build_vcsec_action_message(VCSEC_RKEAction_E_RKE_ACTION_LOCK, bytes, length);
            };
        return id;
    }
    tk::CommandRequest* command() { return controller.command_runner_.current_command(); }
    tk::VcsecRequestMetadata request() {
        tk::VcsecRequestMetadata request;
        const auto& frames = controller.ble.frames;
        const bool captured = !frames.empty() && tk::capture_vcsec_request(
            frames.back().data(), frames.back().size(), request);
        expect(captured, "actual transmitted VCSEC frame has bounded metadata", "actual VCSEC capture failed");
        return request;
    }
    Message seal(Message message, uint32_t counter,
                 UniversalMessage_Domain domain = UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY) {
        const auto& bytes = message.payload.protobuf_message_as_bytes;
        const bool absent = message.which_payload == 0;
        const std::vector<uint8_t> plain = absent ? std::vector<uint8_t>{}
            : std::vector<uint8_t>(bytes.bytes, bytes.bytes + bytes.size);
        size_t hash_size = 0;
        const auto* hash = client.get_last_request_hash(domain, &hash_size);
        char counter_hex[9], fault_hex[9];
        std::snprintf(fault_hex, sizeof fault_hex, "%08x", static_cast<unsigned>(
            message.has_signedMessageStatus ? message.signedMessageStatus.signed_message_fault : 0));
        std::snprintf(counter_hex, sizeof counter_hex, "%08x", static_cast<unsigned>(counter));
        const auto metadata = kat::unhex((std::string(domain == UniversalMessage_Domain_DOMAIN_INFOTAINMENT
                ? "0001090101030211" : "0001090101020211") +
            kat::hex(reinterpret_cast<const uint8_t*>(tc::TEST_VIN), std::strlen(tc::TEST_VIN)) +
            "0504" + counter_hex + "0704000000010811" + kat::hex(hash, hash_size) + "0904" + fault_hex + "ff").c_str());
        const auto nonce = kat::unhex("0102030405060708090a0b0c");
        const auto encrypted = kat::gcm_encrypt(tc::EXPECTED_SESSION_KEY, nonce, kat::sha256(metadata), plain);
        expect(hash && hash_size == 17 && encrypted.size() == plain.size() + 16,
               "independent VCSEC GCM response seals", "VCSEC GCM fixture sealing failed");
        if (encrypted.size() != plain.size() + 16) return {};
        message.flags = 1;
        message.which_sub_sigData = UniversalMessage_RoutableMessage_signature_data_tag;
        auto& signature = message.sub_sigData.signature_data;
        signature.which_sig_type = Signatures_SignatureData_AES_GCM_Response_data_tag;
        auto& gcm = signature.sig_type.AES_GCM_Response_data;
        gcm.counter = counter;
        std::memcpy(gcm.nonce, nonce.data(), 12);
        std::memcpy(gcm.tag, encrypted.data() + plain.size(), 16);
        if (!absent) std::memcpy(message.payload.protobuf_message_as_bytes.bytes, encrypted.data(), plain.size());
        return message;
    }
};
}  // namespace vcsec_native


// All numeric classifications are pinned to the generated, actually linked protocol enum.
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_NONE == 0);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_BUSY == 1);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_TIMEOUT == 2);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_UNKNOWN_KEY_ID == 3);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INACTIVE_KEY == 4);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INVALID_SIGNATURE == 5);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INVALID_TOKEN_OR_COUNTER == 6);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INSUFFICIENT_PRIVILEGES == 7);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INVALID_DOMAINS == 8);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INVALID_COMMAND == 9);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_DECODING == 10);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INTERNAL == 11);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_WRONG_PERSONALIZATION == 12);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_BAD_PARAMETER == 13);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_KEYCHAIN_IS_FULL == 14);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INCORRECT_EPOCH == 15);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_IV_INCORRECT_LENGTH == 16);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_TIME_EXPIRED == 17);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_NOT_PROVISIONED_WITH_IDENTITY == 18);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_COULD_NOT_HASH_METADATA == 19);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_TIME_TO_LIVE_TOO_LONG == 20);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_REMOTE_ACCESS_DISABLED == 21);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_REMOTE_SERVICE_ACCESS_DISABLED == 22);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_COMMAND_REQUIRES_ACCOUNT_CREDENTIALS == 23);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_REQUEST_MTU_EXCEEDED == 24);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_RESPONSE_MTU_EXCEEDED == 25);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_REPEATED_COUNTER == 26);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INVALID_KEY_HANDLE == 27);
static_assert(UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_REQUIRES_RESPONSE_ENCRYPTION == 28);
#include "command_completion_fixture.hpp"

static void check_native_command_faults() {
    using namespace vcsec_native;
    const auto fault = [](Message message, unsigned code) {
        message.has_signedMessageStatus = true;
        message.signedMessageStatus.operation_status = UniversalMessage_OperationStatus_E_OPERATIONSTATUS_ERROR;
        message.signedMessageStatus.signed_message_fault = static_cast<UniversalMessage_MessageFault_E>(code);
        return message;
    };
    for (unsigned code = 0; code < 30; ++code) {
        Fixture f; f.enqueue(); f.controller.drive_command_runner_(); const auto request = f.request();
        // Nonempty payload exercises in-place PSA GCM decryption, not only empty tags.
        auto response = f.seal(fault(status(envelope(request, true)), code), 0);
        auto foreign = response; foreign.to_destination.sub_destination.routing_address.bytes[0] ^= 1;
        auto wrong_uuid = response; wrong_uuid.request_uuid.bytes[0] ^= 1;
        auto bad_tag = response; bad_tag.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.tag[0] ^= 1;
        auto altered_fault = response;
        altered_fault.signedMessageStatus.signed_message_fault = static_cast<UniversalMessage_MessageFault_E>((code + 1) % 29);
        send(f.controller, foreign); send(f.controller, wrong_uuid); send(f.controller, bad_tag); send(f.controller, altered_fault);
        auto* peer = f.client.get_peer(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY);
        expect(f.completions == 0 && f.controller.recovery_deliveries == 0 && peer->is_initialized() &&
               f.command()->retry_count == 0 && f.command()->payload_maybe_sent,
               "foreign, bad-tag and AAD fault mutation cannot reset, complete or retry", "invalid fault changed command or peer");
        send(f.controller, response);
        // Expected sets come from the pinned vehicle-command reference, independent of the classifier.
        const bool retry = code == 1 || code == 2 || code == 5 || code == 6 || code == 11 || code == 15 || code == 17 || code == 20;
        const bool uncertain = code == 0 || code == 25 || code == 29;
        expect(retry ? (f.completions == 0 && f.command()->retry_count == 1 && !f.command()->payload_maybe_sent)
                     : (f.completions == 1 && !f.success && tk::execution_uncertain(f.last_failure) == uncertain &&
                        f.last_failure.severity == ((code == 0 || code == 29) ? tk::CommandError::Severity::Unknown
                                                                              : tk::CommandError::Severity::Permanent)),
               "actual authenticated numeric fault retains reference retry and possible-success classification",
               "authenticated numeric fault classification lost at native boundary");
        const auto recovery = f.controller.recovery_deliveries;
        send(f.controller, response);
        expect(f.controller.recovery_deliveries == recovery && f.controller.ble.writes == 1,
               "replayed fault cannot signal twice or restart transport", "fault replay changed retry or completion");
        if (retry) {
            native_ticks += 999; f.controller.drive_command_runner_();
            expect(f.controller.ble.writes == 1, "authenticated rejection honors BLE retry delay", "authenticated rejection retried too early");
        } else {
            for (unsigned i = 0; i < 600; ++i) { native_ticks += 1000; f.controller.drive_command_runner_(); }
            expect(f.controller.ble.writes == 1 && f.completions == 1,
                   "terminal fault stays quiescent for ten minutes", "terminal fault sent an extra command");
        }
    }
    {
        Fixture f; f.enqueue(); f.controller.drive_command_runner_(); const auto old = f.request();
        send(f.controller, f.seal(fault(envelope(old), 1), 0));
        native_ticks += 1000; f.controller.drive_command_runner_(); const auto fresh = f.request();
        expect(f.controller.ble.writes == 2 && old.uuid != fresh.uuid && old.reply_address != fresh.reply_address,
               "proven unexecuted BUSY allows a fresh bounded attempt", "safe BUSY retry did not get fresh metadata");
        send(f.controller, envelope(old));
        expect(f.completions == 0, "safe retry rejects retired reply", "retired fault attempt completed retry");
        send(f.controller, f.seal(envelope(fresh), 1));
        expect(f.completions == 1 && f.success, "safe retry completes on its actual authenticated response", "fresh retry did not complete");
    }
    for (unsigned variant = 0; variant < 8; ++variant) {
        Fixture f; f.enqueue(); f.controller.drive_command_runner_(); const auto request = f.request();
        if (variant == 0) send(f.controller, fault(envelope(request), 1));
        if (variant == 1) send(f.controller, fault(session_info(envelope(request), request.uuid), 15));
        if (variant == 2) { native_ticks += tk::CommandRunner::kDefaultResponseTimeoutMs; f.controller.drive_command_runner_(); }
        if (variant == 3) { f.controller.ble.connected = false; f.controller.command_runner_.notify_link_lost(); }
        if (variant == 4) f.controller.command_runner_.cancel_current();
        if (variant == 5) {
            auto wait = f.seal(fault(envelope(request), 0), 0);
            wait.signedMessageStatus.operation_status = UniversalMessage_OperationStatus_E_OPERATIONSTATUS_WAIT;
            send(f.controller, wait);
            expect(f.completions == 0 && f.command()->payload_maybe_sent,
                   "outer WAIT does not erase uncertainty or reserialize", "outer WAIT granted a retry");
            native_ticks += tk::CommandRunner::kDefaultResponseTimeoutMs; f.controller.drive_command_runner_();
        }
        if (variant == 6) send(f.controller, f.seal(fault(envelope(request), 0), 0));
        if (variant == 7) {
            auto unknown = f.seal(fault(envelope(request), 0), 0);
            unknown.signedMessageStatus.operation_status = static_cast<UniversalMessage_OperationStatus_E>(99);
            send(f.controller, unknown);
        }
        const auto* peer = f.client.get_peer(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY);
        expect(f.completions == 1 && !f.success && tk::execution_uncertain(f.last_failure) &&
               f.last_reason.find(tk::uncertain_command_reason()) == 0 && peer->is_initialized(),
               "uncertain timeout, disconnect, cancellation and unauthenticated faults preserve typed and visible outcome",
               "uncertain command became definite or reset peer");
        for (unsigned i = 0; i < 600; ++i) { native_ticks += 1000; f.controller.drive_command_runner_(); }
        expect(f.controller.ble.writes == 1 && f.completions == 1,
               "uncertain mutating command never serializes another attempt", "uncertain command automatically resent");
    }
    {
        Fixture f; f.enqueue(); f.controller.drive_command_runner_(); const auto request = f.request();
        const auto rejection = f.seal(fault(envelope(request, true), 7), 0);
        uint8_t wire[2048]; auto stream = pb_ostream_from_buffer(wire, sizeof wire);
        expect(pb_encode(&stream, UniversalMessage_RoutableMessage_fields, &rejection),
               "permanent rejection encodes before allocation injection", "cannot encode OOM control");
        bool threw = false;
        fail_next_cpp_allocation = true;
        try { f.controller.process_rx_frame_(wire, stream.bytes_written); } catch (const std::bad_alloc&) { threw = true; }
        fail_next_cpp_allocation = false;
        expect(threw && f.command()->is_completed && f.command()->state == tk::CommandState::Failed &&
               f.command()->failure.severity == tk::CommandError::Severity::Permanent &&
               !tk::execution_uncertain(f.command()->failure),
               "typed permanent outcome is terminal before fallible error text", "allocation failure lost terminal rejection");
        for (unsigned i = 0; i < 600; ++i) { native_ticks += 1000; f.controller.drive_command_runner_(); }
        expect(f.controller.ble.writes == 1, "allocation failure after rejection cannot reserialize", "OOM retried permanently rejected command");
    }
    for (bool authenticated : {false, true}) {
        Fixture f; f.enqueue(); f.controller.drive_command_runner_(); const auto request = f.request();
        VCSEC_FromVCSECMessage nominal = VCSEC_FromVCSECMessage_init_default;
        nominal.which_sub_message = VCSEC_FromVCSECMessage_nominalError_tag;
        nominal.sub_message.nominalError.genericError = Errors_GenericError_E_GENERICERROR_UNAUTHORIZED;
        auto rejection = payload(envelope(request, true), nominal);
        if (authenticated) {
            rejection = f.seal(rejection, 0);
            auto bad = rejection; bad.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.tag[0] ^= 1;
            send(f.controller, bad);
            expect(f.completions == 0 && f.command()->payload_maybe_sent,
                   "bad-tag nominal rejection cannot erase submission uncertainty", "bad nominal rejection changed outcome");
        }
        send(f.controller, rejection);
        expect(f.completions == 1 && !f.success && tk::execution_uncertain(f.last_failure) == !authenticated &&
               (!authenticated || (f.last_failure.severity == tk::CommandError::Severity::Permanent &&
                f.last_reason == "VCSEC command rejected")),
               "only authenticated known nominal rejection is permanent and definite", "nominal rejection outcome was flattened");
    }
    for (bool authenticated : {false, true}) {
        Fixture f;
        const auto id = f.enqueue(Kind::Action, "Info", false, tk::BleDomain::Infotainment);
        f.controller.command_builders_[id % tk::CommandRunner::kMaxQueueSize] =
            [](Client* c, uint8_t* b, size_t* n) { return c->build_car_server_get_vehicle_data_message(
                b, n, CarServer_GetVehicleData_getChargeState_tag); };
        f.controller.drive_command_runner_();
        Message outgoing = UniversalMessage_RoutableMessage_init_default;
        expect(car_side_decode(f.controller.ble.frames.back(), f.client, outgoing),
               "actual Info request decodes for inner rejection control", "Info control did not serialize");
        tk::VcsecRequestMetadata metadata; metadata.reply_address = f.controller.ordinary_routing_address_;
        std::memcpy(metadata.uuid.data(), outgoing.uuid.bytes, 16);
        auto rejection = envelope(metadata, true);
        rejection.from_destination.sub_destination.domain = UniversalMessage_Domain_DOMAIN_INFOTAINMENT;
        rejection.which_payload = UniversalMessage_RoutableMessage_protobuf_message_as_bytes_tag;
        CarServer_Response inner = CarServer_Response_init_default;
        inner.has_actionStatus = true; inner.actionStatus.result = CarServer_OperationStatus_E_OPERATIONSTATUS_ERROR;
        auto& bytes = rejection.payload.protobuf_message_as_bytes;
        auto stream = pb_ostream_from_buffer(bytes.bytes, sizeof bytes.bytes);
        expect(pb_encode(&stream, CarServer_Response_fields, &inner), "inner CarServer rejection encodes", "CarServer rejection did not encode");
        bytes.size = stream.bytes_written;
        if (authenticated) {
            rejection = f.seal(rejection, 0, UniversalMessage_Domain_DOMAIN_INFOTAINMENT);
            auto bad = rejection; bad.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.tag[0] ^= 1;
            send(f.controller, bad);
            expect(f.completions == 0 && f.command()->payload_maybe_sent,
                   "bad-tag action rejection leaves submitted outcome uncertain", "bad CarServer tag changed outcome");
        }
        send(f.controller, rejection);
        expect(f.completions == 1 && !f.success && tk::execution_uncertain(f.last_failure) == !authenticated &&
               (!authenticated || (f.last_failure.severity == tk::CommandError::Severity::Permanent &&
                f.last_reason == "Infotainment action failed")),
               "authenticated inner CarServer rejection retains definite failure", "CarServer rejection lost typed outcome");
    }
    for (unsigned variant = 0; variant < 3; ++variant) {
        Fixture f; f.enqueue(); f.controller.drive_command_runner_(); const auto request = f.request();
        VCSEC_FromVCSECMessage inner = VCSEC_FromVCSECMessage_init_default;
        if (variant == 0) {
            inner.which_sub_message = VCSEC_FromVCSECMessage_nominalError_tag;
            inner.sub_message.nominalError.genericError = static_cast<Errors_GenericError_E>(8);
        } else {
            inner.which_sub_message = VCSEC_FromVCSECMessage_commandStatus_tag;
            inner.sub_message.commandStatus.operationStatus = variant == 1
                ? static_cast<VCSEC_OperationStatus_E>(99) : VCSEC_OperationStatus_E_OPERATIONSTATUS_ERROR;
        }
        send(f.controller, f.seal(payload(envelope(request, true), inner), 0));
        expect(f.completions == 1 && !f.success && tk::execution_uncertain(f.last_failure) == (variant != 2),
               "unknown inner VCSEC values retain uncertainty; known encrypted ERROR is definite",
               "unknown VCSEC inner value cleared uncertainty");
    }
    for (bool unknown_inner : {false, true}) {
        Fixture f;
        const auto id = f.enqueue(Kind::Action, "Info", false, tk::BleDomain::Infotainment);
        f.controller.command_builders_[id % tk::CommandRunner::kMaxQueueSize] =
            [](Client* c, uint8_t* b, size_t* n) { return c->build_car_server_get_vehicle_data_message(
                b, n, CarServer_GetVehicleData_getChargeState_tag); };
        f.controller.drive_command_runner_();
        Message outgoing = UniversalMessage_RoutableMessage_init_default;
        expect(car_side_decode(f.controller.ble.frames.back(), f.client, outgoing), "Info fault control serializes", "Info fault control missing TX");
        tk::VcsecRequestMetadata request; request.reply_address = f.controller.ordinary_routing_address_;
        std::memcpy(request.uuid.data(), outgoing.uuid.bytes, 16);
        auto reply = envelope(request, true);
        reply.from_destination.sub_destination.domain = UniversalMessage_Domain_DOMAIN_INFOTAINMENT;
        if (unknown_inner) {
            CarServer_Response inner = CarServer_Response_init_default;
            inner.has_actionStatus = true; inner.actionStatus.result = static_cast<CarServer_OperationStatus_E>(99);
            reply.which_payload = UniversalMessage_RoutableMessage_protobuf_message_as_bytes_tag;
            auto& bytes = reply.payload.protobuf_message_as_bytes;
            auto stream = pb_ostream_from_buffer(bytes.bytes, sizeof bytes.bytes);
            expect(pb_encode(&stream, CarServer_Response_fields, &inner), "unknown CarServer status encodes", "unknown CarServer control failed");
            bytes.size = stream.bytes_written;
        } else reply = fault(reply, 1);
        reply = f.seal(reply, 0, UniversalMessage_Domain_DOMAIN_INFOTAINMENT);
        auto foreign = reply; foreign.to_destination.sub_destination.routing_address.bytes[0] ^= 1;
        auto wrong = reply; wrong.request_uuid.bytes[0] ^= 1;
        if (!unknown_inner) send(f.controller, foreign);
        send(f.controller, wrong);
        expect(f.completions == 0 && f.command()->payload_maybe_sent && f.command()->retry_count == 0,
               "Info outer fault rejects foreign address; all Info replies reject foreign UUID before decryption", "foreign Info fault changed outcome");
        send(f.controller, reply);
        expect(unknown_inner ? f.completions == 1 && tk::execution_uncertain(f.last_failure)
                             : f.completions == 0 && f.command()->retry_count == 1 && !f.command()->payload_maybe_sent,
               "unknown Info inner status stays uncertain; actual authenticated BUSY may retry",
               "Info fault or unknown status qualification failed");
    }
    {
        Fixture f; f.enqueue(Kind::Action, "Info", false, tk::BleDomain::Infotainment);
        f.controller.vcsec_sleep_state_.store(static_cast<int>(tk::SleepState::Asleep));
        f.controller.drive_command_runner_(); const auto wake = f.request();
        // Retained uncertainty from an earlier payload cannot be erased by this wake's valid tag.
        f.command()->payload_maybe_sent = true;
        send(f.controller, f.seal(fault(envelope(wake, true), 1), 0));
        expect(f.completions == 1 && tk::execution_uncertain(f.last_failure) && f.command()->retry_count == 0,
               "authenticated wake prerequisite cannot prove an earlier payload unexecuted",
               "prerequisite fault erased payload uncertainty");
        for (unsigned i = 0; i < 600; ++i) { native_ticks += 1000; f.controller.drive_command_runner_(); }
        expect(f.controller.ble.writes == 1, "prerequisite fault preserves long-term no-resend", "prerequisite fault restarted mutation");
    }
    for (bool tx_only : {false, true}) {
        Fixture f; f.enqueue(Kind::Action, "payload", tx_only);
        f.controller.ble.write_ok = false; f.controller.ble.submit_then_fail = true;
        f.controller.drive_command_runner_();
        expect(f.controller.ble.frames.size() == 1 && f.completions == 1 && !f.success && tk::execution_uncertain(f.last_failure),
               "false write after final submission is uncertain including TX-only payload", "post-submission false write lost uncertainty");
        for (unsigned i = 0; i < 600; ++i) { native_ticks += 1000; f.controller.drive_command_runner_(); }
        expect(f.controller.ble.writes == 1, "submitted false write cannot retry automatically", "false write resent mutation");
    }
    {
        Fixture f; f.enqueue(Kind::Action, "Wake", true); f.controller.drive_command_runner_();
        expect(f.completions == 1 && f.success && f.controller.ble.frames.size() == 1,
               "explicit TX-only success contract remains intact", "TX-only success changed");
    }
}


static void check_native_session_availability() {
    using namespace vcsec_native;
    using Save = tk::SessionSaveState;
    const auto security = UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY;
    const auto info = UniversalMessage_Domain_DOMAIN_INFOTAINMENT;
    for (bool throw_save : {false, true}) for (bool old_blob : {false, true}) {
        Fixture f; f.controller.command_runner_.vcsec_session().reset();
        f.controller.session_storage.save_ok = false;
        f.controller.session_storage.throw_save = throw_save;
        const std::vector<uint8_t> previous{0x10, 0x20, 0x30};
        if (old_blob) f.controller.session_storage.records[tk::nvs_contract::kSessionVcsec] = previous;
        f.enqueue(); f.controller.drive_command_runner_(); const auto auth = f.request();
        f.controller.session_storage.during_save = [&] {
            const auto state = f.controller.session_availability().vcsec;
            expect(state.known && state.usable && state.save == Save::Unconfirmed &&
                   f.controller.command_runner_.vcsec_session().is_authenticated() &&
                   f.command()->state == tk::CommandState::Idle,
                   "actual native RAM tracker/auth/cache progress precedes fallible save",
                   "persistence still gates usable RAM establishment");
        };
        send(f.controller, session_info(envelope(auth), auth.uuid));
        auto state = f.controller.session_availability();
        expect(f.controller.session_available() && state.vcsec.known && state.vcsec.usable &&
               state.vcsec.save == Save::Error && f.controller.session_persists == 1 && f.completions == 0,
               "actual HMAC SessionInfo retains usable RAM after false/throwing save",
               "failed save lost RAM or erased error metadata");
        const auto record = f.controller.session_storage.records.find(tk::nvs_contract::kSessionVcsec);
        expect(old_blob ? record != f.controller.session_storage.records.end() && record->second == previous
                        : record == f.controller.session_storage.records.end(),
               "failed new save cannot claim an old or absent record confirms current RAM",
               "old-blob save control changed the fixture record");
        f.controller.session_storage.during_save = {};
        f.controller.drive_command_runner_(); const auto payload_request = f.request();
        send(f.controller, f.seal(envelope(payload_request, true), 0));
        expect(f.completions == 1 && f.success,
               "RAM-only native command builds and authenticates a real current response after save failure",
               "RAM-only command was blocked by persistence outcome");
        const auto writes = f.controller.ble.writes, saves = f.controller.session_persists;
        const auto warnings = native_warnings, errors = native_errors;
        bool remained_available = true;
        for (unsigned tick = 0; tick < 600; ++tick) {
            native_ticks += 1000;
            (void)f.controller.session_availability();
            remained_available &= f.controller.session_available();
            f.controller.drive_command_runner_();
        }
        expect(remained_available && f.controller.ble.writes == writes && f.controller.session_persists == saves &&
               native_warnings == warnings && native_errors == errors,
               "ten-minute cache reads add no write, frame, enrollment or log activity",
               "RAM availability introduced a periodic side effect");
        // A later naturally received SessionInfo performs its one ordinary persistence attempt.
        f.controller.command_runner_.pop_current(); f.enqueue(); f.controller.drive_command_runner_(); const auto current = f.request();
        f.controller.session_storage.save_ok = true; f.controller.session_storage.throw_save = false;
        send(f.controller, session_info(envelope(current), current.uuid));
        state = f.controller.session_availability();
        expect(f.controller.session_available() && state.vcsec.save == Save::Confirmed &&
               f.controller.session_persists == saves + 1 && f.completions == 1,
               "later natural save success clears only current-session persistence error",
               "natural session save did not clear uncertainty or completed an unrelated command");
    }
    {
        Fixture f;
        f.controller.publish_session_runtime_locked_(security, Save::Confirmed, 0);
        f.controller.publish_session_runtime_locked_(info, Save::Confirmed, 0);
        const auto bits = f.controller.session_runtime_bits_.load();
        f.controller.identity_ready = false;
        expect(!f.controller.session_available() && !f.controller.session_availability().vcsec.known,
               "temporary identity admission gate hides RAM availability", "unsafe identity exposed RAM availability");
        f.controller.identity_ready = true;
        expect(f.controller.session_available() && f.controller.session_runtime_bits_.load() == bits,
               "pre-commit identity rollback restores the unchanged old RAM state", "temporary key gate stranded RAM cache");
        f.enqueue(); f.controller.command_runner_.cancel_current(); f.controller.command_runner_.clear();
        expect(f.controller.session_available(), "ordinary FIFO cancel/flush does not invalidate real peer sessions", "FIFO cleanup stranded healthy RAM");
        ++f.controller.identity_epoch_;
        expect(!f.controller.session_available(), "old identity cache is never promoted into a new identity", "identity epoch admitted old RAM");
        f.controller.publish_session_runtime_locked_(security, Save::Confirmed, 0);
        expect(!f.controller.session_available(), "late old-epoch persistence result cannot revive RAM", "late old result revived new identity");
        f.controller.publish_session_runtime_locked_(security, Save::Confirmed, 1);
        expect(f.controller.session_available() && !f.controller.session_availability().infotainment.known,
               "fresh identity publication cannot carry an old other-domain nibble", "old Info availability crossed identity boundary");
        f.controller.publish_session_runtime_locked_(security, Save::Error, 0);
        expect(f.controller.session_available() && f.controller.session_availability().vcsec.save == Save::Confirmed,
               "late old identity result cannot replace a fresh current-identity observation",
               "stale publication damaged current identity metadata");
    }
    {
        Fixture f;
        f.controller.publish_session_runtime_locked_(info, Save::Confirmed, 0);
        auto state = f.controller.session_availability();
        expect(state.infotainment.known && state.infotainment.usable && !f.controller.session_available() &&
               !tk::session_can_operate(true, false, f.controller.session_available()),
               "Info-only RAM is diagnostic, not global VCSEC eligibility", "Info-only session admitted background or enrollment steady state");
    }
    {
        Fixture f; f.controller.command_runner_.vcsec_session().reset();
        f.enqueue(); f.controller.drive_command_runner_(); const auto auth = f.request();
        f.controller.session_storage.during_save = [&] {
            ++f.controller.identity_epoch_; f.controller.session_runtime_bits_.store(0);
        };
        send(f.controller, session_info(envelope(auth), auth.uuid));
        expect(!f.controller.session_available() && !f.controller.session_availability().vcsec.known,
               "identity change during a save cannot publish its stale completion", "stale save completion promoted identity");
    }
    {
        Fixture f;
        f.controller.publish_session_runtime_locked_(security, Save::Confirmed, 0);
        f.controller.publish_session_runtime_locked_(info, Save::Confirmed, 0);
        f.enqueue(); f.controller.drive_command_runner_(); const auto request = f.request();
        auto repair = envelope(request, true);
        repair.has_signedMessageStatus = true;
        repair.signedMessageStatus.operation_status = UniversalMessage_OperationStatus_E_OPERATIONSTATUS_ERROR;
        repair.signedMessageStatus.signed_message_fault = UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INCORRECT_EPOCH;
        const auto valid = f.seal(repair, 0);
        auto bad = valid; bad.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.tag[0] ^= 1;
        send(f.controller, bad);
        expect(f.controller.session_available(), "bad-tag repair cannot invalidate RAM availability", "forged repair cleared RAM cache");
        send(f.controller, valid);
        const auto state = f.controller.session_availability();
        expect(!f.controller.session_available() && !state.vcsec.known && state.infotainment.usable,
               "authenticated peer reset invalidates only its domain", "peer repair left stale RAM or cleared unrelated Info");
    }
    for (bool callback_throws : {false, true}) {
        Fixture f;
        f.controller.publish_session_runtime_locked_(security, Save::Confirmed, 0);
        f.controller.publish_session_runtime_locked_(info, Save::Confirmed, 0);
        f.enqueue(); f.controller.drive_command_runner_();
        if (callback_throws) f.command()->on_complete = [](bool, const std::string&, tk::CommandFailureInfo) { throw std::bad_alloc(); };
        const bool reset = f.controller.apply_ble_link_state_(false);
        expect(reset == !callback_throws && !f.controller.session_available() &&
               !f.controller.session_availability().infotainment.usable &&
               !f.controller.command_runner_.vcsec_session().is_authenticated() &&
               !f.client.get_peer(security)->is_valid(),
               "actual link reset invalidates peers/trackers/cache before fallible completion callback",
               "link teardown exception retained usable old RAM");
    }
    {
        Fixture f;
        const auto bytes = kat::unhex(kat::kSessionInfo);
        f.controller.session_storage.records[tk::nvs_contract::kSessionVcsec] = bytes;
        UniversalMessage_RoutableMessage_session_info_t payload{};
        std::memcpy(payload.bytes, bytes.data(), bytes.size()); payload.size = bytes.size();
        Signatures_SessionInfo parsed = Signatures_SessionInfo_init_default;
        expect(f.client.parse_payload_session_info(&payload, &parsed) == 0,
               "stored public test session parses for restore control", "restore fixture could not parse");
        native_session_wall_seconds = parsed.clock_time + 1;
        f.controller.command_runner_.vcsec_session().reset(); f.client.get_peer(security)->reset();
        expect(f.controller.load_nvs_sessions_() && f.controller.session_available() &&
               f.controller.session_availability().vcsec.save == Save::Confirmed && f.controller.session_persists == 0,
               "actual existing session restore publishes usable current blob without a new save",
               "restored session did not publish truthful RAM metadata");
    }
}

static void check_native_vcsec_routing() {
    using namespace vcsec_native;
    {
        Fixture f;
        f.enqueue(Kind::Status);
        auto passive = status(envelope({}));
        passive.has_to_destination = false;
        const auto warnings = native_warnings, errors = native_errors;
        send(f.controller, passive);
        expect(f.completions == 0 && f.samples == 0 && f.controller.sleep_deliveries == 1,
               "pre-TX broadcast only updates passive sleep state", "pre-TX broadcast completed a poll");
        f.controller.drive_command_runner_();
        const auto request = f.request();
        send(f.controller, passive);
        auto foreign = status(envelope(request));
        foreign.to_destination.sub_destination.routing_address.bytes[0] ^= 1;
        send(f.controller, foreign);
        send(f.controller, command_status(envelope(request), VCSEC_OperationStatus_E_OPERATIONSTATUS_OK));
        expect(f.completions == 0 && f.samples == 0 && native_warnings == warnings && native_errors == errors &&
               f.controller.recovery_deliveries == 0, "broadcasts and mere ACK do not finish or warn for a status poll",
               "unrelated status/ACK delivered, warned or triggered recovery");
        send(f.controller, status(envelope(request)));
        expect(f.completions == 1 && f.success && f.samples == 1, "post-TX correlated status delivers once",
               "correlated status did not complete the poll");
        send(f.controller, status(envelope(request)));
        expect(f.completions == 1 && f.samples == 1, "duplicate status cannot signal the completed poll",
               "duplicate status signalled the poll twice");
    }
    {
        Fixture f; f.enqueue(); f.controller.drive_command_runner_(); const auto request = f.request();
        send(f.controller, command_status(envelope(request), VCSEC_OperationStatus_E_OPERATIONSTATUS_WAIT));
        auto passive = status(envelope({})); passive.has_to_destination = false;
        send(f.controller, passive);
        expect(f.completions == 0, "WAIT and passive status remain nonterminal", "passive status masked pending error");
        send(f.controller, command_status(envelope(request), VCSEC_OperationStatus_E_OPERATIONSTATUS_ERROR));
        expect(f.completions == 1 && !f.success, "actual ERROR wins after WAIT and passive status", "actual error was lost");
    }
    for (unsigned variant = 0; variant < 4; ++variant) {
        Fixture f; f.enqueue(); f.controller.drive_command_runner_(); const auto request = f.request();
        auto valid = envelope(request, true);
        auto wrong = valid; wrong.request_uuid.bytes[0] ^= 1;
        send(f.controller, wrong);
        auto malformed = valid;
        malformed.which_payload = UniversalMessage_RoutableMessage_protobuf_message_as_bytes_tag;
        malformed.payload.protobuf_message_as_bytes.size = 1;
        malformed.payload.protobuf_message_as_bytes.bytes[0] = 0xff;
        send(f.controller, malformed);
        auto other = valid; other.which_payload = UniversalMessage_RoutableMessage_session_info_request_tag;
        send(f.controller, other);
        expect(f.completions == 0, "foreign UUID, malformed and other universal payloads are refused",
               "invalid action response completed a command");
        if (variant == 1) valid = payload(valid, VCSEC_FromVCSECMessage_init_default);
        if (variant == 2) valid = status(valid);
        if (variant == 3) valid = command_status(valid, VCSEC_OperationStatus_E_OPERATIONSTATUS_OK);
        send(f.controller, valid);
        expect(f.completions == 1 && f.success, "reference-defined action reply completes after correct routing",
               "valid correlated action reply was dropped");
    }
    {
        Fixture f; f.enqueue(Kind::Status);
        f.controller.drive_command_runner_(); const auto old = f.request();
        native_ticks += tk::CommandRunner::kDefaultResponseTimeoutMs;
        f.controller.drive_command_runner_();
        send(f.controller, status(envelope(old)));
        expect(f.completions == 0 && f.controller.ble.writes == 1, "old empty-UUID reply drops during retry backoff",
               "old reply completed during backoff");
        native_ticks += 1000;
        f.controller.drive_command_runner_();
        const auto next = f.request();
        expect(next.reply_address != old.reply_address && next.uuid != old.uuid && f.controller.vcsec_attempt_counter_ == 2,
               "retry of the same command serializes a fresh reply address and UUID", "retry reused routing metadata");
        send(f.controller, status(envelope(old)));
        expect(f.completions == 0, "previous-attempt empty-UUID reply drops after new TX", "old attempt completed new TX");
        send(f.controller, status(envelope(next)));
        expect(f.completions == 1 && f.success && f.samples == 1, "real GET_STATUS retry completes with one current sample", "new attempt reply was dropped");
    }
    {
        Fixture f; f.controller.vcsec_attempt_counter_ = UINT32_MAX;
        f.enqueue(); f.controller.drive_command_runner_(); const auto request = f.request();
        expect(f.controller.vcsec_attempt_counter_ == 1,
               "controller attempt serial skips zero on wrap", "attempt serial wrapped to zero");
        f.controller.retire_vcsec_contexts_();
        expect(f.controller.vcsec_attempt_counter_ == 1,
               "retirement preserves controller attempt serial", "retirement reset attempt serial");
        send(f.controller, envelope(request));
        expect(f.completions == 0, "retired wrapped attempt cannot complete", "retired wrapped attempt completed");
    }
    for (unsigned stale = 0; stale < 4; ++stale) {
        Fixture f; f.enqueue(); f.controller.drive_command_runner_(); const auto request = f.request();
        if (stale == 0) ++f.controller.command_generation_;
        if (stale == 1) f.controller.ble.connected = false;
        if (stale == 2) native_ticks += tk::CommandRunner::kDefaultResponseTimeoutMs;
        if (stale == 3) f.controller.command_runner_.reset(false);
        const auto before = f.completions;
        send(f.controller, envelope(request));
        expect(f.completions == before, "generation, link, deadline and queue retirement refuse late ACKs", "stale ACK completed");
    }
    for (unsigned failure = 0; failure < 4; ++failure) {
        Fixture f; const auto id = f.enqueue();
        auto& build = f.controller.command_builders_[id % tk::CommandRunner::kMaxQueueSize];
        if (failure == 0) f.controller.ble.write_ok = false;
        if (failure == 1) build = [](Client*, uint8_t*, size_t*) { return -1; };
        if (failure == 2) build = [](Client*, uint8_t* bytes, size_t* length) {
            bytes[0] = 0; bytes[1] = 1; bytes[2] = 0; *length = 3; return 0;
        };
        if (failure == 3) build = [](Client*, uint8_t*, size_t*) -> int { throw std::runtime_error("builder failure"); };
        try { f.controller.drive_command_runner_(); } catch (const std::runtime_error&) {}
        const auto before = f.completions;
        expect(f.controller.ble.frames.empty() && before == (failure == 3 ? 0U : 1U) && !f.success &&
               (failure == 3 || tk::execution_uncertain(f.last_failure) == (failure == 0)),
               "failed write reports uncertainty; failed builds report definite failure without TX", "failed build/write outcome was lost");
        auto stale = envelope({});
        stale.to_destination.sub_destination.routing_address.size = 16;
        std::memcpy(stale.to_destination.sub_destination.routing_address.bytes,
                    f.controller.previous_vcsec_reply_address_.data(), 16);
        send(f.controller, stale);
        expect(f.completions == before, "failure leaves no deliverable context", "failed transmission accepted an ACK");
        uint8_t ordinary_wire[2048]; size_t length = sizeof ordinary_wire;
        Message ordinary = UniversalMessage_RoutableMessage_init_default;
        const bool restored = f.client.build_session_info_request_message(
            UniversalMessage_Domain_DOMAIN_INFOTAINMENT, ordinary_wire, &length) == 0 &&
            f.client.parse_universal_message_ble(ordinary_wire, length, &ordinary) == 0 &&
            std::memcmp(ordinary.from_destination.sub_destination.routing_address.bytes,
                        f.controller.ordinary_routing_address_.data(), 16) == 0;
        expect(restored, "temporary VCSEC address restores on every failed builder/write path", "temporary address leaked");
    }
    for (bool absent : {false, true}) {
        Fixture f; f.enqueue(); f.controller.drive_command_runner_(); const auto request = f.request();
        auto reply = envelope(request);
        if (!absent) reply = payload(reply, VCSEC_FromVCSECMessage_init_default);
        const auto zero = f.seal(reply, 0);
        auto foreign = zero; foreign.to_destination.sub_destination.routing_address.bytes[0] ^= 1;
        send(f.controller, foreign);
        auto bad = zero; bad.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.tag[0] ^= 1;
        send(f.controller, bad);
        expect(f.completions == 0, "foreign and bad-tag encrypted empty replies do not complete", "bad empty reply completed");
        send(f.controller, zero);
        auto* peer = f.client.get_peer(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY);
        expect(f.completions == 1 && f.success && !peer->validate_response_counter(0),
               "authenticated empty reply accepts counter zero exactly once", "authenticated zero was dropped or not recorded");
        send(f.controller, zero);
        expect(f.completions == 1, "replayed encrypted empty reply has no second completion", "replayed empty reply completed twice");
    }
    {
        Fixture f; f.enqueue(); f.controller.drive_command_runner_(); const auto request = f.request();
        const auto wait = f.seal(command_status(envelope(request), VCSEC_OperationStatus_E_OPERATIONSTATUS_WAIT), 0);
        send(f.controller, wait);
        expect(f.completions == 0, "authenticated counter-zero WAIT stays pending", "encrypted WAIT completed");
        send(f.controller, f.seal(status(envelope(request)), 0));
        expect(f.completions == 0 && f.samples == 0, "real peer drops reused counter with different valid payload",
               "counter replay completed an action");
        send(f.controller, f.seal(command_status(envelope(request), VCSEC_OperationStatus_E_OPERATIONSTATUS_OK), 1));
        expect(f.completions == 1 && f.success, "next authentic counter completes the action",
               "fresh authenticated response was dropped");
    }
    {
        Fixture f; f.enqueue(Kind::Status); f.controller.drive_command_runner_(); const auto request = f.request();
        send(f.controller, envelope(request));
        send(f.controller, payload(envelope(request), VCSEC_FromVCSECMessage_init_default));
        auto encrypted = status(envelope(request));
        encrypted.which_sub_sigData = UniversalMessage_RoutableMessage_signature_data_tag;
        encrypted.sub_sigData.signature_data.which_sig_type = Signatures_SignatureData_AES_GCM_Response_data_tag;
        send(f.controller, encrypted);
        expect(f.completions == 0 && f.samples == 0,
               "empty poll replies and encrypted replies without a signed request hash are refused",
               "unsigned status request accepted empty or undecryptable response");
        send(f.controller, status(envelope(request)));
        expect(f.completions == 1 && f.samples == 1 && f.success,
               "actual unsigned status request accepts its correlated plaintext sample", "correlated sample was dropped");
    }
    {
        Fixture f; f.controller.command_runner_.vcsec_session().reset();
        f.enqueue(); f.controller.drive_command_runner_(); const auto old = f.request();
        expect(old.payload == tk::VcsecRequestPayload::SessionInfoRequest && f.command()->state == tk::CommandState::WaitingVcsecAuth,
               "VCSEC auth uses its own actual serialized attempt", "VCSEC auth did not enter its own context");
        send(f.controller, envelope(old));
        expect(f.completions == 0, "empty auth reply never completes an action", "auth reply completed payload");
        native_ticks += tk::CommandRunner::kDefaultStepTimeoutMs;
        f.controller.drive_command_runner_(); native_ticks += 1000; f.controller.drive_command_runner_();
        const auto next = f.request();
        send(f.controller, session_info(envelope(old), old.uuid));
        expect(f.controller.session_persists == 0 && f.command()->state == tk::CommandState::WaitingVcsecAuth,
               "late previous-auth SessionInfo cannot persist or advance current auth", "old auth response mutated session");
        auto bad = session_info(envelope(next), next.uuid);
        bad.to_destination.sub_destination.routing_address.bytes[0] ^= 1;
        send(f.controller, bad);
        send(f.controller, session_info(envelope(next), next.uuid));
        expect(next.reply_address != old.reply_address && f.controller.session_persists == 1 &&
               f.command()->state == tk::CommandState::Idle && f.completions == 0,
               "correct new auth verifies HMAC and advances without completing payload", "fresh auth context failed");
        ++native_ticks; f.controller.drive_command_runner_(); const auto action = f.request();
        send(f.controller, envelope(old)); send(f.controller, envelope(next));
        expect(f.completions == 0, "both retired auth attempts cannot acknowledge payload", "auth address leaked into payload");
        send(f.controller, envelope(action));
        expect(f.completions == 1 && f.success, "authenticated payload keeps its separate correlated response", "payload reply failed");
    }
    {
        Fixture f; f.enqueue(); f.controller.drive_command_runner_(); const auto request = f.request();
        auto proactive = session_info(envelope(request), request.uuid);
        send(f.controller, proactive);
        expect(f.controller.session_persists == 1 && f.completions == 0,
               "matched proactive SessionInfo retains actual HMAC verification", "proactive SessionInfo was lost");
        auto fault = proactive;
        fault.has_signedMessageStatus = true;
        fault.signedMessageStatus.operation_status = UniversalMessage_OperationStatus_E_OPERATIONSTATUS_ERROR;
        fault.signedMessageStatus.signed_message_fault = UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INCORRECT_EPOCH;
        auto old = fault; old.to_destination.sub_destination.routing_address.bytes[0] ^= 1;
        const auto warnings = native_warnings;
        send(f.controller, old);
        expect(f.controller.recovery_deliveries == 0 && native_warnings == warnings,
               "unmatched fault is silent and cannot reset session or trigger recovery", "foreign fault reached recovery");
        send(f.controller, fault);
        expect(f.controller.recovery_deliveries == 1 && f.controller.session_persists == 2,
               "matched fault still applies authenticated proactive SessionInfo", "matched proactive fault path broke");
    }
    for (bool whitelist : {false, true}) {
        Fixture f;
        const auto id = f.enqueue(Kind::Action, whitelist ? "Whitelist Add Key" : "Wake", true);
        if (whitelist) f.controller.command_builders_[id % tk::CommandRunner::kMaxQueueSize] =
            [](Client* c, uint8_t* b, size_t* n) { return c->build_white_list_message(
                Keys_Role_ROLE_CHARGING_MANAGER, VCSEC_KeyFormFactor_KEY_FORM_FACTOR_CLOUD_KEY, b, n); };
        f.controller.drive_command_runner_();
        expect(f.completions == 1 && f.success && f.controller.ble.frames.size() == 1,
               "raw automatic WhitelistAdd and explicit Wake still complete on TX", "transmit-only completion changed");
        if (whitelist) {
            VCSEC_ToVCSECMessage raw = VCSEC_ToVCSECMessage_init_default;
            const auto& frame = f.controller.ble.frames.front();
            auto input = pb_istream_from_buffer(frame.data() + 2, frame.size() - 2);
            expect(pb_decode(&input, VCSEC_ToVCSECMessage_fields, &raw) && !f.controller.ordinary_routing_address_ready_,
                   "WhitelistAdd remains bare VCSEC without routable capture", "WhitelistAdd framing/address changed");
        }
    }
    {
        Fixture f; f.controller.command_runner_.vcsec_session().reset();
        f.enqueue(Kind::Action, "Info", false, tk::BleDomain::Infotainment);
        f.controller.drive_command_runner_(); const auto auth = f.request();
        send(f.controller, session_info(envelope(auth), auth.uuid));
        expect(f.controller.session_persists == 1 && f.command()->state == tk::CommandState::Idle,
               "Infotainment prerequisite VCSEC auth matches its own phase", "Infotainment VCSEC auth was blocked");
        const auto id = f.command()->id;
        f.controller.command_builders_[id % tk::CommandRunner::kMaxQueueSize] =
            [](Client* c, uint8_t* b, size_t* n) { return c->build_car_server_get_vehicle_data_message(
                b, n, CarServer_GetVehicleData_getChargeState_tag); };
        ++native_ticks; f.controller.drive_command_runner_();
        Message info = UniversalMessage_RoutableMessage_init_default;
        const auto& frame = f.controller.ble.frames.back();
        size_t hash_size = 0;
        const auto* hash = f.client.get_last_request_hash(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, &hash_size);
        expect(f.client.parse_universal_message_ble(const_cast<uint8_t*>(frame.data()), frame.size(), &info) == 0 &&
               info.to_destination.sub_destination.domain == UniversalMessage_Domain_DOMAIN_INFOTAINMENT &&
               std::memcmp(info.from_destination.sub_destination.routing_address.bytes,
                           f.controller.ordinary_routing_address_.data(), 16) == 0 && hash && hash_size == 17,
               "Infotainment restores ordinary routing and a real signed request hash", "VCSEC scope leaked into Infotainment");
    }
    {
        Fixture f; f.enqueue();
        expect(f.controller.command_runner_.tick(native_ticks, true, true, false) == tk::TxAction::SendCommandPayload,
               "real runner enters payload construction", "runner did not construct payload");
        uint8_t bytes[2048]; size_t length = sizeof bytes;
        tk::VcsecRequestMetadata metadata;
        const bool built = f.controller.build_vcsec_attempt_(Kind::Action, bytes, &length) == 0 &&
            tk::capture_vcsec_request(bytes, length, metadata);
        f.controller.command_runner_.notify_tx_complete(tk::TxAction::SendCommandPayload, native_ticks);
        send(f.controller, envelope(metadata));
        expect(built && f.completions == 0 && f.controller.ble.writes == 0,
               "serialized but untransmitted context cannot accept an ACK", "build alone admitted an ACK");
    }
    {
        Fixture f;
        f.enqueue(Kind::Action, "Info", false, tk::BleDomain::Infotainment);
        f.controller.vcsec_sleep_state_.store(static_cast<int>(tk::SleepState::Asleep));
        f.controller.drive_command_runner_();
        expect(f.command()->state == tk::CommandState::WaitingWake && f.controller.ble.writes == 1,
               "Infotainment prerequisite wake still transmits", "prerequisite wake changed");
        auto passive = status(envelope({})); passive.has_to_destination = false;
        send(f.controller, passive);
        expect(f.command()->wake_confirmed && f.completions == 0 && f.samples == 0,
               "passive awake status advances wake progress without completing Infotainment", "passive wake progress broke");
    }
    {
        Fixture f;
        f.enqueue(Kind::Action, "Info", false, tk::BleDomain::Infotainment);
        f.controller.vcsec_sleep_state_.store(static_cast<int>(tk::SleepState::Asleep));
        f.controller.drive_command_runner_(); const auto wake = f.request();
        auto encrypted = f.seal(status(envelope(wake)), 0);
        auto foreign = encrypted; foreign.to_destination.sub_destination.routing_address.bytes[0] ^= 1;
        auto bad_tag = encrypted; bad_tag.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.tag[0] ^= 1;
        send(f.controller, foreign); send(f.controller, bad_tag);
        expect(!f.command()->wake_confirmed && f.completions == 0 && f.controller.sleep_deliveries == 0,
               "foreign and bad-tag encrypted wake status cannot advance wake", "invalid encrypted wake advanced state");
        send(f.controller, encrypted);
        expect(f.command()->wake_confirmed && f.completions == 0 && f.samples == 0 &&
               f.controller.sleep_deliveries == 1,
               "correlated authenticated zero-counter wake status advances wake only",
               "authenticated wake status was dropped or completed Info");
        send(f.controller, encrypted);
        auto* peer = f.client.get_peer(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY);
        expect(peer && !peer->validate_response_counter(0) && f.controller.sleep_deliveries == 1 &&
               f.completions == 0 && f.samples == 0,
               "wake status uses real zero-inclusive replay window", "wake replay reached state update");
    }
    {
        Fixture f;
        f.enqueue(Kind::Action, "Info", false, tk::BleDomain::Infotainment);
        f.controller.vcsec_sleep_state_.store(static_cast<int>(tk::SleepState::Asleep));
        f.controller.drive_command_runner_(); const auto old = f.request();
        const auto delayed = f.seal(status(envelope(old)), 0);
        native_ticks += tk::CommandRunner::kDefaultStepTimeoutMs;
        f.controller.drive_command_runner_(); native_ticks += 1000;
        f.controller.drive_command_runner_(); const auto fresh = f.request();
        send(f.controller, delayed);
        expect(old.reply_address != fresh.reply_address && !f.command()->wake_confirmed &&
               f.controller.sleep_deliveries == 0 && f.completions == 0,
               "previous wake GCM attempt cannot advance retry or consume its window",
               "old encrypted wake matched a new attempt");
        send(f.controller, f.seal(status(envelope(fresh)), 0));
        expect(f.command()->wake_confirmed && f.controller.sleep_deliveries == 1 &&
               f.completions == 0 && f.samples == 0,
               "fresh encrypted wake retry retains zero-counter acceptance",
               "old wake response poisoned the fresh authenticated retry");
    }
    // The serialized SendWake request owns a VCSEC context even though its current
    // command is Infotainment. Proactive authenticated session repair must survive;
    // neither its empty reply nor CommandStatus is an Infotainment result.
    for (bool with_fault : {false, true}) {
        Fixture f;
        f.enqueue(Kind::Action, "Info", false, tk::BleDomain::Infotainment);
        f.controller.vcsec_sleep_state_.store(static_cast<int>(tk::SleepState::Asleep));
        f.controller.drive_command_runner_(); const auto wake = f.request();
        auto reply = session_info(envelope(wake), wake.uuid);
        if (with_fault) {
            reply.has_signedMessageStatus = true;
            reply.signedMessageStatus.operation_status = UniversalMessage_OperationStatus_E_OPERATIONSTATUS_ERROR;
            reply.signedMessageStatus.signed_message_fault = UniversalMessage_MessageFault_E_MESSAGEFAULT_ERROR_INCORRECT_EPOCH;
        }
        auto foreign = reply; foreign.to_destination.sub_destination.routing_address.bytes[0] ^= 1;
        const auto warnings = native_warnings;
        send(f.controller, foreign);
        expect(f.controller.session_persists == 0 && f.controller.recovery_deliveries == 0 && native_warnings == warnings,
               "foreign wake repair is silent and cannot change session", "foreign wake repair entered native handling");
        send(f.controller, reply);
        expect(f.controller.session_persists == 1 && f.controller.recovery_deliveries == unsigned(with_fault) &&
               f.command()->state == (with_fault ? tk::CommandState::Idle : tk::CommandState::WaitingWake) &&
               f.command()->retry_count == unsigned(with_fault) && f.completions == 0 && f.samples == 0,
               "matched wake SessionInfo and fault retain authenticated repair without Info completion",
               "wake prerequisite repair was lost or completed Info");
        send(f.controller, envelope(wake));
        send(f.controller, command_status(envelope(wake), VCSEC_OperationStatus_E_OPERATIONSTATUS_OK));
        expect(f.completions == 0 && f.samples == 0 && !f.command()->wake_confirmed,
               "wake ACK and CommandStatus cannot substitute for awake status or Info result",
               "wake ACK completed Info or confirmed awake");
    }
    for (unsigned obsolete : {0U, 1U, 2U, 3U, 4U}) {
        Fixture f;
        f.enqueue(Kind::Action, "Info", false, tk::BleDomain::Infotainment);
        f.controller.vcsec_sleep_state_.store(static_cast<int>(tk::SleepState::Asleep));
        tk::VcsecRequestMetadata old;
        if (obsolete == 0) {
            expect(f.controller.command_runner_.tick(native_ticks, true, false, true) == tk::TxAction::SendWake,
                   "real runner selects wake prerequisite", "runner did not select wake");
            uint8_t bytes[2048]; size_t size = sizeof bytes;
            expect(f.controller.build_vcsec_attempt_(Kind::WakePrerequisite, bytes, &size) == 0 &&
                   tk::capture_vcsec_request(bytes, size, old),
                   "real wake builds before transmission", "pretransmission wake did not build");
        } else if (obsolete == 1) {
            f.controller.ble.write_ok = false;
            f.controller.drive_command_runner_();
            const auto& attempted = f.controller.tx_buffer_;
            expect(f.controller.ble.writes == 1 && f.controller.ble.frames.empty() &&
                   tk::capture_vcsec_request(attempted.data(), attempted.size(), old),
                   "native failed wake write retains bytes only for the negative control",
                   "failed wake write was not exercised");
        } else {
            f.controller.drive_command_runner_(); old = f.request();
            if (obsolete == 2) native_ticks += tk::CommandRunner::kDefaultStepTimeoutMs;
            if (obsolete == 3) ++f.controller.command_generation_;
            if (obsolete == 4) f.controller.ble.connected = false;
        }
        const auto warnings = native_warnings;
        send(f.controller, session_info(envelope(old), old.uuid));
        expect(f.controller.session_persists == 0 && f.controller.recovery_deliveries == 0 &&
               f.completions == 0 && f.samples == 0 && native_warnings == warnings,
               "untransmitted, failed, expired, old-generation and disconnected wake repair is rejected",
               "obsolete wake context admitted repair");
        if (obsolete == 2) {
            f.controller.drive_command_runner_(); native_ticks += 1000;
            f.controller.drive_command_runner_(); const auto fresh = f.request();
            send(f.controller, session_info(envelope(old), old.uuid));
            expect(fresh.reply_address != old.reply_address && f.controller.session_persists == 0,
                   "new wake retry retires previous address", "old wake repair matched retry");
            send(f.controller, session_info(envelope(fresh), fresh.uuid));
            expect(f.controller.session_persists == 1 && f.completions == 0,
                   "fresh wake retry accepts its own authenticated repair", "new wake repair did not match");
        }
    }
    {
        Fixture f;
        f.controller.command_runner_.vcsec_session().reset();
        f.controller.command_runner_.info_session().reset();
        const auto id = f.enqueue(Kind::Action, "Info", false, tk::BleDomain::Infotainment);
        f.controller.command_builders_[id % tk::CommandRunner::kMaxQueueSize] =
            [](Client* c, uint8_t* b, size_t* n) { return c->build_car_server_get_vehicle_data_message(
                b, n, CarServer_GetVehicleData_getChargeState_tag); };
        f.controller.vcsec_sleep_state_.store(static_cast<int>(tk::SleepState::Asleep));
        f.controller.drive_command_runner_(); const auto auth = f.request();
        const auto auth_reply = session_info(envelope(auth), auth.uuid);
        send(f.controller, auth_reply);
        expect(f.command()->state == tk::CommandState::Idle && f.controller.session_persists == 1,
               "full native sequence establishes VCSEC prerequisite", "full sequence failed VCSEC auth");
        const auto ordinary = f.controller.ordinary_routing_address_;
        ++native_ticks; f.controller.drive_command_runner_(); const auto wake = f.request();
        send(f.controller, auth_reply); send(f.controller, envelope(auth));
        const auto wake_reply = f.seal(status(envelope(wake)), 0);
        send(f.controller, wake_reply);
        expect(f.command()->wake_confirmed && f.controller.session_persists == 1 && f.completions == 0,
               "full sequence replaces auth context with authenticated wake", "full sequence failed wake replacement");
        ++native_ticks; f.controller.drive_command_runner_();
        Message outgoing = UniversalMessage_RoutableMessage_init_default;
        const auto& auth_frame = f.controller.ble.frames.back();
        const bool decoded = f.client.parse_universal_message_ble(
            const_cast<uint8_t*>(auth_frame.data()), auth_frame.size(), &outgoing) == 0;
        tk::VcsecRequestMetadata info;
        info.reply_address = ordinary;
        std::memcpy(info.uuid.data(), outgoing.uuid.bytes, info.uuid.size());
        auto info_reply = session_info(envelope(info, true), info.uuid);
        info_reply.from_destination.sub_destination.domain = UniversalMessage_Domain_DOMAIN_INFOTAINMENT;
        send(f.controller, auth_reply); send(f.controller, wake_reply);
        send(f.controller, session_info(envelope(wake), wake.uuid));
        expect(decoded && f.command()->state == tk::CommandState::WaitingInfoAuth &&
               outgoing.which_payload == UniversalMessage_RoutableMessage_session_info_request_tag &&
               outgoing.to_destination.sub_destination.domain == UniversalMessage_Domain_DOMAIN_INFOTAINMENT &&
               std::memcmp(outgoing.from_destination.sub_destination.routing_address.bytes, ordinary.data(), 16) == 0 &&
               f.controller.session_persists == 1 && f.completions == 0 && f.samples == 0,
               "full sequence restores ordinary Info auth and rejects retired VCSEC replies",
               "old VCSEC reply disrupted Info authentication");
        send(f.controller, info_reply);
        ++native_ticks; f.controller.drive_command_runner_();
        const auto& payload_frame = f.controller.ble.frames.back();
        const bool payload_decoded = f.client.parse_universal_message_ble(
            const_cast<uint8_t*>(payload_frame.data()), payload_frame.size(), &outgoing) == 0;
        send(f.controller, auth_reply); send(f.controller, wake_reply); send(f.controller, envelope(wake));
        expect(payload_decoded && f.command()->state == tk::CommandState::AwaitingResponse &&
               outgoing.to_destination.sub_destination.domain == UniversalMessage_Domain_DOMAIN_INFOTAINMENT &&
               std::memcmp(outgoing.from_destination.sub_destination.routing_address.bytes, ordinary.data(), 16) == 0 &&
               f.controller.session_persists == 2 && f.completions == 0 && f.samples == 0,
               "full sequence sends Info payload with unchanged ordinary address and no foreign completion",
               "retired VCSEC context leaked into Info payload");
        Message current = UniversalMessage_RoutableMessage_init_default;
        expect(car_side_decode(make_carserver_frame(f.command()->uuid.data()), f.client, current),
               "full sequence builds an actual CarServer response", "could not build full-sequence response");
        send(f.controller, f.seal(current, 0, UniversalMessage_Domain_DOMAIN_INFOTAINMENT));
        expect(f.completions == 1 && f.success && f.controller.telemetry_deliveries == 1 && f.samples == 0,
               "full auth-wake-auth-payload sequence completes only on current authenticated Info response",
               "full sequence lost current Info completion or telemetry");
    }
    {
        Fixture f; f.controller.command_runner_.info_session().reset();
        f.enqueue(Kind::Action, "Info", false, tk::BleDomain::Infotainment);
        f.controller.drive_command_runner_();
        const auto& frame = f.controller.ble.frames.back();
        Message outgoing = UniversalMessage_RoutableMessage_init_default;
        const bool decoded = f.client.parse_universal_message_ble(
            const_cast<uint8_t*>(frame.data()), frame.size(), &outgoing) == 0;
        tk::VcsecRequestMetadata metadata;
        metadata.reply_address = f.controller.ordinary_routing_address_;
        std::memcpy(metadata.uuid.data(), outgoing.uuid.bytes, 16);
        auto response = session_info(envelope(metadata, true), metadata.uuid);
        response.from_destination.sub_destination.domain = UniversalMessage_Domain_DOMAIN_INFOTAINMENT;
        send(f.controller, response);
        expect(decoded && outgoing.which_payload == UniversalMessage_RoutableMessage_session_info_request_tag &&
               outgoing.to_destination.sub_destination.domain == UniversalMessage_Domain_DOMAIN_INFOTAINMENT &&
               f.controller.session_persists == 1 && f.command()->state == tk::CommandState::Idle && f.completions == 0,
               "ordinary Infotainment SessionInfo routing and HMAC remain intact", "ordinary auth routing changed");
    }
    {
        Fixture f; f.controller.command_runner_.vcsec_session().reset();
        f.enqueue(); f.controller.drive_command_runner_(); const auto request = f.request();
        auto bad = session_info(envelope(request), request.uuid);
        bad.sub_sigData.signature_data.sig_type.session_info_tag.tag.bytes[0] ^= 1;
        send(f.controller, bad);
        expect(f.completions == 1 && !f.success && f.controller.session_persists == 0,
               "matched auth reply still requires authentic SessionInfo HMAC", "bad HMAC established auth");
    }

}

// Pack an existing published serialized fixture into the same bounded payload type as callers.
static UniversalMessage_RoutableMessage_session_info_t session_info_payload(const std::vector<uint8_t>& bytes) {
    UniversalMessage_RoutableMessage_session_info_t result{};
    expect(bytes.size() <= sizeof(result.bytes), "published SessionInfo fixture fits its payload",
           "published SessionInfo fixture exceeds the protocol payload");
    if (bytes.size() > sizeof(result.bytes)) return result;
    result.size = static_cast<pb_size_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), result.bytes);
    return result;
}

// Ordinary defensive API tests: known published vectors, key matching and state preservation.
// No generated SessionInfo tags, forged routable frames or radio/vehicle operations.
static void check_session_info_continuity() {
    namespace tc = TeslaBLE::TestConstants;
    const auto encoded = kat::unhex(kat::kSessionInfo);
    const auto payload = session_info_payload(encoded);
    const auto uuid = kat::unhex(kat::kSessionInfoRequestUuid);
    const auto tag = kat::unhex(kat::kSessionInfoTag);
    Signatures_SessionInfo vector_info = Signatures_SessionInfo_init_default;
    auto input = pb_istream_from_buffer(encoded.data(), encoded.size());
    expect(pb_decode(&input, Signatures_SessionInfo_fields, &vector_info),
           "continuity cases decode the unchanged published SessionInfo vector",
           "continuity fixture could not decode the published vector");
    for (auto domain : {UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY,
                        UniversalMessage_Domain_DOMAIN_INFOTAINMENT}) {
        Client client;
        client.set_vin(tc::TEST_VIN);
        const bool loaded = client.load_private_key(kat::client_pem(), kat::client_pem_size()) == 0;
        auto verify = [&] (UniversalMessage_Domain selected, const Signatures_SessionInfo& info) {
            return client.verify_session_info_tag(selected, info, payload,
                uuid.data(), uuid.size(), tag.data(), tag.size());
        };
        auto* peer = client.get_peer(domain);
        expect(loaded && peer && verify(domain, vector_info),
               "both fresh domains accept the unchanged known-good SessionInfo HMAC",
               "fresh domain rejected the known-good SessionInfo vector");
        auto established = vector_info;
        const bool ready = peer && peer->update_session(&established) == TeslaBLE_Status_E_OK;
        expect(ready && verify(domain, vector_info),
               "established same-key refresh authenticates against retained material",
               "established known-good refresh failed");
        peer->set_is_valid(false);
        expect(peer->has_shared_secret() && verify(domain, vector_info),
               "temporary validity loss retains the same-key authentication binding",
               "temporary validity changed the established-key contract");
        expect(!verify(UniversalMessage_Domain_DOMAIN_BROADCAST, vector_info),
               "SessionInfo verification refuses an unsupported selected domain",
               "SessionInfo verification inferred a peer for an unsupported domain");

        // A different preexisting peer key must not be selected for this published vector.
        const auto other = domain == UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY
            ? UniversalMessage_Domain_DOMAIN_INFOTAINMENT : UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY;
        auto alternate = vector_info;
        std::memcpy(alternate.publicKey.bytes, tc::EXPECTED_CLIENT_PUBLIC_KEY, 65);
        auto* other_peer = client.get_peer(other);
        expect(other_peer && other_peer->update_session(&alternate) == TeslaBLE_Status_E_OK &&
                   !verify(other, vector_info) && verify(domain, vector_info),
               "mandatory domain selects only its own existing key binding",
               "SessionInfo verification crossed a domain key binding");
        other_peer->set_is_valid(false);
        expect(!verify(other, vector_info),
               "invalid session flag cannot bypass a retained different key",
               "temporary invalidity bypassed retained key matching");
    }

    for (auto domain : {UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY,
                        UniversalMessage_Domain_DOMAIN_INFOTAINMENT}) {
        for (bool invalid : {false, true}) for (unsigned kind = 0; kind < 4; ++kind) {
            Client client;
            client.set_vin(tc::TEST_VIN);
            const bool loaded = client.load_private_key(kat::client_pem(), kat::client_pem_size()) == 0;
            auto original = vector_info;
            original.counter = 7;
            auto* peer = client.get_peer(domain);
            const bool ready = loaded && peer && peer->update_session(&original) == TeslaBLE_Status_E_OK &&
                               peer->validate_response_counter(42);
            expect(ready, "refusal fixture establishes ordinary peer state", "refusal fixture setup failed");
            if (!ready) continue;
            peer->set_is_valid(!invalid);
            VehicleController controller(client);
            controller.publish_session_runtime_locked_(domain, tk::SessionSaveState::Confirmed, 0);
            const auto bits = controller.session_runtime_bits_.load();
            const auto records = controller.session_storage.records;
            const auto epoch = std::array<uint8_t,16>{
                original.epoch[0], original.epoch[1], original.epoch[2], original.epoch[3],
                original.epoch[4], original.epoch[5], original.epoch[6], original.epoch[7],
                original.epoch[8], original.epoch[9], original.epoch[10], original.epoch[11],
                original.epoch[12], original.epoch[13], original.epoch[14], original.epoch[15]};
            std::array<uint8_t,16> secret{};
            std::memcpy(secret.data(), peer->get_shared_secret(), secret.size());
            auto rejected = original;
            rejected.counter = 99;
            rejected.clock_time += 99;
            rejected.epoch[0] ^= 1;
            if (kind == 0) rejected.publicKey.size = 0;
            if (kind == 1) rejected.publicKey.size = 64;
            if (kind == 2) rejected.publicKey.bytes[0] = 0x02;
            if (kind == 3) std::memcpy(rejected.publicKey.bytes, tc::EXPECTED_CLIENT_PUBLIC_KEY, 65);
            auto unchanged = [&] {
                return peer->get_counter() == original.counter && peer->get_clock_time() == original.clock_time &&
                    peer->is_valid() == !invalid && peer->has_shared_secret() &&
                    std::memcmp(peer->get_epoch(), epoch.data(), epoch.size()) == 0 &&
                    std::memcmp(peer->get_shared_secret(), secret.data(), secret.size()) == 0 &&
                    peer->accepts_session_key(original.publicKey.bytes, original.publicKey.size) &&
                    !peer->validate_response_counter(42);
            };
            expect(peer->update_session(&rejected) != TeslaBLE_Status_E_OK && unchanged(),
                   "ordinary update refuses missing/malformed/different key before peer changes",
                   "ordinary update changed established state on key refusal");
            expect(peer->force_update_session(&rejected) != TeslaBLE_Status_E_OK && unchanged(),
                   "forced update cannot bypass the existing key binding",
                   "forced update bypassed key matching or changed peer state");
            expect(peer->load_tesla_key(rejected.publicKey.bytes, rejected.publicKey.size) != TeslaBLE_Status_E_OK && unchanged(),
                   "direct key load cannot replace retained peer material",
                   "direct load changed the existing peer binding");
            UniversalMessage_RoutableMessage_session_info_t unused_payload{};
            controller.establish_verified_session_locked_(domain, rejected, unused_payload);
            expect(unchanged() && controller.session_runtime_bits_.load() == bits &&
                       controller.session_persists == 0 && controller.session_storage.records == records &&
                       controller.ble.writes == 0,
                   "native refusal preserves availability, peer, storage and passive BLE sink",
                   "native refusal cleared availability or changed peer/storage/BLE state");
            peer->clear_shared_secret();
            expect(!peer->has_shared_secret() && peer->accepts_session_key(tc::EXPECTED_CLIENT_PUBLIC_KEY, 65),
                   "explicit clear retains its existing fresh-bootstrap lifetime boundary",
                   "continuity correction silently made a persistent vehicle-key pin");
        }
    }
}

#include "ble_deferred_delivery_fixture.hpp"

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

    check_native_ble_deferred_delivery();

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
    check_coredump_hash_vectors([](bool ok, const char* label) { expect(ok, label, label); });
    check_native_carserver_replay();
    check_native_pairing_cleanup();
    check_native_vcsec_routing();
    check_native_command_faults();
    check_native_command_completion();
    check_native_wake_confirmation();
    check_native_session_availability();
    check_session_info_continuity();
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
    {
        // v5.3 must not turn metadata-only SessionInfo into an authenticated peer. NVS and
        // network callers must inspect is_initialized(), even when update_session returns OK.
        auto context = kat::loaded_client_context();
        Peer peer(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, context, tc::TEST_VIN);
        Signatures_SessionInfo info = Signatures_SessionInfo_init_zero;
        std::memcpy(info.epoch, tc::TEST_EPOCH, sizeof info.epoch);
        info.clock_time = 1234;
        info.counter = 7;
        expect(context && peer.update_session(&info) == TeslaBLE_Status_E_OK &&
                   !peer.is_valid() && !peer.has_shared_secret() && !peer.is_initialized() &&
                   !peer.can_send_command(),
               "metadata-only session update cannot authenticate a peer",
               "metadata-only session update claims authentication without a shared secret");
        expect(peer.force_update_session(&info) == TeslaBLE_Status_E_OK &&
                   !peer.is_valid() && !peer.has_shared_secret() && !peer.is_initialized(),
               "metadata-only forced update cannot authenticate a peer",
               "forced update claims authentication without a shared secret");
        std::memcpy(info.publicKey.bytes, tc::EXPECTED_VEHICLE_PUBLIC_KEY, 65);
        info.publicKey.size = 65;
        expect(peer.update_session(&info) == TeslaBLE_Status_E_OK && peer.is_initialized() &&
                   peer.can_send_command() &&
                   std::memcmp(peer.get_shared_secret(), tc::EXPECTED_SESSION_KEY, 16) == 0,
               "official public key establishes a usable session with the known ECDH secret",
               "official public key did not establish the expected session");
        info.counter = 3;
        ++info.clock_time;
        expect(peer.update_session(&info) == TeslaBLE_Status_E_OK && peer.is_initialized() &&
                   peer.get_counter() == 7 && peer.get_clock_time() == info.clock_time,
               "lower reported counter preserves authentication and max(local, reported)",
               "session-counter alignment or established-session reuse regressed");
        peer.clear_shared_secret();
        info.publicKey.size = 0;
        expect(peer.update_session(&info) == TeslaBLE_Status_E_OK && !peer.is_initialized() &&
                   !peer.has_shared_secret() && !peer.can_send_command(),
               "cleared key material cannot be restored by metadata-only SessionInfo",
               "metadata-only SessionInfo restored a cleared shared secret");
    }
    {
        // vehicle-command binds each response to its own domain's signed request. A later
        // Infotainment transmission must leave the outstanding VCSEC hash intact, and vice versa.
        Client scoped;
        scoped.set_vin(tc::TEST_VIN);
        bool ready = scoped.load_private_key(reinterpret_cast<const uint8_t*>(tc::CLIENT_PRIVATE_KEY_PEM),
                                             std::strlen(tc::CLIENT_PRIVATE_KEY_PEM) + 1) == 0;
        Signatures_SessionInfo info = Signatures_SessionInfo_init_zero;
        std::memcpy(info.epoch, tc::TEST_EPOCH, sizeof info.epoch);
        std::memcpy(info.publicKey.bytes, tc::EXPECTED_VEHICLE_PUBLIC_KEY, 65);
        info.publicKey.size = 65;
        info.clock_time = 1234;
        info.counter = 7;
        for (auto domain : {UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY,
                            UniversalMessage_Domain_DOMAIN_INFOTAINMENT}) {
            auto* peer = scoped.get_peer(domain);
            ready = ready && peer && peer->update_session(&info) == 0 && peer->is_initialized();
        }
        uint8_t payload[] = {0x08, 0x01};
        uint8_t wire[2048];
        size_t wire_size = 0;
        auto send = [&](UniversalMessage_Domain domain) {
            return ready && scoped.build_universal_message_with_payload(
                payload, sizeof payload, domain, wire, &wire_size, true) == 0;
        };
        auto hash = [&](UniversalMessage_Domain domain) {
            size_t size = 0;
            const auto* data = scoped.get_last_request_hash(domain, &size);
            return data ? std::vector<uint8_t>(data, data + size) : std::vector<uint8_t>{};
        };
        ready = send(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY);
        const auto vcsec_hash = hash(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY);
        ready = ready && send(UniversalMessage_Domain_DOMAIN_INFOTAINMENT);
        const auto info_hash = hash(UniversalMessage_Domain_DOMAIN_INFOTAINMENT);
        expect(ready && !vcsec_hash.empty() && !info_hash.empty() &&
                   hash(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY) == vcsec_hash,
               "Infotainment transmission preserves the VCSEC request hash",
               "Infotainment transmission overwrote the VCSEC response context");
        expect(send(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY) &&
                   hash(UniversalMessage_Domain_DOMAIN_INFOTAINMENT) == info_hash,
               "VCSEC transmission preserves the Infotainment request hash",
               "VCSEC transmission overwrote the Infotainment response context");
        size_t size = 99;
        expect(scoped.get_last_request_hash(UniversalMessage_Domain_DOMAIN_BROADCAST, &size) == nullptr &&
                   size == 0,
               "unsupported request-hash domain has no decryption context",
               "unsupported request-hash domain leaked another domain's context");
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
        Signatures_SessionInfo info = Signatures_SessionInfo_init_zero;
        std::memcpy(info.epoch, tc::TEST_EPOCH, sizeof info.epoch);
        std::memcpy(info.publicKey.bytes, tc::EXPECTED_VEHICLE_PUBLIC_KEY, 65);
        info.publicKey.size = 65;
        info.clock_time = 2650;
        info.counter = 6;
        const bool ready = context && peer.update_session(&info) == TeslaBLE_Status_E_OK &&
                           peer.is_initialized();
        peer.set_counter(7);
        uint8_t ad[128]{};
        size_t ad_size = 0;
        // Exercise the library's request builder against the independent official TLV vector;
        // supplying that vector directly to encrypt() would not test metadata construction.
        const bool built = ready && peer.construct_ad_buffer(
            Signatures_SignatureType_SIGNATURE_TYPE_AES_GCM_PERSONALIZED, tc::TEST_VIN,
            2655, ad, &ad_size, 2) == TeslaBLE_Status_E_OK;
        expect(built && ad_size == metadata.size() && std::memcmp(ad, metadata.data(), ad_size) == 0,
               "request metadata builder matches the official type, domain, VIN, epoch, expiry, counter and flags",
               "request metadata builder diverges from the official command vector");
        std::vector<uint8_t> input = plaintext;
        uint8_t ciphertext[64]{};
        size_t ciphertext_size = 0;
        uint8_t tag[16]{};
        uint8_t nonce[12]{};
        const bool encrypted =
            built &&
            peer.encrypt(input.data(), input.size(), ciphertext, sizeof ciphertext, &ciphertext_size, tag,
                         ad, ad_size, nonce) == TeslaBLE_Status_E_OK &&
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
        // The real peer window admits zero and reordered counters once, retains exactly 64
        // counters, and starts afresh when Client signs the next request. Authentication is a
        // separate prerequisite covered by the native response-adapter tests; counter zero
        // alone does not identify a plaintext reply (ADR-0005).
        Client scoped;
        scoped.set_vin(tc::TEST_VIN);
        bool ready = scoped.load_private_key(reinterpret_cast<const uint8_t*>(tc::CLIENT_PRIVATE_KEY_PEM),
                                             std::strlen(tc::CLIENT_PRIVATE_KEY_PEM) + 1) == 0;
        Signatures_SessionInfo info = Signatures_SessionInfo_init_zero;
        std::memcpy(info.epoch, tc::TEST_EPOCH, sizeof info.epoch);
        std::memcpy(info.publicKey.bytes, tc::EXPECTED_VEHICLE_PUBLIC_KEY, 65);
        info.publicKey.size = 65;
        info.clock_time = 1234;
        info.counter = 7;
        auto* peer = scoped.get_peer(UniversalMessage_Domain_DOMAIN_INFOTAINMENT);
        ready = ready && peer && peer->update_session(&info) == 0 && peer->is_initialized();
        const bool window = ready &&
                            peer->validate_response_counter(0) && !peer->validate_response_counter(0) &&
                            peer->validate_response_counter(5) && peer->validate_response_counter(3) &&
                            !peer->validate_response_counter(3) && !peer->validate_response_counter(5) &&
                            peer->validate_response_counter(100) && peer->validate_response_counter(37) &&
                            !peer->validate_response_counter(37) && !peer->validate_response_counter(36);
        expect(window,
               "response window: zero, repeats, reordering, 63-behind admission and 64-behind expiry",
               "response replay window accepts a repeat or refuses a valid reordered response");
        uint8_t payload[] = {0x08, 0x01};
        uint8_t wire[2048];
        size_t wire_size = 0;
        const bool fresh = window &&
                           scoped.build_universal_message_with_payload(
                               payload, sizeof payload, UniversalMessage_Domain_DOMAIN_INFOTAINMENT, wire,
                               &wire_size, true) == 0 &&
                           peer->validate_response_counter(37) && !peer->validate_response_counter(37);
        expect(fresh, "the next request starts a fresh response window",
               "a response counter seen for the previous request stays refused for the next one");
    }
    {
        // The known answer above pins Peer's metadata assembly; this pins what Client feeds it. A real
        // encrypted Infotainment request is decoded the way the vehicle decodes it, protocol.md
        // metadata is serialized from the message's OWN epoch, expiry, counter and flags, and the
        // reference AES-GCM with that metadata must reproduce the message's ciphertext and tag.
        Client scoped;
        scoped.set_vin(tc::TEST_VIN);
        bool ready = scoped.load_private_key(reinterpret_cast<const uint8_t*>(tc::CLIENT_PRIVATE_KEY_PEM),
                                             std::strlen(tc::CLIENT_PRIVATE_KEY_PEM) + 1) == 0;
        Signatures_SessionInfo info = Signatures_SessionInfo_init_zero;
        std::memcpy(info.epoch, tc::TEST_EPOCH, sizeof info.epoch);
        std::memcpy(info.publicKey.bytes, tc::EXPECTED_VEHICLE_PUBLIC_KEY, 65);
        info.publicKey.size = 65;
        info.clock_time = 1234;
        info.counter = 7;
        auto* peer = scoped.get_peer(UniversalMessage_Domain_DOMAIN_INFOTAINMENT);
        ready = ready && peer && peer->update_session(&info) == 0 && peer->is_initialized();
        uint8_t payload[] = {0x08, 0x01};
        uint8_t wire[2048];
        size_t wire_size = 0;
        UniversalMessage_RoutableMessage msg = UniversalMessage_RoutableMessage_init_default;
        pb_istream_t in = pb_istream_from_buffer(wire, 0);
        bool decoded = ready && scoped.build_universal_message_with_payload(
                                    payload, sizeof payload,
                                    UniversalMessage_Domain_DOMAIN_INFOTAINMENT, wire, &wire_size, true) == 0;
        if (decoded) {
            in = pb_istream_from_buffer(wire, wire_size);
            decoded = pb_decode(&in, UniversalMessage_RoutableMessage_fields, &msg) &&
                      msg.which_payload == UniversalMessage_RoutableMessage_protobuf_message_as_bytes_tag &&
                      msg.which_sub_sigData == UniversalMessage_RoutableMessage_signature_data_tag &&
                      msg.sub_sigData.signature_data.which_sig_type ==
                          Signatures_SignatureData_AES_GCM_Personalized_data_tag;
        }
        auto sealed_from_wire = [&](const UniversalMessage_RoutableMessage& candidate) {
            const auto& gcm = candidate.sub_sigData.signature_data.sig_type.AES_GCM_Personalized_data;
            std::vector<uint8_t> metadata_from_wire;
            auto tlv = [&](uint8_t tag, const uint8_t* value, size_t size) {
                metadata_from_wire.push_back(tag);
                metadata_from_wire.push_back(static_cast<uint8_t>(size));
                metadata_from_wire.insert(metadata_from_wire.end(), value, value + size);
            };
            auto be32 = [&](uint8_t tag, uint32_t value) {
                const uint8_t bytes[4] = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                                          static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
                tlv(tag, bytes, sizeof bytes);
            };
            const uint8_t type = Signatures_SignatureType_SIGNATURE_TYPE_AES_GCM_PERSONALIZED;
            const uint8_t domain = UniversalMessage_Domain_DOMAIN_INFOTAINMENT;
            tlv(Signatures_Tag_TAG_SIGNATURE_TYPE, &type, 1);
            tlv(Signatures_Tag_TAG_DOMAIN, &domain, 1);
            tlv(Signatures_Tag_TAG_PERSONALIZATION, reinterpret_cast<const uint8_t*>(tc::TEST_VIN),
                std::strlen(tc::TEST_VIN));
            tlv(Signatures_Tag_TAG_EPOCH, gcm.epoch, sizeof gcm.epoch);
            be32(Signatures_Tag_TAG_EXPIRES_AT, gcm.expires_at);
            be32(Signatures_Tag_TAG_COUNTER, gcm.counter);
            if (candidate.flags != 0) be32(Signatures_Tag_TAG_FLAGS, candidate.flags);
            metadata_from_wire.push_back(Signatures_Tag_TAG_END);
            const auto& body = candidate.payload.protobuf_message_as_bytes;
            const auto reference = kat::gcm_encrypt(tc::EXPECTED_SESSION_KEY,
                                                    std::vector<uint8_t>(gcm.nonce, gcm.nonce + sizeof gcm.nonce),
                                                    kat::sha256(metadata_from_wire),
                                                    std::vector<uint8_t>(payload, payload + sizeof payload));
            return body.size == sizeof payload && reference.size() == sizeof payload + 16 &&
                   std::memcmp(body.bytes, reference.data(), sizeof payload) == 0 &&
                   std::memcmp(gcm.tag, reference.data() + sizeof payload, 16) == 0;
        };
        expect(decoded && msg.sub_sigData.signature_data.sig_type.AES_GCM_Personalized_data.counter == 8 &&
                   msg.flags != 0,
               "Client advances the session counter for the request and sends nonzero flags",
               "Client request did not exercise the next counter and flags");
        expect(decoded && sealed_from_wire(msg),
               "Client seals each request with metadata built from the counter, expiry and flags it sends",
               "Client's request AAD does not match the counter, expiry or flags on the wire");
        // These controls must fail through the independent AEAD comparison, not the counter/flags
        // assertions above. Change one wire field while retaining the real ciphertext and tag.
        auto altered = msg;
        altered.sub_sigData.signature_data.sig_type.AES_GCM_Personalized_data.counter ^= 1;
        expect(decoded && !sealed_from_wire(altered), "request AAD control rejects a changed counter",
               "request AAD comparison ignored the wire counter");
        altered = msg;
        altered.sub_sigData.signature_data.sig_type.AES_GCM_Personalized_data.expires_at ^= 1;
        expect(decoded && !sealed_from_wire(altered), "request AAD control rejects a changed expiry",
               "request AAD comparison ignored the wire expiry");
        altered = msg;
        altered.sub_sigData.signature_data.sig_type.AES_GCM_Personalized_data.epoch[0] ^= 1;
        expect(decoded && !sealed_from_wire(altered), "request AAD control rejects a changed epoch",
               "request AAD comparison ignored the wire epoch");
        altered = msg;
        altered.flags = 0;
        expect(decoded && !sealed_from_wire(altered), "request AAD control rejects removed flags",
               "request AAD comparison ignored the wire flags");
        altered = msg;
        altered.sub_sigData.signature_data.sig_type.AES_GCM_Personalized_data.tag[0] ^= 1;
        expect(decoded && !sealed_from_wire(altered), "request AAD control rejects a changed tag",
               "request AAD comparison ignored the wire tag");
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
        const auto payload = session_info_payload(info_bytes);
        const auto uuid = kat::unhex(kat::kSessionInfoRequestUuid);
        auto tag = kat::unhex(kat::kSessionInfoTag);
        Signatures_SessionInfo info = Signatures_SessionInfo_init_default;
        pb_istream_t in = pb_istream_from_buffer(info_bytes.data(), info_bytes.size());
        const bool decoded = client.load_private_key(kat::client_pem(), kat::client_pem_size()) == 0 &&
                             pb_decode(&in, Signatures_SessionInfo_fields, &info);
        const bool accepted = decoded && client.verify_session_info_tag(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, info, payload,
                                                                        uuid.data(), uuid.size(), tag.data(),
                                                                        tag.size());
        tag[0] ^= 0x01;
        const bool refused = decoded && !client.verify_session_info_tag(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, info, payload,
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
        // Expected legacy SEC1 fixture plus final newline and NUL (228 B). This check does not
        // independently execute the old Mbed TLS implementation; the NVS text format stays pinned.
        const std::string legacy_export = std::string(tc::CLIENT_PRIVATE_KEY_PEM) + "\n";
        const bool ok = pem_client.load_private_key(kat::client_pem(), kat::client_pem_size()) == 0 &&
                        pem_client.get_private_key(pem_out, sizeof pem_out, &pem_size) == 0 &&
                        pem_size == legacy_export.size() + 1 && pem_out[pem_size - 1] == 0 &&
                        std::memcmp(pem_out, legacy_export.data(), legacy_export.size()) == 0 &&
                        reloaded->load_private_key(pem_out, pem_size) == TeslaBLE_Status_E_OK &&
                        reloaded->generate_public_key(point, &point_size) == TeslaBLE_Status_E_OK &&
                        point_size == 65 && std::memcmp(point, tc::EXPECTED_CLIENT_PUBLIC_KEY, 65) == 0;
        expect(ok, "PEM export matches the expected legacy SEC1 fixture and reloads",
               "PEM export differs from the expected legacy fixture or does not reload");
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

    {
        // Last crypto check: shut down the real PSA RNG after Client initialization. Both
        // routing-address and UUID failures must stop handshake and payload construction.
        Client no_connection;
        Client with_connection;
        no_connection.set_vin(kVin);
        with_connection.set_vin(kVin);
        const pb_byte_t fixed_connection[16] = {0x42};
        const pb_byte_t empty_connection[16] = {};
        with_connection.set_connection_id(fixed_connection);
        pb_byte_t output[1024];
        size_t length = sizeof output;
        const bool initialized = no_connection.load_private_key(kat::client_pem(), kat::client_pem_size()) == 0 &&
                                 with_connection.load_private_key(kat::client_pem(), kat::client_pem_size()) == 0;
        expect(initialized && no_connection.build_session_info_request_message(
                   UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, output, &length) == 0,
               "healthy RNG permits a handshake", "healthy RNG unexpectedly blocks a handshake");
        no_connection.set_connection_id(empty_connection);
        vcsec_native::Fixture native_no_address, native_with_address;
        native_no_address.enqueue();
        native_with_address.enqueue();
        expect(native_with_address.controller.initialize_routing_address_(),
               "ordinary native address initializes with healthy RNG", "native routing initialization failed");
        NativeRoutingRandom::fail = true;
        for (auto* fixture : {&native_no_address, &native_with_address}) {
            fixture->controller.drive_command_runner_();
            expect(fixture->controller.ble.writes == 0 && fixture->controller.ble.frames.empty() && fixture->completions == 1 && !fixture->success &&
                   !tk::execution_uncertain(fixture->last_failure),
                   "injected routing RNG failure blocks both ordinary and per-attempt address writes",
                   "injected routing RNG failure emitted a frame or lost definite failure");
        }
        NativeRoutingRandom::fail = false;
        // CryptoUtils initializes PSA on demand, so shutting PSA down cannot inject failure
        // into that public helper. The following direct Client RNG path does not reinitialize
        // PSA: shut it down only after all native routing calls and assert the real failure.
        mbedtls_psa_crypto_free();
        for (Client* candidate : {&no_connection, &with_connection}) {
            std::memset(output, 0xa5, sizeof output);
            length = sizeof output;
            const auto handshake = candidate->build_session_info_request_message(
                UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, output, &length);
            pb_byte_t payload[] = {1};
            const auto request = candidate->build_universal_message_with_payload(
                payload, sizeof payload,
                UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, output, &length, false);
            bool untouched = length == sizeof output;
            for (const auto byte : output) untouched = untouched && byte == 0xa5;
            expect(initialized && handshake == TeslaBLE_Status_E_ERROR_CRYPTO &&
                       request == TeslaBLE_Status_E_ERROR_CRYPTO && untouched,
                   "failed RNG refuses handshake/payload without emitting a frame",
                   "failed RNG generated a fallback identifier or emitted a frame");
        }
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
