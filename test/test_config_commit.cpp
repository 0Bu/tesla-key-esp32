// Actual shipped configuration owner/query/confirmation and portal shells, with passive host I/O.
// The linked C fixture contains pinned SDK CRC/query bodies; no board/flash/vehicle is contacted.
#include "stack_watch.hpp"
#include "config_commit_sdk_bridge.h"
#include "logic/health_gate.hpp"
#include "ota_recovery_memory.hpp"
#include "logic/config_commit.hpp"
#include "logic/effect_reply.hpp"
#include "logic/heap_watchdog.hpp"
#include "logic/config_store.hpp"
#include "logic/http_body.hpp"
#include "logic/wifi_credentials.hpp"
#include "logic/vin.hpp"
#include "logic/runtime_admission.hpp"
#include <vector>
#include <array>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

static int checks, timed_fixture_runs, config_fixture_runs;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << "FAIL " << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
// Passive own-task SDK I/O only: the real stack_watch.cpp owns encoding/CAS/minimum logic.
// The host stub ABI is UBaseType_t=unsigned, TaskHandle_t=void*; no SDK/target ABI is claimed.
static unsigned f135_hwm_queries = 0;
extern "C" unsigned uxTaskGetStackHighWaterMark(void* task) {
    CHECK(task == nullptr); ++f135_hwm_queries; return 4096;
}
using Image = tk::ConfigImageResult;
using Owner = tk::OtaIdentityGateState;
#include "config_commit_owner.generated.hpp"
static tk::OtaIdentityOperationGate s_operation_gate;
// Passive old-operation cleanup inputs; the entire production query body is generated below.
// The separate OTA application family executes it against real OtaOperation/Coordinator/Slot.
static bool reference_present=false,reference_done=true,reference_reclaimable=true,reference_slot=true;
static unsigned reference_loads=0,reference_done_reads=0,reference_cleanup_reads=0,reference_slot_reads=0;
static bool reference_owner(){
 const auto o=s_operation_gate.state();
 return o==Owner::HealthCommit||o==Owner::ConfigRestart||o==Owner::Ota;
}
struct ReferenceJob {};
struct ReferenceDone {
 bool load(std::memory_order order)const noexcept {
  CHECK(reference_owner()&&order==std::memory_order_acquire);++reference_done_reads;return reference_done;
 }
};
struct ReferenceCoordinator {
 bool can_reclaim()const noexcept {
  ++reference_cleanup_reads;CHECK(reference_owner()&&reference_done);return reference_reclaimable;
 }
};
struct ReferenceOperation {ReferenceDone coordinator_done;ReferenceCoordinator coordinator;ReferenceJob job;};
static ReferenceOperation reference_operation;
struct ReferencePointer {
 ReferenceOperation* load(std::memory_order order)const noexcept {
  ++reference_loads;CHECK(reference_owner()&&order==std::memory_order_acquire);
  return reference_present?&reference_operation:nullptr;
 }
};
static ReferencePointer s_operation;
struct ReferenceSlot {
 bool owns(const ReferenceJob&)const noexcept {++reference_slot_reads;CHECK(reference_owner());return reference_slot;}
};
static ReferenceSlot s_network_slot;
static esp_ota_select_entry_t headers[2];
static esp_partition_t running{ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN};
static bool partition_missing, read_failure, ready, synced, throw_log;
static unsigned resets;
static size_t largest;
static size_t recovery_required=tk::kOtaRecoveryEntryRequiredB, post_owner_largest=tk::kOtaRecoveryEntryRequiredB-1;
namespace tk {
std::size_t ota_recovery_required_bytes(OtaRecoveryUse use) noexcept {
    return ota_recovery_required_bytes_for(use,{recovery_required,kOtaTlsRecordCapacityB});
}
}
static int app_count, reads, marks, logs, mark_failure;
static bool mark_throw, throw_after_mark;
static Owner expected_mark_owner = Owner::ConfigRestart;
static int refreshes, polls, max_polls = 121, post_refusal = -1;
static uint32_t ticks;
static bool timed_fixture = false, network_up = true;
static int wait_polls = 0, throw_health_field = -1;
static uint32_t refreshed_raw_state;
static tk::RuntimeAdmissionAction admission_action = tk::RuntimeAdmissionAction::Run;
static std::vector<std::pair<char, Owner>> health_reads;
static std::vector<std::string> messages;
static bool post_fault(int field) {
    const auto owner = s_operation_gate.state();
    if ((owner == Owner::HealthCommit || owner == Owner::ConfigRestart) && throw_health_field == field)
        throw std::bad_alloc();
    return owner == Owner::HealthCommit && post_refusal == field;
}

