// Complete production bodies with passive SDK/storage/RTOS adapters. No real identity is used.
#include "logic/identity_recovery.hpp"
#include "logic/key_rotation.hpp"
#include "logic/heap_supervision.hpp"
#include "logic/status_model.hpp"
#include "runtime_admission.hpp"
#include "ota_update.hpp"
#include "rtos_guard.hpp"
#include <freertos/task.h>
#include <atomic>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>

static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << "identity recovery " << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
using Owner = tk::OtaIdentityGateState;
static tk::OtaIdentityOperationGate s_operation_gate;
static void finish_operation(Owner owner) { CHECK(s_operation_gate.finish(owner)); }
namespace tk { using MutexGuard = SemGuard; }

struct HostSemaphore { bool held{}; } command_semaphore;
static bool lock_ok = true, storage_ok = true, marker_present = true, private_present = false;
static bool storage_throws = false, log_throws = false, delay_throws = false, restart_throws = false;
static bool query_ok = true, partition_ok = true, query_throws = false, expect_config_query = true;
static unsigned queries = 0, marker_reads = 0, private_reads = 0, restarts = 0, generations = 0;
static bool generation_throws = false;
static bool fatal_in_query = false, fatal_in_journal = false, fatal_after_claim = false;
static tk::IdentityReloadOnce* reserve_in_journal = nullptr;
static tk::KeyRotationResult generated_result = tk::KeyRotationResult::CommitUnknown;
struct Parked {};
enum esp_ota_img_states_t { ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID,
    ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED };
static esp_ota_img_states_t image_state = ESP_OTA_IMG_VALID;
struct esp_partition_t {};
static esp_partition_t partition;
constexpr int ESP_OK = 0;
const esp_partition_t* esp_ota_get_running_partition() { return partition_ok ? &partition : nullptr; }
int esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t* out) {
    ++queries;
    if (expect_config_query) CHECK(s_operation_gate.state() == Owner::ConfigRestart);
    if (query_throws) throw std::bad_alloc();
    if (fatal_in_query) tk::runtime_admission_mark_fatal(tk::RuntimeFatalComponent::Http);
    *out = image_state;
    return query_ok ? ESP_OK : -1;
}
class NvsStorageAdapter {
public:
    bool probe_blob(const std::string& key, bool& present) {
        CHECK(command_semaphore.held);
        if (key == tk::kKeyRotationMarker) { ++marker_reads; present = marker_present; }
        else { CHECK(key == tk::nvs_contract::kPrivateKey); ++private_reads; present = private_present; }
        if (storage_throws) throw std::bad_alloc();
        if (key == tk::kKeyRotationMarker) {
            if (fatal_in_journal) tk::runtime_admission_mark_fatal(tk::RuntimeFatalComponent::Nvs);
            if (reserve_in_journal) {
                const tk::IdentityReloadAdmission good{true,true,true,false,false,false,true,true,true};
                CHECK(reserve_in_journal->reserve(good));
            }
        }
        return storage_ok;
    }
};
static NvsStorageAdapter storage;
class VehicleController {
public:
    NvsStorageAdapter* storage_{&storage};
    SemaphoreHandle_t command_mutex_{&command_semaphore};
    std::atomic<bool> key_runtime_safe_{false}, key_reload_required_{true};
    std::atomic<bool> vin_transition_pending_{false}, pairing_cleanup_pending_{false};
    std::atomic<bool> believed_paired_{true}, pairing_recovery_hint_{false};
    std::atomic<tk::AutomaticKeyAction> automatic_key_action_{tk::AutomaticKeyAction::AutomaticCommitReload};
    bool key_rotation_recovered_at_boot_{false};
    bool generate_key();
    tk::KeyRotationResult generate_key_locked_() {
        CHECK(command_semaphore.held && s_operation_gate.state() == Owner::IdentityMutation);
        ++generations;
        if (generated_result == tk::KeyRotationResult::CommitUnknown) key_reload_required_.store(true);
        if (generation_throws) throw std::bad_alloc();
        return generated_result;
    }
};
extern "C" BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks) {
    CHECK(sem == &command_semaphore && !sem->held);
    CHECK(ticks == portMAX_DELAY || ticks == pdMS_TO_TICKS(100));
    if (!lock_ok) return pdFALSE;
    sem->held = true; return pdTRUE;
}
extern "C" BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    CHECK(sem == &command_semaphore && sem->held);
    if (fatal_after_claim) tk::runtime_admission_mark_fatal(tk::RuntimeFatalComponent::Network);
    sem->held = false; return pdTRUE;
}
extern "C" void vTaskDelay(TickType_t ticks) {
    CHECK(!command_semaphore.held);
    if (ticks == pdMS_TO_TICKS(10000)) throw Parked{};
    CHECK(ticks == pdMS_TO_TICKS(300) || ticks == pdMS_TO_TICKS(30000));
    if (delay_throws) throw std::bad_alloc();
}
void esp_restart() {
    CHECK(!command_semaphore.held && s_operation_gate.state() == Owner::ConfigRestart);
    ++restarts; if (restart_throws) throw std::bad_alloc();
}
static const char* TAG = "passive_identity_fixture";
static void host_log(const char*, const char*, ...) { if (log_throws) throw std::bad_alloc(); }
#define ESP_LOGE(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_log(__VA_ARGS__)

