#include "stack_watch.hpp"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <string>
#include <atomic>
#include <exception>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "esp_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "bootloader_random.h"

#include "boot_fatal.hpp"
#include "net.hpp"
#include "ble_client.hpp"
#include "nvs_storage.hpp"
#include "task_config.hpp"
#include "vehicle_ctrl.hpp"
#include "http_server.hpp"
#include "provisioning.hpp"
#include "diag_log.hpp"
#include "diag_crash.hpp"
#include "safe_mode.hpp"
#include "heap_supervisor.hpp"
#include "runtime_admission.hpp"
#include "config_blob.hpp"
#include "ota_update.hpp"
#include "mqtt_ha.hpp"
#include "syslog.hpp"
#include "display.hpp"
#include "led_status.hpp"
#include "time_sync.hpp"
#include "logic/bootlog.hpp"
#include "logic/device_hostname.hpp"
#include "logic/health_gate.hpp"
#include "logic/heap_watchdog.hpp"
#include "logic/vin_transition.hpp"

#if defined(CONFIG_TESLA_CONFIG_REPAIR_USB) && CONFIG_TESLA_CONFIG_REPAIR_USB
#include "config_repair.hpp"
#endif

static const char* TAG = "main";

// Retry ownership while a temporary operation is in flight. The one-second yield is a defined
// retry cadence, not a timeout that silently abandons PendingVerify rollback. A sealed competing
// restart remains authoritative and is never stolen. Stable/unknown metadata releases our lease.
static void fatal_startup_rollback() noexcept {
    for (;;) {
        tk::StackWatchScope f135_fatal_retry_exit(tk::StackWatch::Main);
        OtaFaultRestartGuard owner;
        if (!owner.held()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        try {
            const esp_partition_t* running = esp_ota_get_running_partition();
            esp_ota_img_states_t state{};
            if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK ||
                state != ESP_OTA_IMG_PENDING_VERIFY) return;
            try { ESP_LOGE(TAG, "fatal startup failure on pending OTA image — rolling back"); }
            catch (...) {}
            vTaskDelay(pdMS_TO_TICKS(250));
            // The SDK can change metadata before reporting an error. Retain ownership after this
            // uncertain effect so no other recovery crosses it; this never confirms an image.
            owner.seal();
            try {
                tk::stack_watch_sample(tk::StackWatch::Main); // before terminal effect
                const esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
                ESP_LOGE(TAG, "OTA rollback could not be started: %s", esp_err_to_name(err));
            } catch (...) {}
            return;
        } catch (...) { return; }
    }
}

// An essential failure closes vehicle work permanently for this boot. PendingVerify actively
// rolls back once its operation owner is available; Stable/unknown parks and preserves evidence.
[[noreturn]] void boot_fatal(const char* component) {
    tk::runtime_admission_mark_fatal(tk::runtime_fatal_component(component));
    tk::stack_watch_sample(tk::StackWatch::Main); // before nonreturning park
    try {
        ESP_LOGE(TAG, "FATAL: essential component '%s' failed to initialize; refusing to run a "
                      "partial firmware", component);
    } catch (...) {}
    fatal_startup_rollback();
    try { ESP_LOGE(TAG, "fatal startup halted; external reset or OTA recovery required"); }
    catch (...) {}
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}

// ─── Wall clock ───────────────────────────────────────────────────────────────
// Only a validated, successfully applied NTP sample is authority for durable dates.
// Browser time and the restored checkpoint are temporary estimates, never authority.
static std::atomic<bool> s_ntp_synced{false}; // ever committed this boot; never reset on failure
static NvsStorageAdapter* s_cfg_store = nullptr;
static StaticSemaphore_t s_clock_mutex_storage;
static SemaphoreHandle_t s_clock_mutex = nullptr;
// SNTP calls its hook serially on the TCP/IP task. Browser requests never touch this policy.
static tk::ClockCheckpointPolicy s_clock_checkpoint;

int64_t clock_plausibility_ceiling() noexcept {
    return tk::clock_ceiling_seconds(esp_app_get_description()->date);
}

static void initialize_clock_gate_() {
    // Pinned libc lazily initializes private Adjtime/Boot locks here, before producers and
    // outside our gate. This bootstrap can allocate/abort on SDK OOM; it is not C++-catchable.
    timeval warm{};
    if (gettimeofday(&warm, nullptr) != 0) boot_fatal("clock bootstrap");
    s_clock_mutex = xSemaphoreCreateMutexStatic(&s_clock_mutex_storage);
    if (!s_clock_mutex) boot_fatal("clock mutex");
}