static const char* TAG="offline-config";
extern "C" const esp_partition_t* read_otadata(esp_ota_select_entry_t* out) {
    ++reads; if (read_failure) return nullptr;
    std::memcpy(out, headers, sizeof(headers)); return &running;
}
extern "C" int esp_ota_get_app_partition_count(void) { return app_count; }
extern "C" bool is_ota_partition(const esp_partition_t* p) {
    return p->type==ESP_PARTITION_TYPE_APP && p->subtype>=ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
           p->subtype<ESP_PARTITION_SUBTYPE_APP_OTA_MIN+16;
}
static const esp_partition_t* esp_ota_get_running_partition() { return partition_missing?nullptr:&running; }
namespace tk {
static bool runtime_admission_vehicle_ready() {
    health_reads.emplace_back('R', s_operation_gate.state()); return ready && !post_fault(0);
}
static RuntimeAdmissionAction runtime_admission_action() { return polls <= wait_polls ? RuntimeAdmissionAction::Wait : admission_action; }
static bool net_is_up() { return network_up; }
static const char* net_link_str(int) { return "test"; }
static int net_kind() { return 0; }
}
static bool ble_host_synced() {
    health_reads.emplace_back('S', s_operation_gate.state()); return synced && !post_fault(1);
}
static unsigned ble_host_reset_count() {
    health_reads.emplace_back('C', s_operation_gate.state()); return post_fault(2) ? 1 : resets;
}
constexpr int MALLOC_CAP_8BIT=1, MALLOC_CAP_INTERNAL=2;
static size_t heap_caps_get_largest_free_block(int caps) {
    CHECK(caps==(MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL));
    health_reads.emplace_back('H', s_operation_gate.state()); return post_fault(3) ? post_owner_largest : largest;
}
static int esp_ota_mark_app_valid_cancel_rollback() {
    ++marks; CHECK(s_operation_gate.state()==expected_mark_owner);
    if (mark_failure==1) return ESP_FAIL;
    headers[0].ota_state=ESP_OTA_IMG_VALID; // model a durable effect before error/throw
    if (mark_throw) throw std::bad_alloc();
    return mark_failure==2?ESP_FAIL:ESP_OK;
}
template<typename... Args> static void offline_log(const char*, const char* format, Args... args) {
    ++logs;
    if (timed_fixture && (std::strstr(format, "OTA image healthy") || std::strstr(format, "OTA mark-valid failed")))
        CHECK(refreshes == 1 && s_operation_gate.state() == Owner::HealthCommit);
    char text[1024];
    if constexpr (sizeof...(args)) std::snprintf(text, sizeof(text), format, args...);
    else std::snprintf(text, sizeof(text), "%s", format);
    messages.emplace_back(text);
    if (throw_log || (throw_after_mark && marks)) throw std::bad_alloc();
}
#define ESP_LOGI(...) offline_log(__VA_ARGS__)
#define ESP_LOGE(...) offline_log(__VA_ARGS__)
// Refresh is a passive spy; separate admission/cache tests execute its actual body.
static tk::OtaVerificationState ota_refresh_verification_cache() noexcept {
    ++refreshes; CHECK(s_operation_gate.state()==expected_mark_owner);
    refreshed_raw_state=headers[0].ota_state;
    return tk::OtaVerificationState::Unknown;
}
using TickType_t = uint32_t;
#define pdMS_TO_TICKS(x) (x)
#define pdTICKS_TO_MS(x) (x)
struct TaskEnd {};
static TickType_t xTaskGetTickCount() { return ticks; }
static void vTaskDelay(uint32_t delay) {
    CHECK(timed_fixture && delay == 5000); ticks += delay;
    if (++polls > max_polls) throw TaskEnd{};
}
static void vTaskDelete(void*) { throw TaskEnd{}; }
static const char* esp_err_to_name(esp_err_t code) { CHECK(code == ESP_FAIL); return "ESP_FAIL"; }
static bool s_ota_gate_link_expected = true;

#include "config_commit_runtime.generated.hpp"
#include "ota_health_task.generated.hpp"

