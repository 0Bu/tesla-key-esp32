// Complete shipped OTA status/start/check/install family plus real parser/coordinator/worker/raw
// adapter. Only SDK/RTOS/clock/TLS/flash effects are passive doubles. No target ABI/TLS/crypto proof.
#include "ota_update.hpp"
#include "ota_recovery_memory.hpp"
#include "ota_writer.hpp"
#include "ota_manifest.hpp"
#include "logic/json_syntax.hpp"
#include "logic/ota_changelog_range.hpp"
#include "logic/tls_memory.hpp"
#include "rtos_guard.hpp"
#include "task_config.hpp"
#include "stack_watch.hpp"
#include "freertos/task.h"
#include <esp_tls.h>
#include <esp_crt_bundle.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <memory>
#include <string_view>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

static std::atomic<bool> fail_body_reserve{false};
void* operator new(std::size_t n) {
    if (n>=8192 && n<16384 && fail_body_reserve.exchange(false)) throw std::bad_alloc();
    if (void* p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
// Pair scalar nothrow allocation with this fixture's malloc/free replacement family.
void* operator new(std::size_t n,const std::nothrow_t&) noexcept {
    try { return ::operator new(n); }
    catch (...) { return nullptr; }
}
void operator delete(void* p,const std::nothrow_t&) noexcept {::operator delete(p);}
namespace fixture {
std::atomic<int> checks{0};
[[noreturn]] void failed(int line,const char* expression){
 // Deliberate negative fixtures terminate their owned process without a joinable-thread
 // global destructor replacing the first CHECK exit. Positive normal execution joins/reclaims.
 std::cerr<<"OTA application "<<line<<": "<<expression<<'\n'<<std::flush;std::_Exit(1);
}
#define CHECK(x) do { ++fixture::checks; if(!(x))fixture::failed(__LINE__,#x); } while(false)
std::atomic<uint64_t> now{0};
thread_local bool network_thread=false;
std::thread worker;
std::mutex connection_mutex;
std::condition_variable connection_ready;
bool hold_connect=false,release_connect=false,connect_entered=false;
bool hold_image_read=false,image_read_entered=false;
std::atomic<int> init_calls{0},connects{0},destroys{0},reads{0},deletes{0};
std::atomic<int> begins{0},writes{0},ends{0},selectors{0},aborts{0},restart_calls{0};
std::vector<size_t> largest_samples;
size_t largest_cursor=0;
size_t recovery_required=tk::kOtaTlsRecordCapacityB;
size_t entry_required=tk::kOtaRecoveryEntryRequiredB,entry_largest=65536;
unsigned entry_heap_reads=0;
size_t entry_sample();
std::mutex heap_mutex;
// Independent diagnostic samples must not consume or replace the policy sample sequence.
constexpr size_t diagnostic_free=1000000,diagnostic_largest=22222;
std::atomic<unsigned> info_calls{0};
std::atomic<bool> throw_memory_log{false},throw_memory_sample{false};
std::atomic<size_t> delivered_bytes{0},header_end_bytes{0};
size_t read_limit=71;
struct MemoryRecord {
 unsigned generation;unsigned long long origin;const char* phase;size_t free,largest,delivered,header_end;
};
std::mutex memory_records_mutex;
std::vector<MemoryRecord> memory_records;
std::vector<MemoryRecord> records(){
 std::lock_guard<std::mutex> lock(memory_records_mutex);return memory_records;
}
size_t phase_count(const std::vector<MemoryRecord>& rows,const char* phase){
 return static_cast<size_t>(std::count_if(rows.begin(),rows.end(),[&](const auto& row){
  return std::strcmp(row.phase,phase)==0;
 }));
}
std::atomic<bool> init_null{false};
std::atomic<bool> cleanup_hook_armed{false},cleanup_hook_entered{false},cleanup_hook_released{false};
std::atomic<bool> worker_returned{false},worker_paused_after_cleanup{false};
bool changelog_destroy_fails=false,jump_image_clock=false,image_clock_jumped=false;
bool reject_late_effect=false,raw_handle_live=false;
int raw_write_error_at=0,raw_write_error=ESP_OK;
bool connect_ok=true,create_ok=true,lock_ok=true,throw_create=false;
esp_ota_img_states_t sdk_state=ESP_OTA_IMG_VALID;
int queries=0;
TaskFunction_t selected_task=nullptr;
void* selected_argument=nullptr;
unsigned selected_stack=0,selected_priority=0;
std::string manifest="{\"version\":\"1.6.3\"}";
std::string changelog="{\"version\":\"1.6.3\",\"changelog\":\"A change\"}";
unsigned changelog_status=200;
int raw_end_error=ESP_OK,raw_select_error=ESP_OK;
std::string terminal_case;
std::string image_version="1.6.3",extra_header;
struct TlsConnection {std::string request,response;size_t offset=0;};
TlsConnection connection;
esp_tls_t tls{};
esp_partition_t running{ESP_PARTITION_TYPE_APP,0,0x210000,0x1f0000};
esp_partition_t inactive{ESP_PARTITION_TYPE_APP,0,0x20000,0x1f0000};
esp_app_desc_t running_description{};
std::string image_body(){
 std::string result(1024,'x');esp_image_header_t image{};image.magic=ESP_IMAGE_HEADER_MAGIC;
 esp_app_desc_t desc{};desc.magic_word=ESP_APP_DESC_MAGIC_WORD;
 std::snprintf(desc.version,sizeof(desc.version),"%s",image_version.c_str());
 std::memcpy(result.data(),&image,sizeof(image));
 std::memcpy(result.data()+sizeof(image)+sizeof(esp_image_segment_header_t),&desc,sizeof(desc));
 return result;
}
void join_worker(){if(worker.joinable())worker.join();}
[[noreturn]] void check_uncertain_terminal();
void check_writer_owner();
void check_effect_admission();
}
struct HostSemaphore {std::mutex mutex;};
extern "C" SemaphoreHandle_t xSemaphoreCreateMutex(){return fixture::lock_ok?new HostSemaphore:nullptr;}
extern "C" BaseType_t xSemaphoreTake(SemaphoreHandle_t p,TickType_t){p->mutex.lock();return pdTRUE;}
extern "C" BaseType_t xSemaphoreGive(SemaphoreHandle_t p){p->mutex.unlock();return pdTRUE;}
extern "C" void vSemaphoreDelete(SemaphoreHandle_t p){delete p;}
extern "C" BaseType_t xTaskCreate(TaskFunction_t task,const char* name,uint32_t stack,void* arg,
                                  UBaseType_t priority,TaskHandle_t*){
 CHECK(stack==8192);
 if(fixture::throw_create)throw std::bad_alloc();
 if(!fixture::create_ok)return pdFAIL;
 if(std::strcmp(name,"ota_net")==0){
  CHECK(task==tk::ota::ota_network_task&&!fixture::worker.joinable());
  fixture::worker=std::thread([task,arg]{fixture::network_thread=true;task(arg);});
 }else{
  CHECK(std::strcmp(name,"ota")==0||std::strcmp(name,"ota_chk")==0);
  fixture::selected_task=task;fixture::selected_argument=arg;
  fixture::selected_stack=stack;fixture::selected_priority=priority;
 }
 return pdPASS;
}
extern "C" void vTaskDelete(TaskHandle_t p){
 CHECK(p==nullptr);++fixture::deletes;
 if(fixture::network_thread){fixture::worker_returned=true;fixture::connection_ready.notify_all();}
}
extern "C" void vTaskDelay(TickType_t ticks){
 if(fixture::network_thread){
  if(fixture::cleanup_hook_released){fixture::worker_paused_after_cleanup=true;fixture::connection_ready.notify_all();}
  std::this_thread::yield();return;
 }
 // Hold the fixture clock until the actual cleanup CAS has captured its old word.
 // The separate hook advances it only after that read; no production clock is replaced.
 if(fixture::cleanup_hook_armed&&!fixture::cleanup_hook_entered){
  std::this_thread::sleep_for(std::chrono::microseconds(50));return;
 }
 if(ticks==10000&&fixture::terminal_case=="uncertain")fixture::check_uncertain_terminal();
 fixture::now.fetch_add(ticks);
 std::this_thread::sleep_for(std::chrono::microseconds(50));
}
extern "C" TickType_t xTaskGetTickCount(){return static_cast<TickType_t>(fixture::now.load());}
extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t p){CHECK(p==nullptr);return 2048;}
std::int64_t esp_timer_get_time(){return static_cast<int64_t>(fixture::now.load()*1000);}
size_t heap_caps_get_largest_free_block(unsigned caps){
 CHECK(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
 // Actual calls before Operation creation run on the caller; TLS calls run on the owned worker.
 // Distinct named reference inputs keep the new entry sample from shifting TLS guard oracles.
 if(!fixture::network_thread)return fixture::entry_sample();
 std::lock_guard<std::mutex> lock(fixture::heap_mutex);
 CHECK(!fixture::largest_samples.empty());
 // Repeat the last physical sample. Sample exhaustion cannot be the negative oracle.
 const size_t index=std::min(fixture::largest_cursor++,fixture::largest_samples.size()-1);
 return fixture::largest_samples[index];
}
void heap_caps_get_info(multi_heap_info_t* info,std::uint32_t caps){
 CHECK(info&&caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));++fixture::info_calls;
 if(fixture::throw_memory_sample)throw std::bad_alloc();
 info->total_free_bytes=fixture::diagnostic_free;info->largest_free_block=fixture::diagnostic_largest;
}
[[maybe_unused]] size_t esp_get_free_heap_size(){return 1000000;}
int esp_crt_bundle_attach(void*){return 0;}
esp_tls_t* esp_tls_init(){++fixture::init_calls;return fixture::init_null?nullptr:&fixture::tls;}
int esp_tls_conn_new_sync(const char* host,int count,int port,const esp_tls_cfg_t* cfg,esp_tls_t* p){
 CHECK(p==&fixture::tls&&port==443&&count>0&&count<=253);
 CHECK(std::string(host,static_cast<size_t>(count))==cfg->common_name);
 CHECK(!cfg->skip_common_name&&cfg->crt_bundle_attach==esp_crt_bundle_attach&&cfg->timeout_ms>0);
 ++fixture::connects;fixture::connection={};fixture::delivered_bytes=fixture::header_end_bytes=0;
 std::unique_lock<std::mutex> lock(fixture::connection_mutex);fixture::connect_entered=true;
 fixture::connection_ready.notify_all();
 fixture::connection_ready.wait(lock,[]{return !fixture::hold_connect||fixture::release_connect;});
 return fixture::connect_ok?1:-1;
}
ssize_t esp_tls_conn_write(esp_tls_t* p,const void* data,size_t n){
 CHECK(p==&fixture::tls);fixture::connection.request.append(static_cast<const char*>(data),n);
 return static_cast<ssize_t>(n);
}
ssize_t esp_tls_conn_read(esp_tls_t* p,void* out,size_t n){
 CHECK(p==&fixture::tls);++fixture::reads;auto& c=fixture::connection;
 if(c.response.empty()){
  CHECK(c.request.find("Accept-Encoding: identity\r\n")!=std::string::npos);
  const bool change=c.request.find("changelog.json")!=std::string::npos;
  const bool image=c.request.find(".bin")!=std::string::npos;
  const std::string payload=image?fixture::image_body():change?fixture::changelog:fixture::manifest;
  const unsigned status=change?fixture::changelog_status:200;
  c.response="HTTP/1.1 "+std::to_string(status)+" Fixture\r\n"+fixture::extra_header+"Content-Length: "+
    std::to_string(payload.size())+"\r\nConnection: close\r\n\r\n"+payload;
  fixture::header_end_bytes=c.response.find("\r\n\r\n")+4;
 }
 if(fixture::hold_image_read&&c.request.find(".bin")!=std::string::npos&&c.offset>=600){
  std::unique_lock<std::mutex> lock(fixture::connection_mutex);fixture::image_read_entered=true;
  fixture::connection_ready.notify_all();
  fixture::connection_ready.wait(lock,[]{return fixture::release_connect;});
 }
 if(fixture::jump_image_clock&&!fixture::image_clock_jumped&&
    c.request.find(".bin")!=std::string::npos&&c.offset>=600){
  fixture::image_clock_jumped=true;fixture::now=30000;
 }
 const size_t take=std::min({n,fixture::read_limit,c.response.size()-c.offset});
 std::memcpy(out,c.response.data()+c.offset,take);c.offset+=take;
 fixture::delivered_bytes=c.offset;return static_cast<ssize_t>(take);
}
int esp_tls_conn_destroy(esp_tls_t* p){
 CHECK(p==&fixture::tls);++fixture::destroys;
 return fixture::changelog_destroy_fails&&fixture::connection.request.find("changelog.json")!=std::string::npos?-1:0;
}
const esp_app_desc_t* esp_app_get_description(){return &fixture::running_description;}
const esp_partition_t* esp_ota_get_running_partition(){return &fixture::running;}
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* p){CHECK(p==nullptr);return &fixture::inactive;}
esp_err_t esp_ota_get_state_partition(const esp_partition_t* p,esp_ota_img_states_t* state){
 CHECK(p==&fixture::running);++fixture::queries;*state=fixture::sdk_state;return ESP_OK;
}
esp_err_t esp_ota_check_image_validity(esp_partition_type_t t,const esp_image_header_t* h,const esp_app_desc_t*){
 CHECK(t==ESP_PARTITION_TYPE_APP);return h->magic==ESP_IMAGE_HEADER_MAGIC?ESP_OK:ESP_ERR_OTA_VALIDATE_FAILED;
}
esp_err_t esp_ota_begin(const esp_partition_t* p,size_t n,esp_ota_handle_t* h){
 CHECK(!fixture::network_thread&&p==&fixture::inactive&&n==OTA_WITH_SEQUENTIAL_WRITES);
 fixture::check_effect_admission();CHECK(!fixture::raw_handle_live);
 ++fixture::begins;fixture::raw_handle_live=true;*h=1;return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t h,const void* bytes,size_t n){
 CHECK(!fixture::network_thread&&h==1&&bytes&&n&&n<=1024&&fixture::raw_handle_live);
 fixture::check_effect_admission();const int call=++fixture::writes;
 return call==fixture::raw_write_error_at?fixture::raw_write_error:ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t h){
 CHECK(!fixture::network_thread&&h==1&&fixture::raw_handle_live);fixture::check_effect_admission();
 ++fixture::ends;fixture::raw_handle_live=false;return fixture::raw_end_error; // Consumed on every SDK return.
}
esp_err_t esp_ota_abort(esp_ota_handle_t h){
 CHECK(!fixture::network_thread&&h==1&&fixture::raw_handle_live);fixture::check_writer_owner();
 ++fixture::aborts;fixture::raw_handle_live=false;return ESP_OK; // Abort is permitted after deadline.
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* p){
 CHECK(!fixture::network_thread&&p==&fixture::inactive&&fixture::ends==1);
 fixture::check_effect_admission();CHECK(!fixture::raw_handle_live);
 ++fixture::selectors;return fixture::raw_select_error;
}
static void fixture_log(const char* tag,const char* format,...){
 if(std::strcmp(tag,"ota_mem")!=0)return;
 CHECK(std::strcmp(format,"g=%u t0=%llu phase=%.24s internal_free=%zu internal_largest=%zu")==0);
 if(fixture::throw_memory_log)throw std::bad_alloc();
 va_list args;va_start(args,format);
 const unsigned generation=va_arg(args,unsigned);
 const auto origin=va_arg(args,unsigned long long);
 const char* phase=va_arg(args,const char*);
 const size_t free=va_arg(args,size_t),largest=va_arg(args,size_t);va_end(args);
 std::lock_guard<std::mutex> lock(fixture::memory_records_mutex);
 fixture::memory_records.push_back({generation,origin,phase,free,largest,
   fixture::delivered_bytes.load(),fixture::header_end_bytes.load()});
}
#define ESP_LOGI(...) fixture_log(__VA_ARGS__)
#define ESP_LOGW(...) fixture_log(__VA_ARGS__)
#define ESP_LOGE(...) fixture_log(__VA_ARGS__)
#define CONFIG_TESLA_OTA_MANIFEST_URL "https://update.example/manifest.json"
#define CONFIG_TESLA_OTA_FIRMWARE_BASE_URL "https://update.example/"
#define TESLA_OTA_IMG_SUFFIX ""
void esp_restart();
namespace tk {
std::size_t ota_recovery_required_bytes(OtaRecoveryUse use) noexcept {
 return ota_recovery_required_bytes_for(use,{fixture::entry_required,fixture::recovery_required});
}
}
#include "ota_tls_adapter.generated.hpp"

