#include "stack_watch.hpp"
#include "ota_update.hpp"
#include "ota_recovery_memory.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include "ota_writer.hpp"
#include "logic/ota_lifetime.hpp"
#include <cJSON.h>

#include "logic/heap_watchdog.hpp"
#include "logic/tls_memory.hpp"
#include "logic/json_syntax.hpp"
#include "logic/ota_contract.hpp"
#include "logic/ota_changelog_range.hpp"
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
#include <new>
#include <string_view>

static const char* TAG = "ota";

// One atomic owner word closes the OTA/identity/fault-restart TOCTOU windows. A separate
// `s_running` boolean can answer status questions, but cannot serialize "checked false, then OTA
// started". Every OTA worker owns Ota from before task creation until it exits/reboots; every
// key/VIN transaction owns IdentityMutation for its complete journal + NVS mutation lifetime; a
// fault restart owns FaultRestart from before reboot_why is persisted until restart (or releases
// it when persistence fails).
static tk::OtaIdentityOperationGate s_operation_gate;
static std::atomic<tk::OtaVerificationState> s_verification{tk::OtaVerificationState::Unknown};

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

// One pure selector owns both ordinary-entry consumers and the separate TLS screen.
// The selected threshold is an empirical policy, not a reservation or universal TLS guarantee.
std::size_t tk::ota_recovery_required_bytes(tk::OtaRecoveryUse use) noexcept {
    return tk::ota_recovery_required_bytes_for(use);
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

OtaFaultRestartGuard::OtaFaultRestartGuard() noexcept
    : acquisition_(s_operation_gate.try_begin_observed(tk::OtaIdentityGateState::FaultRestart)) {}

OtaFaultRestartGuard::~OtaFaultRestartGuard() noexcept {
    if (held() && !sealed_) {
        try {
            finish_operation(tk::OtaIdentityGateState::FaultRestart);
        } catch (...) {
            // Owner-checked CAS precedes any fallible mismatch diagnostic.
        }
    }
}

OtaHealthCommitGuard::OtaHealthCommitGuard() {
    const auto acquired = s_operation_gate.try_begin_observed(tk::OtaIdentityGateState::HealthCommit);
    held_ = acquired.acquired;
    observed_ = acquired.observed;
}

OtaHealthCommitGuard::~OtaHealthCommitGuard() {
    if (held_) finish_operation(tk::OtaIdentityGateState::HealthCommit);
}

OtaConfigRestartGuard::~OtaConfigRestartGuard() noexcept {
    if (held_ && !sealed_) {
        held_ = false;
        // No logging or other fallible diagnostics in noexcept cleanup.
        (void)s_operation_gate.finish(tk::OtaIdentityGateState::ConfigRestart);
    }
}

bool OtaConfigRestartGuard::try_begin() noexcept {
    if (held_) return owns_gate();
    held_ = s_operation_gate.try_begin(tk::OtaIdentityGateState::ConfigRestart);
    return held_;
}

bool OtaConfigRestartGuard::owns_gate() const noexcept {
    return held_ && s_operation_gate.state() == tk::OtaIdentityGateState::ConfigRestart;
}

tk::RuntimeHealthSample ota_runtime_health() {
    return tk::sample_runtime_health(
        [] { return tk::runtime_admission_vehicle_ready(); },
        [] { return ble_host_synced(); },
        [] { return ble_host_reset_count(); },
        [] { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL); },
        tk::ota_recovery_required_bytes(tk::OtaRecoveryUse::ImageConfirmation));
}

// Defined after the complete private Operation type. Never called from unowned snapshots.
static bool ordinary_recovery_reference_ready_owned(tk::OtaIdentityGateState owner) noexcept;

bool ota_recovery_reference_ready(const OtaHealthCommitGuard& owner) noexcept {
    if (!owner || s_operation_gate.state() != tk::OtaIdentityGateState::HealthCommit) return false;
    return ordinary_recovery_reference_ready_owned(tk::OtaIdentityGateState::HealthCommit);
}

tk::ConfigImageResult ota_config_image_state(const OtaConfigRestartGuard& owner) noexcept {
    using Result = tk::ConfigImageResult;
    if (!owner.owns_gate()) return Result::WrongOwner;
    try {
        const esp_partition_t* running = esp_ota_get_running_partition();
        esp_ota_img_states_t state{};
        if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK)
            return Result::Unknown;
        switch (state) {
            case ESP_OTA_IMG_VALID: return Result::Valid;
            case ESP_OTA_IMG_UNDEFINED: return Result::Undefined;
            case ESP_OTA_IMG_NEW: return Result::New;
            case ESP_OTA_IMG_PENDING_VERIFY: return Result::Pending;
            default: return Result::Unsupported;
        }
    } catch (...) { return Result::Unknown; }
}

