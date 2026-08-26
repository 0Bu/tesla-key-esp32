#include "nvs_storage.hpp"
#include "config_blob.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

int checks = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        ++checks;                                                                \
        if (!(condition)) {                                                      \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": "         \
                      << #condition << '\n';                                     \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

struct StringRead {
    esp_err_t error;
    size_t length;
    std::string value;
};

struct BlobRead {
    esp_err_t error;
    size_t length;
    std::vector<uint8_t> value;
};

std::vector<StringRead> string_reads;
size_t string_read_index = 0;
std::vector<BlobRead> blob_reads;
size_t blob_read_index = 0;

void script_string_reads(std::initializer_list<StringRead> reads) {
    string_reads.assign(reads);
    string_read_index = 0;
}

void check_script_consumed() {
    CHECK(string_read_index == string_reads.size());
}

void script_blob_reads(std::initializer_list<BlobRead> reads) {
    blob_reads.assign(reads);
    blob_read_index = 0;
}

void check_blob_script_consumed() {
    CHECK(blob_read_index == blob_reads.size());
}

}  // namespace

extern "C" esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t* out_handle) {
    *out_handle = 1;
    return ESP_OK;
}

extern "C" void nvs_close(nvs_handle_t) {}

extern "C" esp_err_t nvs_get_blob(nvs_handle_t, const char*, void* out_value, size_t* length) {
    if (blob_read_index >= blob_reads.size()) std::abort();
    const BlobRead& scripted = blob_reads[blob_read_index++];
    const size_t capacity = *length;
    *length = scripted.length;
    if (scripted.error != ESP_OK) return scripted.error;
    if (!out_value) return ESP_OK;
    if (capacity < scripted.length) return ESP_ERR_NVS_INVALID_LENGTH;
    std::memcpy(out_value, scripted.value.data(),
                std::min(scripted.length, scripted.value.size()));
    return ESP_OK;
}

extern "C" esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, size_t) {
    return ESP_OK;
}

extern "C" esp_err_t nvs_erase_key(nvs_handle_t, const char*) {
    return ESP_OK;
}

extern "C" esp_err_t nvs_commit(nvs_handle_t) {
    return ESP_OK;
}

extern "C" esp_err_t nvs_get_str(nvs_handle_t, const char*, char* out_value, size_t* length) {
    if (string_read_index >= string_reads.size()) std::abort();
    const StringRead& scripted = string_reads[string_read_index++];
    const size_t capacity = *length;
    *length = scripted.length;
    if (scripted.error != ESP_OK) return scripted.error;
    if (!out_value) return ESP_OK;
    if (capacity < scripted.length) return ESP_ERR_NVS_INVALID_LENGTH;

    std::fill_n(out_value, scripted.length, '\0');
    const size_t payload_size = scripted.length == 0 ? 0 : scripted.length - 1;
    std::memcpy(out_value, scripted.value.data(),
                std::min(payload_size, scripted.value.size()));
    return ESP_OK;
}

extern "C" esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*) {
    return ESP_OK;
}

extern "C" const char* esp_err_to_name(esp_err_t err) {
    switch (err) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
        case ESP_ERR_NVS_TYPE_MISMATCH: return "ESP_ERR_NVS_TYPE_MISMATCH";
        case ESP_ERR_NVS_INVALID_LENGTH: return "ESP_ERR_NVS_INVALID_LENGTH";
        default: return "UNKNOWN";
    }
}