struct ClockOperationGuard {
    bool held{false};
    ClockOperationGuard() noexcept
        : held(s_clock_mutex && xSemaphoreTake(s_clock_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {}
    ~ClockOperationGuard() { if (held) xSemaphoreGive(s_clock_mutex); }
    ClockOperationGuard(const ClockOperationGuard&) = delete;
    ClockOperationGuard& operator=(const ClockOperationGuard&) = delete;
};

bool clock_synced_via_ntp() noexcept { return s_ntp_synced.load(std::memory_order_acquire); }
bool clock_is_authoritative() noexcept { return clock_synced_via_ntp(); }

BrowserClockResult apply_browser_clock_sample(tk::ClockSample sample) noexcept {
    try {
        ClockOperationGuard guard;
        if (!guard.held) return {BrowserClockStatus::Busy, 0};
        // Check INSIDE the same gate that commits the first NTP apply. Once committed,
        // no later browser writer can overwrite it, including an already admitted request.
        if (clock_synced_via_ntp()) return {BrowserClockStatus::AlreadyNtpSynced, 0};
        if (!tk::clock_sample_valid(sample, clock_plausibility_ceiling()))
            return {BrowserClockStatus::Invalid, 0};
        timeval value{static_cast<time_t>(sample.seconds),
                      static_cast<suseconds_t>(sample.microseconds)};
        if (settimeofday(&value, nullptr) != 0) return {BrowserClockStatus::SetFailed, 0};
        return {BrowserClockStatus::Applied, sample.seconds};
    } catch (...) {
        // Conservative if a platform test double throws after an attempted syscall. No retry.
        return {BrowserClockStatus::SetFailed, 0};
    }
}

BrowserClockResult apply_browser_clock(double milliseconds) noexcept {
    tk::ClockSample sample;
    // Preserve the valid-body/schema NTP no-op even for an out-of-window numeric value.
    if (!tk::browser_clock_sample(milliseconds, clock_plausibility_ceiling(), sample)) {
        // Recheck under the operation gate; an NTP transition must not create an unguarded
        // browser apply. An empty sample is rejected if NTP still has not committed.
        return apply_browser_clock_sample({});
    }
    return apply_browser_clock_sample(sample);
}

static void checkpoint_ntp_time_(tk::ClockSample sample, uint64_t monotonic_us) {
    if (!s_cfg_store || !s_clock_checkpoint.due(monotonic_us)) return;
    // Mark before any allocation/storage: failed attempts cannot cause a tight write loop.
    s_clock_checkpoint.note_attempt(monotonic_us);
    bool saved = false;
    try {
        saved = s_cfg_store->save_str(tk::nvs_contract::kLastTime,
                                      std::to_string(sample.seconds));
    } catch (...) {
        ESP_LOGW(TAG, "NTP checkpoint metadata allocation failed");
    }
    s_clock_checkpoint.note_result(saved, static_cast<uint64_t>(esp_timer_get_time()));
    if (!saved) ESP_LOGW(TAG, "NTP synced but its checkpoint was not confirmed");
}

// Called by the C hook below. Only bounded POD/time operations while holding this gate.
static bool apply_ntp_clock_(tk::ClockSample supplied, int64_t entered_us,
                              tk::ClockSample& applied) {
    ClockOperationGuard guard;
    if (!guard.held || !tk::clock_advance_sample(supplied,
            esp_timer_get_time() - entered_us, clock_plausibility_ceiling(), applied)) return false;
    // 100ms bounds admission only. libc's inner time locks can wait indefinitely.
    timeval value{static_cast<time_t>(applied.seconds),
                  static_cast<suseconds_t>(applied.microseconds)};
    if (settimeofday(&value, nullptr) != 0) return false;
    s_ntp_synced.store(true, std::memory_order_release);
    return true;
}

// Strong implementation of the pinned IDF weak extension seam. The SDK caller
// sntp_set_system_time reaches this hook INSTEAD of its unguarded default clock write.
extern "C" void sntp_sync_time(struct timeval* source) {
    tk::StackWatchScope f135_callback_exit(tk::StackWatch::SntpSync);

    try {
        const int64_t entered_us = esp_timer_get_time();
        if (!source || sntp_get_sync_mode() != SNTP_SYNC_MODE_IMMED ||
            source->tv_usec < 0 || source->tv_usec >= 1000000) {
            sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
            return;
        }
        const tk::ClockSample supplied{static_cast<int64_t>(source->tv_sec),
                                       static_cast<int32_t>(source->tv_usec)};
        tk::ClockSample applied;
        if (!tk::clock_sample_valid(supplied, clock_plausibility_ceiling()) ||
            !apply_ntp_clock_(supplied, entered_us, applied)) {
            sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
            return;
        }
        // Status, strings, storage and logging follow gate release.
        sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
        checkpoint_ntp_time_(applied, static_cast<uint64_t>(esp_timer_get_time()));
        ESP_LOGI(TAG, "NTP time synced");
    } catch (...) {
        // Never unwind through the SDK C caller or reset an already committed authority bit.
        sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
        ESP_LOGE(TAG, "NTP clock boundary failed");
    }
}

// Startup-only checkpoint estimate, before controller/HTTP/SNTP producers. It does not
// certify wall time or repair the inherited Unix-versus-vehicle session-age comparison.
static void restore_clock_from_nvs(NvsStorageAdapter& config_store) {
    if (!tk::clock_should_restore(static_cast<int64_t>(time(nullptr)))) return;
    std::string checkpoint;
    int64_t seconds = 0;
    if (!config_store.load_str(tk::nvs_contract::kLastTime, checkpoint) ||
        !tk::clock_parse_checkpoint(checkpoint, clock_plausibility_ceiling(), seconds)) {
        ESP_LOGW(TAG, "no valid clock checkpoint; waiting for NTP or a browser estimate");
        return;
    }
    timeval value{static_cast<time_t>(seconds), 0};
    if (settimeofday(&value, nullptr) != 0) {
        ESP_LOGW(TAG, "clock checkpoint could not be applied");
        return;
    }
    ESP_LOGI(TAG, "clock checkpoint restored as an estimate; NTP will refine it");
}

// ─── app_main ─────────────────────────────────────────────────────────────────

static const char* reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW(ota/restart)";
        case ESP_RST_PANIC:     return "PANIC(abort/exception)";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

// Heap-attribution probe: logs free + largest contiguous block at each init milestone.
// The LARGEST free block (not total free) is what bounds big allocations (OTA TLS record
// buffers, the tesla-ble session), so it's the number that decides whether the device
// OOM-crashes. Measured budget on this board: WiFi −57 KB, NimBLE −86 KB (dominant),
// HTTP −12 KB, MQTT −20 KB of largest-block — handy when tuning the footprint further.
static void log_heap(const char* where) {
    ESP_LOGW(TAG, "HEAP @%-9s free=%u largest=%u min=%u", where,
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

// OTA rollback health gate. CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE leaves a freshly-flashed
// image in ESP_OTA_IMG_PENDING_VERIFY until the app calls
// esp_ota_mark_app_valid_cancel_rollback(); if the device reboots before that call, the
// bootloader reverts to the previous slot. We defer the call to this task so the new image
// has to prove itself first — catching a "boots fine, then crashes/OOM-reboots under load"
// image, which the old mark-at-startup placement would have already committed.
//
// What counts as proof is decided by logic/health_gate.hpp: CONNECTIVITY, a non-critical largest
// internal heap block, and a minimum uptime — not uptime alone. An image that boots perfectly but
// never gets on the network or immediately exhausts contiguous heap is exactly the image rollback
// must retain. No-op on a normal (non-pending) boot.
//
// Whether being online is even the expected state is the caller's fact, sampled once at arm
// time: a device with no credentials and no wire is legitimately offline. On this firmware
// that state never reaches here (the first-time portal does not return, and the retrying portal
// returns only once the network is up), which is why the flag is computed rather than assumed —
// it is the control flow around it that would change.
static bool s_ota_gate_link_expected = false;

static void ota_health_gate_task(void*) {
    tk::stack_watch_sample(tk::StackWatch::OtaGate); // current task at entry
    tk::StackWatchScope f135_task_exit(tk::StackWatch::OtaGate);

  try {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) != ESP_OK ||
        st != ESP_OTA_IMG_PENDING_VERIFY) {
        tk::stack_watch_sample(tk::StackWatch::OtaGate); // no C++ unwind on delete
        vTaskDelete(nullptr);   // normal boot — nothing pending to confirm
        return;
    }

    ESP_LOGI(TAG, "OTA image pending verify — rollback stays armed until link, BLE host and heap health "
                  "are proven (min %us, giving up after %us)",
             (unsigned) tk::kHealthGateBaseS, (unsigned) tk::kHealthGateCapS);

    // Poll rather than sleep-then-decide: the verdict is a function of elapsed time AND a link
    // that can appear at any moment, so the commit should land shortly after the evidence does.
    constexpr uint32_t kPollS = 5;
    const TickType_t   start  = xTaskGetTickCount();
    for (;;) {
        tk::StackWatchScope f135_iteration_exit(tk::StackWatch::OtaGate);
        vTaskDelay(pdMS_TO_TICKS(kPollS * 1000));
        // The health timer is not proof that boot completed.  A late essential failure can leave
        // HTTP and this task alive while app_main is parked; Safe Mode intentionally omits the
        // vehicle runtime.  Neither state may spend the one automatic OTA rollback.
        switch (tk::runtime_admission_action()) {
            case tk::RuntimeAdmissionAction::Wait:
                continue;
            case tk::RuntimeAdmissionAction::Stop:
                ESP_LOGE(TAG, "OTA image remains pending: essential runtime was not admitted");
                tk::stack_watch_sample(tk::StackWatch::OtaGate); // no C++ unwind on delete
                vTaskDelete(nullptr);
                return;
            case tk::RuntimeAdmissionAction::Run:
                break;
        }
        const uint32_t elapsed_s =
            (uint32_t) (pdTICKS_TO_MS(xTaskGetTickCount() - start) / 1000);
        const bool runtime_healthy = ota_runtime_health().healthy();
        const tk::HealthVerdict v =
            tk::health_gate_decide(elapsed_s, tk::kHealthGateBaseS, tk::kHealthGateCapS,
                                   s_ota_gate_link_expected, tk::net_is_up(), runtime_healthy);
        if (v == tk::HealthVerdict::Wait) continue;

        if (v == tk::HealthVerdict::Commit) {
            // Mark-valid spends the only automatic rollback. Join the same owner word as OTA,
            // identity journals and FaultRestart, then re-check heap after winning the CAS: a
            // watchdog sample racing the verdict either owns FaultRestart first (we retry), or
            // loses to this owner while the heap re-check can still veto the commit.
            OtaHealthCommitGuard commit_guard;
            if (!commit_guard) continue;
            if (!ota_recovery_reference_ready(commit_guard)) continue;
            const auto commit_health = ota_runtime_health();
            if (!commit_health.healthy()) continue;
            const size_t commit_largest = commit_health.largest;

            const esp_err_t mark_result = esp_ota_mark_app_valid_cancel_rollback();
            // Refresh before fallible diagnostics: an SDK error may follow a durable effect.
            ota_refresh_verification_cache();
            if (mark_result == ESP_OK) {
                ESP_LOGI(TAG, "OTA image healthy after %us on %s — marked valid (rollback "
                              "cancelled, largest block %u)",
                         (unsigned) elapsed_s, tk::net_link_str(tk::net_kind()),
                         (unsigned) commit_largest);
            } else {
                ESP_LOGE(TAG, "OTA mark-valid failed: %s (%d); verification is uncertain; "
                              "health task stops without retry",
                         esp_err_to_name(mark_result), (int) mark_result);
            }
        } else {
            // Report the unresolved gate through syslog and /diag. A later restart may roll
            // back; qualifying configuration commits report their own verification result
            // and can confirm the image only when their readiness/host/heap checks pass.
            ESP_LOGE(TAG, "OTA image never proved link, BLE host and heap health within %us — NOT marking "
                          "it valid; the "
                          "next reboot may roll back. A changed MQTT or Syslog save may confirm "
                          "it only with Ready admission, a synced/reset-free BLE host and healthy "
                          "internal heap. WiFi changes remain blocked during probation.",
                     (unsigned) tk::kHealthGateCapS);
        }
        break;
    }
    tk::stack_watch_sample(tk::StackWatch::OtaGate); // no C++ unwind on delete
    vTaskDelete(nullptr);
  } catch (...) {
    // An exception after a mark attempt cannot prove that rollback remains armed.
    ESP_LOGE(TAG, "OTA health-gate task threw; verification is uncertain and task stops");
    tk::stack_watch_sample(tk::StackWatch::OtaGate); // no C++ unwind on delete
    vTaskDelete(nullptr);
  }
}

__attribute__((noinline)) static void load_config_blob_verified(NvsStorageAdapter& config_store, tk::ConfigBlob& cfg_blob) {
    tk::ConfigLoadState cfg_blob_state = tk::ConfigLoadState::Error;
    for (int retry = 0; retry < 3; ++retry) {
        cfg_blob_state = tk::cfg_load_state(config_store, cfg_blob);
        if (cfg_blob_state != tk::ConfigLoadState::Error) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (cfg_blob_state == tk::ConfigLoadState::Error) {
        ESP_LOGE(TAG, "ConfigBlob is unreadable or invalid — refusing boot with stale or missing configuration");
        boot_fatal("configuration verification");
    }
}

extern "C" void app_main() {
    tk::StackWatchScope f135_callback_exit(tk::StackWatch::Main);
    tk::stack_watch_sample(tk::StackWatch::Main); // initial current startup task mark

  // Top-level exception boundary (issue #204): app_main runs C++ that allocates (std::string
  // config, make_unique, the component start()s). An uncaught throw would unwind into the C
  // startup that invoked app_main → std::terminate → abort — the same reboot, but with no
  // diagnostic. Contain it, log it, then roll back a pending image or halt a valid one.
  try {
#if defined(CONFIG_TESLA_CONFIG_REPAIR_USB) && CONFIG_TESLA_CONFIG_REPAIR_USB
    tk::repair::console_entry(); // terminal; no normal startup or writer coexists
#else
    // Capture console output into the in-memory diagnostic ring (GET /diag).
    diag_log_init();
    initialize_clock_gate_();

    // Why we (re)booted, plus the heap baseline to pair it with. SAMPLED HERE, LOGGED LATER:
    // the line itself is emitted after syslog_start() below, because syslog_send() is a no-op
    // until then and this line would otherwise never leave the device. It did not, for the
    // whole of 17.-24.07.2026: 56 boots, zero `BOOT reset_reason=` lines at the collector,
    // which is what made the unattended 20.07. reboot (1h54m of silence, then a boot)
    // impossible to explain afterwards — /diag is RAM and does not survive the restart.
    // The values must still be read HERE, before NVS init, WiFi, the syslog queue and the
    // component start()s have allocated: sampled after all that, "free heap at boot" would
    // describe a boot that already happened rather than the state we came up in.
    const char*    boot_reason      = reset_reason_str(esp_reset_reason());
    const unsigned boot_free        = (unsigned) esp_get_free_heap_size();
    const unsigned boot_min_free    = (unsigned) esp_get_minimum_free_heap_size();
    const unsigned boot_largest     = (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    // Verify bounded crash bytes and build the immutable boot report before service tasks.
    // Live consumers only recheck header/admission; bootstrap fault policy uses the reset POD.
    tk::diag_crash_capture();

    // NimBLE logs every GAP/GATT procedure at INFO — tens of lines per connect.
    // That noise buries the pairing/key-lifecycle messages in /diag (and fills the
    // ring fast). Raise its threshold to WARN so /diag reads as a clean lifecycle log;
    // our own components (vehicle_ctrl, ble_client, …) keep logging at INFO.
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    // Initialize NVS without ever applying ESP-IDF's destructive example fallback. This single
    // partition contains WiFi, VIN, the vehicle private key and BLE sessions: NO_FREE_PAGES or a
    // newer on-flash format is recovery evidence, not permission to erase identity. A pending OTA
    // actively rolls back through boot_fatal(); a valid image parks for explicit NVS recovery.
    const esp_err_t flash_init_err = nvs_flash_init();
    if (flash_init_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash initialization failed (%s); preserving the complete partition",
                 esp_err_to_name(flash_init_err));
        boot_fatal("NVS flash initialization");
    }

    // Static so they outlive app_main() (which deletes itself via vTaskDelete)
    static NvsStorageAdapter config_store(tk::nvs_contract::kConfigNamespace);
    if (!config_store.initialize())
        boot_fatal("configuration NVS");

    // Resolve the cross-namespace VIN journal and its ConfigBlob evidence before starting syslog,
    // provisioning, networking or constructing Vehicle. During a staged /set_vin, a transient
    // cfg probe/read/CRC/schema failure must not fall back to an older legacy VIN and delete the
    // only recovery authority. Only exact blob NOT_FOUND permits the pre-blob legacy path.
    std::string vin_txn;
    const tk::NvsStringLoadState vin_txn_state =
        config_store.load_str_state(tk::nvs_contract::kVinTransition, vin_txn);
    tk::VinTransitionMarker marker;
    const bool vin_txn_valid = vin_txn_state == tk::NvsStringLoadState::Present &&
                               tk::parse_vin_transition_marker(vin_txn, marker);
    const tk::VinTransitionJournalAction vin_txn_action =
        tk::decide_vin_transition_journal(vin_txn_state, vin_txn_valid);
    if (vin_txn_action == tk::VinTransitionJournalAction::Halt) {
        // Keep an unreadable/wrong-type/malformed marker armed. Deleting it would discard evidence
        // that the ConfigBlob and private key may belong to different sides of the transaction.
        ESP_LOGE(TAG, "VIN transition journal is unreadable or malformed — refusing recovery guess");
        boot_fatal("VIN transition journal verification");
    }

    static tk::ConfigBlob cfg_blob;
    if (vin_txn_action == tk::VinTransitionJournalAction::Recover) {
        const tk::ConfigLoadState cfg_blob_state = tk::cfg_load_state(config_store, cfg_blob);
        if (cfg_blob_state == tk::ConfigLoadState::Error) {
            // The journal remains untouched. A retrying boot may read the authoritative blob; this
            // boot must not start a setup portal or classify from legacy mirrors.
            ESP_LOGE(TAG, "VIN transition ConfigBlob is unreadable or invalid — recovery remains armed");
            boot_fatal("VIN transition configuration verification");
        }
        if (cfg_blob_state == tk::ConfigLoadState::Legacy) {
            // Exact NOT_FOUND proves cfg_save never published the staged blob. The journal itself
            // captured the previous VIN before that attempted save, so use it as the recovery
            // identity rather than a best-effort legacy read. A changed key beside this VIN is
            // rejected later by the explicit 2x2 recovery matrix.
            cfg_blob.vin = marker.previous_vin;
        }
    } else {
        load_config_blob_verified(config_store, cfg_blob);
    }
    tk::cfg_init_wifi_rollback_status(cfg_blob.wifi_rolled_back);

    if (cfg_blob.has_ota) {
        ota_set_channel(tk::ota_channel_from_int(cfg_blob.ota_channel));
    } else {
        const esp_app_desc_t* desc = esp_app_get_description();
        ota_set_channel(tk::default_ota_channel_for_version(desc ? desc->version : ""));
    }

    // Did WE end the last boot on purpose? esp_reset_reason() cannot tell a deliberate
    // esp_restart() apart from a user power-cycle — both read SW/POWERON — so the heap watchdog
    // leaves a breadcrumb in NVS on its way out. Take it (read + clear) before anything else can
    // reboot, so it always describes the boot just made, and surface it in /status: a device that
    // self-healed at 04:00 must be able to say so, or the next investigation starts from scratch
    // exactly the way this one did.
    const tk::RebootReasonRecord boot_reboot_record = VehicleController::take_reboot_reason(config_store);
    VehicleController::set_boot_reboot_reason(boot_reboot_record);

    // One durable ledger charges real fault boots and deliberate heap recovery. Heap SW boots
    // preserve their prior reservation; legacy verified counts set a conservative enrollment
    // floor. Admission latches at four, before any normal Vehicle/auto-pair/MQTT task exists.
    const bool safe_mode = tk::safe_mode_begin(config_store,
        tk::boot_counter_cause(tk::diag_crash_reset_info().code, boot_reboot_record.state),
        boot_reboot_record.state, boot_reboot_record.heap_restarts);
    if (safe_mode && !tk::runtime_admission_mark_safe_mode()) {
        boot_fatal("safe-mode runtime admission");
    }

    // Essential before optional Syslog/Ethernet or a non-returning setup portal. The independent
    // task remains alive if vehicle initialization fails or its loop later stops making progress.
    if (!tk::heap_supervisor_start(config_store, boot_reboot_record)) {
        boot_fatal("heap supervisor");
    }

    // UDP Syslog forwarder for the diag log (NVS "syslog_uri" / CONFIG_TESLA_SYSLOG_SERVER;
    // "" = disabled). Started before the network so it captures boot-time log lines too — its
    // own task blocks on tk::net_is_up() until the link is up, so this is safe even though
    // esp_netif_init() itself hasn't run yet (that happens in tk::net_init() below).
    // OPTIONAL subsystem: a false return (resources/task couldn't be allocated) leaves
    // forwarding disabled and degraded, but never stops boot — syslog_start() has already
    // logged the specific reason.
    if (!syslog_start(config_store))
        ESP_LOGW(TAG, "Syslog forwarding is degraded/disabled (see the error above)");

    // FIRST line past the forwarder, because it is the one a post-mortem starts from: syslog
    // is the only record that outlives a restart. PANIC = abort()/uncaught C++ exception,
    // BROWNOUT = power dip, *_WDT = a stuck task; the heap figures next to it are what
    // distinguish an OOM-driven abort from a clean restart. Values sampled at entry (above).
    ESP_LOGW(TAG, "BOOT reset_reason=%s free_heap=%u min_free=%u largest_block=%u",
             boot_reason, boot_free, boot_min_free, boot_largest);

    // Announce the breadcrumb only NOW, deliberately after syslog_start(): syslog_send() is a
    // no-op until then (diag_log.cpp's capture hook has nowhere to forward to), so logging it
    // above — where the value is read — would confine the one line explaining an unattended
    // self-heal to the /diag RAM ring, which the next restart erases. Queued lines survive until
    // WiFi is up, so this does reach the collector. The read itself stays above, before anything
    // else can reboot, so the value always describes the boot just made.
    if (boot_reboot_record.state == tk::RebootReasonState::Present) {
        ESP_LOGW(TAG, "BOOT this boot was caused by the firmware itself: reason=%s — the previous "
                      "run restarted deliberately because its heap stayed unusable (also in "
                      "/status as last_reboot; see docs/ARCHITECTURE.md)",
                 boot_reboot_record.text);
    } else if (boot_reboot_record.state == tk::RebootReasonState::Error) {
        ESP_LOGW(TAG, "BOOT reboot reason unavailable or invalid: reason=%s; "
                      "previous reset cause is unverified",
                 boot_reboot_record.text);
    }

    // Resolve WiFi credentials from the configuration snapshot verified above. With an armed VIN
    // journal this point is unreachable on blob probe/read/CRC/schema failure, so setup mode can
    // never consume a stale legacy fallback before identity recovery.
    static std::string ssid     = cfg_blob.wifi_ssid;
    static std::string password = cfg_blob.wifi_pass;

    // Is there a wire? Probed HERE, before the setup-portal decision and before anything else
    // has touched a GPIO — it tries the bounded, hardware-verified candidate table by reading the
    // W5500 identity register, without waiting for a link or lease. On a board with no controller
    // (or on the T-Dongle-S3, whose panel owns the SPI clock pin) it frees the bus again and
    // reports false.
    const bool have_wire = tk::net_eth_probe();

    // The setup portal exists because a device with no credentials has no other way to be
    // reached. A WIRED device does: DHCP gives it an address with nothing configured at all, so
    // sending it to a captive AP would strand a perfectly reachable board — a regression created
    // purely by adding a transport. The VIN is then set over the LAN like any other setting.
    const bool setup_portal_required = ssid.empty() && !have_wire;
    if (setup_portal_required &&
        vin_txn_action != tk::VinTransitionJournalAction::Recover) {
        ESP_LOGW(TAG, "No WiFi configured — starting setup portal (join WiFi '%s')",
                 "tesla-key-esp32-setup");
        provisioning_run(config_store);  // never returns; reboots on save
    }
    if (ssid.empty() && have_wire)
        ESP_LOGI(TAG, "no WiFi configured, but an Ethernet controller is present — coming up "
                      "on the wire (set the VIN in the web UI at the DHCP address)");
    else if (setup_portal_required)
        ESP_LOGW(TAG, "setup portal deferred until the armed VIN transition is recovered");

    // Resolve VIN
    static std::string vin = cfg_blob.vin;
    if (vin.empty()) {
        ESP_LOGW(TAG, "VIN not configured — pairing disabled until a VIN is set "
                      "(setup AP or POST /set_vin / CONFIG_TESLA_VIN / NVS key 'vin'). "
                      "Nearby Teslas are still listed by /scan, but none is connected/enrolled.");
        vin = "UNKNOWN";  // placeholder for display/logging only — kept out of the BLE matching path
    }

    // Resolve BLE MAC (persisted after first successful scan)
    static std::string ble_mac = CONFIG_TESLA_BLE_MAC;
    config_store.load_str(tk::nvs_contract::kBleMac, ble_mac);

    // Physical board identity is diagnostic evidence, not the HA identity (which follows VIN).
    uint8_t board_mac[6] = {0};
    char board_mac_text[18] = "unavailable";
    if (esp_read_mac(board_mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        std::snprintf(board_mac_text, sizeof(board_mac_text), "%02x:%02x:%02x:%02x:%02x:%02x",
                      (unsigned)board_mac[0], (unsigned)board_mac[1], (unsigned)board_mac[2],
                      (unsigned)board_mac[3], (unsigned)board_mac[4], (unsigned)board_mac[5]);
    }
    ESP_LOGI(TAG, "VIN: %s  BLE MAC: %s  Board MAC: %s", vin.c_str(),
             ble_mac.empty() ? "(scan)" : ble_mac.c_str(), board_mac_text);

    log_heap("preinit");

    // Restore a strictly validated estimate only if the warm clock is unset, before loading
    // sessions. NTP alone permits durable new dates. Session clock epochs remain an independent
    // limitation documented in ADR-0005; this checkpoint is not proof of session freshness.
    restore_clock_from_nvs(config_store);

    // ── Tesla BLE controller ─────────────────────────────────────────────────
    // Construct the controller (NVS + key) here; NimBLE itself (ble_client.start)
    // is started after WiFi is up. The controller's accessors are safe to call
    // before that — they report "not connected" until the link comes up.
    static BleClient ble_client;
    static VehicleController vehicle;
    tk::VehicleTaskStartPhase vehicle_task_phase =
        tk::VehicleTaskStartPhase::ControllerWired;

    static NvsStorageAdapter tesla_store(tk::nvs_contract::kTeslaBleNamespace);
    const bool tesla_store_ok = tesla_store.initialize();
    if (!tesla_store_ok && !safe_mode)
        boot_fatal("Tesla NVS");
    if (safe_mode && !tesla_store_ok)
        ESP_LOGE(TAG, "safe mode: Tesla NVS unavailable — identity fields will read empty");

    if (safe_mode) {
        ESP_LOGW(TAG, "Safe mode active — initializing inert vehicle controller");
        if (!vehicle.init_safe_mode(vin, tesla_store, config_store)) {
            boot_fatal("VehicleController safe mode");
        }
        vehicle_task_phase = tk::VehicleTaskStartPhase::IdentityResolved;
    } else {

        // TeslaBLE loads an existing private key while constructing its crypto context, and the
        // first-boot branch below may generate one. ESP-IDF 6 routes every PSA random draw (key
        // generation, key-parse blinding) straight to the hardware RNG (MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG),
        // so each draw is only as good as the entropy source at that moment; enabling it only in the
        // no-key branch would leave every already-provisioned device's key load uncovered. Keep
        // SAR-ADC entropy active across BOTH controller construction/key load and a possible
        // first-boot key generation; WiFi/BLE are not running yet and therefore cannot supply RF
        // entropy themselves.
        bootloader_random_enable();
        // init() wires the connected + rx callbacks onto ble_client and passes the
        // config_store so it can save the discovered MAC. ESSENTIAL: without the controller
        // there is no BLE proxy at all, so a failed init halts boot (and leaves any pending OTA
        // image unconfirmed → rolled back).
        // The controller is fully WIRED here, but its mutating tasks are deliberately deferred until
        // VIN/key recovery and every ESSENTIAL initializer have succeeded. boot_fatal parks app_main;
        // starting auto_pair here would therefore let it rotate keys behind a recovery halt.
        if (!vehicle.init(vin, ble_client, tesla_store, config_store, ble_mac,
                          /*start_tasks=*/false)) {
            bootloader_random_disable();
            boot_fatal("VehicleController");
        }

    // Recover a /set_vin transaction interrupted between the tesla_cfg ConfigBlob commit and the
    // tesla_ble private-key/session commits. The marker stores "previous VIN|previous key id".
    // Comparing it with the key that the controller actually loaded gives a strict 2x2 matrix:
    // old key + old VIN -> clear the pre-stage marker; old key + new VIN -> roll VIN back;
    // new key + new VIN -> complete cleanup; new key + old VIN -> contradictory, halt.
    // Reboot after either repair so the in-memory Vehicle cannot retain the VIN or peer sessions
    // that were loaded before recovery.
    if (vin_txn_action == tk::VinTransitionJournalAction::Recover) {
        bool durable_key_present = false;
        const bool key_probe_ok =
            tesla_store.probe_blob(tk::nvs_contract::kPrivateKey, durable_key_present);
        const std::string current_key = vehicle.key_fingerprint();
        if (!tk::vin_transition_key_evidence_verified(marker.previous_key_id, current_key,
                                                       key_probe_ok, durable_key_present)) {
            ESP_LOGE(TAG, "cannot verify durable key identity while recovering VIN transition");
            bootloader_random_disable();
            boot_fatal("VIN transition key verification");
        }
        const tk::VinTransitionRecovery recovery =
            tk::decide_vin_transition_recovery(marker, cfg_blob.vin, current_key);
        if (recovery == tk::VinTransitionRecovery::HaltInconsistent) {
            // The staged write order is journal -> ConfigBlob -> key. A changed key beside the
            // previous VIN is therefore not a completed transaction; it is contradictory durable
            // evidence. Preserve vin_txn and halt rather than blessing the tuple by cleanup.
            ESP_LOGE(TAG, "VIN transition key changed but configured VIN did not — recovery evidence is inconsistent");
            bootloader_random_disable();
            boot_fatal("VIN transition consistency verification");
        }
        // The early heap supervisor can now run while bootstrap is blocked. Serialize the
        // complete repair mutation/restart window against it without granting OTA confirmation.
        // Return errors out of the lease scope before boot_fatal parks a Stable image.
        const char* recovery_failure = [&]() -> const char* {
            OtaConfigRestartGuard owner;
            if (!owner.try_begin()) return "VIN transition recovery ownership";
            bool must_restart = false;
            if (recovery == tk::VinTransitionRecovery::RollBackPreviousVin) {
                ESP_LOGW(TAG, "interrupted VIN change detected before key commit — restoring previous VIN");
                cfg_blob.vin = marker.previous_vin;
                if (!tk::cfg_save(config_store, cfg_blob)) return "VIN transition rollback";
                if (!config_store.remove(tk::nvs_contract::kVinTransition))
                    return "VIN transition marker cleanup";
                must_restart = true;
            } else if (recovery == tk::VinTransitionRecovery::ClearMarker) {
                // Only the verified old-key + old-VIN quadrant may retire the journal without a
                // repair reboot. The verified 2x2 classification above remains unchanged.
                if (!config_store.remove(tk::nvs_contract::kVinTransition))
                    return "VIN transition marker cleanup";
            } else if (recovery == tk::VinTransitionRecovery::CompleteNewIdentity) {
                ESP_LOGW(TAG, "interrupted VIN change detected after key commit — completing cleanup");
                const bool vcsec_removed = tesla_store.remove(tk::nvs_contract::kSessionVcsec);
                const bool info_removed = tesla_store.remove(tk::nvs_contract::kSessionInfotainment);
                const bool paired_removed = tesla_store.remove(tk::nvs_contract::kPairedAt);
                const bool mac_removed = config_store.remove(tk::nvs_contract::kBleMac);
                if (!tk::vin_transition_cleanup_ready_for_marker_removal(
                        vcsec_removed, info_removed, paired_removed, mac_removed))
                    return "VIN transition completion";
                if (!config_store.remove(tk::nvs_contract::kVinTransition))
                    return "VIN transition marker cleanup";
                must_restart = true;
            }
            if (must_restart) {
                static_assert(!tk::ota_reboot_confirms_pending_image(
                    tk::OtaRebootClass::AutomaticIdentityRecovery));
                owner.seal();
                bootloader_random_disable();
                vTaskDelay(pdMS_TO_TICKS(100));
                tk::stack_watch_sample(tk::StackWatch::Main); // before terminal effect
                esp_restart();
                // An unexpected return is not authority to release the owner or continue startup.
                for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
            }
            return nullptr;
        }();
        if (recovery_failure) {
            bootloader_random_disable();
            boot_fatal(recovery_failure);
        }
    }

    // An armed transition suppresses provisioning until its blob/key tuple is classified. The only
    // path reaching this call is verified ClearMarker; rollback/new-identity paths reboot and all
    // ambiguous paths halt with the marker intact.
    if (setup_portal_required &&
        vin_txn_action == tk::VinTransitionJournalAction::Recover) {
        bootloader_random_disable();
        ESP_LOGW(TAG, "VIN transition recovered — starting setup portal (join WiFi '%s')",
                 "tesla-key-esp32-setup");
        provisioning_run(config_store);  // never returns; reboots on save
    }

    // Create the ECDSA key on first boot so a key always exists (and a fingerprint is shown).
    // A recovered key_rotate marker is a one-reboot classification boundary: if no durable key
    // was loaded after that recovery, halt instead of immediately repeating the same ambiguous
    // write/reboot forever. An external reset may make a fresh, explicitly observable attempt.
    const tk::InitialKeyBootAction initial_key_action = tk::decide_initial_key_boot_action(
        vehicle.has_key(), vehicle.key_rotation_recovered_at_boot());
    if (initial_key_action == tk::InitialKeyBootAction::HaltAfterEmptyRecovery) {
        bootloader_random_disable();
        boot_fatal("initial key recovery found no durable key");
    }
    if (initial_key_action == tk::InitialKeyBootAction::Generate) {
        OtaIdentityMutationGuard identity_guard(tk::IdentityMutationEntry::InitialKey);
        if (!identity_guard) {
            // A rollback reboot would hand any newly-written key/journal to the previous image.
            // Leave first-boot identity absent during probation; the gated auto-pair supervisor
            // may generate it only after the health task has marked this image stable. The same
            // guard also excludes a concurrently-starting OTA worker if startup ordering changes.
            ESP_LOGW(TAG, "initial key generation deferred during OTA verification/update");
        } else {
            ESP_LOGI(TAG, "no key in storage — generating initial key");
            const VehicleController::KeyGenerationResult generated =
                vehicle.generate_key_result(/*allow_replace=*/false);
            if (generated.key_probe_failed || generated.transition_blocked ||
                generated.existing_key_refused) {
                bootloader_random_disable();
                boot_fatal("initial key generation preflight");
            }
            if (generated.rotation == tk::KeyRotationResult::Complete) {
                ESP_LOGI(TAG, "initial key generated, fingerprint %s",
                         vehicle.key_fingerprint().c_str());
            } else if (generated.rotation == tk::KeyRotationResult::CommitUnknown) {
                // key_rotate remains durable. The next boot clears obsolete sessions before
                // Vehicle construction, then either loads the committed key or takes the guarded
                // halt above. The identity gate remains held through this recovery reboot.
                ESP_LOGE(TAG, "initial key commit outcome is ambiguous — rebooting once to classify storage");
                bootloader_random_disable();
                // Do NOT confirm a pending OTA here. This is automatic fault recovery before the
                // 90-second link-health gate, not a user-approved config reboot; rollback must
                // remain armed if the new image cannot establish a healthy runtime.
                static_assert(!tk::ota_reboot_confirms_pending_image(
                    tk::OtaRebootClass::AutomaticIdentityRecovery));
                vTaskDelay(pdMS_TO_TICKS(300));
                tk::stack_watch_sample(tk::StackWatch::Main); // before terminal effect
                esp_restart();
            } else {
                ESP_LOGE(TAG, "initial key generation incomplete (state=%d) — runtime remains fail-closed",
                         static_cast<int>(generated.rotation));
            }
        }
    } else {
        ESP_LOGI(TAG, "key present, fingerprint %s", vehicle.key_fingerprint().c_str());
    }
    vehicle_task_phase = tk::VehicleTaskStartPhase::IdentityResolved;
    bootloader_random_disable();
    }  // !safe_mode
    // Match by the VIN-derived BLE name on scan. Pass the real VIN only when it is a plausible
    // 17-char VIN; with none configured we pass an EMPTY target so the scanner lists nearby
    // Teslas but never connects/enrols on one. The "UNKNOWN" placeholder must stay out of the
    // matching path — it would hash to a name that just happens never to collide, making the
    // safe outcome accidental rather than designed. Pairing is gated on a real VIN.
    ble_client.set_target_vin(vehicle.has_plausible_vin() ? vin : std::string{});

    // ── network: the wire first, the radio second ────────────────────────────
    // Ethernet is preferred whenever it can actually carry the lease, and the reason is not
    // bandwidth — it is the radio. WiFi and BLE share ONE antenna path on every chip this
    // firmware targets, so a running WiFi stack means time-division coexistence with NimBLE for
    // as long as the device is powered; that is what forces WIFI_PS_MIN_MODEM and what makes
    // every GATT round-trip to the car slower than it needs to be. Coming up on a wire does not
    // merely avoid using WiFi, it avoids STARTING it: no coexistence arbitration at all, and the
    // ~57 KB of largest-block the stack holds stays free on a device whose binding limit is the
    // largest CONTIGUOUS block.
    //
    // A controller with no cable falls through to WiFi rather than stranding the board, and its
    // driver keeps running, so a cable plugged in later still takes over.
    const bool on_wire = have_wire && tk::net_start_eth();
    if (on_wire) log_heap("eth");

    if (!on_wire && ssid.empty()) {
        // The wire was present at probe time but never got a lease, and there are no credentials
        // to fall back to. Rebooting would just repeat this; the portal at least makes the device
        // configurable by someone standing next to it.
        ESP_LOGE(TAG, "Ethernet present but not usable and no WiFi configured — setup portal");
        provisioning_run(config_store);  // never returns; reboots on save
    }

    // Connect to WiFi. With stored credentials, a failure is usually a transient
    // outage (e.g. router rebooting), but if it persists (e.g. wrong password),
    // fallback to the setup portal so the user can reconfigure it.
    if (!on_wire &&
        !tk::net_start_wifi(ssid.c_str(), password.c_str(), cfg_blob.wifi_rollback_active)) {
        bool rollback_unpersisted = false;
        if (cfg_blob.wifi_rollback_active) {
            // The credentials from the last /set_wifi did not work and the grace window is spent.
            // Restore the pair that DID work and reboot onto it, rather than dropping into the
            // setup portal — which would require someone to be standing next to the device, the
            // exact situation being able to change WiFi over the LAN exists to avoid.
            ESP_LOGE(TAG, "WiFi credential change failed — restoring the previous network and "
                          "rebooting (reported on /status as wifi.rolled_back)");
            {
                OtaConfigRestartGuard owner;
                if (owner.try_begin()) {
                    cfg_blob.wifi_ssid = cfg_blob.wifi_ssid_backup;
                    cfg_blob.wifi_pass = cfg_blob.wifi_pass_backup;
                    cfg_blob.wifi_ssid_backup.clear();
                    cfg_blob.wifi_pass_backup.clear();
                    cfg_blob.wifi_rollback_active = false;
                    cfg_blob.wifi_rolled_back = true;
                    if (tk::cfg_save(config_store, cfg_blob)) {
                        // Automatic rollback never confirms a candidate. Retain the actual owner
                        // through restart so heap recovery cannot cross this committed repair.
                        static_assert(!tk::ota_reboot_confirms_pending_image(
                            tk::OtaRebootClass::AutomaticWifiRollback));
                        owner.seal();
                        vTaskDelay(pdMS_TO_TICKS(300));
                        tk::stack_watch_sample(tk::StackWatch::Main); // before terminal effect
                        esp_restart();
                        for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
                    }
                }
            }
            // The restore lives in NVS alone, so an unpersisted one would be re-decided identically
            // on every boot — a reboot loop we cannot write our way out of. Fall through to the
            // portal instead, which at least leaves the device configurable.
            ESP_LOGE(TAG, "could not persist the credential rollback — falling back to the setup "
                          "portal rather than rebooting into a loop");
            rollback_unpersisted = true;
        }
        // LAST look before surrendering the running firmware to the portal. The Ethernet
        // driver keeps polling through the whole WiFi boot window, so a cable plugged in
        // during those ~30 s can have taken the link while this branch was being reached —
        // and dropping a working, reachable device into a captive AP would be the worst
        // possible answer to "the network is up".
        if (tk::net_is_up()) {
            ESP_LOGW(TAG, "WiFi did not come up, but the link is carried by %s — staying up",
                     tk::net_link_str(tk::net_kind()));
        } else if (rollback_unpersisted) {
            // The station still holds the refused trial pair and the restored one exists only in
            // RAM, so a retry would repeat the refusal: the terminal portal, as before.
            provisioning_run(config_store);  // never returns; reboots on save
        } else {
            // Stored credentials stay in use: the portal retries them in the background and the
            // boot continues here once the network is up (a router that boots slower than this
            // board after a power cut). Saving the form still reboots. No timed reboot: it would
            // revert a pending OTA image, defeat the crash counter and re-seed the vehicle window.
            ESP_LOGW(TAG, "WiFi connection failed — setup portal with background retry");
            provisioning_run_with_retry(config_store);
        }
    }
    // Associated on the new credentials: the trial is over and the backup has done its job. Drop it
    // so a LATER, unrelated outage can never restore credentials from months ago.
    //
    // NOT on the wired path: coming up on Ethernet proves nothing about the credentials on
    // trial, so consuming the backup there would silently discard the only way back to a working
    // network the moment the cable is unplugged. Every failure path above disarms the trial in RAM
    // first, so the portal and the last look never reach this with a trial armed.
    if (!on_wire && cfg_blob.wifi_rollback_active) {
        cfg_blob.wifi_ssid_backup.clear();
        cfg_blob.wifi_pass_backup.clear();
        cfg_blob.wifi_rollback_active = false;
        if (!tk::cfg_save(config_store, cfg_blob))
            ESP_LOGW(TAG, "WiFi credentials are good but the one-shot backup was not cleared");
    }
    if (!on_wire) log_heap("wifi");

    // mDNS: advertise http://tesla-key-esp32.local so users need not find the IP
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(tk::kDeviceHostname);
        mdns_instance_name_set(tk::kDeviceHostname);
        // Advertise only the firmware version. The full VIN is private identity data and must not
        // become multicast-browseable merely because a client enumerates HTTP services; callers
        // that are already trusted to query this open LAN API can obtain it from /status.
        // mdns copies this internally, so the pointer need only outlive the call.
        mdns_txt_item_t txt[] = {
            { "ver", esp_app_get_description()->version },
        };
        mdns_service_add(nullptr, "_http", "_tcp", 80, txt, 1);
        ESP_LOGI(TAG, "mDNS: http://%s.local", tk::kDeviceHostname);
    } else {
        ESP_LOGW(TAG, "mDNS init failed");
    }

    // SNTP takes over the wall clock from here (the NVS restore already ran before
    // VehicleController::init, see restore_clock_from_nvs above). On sync it refreshes the NVS
    // cache and supersedes any restored or browser-supplied (POST /set_time) value.
    s_cfg_store = &config_store;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_init();

    // Start NimBLE host. Discovery scanning is manual/time-limited; the client
    // connects on demand when a command is issued. (The controller was set up
    // before WiFi, above.) ESSENTIAL.
    // Not in safe mode: NimBLE is the largest, most allocation-heavy subsystem here and the one
    // every vehicle code path runs through, so a crash loop that safe mode is meant to break out of
    // is far more likely to live behind it than in the web server. Leaving the host down also frees
    // the heap it would hold, which is what makes the recovery surface (UI + OTA) comfortable.
    if (!safe_mode) {
        if (!ble_client.start())
            boot_fatal("NimBLE");
        vehicle_task_phase = tk::VehicleTaskStartPhase::BleReady;
        log_heap("ble");
    } else {
        ESP_LOGW(TAG, "SAFE MODE — NimBLE not started");
    }

    // Seed diagnostic verification before HTTP/health/vehicle tasks can observe or change it.
    ota_refresh_verification_cache();

    // Primary HTTP API (evcc + web UI + MCP). ESSENTIAL: it is the device's whole reason to
    // exist. http_server_start() unwinds a partial registration internally and returns false;
    // halt boot on that so we never announce a server that answers some routes and 404s others.
    if (!http_server_start(vehicle, config_store))
        boot_fatal("HTTP server");
    log_heap("http");

    // Home Assistant MQTT bridge: publishes all telemetry + device status (read-only)
    // if a broker is configured (NVS "mqtt_uri" / CONFIG_TESLA_MQTT_BROKER_URI); a
    // no-op otherwise. Runs in its own task, independent of evcc/BLE/pairing. OPTIONAL:
    // a failed start degrades to disabled (logged) without stopping boot.
    // Not in safe mode: with the vehicle loop down there is nothing fresh to publish, and the
    // bridge's TLS path is a second large allocator competing with the OTA that is the whole point
    // of staying reachable. HA sees the LWT go offline, which is the honest signal.
    if (!safe_mode) {
        if (!mqtt_ha_start(vehicle, config_store))
            ESP_LOGW(TAG, "MQTT bridge is degraded/disabled (see the error above)");
        log_heap("mqtt");
    } else {
        ESP_LOGW(TAG, "SAFE MODE — MQTT bridge not started");
    }

    // On-device status display (LilyGo T-Dongle-S3). No-op unless the board
    // build selects CONFIG_TESLA_DISPLAY_ENABLED — and on esp32s3 also a no-op unless the
    // T-Dongle-S3 is auto-detected (a generic ESP32-S3 has no panel). Reads only cached
    // state (never wakes the car) in its own task, so it can't queue behind a BLE poll.
    display_start(vehicle);
    log_heap("display");

    // On-device status LED (LilyGo T-Dongle underside APA102). No-op unless the board build
    // selects CONFIG_TESLA_LED_ENABLED; reads only cached state via the same UiSnapshot the
    // display uses (never wakes the car), independent of the display / MQTT.
    led_status_start(vehicle);
    log_heap("led");

    // LAN connectivity watchdog: forces the active transport to re-establish if the link
    // silently dies, including the "ghost association" case that fires no disconnect event
    // (net.cpp; the decision logic is logic/net_link.hpp). Without it the device can sit
    // reachable-over-BLE but off the LAN indefinitely, recoverable only by a manual reset.
    // ESSENTIAL: without the watchdog a silent LAN drop can strand the device off the network
    // with no automatic recovery — so a failure to create it halts boot rather than run
    // without the safety net.
    if (!tk::net_watchdog_start())
        boot_fatal("LAN watchdog");

    // Confirm a freshly OTA-flashed image only after it has proven it can RUN — not merely
    // reach this line. Marking it valid here (the old behaviour) would disarm rollback the
    // instant the tasks start, so an image that boots but then crashes/OOM-reboots only under
    // load would already have cancelled its safety net. The health-gate task instead holds
    // rollback armed for a window of healthy uptime; if the image dies first it reboots while
    // still PENDING_VERIFY and the bootloader reverts to the previous slot. A no-op on a normal
    // (non-pending-verify) boot.
    // ESSENTIAL for the safety of a freshly-flashed image: this is the task that eventually
    // marks a PENDING_VERIFY OTA image valid after a healthy window. If it cannot even be
    // created, the image would sit unconfirmed forever; boot_fatal() explicitly rolls a pending
    // image back to the last-good slot and halts an already-valid image.
    //
    // Is being online the expected state? Only if this device has a route to lose. Sampled here,
    // where both facts are in scope, rather than re-derived inside the task from state that other
    // code is free to change underneath it.
    s_ota_gate_link_expected = !ssid.empty() || on_wire;
    if (xTaskCreate(ota_health_gate_task, "ota_gate", 3072, nullptr,
                    tk::kPrioOtaGate, nullptr) != pdPASS)
        boot_fatal("OTA health gate");

    vehicle_task_phase = tk::VehicleTaskStartPhase::EssentialServicesReady;
    if (!safe_mode) {
        if (!tk::vehicle_tasks_may_start(vehicle_task_phase, safe_mode) ||
            !vehicle.start_tasks()) {
            // start_tasks() keeps both entries behind a start barrier and cooperatively cancels a
            // partial create before returning false, so boot_fatal cannot leave a mutating task.
            boot_fatal("Vehicle background tasks");
        }
        // A sync acknowledgement proved the hidden NimBLE task was created. It must also remain
        // stable through essential-service admission: a reset/re-sync during boot is not authority
        // to start vehicle tasks or to spend rollback on this image.
        if (!ble_host_synced() || ble_host_reset_count() != 0) {
            boot_fatal("NimBLE host stability");
        }
        // Last essential transition.  Both task entries continue waiting until this succeeds, so
        // there is no scheduling window in which a merely allocated task can contact the car.
        if (!tk::runtime_admission_mark_ready()) {
            boot_fatal("vehicle runtime admission");
        }
    }

    // The early heap supervisor qualifies crash-counter maintenance from a continuous healthy
    // normal runtime. Raw uptime and the separate OTA health timer grant no crash-counter clear.

    if (safe_mode) {
        ESP_LOGW(TAG, "tesla-key-esp32 running in SAFE MODE on port 80 — web UI, /status and OTA "
                      "are up; BLE, pairing, commands and MQTT are DOWN. Fix the configuration or "
                      "install a newer build, then reboot.");
    } else {
        ESP_LOGI(TAG, "tesla-key-esp32 running. API on port 80.");
    }
    // Main task is no longer needed; Vehicle loop + HTTP server run in their own tasks.
    tk::stack_watch_sample(tk::StackWatch::Main); // before terminal effect
    vTaskDelete(nullptr);
#endif // CONFIG_TESLA_CONFIG_REPAIR_USB
  } catch (const std::exception& e) {
      ESP_LOGE(TAG, "app_main initialization threw (%s)", e.what());
      boot_fatal("app_main exception boundary");
  } catch (...) {
      ESP_LOGE(TAG, "app_main initialization threw (unknown)");
      boot_fatal("app_main exception boundary");
  }
}
