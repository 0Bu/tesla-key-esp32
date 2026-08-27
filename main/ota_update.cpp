#include "ota_update.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_app_desc.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_crt_bundle.h>
#include <cJSON.h>

#include "logic/heap_watchdog.hpp"
#include "logic/json_syntax.hpp"
#include "logic/ota_contract.hpp"
#include "ble_client.hpp"
#include "ota_manifest.hpp"
#include "platform.hpp"
#include "runtime_admission.hpp"
#include "task_config.hpp"
#include "rtos_guard.hpp"

#include <atomic>
#include <array>
#include <cstring>
#include <exception>
#include <memory>
#include <string_view>

static const char* TAG = "ota";

// One atomic owner word closes the OTA/identity/fault-restart TOCTOU windows. A separate
// `s_running` boolean can answer status questions, but cannot serialize "checked false, then OTA
// started". Every OTA worker owns Ota from before task creation until it exits/reboots; every
// key/VIN transaction owns IdentityMutation for its complete journal + NVS mutation lifetime; a
// fault restart owns FaultRestart from before reboot_why is persisted until restart (or releases
// it when persistence fails).
static tk::OtaIdentityOperationGate s_operation_gate;

static bool try_begin_ota_operation() {
    return s_operation_gate.try_begin(tk::OtaIdentityGateState::Ota);
}

static void finish_operation(tk::OtaIdentityGateState owner) {
    if (!s_operation_gate.finish(owner)) {
        // Never clear a different owner's gate: doing so would turn an invariant violation into
        // permission for a real overlapping reboot/mutation. Leave it fail-closed and diagnose.
        ESP_LOGE(TAG, "OTA/identity operation gate owner mismatch (expected=%d, actual=%d)",
                 static_cast<int>(owner), static_cast<int>(s_operation_gate.state()));
    }
}

tk::OtaVerificationState ota_verification_state() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return tk::OtaVerificationState::Unknown;

    esp_ota_img_states_t state{};
    if (esp_ota_get_state_partition(running, &state) != ESP_OK)
        return tk::OtaVerificationState::Unknown;
    switch (state) {
        case ESP_OTA_IMG_UNDEFINED:
        case ESP_OTA_IMG_VALID:
            return tk::OtaVerificationState::Stable;
        case ESP_OTA_IMG_NEW:
        case ESP_OTA_IMG_PENDING_VERIFY:
            return tk::OtaVerificationState::PendingVerify;
        default:
            // INVALID/ABORTED cannot normally be the running partition. If storage reports such
            // a state, it is not authority to begin an irreversible cross-version identity write.
            return tk::OtaVerificationState::Unknown;
    }
}

bool ota_identity_mutation_allowed(tk::IdentityMutationEntry entry) {
    return tk::identity_mutation_may_start(
        ota_verification_state(),
        s_operation_gate.state(), entry);
}

OtaIdentityMutationGuard::OtaIdentityMutationGuard(tk::IdentityMutationEntry entry) {
    const tk::OtaVerificationState before = ota_verification_state();
    const auto expected = s_operation_gate.state();
    if (!tk::identity_mutation_may_start(before, expected, entry)) return;
    if (!s_operation_gate.try_begin(tk::OtaIdentityGateState::IdentityMutation)) {
        return;
    }

    // Re-sample after acquiring exclusivity. An OTA start racing the first sample either won the
    // CAS (so we never get here) or is now blocked. Any other state change/error remains
    // non-authoritative and releases the gate without permitting a write.
    if (!tk::identity_mutation_allowed(ota_verification_state(), entry)) {
        finish_operation(tk::OtaIdentityGateState::IdentityMutation);
        return;
    }
    held_ = true;
}

OtaIdentityMutationGuard::~OtaIdentityMutationGuard() {
    if (held_) finish_operation(tk::OtaIdentityGateState::IdentityMutation);
}

bool ota_fault_restart_begin() {
    return s_operation_gate.try_begin(tk::OtaIdentityGateState::FaultRestart);
}

void ota_fault_restart_cancel() {
    finish_operation(tk::OtaIdentityGateState::FaultRestart);
}

OtaHealthCommitGuard::OtaHealthCommitGuard()
    : held_(s_operation_gate.try_begin(tk::OtaIdentityGateState::HealthCommit)) {}

