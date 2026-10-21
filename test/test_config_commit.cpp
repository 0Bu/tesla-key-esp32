// Actual shipped configuration owner/query/confirmation and portal shells, with passive host I/O.
// The linked C fixture contains pinned SDK CRC/query bodies; no board/flash/vehicle is contacted.
#include "config_commit_sdk_bridge.h"
#include "logic/health_gate.hpp"
#include "logic/config_commit.hpp"
#include "logic/effect_reply.hpp"
#include "logic/heap_watchdog.hpp"
#include "logic/config_store.hpp"
#include "logic/http_body.hpp"
#include "logic/wifi_credentials.hpp"
#include "logic/vin.hpp"
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

static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << "FAIL " << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (0)
using Image = tk::ConfigImageResult;
using Owner = tk::OtaIdentityGateState;
#include "config_commit_owner.generated.hpp"
static tk::OtaIdentityOperationGate s_operation_gate;
static esp_ota_select_entry_t headers[2];
static esp_partition_t running{ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN};
static bool partition_missing, read_failure, ready, synced, throw_log;
static unsigned resets;
static size_t largest;
static int app_count, reads, marks, logs, mark_failure;
static bool mark_throw;
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
namespace tk { static bool runtime_admission_vehicle_ready() { return ready; } }
static bool ble_host_synced() { return synced; }
static unsigned ble_host_reset_count() { return resets; }
constexpr int MALLOC_CAP_8BIT=1, MALLOC_CAP_INTERNAL=2;
static size_t heap_caps_get_largest_free_block(int caps) {
    CHECK(caps==(MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL)); return largest;
}
static int esp_ota_mark_app_valid_cancel_rollback() {
    ++marks; CHECK(s_operation_gate.state()==Owner::ConfigRestart);
    if (mark_failure==1) return ESP_FAIL;
    headers[0].ota_state=ESP_OTA_IMG_VALID; // model a durable effect before error/throw
    if (mark_throw) throw std::bad_alloc();
    return mark_failure==2?ESP_FAIL:ESP_OK;
}
template<typename... Args> static void offline_log(Args...) {
    ++logs; if (throw_log) throw std::bad_alloc();
}
#define ESP_LOGI(...) offline_log(__VA_ARGS__)
#include "config_commit_runtime.generated.hpp"

static void reset_state(uint32_t state=ESP_OTA_IMG_PENDING_VERIFY) {
    CHECK(s_operation_gate.state()==Owner::Idle);
    std::memset(headers,0xff,sizeof(headers));
    headers[0].ota_seq=1; headers[0].ota_state=state;
    headers[0].crc=bootloader_common_ota_select_crc(&headers[0]);
    running={ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_MIN};
    partition_missing=false; read_failure=false; ready=true; synced=true; throw_log=false;
    resets=0; largest=4096; app_count=2; reads=marks=logs=mark_failure=0; mark_throw=false;
}
static Image confirm(const OtaConfigRestartGuard& guard) {
    return ota_confirm_pending_image(guard,tk::OtaRebootClass::SuccessfulUserConfigCommit);
}
static void sdk_and_owner_tests() {
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
          CHECK(ota_config_image_state(denied)==Image::WrongOwner); CHECK(reads==0 && marks==0); }
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
          CHECK(marks==int(raw[i]==ESP_OTA_IMG_PENDING_VERIFY)); }
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
        CHECK(ota_confirm_pending_image(guard,klass)==Image::WrongClass); CHECK(reads==0 && marks==0);
    }
    for (int failure=0;failure<3;++failure) {
        reset_state(); mark_failure=failure==0?1:2; mark_throw=failure==2;
        { OtaConfigRestartGuard guard; CHECK(guard.try_begin()); CHECK(confirm(guard)==Image::MarkUnknown);
          CHECK(marks==1); CHECK(headers[0].ota_state==(failure==0?ESP_OTA_IMG_PENDING_VERIFY:ESP_OTA_IMG_VALID)); }
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
    sdk_and_owner_tests();
    portal_tests();
    std::cout << "OK " << checks << " actual config owner/SDK-query/confirmation/portal checks\n";
}