static void reset_state(uint32_t state=ESP_OTA_IMG_PENDING_VERIFY) {
    CHECK(s_operation_gate.state()==Owner::Idle);
    std::memset(headers,0xff,sizeof(headers));
    headers[0].ota_seq=1; headers[0].ota_state=state;
    headers[0].crc=bootloader_common_ota_select_crc(&headers[0]);
    running={ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_MIN};
    partition_missing=false; read_failure=false; ready=true; synced=true; throw_log=false;
    recovery_required=tk::kOtaRecoveryEntryRequiredB; post_owner_largest=recovery_required-1;
    reference_present=false;reference_done=reference_reclaimable=reference_slot=true;
    reference_loads=reference_done_reads=reference_cleanup_reads=reference_slot_reads=0;
    resets=0; largest=recovery_required; app_count=2; reads=marks=logs=mark_failure=0; mark_throw=false;
    expected_mark_owner=Owner::ConfigRestart; refreshes=polls=0; max_polls=121; post_refusal=-1;
    throw_after_mark=false; network_up=true; wait_polls=0; throw_health_field=-1; refreshed_raw_state=99;
    ticks=0; timed_fixture=false; admission_action=tk::RuntimeAdmissionAction::Run;
    health_reads.clear(); messages.clear();

}
static Image confirm(const OtaConfigRestartGuard& guard) {
    return ota_confirm_pending_image(guard,tk::OtaRebootClass::SuccessfulUserConfigCommit);
}
static void timed_health_tests() {
    ++timed_fixture_runs;
    for (int failure = 0; failure != 3; ++failure) {
        reset_state(); timed_fixture=true; expected_mark_owner=Owner::HealthCommit;
        mark_failure=failure;
        try { ota_health_gate_task(nullptr); } catch (const TaskEnd&) {}
        CHECK(marks==1 && refreshes==1 && polls==18);
        CHECK(refreshed_raw_state==headers[0].ota_state);
        CHECK(s_operation_gate.state()==Owner::Idle);
        std::string owned;
        for (auto read : health_reads) if (read.second==Owner::HealthCommit) owned += read.first;
        CHECK(owned=="RSCH");
        if (failure) {
            CHECK(std::any_of(messages.begin(),messages.end(),[](const std::string& line) {
                return line.find("OTA mark-valid failed: ESP_FAIL (-1); verification is uncertain")!=std::string::npos;
            }));
            CHECK(headers[0].ota_state==(failure==1 ? ESP_OTA_IMG_PENDING_VERIFY : ESP_OTA_IMG_VALID));
        }
    }
    for (int fault=0; fault!=4; ++fault) {
        reset_state(); timed_fixture=true; expected_mark_owner=Owner::HealthCommit;
        if (fault==0) ready=false;
        if (fault==1) synced=false;
        if (fault==2) resets=1;
        if (fault==3) largest=4095;
        try { ota_health_gate_task(nullptr); } catch (const TaskEnd&) {}
        CHECK(marks==0 && refreshes==0 && polls==120 && s_operation_gate.state()==Owner::Idle);
        CHECK(std::all_of(health_reads.begin(),health_reads.end(),[](auto read) { return read.second==Owner::Idle; }));
    }
    for (int fault=0; fault!=4; ++fault) {
        reset_state(); timed_fixture=true; expected_mark_owner=Owner::HealthCommit;
        max_polls=18; post_refusal=fault;
        try { ota_health_gate_task(nullptr); } catch (const TaskEnd&) {}
        CHECK(marks==0 && refreshes==0 && polls==19 && s_operation_gate.state()==Owner::Idle);
        std::string owned;
        for (auto read : health_reads) if (read.second==Owner::HealthCommit) owned += read.first;
        CHECK(owned==std::string("RSCH").substr(0,fault+1));
    }
    for (int failure : {0, 1}) {
        reset_state(); timed_fixture=true; expected_mark_owner=Owner::HealthCommit;
        mark_failure=failure; throw_after_mark=true;
        try { ota_health_gate_task(nullptr); } catch (...) {}
        CHECK(marks==1 && refreshes==1 && s_operation_gate.state()==Owner::Idle);
    }
    reset_state(); timed_fixture=true; expected_mark_owner=Owner::HealthCommit;
    mark_throw=true;
    try { ota_health_gate_task(nullptr); } catch (const TaskEnd&) {}
    CHECK(marks==1 && refreshes==0 && headers[0].ota_state==ESP_OTA_IMG_VALID);
    CHECK(s_operation_gate.state()==Owner::Idle); // exceptional SDK return preserves no refresh promise
    for (auto state : {ESP_OTA_IMG_VALID, ESP_OTA_IMG_UNDEFINED, ESP_OTA_IMG_NEW,
                       ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED}) {
        reset_state(state); timed_fixture=true;
        try { ota_health_gate_task(nullptr); } catch (const TaskEnd&) {}
        CHECK(marks==0 && polls==0 && health_reads.empty());
    }
    reset_state(); timed_fixture=true; read_failure=true;
    try { ota_health_gate_task(nullptr); } catch (const TaskEnd&) {}
    CHECK(marks==0 && polls==0 && health_reads.empty());
    reset_state(); timed_fixture=true; wait_polls=3; admission_action=tk::RuntimeAdmissionAction::Stop;
    try { ota_health_gate_task(nullptr); } catch (const TaskEnd&) {}
    CHECK(polls==4 && marks==0 && health_reads.empty());
    reset_state(); timed_fixture=true; network_up=false;
    try { ota_health_gate_task(nullptr); } catch (const TaskEnd&) {}
    CHECK(polls==120 && marks==0 && refreshes==0);
    CHECK(std::any_of(messages.begin(),messages.end(),[](const std::string& line) {
        return line.find("never proved link")!=std::string::npos;
    }));
    for (auto owner : {Owner::Ota,Owner::IdentityMutation,Owner::FaultRestart,Owner::HealthCommit,Owner::ConfigRestart}) {
        reset_state(); timed_fixture=true; expected_mark_owner=Owner::HealthCommit; max_polls=18;
        CHECK(s_operation_gate.try_begin(owner));
        try { ota_health_gate_task(nullptr); } catch (const TaskEnd&) {}
        CHECK(polls==19 && marks==0 && refreshes==0 && s_operation_gate.state()==owner);
        CHECK(s_operation_gate.finish(owner));
    }
    for (int field=0; field!=4; ++field) {
        reset_state(); timed_fixture=true; expected_mark_owner=Owner::HealthCommit; throw_health_field=field;
        try { ota_health_gate_task(nullptr); } catch (const TaskEnd&) {}
        CHECK(marks==0 && refreshes==0 && polls==18 && s_operation_gate.state()==Owner::Idle);
    }
}