OtaHealthCommitGuard::~OtaHealthCommitGuard() {
    if (held_) finish_operation(tk::OtaIdentityGateState::HealthCommit);
}

// Confirm a still-unverified OTA image before a successful network/logging/setup commit reboot —
// see the header. Mirrors the mark-valid path in main.cpp's ota_health_gate_task, but fires once the
// durable commit AND the post-admission heap sample prove the runtime healthy. Identity mutations
// deliberately never reach this path. A qualifying restart inside the health window must not
// trigger a rollback unless the image is already critically heap-starved.
void ota_confirm_pending_image(tk::OtaRebootClass reboot_class) {
    if (!tk::ota_reboot_confirms_pending_image(reboot_class)) {
        ESP_LOGE(TAG, "refusing OTA confirmation for non-user recovery reboot class %d",
                 static_cast<int>(reboot_class));
        return;
    }

    // The user commit is valid health evidence, but mark-valid is still the same irreversible
    // operation as the timed health gate. Serialize it against a persisted FaultRestart window,
    // OTA and identity work; if another owner won, leave PENDING_VERIFY armed so the imminent
    // reboot fails closed into rollback instead of crossing that owner's shutdown transaction.
    OtaHealthCommitGuard commit_guard;
    if (!commit_guard) {
        ESP_LOGW(TAG, "OTA image confirmation postponed by an active operation; rollback remains armed");
        return;
    }
    // A durable configuration write is not proof that the essential runtime started. In
    // particular, recovery HTTP remains available in Safe Mode and during a partial boot. Check
    // admission only after owning HealthCommit so Ready cannot race a FaultRestart/OTA owner; all
    // non-Ready states leave the one automatic rollback armed.
    if (!tk::runtime_admission_vehicle_ready()) {
        ESP_LOGW(TAG, "OTA image confirmation refused: essential runtime is not ready; "
                      "rollback remains armed");
        return;
    }
    if (!ble_host_synced()) {
        ESP_LOGW(TAG, "OTA image confirmation refused: NimBLE host is not synced; "
                      "rollback remains armed");
        return;
    }
    if (ble_host_reset_count() != 0) {
        ESP_LOGW(TAG, "OTA image confirmation refused: NimBLE host reset since admission; "
                      "rollback remains armed");
        return;
    }
    // Re-sample only after winning the same exclusive owner as the timed health gate. A healthy
    // sample taken before the CAS could become stale while FaultRestart persists its breadcrumb;
    // crossing that window would permanently validate the image immediately before it reboots.
    const size_t commit_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (commit_largest < tk::kHeapCriticalBytes) {
        ESP_LOGW(TAG, "OTA image confirmation refused: internal largest block %u B < %u B; "
                      "rollback remains armed",
                 static_cast<unsigned>(commit_largest),
                 static_cast<unsigned>(tk::kHeapCriticalBytes));
        return;
    }
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
            ESP_LOGI(TAG, "OTA image confirmed valid before a user-initiated reboot (rollback cancelled)");
    }
}

// Short per-target image suffix so "esp32" appears only once in the OTA filename: esp32 ->
// "" (tesla-key-esp32.bin), esp32s3 -> "-s3", esp32c3 -> "-c3", esp32c6 -> "-c6".
// Must stay in lockstep with the signer/Pages target tables and the host-tested
// tk::image_suffix() mapping (which name the published asset the device pulls) — a mismatch
// 404s every OTA download.
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

static constexpr size_t kOtaStatusMessageCapacity = 96;
static constexpr size_t kOtaStatusVersionCapacity = tk::kOtaVersionMaxBytes + 1;

template <size_t N>
static void copy_status_text(std::array<char, N>& out, const char* text) noexcept {
    static_assert(N > 0);
    out.fill('\0');
    if (!text) return;
    const size_t size = strnlen(text, N - 1);
    std::memcpy(out.data(), text, size);
}

struct OtaStatusPod {
    OtaState state{OtaState::Idle};
    int progress{0};
    bool update_available{false};
    std::array<char, kOtaStatusMessageCapacity> message{};
    std::array<char, kOtaStatusVersionCapacity> available{};
    std::array<char, kOtaStatusVersionCapacity> current{};
};

