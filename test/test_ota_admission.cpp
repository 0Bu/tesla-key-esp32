// Actual OTA startup/cache bodies with passive SDK/RTOS counters. No flash, HTTP or worker I/O.
#include "logic/health_gate.hpp"
#include "logic/ota_contract.hpp"
#include "task_config.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <cstring>

namespace {
int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << "OTA admission " << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
using State = tk::OtaIdentityGateState;
using Verification = tk::OtaVerificationState;
using Result = tk::OtaStartResult;
tk::OtaIdentityOperationGate s_operation_gate;
std::atomic<bool> s_running{false}, s_updating{false};
std::atomic<unsigned> s_target_pr{0};
std::atomic<Verification> s_verification{Verification::Unknown};
bool lock_available, partition_available, query_ok, query_throws, task_ok, state_throws;
int queries, partition_queries, tasks, errors, publications;
enum esp_ota_img_states_t : unsigned { ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY,
    ESP_OTA_IMG_VALID, ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED=~0u };
esp_ota_img_states_t sdk_state;
struct esp_partition_t {};
esp_partition_t partition;
constexpr int ESP_OK=0, pdPASS=1;
enum class OtaState { Unseen, Checking, Downloading, Error };
OtaState published_state;
using Task = void(*)(void*);
Task selected_task;
unsigned selected_stack, selected_priority;
void* selected_parameter;
void ota_check_task(void*) {}
void ota_task(void*) {}
bool ensure_lock() { return lock_available; }
bool try_begin_ota_operation() { return s_operation_gate.try_begin(State::Ota); }
void finish_operation(State owner) { CHECK(s_operation_gate.finish(owner)); }
const esp_partition_t* esp_ota_get_running_partition() {
    ++partition_queries;
    return partition_available ? &partition : nullptr;
}
int esp_ota_get_state_partition(const esp_partition_t* p, esp_ota_img_states_t* state) {
    CHECK(p==&partition); ++queries;
    if (query_throws) throw std::bad_alloc();
    *state=sdk_state;
    return query_ok ? ESP_OK : -1;
}
void set_state(OtaState state, int, const char*) {
    ++publications;
    CHECK(s_operation_gate.state()==State::Ota && s_running);
    if (state_throws) throw std::bad_alloc();
    published_state=state;
}
void set_check_error(const char*) noexcept {
    CHECK(s_operation_gate.state()==State::Ota && s_running);
    ++errors; ++publications; published_state=OtaState::Error;
}
int xTaskCreate(Task task, const char* name, unsigned stack, void* parameter, unsigned priority, void*) {
    CHECK(s_operation_gate.state()==State::Ota && s_running);
    CHECK(std::strcmp(name,task==ota_task?"ota":"ota_chk")==0);
    ++tasks; selected_task=task; selected_stack=stack; selected_priority=priority;
    selected_parameter=parameter;
    return task_ok ? pdPASS : 0;
}

#include "ota_admission_adapter.generated.hpp"

void reset() {
    CHECK(s_operation_gate.state()==State::Idle);
    s_running=false; s_updating=false; s_target_pr=0; s_verification=Verification::Unknown;
    lock_available=partition_available=query_ok=task_ok=true; query_throws=state_throws=false;
    queries=partition_queries=tasks=errors=publications=0; sdk_state=ESP_OTA_IMG_VALID;
    published_state=OtaState::Unseen;
    selected_task=nullptr; selected_stack=selected_priority=0; selected_parameter=nullptr;
}
void check_clean() {
    CHECK(s_operation_gate.state()==State::Idle && !s_running && !s_updating);
    CHECK(tasks==0 && publications==0);
}
void finish_worker() {
    CHECK(s_operation_gate.finish(State::Ota)); s_running=false; s_updating=false;
}
}

