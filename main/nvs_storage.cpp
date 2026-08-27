#include "nvs_storage.hpp"
#include <esp_log.h>
#include <cstring>

static const char* TAG = "nvs_storage";

NvsStorageAdapter::NvsStorageAdapter(const char* namespace_name)
    : ns_(namespace_name),
      ns_kind_(tk::nvs_contract::classify_namespace(namespace_name ? namespace_name : "")) {}

NvsStorageAdapter::~NvsStorageAdapter() {
    if (initialized_) {
        nvs_close(handle_);
    }
}

bool NvsStorageAdapter::initialize() {
    if (ns_kind_ == tk::nvs_contract::Namespace::Unknown) {
        ESP_LOGE(TAG, "refusing unregistered NVS namespace");
        return false;
    }
    esp_err_t err = nvs_open(ns_, NVS_READWRITE, &handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    initialized_ = true;
    return true;
}

const char* NvsStorageAdapter::map_key(std::string_view key,
                                      tk::nvs_contract::StorageApi api,
                                      bool erase_any_api) const {
    const auto* entry = tk::nvs_contract::find(ns_kind_, key);
    if (!entry || (!erase_any_api && entry->api != api) ||
        entry->api == tk::nvs_contract::StorageApi::DirectU8) {
        ESP_LOGE(TAG, "refusing unregistered NVS key/API in namespace '%s'", ns_ ? ns_ : "?");
        return nullptr;
    }
    return entry->stored_key.data();
}

bool NvsStorageAdapter::load(const std::string& key, std::vector<uint8_t>& buffer) {
    buffer.clear();
    if (!initialized_) return false;
    const char* nvskey = map_key(key, tk::nvs_contract::StorageApi::Blob);
    if (!nvskey) return false;
    size_t len = 0;
    esp_err_t err = nvs_get_blob(handle_, nvskey, nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load probe failed '%s': %s", nvskey, esp_err_to_name(err));
        return false;
    }
    if (len == 0) {
        ESP_LOGW(TAG, "load probe returned empty blob '%s'", nvskey);
        return false;
    }

    const size_t probed_len = len;
    try {
        std::vector<uint8_t> candidate(probed_len);
        size_t read_len = probed_len;
        err = nvs_get_blob(handle_, nvskey, candidate.data(), &read_len);
        if (err != ESP_OK || read_len != probed_len) {
            // A changing record is not a valid snapshot. In particular, accepting a shorter
            // private-key read would expose runtime key B in a vector still sized for probed key A,
            // while a later NVS fingerprint could classify the durable identity differently.
            ESP_LOGE(TAG, "load failed '%s': %s (probe=%u read=%u)", nvskey,
                     esp_err_to_name(err), static_cast<unsigned>(probed_len),
                     static_cast<unsigned>(read_len));
            return false;
        }
        buffer = std::move(candidate);
        return true;
    } catch (...) {
        buffer.clear();
        ESP_LOGE(TAG, "load failed '%s': allocation/copy failure", nvskey);
        return false;
    }
}

bool NvsStorageAdapter::blob_exists(const std::string& key) const {
    if (!initialized_) return false;
    // map_key() returns a pointer into the constexpr registry, so this hot existence probe does
    // not allocate or materialise the blob in a vector.
    const char* nvskey = map_key(key, tk::nvs_contract::StorageApi::Blob);
    if (!nvskey) return false;
    size_t len = 0;
    esp_err_t err = nvs_get_blob(handle_, nvskey, nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || len == 0) return false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "exists probe failed '%s': %s", nvskey, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool NvsStorageAdapter::probe_blob(const std::string& key, bool& exists) const {
    exists = false;
    if (!initialized_) {
        ESP_LOGE(TAG, "probe unavailable '%s': NVS namespace is not initialized", key.c_str());
        return false;
    }
    const char* nvskey = map_key(key, tk::nvs_contract::StorageApi::Blob);
    if (!nvskey) return false;
    size_t len = 0;
    const esp_err_t err = nvs_get_blob(handle_, nvskey, nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "safety probe failed '%s': %s", nvskey, esp_err_to_name(err));
        return false;
    }
    // Any existing record is safety-relevant. A zero-length key cannot be produced by our
    // one-byte marker writer, so treat it as Present (cleanup required), never as Missing.
    exists = true;
    return true;
}

bool NvsStorageAdapter::save(const std::string& key, const std::vector<uint8_t>& buffer) {
    if (!initialized_) return false;
    const char* nvskey = map_key(key, tk::nvs_contract::StorageApi::Blob);
    if (!nvskey) return false;
    esp_err_t err = nvs_set_blob(handle_, nvskey, buffer.data(), buffer.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed '%s': %s", nvskey, esp_err_to_name(err));
        return false;
    }
    err = nvs_commit(handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit failed after save '%s': %s", nvskey, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool NvsStorageAdapter::remove(const std::string& key) {
    if (!initialized_) return false;
    const char* nvskey = map_key(key, tk::nvs_contract::StorageApi::Blob, true);
    if (!nvskey) return false;
    esp_err_t err = nvs_erase_key(handle_, nvskey);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "remove failed '%s': %s", nvskey, esp_err_to_name(err));
        return false;
    }
    err = nvs_commit(handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit failed after remove '%s': %s", nvskey, esp_err_to_name(err));
        return false;
    }
    return true;
}