static OtaStatusPod initial_status() noexcept {
    OtaStatusPod status{};
    copy_status_text(status.message, "idle");
    return status;
}

static std::atomic<SemaphoreHandle_t> s_lock{nullptr};
static OtaStatusPod                   s_status = initial_status();
static std::atomic<bool>              s_running{false};    // a check or download task is active

static SemaphoreHandle_t ensure_lock() {
    SemaphoreHandle_t lock = s_lock.load(std::memory_order_acquire);
    if (lock) return lock;

    SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
    if (!candidate) return nullptr;
    if (!s_lock.compare_exchange_strong(lock, candidate,
                                        std::memory_order_release,
                                        std::memory_order_acquire)) {
        vSemaphoreDelete(candidate);
        return lock;
    }
    return candidate;
}

static void set_state(OtaState st, int pct, const char* msg) {
    std::array<char, kOtaStatusMessageCapacity> message{};
    copy_status_text(message, msg);
    SemaphoreHandle_t lock = ensure_lock();
    if (!lock) return;
    tk::SemGuard g(lock);
    if (!g) return;
    s_status.state    = st;
    s_status.progress = pct;
    s_status.message  = message;
}

static OtaStatus unavailable_status_snapshot() {
    // This object is built independently of s_status. It may still throw if the standard library
    // cannot represent even these short strings under total OOM (callers already contain that),
    // but it can never race a writer or copy a concurrently-reallocated std::string buffer.
    return {OtaState::Error, 0, "unavailable", "", false, ""};
}

OtaStatus ota_get_status() {
    SemaphoreHandle_t lock = ensure_lock();
    if (!lock) return unavailable_status_snapshot();
    OtaStatusPod snapshot{};
    {
        tk::SemGuard g(lock);
        if (!g) return unavailable_status_snapshot();
        snapshot = s_status;  // fixed POD/arrays only; noexcept and cross-generation coherent
    }
    // Any allocation happens after releasing the status lock. A failed materialization is caught
    // by the HTTP/task boundary and cannot leave a partially published shared generation.
    return {snapshot.state, snapshot.progress, snapshot.message.data(),
            snapshot.available.data(), snapshot.update_available, snapshot.current.data()};
}

bool ota_is_busy() {
    return s_running.load(std::memory_order_acquire) ||
           s_operation_gate.state() ==
               tk::OtaIdentityGateState::Ota;
}

// ─── Canonical, bounded version input ──────────────────────────────────────────

static std::string_view running_version() {
    const esp_app_desc_t* description = esp_app_get_description();
    return description ? tk::bounded_c_string_view(description->version) : std::string_view{};
}

static void publish_check_status(const OtaStatusPod& candidate) noexcept {
    SemaphoreHandle_t lock = ensure_lock();
    if (!lock) return;
    tk::SemGuard g(lock);
    if (!g) return;
    s_status = candidate;
}

static void set_check_error(const char* message) noexcept {
    OtaStatusPod candidate{};
    candidate.state = OtaState::Error;
    copy_status_text(candidate.message, message);
    const std::string_view current = running_version();
    copy_status_text(candidate.current, current.data());
    publish_check_status(candidate);
}

// ─── Small HTTPS GET into a buffer (for the tiny manifest.json) ─────────────────