static void recovery_policy_tests() {
    // Literal12345 is a host-only oracle between both historical hardcodings.
    for (size_t observed : {size_t(12344), size_t(12345), size_t(12346)}) {
        const bool affordable=observed==12345 || observed==12346;
        reset_state(); recovery_required=12345; largest=observed;
        { OtaConfigRestartGuard guard; CHECK(guard.try_begin());
          CHECK(confirm(guard)==(affordable ? Image::Confirmed : Image::CriticalHeap));
          CHECK(marks==int(affordable) && guard.owns_gate()); }
        CHECK(s_operation_gate.state()==Owner::Idle);
        reset_state(); recovery_required=12345; largest=observed;
        timed_fixture=true; expected_mark_owner=Owner::HealthCommit;
        try { ota_health_gate_task(nullptr); } catch (const TaskEnd&) {}
        CHECK(marks==int(affordable) && refreshes==int(affordable));
        CHECK(polls==(affordable ? 18 : 120) && s_operation_gate.state()==Owner::Idle);
    }
    reset_state(); recovery_required=12345; largest=12345; post_owner_largest=12344;
    timed_fixture=true; expected_mark_owner=Owner::HealthCommit; max_polls=18; post_refusal=3;
    try { ota_health_gate_task(nullptr); } catch (const TaskEnd&) {}
    CHECK(marks==0 && refreshes==0 && polls==19 && s_operation_gate.state()==Owner::Idle);
    std::string owned;
    for (auto read:health_reads) if (read.second==Owner::HealthCommit) owned+=read.first;
    CHECK(owned=="RSCH"); // Fresh lower block only after actual owner acquisition.
    for (Owner winner : {Owner::Ota,Owner::IdentityMutation,Owner::FaultRestart,Owner::HealthCommit}) {
        reset_state(); recovery_required=12345; largest=12346;
        CHECK(s_operation_gate.try_begin(winner)); OtaConfigRestartGuard denied;
        CHECK(!denied.try_begin() && confirm(denied)==Image::WrongOwner);
        CHECK(marks==0 && reads==0 && health_reads.empty()); CHECK(s_operation_gate.finish(winner));
    }
    reset_state(); recovery_required=0; largest=SIZE_MAX;
    { OtaConfigRestartGuard guard; CHECK(guard.try_begin()); CHECK(confirm(guard)==Image::CriticalHeap && marks==0); }
    reset_state(); // Leave every pre-existing default regression unchanged.
}