// The string API is constrained to entries registered as String. A future long or misspelled key
// is rejected rather than truncated or silently treated as an absent record.
bool NvsStorageAdapter::load_str(const char* key, std::string& out) {
    if (!initialized_) return false;
    const char* nvskey = map_key(key ? key : "", tk::nvs_contract::StorageApi::String);
    if (!nvskey) return false;
    size_t len = 0;
    esp_err_t err = nvs_get_str(handle_, nvskey, nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || len == 0) return false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load_str failed '%s': %s", nvskey, esp_err_to_name(err));
        return false;
    }

    const size_t probed_len = len;
    try {
        std::vector<char> buf(probed_len);
        size_t read_len = probed_len;
        err = nvs_get_str(handle_, nvskey, buf.data(), &read_len);
        const bool value_well_formed = err == ESP_OK && read_len == probed_len &&
            buf.back() == '\0' &&
            std::memchr(buf.data(), '\0', probed_len - 1) == nullptr;
        if (!value_well_formed) {
            ESP_LOGE(TAG, "load_str failed '%s': %s (probe=%u read=%u)", nvskey,
                     esp_err_to_name(err), static_cast<unsigned>(probed_len),
                     static_cast<unsigned>(read_len));
            return false;
        }

        // Build the complete replacement before touching the caller's value. std::string::swap
        // with the default allocator is noexcept, so every read/allocation/copy failure preserves
        // the previous output exactly. A one-byte NVS string is the valid explicit empty value
        // used to disable MQTT/Syslog and therefore remains distinguishable from NOT_FOUND.
        std::string candidate(buf.data(), probed_len - 1);
        out.swap(candidate);
        return true;
    } catch (...) {
        ESP_LOGE(TAG, "load_str failed '%s': allocation/copy failure", nvskey);
        return false;
    }
}

tk::NvsStringLoadState NvsStorageAdapter::load_str_state(const char* key, std::string& out) {
    out.clear();
    if (!initialized_) {
        ESP_LOGE(TAG, "safety string read unavailable '%s': NVS namespace is not initialized", key);
        return tk::NvsStringLoadState::Error;
    }

    const char* nvskey = map_key(key ? key : "", tk::nvs_contract::StorageApi::String);
    if (!nvskey) return tk::NvsStringLoadState::Error;
    size_t len = 0;
    esp_err_t err = nvs_get_str(handle_, nvskey, nullptr, &len);
    const tk::NvsStringProbe probe =
        err == ESP_OK ? tk::NvsStringProbe::Ok
                      : err == ESP_ERR_NVS_NOT_FOUND ? tk::NvsStringProbe::NotFound
                                                     : tk::NvsStringProbe::Error;
    if (probe == tk::NvsStringProbe::NotFound) return tk::NvsStringLoadState::Missing;
    if (probe == tk::NvsStringProbe::Error || len <= 1) {
        ESP_LOGE(TAG, "safety string probe failed '%s': %s (len=%u)", nvskey,
                 esp_err_to_name(err), static_cast<unsigned>(len));
        return tk::NvsStringLoadState::Error;
    }

    const size_t probed_len = len;
    try {
        std::vector<char> buf(probed_len);
        size_t read_len = probed_len;
        err = nvs_get_str(handle_, nvskey, buf.data(), &read_len);
        const bool read_ok = err == ESP_OK;
        const bool value_well_formed = read_ok && read_len == probed_len &&
            buf.front() != '\0' && buf.back() == '\0' &&
            std::memchr(buf.data(), '\0', probed_len - 1) == nullptr;
        const tk::NvsStringLoadState state = tk::classify_nvs_string_load(
            probe, probed_len, read_ok, read_len, value_well_formed);
        if (state != tk::NvsStringLoadState::Present) {
            ESP_LOGE(TAG, "safety string read failed '%s': %s (probe=%u read=%u)",
                     nvskey, esp_err_to_name(err),
                     static_cast<unsigned>(probed_len), static_cast<unsigned>(read_len));
            return tk::NvsStringLoadState::Error;
        }
        out.assign(buf.data(), probed_len - 1);
        return tk::NvsStringLoadState::Present;
    } catch (...) {
        out.clear();
        ESP_LOGE(TAG, "safety string read failed '%s': allocation/copy failure", nvskey);
        return tk::NvsStringLoadState::Error;
    }
}

