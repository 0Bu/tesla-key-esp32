// Atomic credential/service config storage (see config_blob.hpp). The encoding, the CRC and the
// version rules are the pure, host-tested logic/config_store.hpp; this file is the NVS glue.
#include "config_blob.hpp"

#include "nvs_storage.hpp"
#include "sdkconfig.h"

#include <esp_log.h>

#include <atomic>
#include <vector>
#include <utility>

static const char* TAG = "config_blob";

namespace tk {

namespace {
ConfigBlobBuffer s_encode_buffer{};
std::atomic_flag s_encode_active = ATOMIC_FLAG_INIT;

// Whole cfg_save operation ownership, not a leaf lock: no waiting, allocation or callout in
// acquisition/release. A busy/reentrant caller touches no bytes and performs no NVS/log work.
struct ConfigEncodeLease {
    bool held;
    ConfigEncodeLease() noexcept : held(!s_encode_active.test_and_set(std::memory_order_acquire)) {}
    ~ConfigEncodeLease() {
        if (!held) return;
        volatile uint8_t* bytes = s_encode_buffer.data();
        for (size_t i = 0; i < s_encode_buffer.size(); ++i) bytes[i] = 0;
        s_encode_active.clear(std::memory_order_release);
    }
    ConfigEncodeLease(const ConfigEncodeLease&) = delete;
    ConfigEncodeLease& operator=(const ConfigEncodeLease&) = delete;
};
} // namespace

static std::atomic<bool> s_wifi_rolled_back{false};

void cfg_init_wifi_rollback_status(bool rolled_back) noexcept {
    s_wifi_rolled_back.store(rolled_back, std::memory_order_relaxed);
}

bool cfg_wifi_rolled_back() noexcept {
    return s_wifi_rolled_back.load(std::memory_order_relaxed);
}

static bool load_legacy(NvsStorageAdapter& cfg, ConfigBlob& out) {
    // Legacy per-key layout: a fresh device, or one that has not saved anything since upgrading to
    // the blob. Start from the build defaults and let an existing legacy key override them,
    // including an explicitly stored empty string. Centralising this matters: callers used to seed
    // defaults differently, so the first unrelated blob save could silently turn a Kconfig MQTT or
    // Syslog default into an explicit disable.
    ConfigBlob legacy;
    legacy.wifi_ssid = CONFIG_TESLA_WIFI_SSID;
    legacy.wifi_pass = CONFIG_TESLA_WIFI_PASSWORD;
    legacy.vin       = CONFIG_TESLA_VIN;
    legacy.mqtt_uri  = CONFIG_TESLA_MQTT_BROKER_URI;
    legacy.syslog_uri = CONFIG_TESLA_SYSLOG_SERVER;
    const auto read = [&cfg](const char* key, std::string& field) {
        std::string value;
        const NvsStringLoadState state = cfg.load_str_state(key, value, /*allow_empty=*/true);
        if (state == NvsStringLoadState::Present) field.swap(value);
        return state != NvsStringLoadState::Error;
    };
    if (!read(nvs_contract::kLegacyWifiSsid, legacy.wifi_ssid) ||
        !read(nvs_contract::kLegacyWifiPass, legacy.wifi_pass) ||
        !read(nvs_contract::kLegacyVin, legacy.vin) ||
        !read(nvs_contract::kLegacyMqttUri, legacy.mqtt_uri) ||
        !read(nvs_contract::kLegacySyslogUri, legacy.syslog_uri)) {
        ESP_LOGE(TAG, "legacy configuration read failed — refusing migration");
        return false;
    }
    out = std::move(legacy);
    return true;
}

ConfigLoadState cfg_load_state(NvsStorageAdapter& cfg, ConfigBlob& out) {
    std::vector<uint8_t> raw;
    const NvsBlobLoadState raw_state = cfg.load_blob_state(kConfigBlobKey, raw);
    if (raw_state == NvsBlobLoadState::Error) {
        ESP_LOGE(TAG, "config blob could not be read authoritatively");
        return ConfigLoadState::Error;
    }
    if (raw_state == NvsBlobLoadState::Missing) {
        if (!load_legacy(cfg, out)) return ConfigLoadState::Error;
        return ConfigLoadState::Legacy;
    }
    if (!config_blob_decode(raw.data(), raw.size(), out)) {
        ESP_LOGE(TAG, "config blob present but failed CRC/schema decoding");
        return ConfigLoadState::Error;
    }
    return ConfigLoadState::Blob;
}

bool cfg_load(NvsStorageAdapter& cfg, ConfigBlob& out) {
    const ConfigLoadState state = cfg_load_state(cfg, out);
    if (state == ConfigLoadState::Blob) return true;
    if (state == ConfigLoadState::Legacy) return false;
    // Error state: do NOT fall back to stale legacy mirrors. Leave out untouched.
    return false;
}

bool cfg_load_for_update(NvsStorageAdapter& cfg, ConfigBlob& out) {
    ConfigBlob loaded;
    const ConfigLoadState state = cfg_load_state(cfg, loaded);
    if (state == ConfigLoadState::Error) {
        ESP_LOGE(TAG, "config blob unreadable — refusing to build a configuration change on "
                      "legacy mirrors");
        return false;
    }
    out = std::move(loaded);
    return true;
}

bool cfg_save(NvsStorageAdapter& cfg, const ConfigBlob& in) {
    ConfigEncodeLease lease;
    if (!lease.held) return false;
    auto& buf = s_encode_buffer;
    const size_t n = config_blob_encode(in, buf.data(), buf.size());
    if (n == 0) {
        // Encoding refuses rather than truncates when a field is over its bound, so this means the
        // caller handed us something it should have validated. Never write a partial config.
        ESP_LOGE(TAG, "config blob refused encoding (a field is over its length bound) — "
                      "nothing was written");
        return false;
    }
    if (!cfg.save_blob(kConfigBlobKey, buf.data(), n)) {
        ESP_LOGE(TAG, "config blob write/commit was not confirmed");
        return false;
    }

    // Publish only durable state, before fallible legacy mirroring or HTTP reply/restart work.
    s_wifi_rolled_back.store(in.wifi_rolled_back, std::memory_order_relaxed);

    // Mirror into the legacy keys so a DOWNGRADE to a build that predates the blob still finds its
    // configuration. Best-effort by design and deliberately after the blob: the blob is the source
    // of truth from here on, so a failure to mirror costs only the downgrade path, and turning that
    // into a failed save would reject a credential change that has already been committed durably.
    if (!cfg.save_str(nvs_contract::kLegacyWifiSsid, in.wifi_ssid) ||
        !cfg.save_str(nvs_contract::kLegacyWifiPass, in.wifi_pass) ||
        !cfg.save_str(nvs_contract::kLegacyVin, in.vin) ||
        !cfg.save_str(nvs_contract::kLegacyMqttUri, in.mqtt_uri) ||
        !cfg.save_str(nvs_contract::kLegacySyslogUri, in.syslog_uri)) {
        ESP_LOGW(TAG, "config saved, but the legacy key mirror is incomplete — only a downgrade to "
                      "a pre-blob build would notice");
    }
    return true;
}

}  // namespace tk