static bool http_get_to_buffer(const char* url, std::string& out) {
    out.clear();
    esp_http_client_config_t cfg = {};
    cfg.url               = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // verify against bundled CA roots
    cfg.timeout_ms        = 10000;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    struct HttpClientGuard {
        esp_http_client_handle_t handle;
        ~HttpClientGuard() {
            esp_http_client_close(handle);
            esp_http_client_cleanup(handle);
        }
    } client_guard{c};

    if (esp_http_client_open(c, 0) == ESP_OK) {
        const std::int64_t content_length = esp_http_client_fetch_headers(c);
        const int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            const bool chunked = esp_http_client_is_chunked_response(c);
            tk::BoundedHttpBodyGate body_gate(content_length, chunked);
            if (!body_gate.valid_headers()) {
                ESP_LOGW(TAG, "manifest length is missing, empty or exceeds %u bytes",
                         static_cast<unsigned>(tk::kOtaManifestMaxBytes));
                return false;
            }

            // One exact bounded allocation: repeated append growth can transiently require both
            // the old and new contiguous blocks while TLS is still live. Fixed-length responses
            // reserve their declared size; chunked responses reserve the hard 8192-byte ceiling.
            try {
                out.reserve(chunked ? tk::kOtaManifestMaxBytes
                                    : static_cast<std::size_t>(content_length));
            } catch (...) {
                ESP_LOGW(TAG, "manifest buffer allocation failed");
                return false;
            }

            char buf[512];
            while (true) {
                const std::size_t requested = body_gate.next_read_size(sizeof(buf));
                if (requested == 0) {
                    out.clear();
                    return false;
                }
                const int read = esp_http_client_read(c, buf, static_cast<int>(requested));
                const auto decision = body_gate.accept_read(
                    read, esp_http_client_is_complete_data_received(c));
                if (decision == tk::BoundedBodyReadResult::Reject) {
                    ESP_LOGW(TAG, "manifest response was truncated, oversized or read failed");
                    out.clear();
                    return false;
                }
                if (read > 0) out.append(buf, static_cast<std::size_t>(read));
                if (decision == tk::BoundedBodyReadResult::Complete) return true;
            }
        } else {
            ESP_LOGW(TAG, "manifest HTTP status %d", status);
        }
    } else {
        ESP_LOGW(TAG, "manifest connection failed");
    }

    out.clear();
    return false;
}

// ─── Check for a newer release ──────────────────────────────────────────────────

OtaCheckResult ota_check() {
    OtaCheckResult res{};
    const std::string_view current = running_version();
    res.current.assign(current.data(), current.size());

    ESP_LOGI(TAG, "checking %s (running %s)", CONFIG_TESLA_OTA_MANIFEST_URL, res.current.c_str());

    std::string body;
    if (!http_get_to_buffer(CONFIG_TESLA_OTA_MANIFEST_URL, body)) {
        res.ok     = false;
        res.reason = "could not reach update server";
        return res;
    }

    const char* parse_end = nullptr;
    const auto materialized = tk::json_materialize<cJSON>(
        body.data(), body.size(), [&](const char* text) {
            return cJSON_ParseWithLengthOpts(
                text, body.size() + 1, &parse_end, true);
        });
    if (materialized.status != tk::JsonMaterializeStatus::Ok) {
        res.ok     = false;
        res.reason = materialized.status == tk::JsonMaterializeStatus::NoMemory
                         ? "update manifest ran out of resources"
                         : "invalid update manifest";
        return res;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> j(materialized.root, cJSON_Delete);
    const tk::OtaManifestVersion manifest = tk::inspect_ota_manifest(j.get());
    if (parse_end != body.c_str() + body.size() ||
        manifest.status != tk::OtaManifestInspectStatus::Valid || !manifest.value) {
        res.ok     = false;
        res.reason = "invalid update manifest";
        return res;
    }
    // cJSON owns decoded strings. Release the bounded transport body before any result-string
    // allocation so the largest-block peak is body+cJSON, never body+cJSON+another body-sized copy.
    std::string{}.swap(body);
    const std::string_view available(manifest.value);
    // Validate the cJSON-owned view before copying: an attacker-controlled ~8 KiB version must
    // not trigger another large contiguous std::string allocation while TLS/body/cJSON coexist.
    if (!tk::canonical_ota_version(available)) {
        res.ok     = false;
        res.reason = "invalid version in manifest";
        return res;
    }
    if (!tk::canonical_ota_version(res.current)) {
        res.ok     = false;
        res.reason = "invalid running firmware version";
        return res;
    }
    res.available.assign(available.data(), available.size());

    res.ok               = true;
    res.update_available = tk::compare_ota_versions(res.available, res.current) ==
                           tk::OtaVersionOrder::Newer;
    res.reason           = res.update_available ? "update available" : "up to date";
    ESP_LOGI(TAG, "available %s — %s", res.available.c_str(), res.reason.c_str());
    return res;
}

// Publish a finished check into the shared status for /ota/status polling.
static void set_check_done(const OtaCheckResult& r) {
    OtaStatusPod candidate{};
    candidate.state = r.ok ? OtaState::Idle : OtaState::Error;
    candidate.progress = 0;
    candidate.update_available = r.update_available;
    copy_status_text(candidate.message, r.reason.c_str());
    copy_status_text(candidate.available, r.available.c_str());
    copy_status_text(candidate.current, r.current.c_str());
    publish_check_status(candidate);
}

static void ota_check_task(void*) {
    try {
        // A one-shot job: contain any throw (http_get_to_buffer's std::string appends, cJSON, the
        // result std::string ops can all bad_alloc) as a terminal Error state — NEVER let it unwind
        // into the FreeRTOS C trampoline and reboot the device mid-check (issue #204).
        try {
            OtaCheckResult r = ota_check();   // blocking HTTPS GET, runs off the HTTP task
            set_check_done(r);
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "OTA check task exception: %s", e.what());
            set_check_error("update check ran out of resources");
        } catch (...) {
            ESP_LOGE(TAG, "OTA check task unknown exception");
            set_check_error("update check failed unexpectedly");
        }
        s_running.store(false, std::memory_order_release);
        finish_operation(tk::OtaIdentityGateState::Ota);
        vTaskDelete(nullptr);
    } catch (...) {
        // The outer boundary also contains any future throwing setup/cleanup statement added
        // around the operation-specific handler above.
        ESP_LOGE(TAG, "OTA check boundary cleanup threw; stopping task");
        s_running.store(false, std::memory_order_release);
        finish_operation(tk::OtaIdentityGateState::Ota);
        vTaskDelete(nullptr);
    }
}

