// Actual supervisor/facade/operation owners + real BootGuard/trend shell, with passive platform
// and NVS method doubles. No board, vehicle, network, signing key or actual persistent mutation.
#include "stack_watch.hpp"
#include "heap_supervisor.hpp"
#include "heap_trend.hpp"
#include "config_blob.hpp"
#include "logic/heap_supervision.hpp"
#include "logic/ota_lifetime.hpp"
#include "logic/uptime.hpp"
#include "logic/vin_transition.hpp"
#include "ota_update.hpp"
#include "runtime_admission.hpp"
#include "rtos_guard.hpp"
#include "safe_mode.hpp"
#include "task_config.hpp"
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << "heap supervisor " << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
using Owner = tk::OtaIdentityGateState;
using Cause = tk::BootCounterCause;
tk::OtaIdentityOperationGate s_operation_gate;
void finish_operation(Owner owner) { CHECK(s_operation_gate.finish(owner)); }
uint64_t clock_ms = 0;
size_t largest = 65536, free_bytes = 100000;
bool synced = true, network_up = true, ota_busy = false;
unsigned ota_abort_requests = 0;
uint32_t host_resets = 0;
bool partition_ok = true, query_ok = true, query_throws = false;
enum esp_ota_img_states_t { ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID,
    ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED };
esp_ota_img_states_t image_state = ESP_OTA_IMG_VALID;
struct esp_partition_t {};
esp_partition_t partition;
int queries = 0, restart_calls = 0, log_calls = 0, host_reads = 0;
int invalidations = 0, config_saves = 0;
bool config_save_ok = true, config_save_throws = false;
Owner expected_restart_owner = Owner::FaultRestart;
bool throw_log = false, restart_throws = false;
uint64_t stop_supervisor_ms = UINT64_MAX;
std::vector<std::string> logs;
constexpr unsigned MALLOC_CAP_8BIT = 1, MALLOC_CAP_INTERNAL = 2;
unsigned timer_reads = 0, change_on_timer = 0, timer_fault = 0;
unsigned f135_hwm_queries = 0;
unsigned heap_reads = 0, change_on_heap = 0;
bool recover_heap_on_read = false;
int64_t esp_timer_get_time() {
    if (++timer_reads == change_on_timer) {
        switch (timer_fault) {
            case 0: tk::runtime_admission_mark_fatal(); break;
            case 1: synced = false; break;
            case 2: host_resets = 1; break;
            case 3: clock_ms += 60001; break;
            case 4: network_up = false; break;
            case 5: ota_busy = true; break;
            default: largest = 4095; break;
        }
    }
    return static_cast<int64_t>(clock_ms * 1000);
}
size_t heap_caps_get_largest_free_block(unsigned caps) {
    CHECK(caps == MALLOC_CAP_8BIT || caps == (MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    // A large external-capable heap must never mask the critical INTERNAL sample.
    if (caps == MALLOC_CAP_8BIT) return 1000000;
    if (++heap_reads == change_on_heap) {
        if (recover_heap_on_read) largest = 4096;
        else ota_busy = true;
    }
    CHECK(caps == (MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)); return largest;
}
size_t heap_caps_get_free_size(unsigned caps) {
    CHECK(caps == MALLOC_CAP_8BIT || caps == (MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    return caps == MALLOC_CAP_8BIT ? 2000000 : free_bytes;
}
size_t heap_caps_get_minimum_free_size(unsigned caps) {
    CHECK(caps == MALLOC_CAP_8BIT); return 500000;
}
const esp_partition_t* esp_ota_get_running_partition() { return partition_ok ? &partition : nullptr; }
int esp_ota_get_state_partition(const esp_partition_t* p, esp_ota_img_states_t* state) {
    CHECK(p == &partition); ++queries;
    if (query_throws) throw std::bad_alloc();
    *state = image_state; return query_ok ? ESP_OK : ESP_FAIL;
}
void esp_restart() {
    CHECK(s_operation_gate.state() == expected_restart_owner);
    ++restart_calls;
    if (restart_throws) throw std::bad_alloc();
}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot() {
    CHECK(s_operation_gate.state() == Owner::FaultRestart);
    ++invalidations; return ESP_FAIL; // uncertain SDK return must retain this owner
}
void bootloader_random_disable() {}
const char* TAG = "main_fixture";
void host_log(const char*, const char* format, ...) {
    ++log_calls;
    if (throw_log) throw std::bad_alloc();
    char text[256]; va_list args; va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args); va_end(args);
    logs.emplace_back(text);
}
extern "C" const char* esp_err_to_name(esp_err_t) { return "fixture-error"; }
}

bool ble_host_synced() noexcept { ++host_reads; return synced; }
uint32_t ble_host_reset_count() noexcept { ++host_reads; return host_resets; }
bool ota_is_busy() { return ota_busy; }
void ota_request_heap_abort() noexcept { ++ota_abort_requests; }
namespace tk { bool net_is_up() { return network_up; } }

// The generated production TU's logs execute a fallible host sink, rather than the quiet SDK
// macros used by unrelated native tests. All source decisions/functions remain unchanged.
#define ESP_LOGE(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_log(__VA_ARGS__)
#define ESP_LOGI(...) host_log(__VA_ARGS__)

namespace {
TaskFunction_t created_task = nullptr;
void* task_arg = nullptr;
uint32_t task_stack = 0;
UBaseType_t task_priority = 0;
bool task_ok = true, mutex_ok = true, mutex_busy = false;
bool release_owner_on_delay = false;
int task_creates = 0, zero_waits = 0, delay_calls = 0;
struct StopPark {};
}
struct HostSemaphore { bool held = false; };
HostSemaphore trend_mutex;
extern "C" SemaphoreHandle_t xSemaphoreCreateMutex() { return mutex_ok ? &trend_mutex : nullptr; }
extern "C" BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks) {
    CHECK(sem == &trend_mutex);
    if (ticks == 0) ++zero_waits;
    if (mutex_busy || sem->held) return pdFALSE;
    sem->held = true; return pdTRUE;
}
extern "C" BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    CHECK(sem == &trend_mutex && sem->held); sem->held = false; return pdTRUE;
}
extern "C" BaseType_t xTaskCreate(TaskFunction_t task, const char* name, uint32_t stack,
                                  void* arg, UBaseType_t priority, TaskHandle_t*) {
    ++task_creates; CHECK(std::strcmp(name, "heapsup") == 0);
    created_task = task; task_arg = arg; task_stack = stack; task_priority = priority;
    return task_ok ? pdPASS : pdFAIL;
}
extern "C" void vTaskDelay(TickType_t ticks) {
    ++delay_calls;
    if (ticks == 10000) throw StopPark{};
    if (ticks == 1000 && release_owner_on_delay) {
        CHECK(s_operation_gate.finish(s_operation_gate.state()));
        release_owner_on_delay = false;
    }
    if (ticks == 30000 && clock_ms >= stop_supervisor_ms) throw StopPark{};
    clock_ms += ticks;
}
extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) {
    CHECK(task == nullptr); ++f135_hwm_queries; return 2048;
}