static void reference_policy_tests(){
 // The unowned advisory sampler must never borrow the old Operation, even when present.
 reset_state();reference_present=true;reference_done=false;
 CHECK(ota_runtime_health().healthy()&&reference_loads==0);
 {OtaHealthCommitGuard denied;CHECK(denied);CHECK(s_operation_gate.state()==Owner::HealthCommit);}
 CHECK(reference_loads==0);
 for(int refusal:{0,1,2}){
  reset_state();reference_present=true;
  reference_done=refusal!=0;reference_reclaimable=refusal!=1;reference_slot=refusal!=2;
  {OtaConfigRestartGuard guard;CHECK(guard.try_begin());CHECK(confirm(guard)==Image::Pending);
   CHECK(marks==0&&reads==1&&health_reads.empty());}
  CHECK(reference_loads==1&&reference_done_reads==1);
  CHECK(reference_cleanup_reads==unsigned(refusal!=0));
  CHECK(reference_slot_reads==unsigned(refusal==2));
 }
 reset_state();reference_present=true;
 {OtaConfigRestartGuard guard;CHECK(guard.try_begin());CHECK(confirm(guard)==Image::Confirmed&&marks==1);}
 CHECK(reference_loads==1&&reference_cleanup_reads==1&&reference_slot_reads==1);
 reset_state();reference_present=true;reference_done=false;
 timed_fixture=true;expected_mark_owner=Owner::HealthCommit;max_polls=18;
 try{ota_health_gate_task(nullptr);}catch(const TaskEnd&){}
 CHECK(marks==0&&refreshes==0&&reference_loads==1&&reference_cleanup_reads==0);
 reset_state();
 {OtaHealthCommitGuard guard;CHECK(guard);
  {OtaHealthCommitGuard denied;CHECK(!denied);
   CHECK(!ota_recovery_reference_ready(denied)&&reference_loads==0);}
  CHECK(ota_recovery_reference_ready(guard));}
 CHECK(reference_loads==1&&reference_done_reads==0);
 reset_state();CHECK(s_operation_gate.try_begin(Owner::FaultRestart));
 CHECK(!ordinary_recovery_reference_ready_owned(Owner::HealthCommit)&&reference_loads==0);
 CHECK(s_operation_gate.finish(Owner::FaultRestart));reset_state();
}