bool ota_check_start() {
    if (!ensure_lock()) return false;
    // Acquire the cross-domain gate before publishing/starting the worker. This atomically loses
    // to an in-flight key/VIN transaction instead of sampling a separate busy flag and racing it.
    if (!try_begin_ota_operation()) return false;
    if (s_running.exchange(true, std::memory_order_acq_rel)) {
        finish_operation(tk::OtaIdentityGateState::Ota);
        return false;
    }
    try {
        set_state(OtaState::Checking, 0, "checking for updates");
    } catch (...) {
        s_running.store(false, std::memory_order_release);
        finish_operation(tk::OtaIdentityGateState::Ota);
        return false;
    }

    // mbedTLS handshake + manifest fetch run here; same generous stack as ota_task.
    if (xTaskCreate(ota_check_task, "ota_chk", 8192, nullptr, tk::kPrioOtaCheck, nullptr) != pdPASS) {
        s_running.store(false, std::memory_order_release);
        finish_operation(tk::OtaIdentityGateState::Ota);
        set_check_error("could not start check task");
        return false;
    }
    return true;
}

// ─── Background download + install ──────────────────────────────────────────────

static void ota_task_impl() {
    // Re-fetch the manifest HERE rather than trusting whatever a previous /ota/check left in the
    // shared status. Two independent reasons: POST /ota/update is reachable on its own, so gating
    // only inside the check would mean no gate at all for a direct caller; and the manifest could
    // have moved between the check and the confirmation. Costs one small HTTPS GET on this task —
    // never on a request path.
    const OtaCheckResult pre = ota_check();
    if (!pre.ok) {
        ESP_LOGE(TAG, "OTA refused: %s", pre.reason.c_str());
        set_state(OtaState::Error, 0, pre.reason.c_str());
        return;
    }
    if (!pre.update_available) {
        ESP_LOGW(TAG, "OTA refused: manifest %s is not newer than running %s",
                 pre.available.c_str(), pre.current.c_str());
        set_state(OtaState::Error, 0, "no newer version available");
        return;
    }

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
        return;
    }
    struct OtaHandleGuard {
        esp_https_ota_handle_t handle;
        ~OtaHandleGuard() { if (handle) esp_https_ota_abort(handle); }
        esp_https_ota_handle_t release() {
            auto h = handle;
            handle = nullptr;
            return h;
        }
    } handle_guard{handle};

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
        set_state(OtaState::Error, 0, "could not read image header");
        return;
    }
    const std::string_view new_version = tk::bounded_c_string_view(new_app.version);
    const std::string_view current_version = running_version();
    if (!tk::canonical_ota_version(new_version) ||
        !tk::canonical_ota_version(current_version)) {
        ESP_LOGW(TAG, "OTA refused: image or running version is malformed");
        set_state(OtaState::Error, 0, "invalid firmware version");
        return;
    }
    if (tk::compare_ota_versions(new_version, current_version) !=
        tk::OtaVersionOrder::Newer) {
        ESP_LOGW(TAG, "OTA refused: image %.*s not newer than running %.*s (downgrade blocked)",
                 static_cast<int>(new_version.size()), new_version.data(),
                 static_cast<int>(current_version.size()), current_version.data());
        set_state(OtaState::Error, 0, "no newer version available");
        return;
    }

    // SECOND point of the gate: the manifest and the image must name the SAME version.
    //
    // The freshness check above is necessary but not sufficient, and the gap is easy to miss: the
    // manifest and the .bin are two separately-controlled artifacts. A host serving manifest 1.9.0
    // beside a 1.8.0 image passes every check individually — the image is signed, and 1.8.0 really
    // is newer than a device running 1.7.0 — so the device installs a build the operator never
    // published, while the UI reports the version the manifest claimed. Requiring the two strings
    // to match exactly turns "the newest thing the host is willing to serve" back into "the build
    // the release actually is". Also catches the ordinary, far more likely case: a publish that
    // wrote a new manifest beside a stale image.
    if (std::string_view(pre.available) != new_version) {
        ESP_LOGE(TAG, "OTA refused: manifest advertises %s but the image is %.*s — the manifest and "
                      "the image disagree, so neither can be trusted to be the published release",
                 pre.available.c_str(), static_cast<int>(new_version.size()), new_version.data());
        set_state(OtaState::Error, 0, "manifest and image versions disagree");
        return;
    }
    ESP_LOGI(TAG, "OTA image %.*s newer than running %.*s and matches the manifest — proceeding",
             static_cast<int>(new_version.size()), new_version.data(),
             static_cast<int>(current_version.size()), current_version.data());

    while (true) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int read = esp_https_ota_get_image_len_read(handle);
        int pct  = (image_size > 0) ? (int)((int64_t)read * 100 / image_size) : 0;
        set_state(OtaState::Downloading, pct, "downloading");
    }

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(handle)) {
        err = esp_https_ota_finish(handle_guard.release());
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
        set_state(OtaState::Error, 0, "download failed");
    }
}