// Only the JSON allocator/platform is doubled here. The complete actual MQTT device emitter
// is extracted below; real locked-cJSON OOM/retained-publish tests remain a separate host gate.
struct cJSON {};
namespace tk {
using JsonOwner = std::map<std::string, std::string>;
class JsonBuilder {
    JsonOwner fields_;
    cJSON root_;
    bool failed_{};
public:
    cJSON* root() { return failed_ ? nullptr : &root_; }
    void invalidate() noexcept { failed_ = true; }
    void string(cJSON*, const char* key, const char* value) { if (!failed_) fields_[key] = value; }
    void boolean(cJSON*, const char* key, bool value) { if (!failed_) fields_[key] = value ? "true" : "false"; }
    void number(cJSON*, const char* key, double value) { if (!failed_) fields_[key] = std::to_string(value); }
    bool null(cJSON*, const char* key) {
        if (failed_) return false;
        fields_[key] = "null";
        return true;
    }
    JsonOwner finish() {
        if (failed_) { fields_.clear(); return {}; }
        return fields_;
    }
};
}
#include "identity_recovery_adapter.generated.hpp"

static void reset() {
    CHECK(!command_semaphore.held);
    new (&s_operation_gate) tk::OtaIdentityOperationGate;
    new (&tk::s_runtime_admission) tk::RuntimeAdmissionGate;
    CHECK(tk::runtime_admission_mark_ready());
    lock_ok = storage_ok = marker_present = query_ok = partition_ok = expect_config_query = true;
    storage_throws = log_throws = delay_throws = restart_throws = query_throws = false;
    generation_throws = private_present = false;
    fatal_in_query = fatal_in_journal = fatal_after_claim = false;
    reserve_in_journal = nullptr;
    queries = marker_reads = private_reads = restarts = generations = 0;
    image_state = ESP_OTA_IMG_VALID;
    generated_result = tk::KeyRotationResult::CommitUnknown;
}
static bool invoke(tk::IdentityReloadOnce& once, VehicleController& self) {
    return automatic_identity_reload_once(once, self.storage_, self.command_mutex_, [&] {
        return tk::IdentityReloadAdmission{false,
            self.automatic_key_action_.load() == tk::AutomaticKeyAction::AutomaticCommitReload,
            self.key_reload_required_.load(), self.vin_transition_pending_.load(),
            self.pairing_cleanup_pending_.load(), self.key_rotation_recovered_at_boot_};
    });
}
struct Emitter {
    std::vector<std::string> path;
    std::map<std::string, std::string> fields;
    std::string name(const char* key) {
        std::string out; for (const auto& p : path) { if (!out.empty()) out += '.'; out += p; }
        if (key) { if (!out.empty()) out += '.'; out += key; } return out;
    }
    void obj_begin(const char* key) { path.emplace_back(key ? key : "[]"); }
    void obj_end() { path.pop_back(); }
    void arr_begin(const char* key) { obj_begin(key); }
    void arr_end() { obj_end(); }
    void str(const char* key, const char* value) { fields[name(key)] = value; }
    void num(const char* key, double value) { fields[name(key)] = std::to_string(value); }
    void boolean(const char* key, bool value) { fields[name(key)] = value ? "true" : "false"; }
    void null(const char* key) { fields[name(key)] = "null"; }
};
int main() {
    const tk::IdentityReloadAdmission all_good{true, true, true, false, false, false, true, true, true};
    tk::IdentityReloadOnce direct;
    CHECK(direct.reserve(all_good) && direct.reserved());
    CHECK(!direct.reserve(all_good));

    for (auto state : {ESP_OTA_IMG_VALID, ESP_OTA_IMG_UNDEFINED}) {
        reset(); image_state = state; VehicleController self; tk::IdentityReloadOnce once;
        bool parked = false;
        try { actual_reload_caller(&self, once); } catch (const Parked&) { parked = true; }
        CHECK(parked && restarts == 1 && queries == 1 && marker_reads == 1);
        CHECK(once.reserved() && s_operation_gate.state() == Owner::ConfigRestart);
        CHECK(!self.believed_paired_ && !self.key_runtime_safe_);
        CHECK(!invoke(once, self) && restarts == 1);
        CHECK(!s_operation_gate.try_begin(Owner::Ota));
    }
    // Real query/probe facades publish Fatal AFTER the helper's cached Ready read. Neither
    // may earn the final CAS, reserve a restart or strand ConfigRestart ownership.
    for (bool from_query : {true, false}) {
        reset(); VehicleController candidate; tk::IdentityReloadOnce single;
        fatal_in_query = from_query; fatal_in_journal = !from_query;
        CHECK(!invoke(single, candidate) && !single.reserved() && restarts == 0);
        CHECK(queries == 1 && marker_reads == 1 && s_operation_gate.state() == Owner::Idle);
        const auto snapshot = tk::runtime_admission_snapshot();
        CHECK(snapshot.state == tk::RuntimeAdmissionState::Fatal);
        CHECK(snapshot.component == (from_query ? tk::RuntimeFatalComponent::Http : tk::RuntimeFatalComponent::Nvs));
        CHECK(!tk::runtime_admission_try_claim_identity_reload());
    }
    // Once the final claim wins, a subsequent Fatal remains visible and cannot revoke/replay
    // the already-linearized sealed attempt. A returning restart still reaches caller park.
    reset(); VehicleController late; tk::IdentityReloadOnce late_once;
    fatal_after_claim = true; bool late_park = false;
    try { actual_reload_caller(&late, late_once); } catch (const Parked&) { late_park = true; }
    CHECK(late_park && late_once.reserved() && restarts == 1 && !command_semaphore.held);
    CHECK(s_operation_gate.state() == Owner::ConfigRestart);
    CHECK(tk::runtime_admission_snapshot().component == tk::RuntimeFatalComponent::Network);
    CHECK(!tk::runtime_admission_try_claim_identity_reload());
    // Defensive impossible local-reservation loss after the claim: seal + park, zero effects.
    reset(); VehicleController consumed; tk::IdentityReloadOnce consumed_once;
    reserve_in_journal = &consumed_once; bool consumed_park = false;
    try { actual_reload_caller(&consumed, consumed_once); } catch (const Parked&) { consumed_park = true; }
    CHECK(consumed_park && consumed_once.reserved() && restarts == 0);
    CHECK(!command_semaphore.held && s_operation_gate.state() == Owner::ConfigRestart);
    CHECK(!tk::runtime_admission_try_claim_identity_reload());
    // Actual competing CAS calls, not a mirrored decision or task-name attribution guess.
    for (unsigned trial = 0; trial != 64; ++trial) {
        tk::RuntimeAdmissionGate race; CHECK(race.mark_ready());
        std::atomic<bool> start{false}; bool claimed = false;
        std::thread reload([&] { while (!start.load(std::memory_order_acquire)) {};
            claimed = race.try_claim_identity_reload(); });
        std::thread fatal([&] { while (!start.load(std::memory_order_acquire)) {};
            race.mark_fatal(tk::RuntimeFatalComponent::Http); });
        start.store(true, std::memory_order_release); reload.join(); fatal.join();
        CHECK(race.snapshot().state == tk::RuntimeAdmissionState::Fatal);
        CHECK(race.snapshot().component == tk::RuntimeFatalComponent::Http);
        CHECK(!race.try_claim_identity_reload());
        (void)claimed; // Both legal orderings are allowed; final Fatal never reopens.
    }
    for (int fault = 0; fault != 19; ++fault) {
        reset(); VehicleController self; tk::IdentityReloadOnce once;
        switch (fault) {
            case 0: image_state = ESP_OTA_IMG_NEW; break;
            case 1: image_state = ESP_OTA_IMG_PENDING_VERIFY; break;
            case 2: image_state = ESP_OTA_IMG_INVALID; break;
            case 3: image_state = ESP_OTA_IMG_ABORTED; break;
            case 4: query_ok = false; break;
            case 5: partition_ok = false; break;
            case 6: marker_present = false; break;
            case 7: storage_ok = false; break;
            case 8: storage_throws = true; break;
            case 9: self.vin_transition_pending_ = true; break;
            case 10: self.pairing_cleanup_pending_ = true; break;
            case 11: self.key_rotation_recovered_at_boot_ = true; break;
            case 12: self.automatic_key_action_ = tk::AutomaticKeyAction::RebootRequired; break;
            case 13: query_throws = true; break;
            case 14: self.key_reload_required_ = false; break;
            case 15: new (&tk::s_runtime_admission) tk::RuntimeAdmissionGate; break;
            case 16: tk::runtime_admission_mark_fatal(tk::RuntimeFatalComponent::Nvs); break;
            case 17: self.storage_ = nullptr; break;
            case 18: self.command_mutex_ = nullptr; break;
        }
        CHECK(!invoke(once, self) && !once.reserved() && restarts == 0);
        CHECK(s_operation_gate.state() == Owner::Idle && !command_semaphore.held);
    }
    for (Owner winner : {Owner::Ota, Owner::IdentityMutation, Owner::FaultRestart,
                         Owner::HealthCommit, Owner::ConfigRestart}) {
        reset(); VehicleController self; tk::IdentityReloadOnce once;
        CHECK(s_operation_gate.try_begin(winner));
        CHECK(!invoke(once, self) && !once.reserved() && queries == 0 && marker_reads == 0);
        CHECK(s_operation_gate.state() == winner && restarts == 0);
    }
    reset(); VehicleController self; tk::IdentityReloadOnce once;
    lock_ok = false; CHECK(!invoke(once, self) && queries == 0 && marker_reads == 0);
    CHECK(!once.reserved() && s_operation_gate.state() == Owner::Idle);
    lock_ok = true; CHECK(invoke(once, self) && restarts == 1);
    for (int fault = 0; fault != 3; ++fault) {
        reset(); VehicleController candidate; tk::IdentityReloadOnce single;
        log_throws = fault == 0; restart_throws = fault == 1; delay_throws = fault == 2;
        bool parked = false;
        try { actual_reload_caller(&candidate, single); } catch (const Parked&) { parked = true; }
        CHECK(parked && single.reserved() && s_operation_gate.state() == Owner::ConfigRestart);
        CHECK(restarts == unsigned(fault != 2)); CHECK(!invoke(single, candidate));
    }
    // The actual automatic generation wrapper publishes origin on normal return AND unwind.
    for (bool throws : {false, true}) {
        reset(); expect_config_query = false; VehicleController candidate;
        candidate.key_reload_required_ = false; candidate.automatic_key_action_ = tk::AutomaticKeyAction::Generate;
        generation_throws = throws; bool caught = false;
        try { CHECK(!candidate.generate_key()); } catch (const std::bad_alloc&) { caught = true; }
        CHECK(caught == throws && generations == 1 && candidate.key_reload_required_);
        CHECK(candidate.automatic_key_action_ == tk::AutomaticKeyAction::AutomaticCommitReload);
        CHECK(s_operation_gate.state() == Owner::Idle && !command_semaphore.held);
    }
    reset(); expect_config_query = false; VehicleController foreign;
    foreign.key_reload_required_ = false; foreign.automatic_key_action_ = tk::AutomaticKeyAction::Continue; private_present = true;
    CHECK(!foreign.generate_key() && generations == 0);
    CHECK(foreign.automatic_key_action_ != tk::AutomaticKeyAction::AutomaticCommitReload);
    CHECK(tk::decide_initial_key_boot_action(false, true) == tk::InitialKeyBootAction::HaltAfterEmptyRecovery);
    CHECK(tk::decide_initial_key_boot_action(true, true) == tk::InitialKeyBootAction::UseExisting);
    // Every state uses the same pure names in the actual HTTP and MQTT emitters.
    for (auto runtime : {tk::RuntimeAdmissionState::Booting, tk::RuntimeAdmissionState::Ready,
                         tk::RuntimeAdmissionState::SafeMode, tk::RuntimeAdmissionState::Fatal}) {
        for (const char* reason : std::array<const char*, 6>{{nullptr, "key_reload_required", "key_runtime_mismatch",
                                  "key_cleanup_pending", "vin_transition_pending", "key_storage_unavailable"}}) {
            tk::status::Inputs in; in.runtime_state = runtime;
            in.fatal_component = tk::RuntimeFatalComponent::Network; in.pairing_block_reason = reason;
            Emitter http; tk::status::emit_status(in, http);
            tk::mqtt::DevicePayload device; device.version = "fixture"; device.reset_reason = "sw";
            device.runtime = tk::RuntimeAdmissionSnapshot{runtime, tk::RuntimeFatalComponent::Network};
            device.pairing_block_reason = reason;
            const auto mqtt = tk::mqtt::build_device_payload(device);
            CHECK(http.fields.at("sys.runtime_state") == mqtt.at("runtime_state"));
            CHECK(http.fields.count("identity_state") == 1 && mqtt.count("identity_state") == 1);
            CHECK(http.fields.at("identity_state") == mqtt.at("identity_state"));
            CHECK(http.fields.at("identity_state") == tk::identity_recovery_state_name(tk::identity_recovery_state(reason, runtime)));
            CHECK((http.fields.count("sys.fatal_component") != 0) == (runtime == tk::RuntimeAdmissionState::Fatal));
            CHECK((mqtt.count("fatal_component") != 0) == (runtime == tk::RuntimeAdmissionState::Fatal));
            if (runtime == tk::RuntimeAdmissionState::Fatal) CHECK(mqtt.at("fatal_component") == "network");
        }
    }
    std::cout << "identity recovery actual-body checks " << checks << '\n';
}