static void sdk_and_owner_tests() {
    ++config_fixture_runs;
    static_assert(sizeof(esp_ota_select_entry_t)==32,"exact public OTA header layout");
    static_assert(ESP_OTA_IMG_UNDEFINED==UINT32_MAX,"raw SDK UNDEFINED is not zero");
    static_assert(std::is_nothrow_default_constructible<OtaConfigRestartGuard>::value &&
                  std::is_nothrow_destructible<OtaConfigRestartGuard>::value &&
                  !std::is_copy_constructible<OtaConfigRestartGuard>::value &&
                  !std::is_move_constructible<OtaConfigRestartGuard>::value,"request owns lease");
    for (auto owner : {Owner::Ota,Owner::IdentityMutation,Owner::FaultRestart,Owner::HealthCommit,Owner::ConfigRestart}) {
        reset_state(); CHECK(s_operation_gate.try_begin(owner));
        { OtaConfigRestartGuard denied; CHECK(!denied.try_begin()); CHECK(!denied.owns_gate());
          CHECK(confirm(denied)==Image::WrongOwner);
          CHECK(ota_config_image_state(denied)==Image::WrongOwner); CHECK(reads==0 && marks==0 && health_reads.empty()); }
        CHECK(s_operation_gate.state()==owner); CHECK(s_operation_gate.finish(owner));
    }
    const uint32_t raw[]={ESP_OTA_IMG_VALID,ESP_OTA_IMG_UNDEFINED,ESP_OTA_IMG_NEW,
        ESP_OTA_IMG_PENDING_VERIFY,ESP_OTA_IMG_INVALID,ESP_OTA_IMG_ABORTED,99};
    const Image expected[]={Image::Valid,Image::Undefined,Image::New,Image::Pending,
        Image::Unsupported,Image::Unsupported,Image::Unsupported};
    for (unsigned i=0;i<sizeof(raw)/sizeof(raw[0]);++i) {
        reset_state(raw[i]);
        { OtaConfigRestartGuard guard; CHECK(guard.try_begin()); CHECK(guard.owns_gate());
          CHECK(ota_config_image_state(guard)==expected[i]);
          for (auto owner : {Owner::Ota,Owner::IdentityMutation,Owner::FaultRestart,Owner::HealthCommit,Owner::ConfigRestart})
              CHECK(!s_operation_gate.try_begin(owner));
          CHECK(confirm(guard)==(raw[i]==ESP_OTA_IMG_PENDING_VERIFY?Image::Confirmed:expected[i]));
          CHECK(marks==int(raw[i]==ESP_OTA_IMG_PENDING_VERIFY));
          CHECK(health_reads.empty() == (raw[i]!=ESP_OTA_IMG_PENDING_VERIFY));
          for (auto read : health_reads) CHECK(read.second==Owner::ConfigRestart); }
        CHECK(s_operation_gate.state()==Owner::Idle);
    }
    // Actual pinned query and CRC reject blank/CRC-invalid storage; no NOT_FOUND->Stable exception.
    for (int fault=0;fault<6;++fault) {
        reset_state();
        if (fault==0) std::memset(headers,0xff,sizeof(headers));
        if (fault==1) headers[0].crc^=1;
        if (fault==2) read_failure=true; // SDK's shared missing/mapping-failure return
        if (fault==3) app_count=0;
        if (fault==4) partition_missing=true;
        if (fault==5) running.subtype=1; // non-OTA running partition
        { OtaConfigRestartGuard guard; CHECK(guard.try_begin());
          CHECK(ota_config_image_state(guard)==Image::Unknown); CHECK(confirm(guard)==Image::Unknown);
          CHECK(marks==0); }
    }
    reset_state(); std::memset(headers,0xff,sizeof(headers));
    CHECK(bootloader_common_ota_select_crc(&headers[0])==0x2144df1cU);
    CHECK(headers[0].crc!=bootloader_common_ota_select_crc(&headers[0]));
    for (int refusal=0;refusal<4;++refusal) {
        reset_state();
        if (refusal==0) ready=false;
        if (refusal==1) synced=false;
        if (refusal==2) resets=1;
        if (refusal==3) largest=4095;
        const Image results[]={Image::NotReady,Image::HostUnsynced,Image::HostReset,Image::CriticalHeap};
        { OtaConfigRestartGuard guard; CHECK(guard.try_begin()); CHECK(confirm(guard)==results[refusal]); CHECK(marks==0); }
    }
    for (auto klass : {tk::OtaRebootClass::AutomaticIdentityRecovery,tk::OtaRebootClass::AutomaticWifiRollback,
                       tk::OtaRebootClass::AutomaticFaultRecovery}) {
        reset_state(); OtaConfigRestartGuard guard; CHECK(guard.try_begin());
        CHECK(ota_confirm_pending_image(guard,klass)==Image::WrongClass); CHECK(reads==0 && marks==0 && health_reads.empty());
    }
    for (int failure=0;failure<3;++failure) {
        reset_state(); mark_failure=failure==0?1:2; mark_throw=failure==2;
        { OtaConfigRestartGuard guard; CHECK(guard.try_begin()); CHECK(confirm(guard)==Image::MarkUnknown);
          CHECK(marks==1); CHECK(headers[0].ota_state==(failure==0?ESP_OTA_IMG_PENDING_VERIFY:ESP_OTA_IMG_VALID)); }
    }
    reset_state(); network_up=false;
    { OtaConfigRestartGuard guard; CHECK(guard.try_begin()); CHECK(confirm(guard)==Image::Confirmed); }
    CHECK(marks==1 && refreshes==1); // user-save confirmation does not add a link requirement
    for (int field=0; field!=4; ++field) {
        reset_state(); throw_health_field=field;
        { OtaConfigRestartGuard guard; CHECK(guard.try_begin()); CHECK(confirm(guard)==Image::Pending); }
        CHECK(marks==0 && refreshes==0 && s_operation_gate.state()==Owner::Idle);
    }
    reset_state(); throw_log=true;
    { OtaConfigRestartGuard guard; CHECK(guard.try_begin()); CHECK(confirm(guard)==Image::Confirmed);
      CHECK(marks==1 && logs==1 && headers[0].ota_state==ESP_OTA_IMG_VALID); }
    CHECK(s_operation_gate.state()==Owner::Idle); // noexcept cleanup cannot log/throw
    reset_state();
    { OtaConfigRestartGuard guard; CHECK(guard.try_begin()); CHECK(s_operation_gate.finish(Owner::ConfigRestart));
      CHECK(s_operation_gate.try_begin(Owner::Ota)); CHECK(confirm(guard)==Image::WrongOwner); }
    CHECK(s_operation_gate.state()==Owner::Ota); CHECK(s_operation_gate.finish(Owner::Ota));
}

