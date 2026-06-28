#include "ota_update.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_app_desc.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_ota_ops.h>
#include <esp_crt_bundle.h>
#include <cJSON.h>

#include "platform.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>

static const char* TAG = "ota";

// Short per-target image suffix so "esp32" appears only once in the OTA filename:
// esp32 -> "" (tesla-key-esp32.bin), esp32s3 -> "-s3", esp32c3 -> "-c3", esp32c6 -> "-c6".
// Must stay in lockstep with image_suffix() in scripts/ci-build-all.sh + build-pages.sh
// (which name the published asset the device pulls) — a mismatch 404s every OTA download.
// Kept as a string-literal macro because the download URL is assembled by compile-time
// concatenation below; the static_assert ties it to the host-tested tk::image_suffix()
// so the macro and the pure mapping can never drift.
#if   defined(CONFIG_IDF_TARGET_ESP32)
#  define TESLA_OTA_IMG_SUFFIX ""
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#  define TESLA_OTA_IMG_SUFFIX "-s3"
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#  define TESLA_OTA_IMG_SUFFIX "-c3"
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
#  define TESLA_OTA_IMG_SUFFIX "-c6"
#else
#  error "Unsupported CONFIG_IDF_TARGET for OTA image naming"
#endif
static_assert(std::string_view{TESLA_OTA_IMG_SUFFIX} == tk::image_suffix(TK_TARGET),
              "OTA image suffix macro drifted from tk::image_suffix()");

// ─── Shared status (written by the OTA task, read by HTTP handlers) ────────────

static SemaphoreHandle_t s_lock = nullptr;
static OtaStatus         s_status = { OtaState::Idle, 0, "idle", "", false, "" };
static std::atomic<bool> s_running{false};    // a check or download task is active

