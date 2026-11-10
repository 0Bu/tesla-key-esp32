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
#include <esp_http_server.h>
#include <esp_random.h>
#include <lwip/sockets.h>
#include "ota_writer.hpp"
#include "ota_hash.hpp"
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
#include <algorithm>
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

struct OtaStatusPod : OtaStatus {
    uint32_t channel_state{0}, generation{0};
    std::array<uint8_t, 32> manifest_sha256{}, notes_sha256{};
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

static void set_state(OtaState st, int pct, const char* msg, bool upload = false) {
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
    s_status.upload    = upload;
}

static std::atomic<uint32_t> s_ota_channel{0}; // packed channel plus nonwrapping revision
static std::atomic<uint32_t> s_offer_serial{0}; // only the actual Ota owner advances; readers are lock-free

static uint64_t offer_boot_nonce() noexcept {
    // Initialize only outside the status lock. The nonce is public identity, not a credential.
    static const uint64_t nonce = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
    return nonce;
}

static OtaStatus unavailable_status_snapshot() {
    OtaStatus result{};
    result.state = OtaState::Error;
    copy_status_text(result.message, "unavailable");
    result.channel = result.configured_channel = ota_get_channel();
    return result;
}

tk::OtaChannel ota_get_channel() {
    return tk::ota_channel_in_state(s_ota_channel.load(std::memory_order_acquire));
}

void ota_set_channel(tk::OtaChannel channel) {
    auto observed = s_ota_channel.load(std::memory_order_acquire);
    for (;;) {
        const auto next = tk::ota_changed_channel_state(observed, channel);
        if (next == observed || s_ota_channel.compare_exchange_strong(observed, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) return;
    }
}

OtaStatus ota_get_status() {
    SemaphoreHandle_t lock = ensure_lock();
    if (!lock) return unavailable_status_snapshot();
    OtaStatusPod snapshot{};
    uint32_t configured = 0;
    uint32_t latest = 0;
    {
        tk::SemGuard g(lock);
        if (!g) return unavailable_status_snapshot();
        snapshot = s_status;  // fixed POD/arrays only; noexcept and cross-generation coherent
        configured = s_ota_channel.load(std::memory_order_acquire);
        latest = s_offer_serial.load(std::memory_order_acquire);
    }
    snapshot.configured_channel = tk::ota_channel_in_state(configured);
    if (snapshot.generation != latest || !tk::ota_offer_current(snapshot.channel_state, configured, snapshot.generation,
                               snapshot.update_available)) {
        // A withdrawn completed offer must not keep reading as "update available".
        if (snapshot.update_available) {
            snapshot.offer_changed = true;
            copy_status_text(snapshot.message, tk::ota_start_reason(tk::OtaStartResult::OfferChanged, false));
        }
        snapshot.update_available = false;
        if (snapshot.upload || (snapshot.state != OtaState::Downloading && snapshot.state != OtaState::Done))
            snapshot.available.fill('\0');
        snapshot.offer_id.fill('\0');
        snapshot.image_sha256.fill(0);
        snapshot.image_bytes = 0;
    }
    return static_cast<const OtaStatus&>(snapshot); // no string materialization/allocation
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

bool ota_get_changelog(char* out, size_t max_len, size_t& out_len,
                       std::string_view offer_id, bool* stale) {
    out_len = 0;
    if (stale) *stale = !offer_id.empty();
    if (!out || max_len == 0) return false;
    SemaphoreHandle_t lock = ensure_lock();
    if (!lock) return false;
    tk::SemGuard g(lock);
    if (!g || s_status.generation != s_offer_serial.load(std::memory_order_acquire) ||
        !tk::ota_offer_current(s_status.channel_state,
            s_ota_channel.load(std::memory_order_acquire), s_status.generation,
            s_status.update_available) ||
        (!offer_id.empty() && offer_id != s_status.offer_id.data())) return false;
    if (stale) *stale = false;
    if (s_changelog_len == 0) return false;
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
    if (!candidate.update_available) set_changelog_locked(nullptr);
}

// Every error names the channel its owner captured. Generation zero never forms an offer.
static void set_check_error(const char* message, uint32_t channel_state, bool upload = false) noexcept {
    OtaStatusPod candidate{};
    candidate.state = OtaState::Error;
    candidate.target_pr = s_target_pr.load(std::memory_order_relaxed);
    candidate.channel_state = channel_state;
    candidate.channel = tk::ota_channel_in_state(channel_state);
    candidate.upload = upload;
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
    static bool release_owned() noexcept {
        if (s_operation_gate.state() != tk::OtaIdentityGateState::Ota) return false;
        s_updating.store(false, std::memory_order_release);
        s_running.store(false, std::memory_order_release);
        return s_operation_gate.finish(tk::OtaIdentityGateState::Ota);
    }
    bool release() noexcept {
        if (!held_) return true;
        // Publish quiescence before releasing authority; no later global flag store can clear
        // a successor operation's flags. This method is called only after raw writer abort/end.
        if (!release_owned()) return false;
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
    uint32_t channel_state{0}, offer_generation{0};
    uint64_t boot_nonce{0};
    OtaCheckResult expected{};
    std::atomic<bool> coordinator_done{false};
    NetworkLaunch network_launch{NetworkLaunch::NotAttempted}; // Coordinator-only.
    httpd_req_t* upload_request{nullptr}; // Exclusive async ownership, never a borrowed request.
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
    const auto current = running_version();
    if (!tk::canonical_ota_version(current)) {
        res.reason = "invalid firmware version";
        return res;
    }
    copy_status_text(res.current, current.data());
    const tk::OtaChannel channel = operation.channel;

    std::string manifest_url_storage;
    const char* manifest_url = tk::resolve_manifest_url_into(
        pr_number, channel, CONFIG_TESLA_OTA_MANIFEST_URL, manifest_url_storage);
    ESP_LOGI(TAG, "checking %s (channel %s, running %s)", manifest_url,
             tk::ota_channel_name(channel), res.current.data());

    std::string body;
    const auto manifest_fetch = http_get_to_buffer(operation, manifest_url, body);
    if (manifest_fetch != OtaFetchResult::Ok) {
        res.ok     = false;
        res.reason = manifest_fetch == OtaFetchResult::NoMemory
                         ? kOtaNoMemory : manifest_fetch == OtaFetchResult::Deadline ? "update check deadline reached" : "could not reach update server";
        return res;
    }

    {
        if (tk::ota::sha256(body.data(), body.size(), res.manifest_sha256) != PSA_SUCCESS) {
            res.reason = "could not identify update manifest";
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
        tk::OtaManifestImage image;
        if (parse_end != body.c_str() + body.size() ||
            manifest.status != tk::OtaManifestInspectStatus::Valid || !manifest.value ||
            !tk::inspect_ota_image(j.get(), "tesla-key-esp32" TESLA_OTA_IMG_SUFFIX ".bin", image)) {
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
        copy_status_text(res.available, manifest.value);
        res.image_sha256 = image.sha256;
        res.image_bytes = image.bytes;
    } // Notes need only the copied version; free the tree before their body/TLS allocations.

    res.ok               = true;
    res.update_available = tk::is_ota_update_available(res.available.data(), res.current.data(), pr_number, channel);
    res.reason           = res.update_available ? "update available" : "up to date";
    ESP_LOGI(TAG, "available %s — %s", res.available.data(), res.reason);

    if (tk::ota::sha256("", 0, res.notes_sha256) != PSA_SUCCESS) {
        res.ok = res.update_available = false;
        res.reason = "could not identify release notes";
        return res;
    }

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
                                           res.available.data(), decoded, sizeof(decoded))) {
                    if (tk::ota_changelog_select_range(decoded, res.current.data(), res.available.data()) !=
                        tk::OtaChangelogRangeResult::Invalid) {
                        if (tk::ota::sha256(decoded, std::strlen(decoded), res.notes_sha256) != PSA_SUCCESS) {
                            res.ok = res.update_available = false;
                            res.reason = "could not identify release notes";
                            return res;
                        }
                        SemaphoreHandle_t lock = ensure_lock();
                        if (lock && !operation.image) {
                            tk::SemGuard g(lock);
                            if (g && s_status.generation == operation.offer_generation &&
                                operation.channel_state == s_ota_channel.load(std::memory_order_acquire))
                                set_changelog_locked(decoded);
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
static void set_check_done(const OtaCheckResult& r, const OtaOperation& operation) {
    OtaStatusPod candidate{};
    candidate.state = r.ok ? OtaState::Idle : OtaState::Error;
    candidate.progress = 0;
    candidate.update_available = r.ok && r.update_available && !operation.image;
    candidate.target_pr = r.target_pr;
    copy_status_text(candidate.message, r.reason);
    candidate.available = r.available;
    candidate.current = r.current;
    candidate.channel = operation.channel;
    candidate.channel_state = operation.channel_state;
    candidate.generation = operation.offer_generation;
    candidate.manifest_sha256 = r.manifest_sha256;
    candidate.notes_sha256 = r.notes_sha256;
    candidate.image_sha256 = r.image_sha256;
    candidate.image_bytes = r.image_bytes;
    if (candidate.update_available &&
        !tk::format_ota_offer_id(candidate.offer_id, operation.boot_nonce, candidate.generation)) {
        candidate.update_available = false;
        candidate.state = OtaState::Error;
        copy_status_text(candidate.message, "update offer identity unavailable");
    }
    SemaphoreHandle_t lock = ensure_lock();
    if (!lock) return;
    tk::SemGuard g(lock);
    if (!g || s_status.generation != operation.offer_generation ||
        operation.offer_generation != s_offer_serial.load(std::memory_order_acquire)) return;
    if (operation.channel_state != s_ota_channel.load(std::memory_order_acquire)) {
        candidate.state = OtaState::Error;
        candidate.update_available = false;
        copy_status_text(candidate.message, "update channel changed; check again");
    }
    s_status = candidate;
    if (!candidate.update_available) set_changelog_locked(nullptr);
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
static void ota_task_impl(OtaOperation& operation, const OtaCheckResult& pre) {
    if (!pre.ok || !pre.update_available) {
        // One publication: install polling must never read an intermediate Idle record as success.
        if (pre.ok) set_check_error("no newer version available", operation.channel_state);
        else set_check_done(pre, operation); // Fresh failures/OOM withdraw any earlier offer.
        return;
    }
    if (operation.channel_state != s_ota_channel.load(std::memory_order_acquire) ||
        pre.available != operation.expected.available ||
        pre.manifest_sha256 != operation.expected.manifest_sha256 ||
        pre.notes_sha256 != operation.expected.notes_sha256 ||
        pre.image_sha256 != operation.expected.image_sha256 || pre.image_bytes != operation.expected.image_bytes) {
        set_check_error(tk::ota_start_reason(tk::OtaStartResult::OfferChanged, false), operation.channel_state);
        operation.coordinator.cancel(tk::ota::Outcome::ProtocolError);
        return; // No image transfer, raw write or boot selection for a replaced candidate.
    }
    {
        tk::ota::ImagePolicy policy{};
        policy.target_pr = operation.target_pr;
        policy.channel = operation.channel;
        std::memcpy(policy.expected, pre.available.data(), sizeof(policy.expected));
        std::memcpy(policy.current, pre.current.data(), sizeof(policy.current));
        policy.expected_sha256 = operation.expected.image_sha256;
        policy.expected_bytes = operation.expected.image_bytes;
        if (!operation.writer.configure(policy)) {
            operation.coordinator.cancel(tk::ota::Outcome::InvalidState);
            set_check_error("could not configure image policy", operation.channel_state); return;
        }
    }
    const std::string firmware_url = resolve_firmware_url(operation.target_pr, operation.channel);
    tk::ota::HttpLimits limits{kMaximumImageBytes};
    limits.allow_eof_image = true; // SDK image verification remains mandatory at end/selection.
    tk::ota::memory_checkpoint("image_ready", operation.job.generation, operation.job.started_ms);
    if (!operation.coordinator.start(firmware_url.c_str(), limits, true)) {
        const bool expired = !operation.budget.live(operation_now(nullptr));
        operation.coordinator.cancel(expired ? tk::ota::Outcome::Deadline : tk::ota::Outcome::ProtocolError);
        set_check_error(expired ? "update deadline reached" : "could not start download", operation.channel_state);
        return;
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
                            "download or image validation failed", operation.channel_state);
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

static void finish_upload_request(OtaOperation& operation, bool committed,
                                  const char* failure_status = "400 Bad Request") noexcept {
    auto* req = operation.upload_request;
    if (!req) return;
    operation.upload_request = nullptr;
    // Retire RX/TX while the async request still owns the session: no unread body drain,
    // queued-close/reused-fd race, or per-request heap allocation on any failure path.
    (void)httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_set_status(req, committed ? "200 OK" : failure_status);
    const bool uncertain = operation.coordinator.outcome() == tk::ota::Outcome::WriterUncertain;
    (void)httpd_resp_sendstr(req, committed
        ? "{\"result\":true,\"reason\":\"installed; rebooting\"}"
        : uncertain ? "{\"result\":false,\"outcome\":\"unknown\",\"reason\":\"uncertain; do not retry\"}"
        : "{\"result\":false,\"reason\":\"upload failed; inspect /ota/status\"}");
    (void)shutdown(httpd_req_to_sockfd(req), SHUT_RDWR);
    (void)httpd_req_async_handler_complete(req);
}

static void ota_upload_impl(OtaOperation& operation) {
    auto* req = operation.upload_request;
    tk::ota::ImagePolicy policy{};
    policy.channel = tk::OtaChannel::Dev;
    policy.direct_upload = true;
    const auto current = running_version();
    if (!tk::canonical_ota_version(current) || current.size() >= sizeof(policy.current)) {
        set_check_error("invalid firmware version", operation.channel_state, true); return;
    }
    std::memcpy(policy.current, current.data(), current.size());
    if (ota_get_channel() != tk::OtaChannel::Dev || !operation.writer.configure(policy) ||
        !operation.coordinator.start_upload(req->content_len)) {
        set_check_error("upload refused", operation.channel_state, true); return;
    }
    size_t received = 0;
    while (received < req->content_len) {
        tk::StackWatchScope f135_ota_chunk_exit(tk::StackWatch::OtaWriter);
        if (s_heap_abort_requested.load(std::memory_order_acquire) ||
            ota_get_channel() != tk::OtaChannel::Dev ||
            !operation.budget.live(operation_now(nullptr))) {
            operation.coordinator.cancel(tk::ota::Outcome::Deadline);
            set_check_error("upload aborted", operation.channel_state, true); return;
        }
        const size_t want = std::min(operation.job.block.size(), req->content_len - received);
        const int count = httpd_req_recv(req, reinterpret_cast<char*>(operation.job.block.data()), want);
        if (count <= 0 || static_cast<size_t>(count) > want) {
            operation.coordinator.cancel(tk::ota::Outcome::NetworkError);
            set_check_error("upload interrupted", operation.channel_state, true); return;
        }
        if (!operation.coordinator.upload(operation.job.block.data(), static_cast<size_t>(count))) {
            set_check_error("upload validation failed", operation.channel_state, true); return;
        }
        received += static_cast<size_t>(count);
        set_state(OtaState::Downloading, static_cast<int>(received * 100 / req->content_len), "uploading", true);
    }
    if (s_heap_abort_requested.load(std::memory_order_acquire) ||
        ota_get_channel() != tk::OtaChannel::Dev) {
        operation.coordinator.cancel(tk::ota::Outcome::Deadline);
        set_check_error("upload aborted", operation.channel_state, true); return;
    }
    operation.coordinator.finish_upload(); // Same SDK signature check and boot selection as pull OTA.
    if (operation.coordinator.outcome() != tk::ota::Outcome::Committed) {
        set_check_error(operation.coordinator.outcome() == tk::ota::Outcome::WriterUncertain
            ? "upload uncertain; do not retry"
            : "upload validation failed", operation.channel_state, true); return;
    }
    set_state(OtaState::Done, 100, "update complete — rebooting", true);
    finish_upload_request(operation, true);
    vTaskDelay(pdMS_TO_TICKS(1200));
    tk::stack_watch_sample(tk::StackWatch::OtaWriter);
    esp_restart(); // Committed ownership remains sealed even if restart returns.
}

static void run_coordinator_task(void* argument, bool image) noexcept {
    auto& operation = *static_cast<OtaOperation*>(argument);
    CoordinatorCompletion completion(operation);
    try {
        if (operation.upload_request) ota_upload_impl(operation);
        else if (!start_network_task(operation)) set_check_error(
            operation.coordinator.outcome() == tk::ota::Outcome::Deadline
                ? "update deadline reached during setup" : "could not start network task",
            operation.channel_state);
        else {
            // Check and install share one metadata snapshot in this task frame.
            auto result = perform_check(operation, operation.target_pr);
            if (image) ota_task_impl(operation, result);
            else {
                finish_check(operation, result);
                set_check_done(result, operation);
            }
        }
    } catch (const std::bad_alloc&) {
        operation.coordinator.cancel(tk::ota::Outcome::NoMemory);
        set_check_error(kOtaNoMemory, operation.channel_state, operation.upload_request != nullptr);
    } catch (...) {
        operation.coordinator.cancel(tk::ota::Outcome::NetworkError);
        set_check_error("update operation failed unexpectedly", operation.channel_state,
                        operation.upload_request != nullptr);
    }
    if (operation.upload_request) {
        finish_upload_request(operation, false);
        acknowledge_no_network_task(operation);
    } else if (operation.network_launch == NetworkLaunch::NotAttempted) {
        acknowledge_no_network_task(operation);
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

static void release_unstarted_operation() noexcept {
    (void)OtaLease::release_owned();
}

// Lock creation precedes this helper. Every remaining operation is allocation-free/noexcept,
// so a successful return transfers exactly the gate acquired here to the caller.
static tk::OtaStartResult reserve_operation(unsigned pr, bool image) noexcept {
    using Result = tk::OtaStartResult;
    if (!try_begin_ota_operation()) return Result::Busy;
    if (!reclaim_finished_operation()) {
        release_unstarted_operation();
        return Result::Busy; // A cleanup-quarantined Network context remains charged.
    }
    if (image) {
        const auto verification = ota_refresh_verification_cache();
        if (verification != tk::OtaVerificationState::Stable) {
            release_unstarted_operation();
            return verification == tk::OtaVerificationState::PendingVerify
                ? Result::PendingVerification : Result::Unavailable;
        }
    }
    s_target_pr.store(pr, std::memory_order_release);
    {
        tk::SemGuard g(s_lock.load(std::memory_order_acquire));
        if (!g) {
            release_unstarted_operation();
            return Result::Unavailable;
        }
    }
    // The old operation is quiescent. A new abort after this point belongs to this reservation
    // and must remain sticky through a queued HTTP callback and coordinator creation.
    s_heap_abort_requested.store(false, std::memory_order_release);
    return Result::Started;
}

static uint32_t advance_offer_generation() noexcept {
    const auto next = tk::ota_next_offer_generation(s_offer_serial.load(std::memory_order_relaxed));
    s_offer_serial.store(next, std::memory_order_release);
    return next;
}

static bool publish_operation_start(bool image, uint32_t channel_state, uint32_t generation,
                                     const OtaCheckResult* expected = nullptr) {
    s_running.store(true, std::memory_order_release);
    s_updating.store(image, std::memory_order_release);
    OtaStatusPod candidate{};
    candidate.state = image ? OtaState::Downloading : OtaState::Checking;
    candidate.target_pr = s_target_pr.load(std::memory_order_relaxed);
    candidate.channel_state = channel_state;
    candidate.channel = tk::ota_channel_in_state(channel_state);
    candidate.generation = generation;
    candidate.upload = image && !expected;
    if (image && expected) candidate.available = expected->available; // active confirmed target, no reusable offer
    copy_status_text(candidate.message, image ? "starting download" : "checking for updates");
    copy_status_text(candidate.current, running_version().data());
    const auto lock = ensure_lock();
    if (!lock) return false;
    tk::SemGuard guard(lock);
    if (!guard) return false;
    s_status = candidate;
    set_changelog_locked(nullptr);
    return true;
}

static tk::OtaStartResult prepare_owned_operation(uint64_t origin, unsigned pr,
        uint32_t channel_state, bool image, OtaOperation*& operation, uint32_t generation,
        const OtaCheckResult* expected = nullptr) {
    using Result = tk::OtaStartResult;
    const bool upload = image && !expected;
    try {
    // No new Operation/task/body/TLS exists at this ordinary reference. The actual Ota owner
    // binds the requested target and fixed refusal publication against a successor.
    const auto entry_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    tk::ota::memory_checkpoint("before_operation", 1, origin, entry_largest);
    if (!tk::ota_recovery_memory_affordable(entry_largest,
            tk::ota_recovery_required_bytes(tk::OtaRecoveryUse::OperationEntry))) {
        set_check_error(kOtaNoMemory, channel_state, upload); // Withdraw any older offer under this owner.
        release_unstarted_operation();
        return Result::NoMemory;
    }
    if (!publish_operation_start(image, channel_state, generation, expected)) {
        release_unstarted_operation();
        return Result::Unavailable;
    }
    operation = new (std::nothrow) OtaOperation(origin, pr, tk::ota_channel_in_state(channel_state), image);
    if (!operation) {
        set_check_error(kOtaNoMemory, channel_state, upload);
        release_unstarted_operation();
        return Result::InternalError;
    }
    operation->channel_state = channel_state;
    operation->offer_generation = generation;
    operation->boot_nonce = offer_boot_nonce();
    if (expected) operation->expected = *expected;
    if (!s_network_slot.claim(operation->job.generation)) {
        // No worker/raw writer exists; release only this actual owner before discarding it.
        (void)operation->lease.release();
        delete operation; operation = nullptr;
        return Result::Busy;
    }
    s_operation.store(operation, std::memory_order_release);
    tk::ota::memory_checkpoint("operation_ready", operation->job.generation, operation->job.started_ms);
    } catch (...) {
        set_check_error(kOtaNoMemory, channel_state, upload); // Publish under this owner before lease release.
        if (operation) {
            acknowledge_no_network_task(*operation);
            operation->coordinator.cancel(tk::ota::Outcome::NoMemory);
            complete_operation(*operation);
        } else {
            release_unstarted_operation();
        }
        return Result::InternalError;
    }
    return Result::Started;
}

static tk::OtaStartResult prepare_operation(uint64_t origin, unsigned pr, bool image,
                                           OtaOperation*& operation) {
    if (image) return tk::OtaStartResult::OfferChanged; // No unbound internal install path.
    if (!ensure_lock()) return tk::OtaStartResult::Unavailable;
    const auto result = reserve_operation(pr, image);
    if (result != tk::OtaStartResult::Started) return result;
    const auto captured = s_ota_channel.load(std::memory_order_acquire);
    const auto generation = advance_offer_generation();
    if ((captured & tk::kOtaChannelExhausted) || generation == UINT32_MAX) {
        set_check_error("update offer identity exhausted; reboot required", captured);
        release_unstarted_operation();
        return tk::OtaStartResult::Unavailable;
    }
    return prepare_owned_operation(origin, pr, captured, false, operation, generation);
}

static tk::OtaStartResult launch_coordinator(OtaOperation* operation, bool image) {
    const uint32_t channel_state = operation->channel_state;
    const bool upload = operation->upload_request != nullptr;
    try {
    const BaseType_t created = image
        ? xTaskCreate(ota_task, "ota", kOtaTaskStackBytes, operation, tk::kPrioOta, nullptr)
        : xTaskCreate(ota_check_task, "ota_chk", kOtaTaskStackBytes, operation, tk::kPrioOtaCheck, nullptr);
    if (created != pdPASS) {
        set_check_error("could not start OTA coordinator task", channel_state, upload);
        finish_upload_request(*operation, false, "500 Internal Server Error");
        acknowledge_no_network_task(*operation);
        operation->coordinator.cancel(tk::ota::Outcome::NoMemory);
        complete_operation(*operation);
        return tk::OtaStartResult::InternalError;
    }
    } catch (...) {
        set_check_error("OTA task creation outcome is uncertain", channel_state, upload);
        return tk::OtaStartResult::InternalError; // No false Ack, delete or owner release.
    }
    return tk::OtaStartResult::Started;
}

tk::OtaStartResult ota_upload_start(httpd_req_t* request, bool& handed_off) noexcept {
    using Result = tk::OtaStartResult;
    const uint64_t origin = operation_now(nullptr);
    handed_off = false;
    if (!request || ota_get_channel() != tk::OtaChannel::Dev || !ensure_lock())
        return Result::Unavailable;
    OtaOperation* operation = nullptr;
    const auto admission = reserve_operation(0, true);
    if (admission != Result::Started) return admission;
    const auto captured = s_ota_channel.load(std::memory_order_acquire);
    const auto generation = advance_offer_generation(); // Also withdraws an old feed offer.
    const auto prepared = prepare_owned_operation(origin, 0, captured, true, operation, generation);
    if (prepared != Result::Started) return prepared;
    if (ota_get_channel() != tk::OtaChannel::Dev ||
        httpd_req_async_handler_begin(request, &operation->upload_request) != ESP_OK) {
        acknowledge_no_network_task(*operation);
        operation->coordinator.cancel(tk::ota::Outcome::NoMemory);
        set_check_error("upload request unavailable", captured, true);
        complete_operation(*operation);
        return Result::NoMemory;
    }
    handed_off = true; // No original request/socket use after the coordinator can run.
    return launch_coordinator(operation, true);
}

static tk::OtaStartResult launch_operation(uint64_t origin, unsigned pr, bool image) {
    OtaOperation* operation = nullptr;
    const auto result = prepare_operation(origin, pr, image, operation);
    return result == tk::OtaStartResult::Started ? launch_coordinator(operation, image) : result;
}

// A single HTTP server task submits/consumes this fixed capture. The actual Ota gate excludes
// replacement until callback consumption, refusal, or coordinator lease release. A nonwrapping
// opaque ticket, rather than a pointer to reused capture storage, rejects stale callbacks.
#if defined(CONFIG_HTTPD_QUEUE_WORK_BLOCKING) && CONFIG_HTTPD_QUEUE_WORK_BLOCKING
#error "Deferred OTA metadata admission requires nonblocking HTTP work submission"
#endif
struct DeferredCheckCapture { uint64_t origin; unsigned pr; uint32_t channel_state, generation; };
static struct {
    DeferredCheckCapture capture{};
    uint32_t ticket{0}; // Submission and consumption run only on this same HTTP server task.
} s_deferred_check;
static uint32_t s_deferred_serial{0}; // Protected by the actual Ota owner, never wraps.
static constexpr uint32_t kDeferredTicketLimit = uint32_t{1} << 31;

static void ota_check_after_http_work(void* argument) noexcept {
    tk::StackWatchScope f135_callback_exit(tk::StackWatch::Httpd);
    try {
        const auto token = reinterpret_cast<std::uintptr_t>(argument);
        // Compare before narrowing: a wider host token must not alias a valid 32-bit ticket.
        if (token == 0 || token != s_deferred_check.ticket) return;
        const DeferredCheckCapture captured = s_deferred_check.capture;
        s_deferred_check.ticket = 0;
        // Use the local copy after retirement/release. Only this HTTP task writes the capture, so a
        // successor can replace it only after this callback returns.
        if (s_heap_abort_requested.load(std::memory_order_acquire) ||
            !operation_budget(captured.origin, false).live(operation_now(nullptr))) {
            set_check_error("update deadline reached during setup", captured.channel_state);
            release_unstarted_operation();
            return;
        }
        OtaOperation* operation = nullptr;
        if (prepare_owned_operation(captured.origin, captured.pr, captured.channel_state, false,
                                    operation, captured.generation) == tk::OtaStartResult::Started)
            (void)launch_coordinator(operation, false);
    } catch (...) {
        // Never infer a failed queue/task creation from an exception, fabricate Ack or release
        // authority that a possibly started coordinator can still use. The capture is still this
        // callback's: its same-task successor cannot run before return (see above).
        set_check_error("update operation failed unexpectedly", s_deferred_check.capture.channel_state);
    }
}

tk::OtaStartResult ota_check_start_after_http(unsigned pr, void* server) noexcept {
    const uint64_t origin = operation_now(nullptr);
    bool held = false;
    uint32_t captured = 0;
    try {
        if (!server || s_deferred_serial == kDeferredTicketLimit - 1 || !ensure_lock())
            return tk::OtaStartResult::Unavailable;
        const auto reserved = reserve_operation(pr, false);
        if (reserved != tk::OtaStartResult::Started) return reserved;
        captured = s_ota_channel.load(std::memory_order_acquire);
        held = true;
        const auto generation = advance_offer_generation();
        if ((captured & tk::kOtaChannelExhausted) || generation == UINT32_MAX) {
            set_check_error("update offer identity exhausted; reboot required", captured);
            release_unstarted_operation();
            return tk::OtaStartResult::Unavailable;
        }
        const auto ticket = ++s_deferred_serial;
        s_deferred_check.capture = {origin, pr, captured, generation};
        if (!publish_operation_start(false, captured, generation)) {
            release_unstarted_operation();
            return tk::OtaStartResult::Unavailable;
        }
        s_deferred_check.ticket = ticket;
        tk::ota::memory_checkpoint("http_req", ticket, origin);
        if (httpd_queue_work(server, ota_check_after_http_work,
                reinterpret_cast<void*>(static_cast<std::uintptr_t>(ticket))) != ESP_OK) {
            s_deferred_check.ticket = 0;
            set_check_error(tk::ota_start_reason(tk::OtaStartResult::Unavailable, true), captured);
            release_unstarted_operation();
            return tk::OtaStartResult::Unavailable;
        }
        return tk::OtaStartResult::Started; // Accepted queue, not allocation/TLS admission.
    } catch (...) {
        if (held) {
            set_check_error("update operation failed unexpectedly", captured);
            // Same-server work cannot run inline/concurrently with this handler. No Operation
            // exists yet: revoke this nonreused token before release. Even if SDK submission
            // succeeded before throwing, its later callback cannot match a successor's token.
            s_deferred_check.ticket = 0;
            release_unstarted_operation();
        }
        return tk::OtaStartResult::InternalError;
    }
}

tk::OtaStartResult ota_check_start(unsigned pr_number) {
    // Capture t0 before status-lock, job, URL, buffer or either task allocation.
    const uint64_t origin = operation_now(nullptr);
    return launch_operation(origin, pr_number, false);
}
tk::OtaStartResult ota_start(unsigned pr_number, std::string_view offer_id) {
    const uint64_t origin = operation_now(nullptr);
    using Result = tk::OtaStartResult;
    if (!ensure_lock()) return Result::Unavailable;
    const auto admission = reserve_operation(pr_number, true);
    if (admission != Result::Started) return admission;
    OtaCheckResult expected{};
    uint32_t captured = 0;
    bool matches = false;
    {
        tk::SemGuard guard(s_lock.load(std::memory_order_acquire));
        if (guard) {
            captured = s_ota_channel.load(std::memory_order_acquire);
            matches = tk::ota_offer_id_valid(offer_id) && offer_id == s_status.offer_id.data() &&
                s_status.target_pr == pr_number && s_status.generation == s_offer_serial.load(std::memory_order_acquire) &&
                tk::ota_offer_current(s_status.channel_state, captured, s_status.generation, s_status.update_available);
            if (matches) {
                expected.current = s_status.current;
                expected.available = s_status.available;
                expected.manifest_sha256 = s_status.manifest_sha256;
                expected.notes_sha256 = s_status.notes_sha256;
                expected.image_sha256 = s_status.image_sha256;
                expected.image_bytes = s_status.image_bytes;
            }
        }
    }
    if (!matches) {
        release_unstarted_operation();
        return Result::OfferChanged; // Never erase another client's valid successor offer.
    }
    const auto generation = advance_offer_generation(); // A confirmed offer is consumed once.
    if (generation == UINT32_MAX) {
        set_check_error("update offer identity exhausted; reboot required", captured);
        release_unstarted_operation();
        return Result::Unavailable;
    }
    OtaOperation* operation = nullptr;
    const auto prepared = prepare_owned_operation(origin, pr_number, captured, true, operation, generation, &expected);
    return prepared == Result::Started ? launch_coordinator(operation, true) : prepared;
}

OtaCheckResult ota_check(unsigned pr_number) {
    // Preserve the documented blocking background API: its caller is the coordinator, never
    // the network worker. HTTP uses deferred metadata admission or ota_start for images.
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
    set_check_done(result, *operation); // The public blocking API must not leave its initial Checking state.
    return result;
}