tk::ConfigImageResult ota_confirm_pending_image(
    const OtaConfigRestartGuard& owner, tk::OtaRebootClass reboot_class) noexcept {
    using Result = tk::ConfigImageResult;
    if (!owner.owns_gate()) return Result::WrongOwner;
    if (!tk::ota_reboot_confirms_pending_image(reboot_class)) return Result::WrongClass;
    Result result = ota_config_image_state(owner);
    if (result != Result::Pending) return result;
    try {
        if (!ordinary_recovery_reference_ready_owned(tk::OtaIdentityGateState::ConfigRestart))
            return Result::Pending; // Raw Pending is known; no mark was attempted.
        const auto health = ota_runtime_health();
        result = tk::config_health_failure(health.failure);
        if (result != Result::None) return result;
        result = Result::MarkUnknown;
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            result = Result::Confirmed;
        }
        ota_refresh_verification_cache();
        if (result == Result::Confirmed) {
            ESP_LOGI(TAG, "OTA image confirmed before user configuration restart");
        }
    } catch (...) {
        // Preserve a successful mark even if its diagnostic throws. Otherwise retain the last
        // known refusal/pending state or the uncertain SDK effect; never retry here.
    }
    return result;
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
static_assert(tk::ota_https_url(CONFIG_TESLA_OTA_MANIFEST_URL),
              "OTA manifest URL must use HTTPS");
static_assert(tk::ota_https_url(CONFIG_TESLA_OTA_FIRMWARE_BASE_URL),
              "OTA firmware base URL must use HTTPS");

// ─── Shared status (written by the OTA task, read by HTTP handlers) ────────────

static constexpr size_t kOtaStatusMessageCapacity = 96;
static constexpr size_t kOtaStatusVersionCapacity = tk::kOtaVersionMaxBytes + 1;

// These INFO lines reuse the existing bounded diagnostic ring. No checkpoint cache, URL,
// identity or heap trace is retained. The pair is a sequential observation, not a reservation.
static void memory_checkpoint_log(const char* phase, uint32_t generation, uint64_t started_ms,
                                  bool guard_sample, size_t guard_largest) noexcept {
    try {
        multi_heap_info_t info{};
        heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGI("ota_mem", "g=%u t0=%llu phase=%.24s internal_free=%zu internal_largest=%zu",
                 static_cast<unsigned>(generation), static_cast<unsigned long long>(started_ms),
                 phase ? phase : "unknown", info.total_free_bytes,
                 guard_sample ? guard_largest : info.largest_free_block);
    } catch (...) {} // A diagnostic never changes an operation outcome or cleanup authority.
}
void tk::ota::memory_checkpoint(const char* phase, uint32_t generation,
                                uint64_t started_ms) noexcept {
    memory_checkpoint_log(phase, generation, started_ms, false, 0);
}
void tk::ota::memory_checkpoint(const char* phase, uint32_t generation,
                                uint64_t started_ms, size_t guard_largest) noexcept {
    memory_checkpoint_log(phase, generation, started_ms, true, guard_largest);
}

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
    unsigned target_pr{0};
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
static std::atomic<bool>              s_updating{false};   // an active download/flash-write update task is running
static std::atomic<unsigned>          s_target_pr{0};      // target PR for current check/update session

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

tk::OtaVerificationState ota_cached_verification_state() noexcept {
    return s_verification.load(std::memory_order_acquire);
}

tk::OtaVerificationState ota_refresh_verification_cache() noexcept {
    tk::OtaVerificationState state = tk::OtaVerificationState::Unknown;
    try { state = ota_verification_state(); } catch (...) {}
    s_verification.store(state, std::memory_order_release);
    return state;
}

static void set_state(OtaState st, int pct, const char* msg) {
    std::array<char, kOtaStatusMessageCapacity> message{};
    copy_status_text(message, msg);
    SemaphoreHandle_t lock = ensure_lock();
    if (!lock) return;
    tk::SemGuard g(lock);
    if (!g) return;
    s_status.state     = st;
    s_status.progress  = pct;
    s_status.target_pr = s_target_pr.load(std::memory_order_relaxed);
    s_status.message   = message;
}

static OtaStatus unavailable_status_snapshot() {
    // This object is built independently of s_status. It may still throw if the standard library
    // cannot represent even these short strings under total OOM (callers already contain that),
    // but it can never race a writer or copy a concurrently-reallocated std::string buffer.
    return {OtaState::Error, 0, "unavailable", "", false, "", tk::ota_channel_name(ota_get_channel())};
}

static std::atomic<tk::OtaChannel> s_ota_channel{tk::OtaChannel::Release};

tk::OtaChannel ota_get_channel() {
    return s_ota_channel.load(std::memory_order_acquire);
}

void ota_set_channel(tk::OtaChannel channel) {
    s_ota_channel.store(channel, std::memory_order_release);
}

