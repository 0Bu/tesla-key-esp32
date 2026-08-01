// Atomic credential/service config storage (see config_blob.hpp). The encoding, the CRC and the
// version rules are the pure, host-tested logic/config_store.hpp; this file is the NVS glue.
#include "config_blob.hpp"

#include "nvs_storage.hpp"

#include <esp_log.h>

#include <vector>

static const char* TAG = "config_blob";

namespace tk {

bool cfg_load(NvsStorageAdapter& cfg, ConfigBlob& out) {
    std::vector<uint8_t> raw;
    if (cfg.load_blob(kConfigBlobKey, raw) && !raw.empty()) {
        if (config_blob_decode(raw.data(), raw.size(), out)) return true;
        // Decoded nothing usable. This is the one place where being loud matters more than being
        // tidy: a CRC failure means the stored credentials are unreadable, and the device is about
        // to fall back to values that may be much older. Saying so on /diag (and therefore syslog)
        // is what separates "the user changed the WiFi" from "the config partition is going bad".
        ESP_LOGW(TAG, "config blob present but failed to decode (bad CRC or a newer format) — "
                      "falling back to the legacy per-key values");
    }

    // Legacy per-key layout: a fresh device, or one that has not saved anything since upgrading to
    // the blob. NOT an error path — it is the normal state of every already-deployed board.
    cfg.load_str("wifi_ssid", out.wifi_ssid);
    cfg.load_str("wifi_pass", out.wifi_pass);
    cfg.load_str("vin",       out.vin);
    cfg.load_str("mqtt_uri",  out.mqtt_uri);
    cfg.load_str("syslog_uri", out.syslog_uri);
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
    if (!cfg.save_str("wifi_ssid", in.wifi_ssid) ||
        !cfg.save_str("wifi_pass", in.wifi_pass) ||
        !cfg.save_str("vin",       in.vin) ||
        !cfg.save_str("mqtt_uri",  in.mqtt_uri) ||
        !cfg.save_str("syslog_uri", in.syslog_uri)) {
        ESP_LOGW(TAG, "config saved, but the legacy key mirror is incomplete — only a downgrade to "
                      "a pre-blob build would notice");
    }
    return true;
}

}  // namespace tk