struct httpd_req_t { const char* input; int content_len; size_t cursor{}; };
static bool readable_cfg, write_ok, throw_write, receive_failure, throw_restart;
static int writes, restarts, transport_calls, status_attempts, fail_transport;
static std::string wire;
static int response_status;
struct NvsStorageAdapter {};
static NvsStorageAdapter config_store;
static NvsStorageAdapter* g_cfg=&config_store;
static tk::ConfigBlob stored_cfg, written_cfg;
namespace tk {
static bool cfg_load_for_update(NvsStorageAdapter&,ConfigBlob& cfg) {
    if (!readable_cfg) return false;
    cfg=stored_cfg; return true;
}
static bool cfg_save(NvsStorageAdapter&,const ConfigBlob& cfg) {
    ++writes; CHECK(s_operation_gate.state()==Owner::ConfigRestart);
    CHECK(!s_operation_gate.try_begin(Owner::Ota)); written_cfg=cfg;
    if (throw_write) throw std::bad_alloc();
    return write_ok;
}
}
static int transport_step() {
    ++transport_calls;
    if (writes) CHECK(s_operation_gate.state()==Owner::ConfigRestart);
    return transport_calls==fail_transport?-7:ESP_OK;
}
static int httpd_resp_set_status(httpd_req_t*,const char* status) {
    ++status_attempts; response_status=std::atoi(status); return transport_step();
}
static int httpd_resp_set_type(httpd_req_t*,const char*) { return transport_step(); }
static int httpd_resp_send_chunk(httpd_req_t*,const char* value,size_t size) {
    const int result=transport_step();
    if (value) wire.append(value,result?std::min(size,size_t{1}):size);
    return result;
}
static int httpd_resp_sendstr_chunk(httpd_req_t* req,const char* text) {
    return httpd_resp_send_chunk(req,text,std::strlen(text));
}
static int httpd_resp_sendstr(httpd_req_t* req,const char* text) {
    return httpd_resp_send_chunk(req,text,std::strlen(text));
}
constexpr int HTTPD_SOCK_ERR_TIMEOUT=-99, SHUT_RD=0;
static int httpd_req_to_sockfd(httpd_req_t*) { return 0; }
static int shutdown(int,int) { return 0; }
static int httpd_req_recv(httpd_req_t* req,char* out,size_t count) {
    if (receive_failure) return -1;
    count=std::min({count,size_t{7},size_t(req->content_len)-req->cursor});
    std::memcpy(out,req->input+req->cursor,count); req->cursor+=count; return int(count);
}
static int64_t esp_timer_get_time() { static int64_t tick; return ++tick*1000; }
#define pdMS_TO_TICKS(x) (x)
static void vTaskDelay(int) { CHECK(s_operation_gate.state()==Owner::ConfigRestart); }
static void esp_restart() {
    ++restarts; CHECK(s_operation_gate.state()==Owner::ConfigRestart);
    if (throw_restart) throw std::bad_alloc();
}
#include "config_portal.generated.hpp"