bool NvsStorageAdapter::save_str(const char* key, const std::string& value) {
    if (!initialized_) return false;
    const char* nvskey = map_key(key ? key : "", tk::nvs_contract::StorageApi::String);
    if (!nvskey) return false;
    esp_err_t err = nvs_set_str(handle_, nvskey, value.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_str failed '%s': %s", nvskey, esp_err_to_name(err));
        return false;
    }
    err = nvs_commit(handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit failed after save_str '%s': %s", nvskey, esp_err_to_name(err));
        return false;
    }
    return true;
}

// ── Raw blob helpers for the atomic config store (logic/config_store.hpp) ──
// Kept separate from the generic tesla-ble Blob API, but still routed through map_key() as the
// registry's sole RawBlob entry. One nvs_set_blob + one commit is the whole point — the
// credential/service state becomes all-or-nothing across a write failure AND a power cut,
// instead of the per-key sequence it replaces.

bool NvsStorageAdapter::load_blob(const char* key, std::vector<uint8_t>& out) {
    return load_blob_state(key, out) == tk::NvsBlobLoadState::Present;
}

tk::NvsBlobLoadState NvsStorageAdapter::load_blob_state(const char* key,
                                                         std::vector<uint8_t>& out) {
    out.clear();
    if (!initialized_) {
        ESP_LOGE(TAG, "safety blob read unavailable '%s': NVS namespace is not initialized", key);
        return tk::NvsBlobLoadState::Error;
    }
    const char* nvskey = map_key(key ? key : "", tk::nvs_contract::StorageApi::RawBlob);
    if (!nvskey) return tk::NvsBlobLoadState::Error;
    size_t len = 0;
    esp_err_t err = nvs_get_blob(handle_, nvskey, nullptr, &len);
    const tk::NvsBlobProbe probe =
        err == ESP_OK ? tk::NvsBlobProbe::Ok
                      : err == ESP_ERR_NVS_NOT_FOUND ? tk::NvsBlobProbe::NotFound
                                                     : tk::NvsBlobProbe::Error;
    if (probe == tk::NvsBlobProbe::NotFound) return tk::NvsBlobLoadState::Missing;
    if (probe != tk::NvsBlobProbe::Ok || len == 0) {
        ESP_LOGE(TAG, "safety blob probe failed '%s': %s (len=%u)", nvskey,
                 esp_err_to_name(err), static_cast<unsigned>(len));
        return tk::NvsBlobLoadState::Error;
    }

    const size_t probed_len = len;
    try {
        std::vector<uint8_t> candidate(probed_len);
        err = nvs_get_blob(handle_, nvskey, candidate.data(), &len);
        const tk::NvsBlobLoadState state = tk::classify_nvs_blob_load(
            probe, probed_len, err == ESP_OK, len);
        if (state != tk::NvsBlobLoadState::Present) {
            ESP_LOGE(TAG, "safety blob read failed '%s': %s (probe=%u read=%u)", nvskey,
                     esp_err_to_name(err), static_cast<unsigned>(probed_len),
                     static_cast<unsigned>(len));
            return tk::NvsBlobLoadState::Error;
        }
        out = std::move(candidate);
        return tk::NvsBlobLoadState::Present;
    } catch (...) {
        out.clear();
        ESP_LOGE(TAG, "safety blob read failed '%s': allocation/copy failure", nvskey);
        return tk::NvsBlobLoadState::Error;
    }
}

bool NvsStorageAdapter::save_blob(const char* key, const uint8_t* data, size_t len) {
    if (!initialized_ || data == nullptr || len == 0) return false;
    const char* nvskey = map_key(key ? key : "", tk::nvs_contract::StorageApi::RawBlob);
    if (!nvskey) return false;
    esp_err_t err = nvs_set_blob(handle_, nvskey, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_blob failed '%s': %s", nvskey, esp_err_to_name(err));
        return false;
    }
    err = nvs_commit(handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit failed after save_blob '%s': %s", nvskey, esp_err_to_name(err));
        return false;
    }
    return true;
}
