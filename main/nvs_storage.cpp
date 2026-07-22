#include "nvs_storage.hpp"
#include <esp_log.h>
#include <cstring>

static const char* TAG = "nvs_storage";

NvsStorageAdapter::NvsStorageAdapter(const char* namespace_name) : ns_(namespace_name) {}

NvsStorageAdapter::~NvsStorageAdapter() {
    if (initialized_) {
        nvs_close(handle_);
    }
}

bool NvsStorageAdapter::initialize() {
    esp_err_t err = nvs_open(ns_, NVS_READWRITE, &handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    initialized_ = true;
    return true;
}

// NVS keys are max 15 chars. Map known long library keys to short ones.
std::string NvsStorageAdapter::map_key(const std::string& key) const {
    if (key == "session_infotainment") return "sess_info";  // ≤15 chars; "sess_infotainmnt" was 16 → KEY_TOO_LONG
    if (key == "session_vcsec")        return "sess_vcsec";
    if (key == "private_key")          return "private_key";
    // Truncate to 15 chars as last resort (should not happen)
    if (key.length() <= 15) return key;
    return key.substr(0, 15);
}

bool NvsStorageAdapter::load(const std::string& key, std::vector<uint8_t>& buffer) {
    if (!initialized_) return false;
    std::string nvskey = map_key(key);
    size_t len = 0;
    esp_err_t err = nvs_get_blob(handle_, nvskey.c_str(), nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || len == 0) return false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load probe failed '%s': %s", nvskey.c_str(), esp_err_to_name(err));
        return false;
    }
    buffer.resize(len);
    err = nvs_get_blob(handle_, nvskey.c_str(), buffer.data(), &len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load failed '%s': %s", nvskey.c_str(), esp_err_to_name(err));
        return false;
    }
    return true;
}

bool NvsStorageAdapter::blob_exists(const std::string& key) const {
    if (!initialized_) return false;
    // map_key() returns a ≤15-char key, so it is small-string-optimised (no heap). Unlike
    // load(), this probes only the stored length and never resizes/reads the blob into a
    // vector — the point is a heap-free existence test on a repeatedly-sampled hot path.
    std::string nvskey = map_key(key);
    size_t len = 0;
    esp_err_t err = nvs_get_blob(handle_, nvskey.c_str(), nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || len == 0) return false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "exists probe failed '%s': %s", nvskey.c_str(), esp_err_to_name(err));
        return false;
    }
    return true;
}

bool NvsStorageAdapter::save(const std::string& key, const std::vector<uint8_t>& buffer) {
    if (!initialized_) return false;
    std::string nvskey = map_key(key);
    esp_err_t err = nvs_set_blob(handle_, nvskey.c_str(), buffer.data(), buffer.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed '%s': %s", nvskey.c_str(), esp_err_to_name(err));
        return false;
    }
    err = nvs_commit(handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit failed after save '%s': %s", nvskey.c_str(), esp_err_to_name(err));
        return false;
    }
    return true;
}

bool NvsStorageAdapter::remove(const std::string& key) {
    if (!initialized_) return false;
    std::string nvskey = map_key(key);
    esp_err_t err = nvs_erase_key(handle_, nvskey.c_str());
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "remove failed '%s': %s", nvskey.c_str(), esp_err_to_name(err));
        return false;
    }
    err = nvs_commit(handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit failed after remove '%s': %s", nvskey.c_str(), esp_err_to_name(err));
        return false;
    }
    return true;
}

// The string API goes through map_key too — today every string key is ≤15 chars so the
// mapping is an identity, but a future long key would otherwise fail silently with
// ESP_ERR_NVS_KEY_TOO_LONG (load_str previously didn't even log).
bool NvsStorageAdapter::load_str(const char* key, std::string& out) {
    if (!initialized_) return false;
    std::string nvskey = map_key(key);
    size_t len = 0;
    esp_err_t err = nvs_get_str(handle_, nvskey.c_str(), nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || len == 0) return false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load_str failed '%s': %s", nvskey.c_str(), esp_err_to_name(err));
        return false;
    }
    std::vector<char> buf(len);
    err = nvs_get_str(handle_, nvskey.c_str(), buf.data(), &len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load_str failed '%s': %s", nvskey.c_str(), esp_err_to_name(err));
        return false;
    }
    out.assign(buf.data());
    return true;
}

bool NvsStorageAdapter::save_str(const char* key, const std::string& value) {
    if (!initialized_) return false;
    std::string nvskey = map_key(key);
    esp_err_t err = nvs_set_str(handle_, nvskey.c_str(), value.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_str failed '%s': %s", nvskey.c_str(), esp_err_to_name(err));
        return false;
    }
    err = nvs_commit(handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit failed after save_str '%s': %s", nvskey.c_str(), esp_err_to_name(err));
        return false;
    }
    return true;
}