struct ProgressBle {
    unsigned disconnects{0};
    void disconnect() { ++disconnects; }
};
struct ProgressSelf {
    std::atomic<bool> ble_fault_{false};
    SemaphoreHandle_t vehicle_mutex_{&trend_mutex};
    ProgressBle ble;
    ProgressBle* ble_{&ble};
    bool early_fault{false}, late_fault{false}, identity_ready{true};
    unsigned runner_fault{0}, runner_calls{0}, invalidations{0};
    void process_ble_host_events_() { if (early_fault) ble_fault_.store(true); }
    void process_pending_telemetry_() { if (late_fault) ble_fault_.store(true); }
    bool command_identity_ready_() { return identity_ready; }
    void drive_command_runner_() {
        ++runner_calls;
        if (runner_fault == 1) throw std::bad_alloc();
        if (runner_fault == 2) throw std::runtime_error("runner fixture");
        if (runner_fault == 3) throw 42;
    }
    void mark_vcsec_unknown_() { ++invalidations; }
    bool ble_connected() { return true; }
};

namespace tk {
struct HostResetInfo { int code; };
HostResetInfo host_reset_info{static_cast<int>(ResetCode::PowerOn)};
const HostResetInfo& diag_crash_reset_info() { return host_reset_info; }
}
struct VehicleController {
    static tk::RebootReasonRecord observed_reason;
    static tk::RebootReasonRecord take_reboot_reason(NvsStorageAdapter& cfg) {
        return tk::take_reboot_reason(cfg);
    }
    static void set_boot_reboot_reason(const tk::RebootReasonRecord& record) { observed_reason = record; }
};
tk::RebootReasonRecord VehicleController::observed_reason{};
[[noreturn]] void boot_fatal(const char*) { CHECK(false); std::abort(); }

// Passive init-only seams; the generated body keeps the real predicate and last_cmd_ticks_ store.
const tk::RebootReasonRecord& boot_reason_slot() { return VehicleController::observed_reason; }
extern "C" TickType_t xTaskGetTickCount() { return static_cast<TickType_t>(clock_ms); }
struct ActivitySeedFixture {
    std::atomic<TickType_t> last_cmd_ticks_{0};
    void run();
};

#include "heap_supervisor_adapter.generated.hpp"

namespace {
NvsStorageAdapter cfg("tesla_cfg");
std::map<std::string, std::string> store, durable_store;
struct Write { std::string key, value; Owner owner; };
std::vector<Write> writes;
enum class StorageFault { None, False, FalseVisible, FalseDurable, Throw, ThrowOther };
StorageFault write_fault = StorageFault::None;
std::string fault_key;
bool load_error = false, load_throws = false;
}
NvsStorageAdapter::NvsStorageAdapter(const char* ns) : ns_(ns) {}
NvsStorageAdapter::~NvsStorageAdapter() = default;
bool NvsStorageAdapter::load(const std::string&, std::vector<uint8_t>&) { return false; }
bool NvsStorageAdapter::save(const std::string&, const std::vector<uint8_t>&) { return false; }
bool NvsStorageAdapter::remove(const std::string& key) { store.erase(key); durable_store.erase(key); return true; }
tk::NvsStringLoadState NvsStorageAdapter::load_str_state(const char* key, std::string& out, bool) {
    if (load_throws) throw std::bad_alloc();
    if (load_error) return tk::NvsStringLoadState::Error;
    const auto found = store.find(key);
    if (found == store.end()) return tk::NvsStringLoadState::Missing;
    out = found->second; return tk::NvsStringLoadState::Present;
}
bool NvsStorageAdapter::save_str(const char* key, const std::string& value) {
    writes.push_back({key, value, s_operation_gate.state()});
    const bool fail = fault_key.empty() || fault_key == key;
    if (fail && write_fault == StorageFault::Throw) throw std::bad_alloc();
    if (fail && write_fault == StorageFault::ThrowOther) throw 42;
    if (fail && write_fault == StorageFault::False) return false;
    store[key] = value;
    if (!(fail && write_fault == StorageFault::FalseVisible)) durable_store[key] = value;
    return !(fail && (write_fault == StorageFault::FalseVisible || write_fault == StorageFault::FalseDurable));
}
namespace tk {
bool cfg_save(NvsStorageAdapter&, const ConfigBlob&) {
    CHECK(s_operation_gate.state() == Owner::ConfigRestart);
    ++config_saves;
    if (config_save_throws) throw std::bad_alloc();
    return config_save_ok;
}
}