void esp_restart(){
 ++fixture::restart_calls;
 CHECK(fixture::terminal_case=="commit"&&fixture::selectors==1&&fixture::ends==1);
 CHECK(ota_is_busy()&&s_operation_gate.state()==tk::OtaIdentityGateState::Ota);
 CHECK(ota_get_state()==OtaState::Done);
 // Exits only the disposable terminal-effect fixture; this is not a task-lifetime/LSan claim.
 std::cout<<"OTA terminal committed owner retained; checks "<<fixture::checks<<'\n'<<std::flush;
 std::_Exit(0);
}
// Only this native build defines the production transport's existing scheduling hook.
// The hook precedes publication, never follows Ack, and owns no OTA/status/gate effect.
namespace tk::ota {
void host_before_cleanup_publish(Job& job,std::uint32_t observed) noexcept {
 if(!fixture::cleanup_hook_armed.exchange(false))return;
 CHECK(!(observed&kRetired));fixture::cleanup_hook_entered=true;fixture::now=30000;
 const auto stop=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(!job.retired()&&std::chrono::steady_clock::now()<stop)std::this_thread::yield();
 CHECK(job.retired());fixture::cleanup_hook_released=true;fixture::connection_ready.notify_all();
}
}
namespace fixture {
size_t entry_sample(){
 ++entry_heap_reads;CHECK(s_operation_gate.state()==tk::OtaIdentityGateState::Ota);
 return entry_largest;
}
void check_writer_owner(){
 CHECK(ota_is_busy()&&s_operation_gate.state()==tk::OtaIdentityGateState::Ota);
 CHECK(!network_thread);
}
void check_effect_admission(){
 check_writer_owner();CHECK(!reject_late_effect||now.load()<30000);
}
[[noreturn]] void check_uncertain_terminal(){
 CHECK(selectors==1&&ends==1&&restart_calls==0&&ota_is_busy());
 CHECK(s_operation_gate.state()==tk::OtaIdentityGateState::Ota&&ota_get_state()==OtaState::Error);
 CHECK(s_operation.load()&&!s_operation.load()->coordinator.writer_lease_released());
 std::cout<<"OTA uncertain selection parked with owner retained; checks "<<checks<<'\n'<<std::flush;
 std::_Exit(0); // Separate irreversible-effect fixture, not leak/task-reclamation proof.
}
void clean_previous(){
 join_worker();
 if(auto* p=s_operation.load()){
  CHECK(p->coordinator_done.load()&&p->coordinator.can_reclaim());
  CHECK(s_operation_gate.try_begin(tk::OtaIdentityGateState::Ota));
  CHECK(reclaim_finished_operation());finish_operation(tk::OtaIdentityGateState::Ota);
 }
 CHECK(!ota_is_busy());
}
void reset(std::initializer_list<size_t> samples={65536}){
 clean_previous();now=0;largest_samples=samples;largest_cursor=0;recovery_required=tk::kOtaTlsRecordCapacityB;
 entry_required=tk::kOtaRecoveryEntryRequiredB;entry_largest=65536;entry_heap_reads=0;
 {std::lock_guard<std::mutex> lock(memory_records_mutex);memory_records.clear();}
 info_calls=0;throw_memory_log=throw_memory_sample=false;
 delivered_bytes=header_end_bytes=0;read_limit=71;
 init_calls=connects=destroys=reads=deletes=0;begins=writes=ends=selectors=aborts=restart_calls=0;
 init_null=false;connect_ok=create_ok=lock_ok=true;throw_create=false;fail_body_reserve=false;
 cleanup_hook_armed=cleanup_hook_entered=cleanup_hook_released=false;
 worker_returned=worker_paused_after_cleanup=false;
 changelog_destroy_fails=jump_image_clock=image_clock_jumped=reject_late_effect=raw_handle_live=false;
 raw_write_error_at=0;raw_write_error=ESP_OK;
 sdk_state=ESP_OTA_IMG_VALID;queries=0;selected_task=nullptr;selected_argument=nullptr;
 hold_connect=release_connect=connect_entered=hold_image_read=image_read_entered=false;changelog_status=200;raw_end_error=raw_select_error=ESP_OK;
 image_version="1.6.3";extra_header.clear();inactive={ESP_PARTITION_TYPE_APP,0,0x20000,0x1f0000};
 manifest="{\"version\":\"1.6.3\"}";changelog="{\"version\":\"1.6.3\",\"changelog\":\"A change\"}";
 std::snprintf(running_description.version,sizeof(running_description.version),"1.6.2");
 ota_set_channel(tk::OtaChannel::Release);
}
void run_selected(){CHECK(selected_task);auto task=selected_task;auto* arg=selected_argument;selected_task=nullptr;task(arg);}
void memory_checks(){
 for(auto samples:{std::vector<size_t>{18444},{65536,18444},{65536,65536,18444},{65536,65536,65536,18444}}){
  reset();largest_samples=samples;const auto r=ota_check(0);join_worker();
  CHECK(!r.ok&&!r.update_available&&r.reason==kOtaNoMemory&&begins==0&&selectors==0);
  CHECK(connects<=1&&destroys==init_calls); // pre-init refusal needs no cleanup; init-null is separate.
 }
 reset({18444});CHECK(ota_check_start(400)==tk::OtaStartResult::Started);run_selected();join_worker();
 CHECK(ota_get_status().message==kOtaNoMemory&&!ota_get_status().update_available&&init_calls==0);
 reset({65536,18444});auto r=ota_check(0);join_worker();CHECK(!r.ok&&init_calls==1&&connects==0&&destroys==1);
 reset();init_null=true;r=ota_check(0);join_worker();CHECK(!r.ok&&r.reason==kOtaNoMemory&&connects==0&&destroys==0);
 reset();connect_ok=false;r=ota_check(0);join_worker();CHECK(!r.ok&&r.reason!="not enough memory for update");
 reset();fail_body_reserve=true;r=ota_check(0);join_worker();CHECK(!r.ok&&r.reason==kOtaNoMemory&&init_calls==0);
 reset();changelog_status=404;r=ota_check(0);join_worker();CHECK(r.ok&&r.update_available&&connects==2);
 reset();r=ota_check(0);join_worker();CHECK(r.ok&&r.current=="1.6.2"&&r.available=="1.6.3"&&r.update_available);
}
void recovery_policy_checks(){
 for(size_t observed:{size_t(12344),size_t(12345),size_t(12346)}){
  reset({observed});recovery_required=12345;const auto result=ota_check(0);join_worker();
  if(observed==12344){CHECK(!result.ok&&result.reason==kOtaNoMemory&&init_calls==0&&connects==0&&destroys==0);}
  else{CHECK(result.ok&&result.update_available&&init_calls==2&&connects==2&&destroys==2);}
  CHECK(begins==0&&selectors==0);
 }
 reset({12345,12344});recovery_required=12345;auto result=ota_check(0);join_worker();
 CHECK(!result.ok&&result.reason==kOtaNoMemory&&init_calls==1&&connects==0&&destroys==1);
 for(size_t observed:{size_t(12344),size_t(12345),size_t(12346)}){
  reset({65536,65536,65536,65536,observed,observed});recovery_required=12345;
  raw_end_error=ESP_ERR_OTA_VALIDATE_FAILED; // No committed/restart terminal in this returning fixture.
  CHECK(ota_start(0)==tk::OtaStartResult::Started);run_selected();join_worker();
  if(observed==12344){CHECK(init_calls==2&&connects==2&&begins==0&&ends==0&&ota_get_status().message==kOtaNoMemory);}
  else{CHECK(init_calls==3&&connects==3&&begins==1&&ends==1);}
  CHECK(selectors==0&&!ota_is_busy());
 }
 reset({SIZE_MAX});recovery_required=0;result=ota_check(0);join_worker();
 CHECK(!result.ok&&result.reason==kOtaNoMemory&&init_calls==0&&connects==0);
 reset(); // Preserve record-capacity/default regression cases as their own matrix.
}
void entry_recovery_checks(){
 for(int shape:{0,1,2}){
  reset();entry_required=12345;entry_largest=12344;
  OtaStatusPod stale{};stale.update_available=true;stale.target_pr=7;
  copy_status_text(stale.available,"1.9.9");publish_check_status(stale);
  s_target_pr.store(7,std::memory_order_release);
  if(shape==0){const auto r=ota_check(400);CHECK(!r.ok&&!r.update_available&&r.reason==kOtaNoMemory&&r.target_pr==400);}
  else CHECK((shape==1?ota_check_start(400):ota_start(400))==tk::OtaStartResult::NoMemory);
  const auto status=ota_get_status();
  CHECK(status.state==OtaState::Error&&status.message==kOtaNoMemory&&!status.update_available);
  CHECK(status.target_pr==400&&status.available.empty());
  CHECK(entry_heap_reads==1&&largest_cursor==0&&!selected_task&&!s_operation.load());
  CHECK(init_calls==0&&connects==0&&begins==0&&selectors==0&&!ota_is_busy());
  CHECK(s_operation_gate.state()==tk::OtaIdentityGateState::Idle);
 }
 for(size_t observed:{size_t(12345),size_t(12346)}){
  reset();entry_required=12345;entry_largest=observed;
  const auto r=ota_check(0);join_worker();CHECK(r.ok&&r.update_available&&entry_heap_reads==1&&connects==2);
 }
 reset();entry_required=0;entry_largest=SIZE_MAX;
 CHECK(ota_check_start(0)==tk::OtaStartResult::NoMemory&&init_calls==0&&!selected_task);
 reset();const auto r=ota_check(0);join_worker();CHECK(r.ok);
 CHECK(s_operation_gate.try_begin(tk::OtaIdentityGateState::Ota));
 auto* old=s_operation.load(std::memory_order_acquire);CHECK(old);
 const auto phase=old->job.control.load();
 CHECK(ordinary_recovery_reference_ready_owned(tk::OtaIdentityGateState::Ota));
 CHECK(!ordinary_recovery_reference_ready_owned(tk::OtaIdentityGateState::HealthCommit));
 old->coordinator_done.store(false,std::memory_order_release);
 CHECK(!ordinary_recovery_reference_ready_owned(tk::OtaIdentityGateState::Ota));
 old->coordinator_done.store(true,std::memory_order_release);
 CHECK(ordinary_recovery_reference_ready_owned(tk::OtaIdentityGateState::Ota));
 CHECK(old==s_operation.load()&&phase==old->job.control.load()&&s_network_slot.owns(old->job));
 finish_operation(tk::OtaIdentityGateState::Ota);reset();
}
void parser_checks(){
 reset();manifest.assign(8193,'x');auto r=ota_check(0);join_worker();
 CHECK(!r.ok&&!r.update_available&&begins==0&&selectors==0);
 reset();extra_header="X-Bounded: "+std::string(1100,'a')+"\r\n";
 r=ota_check(0);join_worker();CHECK(!r.ok&&!r.update_available&&begins==0);
 reset();manifest="{\"version\":\"1.6.3\",\"version\":\"1.6.4\"}";
 r=ota_check(0);join_worker();CHECK(!r.ok&&!r.update_available&&begins==0);
}
void admission_checks(){
 reset();if(auto* mutex=s_lock.exchange(nullptr))vSemaphoreDelete(mutex);lock_ok=false;
 CHECK(ota_check_start(0)==tk::OtaStartResult::Unavailable&&!selected_task&&init_calls==0);
 lock_ok=true;
 for(auto start:{ota_check_start,ota_start}){
  for(auto owner:{tk::OtaIdentityGateState::Ota,tk::OtaIdentityGateState::IdentityMutation,
                 tk::OtaIdentityGateState::FaultRestart,tk::OtaIdentityGateState::HealthCommit,
                 tk::OtaIdentityGateState::ConfigRestart}){
   reset();CHECK(s_operation_gate.try_begin(owner));CHECK(start(400)==tk::OtaStartResult::Busy);
   CHECK(!selected_task&&init_calls==0&&s_operation_gate.state()==owner);CHECK(s_operation_gate.finish(owner));
  }
  reset();create_ok=false;CHECK(start(0)==tk::OtaStartResult::InternalError);
  CHECK(!ota_is_busy()&&init_calls==0);
 }
 for(auto raw:{ESP_OTA_IMG_NEW,ESP_OTA_IMG_PENDING_VERIFY}){
  reset();sdk_state=raw;CHECK(ota_start(0)==tk::OtaStartResult::PendingVerification&&queries==1&&!selected_task);
  CHECK(ota_check_start(0)==tk::OtaStartResult::Started&&queries==1);run_selected();join_worker();
 }
 for(auto raw:{ESP_OTA_IMG_INVALID,ESP_OTA_IMG_ABORTED,static_cast<esp_ota_img_states_t>(17)}){
  reset();sdk_state=raw;CHECK(ota_start(0)==tk::OtaStartResult::Unavailable&&queries==1&&!selected_task);
 }
 reset();CHECK(ota_refresh_verification_cache()==tk::OtaVerificationState::Stable);
 sdk_state=ESP_OTA_IMG_PENDING_VERIFY;CHECK(ota_start(0)==tk::OtaStartResult::PendingVerification&&queries==2);
 reset();CHECK(ota_check_start(0)==tk::OtaStartResult::Started&&selected_stack==8192&&selected_priority==tk::kPrioOtaCheck);
 run_selected();join_worker();CHECK(!ota_is_busy()&&ota_get_status().update_available);
}
void quarantine_checks(){
 reset();hold_connect=true;
 const auto r=ota_check(0);
 {std::unique_lock<std::mutex> lock(connection_mutex);CHECK(connect_entered);}
 CHECK(!r.ok&&r.reason=="update check deadline reached"&&!ota_is_busy()&&begins==0);
 CHECK(s_operation.load()&&s_operation.load()->job.retired()&&!s_operation.load()->coordinator.can_reclaim());
 CHECK(s_operation_gate.try_begin(tk::OtaIdentityGateState::Ota));
 CHECK(!ordinary_recovery_reference_ready_owned(tk::OtaIdentityGateState::Ota));
 finish_operation(tk::OtaIdentityGateState::Ota);
 const auto old_status=ota_get_status();
 CHECK(ota_check_start(0)==tk::OtaStartResult::Busy&&connects==1);
 {std::lock_guard<std::mutex> lock(connection_mutex);release_connect=true;}connection_ready.notify_all();join_worker();
 CHECK(s_operation.load()->job.phase()==tk::ota::Phase::Acked&&begins==0&&selectors==0&&destroys==1);
 CHECK(ota_get_status().message==old_status.message&&ota_get_status().update_available==old_status.update_available);
 CHECK(ota_check_start(0)==tk::OtaStartResult::Started);run_selected();join_worker();CHECK(!ota_is_busy());
}
void raw_memory_checks(){
 // Real RawWriter invokes each passive SDK effect once; write failure must abort a live
 // handle, whereas esp_ota_end consumes that handle even when it returns NO_MEM.
 for(int call:{1,2}){
  reset();raw_write_error_at=call;raw_write_error=ESP_ERR_NO_MEM;
  CHECK(ota_start(0)==tk::OtaStartResult::Started);run_selected();join_worker();
  CHECK(ota_get_status().message==kOtaNoMemory&&!ota_get_status().update_available);
  CHECK(begins==1&&writes==call&&aborts==1&&ends==0&&selectors==0&&restart_calls==0);
  CHECK(!raw_handle_live&&!ota_is_busy());
 }
 reset();raw_end_error=ESP_ERR_NO_MEM;
 CHECK(ota_start(0)==tk::OtaStartResult::Started);run_selected();join_worker();
 CHECK(ota_get_status().message==kOtaNoMemory&&!ota_get_status().update_available);
 CHECK(begins==1&&writes>0&&ends==1&&aborts==0&&selectors==0&&restart_calls==0);
 CHECK(!raw_handle_live&&!ota_is_busy());
}
void checkpoint_semantics_checks(){
 reset();
 tk::ota::memory_checkpoint("sample_probe",17,23,12345);
 auto rows=records();
 CHECK(rows.size()==1&&rows[0].generation==17&&rows[0].origin==23);
 CHECK(rows[0].free==diagnostic_free&&rows[0].largest==12345&&largest_cursor==0);
 tk::ota::memory_checkpoint("ordinary_probe",19,29);
 rows=records();CHECK(rows.size()==2&&rows[1].largest==diagnostic_largest&&largest_cursor==0);
 reset({65536,18444});auto result=ota_check(0);join_worker();rows=records();
 CHECK(!result.ok&&result.reason==kOtaNoMemory&&init_calls==1&&connects==0&&destroys==1);
 CHECK(largest_cursor==2&&phase_count(rows,"tls_pre_init")==1&&phase_count(rows,"tls_post_context")==1);
 for(const auto& row:rows){
  if(std::strcmp(row.phase,"tls_pre_init")==0)CHECK(row.largest==65536);
  if(std::strcmp(row.phase,"tls_post_context")==0)CHECK(row.largest==18444);
 }
 CHECK(phase_count(rows,"tls_connected")==0&&phase_count(rows,"tls_destroyed")==1);
 for(size_t limit:{size_t(71),size_t(1)}){
  reset();read_limit=limit;result=ota_check(0);join_worker();rows=records();
  CHECK(result.ok&&result.update_available&&init_calls==2&&connects==2&&destroys==2);
  CHECK(largest_cursor==4&&begins==0&&selectors==0);
  for(const char* phase:{"before_operation","operation_ready","tasks_ready","manifest_ready","notes_ready"})
   CHECK(phase_count(rows,phase)==1);
  for(const char* phase:{"tls_pre_init","tls_post_context","tls_connected","headers_observed","tls_destroyed","metadata_closed"})
   CHECK(phase_count(rows,phase)==2);
  const std::array<const char*,6> sequence{{"tls_pre_init","tls_post_context","tls_connected",
    "headers_observed","tls_destroyed","metadata_closed"}};
  size_t next=0;
  for(const auto& row:rows){
   CHECK(row.generation==1&&row.origin==0&&row.free==diagnostic_free);
   if(std::find_if(sequence.begin(),sequence.end(),[&](const char* phase){return std::strcmp(row.phase,phase)==0;})!=sequence.end()){
    CHECK(std::strcmp(row.phase,sequence[next%sequence.size()])==0);++next;
   }
   if(std::strcmp(row.phase,"headers_observed")==0){
    CHECK(row.header_end>1&&row.delivered>=row.header_end);
    if(limit==1)CHECK(row.delivered==row.header_end);
   }
  }
  CHECK(next==12);
  clean_previous();rows=records();CHECK(phase_count(rows,"reclaim_ready")==1);
 }
 for(bool fail_sample:{false,true}){
  reset();throw_memory_sample=fail_sample;throw_memory_log=!fail_sample;
  result=ota_check(0);join_worker();
  CHECK(result.ok&&result.update_available&&init_calls==2&&connects==2&&destroys==2);
  CHECK(largest_cursor==4&&begins==0&&selectors==0&&info_calls>0&&records().empty());
  throw_memory_sample=throw_memory_log=false;
 }
 reset();
}
void budget_checks(){
 reset();jump_image_clock=reject_late_effect=true;
 CHECK(ota_start(0)==tk::OtaStartResult::Started);
 CHECK(s_operation.load()&&s_operation.load()->job.deadline_ms>30000);
 run_selected();join_worker();auto* operation=s_operation.load();CHECK(operation);
 // The worker's original maximum remains live at 30s. Only the coordinator's real
 // declared-image-size tightening makes admission expire before the next raw effect.
 CHECK(image_clock_jumped&&operation->budget.deadline()<30000);
 CHECK(operation->coordinator.outcome()==tk::ota::Outcome::Deadline);
 CHECK(begins==1&&writes>0&&aborts==1&&ends==0&&selectors==0&&restart_calls==0);
 CHECK(!raw_handle_live&&!ota_is_busy()&&!ota_get_status().update_available);
}
void cleanup_cas_checks(){
 reset();cleanup_hook_armed=true;const auto result=ota_check(0);
 CHECK(!result.ok&&result.reason=="update check deadline reached"&&cleanup_hook_entered);
 const auto stop=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(!worker_returned&&!worker_paused_after_cleanup&&std::chrono::steady_clock::now()<stop)
  std::this_thread::yield();
 CHECK(worker_returned||worker_paused_after_cleanup);
 // Read atomics before join: an unchecked stale store loses Retired and parks the worker.
 // That mutant must fail this exact CHECK, never earn credit from timeout or destructor.
 auto* operation=s_operation.load();CHECK(operation);
 CHECK(operation->job.retired()&&operation->job.phase()==tk::ota::Phase::Acked);
 join_worker();CHECK(operation->coordinator.can_reclaim()&&connects==1&&destroys==1);
 CHECK(!ota_is_busy()&&begins==0&&selectors==0);
}
[[noreturn]] void cleanup_failed_checks(){
 reset();const auto first=ota_check(0);join_worker();CHECK(first.ok&&first.update_available);
 reset();changelog_destroy_fails=true;const auto result=ota_check(0);join_worker();
 CHECK(!result.ok&&!result.update_available);
 CHECK(ota_get_state()==OtaState::Error&&!ota_get_status().update_available);
 auto* operation=s_operation.load();CHECK(operation);
 CHECK(operation->job.retired()&&operation->job.phase()==tk::ota::Phase::CleanupFailed);
 CHECK(operation->coordinator.writer_lease_released()&&operation->coordinator_done.load());
 CHECK(!operation->coordinator.can_reclaim()&&!ota_is_busy());
 CHECK(connects==2&&destroys==2&&begins==0&&writes==0&&ends==0&&selectors==0&&aborts==0);
 CHECK(ota_check_start(0)==tk::OtaStartResult::Busy&&connects==2);
 // A failed SDK destroy has no truthful Ack/reclaim path. Exit only this disposable
 // quarantine fixture; this is deliberately NOT an LSan/task-reclamation assertion.
 std::cout<<"OTA failed cleanup withdraws offer and retains slot; checks "<<checks<<'\n'<<std::flush;
 std::_Exit(0);
}
void image_checks(){
 for(const char* bad:{"1.6.4","1.6.2","01.6.3"}){
  reset();image_version=bad;CHECK(ota_start(0)==tk::OtaStartResult::Started);run_selected();join_worker();
  CHECK(begins==0&&writes==0&&ends==0&&selectors==0&&!ota_is_busy());
 }
 reset();inactive.address=0x9000;CHECK(ota_start(0)==tk::OtaStartResult::Started);run_selected();join_worker();
 CHECK(begins==0&&writes==0&&ends==0&&selectors==0&&!ota_is_busy());
 reset();hold_image_read=true;
 CHECK(ota_start(0)==tk::OtaStartResult::Started);run_selected();
 {std::unique_lock<std::mutex> lock(connection_mutex);CHECK(image_read_entered);}
 CHECK(begins==1&&writes>0&&aborts==1&&ends==0&&selectors==0&&!ota_is_busy());
 CHECK(s_operation.load()->job.retired()&&!s_operation.load()->coordinator.can_reclaim());
 CHECK(ota_start(0)==tk::OtaStartResult::Busy);const int prior_writes=writes.load();
 {std::lock_guard<std::mutex> lock(connection_mutex);release_connect=true;}connection_ready.notify_all();join_worker();
 CHECK(writes==prior_writes&&aborts==1&&ends==0&&selectors==0);
 reset();raw_end_error=ESP_ERR_OTA_VALIDATE_FAILED;
 CHECK(ota_start(0)==tk::OtaStartResult::Started);run_selected();join_worker();
 CHECK(begins==1&&writes>0&&ends==1&&selectors==0&&!ota_is_busy());
 reset({65536,65536,65536,65536,18444});
 CHECK(ota_start(0)==tk::OtaStartResult::Started);run_selected();join_worker();
 CHECK(ota_get_status().message==kOtaNoMemory&&begins==0&&selectors==0&&!ota_is_busy());
}
// A real atomic backing with a false trait executes the actual non-lock-free branch on host.
// The competing phase writer is an unconditional effect; it does not mirror transport decisions.
struct CasBackedControl {
 static constexpr bool is_always_lock_free=false;
 std::atomic<std::uint32_t> value{0};
 std::uint32_t load(std::memory_order order)const noexcept{return value.load(order);}
 bool compare_exchange_strong(std::uint32_t& expected,std::uint32_t desired,
     std::memory_order success,std::memory_order failure)noexcept{
  return value.compare_exchange_strong(expected,desired,success,failure);
 }
};
void control_rmw_concurrency_checks(){
 for(unsigned round=0;round<32;++round){
  CasBackedControl control;std::atomic<bool> start{false};
  auto wait=[&]{while(!start.load(std::memory_order_acquire))std::this_thread::yield();};
  std::thread retire([&]{wait();tk::ota::publish_control_bits(control,tk::ota::kRetired);});
  std::thread close([&]{wait();tk::ota::publish_control_bits(control,tk::ota::kClose);});
  std::thread phase([&]{wait();auto previous=control.value.load(std::memory_order_acquire);
   while(!control.value.compare_exchange_strong(previous,
     (previous&~tk::ota::kPhaseMask)|static_cast<std::uint32_t>(tk::ota::Phase::Reading),
     std::memory_order_acq_rel,std::memory_order_acquire)){}
  });
  start.store(true,std::memory_order_release);retire.join();close.join();phase.join();
  CHECK(control.value.load(std::memory_order_acquire)==
    (tk::ota::kRetired|tk::ota::kClose|static_cast<std::uint32_t>(tk::ota::Phase::Reading)));
 }
}
}
int main(int argc,char** argv){
 using namespace fixture;
 static_assert(!std::is_copy_constructible_v<tk::ota::RawWriter> &&
               !std::is_move_constructible_v<tk::ota::RawWriter>);
 terminal_case=argc==2?argv[1]:"normal";
 if(terminal_case=="commit"){
  reset();CHECK(ota_start(0)==tk::OtaStartResult::Started);run_selected();CHECK(false);
 }
 if(terminal_case=="uncertain"){
  reset();raw_select_error=ESP_FAIL;CHECK(ota_start(0)==tk::OtaStartResult::Started);run_selected();CHECK(false);
 }
 if(terminal_case=="cleanup-failed")cleanup_failed_checks();
 if(terminal_case=="budget")budget_checks();
 else if(terminal_case=="cleanup-cas")cleanup_cas_checks();
 else {
  CHECK(terminal_case=="normal");
  memory_checks();recovery_policy_checks();entry_recovery_checks();parser_checks();admission_checks();quarantine_checks();image_checks();raw_memory_checks();
  checkpoint_semantics_checks();
  control_rmw_concurrency_checks();
 }
 clean_previous();
 // Release only the passive status mutex after every actual worker is joined and context gone.
 if(auto* mutex=s_lock.exchange(nullptr))vSemaphoreDelete(mutex);
 std::cout<<"OTA application checks "<<checks<<'\n';return 0;
}