static void ota_task(void*) {
    try {
        try {
            ota_task_impl();
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "OTA task exception: %s", e.what());
            try { set_state(OtaState::Error, 0, "update ran out of resources"); } catch (...) {}
        } catch (...) {
            ESP_LOGE(TAG, "OTA task unknown exception");
            try { set_state(OtaState::Error, 0, "update failed unexpectedly"); } catch (...) {}
        }
        s_running.store(false, std::memory_order_release);
        finish_operation(tk::OtaIdentityGateState::Ota);
        vTaskDelete(nullptr);
    } catch (...) {
        ESP_LOGE(TAG, "OTA task boundary cleanup threw; stopping task");
        s_running.store(false, std::memory_order_release);
        finish_operation(tk::OtaIdentityGateState::Ota);
        vTaskDelete(nullptr);
    }
}

bool ota_start() {
    if (!ensure_lock()) return false;
    // The same owner word excludes both a second OTA and every identity transaction. On a
    // successful install it remains owned until esp_restart(); on every returning path the task
    // releases it below.
    if (!try_begin_ota_operation()) return false;
    if (s_running.exchange(true, std::memory_order_acq_rel)) {
        finish_operation(tk::OtaIdentityGateState::Ota);
        return false;
    }
    try {
        set_state(OtaState::Downloading, 0, "starting download");
    } catch (...) {
        s_running.store(false, std::memory_order_release);
        finish_operation(tk::OtaIdentityGateState::Ota);
        return false;
    }

    // A generous stack: mbedTLS record processing + esp_https_ota run here.
    if (xTaskCreate(ota_task, "ota", 8192, nullptr, tk::kPrioOta, nullptr) != pdPASS) {
        s_running.store(false, std::memory_order_release);
        finish_operation(tk::OtaIdentityGateState::Ota);
        try { set_state(OtaState::Error, 0, "could not start OTA task"); } catch (...) {}
        return false;
    }
    return true;
}
