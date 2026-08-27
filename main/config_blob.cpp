// Atomic credential/service config storage (see config_blob.hpp). The encoding, the CRC and the
// version rules are the pure, host-tested logic/config_store.hpp; this file is the NVS glue.
#include "config_blob.hpp"

#include "nvs_storage.hpp"
#include "sdkconfig.h"

#include <esp_log.h>

#include <vector>
#include <utility>

static const char* TAG = "config_blob";

namespace tk {

static void load_legacy(NvsStorageAdapter& cfg, ConfigBlob& out) {
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
    cfg.load_str(nvs_contract::kLegacyWifiSsid, legacy.wifi_ssid);
    cfg.load_str(nvs_contract::kLegacyWifiPass, legacy.wifi_pass);
    cfg.load_str(nvs_contract::kLegacyVin, legacy.vin);
    cfg.load_str(nvs_contract::kLegacyMqttUri, legacy.mqtt_uri);
    cfg.load_str(nvs_contract::kLegacySyslogUri, legacy.syslog_uri);
    out = std::move(legacy);
}

ConfigLoadState cfg_load_state(NvsStorageAdapter& cfg, ConfigBlob& out) {
    std::vector<uint8_t> raw;
    const NvsBlobLoadState raw_state = cfg.load_blob_state(kConfigBlobKey, raw);
    if (raw_state == NvsBlobLoadState::Error) {
        ESP_LOGE(TAG, "config blob could not be read authoritatively");
        return ConfigLoadState::Error;
    }
    if (raw_state == NvsBlobLoadState::Missing) {
        load_legacy(cfg, out);
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
    if (state == ConfigLoadState::Error) {
        // Compatibility path for ordinary, unjournaled boots/callers. Recovery code must use the
        // tri-state API above and fail closed instead of reaching this legacy fallback.
        ESP_LOGW(TAG, "config blob unavailable/invalid — falling back to legacy per-key values");
        load_legacy(cfg, out);
    }
    return false;
}

bool cfg_save(NvsStorageAdapter& cfg, const ConfigBlob& in) {
    ConfigBlobBuffer buf{};
    const size_t n = config_blob_encode(in, buf.data(), buf.size());
    if (n == 0) {
        // Encoding refuses rather than truncates when a field is over its bound, so this means the
        // caller handed us something it should have validated. Never write a partial config.
        ESP_LOGE(TAG, "config blob refused encoding (a field is over its length bound) — "
                      "nothing was written");
        return false;
    }
    if (!cfg.save_blob(kConfigBlobKey, buf.data(), n)) {
        ESP_LOGE(TAG, "config blob write failed — the PREVIOUS configuration is intact");
        return false;
    }

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