namespace {
tk::SupervisorState supervisor_state;
void reset(const char* counter = "0", Cause cause = Cause::Fault,
           tk::RebootReasonState reason = tk::RebootReasonState::Missing, uint8_t count = 0,
           bool trend_mutex_ok = true, bool retain_durable = false, int actual_reset_code = -1) {
    // Reset only the disposable host process. No production reset API is exposed.
    new (&s_operation_gate) tk::OtaIdentityOperationGate;
    new (&tk::s_runtime_admission) tk::RuntimeAdmissionGate;
    new (&tk::s_vehicle_progress) tk::VehicleProgress;
    supervisor_state = tk::SupervisorState{}; tk::s_startup = {}; tk::s_started = false;
    if (retain_durable) store = durable_store;
    else {
        store.clear(); durable_store.clear();
        if (counter) store[tk::nvs_contract::kBootFailures] = durable_store[tk::nvs_contract::kBootFailures] = counter;
    }
    writes.clear(); write_fault = StorageFault::None; fault_key.clear();
    load_error = load_throws = false;
    clock_ms = 0; stop_supervisor_ms = UINT64_MAX; largest = 65536; free_bytes = 100000;
    synced = network_up = true; ota_busy = false; host_resets = 0;
    partition_ok = query_ok = true; query_throws = false; image_state = ESP_OTA_IMG_VALID;
    queries = restart_calls = log_calls = delay_calls = host_reads = 0;
    invalidations = config_saves = 0; config_save_ok = true; config_save_throws = false;
    timer_reads = change_on_timer = timer_fault = heap_reads = change_on_heap = 0;
    recover_heap_on_read = false;
    expected_restart_owner = Owner::FaultRestart; release_owner_on_delay = false;
    throw_log = restart_throws = false;
    logs.clear(); task_ok = true; mutex_ok = trend_mutex_ok;
    mutex_busy = trend_mutex.held = false;
    task_creates = zero_waits = 0; created_task = nullptr;
    if (actual_reset_code >= 0) {
        tk::host_reset_info.code = actual_reset_code;
        actual_recovery_bootstrap(cfg);
    } else {
        (void)tk::safe_mode_begin(cfg, cause, reason, count);
        CHECK(tk::heap_supervisor_start(cfg, tk::make_reboot_reason_record(reason, "", count)));
    }
    writes.clear();
    CHECK(task_creates == 1 && created_task && task_arg == &tk::s_startup);
    supervisor_state = tk::SupervisorState(*static_cast<const tk::SupervisorStartup*>(task_arg));
    CHECK(supervisor_state.cfg == &cfg);
    CHECK(supervisor_state.budget.effective == tk::s_startup.budget.effective);
    CHECK(supervisor_state.budget.trusted == tk::s_startup.budget.trusted);
    CHECK(task_stack == 3072 && task_priority == tk::kPrioHeapSupervisor);
}
void sample(uint64_t ms, bool progress = true) {
    clock_ms = ms;
    if (progress) tk::runtime_vehicle_progress_publish(ms);
    tk::supervisor_iteration(supervisor_state);
}
void ready() { CHECK(tk::runtime_admission_mark_ready()); }
void qualified_until(uint64_t end) {
    for (uint64_t ms = 0; ms <= end; ms += tk::kRecoverySampleMs) sample(ms);
}
void exhaust() {
    largest = 4095; sample(0); sample(299999);
    CHECK(restart_calls == 0 && writes.empty());
}
void expect_committed_restart(uint64_t ms) {
    bool parked = false;
    try { sample(ms); } catch (const StopPark&) { parked = true; }
    CHECK(parked && restart_calls == 1 && s_operation_gate.state() == Owner::FaultRestart);
    CHECK(supervisor_state.shutdown_committed);
    CHECK(!s_operation_gate.try_begin(Owner::Ota));
}

void test_health_maintenance() {
    reset(); ready(); qualified_until(1770000);
    CHECK(writes.empty() && tk::safe_mode_healthy_clear_pending());
    sample(1800000);
    CHECK(writes.size() == 1 && writes[0].value == "0" && writes[0].owner == Owner::HealthCommit);
    CHECK(!tk::safe_mode_healthy_clear_pending());
    qualified_until(3600000); CHECK(writes.size() == 1);

    for (int failure = 0; failure < 7; ++failure) {
        reset();
        if (failure == 6) supervisor_state.budget = tk::HeapRecoveryBudget::seed(5, false);
        if (failure != 0) ready();
        synced = failure != 1; host_resets = failure == 2;
        network_up = failure != 4; ota_busy = failure == 5; largest = failure == 6 ? 4095 : 4096;
        for (uint64_t ms = 0; ms <= 1830000; ms += 30000) sample(ms, failure != 3);
        CHECK(writes.empty() && tk::safe_mode_healthy_clear_pending());
        CHECK((host_reads == 0) == (failure == 0));
    }
    reset(); ready(); qualified_until(1770000); sample(1860001);
    CHECK(writes.empty()); // a late final sample does not credit an unobserved stall

    for (auto fault : {StorageFault::False, StorageFault::FalseVisible,
                       StorageFault::Throw, StorageFault::ThrowOther}) {
        reset(); ready(); write_fault = fault; qualified_until(1800000);
        CHECK(writes.size() == 1 && tk::safe_mode_healthy_clear_pending());
        CHECK(s_operation_gate.state() == Owner::Idle);
        for (uint64_t ms = 1830000; ms < 2100000; ms += 30000) sample(ms);
        CHECK(writes.size() == 1);
        write_fault = StorageFault::None; sample(2100000);
        CHECK(writes.size() == 2 && !tk::safe_mode_healthy_clear_pending());
    }
    reset(); ready(); qualified_until(1770000);
    CHECK(s_operation_gate.try_begin(Owner::ConfigRestart)); sample(1800000);
    CHECK(writes.empty() && !supervisor_state.clear_write.attempted);
    CHECK(s_operation_gate.finish(Owner::ConfigRestart)); sample(1830000);
    CHECK(writes.size() == 1);

    for (unsigned fault = 0; fault != 7; ++fault) {
        reset(); ready(); qualified_until(1770000);
        timer_reads = 0; change_on_timer = 2; timer_fault = fault;
        sample(1800000);
        CHECK(writes.empty() && s_operation_gate.state() == Owner::Idle);
        CHECK(!supervisor_state.clear_write.attempted && tk::safe_mode_healthy_clear_pending());
    }
}

void test_restart_limits_and_ownership() {
    for (auto admission : {tk::RuntimeAdmissionState::Booting, tk::RuntimeAdmissionState::SafeMode,
                           tk::RuntimeAdmissionState::Fatal, tk::RuntimeAdmissionState::Ready}) {
        reset("0", Cause::Clean);
        if (admission == tk::RuntimeAdmissionState::SafeMode) CHECK(tk::runtime_admission_mark_safe_mode());
        if (admission == tk::RuntimeAdmissionState::Fatal) tk::runtime_admission_mark_fatal();
        if (admission == tk::RuntimeAdmissionState::Ready) ready();
        exhaust(); expect_committed_restart(300000);
        CHECK(writes.size() == 2 && writes[0].key == tk::nvs_contract::kBootFailures && writes[0].value == "1");
        CHECK(writes[1].key == tk::nvs_contract::kRebootReason);
        CHECK(writes[1].value == "heap:1" && writes[1].owner == Owner::FaultRestart);
    }
    for (auto owner : {Owner::Ota, Owner::IdentityMutation, Owner::FaultRestart,
                       Owner::HealthCommit, Owner::ConfigRestart}) {
        reset("0", Cause::Clean); exhaust(); CHECK(s_operation_gate.try_begin(owner)); sample(300000);
        CHECK(writes.empty() && restart_calls == 0 && !supervisor_state.restart_write.attempted);
        CHECK(s_operation_gate.state() == owner && logs.back().find("HEAP") != std::string::npos);
        CHECK(s_operation_gate.finish(owner)); expect_committed_restart(330000);
    }
    for (auto fault : {StorageFault::False, StorageFault::FalseVisible,
                       StorageFault::Throw, StorageFault::ThrowOther}) {
        reset("0", Cause::Clean); exhaust(); write_fault = fault; sample(300000);
        CHECK(writes.size() == 1 && restart_calls == 0 && s_operation_gate.state() == Owner::Idle);
        sample(330000); CHECK(writes.size() == 1);
        write_fault = StorageFault::None; expect_committed_restart(600000);
        CHECK(writes.size() == 3 && writes.back().value == "heap:1");
    }
    reset("0", Cause::Clean); exhaust(); throw_log = true; expect_committed_restart(300000);
    CHECK(s_operation_gate.state() == Owner::FaultRestart);

    for (bool recover : {false, true}) {
        reset("0", Cause::Clean); exhaust(); heap_reads = 0; change_on_heap = 2;
        recover_heap_on_read = recover; sample(300000);
        CHECK(writes.empty() && !restart_calls && s_operation_gate.state() == Owner::Idle);
        CHECK(!supervisor_state.restart_write.attempted);
    }
    reset("0", Cause::Clean); exhaust(); clock_ms = 300000; restart_throws = true;
    bool parked = false;
    try { created_task(task_arg); } catch (const StopPark&) { parked = true; }
    CHECK(parked && restart_calls == 1 && writes.size() == 2);
    CHECK(s_operation_gate.state() == Owner::FaultRestart);
    CHECK(!s_operation_gate.try_begin(Owner::Ota));
    CHECK(!s_operation_gate.try_begin(Owner::IdentityMutation));
    CHECK(writes[0].owner == Owner::FaultRestart && writes[1].value == "heap:1");
    // A new real task starts a fresh critical hold, independent of the separately sampled
    // host-owned state. Its thrown restart still seals the observed owner and parks.
    CHECK(clock_ms == 600300);
}

void sample_without_restart(uint64_t ms);

void test_safe_mode_and_budget() {
    for (auto state : {ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_NEW, ESP_OTA_IMG_INVALID}) {
        reset("4", Cause::HeapRecovery); CHECK(tk::safe_mode_active()); image_state = state;
        exhaust(); sample_without_restart(300000);
        CHECK(writes.empty() && restart_calls == 0 && s_operation_gate.state() == Owner::Idle);
    }
    for (auto state : {ESP_OTA_IMG_VALID, ESP_OTA_IMG_UNDEFINED}) {
        reset("4", Cause::HeapRecovery); image_state = state; exhaust(); expect_committed_restart(300000);
        CHECK(writes.size() == 2 && writes[0].key == tk::nvs_contract::kBootFailures && writes[0].value == "5");
        CHECK(writes[1].key == tk::nvs_contract::kRebootReason && writes[1].value == "heap:1");
    }
    reset("4", Cause::HeapRecovery); exhaust(); write_fault = StorageFault::FalseVisible;
    fault_key = tk::nvs_contract::kBootFailures; sample(300000); sample(600000);
    CHECK(writes.size() == 1 && store[tk::nvs_contract::kBootFailures] == "5");
    CHECK(durable_store[tk::nvs_contract::kBootFailures] == "4" && restart_calls == 0);
    write_fault = StorageFault::None; sample(900000);
    CHECK(writes.size() == 1 && restart_calls == 0); // visible exhausted value grants no retry

    for (const char* bad : {"", "broken", "01", "101"}) {
        reset("4", Cause::HeapRecovery); store[tk::nvs_contract::kBootFailures] = bad;
        exhaust(); sample(300000); CHECK(writes.empty() && !restart_calls);
    }
    reset("4", Cause::HeapRecovery); exhaust(); query_ok = false; sample(300000);
    CHECK(writes.empty() && !restart_calls && s_operation_gate.state() == Owner::Idle);

    for (auto reason : {tk::RebootReasonState::Present, tk::RebootReasonState::Error}) {
        reset("0", reason == tk::RebootReasonState::Present ? Cause::HeapRecovery : Cause::Uncertain,
              reason, 1); ready(); qualified_until(1800000);
        CHECK(supervisor_state.budget.effective == (reason == tk::RebootReasonState::Present ? 0 : 5));
        writes.clear(); largest = 4095; sample(1830000);
        if (reason == tk::RebootReasonState::Present) expect_committed_restart(2130000);
        else sample(2130000);
        CHECK(restart_calls == (reason == tk::RebootReasonState::Present ? 1 : 0));
    }
}

void next_real_boot(tk::ResetCode code) {
    reset(nullptr, Cause::Clean, tk::RebootReasonState::Missing, 0, true, true,
          static_cast<int>(code));
}
unsigned confirmed_shared_count() {
    unsigned value = 999;
    CHECK(tk::boot_fail_parse(durable_store[tk::nvs_contract::kBootFailures].c_str(), value));
    return value;
}
void critical_then_boot(unsigned expected_count, uint8_t expected_streak) {
    exhaust(); expect_committed_restart(300000);
    CHECK(writes.size() == 2 && writes[0].key == tk::nvs_contract::kBootFailures &&
          writes[0].value == std::to_string(expected_count));
    CHECK(writes[1].key == tk::nvs_contract::kRebootReason &&
          writes[1].value == tk::heap_reason_format(expected_streak).text);
    CHECK(confirmed_shared_count() == expected_count);
    next_real_boot(tk::ResetCode::Sw);
    CHECK(confirmed_shared_count() == expected_count); // reservation is not charged twice
    CHECK(VehicleController::observed_reason.state == tk::RebootReasonState::Present);
    CHECK(VehicleController::observed_reason.heap_restarts == expected_streak);
    CHECK(!tk::boot_activity_seed_allowed(static_cast<int>(tk::ResetCode::Sw),
                                          VehicleController::observed_reason.state));
}
void sample_without_restart(uint64_t ms) {
    bool parked = false;
    try { sample(ms); } catch (const StopPark&) { parked = true; }
    CHECK(!parked && restart_calls == 0);
}
void test_shared_multiboot_budget() {
    // H/F/H/F/H: every deliberate effect and actual fault shares one durable store.
    reset("0", Cause::Clean);
    critical_then_boot(1, 1);
    ready(); for (uint64_t ms = 0; ms <= 60000; ms += 30000) sample(ms);
    next_real_boot(tk::ResetCode::Panic); CHECK(confirmed_shared_count() == 2);
    CHECK(!tk::safe_mode_active() && !tk::runtime_admission_vehicle_ready());
    critical_then_boot(3, 1);
    ready(); for (uint64_t ms = 0; ms <= 120000; ms += 30000) sample(ms);
    next_real_boot(tk::ResetCode::TaskWdt); CHECK(confirmed_shared_count() == 4);
    CHECK(tk::safe_mode_active());
    CHECK(tk::runtime_admission_state() == tk::RuntimeAdmissionState::SafeMode);
    critical_then_boot(5, 1); CHECK(tk::safe_mode_active());
    exhaust(); for (uint64_t ms = 300000; ms <= 1500000; ms += 300000) sample_without_restart(ms);
    CHECK(writes.empty() && !restart_calls && confirmed_shared_count() == 5);
    CHECK(s_operation_gate.state() == Owner::Idle);
    CHECK(!tk::safe_mode_healthy_clear_pending());

    // All-H charges once per reset, admits SafeMode at four, then stops at five.
    reset("0", Cause::Clean);
    for (unsigned i = 1; i <= 5; ++i) {
        critical_then_boot(i, static_cast<uint8_t>(i));
        CHECK(tk::safe_mode_active() == (i >= 4));
    }
    exhaust(); sample(300000); CHECK(!restart_calls && writes.empty());

    // Fault-first alternating path exhausts the SAME scalar rather than refilling heap credit.
    reset("0", Cause::Clean);
    next_real_boot(tk::ResetCode::Panic); CHECK(confirmed_shared_count() == 1);
    critical_then_boot(2, 1);
    next_real_boot(tk::ResetCode::IntWdt); CHECK(confirmed_shared_count() == 3);
    critical_then_boot(4, 1); CHECK(tk::safe_mode_active());
    critical_then_boot(5, 2);
    exhaust(); sample(300000); CHECK(writes.empty() && !restart_calls);

    // Legacy source enrollment is exercised through the real main call, not supplied causes.
    reset("0", Cause::Clean);
    durable_store[tk::nvs_contract::kRebootReason] = "heap:5";
    next_real_boot(tk::ResetCode::Sw);
    CHECK(confirmed_shared_count() == 5 && tk::safe_mode_active());
    exhaust(); sample_without_restart(300000); CHECK(writes.empty());
    reset("0", Cause::Clean);
    durable_store[tk::nvs_contract::kRebootReason] = "heap:4";
    next_real_boot(tk::ResetCode::Panic);
    CHECK(confirmed_shared_count() == 5 && tk::safe_mode_active());

    for (unsigned previous : {0u, 3u, 4u, 5u, 100u}) {
        reset(std::to_string(previous).c_str(), Cause::HeapRecovery,
              tk::RebootReasonState::Present, 1);
        CHECK(confirmed_shared_count() == std::max(previous, 1u));
        CHECK(tk::safe_mode_active() == (previous >= 4));
        exhaust();
        if (previous < 5) expect_committed_restart(300000);
        else { sample(300000); CHECK(writes.empty() && !restart_calls); }
    }
}

void test_shared_durability_and_refill() {
    // Raw Stable applies in normal mode too, before the first counter write.
    for (auto state : {ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_NEW, ESP_OTA_IMG_INVALID}) {
        reset("0", Cause::Clean); image_state = state;
        exhaust(); sample_without_restart(300000); CHECK(writes.empty());
    }
    for (auto fault : {StorageFault::False, StorageFault::FalseVisible, StorageFault::FalseDurable,
                       StorageFault::Throw, StorageFault::ThrowOther}) {
        reset("2", Cause::HeapRecovery, tk::RebootReasonState::Present, 1);
        exhaust(); fault_key = tk::nvs_contract::kBootFailures; write_fault = fault;
        sample(300000);
        CHECK(!restart_calls && writes.size() == 1 && writes[0].key == tk::nvs_contract::kBootFailures);
        CHECK(durable_store.find(tk::nvs_contract::kRebootReason) == durable_store.end());
        CHECK(s_operation_gate.state() == Owner::Idle);
        // Both permitted outcomes of an uncertain commit remain fail-closed on this attempt.
        CHECK(confirmed_shared_count() == (fault == StorageFault::FalseDurable ? 3 : 2));
    }
    // A confirmed reservation survives a failed final reason and an actual subsequent fault.
    reset("2", Cause::HeapRecovery, tk::RebootReasonState::Present, 1);
    exhaust(); fault_key = tk::nvs_contract::kRebootReason; write_fault = StorageFault::False;
    sample(300000); CHECK(!restart_calls && confirmed_shared_count() == 3);
    CHECK(writes.size() == 2 && writes[0].value == "3");
    next_real_boot(tk::ResetCode::Panic); CHECK(confirmed_shared_count() == 4);
    CHECK(tk::safe_mode_active());

    // Lower visible state cannot erase a confirmed RAM floor in the same boot.
    reset("3", Cause::HeapRecovery, tk::RebootReasonState::Present, 1);
    store[tk::nvs_contract::kBootFailures] = "0";
    exhaust(); expect_committed_restart(300000); CHECK(confirmed_shared_count() == 4);
    reset("1", Cause::HeapRecovery, tk::RebootReasonState::Present, 1);
    store.erase(tk::nvs_contract::kBootFailures); exhaust(); sample(300000);
    CHECK(writes.empty() && !restart_calls);

    // A failed durable zero cannot refill even the diagnostic streak.
    for (auto fault : {StorageFault::FalseVisible, StorageFault::FalseDurable,
                       StorageFault::Throw, StorageFault::False}) {
        reset("2", Cause::HeapRecovery, tk::RebootReasonState::Present, 2); ready();
        fault_key = tk::nvs_contract::kBootFailures; write_fault = fault;
        qualified_until(1800000);
        CHECK(tk::safe_mode_healthy_clear_pending() && supervisor_state.budget.effective == 2);
        CHECK(writes.size() == 1 && !restart_calls);
        // Preserve actual30s fresh observations during the five-minute retry interval.
        for (uint64_t ms = 1830000; ms < 2100000; ms += 30000) sample(ms);
        CHECK(writes.size() == 1 && supervisor_state.budget.effective == 2);
        write_fault = StorageFault::None; sample(2100000);
        CHECK(!tk::safe_mode_healthy_clear_pending() && supervisor_state.budget.effective == 0);
        CHECK(confirmed_shared_count() == 0);
        largest = 4095; sample(2130000); expect_committed_restart(2430000);
        CHECK(confirmed_shared_count() == 1);
    }
}

void test_actual_init_activity_seed() {
    // Literal reset-code oracle, independent of boot_activity_seed_allowed implementation.
    for (int code = -1; code <= 16; ++code) {
        for (auto reason : {tk::RebootReasonState::Missing, tk::RebootReasonState::Present,
                            tk::RebootReasonState::Error}) {
            for (uint32_t tick : {0u, 1u, 1800000u, UINT32_MAX}) {
                reset("0", Cause::Clean);
                tk::host_reset_info.code = code;
                VehicleController::observed_reason = tk::make_reboot_reason_record(reason, "", 1);
                clock_ms = tick;
                ActivitySeedFixture activity;
                activity.run();
                const bool known_clean = code == 1 || code == 2 || code == 3 || code == 8 ||
                                         code == 10 || code == 11 || code == 12;
                const bool admitted = reason == tk::RebootReasonState::Missing && known_clean;
                CHECK(activity.last_cmd_ticks_.load() == (admitted ? tick : 0));
            }
        }
    }
    // Confirmed healthy maintenance clears a trusted shared budget, never immutable boot evidence.
    // Error remains untrusted and must not acquire clear authority merely through elapsed time.
    for (auto reason : {tk::RebootReasonState::Present, tk::RebootReasonState::Error}) {
        reset("2", reason == tk::RebootReasonState::Present ? Cause::HeapRecovery : Cause::Uncertain,
              reason, 2);
        tk::host_reset_info.code = 3;
        VehicleController::observed_reason = tk::make_reboot_reason_record(reason, "", 2);
        const auto original = VehicleController::observed_reason.state;
        ready(); qualified_until(1800000);
        if (reason == tk::RebootReasonState::Present) {
            CHECK(confirmed_shared_count() == 0 && !tk::safe_mode_healthy_clear_pending());
            CHECK(supervisor_state.budget.effective == 0 && writes.size() == 1 && writes[0].value == "0");
        } else {
            CHECK(confirmed_shared_count() == 2 && !tk::safe_mode_healthy_clear_pending());
            CHECK(supervisor_state.budget.effective == 5 && !supervisor_state.budget.trusted && writes.empty());
        }
        CHECK(VehicleController::observed_reason.state == original);
        ActivitySeedFixture activity;
        activity.run();
        CHECK(activity.last_cmd_ticks_.load() == 0);
    }
}

void test_trend_and_startup() {
    reset("0", Cause::Clean); mutex_busy = true; const auto before = zero_waits;
    sample(0); CHECK(zero_waits == before + 1 && !trend_mutex.held);
    mutex_busy = false; sample(30000); CHECK(!trend_mutex.held);
    const auto startup = tk::s_startup;
    CHECK(!tk::heap_supervisor_start(cfg, {})); // no duplicate sampler
    CHECK(tk::s_startup.cfg == startup.cfg);
    CHECK(tk::s_startup.budget.effective == startup.budget.effective);
    CHECK(tk::s_startup.budget.trusted == startup.budget.trusted);
    tk::s_started = false; task_ok = false;
    CHECK(!tk::heap_supervisor_start(cfg, {}));
    CHECK(!tk::s_started);
}

void test_bootstrap_and_progress() {
    for (auto state : {ESP_OTA_IMG_VALID, ESP_OTA_IMG_UNDEFINED, ESP_OTA_IMG_PENDING_VERIFY}) {
        reset("0", Cause::Clean); image_state = state;
        CHECK(s_operation_gate.try_begin(Owner::HealthCommit)); release_owner_on_delay = true;
        fatal_startup_rollback();
        CHECK(delay_calls >= 1 && !release_owner_on_delay);
        CHECK(invalidations == (state == ESP_OTA_IMG_PENDING_VERIFY ? 1 : 0));
        CHECK(s_operation_gate.state() == (state == ESP_OTA_IMG_PENDING_VERIFY
                                           ? Owner::FaultRestart : Owner::Idle));
    }
    reset("0", Cause::Clean); query_ok = false; fatal_startup_rollback();
    CHECK(!invalidations && s_operation_gate.state() == Owner::Idle);
    reset("0", Cause::Clean); query_throws = true; fatal_startup_rollback();
    CHECK(!invalidations && s_operation_gate.state() == Owner::Idle);

    for (bool early : {false, true}) for (bool late : {false, true})
        for (unsigned runner = 0; runner != 4; ++runner) {
            reset("0", Cause::Clean); clock_ms = 42;
            ProgressSelf self; self.early_fault = early; self.late_fault = late;
            self.runner_fault = runner;
            CHECK(!tk::runtime_vehicle_progress_fresh(42));
            actual_progress_caller(&self);
            CHECK(tk::runtime_vehicle_progress_fresh(clock_ms) == (!early && !late && !runner));
            CHECK(self.runner_calls == (early ? 0u : 1u));
        }
    for (bool failed_parse : {false, true}) {
        reset(); ready(); ProgressSelf self;
        self.late_fault = failed_parse; self.identity_ready = false; // idle/unpaired positive
        for (uint64_t ms = 0; ms <= 1830000; ms += 30000) {
            clock_ms = ms; actual_progress_caller(&self); sample(ms, false);
        }
        CHECK(self.runner_calls == 0);
        CHECK(writes.size() == (failed_parse ? 0u : 1u));
        CHECK(tk::safe_mode_healthy_clear_pending() == failed_parse);
    }
    for (auto recovery : {tk::VinTransitionRecovery::ClearMarker,
                           tk::VinTransitionRecovery::RollBackPreviousVin,
                           tk::VinTransitionRecovery::CompleteNewIdentity}) {
        reset("0", Cause::Clean); expected_restart_owner = Owner::ConfigRestart;
        tk::ConfigBlob blob{};
        const tk::VinTransitionMarker marker{"previous-fixture", "key-fixture"};
        bool parked = false;
        try { CHECK(actual_vin_repair(recovery, blob, marker, cfg, cfg) == nullptr); }
        catch (const StopPark&) { parked = true; }
        CHECK(parked == (recovery != tk::VinTransitionRecovery::ClearMarker));
        CHECK(s_operation_gate.state() == (parked ? Owner::ConfigRestart : Owner::Idle));
    }
    for (bool success : {false, true}) {
        reset("0", Cause::Clean); expected_restart_owner = Owner::ConfigRestart;
        config_save_ok = success; tk::ConfigBlob blob{};
        blob.wifi_rollback_active = true; blob.wifi_ssid_backup = "fixture";
        bool parked = false;
        try { actual_wifi_rollback(blob, cfg); } catch (const StopPark&) { parked = true; }
        CHECK(parked == success && config_saves == 1);
        CHECK(s_operation_gate.state() == (success ? Owner::ConfigRestart : Owner::Idle));
        CHECK(invalidations == 0); // automatic configuration recovery never confirms/rolls back
    }
    reset("0", Cause::Fault, tk::RebootReasonState::Error, 5); ready();
    qualified_until(1830000);
    CHECK(store[tk::nvs_contract::kBootFailures] == "1" && writes.empty());
    CHECK(!tk::safe_mode_healthy_clear_pending() && !supervisor_state.budget.trusted);
}

void test_actual_diagnostic_and_observed_owner() {
    reset("0", Cause::Clean); logs.clear(); log_calls = 0;
    unsigned n = 0;
    for (auto owner : {Owner::Ota, Owner::IdentityMutation, Owner::FaultRestart,
                       Owner::HealthCommit, Owner::ConfigRestart}) {
        CHECK(s_operation_gate.try_begin(owner));
        {
            OtaFaultRestartGuard rejected;
            CHECK(!rejected.held() && rejected.observed_owner() == owner);
            rejected.seal();
            tk::report_degraded(supervisor_state, n++ * 30000, "fixture", rejected.observed_owner());
        }
        CHECK(s_operation_gate.state() == owner && s_operation_gate.finish(owner));
    }
    CHECK(log_calls == 1 && logs[0].find("owner=ota") != std::string::npos);
    tk::report_degraded(supervisor_state, 300000, "fixture", Owner::Idle);
    CHECK(log_calls == 2 && logs[1].find("4 suppressed") != std::string::npos);
    const char* names[] = {"idle", "ota", "identity", "fault_restart", "health_maintenance", "config_restart"};
    for (unsigned i = 0; i != 6; ++i)
        CHECK(std::strcmp(tk::operation_owner_name(static_cast<Owner>(i)), names[i]) == 0);
}

void test_actual_task_local_initialization() {
    for (unsigned mode = 0; mode != 3; ++mode) {
        reset("0", mode == 1 ? Cause::Fault : Cause::HeapRecovery, mode == 1 ? tk::RebootReasonState::Error : tk::RebootReasonState::Present,
              mode == 2 ? 5 : 4);
        if (tk::safe_mode_active()) CHECK(tk::runtime_admission_mark_safe_mode());
        else ready();
        largest = 4095; stop_supervisor_ms = 360000;
        bool stopped = false;
        try { created_task(task_arg); } catch (const StopPark&) { stopped = true; }
        CHECK(stopped && clock_ms <= 360000);
        CHECK(restart_calls == (mode == 0 ? 1 : 0));
        if (mode == 0) {
            CHECK(store[tk::nvs_contract::kRebootReason] == "heap:5");
            CHECK(s_operation_gate.state() == Owner::FaultRestart);
        } else {
            CHECK(writes.empty() && s_operation_gate.state() == Owner::Idle);
        }
    }
}

void test_heap_log_preserves_8bit_metrics() {
    reset("0", Cause::Clean);
    sample(0);
    const std::string expected = "HEAP free=2000000 largest_block=1000000 min_free=500000 "
                                 "internal_largest=65536 supervisor_min_stack=2048";
    CHECK(std::find(logs.begin(), logs.end(), expected) != logs.end());
    // Even while the diagnostic 8-bit pool stays large, INTERNAL exhaustion must recover.
    exhaust();
    expect_committed_restart(300000);
}
}  // namespace