static void reset_portal(uint32_t state=ESP_OTA_IMG_VALID) {
    reset_state(state); readable_cfg=true; write_ok=true; throw_write=false;
    receive_failure=false; throw_restart=false; writes=restarts=transport_calls=status_attempts=0;
    fail_transport=0; response_status=0; wire.clear(); stored_cfg={}; written_cfg={};
}
static httpd_req_t portal_request(const char* body="ssid=offline&pass=&vin=") {
    return {body, int(std::strlen(body)),0};
}
static void portal_tests() {
    static_assert(noexcept(portal_save_continue(std::declval<PortalSaveContext&>())),
                  "continuation outside outer catch must be noexcept");
    for (auto owner : {Owner::Ota,Owner::IdentityMutation,Owner::FaultRestart,Owner::HealthCommit,Owner::ConfigRestart}) {
        reset_portal(); CHECK(s_operation_gate.try_begin(owner)); auto req=portal_request();
        CHECK(save_post(&req)==ESP_OK); CHECK(response_status==409);
        CHECK(writes==0 && restarts==0 && marks==0);
        CHECK(s_operation_gate.state()==owner); CHECK(s_operation_gate.finish(owner));
    }
    for (auto state : {ESP_OTA_IMG_VALID,ESP_OTA_IMG_UNDEFINED,ESP_OTA_IMG_PENDING_VERIFY,ESP_OTA_IMG_NEW}) {
        reset_portal(state); auto req=portal_request(); CHECK(save_post(&req)==ESP_OK);
        CHECK(response_status==200 && writes==1 && restarts==1 && marks==0);
        CHECK(written_cfg.wifi_ssid=="offline" && written_cfg.wifi_pass.empty());
        CHECK(wire.find("Saved")!=std::string::npos);
        CHECK((wire.find("id=firmware-warning")!=std::string::npos)==
              (state==ESP_OTA_IMG_PENDING_VERIFY||state==ESP_OTA_IMG_NEW));
        CHECK(s_operation_gate.state()==Owner::Idle);
    }
    for (bool failed_write : {false,true}) {
        reset_portal(); write_ok=false; throw_write=failed_write; auto req=portal_request();
        CHECK(save_post(&req)==ESP_OK); CHECK(response_status==500);
        CHECK(writes==1 && marks==0 && restarts==0); CHECK(wire.find("outcome unknown")!=std::string::npos);
        CHECK(s_operation_gate.state()==Owner::Idle);
    }
    reset_portal(); throw_log=true; auto req=portal_request();
    CHECK(save_post(&req)==ESP_OK); CHECK(writes==1 && restarts==1 && marks==0);
    CHECK(wire.find("Saved")!=std::string::npos); CHECK(status_attempts==1);
    CHECK(s_operation_gate.state()==Owner::Idle); // outer catch retains actual lease through fallback/restart
    for (int failed=1;failed<=7;++failed) {
        reset_portal(); fail_transport=failed; req=portal_request();
        CHECK(save_post(&req)==-7); CHECK(writes==1 && restarts==1 && marks==0);
        CHECK(status_attempts==1 && transport_calls==failed); CHECK(s_operation_gate.state()==Owner::Idle);
    }
    reset_portal(); read_failure=true; req=portal_request();
    CHECK(save_post(&req)==ESP_OK); CHECK(writes==1 && restarts==1 && marks==0);
    CHECK(wire.find("verification is uncertain")!=std::string::npos);
    reset_portal(); throw_restart=true; req=portal_request();
    CHECK(save_post(&req)==ESP_OK); CHECK(restarts==1); CHECK(s_operation_gate.state()==Owner::Idle);
    reset_portal();
    { PortalSaveContext context; req=portal_request(); CHECK(save_post_impl(&req,context)==ESP_OK);
      portal_save_continue(context); portal_save_continue(context); CHECK(restarts==1);
      CHECK(s_operation_gate.state()==Owner::ConfigRestart); }
    CHECK(s_operation_gate.state()==Owner::Idle);
    for (const char* input : {"", "ssid=bad%00network&pass=&vin=", "ssid=offline&pass=short&vin="}) {
        reset_portal(); req=portal_request(input); (void)save_post(&req);
        CHECK(writes==0 && restarts==0 && marks==0); CHECK(s_operation_gate.state()==Owner::Idle);
    }
    reset_portal(); readable_cfg=false; req=portal_request(); CHECK(save_post(&req)==ESP_OK);
    CHECK(response_status==503 && writes==0 && restarts==0);
    reset_portal(); stored_cfg.vin="5YJ3E1EA1JF000001";
    req=portal_request("ssid=offline&pass=&vin=5YJ3E1EA1JF000002"); CHECK(save_post(&req)==ESP_OK);
    CHECK(response_status==409 && writes==0 && restarts==0);
}

int main() {
    CHECK(!tk::stack_watch_min_free_bytes(tk::StackWatch::OtaGate).has_value());
    CHECK(!tk::stack_watch_min_free_bytes(tk::StackWatch::CaptiveHttp).has_value());
    sdk_and_owner_tests();
    timed_health_tests();
    CHECK(config_fixture_runs==1 && timed_fixture_runs==1);
    portal_tests();
    CHECK(f135_hwm_queries > 0);
    CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::OtaGate) == 4096u);
    CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::CaptiveHttp) == 4096u);
    recovery_policy_tests();
    reference_policy_tests();
    std::cout << "OK " << checks << " actual config/timed-health owner/SDK-query/confirmation/portal checks\n";
}