OtaState ota_get_state() {
    SemaphoreHandle_t lock = ensure_lock();
    if (!lock) return OtaState::Error;
    tk::SemGuard guard(lock);
    if (!guard) return OtaState::Error;
    return s_status.state;
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
            snapshot.available.data(), snapshot.update_available, snapshot.current.data(),
            tk::ota_channel_name(ota_get_channel()), snapshot.target_pr};
}

bool ota_is_busy() {
    return s_running.load(std::memory_order_acquire) ||
           s_operation_gate.state() ==
               tk::OtaIdentityGateState::Ota;
}

bool ota_is_updating() {
    return s_updating.load(std::memory_order_acquire);
}

static constexpr size_t kOtaChangelogCapacity = 1024;
static std::array<char, kOtaChangelogCapacity + 1> s_changelog{};
static size_t s_changelog_len = 0;

static void set_changelog_locked(const char* text) {
    if (!text || text[0] == '\0') {
        s_changelog[0] = '\0';
        s_changelog_len = 0;
        return;
    }
    const size_t len = std::strlen(text);
    const size_t copy_len = len < kOtaChangelogCapacity ? len : kOtaChangelogCapacity;
    std::memcpy(s_changelog.data(), text, copy_len);
    s_changelog[copy_len] = '\0';
    s_changelog_len = copy_len;
}

bool ota_get_changelog(char* out, size_t max_len, size_t& out_len) {
    out_len = 0;
    if (!out || max_len == 0) return false;
    SemaphoreHandle_t lock = ensure_lock();
    if (!lock) return false;
    tk::SemGuard g(lock);
    if (!g || s_changelog_len == 0) {
        return false;
    }
    const size_t copy_len = (s_changelog_len < max_len - 1) ? s_changelog_len : (max_len - 1);
    std::memcpy(out, s_changelog.data(), copy_len);
    out[copy_len] = '\0';
    out_len = copy_len;
    return true;
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
    candidate.target_pr = s_target_pr.load(std::memory_order_relaxed);
    copy_status_text(candidate.message, message);
    const std::string_view current = running_version();
    copy_status_text(candidate.current, current.data());
    publish_check_status(candidate);
}

// ─── Small HTTPS GET into a buffer (for the tiny manifest.json) ─────────────────

enum class OtaFetchResult { Ok, OptionalClosedFailure, NoMemory, Failed, Deadline };
static constexpr const char* kOtaNoMemory = "not enough memory for update";
static constexpr uint32_t kOtaTaskStackBytes = 8192; // Existing per-role limit; aggregate fit unmeasured.
static constexpr uint64_t kMaximumImageBytes = 0x1f0000;
static tk::ota::Slot s_network_slot;
static std::atomic<bool> s_heap_abort_requested{false};

static uint64_t operation_now(void*) noexcept {
    return static_cast<uint64_t>(esp_timer_get_time()) / 1000;
}
static tk::ota::Budget operation_budget(uint64_t origin, bool image) noexcept {
    tk::ota::Budget budget;
    (void)budget.begin(origin, tk::ota::kMetadataBudgetBytes + (image ? kMaximumImageBytes : 0),
                       tk::ota::kOperationTimePolicy);
    return budget;
}

// The coordinator alone owns this actual cross-domain authority. No network wrapper can see it.
class OtaLease {
public:
    OtaLease() noexcept = default; // The entry point acquired the gate before allocation.
    OtaLease(const OtaLease&) = delete;
    OtaLease& operator=(const OtaLease&) = delete;
    bool release() noexcept {
        if (!held_) return true;
        if (s_operation_gate.state() != tk::OtaIdentityGateState::Ota) return false;
        // Publish quiescence before releasing authority; no later global flag store can clear
        // a successor operation's flags. This method is called only after raw writer abort/end.
        s_updating.store(false, std::memory_order_release);
        s_running.store(false, std::memory_order_release);
        if (!s_operation_gate.finish(tk::OtaIdentityGateState::Ota)) return false;
        held_ = false;
        return true;
    }
    static bool release_effect(void* context) noexcept {
        return static_cast<OtaLease*>(context)->release();
    }
private:
    bool held_{true}; // Destructor intentionally cannot release unconfirmed writer authority.
};

enum class NetworkLaunch : uint8_t { NotAttempted, Unknown, Started, Refused };
struct OtaOperation {
    OtaOperation(uint64_t origin, unsigned pr, tk::OtaChannel selected_channel, bool install) noexcept
        : budget(operation_budget(origin, install)),
          job(1, origin, budget.deadline()),
          writer({}, &lease, OtaLease::release_effect),
          coordinator(s_network_slot, job, budget, {nullptr, operation_now}, writer.effects(),
                      tk::ota::kMetadataBudgetBytes),
          target_pr(pr), channel(selected_channel), image(install) {}
    OtaLease lease;
    tk::ota::Budget budget;
    tk::ota::Job job;
    tk::ota::RawWriter writer;
    tk::ota::Coordinator coordinator;
    const unsigned target_pr;
    const tk::OtaChannel channel;
    const bool image;
    std::atomic<bool> coordinator_done{false};
    NetworkLaunch network_launch{NetworkLaunch::NotAttempted}; // Coordinator-only.
};
static std::atomic<OtaOperation*> s_operation{nullptr};