void test_actual_ota_abort_before_heap_restart() {
    reset("0", Cause::Clean);
    ota_abort_requests = 0;
    ota_busy = true;
    CHECK(s_operation_gate.try_begin(Owner::Ota));
    exhaust();
    CHECK(ota_abort_requests == 0 && s_operation_gate.state() == Owner::Ota);
    sample(300000);
    CHECK(ota_abort_requests == 1 && writes.empty() && restart_calls == 0);
    CHECK(s_operation_gate.state() == Owner::Ota); // Supervisor cannot steal this owner.
    sample(600000);
    CHECK(ota_abort_requests == 2 && writes.empty() && restart_calls == 0);
    // External passive writer-abort acknowledgement. The OTA application harness must prove
    // that only its actual raw abort/quiescence path can publish this release.
    CHECK(s_operation_gate.finish(Owner::Ota));
    ota_busy = false;
    expect_committed_restart(630000);
    CHECK(s_operation_gate.state() == Owner::FaultRestart);
}

int main() {
    CHECK(!tk::stack_watch_min_free_bytes(tk::StackWatch::HeapSupervisor).has_value());
    CHECK(!tk::stack_watch_min_free_bytes(tk::StackWatch::Main).has_value());
    // First use is deliberately before any successful trend-mutex creation in this process.
    reset("0", Cause::Clean, tk::RebootReasonState::Missing, 0, false);
    CHECK(!tk::heap_trend_prepare()); exhaust(); expect_committed_restart(300000);
    test_health_maintenance();
    test_restart_limits_and_ownership();
    test_safe_mode_and_budget();
    test_shared_multiboot_budget();
    test_shared_durability_and_refill();
    test_actual_init_activity_seed();
    test_trend_and_startup();
    test_bootstrap_and_progress();
    test_actual_diagnostic_and_observed_owner();
    test_heap_log_preserves_8bit_metrics();
    test_actual_task_local_initialization();
    CHECK(f135_hwm_queries > 0);
    CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::HeapSupervisor) == 2048u);
    CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::Main) == 2048u);
    test_actual_ota_abort_before_heap_restart();
    std::cout << "heap supervisor: " << checks << " checks OK\n";
}