int main() {
    using S = tk::NvsStringLoadState;
    using B = tk::NvsBlobLoadState;
    using C = tk::ConfigLoadState;

    // Uninitialised storage is an error, not an absent journal, and performs no NVS read.
    {
        NvsStorageAdapter storage("tesla_cfg");
        std::string out = "stale";
        script_string_reads({});
        CHECK(storage.load_str_state("vin_txn", out) == S::Error);
        CHECK(out.empty());
        check_script_consumed();
    }

    NvsStorageAdapter storage("tesla_cfg");
    CHECK(storage.initialize());

    // Unknown long keys fail closed instead of aliasing after a silent 15-character truncation.
    // No NVS read is attempted, so the empty scripts must remain untouched.
    {
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({});
        CHECK(!storage.load("unknown_key_name_too_long", out));
        CHECK(out.empty());
        check_blob_script_consumed();
        CHECK(!storage.save("unknown_key_name_too_long", std::vector<uint8_t>{1}));
    }

    // The hot VCSEC session-presence check probes NVS once, then follows successful writes and
    // removals from the adapter cache. A read error would stay Unknown and be retried instead.
    {
        script_blob_reads({{ESP_OK, 4, {}}});
        CHECK(storage.blob_exists("session_vcsec"));
        CHECK(storage.blob_exists("session_vcsec"));
        check_blob_script_consumed();
        CHECK(storage.remove("session_vcsec"));
        CHECK(!storage.blob_exists("session_vcsec"));
        CHECK(storage.save("session_vcsec", std::vector<uint8_t>{1, 2, 3, 4}));
        CHECK(storage.blob_exists("session_vcsec"));
    }
    {
        NvsStorageAdapter retrying("tesla_ble");
        CHECK(retrying.initialize());
        script_blob_reads({{ESP_FAIL, 0, {}}, {ESP_OK, 4, {}}});
        CHECK(!retrying.blob_exists("session_vcsec"));
        CHECK(retrying.blob_exists("session_vcsec"));
        check_blob_script_consumed();
    }

    // tesla-ble's generic StorageAdapter path loads the durable private key and sessions. Its
    // probe and data read must describe one stable blob: accepting a shorter second value can put
    // key B in Vehicle RAM while a later fingerprint read observes durable key A, invalidating the
    // armed-VIN recovery decision. Failures must not expose a partial/stale candidate to callers.
    {
        NvsStorageAdapter unavailable("tesla_ble");
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({});
        CHECK(!unavailable.load("private_key", out));
        CHECK(out.empty());
        check_blob_script_consumed();
    }
    {
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({{ESP_ERR_NVS_NOT_FOUND, 0, {}}});
        CHECK(!storage.load("private_key", out));
        CHECK(out.empty());
        check_blob_script_consumed();
    }
    for (const std::initializer_list<BlobRead> reads : {
             std::initializer_list<BlobRead>{{ESP_FAIL, 0, {}}},
             std::initializer_list<BlobRead>{{ESP_OK, 4, {}}, {ESP_FAIL, 4, {}}},
             // Stub models the real API: a longer second record cannot fit the probed capacity
             // and is returned as INVALID_LENGTH even when the scripted backing read is OK.
             std::initializer_list<BlobRead>{{ESP_OK, 4, {}},
                                             {ESP_OK, 7, {1, 2, 3, 4, 5, 6, 7}}},
             std::initializer_list<BlobRead>{{ESP_OK, 4, {}},
                                             {ESP_ERR_NVS_INVALID_LENGTH, 7, {}}}}) {
        std::vector<uint8_t> out{0xAA};
        script_blob_reads(reads);
        CHECK(!storage.load("private_key", out));
        CHECK(out.empty());
        check_blob_script_consumed();
    }
    {
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({{ESP_OK, 0, {}}});
        CHECK(!storage.load("private_key", out));
        CHECK(out.empty());
        check_blob_script_consumed();
    }
    {
        // Reproducer: probe sees durable PEM A, but the second read supplies a shorter, complete
        // PEM B. The old adapter returned true with B plus a zero-filled tail sized for A.
        const std::string durable_a =
            "-----BEGIN PRIVATE KEY-----\nAAAAAAAAAAAAAAAA\n-----END PRIVATE KEY-----\n";
        const std::string runtime_b =
            "-----BEGIN PRIVATE KEY-----\nBBBB\n-----END PRIVATE KEY-----\n";
        const std::vector<uint8_t> key_b(runtime_b.begin(), runtime_b.end());
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({{ESP_OK, durable_a.size(), {}},
                           {ESP_OK, key_b.size(), key_b}});
        CHECK(!storage.load("private_key", out));
        CHECK(out.empty());
        check_blob_script_consumed();
    }
    {
        const std::vector<uint8_t> stable{1, 2, 0, 4};
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({{ESP_OK, stable.size(), {}},
                           {ESP_OK, stable.size(), stable}});
        CHECK(storage.load("session_vcsec", out));
        CHECK(out == stable);  // generic session blobs are binary; embedded NUL is valid
        check_blob_script_consumed();
    }

    // The raw ConfigBlob reader has the same safety boundary as vin_txn: only exact NOT_FOUND is
    // absence. A present-zero, probe/read fault or changing length is recovery ambiguity.
    {
        NvsStorageAdapter unavailable("tesla_cfg");
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({});
        CHECK(unavailable.load_blob_state(tk::kConfigBlobKey, out) == B::Error);
        CHECK(out.empty());
        check_blob_script_consumed();
    }
    {
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({{ESP_ERR_NVS_NOT_FOUND, 0, {}}});
        CHECK(storage.load_blob_state(tk::kConfigBlobKey, out) == B::Missing);
        CHECK(out.empty());
        check_blob_script_consumed();
    }
    for (const esp_err_t error : {ESP_ERR_NVS_TYPE_MISMATCH, ESP_FAIL}) {
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({{error, 0, {}}});
        CHECK(storage.load_blob_state(tk::kConfigBlobKey, out) == B::Error);
        CHECK(out.empty());
        check_blob_script_consumed();
    }
    {
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({{ESP_OK, 0, {}}});
        CHECK(storage.load_blob_state(tk::kConfigBlobKey, out) == B::Error);
        CHECK(out.empty());
        check_blob_script_consumed();
    }
    {
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({{ESP_OK, 4, {}}, {ESP_FAIL, 4, {}}});
        CHECK(storage.load_blob_state(tk::kConfigBlobKey, out) == B::Error);
        CHECK(out.empty());
        check_blob_script_consumed();
    }
    {
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({{ESP_OK, 4, {}}, {ESP_OK, 2, {1, 2}}});
        CHECK(storage.load_blob_state(tk::kConfigBlobKey, out) == B::Error);
        CHECK(out.empty());
        check_blob_script_consumed();
    }
    {
        std::vector<uint8_t> out;
        script_blob_reads({{ESP_OK, 4, {}}, {ESP_OK, 4, {1, 2, 3, 4}}});
        CHECK(storage.load_blob_state(tk::kConfigBlobKey, out) == B::Present);
        CHECK(out == std::vector<uint8_t>({1, 2, 3, 4}));
        check_blob_script_consumed();
    }

    tk::ConfigBlob encoded;
    encoded.wifi_ssid = "new-net";
    encoded.wifi_pass = "password";
    encoded.vin = "LRWYGCEK9PC000001";
    tk::ConfigBlobBuffer encoded_buf{};
    const size_t encoded_len =
        tk::config_blob_encode(encoded, encoded_buf.data(), encoded_buf.size());
    CHECK(encoded_len > 0);
    const std::vector<uint8_t> encoded_bytes(encoded_buf.begin(),
                                             encoded_buf.begin() + encoded_len);

    // Armed VIN recovery may classify only a stable, decoded blob or a verified Missing record.
    // Probe/read faults and present-but-invalid CRC/schema never invoke legacy reads.
    {
        tk::ConfigBlob out;
        script_blob_reads({{ESP_OK, encoded_len, {}},
                           {ESP_OK, encoded_len, encoded_bytes}});
        script_string_reads({});
        CHECK(tk::cfg_load_state(storage, out) == C::Blob);
        CHECK(out.vin == encoded.vin);
        check_blob_script_consumed();
        check_script_consumed();
    }
    {
        std::vector<uint8_t> corrupt = encoded_bytes;
        corrupt.back() ^= 0x01;
        tk::ConfigBlob out;
        out.vin = "untouched";
        script_blob_reads({{ESP_OK, corrupt.size(), {}},
                           {ESP_OK, corrupt.size(), corrupt}});
        script_string_reads({});
        CHECK(tk::cfg_load_state(storage, out) == C::Error);
        CHECK(out.vin == "untouched");
        check_blob_script_consumed();
        check_script_consumed();
    }
    {
        // Ordinary, unjournaled callers retain the migration behavior: an invalid blob may fall
        // back to legacy. Armed recovery calls cfg_load_state() above and never takes this path.
        std::vector<uint8_t> corrupt = encoded_bytes;
        corrupt.back() ^= 0x01;
        const std::string legacy_vin = "5YJ3E1EA7KF000316";
        const size_t legacy_vin_len = legacy_vin.size() + 1;
        tk::ConfigBlob out;
        script_blob_reads({{ESP_OK, corrupt.size(), {}},
                           {ESP_OK, corrupt.size(), corrupt}});
        script_string_reads({{ESP_ERR_NVS_NOT_FOUND, 0, {}},
                             {ESP_ERR_NVS_NOT_FOUND, 0, {}},
                             {ESP_OK, legacy_vin_len, {}},
                             {ESP_OK, legacy_vin_len, legacy_vin},
                             {ESP_ERR_NVS_NOT_FOUND, 0, {}},
                             {ESP_ERR_NVS_NOT_FOUND, 0, {}}});
        CHECK(!tk::cfg_load(storage, out));
        CHECK(out.vin == legacy_vin);
        check_blob_script_consumed();
        check_script_consumed();
    }
    {
        std::vector<uint8_t> future = encoded_bytes;
        future[4] = static_cast<uint8_t>(tk::kConfigBlobVersion + 1);
        tk::ConfigBlob out;
        script_blob_reads({{ESP_OK, future.size(), {}},
                           {ESP_OK, future.size(), future}});
        script_string_reads({});
        CHECK(tk::cfg_load_state(storage, out) == C::Error);
        check_blob_script_consumed();
        check_script_consumed();
    }
    for (const std::initializer_list<BlobRead> reads : {
             std::initializer_list<BlobRead>{{ESP_FAIL, 0, {}}},
             std::initializer_list<BlobRead>{{ESP_OK, encoded_len, {}},
                                             {ESP_FAIL, encoded_len, {}}}}) {
        tk::ConfigBlob out;
        script_blob_reads(reads);
        script_string_reads({});
        CHECK(tk::cfg_load_state(storage, out) == C::Error);
        check_blob_script_consumed();
        check_script_consumed();
    }
    {
        tk::ConfigBlob out;
        script_blob_reads({{ESP_ERR_NVS_NOT_FOUND, 0, {}}});
        script_string_reads({{ESP_ERR_NVS_NOT_FOUND, 0, {}},
                             {ESP_ERR_NVS_NOT_FOUND, 0, {}},
                             {ESP_ERR_NVS_NOT_FOUND, 0, {}},
                             {ESP_ERR_NVS_NOT_FOUND, 0, {}},
                             {ESP_ERR_NVS_NOT_FOUND, 0, {}}});
        CHECK(tk::cfg_load_state(storage, out) == C::Legacy);
        CHECK(out.vin.empty());
        check_blob_script_consumed();
        check_script_consumed();
    }

    // The one and only Missing mapping is an exact first-probe NOT_FOUND.
    {
        std::string out = "stale";
        script_string_reads({{ESP_ERR_NVS_NOT_FOUND, 0, {}}});
        CHECK(storage.load_str_state("vin_txn", out) == S::Missing);
        CHECK(out.empty());
        check_script_consumed();
    }

    // Wrong type, corrupt/probe I/O failure and empty values all fail closed.
    for (const esp_err_t error : {ESP_ERR_NVS_TYPE_MISMATCH, ESP_FAIL}) {
        std::string out = "stale";
        script_string_reads({{error, 0, {}}});
        CHECK(storage.load_str_state("vin_txn", out) == S::Error);
        CHECK(out.empty());
        check_script_consumed();
    }
    for (const size_t invalid_length : {size_t{0}, size_t{1}}) {
        std::string out = "stale";
        script_string_reads({{ESP_OK, invalid_length, {}}});
        CHECK(storage.load_str_state("vin_txn", out) == S::Error);
        CHECK(out.empty());
        check_script_consumed();
    }

    // Failure or shrinkage during the second read is not allowed to inherit the successful probe.
    {
        std::string out = "stale";
        script_string_reads({{ESP_OK, 16, {}}, {ESP_FAIL, 16, {}}});
        CHECK(storage.load_str_state("vin_txn", out) == S::Error);
        CHECK(out.empty());
        check_script_consumed();
    }
    {
        std::string out = "stale";
        script_string_reads({{ESP_OK, 16, {}}, {ESP_OK, 1, {}}});
        CHECK(storage.load_str_state("vin_txn", out) == S::Error);
        CHECK(out.empty());
        check_script_consumed();
    }
    {
        // Reproducer: the second value is shorter but nonempty and syntactically parseable as a
        // VIN marker. It must not inherit the successful longer probe and become Present.
        const std::string probed = "5YJ3E1EA7KF000316|AA:BB:CC:DD";
        const std::string shorter = "LRWYGCEK9PC000001|";
        std::string out = "stale";
        script_string_reads({{ESP_OK, probed.size() + 1, {}},
                             {ESP_OK, shorter.size() + 1, shorter}});
        CHECK(storage.load_str_state("vin_txn", out) == S::Error);
        CHECK(out.empty());
        check_script_consumed();
    }
    {
        std::string out = "stale";
        script_string_reads({{ESP_OK, 8, {}},
                             {ESP_OK, 12, "longer-value"}});
        CHECK(storage.load_str_state("vin_txn", out) == S::Error);
        CHECK(out.empty());
        check_script_consumed();
    }
    {
        std::string out = "stale";
        script_string_reads({{ESP_OK, 8, {}},
                             {ESP_ERR_NVS_INVALID_LENGTH, 12, {}}});
        CHECK(storage.load_str_state("vin_txn", out) == S::Error);
        CHECK(out.empty());
        check_script_consumed();
    }
    {
        // Same length but an empty/NUL first byte is not a safety marker.
        std::string out = "stale";
        script_string_reads({{ESP_OK, 8, {}}, {ESP_OK, 8, {}}});
        CHECK(storage.load_str_state("vin_txn", out) == S::Error);
        CHECK(out.empty());
        check_script_consumed();
    }

    // A stable, nonempty second read is the only Present result.
    {
        const std::string marker = "5YJ3E1EA7KF000316|AA:BB:CC:DD";
        const size_t encoded_length = marker.size() + 1;
        std::string out = "stale";
        script_string_reads({{ESP_OK, encoded_length, {}},
                             {ESP_OK, encoded_length, marker}});
        CHECK(storage.load_str_state("vin_txn", out) == S::Present);
        CHECK(out == marker);
        check_script_consumed();
    }

    std::cout << "OK  " << checks << " NVS adapter checks passed\n";
    return 0;
}