static bool ordinary_recovery_reference_ready_owned(tk::OtaIdentityGateState owner) noexcept {
    // Gate ownership prevents a successor from deleting/replacing this published pointer.
    if ((owner != tk::OtaIdentityGateState::Ota &&
         owner != tk::OtaIdentityGateState::HealthCommit &&
         owner != tk::OtaIdentityGateState::ConfigRestart) || s_operation_gate.state() != owner)
        return false;
    auto* old = s_operation.load(std::memory_order_acquire);
    if (!old) return true;
    // The coordinator release-publishes this as its final Operation access. Its non-atomic
    // fields/RawWriter cannot be read before this acquire. Worker Ack is independently needed.
    if (!old->coordinator_done.load(std::memory_order_acquire)) return false;
    return old->coordinator.can_reclaim() && s_network_slot.owns(old->job);
}

// Called only under a newly acquired Ota owner. A clean Ack ends Job access, not RTOS reclaim.
// The old coordinator's final release store also precedes this reclamation.
static bool reclaim_finished_operation() noexcept {
    auto* old = s_operation.load(std::memory_order_acquire);
    if (!old) return true;
    if (!old->coordinator_done.load(std::memory_order_acquire) ||
        !old->coordinator.can_reclaim() ||
        !s_network_slot.release_after_ack(old->job, true)) return false;
    tk::ota::memory_checkpoint("reclaim_ready", old->job.generation, old->job.started_ms);
    s_operation.store(nullptr, std::memory_order_release);
    delete old;
    return true;
}

void ota_request_heap_abort() noexcept {
    // No foreign task/TLS/flash destruction or owner stealing. The independent coordinator
    // observes this request, aborts its raw writer, then acknowledges by releasing its lease.
    s_heap_abort_requested.store(true, std::memory_order_release);
}

static void acknowledge_no_network_task(OtaOperation& operation) noexcept {
    // Legal only after an actual pdFAIL/no-call path: no worker ever borrowed this Job.
    if (operation.network_launch == NetworkLaunch::Unknown ||
        operation.network_launch == NetworkLaunch::Started) return; // Never fabricate cleanup.
    operation.job.control.store(tk::ota::kRetired |
        static_cast<uint32_t>(tk::ota::Phase::Acked), std::memory_order_release);
}

static bool start_network_task(OtaOperation& operation) {
    if (!operation.budget.live(operation_now(nullptr))) {
        acknowledge_no_network_task(operation);
        operation.coordinator.cancel(tk::ota::Outcome::Deadline);
        return false;
    }
    operation.network_launch = NetworkLaunch::Unknown;
    const auto created = xTaskCreate(tk::ota::ota_network_task, "ota_net", kOtaTaskStackBytes,
                                    &operation.job, tk::kPrioOta, nullptr);
    if (created != pdPASS) {
        operation.network_launch = NetworkLaunch::Refused;
        acknowledge_no_network_task(operation);
        operation.coordinator.cancel(tk::ota::Outcome::NetworkError);
        return false;
    }
    operation.network_launch = NetworkLaunch::Started;
    tk::ota::memory_checkpoint("tasks_ready", operation.job.generation, operation.job.started_ms);
    return true;
}

static tk::ota::Outcome poll_operation(OtaOperation& operation) noexcept {
    if (s_heap_abort_requested.load(std::memory_order_acquire))
        operation.coordinator.cancel(tk::ota::Outcome::Deadline);
    return operation.coordinator.poll();
}

struct MetadataSink {
    std::string* output;
    bool no_memory{false};
    static bool append(void* context, const uint8_t* bytes, size_t count) noexcept {
        auto& sink = *static_cast<MetadataSink*>(context);
        if (count > tk::kOtaManifestMaxBytes - sink.output->size()) return false;
        try { sink.output->append(reinterpret_cast<const char*>(bytes), count); return true; }
        catch (...) { sink.no_memory = true; return false; }
    }
};

