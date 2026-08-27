#include "nvs_storage.hpp"
#include "config_blob.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
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

struct NvsCall {
    std::string api;
    std::string name_space;
    std::string key;
    int mode{-1};
};

std::vector<StringRead> string_reads;
size_t string_read_index = 0;
std::vector<BlobRead> blob_reads;
size_t blob_read_index = 0;
std::string last_namespace;
std::string last_blob_key;
std::string last_string_key;
size_t nvs_calls = 0;
std::vector<NvsCall> nvs_call_log;
std::map<nvs_handle_t, std::string> handle_namespaces;
nvs_handle_t next_handle = 1;
esp_err_t next_open_error = ESP_OK;
esp_err_t next_set_blob_error = ESP_OK;
esp_err_t next_erase_error = ESP_OK;
esp_err_t next_commit_error = ESP_OK;
esp_err_t next_set_str_error = ESP_OK;

esp_err_t consume_error(esp_err_t& value) {
    const esp_err_t result = value;
    value = ESP_OK;
    return result;
}

std::string namespace_for(nvs_handle_t handle) {
    const auto found = handle_namespaces.find(handle);
    return found == handle_namespaces.end() ? std::string{} : found->second;
}

void record_call(const char* api, nvs_handle_t handle, const char* key = nullptr) {
    nvs_call_log.push_back({api, namespace_for(handle), key ? key : "", -1});
}

void clear_call_log() {
    nvs_call_log.clear();
}

void check_last_call(const char* api, const char* name_space, const char* key) {
    CHECK(!nvs_call_log.empty());
    const auto& call = nvs_call_log.back();
    CHECK(call.api == api);
    CHECK(call.name_space == name_space);
    CHECK(call.key == key);
}

void check_call(size_t index, const char* api, const char* name_space, const char* key = "") {
    CHECK(index < nvs_call_log.size());
    const auto& call = nvs_call_log[index];
    CHECK(call.api == api);
    CHECK(call.name_space == name_space);
    CHECK(call.key == key);
}

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

extern "C" esp_err_t nvs_open(const char* name_space, nvs_open_mode_t mode,
                               nvs_handle_t* out_handle) {
    last_namespace = name_space ? name_space : "";
    ++nvs_calls;
    const esp_err_t error = consume_error(next_open_error);
    nvs_call_log.push_back({"open", last_namespace, "", static_cast<int>(mode)});
    if (error != ESP_OK) return error;
    *out_handle = next_handle++;
    handle_namespaces[*out_handle] = last_namespace;
    return ESP_OK;
}

extern "C" void nvs_close(nvs_handle_t handle) {
    handle_namespaces.erase(handle);
}

extern "C" esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value,
                                  size_t* length) {
    last_blob_key = key ? key : "";
    ++nvs_calls;
    record_call("get_blob", handle, key);
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

extern "C" esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void*, size_t) {
    last_blob_key = key ? key : "";
    ++nvs_calls;
    record_call("set_blob", handle, key);
    return consume_error(next_set_blob_error);
}

extern "C" esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key) {
    last_blob_key = key ? key : "";
    ++nvs_calls;
    record_call("erase_key", handle, key);
    return consume_error(next_erase_error);
}

extern "C" esp_err_t nvs_commit(nvs_handle_t handle) {
    ++nvs_calls;
    record_call("commit", handle);
    return consume_error(next_commit_error);
}