static void ensure_lock() {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

static void set_state(OtaState st, int pct, const char* msg) {
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state    = st;
    s_status.progress = pct;
    s_status.message  = msg ? msg : "";
    xSemaphoreGive(s_lock);
}

static void set_available(const char* ver) {
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.available = ver ? ver : "";
    xSemaphoreGive(s_lock);
}

OtaStatus ota_get_status() {
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    OtaStatus copy = s_status;
    xSemaphoreGive(s_lock);
    return copy;
}

// ─── Version comparison (semver-ish "x.y.z") ───────────────────────────────────

static void parse_ver(const char* s, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    if (s) sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
}

// true if version a is strictly newer than b
static bool ver_newer(const char* a, const char* b) {
    int va[3], vb[3];
    parse_ver(a, va);
    parse_ver(b, vb);
    for (int i = 0; i < 3; ++i) {
        if (va[i] != vb[i]) return va[i] > vb[i];
    }
    return false;
}

static const char* running_version() {
    return esp_app_get_description()->version;
}

// ─── Small HTTPS GET into a buffer (for the tiny manifest.json) ─────────────────

static bool http_get_to_buffer(const char* url, std::string& out) {
    esp_http_client_config_t cfg = {};
    cfg.url               = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // verify against bundled CA roots
    cfg.timeout_ms        = 10000;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;

    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            char buf[512];
            int  r;
            out.clear();
            while ((r = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
                out.append(buf, r);
                if (out.size() > 8192) break;   // manifest is tiny; bound the read
            }
            ok = !out.empty();
        } else {
            ESP_LOGW(TAG, "manifest HTTP status %d", status);
        }
    } else {
        ESP_LOGW(TAG, "manifest connection failed");
    }

    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

// ─── Check for a newer release ──────────────────────────────────────────────────

OtaCheckResult ota_check() {
    OtaCheckResult res{};
    res.current = running_version();

    set_state(OtaState::Checking, 0, "checking for updates");
    ESP_LOGI(TAG, "checking %s (running %s)", CONFIG_TESLA_OTA_MANIFEST_URL, res.current.c_str());

    std::string body;
    if (!http_get_to_buffer(CONFIG_TESLA_OTA_MANIFEST_URL, body)) {
        res.ok     = false;
        res.reason = "could not reach update server";
        set_state(OtaState::Idle, 0, "check failed");
        return res;
    }

    cJSON* j = cJSON_Parse(body.c_str());
    cJSON* v = j ? cJSON_GetObjectItemCaseSensitive(j, "version") : nullptr;
    if (cJSON_IsString(v) && v->valuestring) res.available = v->valuestring;
    cJSON_Delete(j);

    if (res.available.empty()) {
        res.ok     = false;
        res.reason = "no version in manifest";
        set_state(OtaState::Idle, 0, "check failed");
        return res;
    }

    res.ok               = true;
    res.update_available = ver_newer(res.available.c_str(), res.current.c_str());
    res.reason           = res.update_available ? "update available" : "up to date";
    set_available(res.available.c_str());
    set_state(OtaState::Idle, 0, res.reason.c_str());
    ESP_LOGI(TAG, "available %s — %s", res.available.c_str(), res.reason.c_str());
    return res;
}

// Publish a finished check into the shared status for /ota/status polling.
static void set_check_done(const OtaCheckResult& r) {
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state            = r.ok ? OtaState::Idle : OtaState::Error;
    s_status.progress         = 0;
    s_status.message          = r.reason;
    s_status.available        = r.available;
    s_status.update_available = r.update_available;
    s_status.current          = r.current;
    xSemaphoreGive(s_lock);
}

static void ota_check_task(void*) {
    OtaCheckResult r = ota_check();   // blocking HTTPS GET, runs off the HTTP task
    set_check_done(r);
    s_running = false;
    vTaskDelete(nullptr);
}

bool ota_check_start() {
    // Atomic test-and-set: bail if a check/update is already running. Don't rely on the
    // httpd being single-threaded to serialize the guard.
    if (s_running.exchange(true)) return false;
    set_state(OtaState::Checking, 0, "checking for updates");

    // mbedTLS handshake + manifest fetch run here; same generous stack as ota_task.
    if (xTaskCreate(ota_check_task, "ota_chk", 8192, nullptr, 5, nullptr) != pdPASS) {
        s_running = false;
        set_state(OtaState::Error, 0, "could not start check task");
        return false;
    }
    return true;
}

// ─── Background download + install ──────────────────────────────────────────────

static void ota_task(void*) {
    // One channel, per-target image: base URL + this chip's short image suffix. The literals
    // concatenate at compile time (TESLA_OTA_IMG_SUFFIX is a string literal), so this is a
    // fixed string with no allocation. esp_https_ota also verifies the image chip-id, so a
    // wrong-target image (e.g. an esp32s3 build pulled by an esp32) is refused, not flashed.
    static constexpr const char* kFwUrl =
        CONFIG_TESLA_OTA_FIRMWARE_BASE_URL "tesla-key-esp32" TESLA_OTA_IMG_SUFFIX ".bin";
    ESP_LOGI(TAG, "OTA starting from %s (free heap %u)",
             kFwUrl, (unsigned)esp_get_free_heap_size());

    esp_http_client_config_t http_cfg = {};
    http_cfg.url               = kFwUrl;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.timeout_ms        = 20000;
    http_cfg.keep_alive_enable = true;

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config = &http_cfg;

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK || handle == nullptr) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        set_state(OtaState::Error, 0, "could not start download (server/TLS)");
        s_running = false;
        vTaskDelete(nullptr);
        return;
    }

    int image_size = esp_https_ota_get_image_size(handle);

    // Downgrade defense (software anti-rollback, no eFuses burned by design). The image is
    // RSA-signed and esp_https_ota verifies that, but a signature only proves AUTHENTICITY,
    // not FRESHNESS: an attacker controlling the update host could serve an OLD, legitimately
    // signed image carrying a since-patched vulnerability. Read the version straight from the
    // downloaded image's own app descriptor (esp_https_ota_get_img_desc parses only the header,
    // before the bulk download) and refuse anything not strictly newer than what is running.
    // Checking the image itself — not the manifest — also closes the gap where a hostile host
    // advertises a new version in manifest.json but serves an old .bin under the image URL.
    esp_app_desc_t new_app{};
    err = esp_https_ota_get_img_desc(handle, &new_app);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_get_img_desc failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        set_state(OtaState::Error, 0, "could not read image header");
        s_running = false;
        vTaskDelete(nullptr);
        return;
    }
    if (!ver_newer(new_app.version, running_version())) {
        ESP_LOGW(TAG, "OTA refused: image %s not newer than running %s (downgrade blocked)",
                 new_app.version, running_version());
        esp_https_ota_abort(handle);
        set_state(OtaState::Error, 0, "no newer version available");
        s_running = false;
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "OTA image %s newer than running %s — proceeding",
             new_app.version, running_version());

    while (true) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int read = esp_https_ota_get_image_len_read(handle);
        int pct  = (image_size > 0) ? (int)((int64_t)read * 100 / image_size) : 0;
        set_state(OtaState::Downloading, pct, "downloading");
    }

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(handle)) {
        err = esp_https_ota_finish(handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA complete — rebooting into new image");
            set_state(OtaState::Done, 100, "update complete — rebooting");
            vTaskDelay(pdMS_TO_TICKS(1200));
            esp_restart();   // does not return
        }
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        set_state(OtaState::Error, 0,
                  err == ESP_ERR_OTA_VALIDATE_FAILED ? "downloaded image is invalid"
                                                     : "could not finalize update");
    } else {
        ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        set_state(OtaState::Error, 0, "download failed");
    }

    s_running = false;
    vTaskDelete(nullptr);
}

bool ota_start() {
    // Atomic test-and-set so two concurrent /ota/update calls can't both launch a task.
    if (s_running.exchange(true)) return false;
    set_state(OtaState::Downloading, 0, "starting download");

    // A generous stack: mbedTLS record processing + esp_https_ota run here.
    if (xTaskCreate(ota_task, "ota", 8192, nullptr, 5, nullptr) != pdPASS) {
        s_running = false;
        set_state(OtaState::Error, 0, "could not start OTA task");
        return false;
    }
    return true;
}