static OtaFetchResult http_get_to_buffer(OtaOperation& operation, const char* url,
                                        std::string& out, bool optional = false) {
    out.clear();
    // One bounded body allocation before opening TLS; worker rechecks actual INTERNAL largest
    // both before init and after context/task/URL/mailbox/body allocations, for every leg/redirect.
    try { out.reserve(tk::kOtaManifestMaxBytes); }
    catch (const std::bad_alloc&) {
        operation.coordinator.cancel(tk::ota::Outcome::NoMemory);
        return OtaFetchResult::NoMemory;
    }
    tk::ota::memory_checkpoint(optional ? "notes_ready" : "manifest_ready",
                               operation.job.generation, operation.job.started_ms);
    MetadataSink sink{&out};
    tk::ota::HttpLimits limits{tk::kOtaManifestMaxBytes};
    if (!operation.coordinator.start(url, limits, false, MetadataSink::append, &sink, optional)) {
        const bool expired = !operation.budget.live(operation_now(nullptr));
        operation.coordinator.cancel(expired ? tk::ota::Outcome::Deadline : tk::ota::Outcome::ProtocolError);
        return expired ? OtaFetchResult::Deadline : OtaFetchResult::Failed;
    }
    for (;;) {
        auto state = poll_operation(operation);
        if (sink.no_memory) {
            operation.coordinator.cancel(tk::ota::Outcome::NoMemory);
            out.clear();
            return OtaFetchResult::NoMemory;
        }
        if (state == tk::ota::Outcome::MetadataReady && operation.job.phase() == tk::ota::Phase::Closed) {
            tk::ota::memory_checkpoint("metadata_closed", operation.job.generation, operation.job.started_ms);
            if (!operation.coordinator.metadata_failed()) return OtaFetchResult::Ok;
            out.clear(); return OtaFetchResult::OptionalClosedFailure; // Best-effort only after actual Closed.
        }
        if (state != tk::ota::Outcome::Working && state != tk::ota::Outcome::MetadataReady) {
            out.clear();
            return state == tk::ota::Outcome::NoMemory ? OtaFetchResult::NoMemory :
                   state == tk::ota::Outcome::Deadline ? OtaFetchResult::Deadline : OtaFetchResult::Failed;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ─── Check for a newer release ──────────────────────────────────────────────────

static OtaCheckResult perform_check(OtaOperation& operation, unsigned pr_number) {
    OtaCheckResult res{};
    res.target_pr = pr_number;
    res.current = running_version();
    const tk::OtaChannel channel = operation.channel;

    {
        SemaphoreHandle_t lock = ensure_lock();
        if (lock) {
            tk::SemGuard g(lock);
            if (g) set_changelog_locked(nullptr);
        }
    }

    std::string manifest_url_storage;
    const char* manifest_url = tk::resolve_manifest_url_into(
        pr_number, channel, CONFIG_TESLA_OTA_MANIFEST_URL, manifest_url_storage);
    ESP_LOGI(TAG, "checking %s (channel %s, running %s)", manifest_url,
             tk::ota_channel_name(channel), res.current.c_str());

    std::string body;
    const auto manifest_fetch = http_get_to_buffer(operation, manifest_url, body);
    if (manifest_fetch != OtaFetchResult::Ok) {
        res.ok     = false;
        res.reason = manifest_fetch == OtaFetchResult::NoMemory
                         ? kOtaNoMemory : manifest_fetch == OtaFetchResult::Deadline ? "update check deadline reached" : "could not reach update server";
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
    res.update_available = tk::is_ota_update_available(res.available, res.current, pr_number, channel);
    res.reason           = res.update_available ? "update available" : "up to date";
    ESP_LOGI(TAG, "available %s — %s", res.available.c_str(), res.reason.c_str());

    if (res.update_available) {
        char changelog_url[256];
        if (tk::ota_manifest_sibling_url(manifest_url, "changelog.json", changelog_url, sizeof(changelog_url))) {
            std::string changelog_body;
            const auto changelog_fetch = http_get_to_buffer(operation, changelog_url, changelog_body, true);
            if (changelog_fetch == OtaFetchResult::NoMemory || changelog_fetch == OtaFetchResult::Deadline ||
                changelog_fetch == OtaFetchResult::Failed) {
                // Only OptionalClosedFailure is best-effort. Memory/deadline or terminal
                // cleanup failure must withdraw the earlier manifest's update offer.
                res.ok = false;
                res.update_available = false;
                res.reason = changelog_fetch == OtaFetchResult::NoMemory ? kOtaNoMemory :
                             changelog_fetch == OtaFetchResult::Deadline ? "update check deadline reached" :
                             "could not complete release notes transfer";
                return res;
            }
            if (changelog_fetch == OtaFetchResult::Ok) {
                char decoded[kOtaChangelogCapacity + 1];
                if (tk::manifest_changelog(changelog_body.data(), changelog_body.size(),
                                           res.available.c_str(), decoded, sizeof(decoded))) {
                    if (tk::ota_changelog_select_range(decoded, res.current.c_str(), res.available.c_str()) !=
                        tk::OtaChangelogRangeResult::Invalid) {
                        SemaphoreHandle_t lock = ensure_lock();
                        if (lock) {
                            tk::SemGuard g(lock);
                            if (g) set_changelog_locked(decoded);
                        }
                    }
                }
            }
        }
    }
    if (poll_operation(operation) == tk::ota::Outcome::Deadline) {
        res.ok = res.update_available = false;
        res.reason = "update check deadline reached";
    }
    return res;
}

// Publish a finished check into the shared status for /ota/status polling.
static void set_check_done(const OtaCheckResult& r) {
    OtaStatusPod candidate{};
    candidate.state = r.ok ? OtaState::Idle : OtaState::Error;
    candidate.progress = 0;
    candidate.update_available = r.update_available;
    candidate.target_pr = r.target_pr;
    copy_status_text(candidate.message, r.reason.c_str());
    copy_status_text(candidate.available, r.available.c_str());
    copy_status_text(candidate.current, r.current.c_str());
    publish_check_status(candidate);
    if (!r.ok || !r.update_available) {
        SemaphoreHandle_t lock = ensure_lock();
        if (lock) {
            tk::SemGuard g(lock);
            if (g) set_changelog_locked(nullptr);
        }
    }
}

[[gnu::noinline]] static std::string resolve_firmware_url(unsigned target_pr, tk::OtaChannel channel) {
    if (target_pr > 0) {
        char buf[96];
        if (tk::format_pr_firmware_url(target_pr, TESLA_OTA_IMG_SUFFIX, buf, sizeof(buf))) {
            return std::string(buf);
        }
    }
    if (channel == tk::OtaChannel::Dev) {
        char buf[96];
        if (tk::format_dev_firmware_url(TESLA_OTA_IMG_SUFFIX, buf, sizeof(buf))) {
            return std::string(buf);
        }
    }
    return std::string(CONFIG_TESLA_OTA_FIRMWARE_BASE_URL "tesla-key-esp32" TESLA_OTA_IMG_SUFFIX ".bin");
}

// The coordinator is the only caller of the real raw-OTA effect table.
static void ota_task_impl(OtaOperation& operation) {
    const OtaCheckResult pre = perform_check(operation, operation.target_pr);
    if (!pre.ok || !pre.update_available) {
        set_check_done(pre); // Fresh failures/OOM withdraw any earlier offer.
        if (pre.ok) set_state(OtaState::Error, 0, "no newer version available");
        return;
    }
    tk::ota::ImagePolicy policy{};
    policy.target_pr = operation.target_pr;
    policy.channel = operation.channel;
    if (pre.available.size() >= sizeof(policy.expected) || pre.current.size() >= sizeof(policy.current)) {
        operation.coordinator.cancel(tk::ota::Outcome::ProtocolError);
        set_check_error("invalid firmware version"); return;
    }
    std::memcpy(policy.expected, pre.available.c_str(), pre.available.size() + 1);
    std::memcpy(policy.current, pre.current.c_str(), pre.current.size() + 1);
    if (!operation.writer.configure(policy)) {
        operation.coordinator.cancel(tk::ota::Outcome::InvalidState);
        set_check_error("could not configure image policy"); return;
    }
    const std::string firmware_url = resolve_firmware_url(operation.target_pr, operation.channel);
    tk::ota::HttpLimits limits{kMaximumImageBytes};
    limits.allow_eof_image = true; // SDK image verification remains mandatory at end/selection.
    tk::ota::memory_checkpoint("image_ready", operation.job.generation, operation.job.started_ms);
    if (!operation.coordinator.start(firmware_url.c_str(), limits, true)) {
        const bool expired = !operation.budget.live(operation_now(nullptr));
        operation.coordinator.cancel(expired ? tk::ota::Outcome::Deadline : tk::ota::Outcome::ProtocolError);
        set_check_error(expired ? "update deadline reached" : "could not start download"); return;
    }
    for (;;) {
        tk::StackWatchScope f135_ota_chunk_exit(tk::StackWatch::OtaWriter);
        const auto state = poll_operation(operation);
        if (state == tk::ota::Outcome::Committed) {
            set_state(OtaState::Done, 100, "update complete — rebooting");
            vTaskDelay(pdMS_TO_TICKS(1200));
            tk::stack_watch_sample(tk::StackWatch::OtaWriter); // before terminal effect
            esp_restart(); // A returning restart never releases this committed operation.
            return;
        }
        if (state != tk::ota::Outcome::Working) {
            set_check_error(state == tk::ota::Outcome::NoMemory ? kOtaNoMemory :
                            state == tk::ota::Outcome::Deadline ? "update deadline reached" :
                            state == tk::ota::Outcome::WriterUncertain ? "update outcome is uncertain" :
                            "download or image validation failed");
            return;
        }
        const uint64_t total = operation.coordinator.declared_size();
        const int percentage = total ? static_cast<int>(operation.coordinator.body_bytes() * 100 / total) : 0;
        set_state(OtaState::Downloading, percentage, "downloading");
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

[[noreturn]] static void returning_restart_park() noexcept {
    // Includes uncertain boot selection and any unconfirmed raw writer cleanup. No automatic
    // release, second write, foreign close, retry or reboot-budget refill is granted here.
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}

static void complete_operation(OtaOperation& operation) noexcept {
    if (!operation.coordinator.writer_lease_released() &&
        operation.coordinator.outcome() == tk::ota::Outcome::MetadataReady)
        operation.coordinator.finish_metadata_operation();
    else if (!operation.coordinator.writer_lease_released() &&
             (operation.coordinator.outcome() == tk::ota::Outcome::Idle ||
              operation.coordinator.outcome() == tk::ota::Outcome::Working))
        operation.coordinator.cancel(tk::ota::Outcome::NetworkError);
    if (!operation.coordinator.writer_lease_released()) {
        tk::stack_watch_sample(operation.image ? tk::StackWatch::OtaWriter : tk::StackWatch::OtaCheck);
        returning_restart_park();
    }
    // This is the coordinator's final Operation access. Network Ack is independently required
    // before a later admission may reclaim its memory. No destructor references operation.
    operation.coordinator_done.store(true, std::memory_order_release);
}

static void finish_check(OtaOperation& operation, OtaCheckResult& result) {
    if (!operation.coordinator.writer_lease_released() &&
        operation.coordinator.outcome() == tk::ota::Outcome::MetadataReady)
        operation.coordinator.finish_metadata_operation();
    if (operation.coordinator.outcome() == tk::ota::Outcome::Deadline) {
        result.ok = result.update_available = false;
        result.reason = "update check deadline reached";
    }
}

class CoordinatorCompletion {
public:
    explicit CoordinatorCompletion(OtaOperation& operation) noexcept : operation_(operation) {}
    ~CoordinatorCompletion() noexcept { complete_operation(operation_); }
    CoordinatorCompletion(const CoordinatorCompletion&) = delete;
    CoordinatorCompletion& operator=(const CoordinatorCompletion&) = delete;
private:
    OtaOperation& operation_;
};

static void run_coordinator_task(void* argument, bool image) noexcept {
    auto& operation = *static_cast<OtaOperation*>(argument);
    CoordinatorCompletion completion(operation);
    try {
        if (!start_network_task(operation)) set_check_error(
            operation.coordinator.outcome() == tk::ota::Outcome::Deadline
                ? "update deadline reached during setup" : "could not start network task");
        else if (image) ota_task_impl(operation);
        else {
            auto result = perform_check(operation, operation.target_pr);
            finish_check(operation, result);
            set_check_done(result);
        }
    } catch (const std::bad_alloc&) {
        operation.coordinator.cancel(tk::ota::Outcome::NoMemory);
        set_check_error(kOtaNoMemory);
    } catch (...) {
        operation.coordinator.cancel(tk::ota::Outcome::NetworkError);
        set_check_error("update operation failed unexpectedly");
    }
    // Completion is the final Operation user, including all exceptional return paths.
}

static void ota_check_task(void* argument) {
    tk::stack_watch_sample(tk::StackWatch::OtaCheck);
    tk::StackWatchScope f135_task_exit(tk::StackWatch::OtaCheck);
    try {
        run_coordinator_task(argument, false);
        tk::stack_watch_sample(tk::StackWatch::OtaCheck);
        vTaskDelete(nullptr);
    } catch (...) { tk::stack_watch_sample(tk::StackWatch::OtaCheck);
        vTaskDelete(nullptr); }
}
static void ota_task(void* argument) {
    tk::stack_watch_sample(tk::StackWatch::OtaWriter);
    tk::StackWatchScope f135_task_exit(tk::StackWatch::OtaWriter);
    try {
        run_coordinator_task(argument, true);
        tk::stack_watch_sample(tk::StackWatch::OtaWriter);
        vTaskDelete(nullptr);
    } catch (...) { tk::stack_watch_sample(tk::StackWatch::OtaWriter);
        vTaskDelete(nullptr); }
}

static tk::OtaStartResult prepare_operation(uint64_t origin, unsigned pr, bool image,
                                           OtaOperation*& operation) {
    using Result = tk::OtaStartResult;
    if (!ensure_lock()) return Result::Unavailable;
    if (!try_begin_ota_operation()) return Result::Busy;
    if (!reclaim_finished_operation()) {
        finish_operation(tk::OtaIdentityGateState::Ota);
        return Result::Busy; // A cleanup-quarantined Network context remains charged.
    }
    if (image) {
        const auto verification = ota_refresh_verification_cache();
        if (verification != tk::OtaVerificationState::Stable) {
            finish_operation(tk::OtaIdentityGateState::Ota);
            return verification == tk::OtaVerificationState::PendingVerify
                ? Result::PendingVerification : Result::Unavailable;
        }
    }
    try {
    // No new Operation/task/body/TLS exists at this ordinary reference. The actual Ota owner
    // binds the requested target and fixed refusal publication against a successor.
    s_target_pr.store(pr, std::memory_order_release);
    const auto entry_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    tk::ota::memory_checkpoint("before_operation", 1, origin, entry_largest);
    if (!tk::ota_recovery_memory_affordable(entry_largest,
            tk::ota_recovery_required_bytes(tk::OtaRecoveryUse::OperationEntry))) {
        set_check_error(kOtaNoMemory); // Fixed POD withdraws any older offer under this owner.
        finish_operation(tk::OtaIdentityGateState::Ota);
        return Result::NoMemory;
    }
    s_running.store(true, std::memory_order_release);
    s_updating.store(image, std::memory_order_release);
    s_heap_abort_requested.store(false, std::memory_order_release);
    operation = new (std::nothrow) OtaOperation(origin, pr, ota_get_channel(), image);
    if (!operation) {
        set_check_error(kOtaNoMemory);
        s_updating.store(false, std::memory_order_release);
        s_running.store(false, std::memory_order_release);
        finish_operation(tk::OtaIdentityGateState::Ota);
        return Result::InternalError;
    }
    if (!s_network_slot.claim(operation->job.generation)) {
        // No worker/raw writer exists; release only this actual owner before discarding it.
        (void)operation->lease.release();
        delete operation; operation = nullptr;
        return Result::Busy;
    }
    s_operation.store(operation, std::memory_order_release);
    tk::ota::memory_checkpoint("operation_ready", operation->job.generation, operation->job.started_ms);
    set_state(image ? OtaState::Downloading : OtaState::Checking, 0,
              image ? "starting download" : pr ? "checking for PR preview" : "checking for updates");
    } catch (...) {
        if (operation) {
            acknowledge_no_network_task(*operation);
            operation->coordinator.cancel(tk::ota::Outcome::NoMemory);
            complete_operation(*operation);
        } else {
            s_updating.store(false, std::memory_order_release);
            s_running.store(false, std::memory_order_release);
            finish_operation(tk::OtaIdentityGateState::Ota);
        }
        set_check_error(kOtaNoMemory);
        return Result::InternalError;
    }
    return Result::Started;
}

static tk::OtaStartResult launch_operation(uint64_t origin, unsigned pr, bool image) {
    OtaOperation* operation = nullptr;
    const auto result = prepare_operation(origin, pr, image, operation);
    if (result != tk::OtaStartResult::Started) return result;
    try {
    const BaseType_t created = image
        ? xTaskCreate(ota_task, "ota", kOtaTaskStackBytes, operation, tk::kPrioOta, nullptr)
        : xTaskCreate(ota_check_task, "ota_chk", kOtaTaskStackBytes, operation, tk::kPrioOtaCheck, nullptr);
    if (created != pdPASS) {
        acknowledge_no_network_task(*operation);
        operation->coordinator.cancel(tk::ota::Outcome::NoMemory);
        set_check_error("could not start OTA coordinator task");
        complete_operation(*operation);
        return tk::OtaStartResult::InternalError;
    }
    } catch (...) {
        set_check_error("OTA task creation outcome is uncertain");
        return tk::OtaStartResult::InternalError; // No false Ack, delete or owner release.
    }
    return tk::OtaStartResult::Started;
}

tk::OtaStartResult ota_check_start(unsigned pr_number) {
    // Capture t0 before status-lock, job, URL, buffer or either task allocation.
    const uint64_t origin = operation_now(nullptr);
    return launch_operation(origin, pr_number, false);
}
tk::OtaStartResult ota_start(unsigned pr_number) {
    const uint64_t origin = operation_now(nullptr);
    return launch_operation(origin, pr_number, true);
}

OtaCheckResult ota_check(unsigned pr_number) {
    // Preserve the documented blocking background API: its caller is the coordinator, never
    // the network worker. HTTP continues to call only ota_check_start/ota_start.
    const uint64_t origin = operation_now(nullptr);
    OtaOperation* operation = nullptr;
    const auto admission = prepare_operation(origin, pr_number, false, operation);
    OtaCheckResult result{};
    result.target_pr = pr_number;
    if (admission != tk::OtaStartResult::Started) {
        result.reason = tk::ota_start_reason(admission, true);
        return result;
    }
    CoordinatorCompletion completion(*operation);
    try {
        if (!start_network_task(*operation)) result.reason =
            operation->coordinator.outcome() == tk::ota::Outcome::Deadline
                ? "update deadline reached during setup" : "could not start network task";
        else result = perform_check(*operation, pr_number);
    } catch (const std::bad_alloc&) {
        operation->coordinator.cancel(tk::ota::Outcome::NoMemory);
        result.ok = result.update_available = false; result.reason = kOtaNoMemory;
    } catch (...) {
        operation->coordinator.cancel(tk::ota::Outcome::NetworkError);
        result.ok = result.update_available = false; result.reason = "update check failed unexpectedly";
    }
    finish_check(*operation, result);
    set_check_done(result); // The public blocking API must not leave its initial Checking state.
    return result;
}