extern "C" esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* out_value,
                                 size_t* length) {
    last_string_key = key ? key : "";
    ++nvs_calls;
    record_call("get_str", handle, key);
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

extern "C" esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char*) {
    last_string_key = key ? key : "";
    ++nvs_calls;
    record_call("set_str", handle, key);
    return consume_error(next_set_str_error);
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

static void test_nvs_contract() {
    namespace NC = tk::nvs_contract;

    // Exact namespace/key/owner/API/retention inventory. This deliberately duplicates the
    // production table as a test oracle: adding, deleting, moving or reclassifying a record must
    // update an explicit gate rather than silently expanding the persistence surface.
    constexpr std::array<NC::Entry, 19> expected{{
        {NC::Namespace::Config, "cfg", "cfg", NC::StorageApi::RawBlob,
         NC::Owner::ConfigHttp, NC::Retention::DurableAcrossOta, true},
        {NC::Namespace::Config, "wifi_ssid", "wifi_ssid", NC::StorageApi::String,
         NC::Owner::LegacyConfigMirror, NC::Retention::LegacyDowngradeMirror, true},
        {NC::Namespace::Config, "wifi_pass", "wifi_pass", NC::StorageApi::String,
         NC::Owner::LegacyConfigMirror, NC::Retention::LegacyDowngradeMirror, true},
        {NC::Namespace::Config, "vin", "vin", NC::StorageApi::String,
         NC::Owner::LegacyConfigMirror, NC::Retention::LegacyDowngradeMirror, true},
        {NC::Namespace::Config, "mqtt_uri", "mqtt_uri", NC::StorageApi::String,
         NC::Owner::LegacyConfigMirror, NC::Retention::LegacyDowngradeMirror, true},
        {NC::Namespace::Config, "syslog_uri", "syslog_uri", NC::StorageApi::String,
         NC::Owner::LegacyConfigMirror, NC::Retention::LegacyDowngradeMirror, true},
        {NC::Namespace::Config, "last_time", "last_time", NC::StorageApi::String,
         NC::Owner::Clock, NC::Retention::ReplaceableCache, false},
        {NC::Namespace::Config, "vin_txn", "vin_txn", NC::StorageApi::String,
         NC::Owner::VinTransition, NC::Retention::RecoveryJournal, true},
        {NC::Namespace::Config, "ble_mac", "ble_mac", NC::StorageApi::String,
         NC::Owner::BleDiscovery, NC::Retention::ReplaceableCache, true},
        {NC::Namespace::Config, "reboot_why", "reboot_why", NC::StorageApi::String,
         NC::Owner::HeapWatchdog, NC::Retention::RecoveryJournal, false},
        {NC::Namespace::Config, "boot_fails", "boot_fails", NC::StorageApi::String,
         NC::Owner::BootGuard, NC::Retention::RecoveryJournal, false},
        {NC::Namespace::Config, "disp_rot", "disp_rot", NC::StorageApi::DirectU8,
         NC::Owner::Display, NC::Retention::DurableAcrossOta, false},
        {NC::Namespace::Config, "disp_flip", "disp_flip", NC::StorageApi::DirectU8,
         NC::Owner::Display, NC::Retention::MigrationOnly, false},
        {NC::Namespace::TeslaBle, "private_key", "private_key", NC::StorageApi::Blob,
         NC::Owner::TeslaBleLibrary, NC::Retention::DurableAcrossOta, true},
        {NC::Namespace::TeslaBle, "session_vcsec", "sess_vcsec", NC::StorageApi::Blob,
         NC::Owner::TeslaBleLibrary, NC::Retention::ReplaceableCache, true},
        {NC::Namespace::TeslaBle, "session_infotainment", "sess_info", NC::StorageApi::Blob,
         NC::Owner::TeslaBleLibrary, NC::Retention::ReplaceableCache, true},
        {NC::Namespace::TeslaBle, "paired_at", "paired_at", NC::StorageApi::String,
         NC::Owner::Pairing, NC::Retention::ReplaceableCache, false},
        {NC::Namespace::TeslaBle, "key_created", "key_created", NC::StorageApi::String,
         NC::Owner::Pairing, NC::Retention::DurableAcrossOta, false},
        {NC::Namespace::TeslaBle, "key_rotate", "key_rotate", NC::StorageApi::Blob,
         NC::Owner::KeyRotation, NC::Retention::RecoveryJournal, true},
    }};
    CHECK(NC::valid());
    CHECK(NC::kEntries.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto& actual = NC::kEntries[i];
        const auto& wanted = expected[i];
        CHECK(actual.name_space == wanted.name_space);
        CHECK(actual.logical_key == wanted.logical_key);
        CHECK(actual.stored_key == wanted.stored_key);
        CHECK(actual.api == wanted.api);
        CHECK(actual.owner == wanted.owner);
        CHECK(actual.retention == wanted.retention);
        CHECK(actual.secret == wanted.secret);
        CHECK(actual.stored_key.size() <= 15);
        CHECK(NC::find(actual.name_space, actual.logical_key) == &actual);
    }

    constexpr std::string_view exactly_fifteen{"123456789012345"};
    constexpr std::string_view sixteen{"1234567890123456"};
    static_assert(exactly_fifteen.size() == 15);
    static_assert(sixteen.size() == 16);
    CHECK(exactly_fifteen.size() <= 15);  // ESP-IDF accepts all fifteen payload bytes.
    CHECK(sixteen.size() > 15);
    CHECK(NC::classify_namespace(NC::kConfigNamespace) == NC::Namespace::Config);
    CHECK(NC::classify_namespace(NC::kTeslaBleNamespace) == NC::Namespace::TeslaBle);
    CHECK(NC::classify_namespace("tesla_unknown") == NC::Namespace::Unknown);

    // Unknown namespaces and unknown/16-byte/wrong-API keys are rejected before any NVS call.
    {
        NvsStorageAdapter unknown("tesla_unknown");
        const size_t before = nvs_calls;
        CHECK(!unknown.initialize());
        CHECK(nvs_calls == before);
    }

    {
        NvsStorageAdapter unavailable(NC::kConfigNamespace);
        next_open_error = ESP_FAIL;
        clear_call_log();
        CHECK(!unavailable.initialize());
        CHECK(nvs_call_log.size() == 1);
        CHECK(nvs_call_log[0].api == "open");
        CHECK(nvs_call_log[0].name_space == NC::kConfigNamespace);
        CHECK(nvs_call_log[0].mode == NVS_READWRITE);
    }
}

template <typename Fn>
void check_rejected_without_nvs(Fn&& operation) {
    const size_t before = nvs_calls;
    clear_call_log();
    CHECK(!operation());
    CHECK(nvs_calls == before);
    CHECK(nvs_call_log.empty());
}

static const char* namespace_name(tk::nvs_contract::Namespace value) {
    namespace NC = tk::nvs_contract;
    return value == NC::Namespace::Config ? NC::kConfigNamespace : NC::kTeslaBleNamespace;
}

static void test_registered_physical_key_mapping(NvsStorageAdapter& config,
                                                 NvsStorageAdapter& tesla) {
    namespace NC = tk::nvs_contract;
    for (const auto& entry : NC::kEntries) {
        NvsStorageAdapter& storage =
            entry.name_space == NC::Namespace::Config ? config : tesla;
        clear_call_log();
        if (entry.api == NC::StorageApi::Blob) {
            bool exists = true;
            script_blob_reads({{ESP_ERR_NVS_NOT_FOUND, 0, {}}});
            CHECK(storage.probe_blob(std::string(entry.logical_key), exists));
            CHECK(!exists);
            check_blob_script_consumed();
            CHECK(nvs_call_log.size() == 1);
            check_last_call("get_blob", namespace_name(entry.name_space), entry.stored_key.data());
        } else if (entry.api == NC::StorageApi::String) {
            std::string out = "unchanged";
            script_string_reads({{ESP_ERR_NVS_NOT_FOUND, 0, {}}});
            CHECK(storage.load_str_state(entry.logical_key.data(), out) ==
                  tk::NvsStringLoadState::Missing);
            CHECK(out.empty());
            check_script_consumed();
            CHECK(nvs_call_log.size() == 1);
            check_last_call("get_str", namespace_name(entry.name_space), entry.stored_key.data());
        } else if (entry.api == NC::StorageApi::RawBlob) {
            std::vector<uint8_t> out{0xAA};
            script_blob_reads({{ESP_ERR_NVS_NOT_FOUND, 0, {}}});
            CHECK(storage.load_blob_state(entry.logical_key.data(), out) ==
                  tk::NvsBlobLoadState::Missing);
            CHECK(out.empty());
            check_blob_script_consumed();
            CHECK(nvs_call_log.size() == 1);
            check_last_call("get_blob", namespace_name(entry.name_space), entry.stored_key.data());
        } else {
            CHECK(entry.api == NC::StorageApi::DirectU8);
        }
    }
}

static void test_adapter_no_call_guards(NvsStorageAdapter& config,
                                        NvsStorageAdapter& tesla) {
    namespace NC = tk::nvs_contract;
    const std::vector<uint8_t> bytes{1, 2, 3};
    const std::array<const char*, 4> bad_blob_keys{{
        NC::kPairedAt, NC::kLastTime, "unknown", "abcdefghijklmnop",
    }};
    for (const char* key : bad_blob_keys) {
        std::vector<uint8_t> out{0xAA};
        check_rejected_without_nvs([&] { return tesla.load(key, out); });
        CHECK(out.empty());
        check_rejected_without_nvs([&] { return tesla.save(key, bytes); });
        check_rejected_without_nvs([&] { return tesla.blob_exists(key); });
        bool exists = true;
        check_rejected_without_nvs([&] { return tesla.probe_blob(key, exists); });
        CHECK(!exists);
    }

    const std::array<const char*, 4> bad_string_keys{{
        NC::kPrivateKey, NC::kLastTime, "unknown", "abcdefghijklmnop",
    }};
    for (const char* key : bad_string_keys) {
        std::string out = "preserved";
        check_rejected_without_nvs([&] { return tesla.load_str(key, out); });
        CHECK(out == "preserved");
        check_rejected_without_nvs([&] {
            return tesla.load_str_state(key, out) == tk::NvsStringLoadState::Present;
        });
        CHECK(out.empty());
        check_rejected_without_nvs([&] { return tesla.save_str(key, "value"); });
    }

    const std::array<const char*, 4> bad_raw_blob_keys{{
        NC::kLastTime, NC::kPrivateKey, "unknown", "abcdefghijklmnop",
    }};
    for (const char* key : bad_raw_blob_keys) {
        std::vector<uint8_t> out{0xAA};
        check_rejected_without_nvs([&] { return config.load_blob(key, out); });
        CHECK(out.empty());
        check_rejected_without_nvs([&] {
            return config.load_blob_state(key, out) == tk::NvsBlobLoadState::Present;
        });
        CHECK(out.empty());
        check_rejected_without_nvs([&] {
            return config.save_blob(key, bytes.data(), bytes.size());
        });
    }

    for (const char* key : {NC::kDisplayRotation, NC::kPrivateKey, "unknown",
                            "abcdefghijklmnop"}) {
        check_rejected_without_nvs([&] { return config.remove(key); });
    }
    std::string null_text = "preserved";
    check_rejected_without_nvs([&] { return config.load_str(nullptr, null_text); });
    CHECK(null_text == "preserved");
    check_rejected_without_nvs([&] {
        return config.load_str_state(nullptr, null_text) ==
               tk::NvsStringLoadState::Present;
    });
    CHECK(null_text.empty());
    check_rejected_without_nvs([&] { return config.save_str(nullptr, "value"); });
    std::vector<uint8_t> null_blob{0xAA};
    check_rejected_without_nvs([&] { return config.load_blob(nullptr, null_blob); });
    CHECK(null_blob.empty());
    check_rejected_without_nvs([&] {
        return config.load_blob_state(nullptr, null_blob) ==
               tk::NvsBlobLoadState::Present;
    });
    CHECK(null_blob.empty());
    check_rejected_without_nvs([&] {
        return config.save_blob(nullptr, bytes.data(), bytes.size());
    });
}

static void test_uninitialized_no_call_guards() {
    namespace NC = tk::nvs_contract;
    NvsStorageAdapter config(NC::kConfigNamespace);
    NvsStorageAdapter tesla(NC::kTeslaBleNamespace);
    std::vector<uint8_t> blob{0xAA};
    std::string text = "preserved";
    bool exists = true;
    const std::vector<uint8_t> bytes{1};

    check_rejected_without_nvs([&] { return tesla.load(NC::kPrivateKey, blob); });
    CHECK(blob.empty());
    check_rejected_without_nvs([&] { return tesla.save(NC::kPrivateKey, bytes); });
    check_rejected_without_nvs([&] { return config.remove(NC::kLastTime); });
    check_rejected_without_nvs([&] { return tesla.blob_exists(NC::kPrivateKey); });
    check_rejected_without_nvs([&] { return tesla.probe_blob(NC::kPrivateKey, exists); });
    CHECK(!exists);
    check_rejected_without_nvs([&] { return config.load_str(NC::kLastTime, text); });
    CHECK(text == "preserved");
    check_rejected_without_nvs([&] {
        return config.load_str_state(NC::kLastTime, text) ==
               tk::NvsStringLoadState::Present;
    });
    CHECK(text.empty());
    check_rejected_without_nvs([&] { return config.save_str(NC::kLastTime, "value"); });
    check_rejected_without_nvs([&] { return config.load_blob(NC::kConfigBlob, blob); });
    CHECK(blob.empty());
    check_rejected_without_nvs([&] {
        return config.load_blob_state(NC::kConfigBlob, blob) ==
               tk::NvsBlobLoadState::Present;
    });
    CHECK(blob.empty());
    check_rejected_without_nvs([&] {
        return config.save_blob(NC::kConfigBlob, bytes.data(), bytes.size());
    });
}

static void test_blob_probe_and_write_paths(NvsStorageAdapter& tesla) {
    namespace NC = tk::nvs_contract;
    const std::vector<uint8_t> bytes{1, 2, 3};

    for (const BlobRead& read : {
             BlobRead{ESP_ERR_NVS_NOT_FOUND, 0, {}},
             BlobRead{ESP_ERR_NVS_TYPE_MISMATCH, 0, {}},
             BlobRead{ESP_FAIL, 0, {}},
             BlobRead{ESP_OK, 0, {}},
             BlobRead{ESP_OK, 3, {}},
         }) {
        script_blob_reads({read});
        clear_call_log();
        const bool expected = read.error == ESP_OK && read.length != 0;
        CHECK(tesla.blob_exists(NC::kPrivateKey) == expected);
        CHECK(nvs_call_log.size() == 1);
        check_last_call("get_blob", NC::kTeslaBleNamespace, NC::kPrivateKey);
        check_blob_script_consumed();
    }

    {
        bool exists = true;
        script_blob_reads({{ESP_ERR_NVS_NOT_FOUND, 0, {}}});
        clear_call_log();
        CHECK(tesla.probe_blob(NC::kPrivateKey, exists));
        CHECK(!exists);
        CHECK(nvs_call_log.size() == 1);
        check_last_call("get_blob", NC::kTeslaBleNamespace, NC::kPrivateKey);
        check_blob_script_consumed();
    }
    for (const esp_err_t error : {ESP_ERR_NVS_TYPE_MISMATCH, ESP_FAIL}) {
        bool exists = true;
        script_blob_reads({{error, 0, {}}});
        clear_call_log();
        CHECK(!tesla.probe_blob(NC::kPrivateKey, exists));
        CHECK(!exists);
        CHECK(nvs_call_log.size() == 1);
        check_last_call("get_blob", NC::kTeslaBleNamespace, NC::kPrivateKey);
        check_blob_script_consumed();
    }
    {
        bool exists = false;
        script_blob_reads({{ESP_OK, 0, {}}});
        clear_call_log();
        CHECK(tesla.probe_blob(NC::kPrivateKey, exists));
        CHECK(exists);  // Existing-but-empty is corruption requiring cleanup, never Missing.
        CHECK(nvs_call_log.size() == 1);
        check_blob_script_consumed();
    }

    next_set_blob_error = ESP_FAIL;
    clear_call_log();
    CHECK(!tesla.save(NC::kSessionVcsec, bytes));
    CHECK(nvs_call_log.size() == 1);
    check_call(0, "set_blob", NC::kTeslaBleNamespace, "sess_vcsec");

    next_commit_error = ESP_FAIL;
    clear_call_log();
    CHECK(!tesla.save(NC::kSessionVcsec, bytes));
    CHECK(nvs_call_log.size() == 2);
    check_call(0, "set_blob", NC::kTeslaBleNamespace, "sess_vcsec");
    check_call(1, "commit", NC::kTeslaBleNamespace);

    clear_call_log();
    CHECK(tesla.save(NC::kSessionInfotainment, bytes));
    CHECK(nvs_call_log.size() == 2);
    check_call(0, "set_blob", NC::kTeslaBleNamespace, "sess_info");
    check_call(1, "commit", NC::kTeslaBleNamespace);
}

static void test_string_read_write_paths(NvsStorageAdapter& config) {
    namespace NC = tk::nvs_contract;

    for (const StringRead& read : {
             StringRead{ESP_ERR_NVS_NOT_FOUND, 0, {}},
             StringRead{ESP_ERR_NVS_TYPE_MISMATCH, 0, {}},
             StringRead{ESP_FAIL, 0, {}},
             StringRead{ESP_OK, 0, {}},
         }) {
        std::string out = "preserved";
        script_string_reads({read});
        clear_call_log();
        CHECK(!config.load_str(NC::kLastTime, out));
        CHECK(out == "preserved");
        CHECK(nvs_call_log.size() == 1);
        check_last_call("get_str", NC::kConfigNamespace, NC::kLastTime);
        check_script_consumed();
    }

    for (const std::initializer_list<StringRead> reads : {
             std::initializer_list<StringRead>{{ESP_OK, 8, {}}, {ESP_FAIL, 8, {}}},
             std::initializer_list<StringRead>{{ESP_OK, 8, {}}, {ESP_OK, 4, "abc"}},
             std::initializer_list<StringRead>{{ESP_OK, 8, {}},
                                               {ESP_ERR_NVS_INVALID_LENGTH, 12, {}}},
         }) {
        std::string out = "preserved";
        script_string_reads(reads);
        clear_call_log();
        CHECK(!config.load_str(NC::kLastTime, out));
        CHECK(out == "preserved");
        CHECK(nvs_call_log.size() == 2);
        check_script_consumed();
    }

    {
        const std::string embedded_nul("ab\0cd", 5);
        std::string out = "preserved";
        script_string_reads({{ESP_OK, embedded_nul.size() + 1, {}},
                             {ESP_OK, embedded_nul.size() + 1, embedded_nul}});
        CHECK(!config.load_str(NC::kLastTime, out));
        CHECK(out == "preserved");
        check_script_consumed();
    }
    {
        std::string out = "preserved";
        script_string_reads({{ESP_OK, 1, {}}, {ESP_OK, 1, {}}});
        CHECK(config.load_str(NC::kLastTime, out));
        CHECK(out.empty());  // Explicit empty is a valid ordinary config value.
        check_script_consumed();
    }
    {
        const std::string value = "1700000000";
        std::string out = "preserved";
        script_string_reads({{ESP_OK, value.size() + 1, {}},
                             {ESP_OK, value.size() + 1, value}});
        clear_call_log();
        CHECK(config.load_str(NC::kLastTime, out));
        CHECK(out == value);
        CHECK(nvs_call_log.size() == 2);
        check_call(0, "get_str", NC::kConfigNamespace, NC::kLastTime);
        check_call(1, "get_str", NC::kConfigNamespace, NC::kLastTime);
        check_script_consumed();
    }

    next_set_str_error = ESP_FAIL;
    clear_call_log();
    CHECK(!config.save_str(NC::kLastTime, "1700000000"));
    CHECK(nvs_call_log.size() == 1);
    check_call(0, "set_str", NC::kConfigNamespace, NC::kLastTime);

    next_commit_error = ESP_FAIL;
    clear_call_log();
    CHECK(!config.save_str(NC::kLastTime, "1700000000"));
    CHECK(nvs_call_log.size() == 2);
    check_call(0, "set_str", NC::kConfigNamespace, NC::kLastTime);
    check_call(1, "commit", NC::kConfigNamespace);

    clear_call_log();
    CHECK(config.save_str(NC::kVinTransition, "marker"));
    CHECK(nvs_call_log.size() == 2);
    check_call(0, "set_str", NC::kConfigNamespace, NC::kVinTransition);
    check_call(1, "commit", NC::kConfigNamespace);
}

static void test_raw_blob_write_paths(NvsStorageAdapter& config) {
    namespace NC = tk::nvs_contract;
    const std::array<uint8_t, 3> bytes{{1, 2, 3}};

    check_rejected_without_nvs([&] {
        return config.save_blob(NC::kConfigBlob, nullptr, bytes.size());
    });
    check_rejected_without_nvs([&] {
        return config.save_blob(NC::kConfigBlob, bytes.data(), 0);
    });

    next_set_blob_error = ESP_FAIL;
    clear_call_log();
    CHECK(!config.save_blob(NC::kConfigBlob, bytes.data(), bytes.size()));
    CHECK(nvs_call_log.size() == 1);
    check_call(0, "set_blob", NC::kConfigNamespace, NC::kConfigBlob);

    next_commit_error = ESP_FAIL;
    clear_call_log();
    CHECK(!config.save_blob(NC::kConfigBlob, bytes.data(), bytes.size()));
    CHECK(nvs_call_log.size() == 2);
    check_call(0, "set_blob", NC::kConfigNamespace, NC::kConfigBlob);
    check_call(1, "commit", NC::kConfigNamespace);

    clear_call_log();
    CHECK(config.save_blob(NC::kConfigBlob, bytes.data(), bytes.size()));
    CHECK(nvs_call_log.size() == 2);
    check_call(0, "set_blob", NC::kConfigNamespace, NC::kConfigBlob);
    check_call(1, "commit", NC::kConfigNamespace);
}

static void test_remove_paths(NvsStorageAdapter& config, NvsStorageAdapter& tesla) {
    namespace NC = tk::nvs_contract;
    struct Case {
        NvsStorageAdapter* storage;
        const char* logical;
        const char* physical;
        const char* name_space;
    };
    const std::array<Case, 3> cases{{
        {&config, NC::kLastTime, NC::kLastTime, NC::kConfigNamespace},
        {&config, NC::kConfigBlob, NC::kConfigBlob, NC::kConfigNamespace},
        {&tesla, NC::kSessionInfotainment, "sess_info", NC::kTeslaBleNamespace},
    }};
    for (const auto& value : cases) {
        clear_call_log();
        CHECK(value.storage->remove(value.logical));
        CHECK(nvs_call_log.size() == 2);
        check_call(0, "erase_key", value.name_space, value.physical);
        check_call(1, "commit", value.name_space);
    }

    next_erase_error = ESP_FAIL;
    clear_call_log();
    CHECK(!config.remove(NC::kLastTime));
    CHECK(nvs_call_log.size() == 1);
    check_call(0, "erase_key", NC::kConfigNamespace, NC::kLastTime);

    next_erase_error = ESP_ERR_NVS_NOT_FOUND;
    next_commit_error = ESP_FAIL;
    clear_call_log();
    CHECK(!config.remove(NC::kLastTime));
    CHECK(nvs_call_log.size() == 2);
    check_call(0, "erase_key", NC::kConfigNamespace, NC::kLastTime);
    check_call(1, "commit", NC::kConfigNamespace);

    next_erase_error = ESP_ERR_NVS_NOT_FOUND;
    clear_call_log();
    CHECK(config.remove(NC::kLastTime));
    CHECK(nvs_call_log.size() == 2);
}

static void test_nvs_blob_load() {
    using B = tk::NvsBlobLoadState;
    namespace NC = tk::nvs_contract;

    // The raw ConfigBlob reader has the same safety boundary as vin_txn: only exact NOT_FOUND is
    // absence. A present-zero, probe/read fault or changing length is recovery ambiguity.
    {
        NvsStorageAdapter unavailable(NC::kConfigNamespace);
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({});
        CHECK(unavailable.load_blob_state(tk::kConfigBlobKey, out) == B::Error);
        CHECK(out.empty());
        check_blob_script_consumed();
    }

    NvsStorageAdapter storage(NC::kConfigNamespace);
    CHECK(storage.initialize());
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
    {
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({{ESP_FAIL, 0, {}}});
        CHECK(!storage.load_blob(tk::kConfigBlobKey, out));
        CHECK(out.empty());
        check_blob_script_consumed();
    }
    {
        const std::vector<uint8_t> stable{5, 6, 7};
        std::vector<uint8_t> out;
        script_blob_reads({{ESP_OK, stable.size(), {}},
                           {ESP_OK, stable.size(), stable}});
        clear_call_log();
        CHECK(storage.load_blob(tk::kConfigBlobKey, out));
        CHECK(out == stable);
        CHECK(nvs_call_log.size() == 2);
        check_call(0, "get_blob", NC::kConfigNamespace, NC::kConfigBlob);
        check_call(1, "get_blob", NC::kConfigNamespace, NC::kConfigBlob);
        check_blob_script_consumed();
    }
}

int main() {
    using S = tk::NvsStringLoadState;
    using C = tk::ConfigLoadState;
    namespace NC = tk::nvs_contract;

    test_nvs_contract();
    test_uninitialized_no_call_guards();
    test_nvs_blob_load();

    // Uninitialised storage is an error, not an absent journal, and performs no NVS read.
    {
        NvsStorageAdapter storage(NC::kConfigNamespace);
        std::string out = "stale";
        script_string_reads({});
        CHECK(storage.load_str_state(NC::kVinTransition, out) == S::Error);
        CHECK(out.empty());
        check_script_consumed();
    }

    NvsStorageAdapter storage(NC::kConfigNamespace);
    CHECK(storage.initialize());
    CHECK(last_namespace == NC::kConfigNamespace);
    CHECK(!nvs_call_log.empty());
    CHECK(nvs_call_log.back().mode == NVS_READWRITE);
    NvsStorageAdapter tesla_storage(NC::kTeslaBleNamespace);
    CHECK(tesla_storage.initialize());
    CHECK(last_namespace == NC::kTeslaBleNamespace);
    CHECK(!nvs_call_log.empty());
    CHECK(nvs_call_log.back().mode == NVS_READWRITE);

    test_registered_physical_key_mapping(storage, tesla_storage);
    test_adapter_no_call_guards(storage, tesla_storage);
    test_blob_probe_and_write_paths(tesla_storage);
    test_string_read_write_paths(storage);
    test_raw_blob_write_paths(storage);
    test_remove_paths(storage, tesla_storage);

    {
        const size_t before = nvs_calls;
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({});
        CHECK(!tesla_storage.load("abcdefghijklmnop", out));
        CHECK(!tesla_storage.load("unknown", out));
        CHECK(!storage.load(NC::kPrivateKey, out));
        CHECK(nvs_calls == before);
        CHECK(out.empty());
        check_blob_script_consumed();
    }

    // tesla-ble's generic StorageAdapter path loads the durable private key and sessions. Its
    // probe and data read must describe one stable blob: accepting a shorter second value can put
    // key B in Vehicle RAM while a later fingerprint read observes durable key A, invalidating the
    // armed-VIN recovery decision. Failures must not expose a partial/stale candidate to callers.
    {
        NvsStorageAdapter unavailable(NC::kTeslaBleNamespace);
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({});
        CHECK(!unavailable.load(NC::kPrivateKey, out));
        CHECK(out.empty());
        check_blob_script_consumed();
    }
    {
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({{ESP_ERR_NVS_NOT_FOUND, 0, {}}});
        CHECK(!tesla_storage.load(NC::kPrivateKey, out));
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
        CHECK(!tesla_storage.load(NC::kPrivateKey, out));
        CHECK(out.empty());
        check_blob_script_consumed();
    }
    {
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({{ESP_OK, 0, {}}});
        CHECK(!tesla_storage.load(NC::kPrivateKey, out));
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
        CHECK(!tesla_storage.load(NC::kPrivateKey, out));
        CHECK(out.empty());
        check_blob_script_consumed();
    }
    {
        const std::vector<uint8_t> stable{1, 2, 0, 4};
        std::vector<uint8_t> out{0xAA};
        script_blob_reads({{ESP_OK, stable.size(), {}},
                           {ESP_OK, stable.size(), stable}});
        CHECK(tesla_storage.load(NC::kSessionVcsec, out));
        CHECK(out == stable);  // generic session blobs are binary; embedded NUL is valid
        CHECK(last_blob_key == "sess_vcsec");
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