int test_ota_admission() {
    for (auto start : {ota_check_start,ota_start}) {
        reset(); lock_available=false;
        CHECK(start(400)==Result::Unavailable); check_clean(); CHECK(partition_queries==0);
        for (auto owner : {State::Ota,State::IdentityMutation,State::FaultRestart,State::HealthCommit,State::ConfigRestart}) {
            reset(); CHECK(s_operation_gate.try_begin(owner));
            CHECK(start(400)==Result::Busy && s_operation_gate.state()==owner);
            CHECK(tasks==0 && partition_queries==0 && !s_running);
            CHECK(s_operation_gate.finish(owner));
        }
        reset(); s_running=true;
        CHECK(start(400)==Result::Busy && s_running && !s_updating);
        CHECK(s_operation_gate.state()==State::Idle && tasks==0 && publications==0);
        s_running=false;
        for (bool publish_fault : {false,true}) {
            reset(); task_ok=false; state_throws=publish_fault;
            CHECK(start(400)==Result::InternalError);
            CHECK(!s_running && !s_updating && s_operation_gate.state()==State::Idle);
            CHECK(tasks==int(!publish_fault));
            CHECK(publications==(publish_fault?1:2));
            CHECK(errors==int(!publish_fault && start==ota_check_start));
            CHECK(published_state==(publish_fault?OtaState::Unseen:OtaState::Error));
            state_throws=false; task_ok=true;
            CHECK(start(401)==Result::Started); CHECK(s_target_pr==401);
            CHECK(selected_task==(start==ota_start?ota_task:ota_check_task));
            CHECK(selected_stack==8192 && selected_priority==(start==ota_start?tk::kPrioOta:tk::kPrioOtaCheck));
            CHECK(reinterpret_cast<uintptr_t>(selected_parameter)==401);
            CHECK(s_updating==(start==ota_start)); finish_worker();
        }
    }
    for (auto state : {ESP_OTA_IMG_NEW,ESP_OTA_IMG_PENDING_VERIFY}) {
        reset(); sdk_state=state;
        CHECK(ota_start(400)==Result::PendingVerification); check_clean();
        CHECK(queries==1 && ota_cached_verification_state()==Verification::PendingVerify);
        CHECK(queries==1); // cache getter must never perform another SDK query
        CHECK(ota_check_start(400)==Result::Started && queries==1); finish_worker();
    }
    for (auto state : {ESP_OTA_IMG_INVALID,ESP_OTA_IMG_ABORTED,static_cast<esp_ota_img_states_t>(17)}) {
        reset(); sdk_state=state;
        CHECK(ota_start(400)==Result::Unavailable); check_clean();
        CHECK(ota_cached_verification_state()==Verification::Unknown && queries==1);
    }
    for (int fault=0; fault<3; ++fault) {
        reset(); partition_available=fault!=0; query_ok=fault!=1; query_throws=fault==2;
        CHECK(ota_start(400)==Result::Unavailable); check_clean();
        CHECK(ota_cached_verification_state()==Verification::Unknown);
        CHECK(queries==int(fault!=0));
    }
    for (auto state : {ESP_OTA_IMG_VALID,ESP_OTA_IMG_UNDEFINED}) {
        reset(); sdk_state=state;
        CHECK(ota_start(0)==Result::Started && tasks==1 && queries==1);
        CHECK(s_operation_gate.state()==State::Ota && s_running && s_updating);
        CHECK(ota_cached_verification_state()==Verification::Stable && queries==1); finish_worker();
    }
    reset(); sdk_state=ESP_OTA_IMG_PENDING_VERIFY;
    CHECK(ota_refresh_verification_cache()==Verification::PendingVerify && queries==1);
    sdk_state=ESP_OTA_IMG_VALID;
    CHECK(ota_cached_verification_state()==Verification::PendingVerify && queries==1);
    CHECK(ota_start(0)==Result::Started && queries==2); // admission must not trust stale cache
    CHECK(ota_cached_verification_state()==Verification::Stable); finish_worker();
    reset(); CHECK(ota_refresh_verification_cache()==Verification::Stable);
    sdk_state=ESP_OTA_IMG_PENDING_VERIFY;
    CHECK(ota_start(0)==Result::PendingVerification && tasks==0 && queries==2); check_clean();
    reset();
    std::cout << "OTA admission/cache checks " << checks << '\n';
    return checks;
}
