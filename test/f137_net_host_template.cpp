// UNCOMPILED/UNRUN DRAFT. Actual entire watchdog/gateway/recovery + real PingProbeControl.
#include <atomic>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include "f137_task_lsan.hpp"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "ping_probe.hpp"
#include "logic/net_link.hpp"
#include "runtime_fault_log.hpp"
#include "stack_watch.hpp"

static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(42);}}while(false)
struct HostSemaphore{unsigned tokens{};};
struct HostPingSession{esp_ping_callbacks_t callbacks{};unsigned replies{};bool ended{};};
struct esp_netif_t{unsigned identity{};};
struct esp_ip4_addr_t{std::uint32_t addr{};};
struct esp_netif_ip_info_t{esp_ip4_addr_t gw{};};
#define CONFIG_TESLA_ETH_ENABLED 1
namespace fixture {
std::string mode;
unsigned step{},end_step{},recoveries{},warnings{},fault_warnings{},boundary_stops{},period_delays{},new_calls{},delete_calls{},give_calls{},stop_calls{},profile_calls{};
unsigned expected_recoveries{};
TickType_t ticks{};
std::vector<std::string> trace;
std::vector<unsigned> recovery_steps;
std::vector<TickType_t> delays;
esp_netif_t wifi{1},eth{2};
std::uint16_t wifi_lease=1,eth_lease=1;
bool throw_clock{},throw_fault_log{},throw_recover_once{};
HostPingSession* last_session{};
HostPingSession* pending{};
std::uint32_t pending_generation{};
HostSemaphore semaphore{};
void record(const char* s){trace.emplace_back(s);}
[[noreturn]] void finish();
void before_observe();
unsigned current_replies();
bool current_pending();
void log(int level,const char*,const char* format,...){
    if(level<2)return;
    if(std::strstr(format,"iteration failed")){++fault_warnings;record("iteration-fault");if(throw_fault_log)throw std::runtime_error("passive log");}
    else if(std::strstr(format,"boundary failed")){++boundary_stops;record("boundary-stop");}
    else ++warnings;
}
}
#define ESP_LOGW(tag,...) fixture::log(2,tag,__VA_ARGS__)
#define ESP_LOGE(tag,...) fixture::log(3,tag,__VA_ARGS__)
#define ESP_LOGD(tag,...) fixture::log(0,tag,__VA_ARGS__)
static const char* TAG="net";
static std::int64_t esp_timer_get_time(){
    if(fixture::throw_clock)throw std::runtime_error("passive clock");
    if(fixture::mode=="fault-quota")return fixture::step<4?1000000:31000000;
    return static_cast<std::int64_t>(fixture::ticks)*1000;
}
extern "C" TickType_t xTaskGetTickCount(){return fixture::ticks;}
extern "C" BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t ticks){
    CHECK(s==&fixture::semaphore);
    if(s->tokens){--s->tokens;return pdTRUE;}
    fixture::ticks+=ticks;return pdFALSE;
}
extern "C" BaseType_t xSemaphoreGive(SemaphoreHandle_t s){CHECK(s==&fixture::semaphore);++s->tokens;++fixture::give_calls;return pdTRUE;}
extern "C" void vTaskDelay(TickType_t ticks){
    fixture::delays.push_back(ticks);
    if(ticks==30000){
        ++fixture::period_delays;
        if(fixture::step==fixture::end_step)fixture::finish();
        ++fixture::step;fixture::throw_clock=false;fixture::throw_fault_log=false;
    }else CHECK(ticks==200);
    fixture::ticks+=ticks;
}
extern "C" void vTaskDelete(TaskHandle_t task){CHECK(task==nullptr);CHECK(false);}
extern "C" esp_err_t esp_ping_new_session(const esp_ping_config_t* cfg,const esp_ping_callbacks_t* cb,esp_ping_handle_t* out){
    CHECK(cfg && cb && out);CHECK(cfg->count==3 && cfg->timeout_ms==1000 && cfg->interface!=0);
    ++fixture::new_calls;auto* s=new HostPingSession;s->callbacks=*cb;s->replies=fixture::current_replies();
    fixture::last_session=s;*out=s;return ESP_OK;
}
extern "C" esp_err_t esp_ping_start(esp_ping_handle_t s){
    CHECK(s);if(!fixture::current_pending()){s->ended=true;s->callbacks.on_ping_end(s,s->callbacks.cb_args);}return ESP_OK;
}
extern "C" esp_err_t esp_ping_stop(esp_ping_handle_t s){CHECK(s);++fixture::stop_calls;return ESP_OK;}
extern "C" esp_err_t esp_ping_delete_session(esp_ping_handle_t s){
    CHECK(s && s->ended);++fixture::delete_calls;
    if(s==fixture::last_session)fixture::last_session=nullptr;
    delete s;return ESP_OK;
}
extern "C" esp_err_t esp_ping_get_profile(esp_ping_handle_t s,int profile,void* value,std::size_t size){
    CHECK(s && profile==ESP_PING_PROF_REPLY && size==sizeof(unsigned));++fixture::profile_calls;*static_cast<unsigned*>(value)=s->replies;return ESP_OK;
}
static esp_err_t esp_netif_get_ip_info(esp_netif_t* netif,esp_netif_ip_info_t* ip){CHECK(netif && ip);ip->gw.addr=netif->identity;return ESP_OK;}
static const char* esp_ip4addr_ntoa(const esp_ip4_addr_t* ip,char* out,std::size_t size){std::snprintf(out,size,"%u",ip->addr);return out;}
static int ipaddr_aton(const char* in,ip_addr_t* out){out->addr=static_cast<unsigned>(std::strtoul(in,nullptr,10));return 1;}
static int esp_netif_get_netif_impl_index(esp_netif_t* netif){return static_cast<int>(netif->identity);}
static void esp_wifi_disconnect(){
    ++fixture::recoveries;fixture::recovery_steps.push_back(fixture::step);fixture::record("wifi-recover");
    if(fixture::throw_recover_once){fixture::throw_recover_once=false;throw std::runtime_error("passive partial recovery");}
}
static void* s_eth_handle=reinterpret_cast<void*>(1);
static void esp_eth_stop(void*){++fixture::recoveries;fixture::recovery_steps.push_back(fixture::step);fixture::record("eth-recover");}
static void esp_eth_start(void*){fixture::record("eth-start");}
namespace tk {
static std::atomic<NetLink> s_kind{NetLink::Wifi};
static esp_netif_t* net_active_netif(){return s_kind.load()==NetLink::Eth?&fixture::eth:&fixture::wifi;}
static std::uint16_t lease_generation(NetLink kind){return kind==NetLink::Eth?fixture::eth_lease:fixture::wifi_lease;}
static bool net_is_up(){fixture::before_observe();return true;}

extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task){CHECK(task==nullptr);return 4096u;}

// ACTUAL_COMPLETE_BODIES

} // namespace tk
namespace fixture {
bool current_pending(){return mode=="pending-preserved" && step==2;}
unsigned current_replies(){
    if(mode=="normal")return step==1?1:0;
    if(mode=="partial-recovery")return (step==1 || step==6)?1:0;
    if(mode=="both-baselines")return (step==1 || step==2 || step==8)?1:0;
    if(mode=="pending-preserved")return (step==1 || step==6)?1:0;
    return step==1?1:0;
}
void before_observe(){
    record("observe");
    if(mode=="both-baselines"){
        tk::s_kind=step==2 || step==4 || step==5 || step>=8?tk::NetLink::Eth:tk::NetLink::Wifi;
        if(step==3)throw std::runtime_error("observation after both baselines");
    }
    if((mode=="observation-throw" || mode=="clock-throw" || mode=="log-throw") && step==2){
        throw_clock=mode=="clock-throw";throw_fault_log=mode=="log-throw";
        throw std::bad_alloc();
    }
    if(mode=="fault-quota" && (step==1 || step==3 || step==4))throw 7;
    if(mode=="pending-preserved"){
        if(step==2){
            esp_ping_config_t config{};config.count=3;config.timeout_ms=1000;config.interface=1;
            CHECK(tk::ping_probe_run(tk::s_wd,config,20,5)==tk::PingProbeResult::PendingEnd);
            pending=tk::s_wd.session;pending_generation=tk::s_wd.session_generation;
            CHECK(pending && !pending->ended && pending_generation!=0);
            throw std::runtime_error("observation with a live quarantined ping");
        }
        if(step==3){
            CHECK(tk::s_wd.session==pending && tk::s_wd.session_generation==pending_generation);
            CHECK(tk::s_wd.generation.active()==pending_generation && tk::s_wd.callback_args.control==&tk::s_wd);
            CHECK(tk::s_wd.done==&semaphore && tk::s_wd.session_started && new_calls==2 && delete_calls==1);
            const unsigned gives=give_calls;
            tk::PingProbeCallbackArgs stale{&tk::s_wd,pending_generation+1};
            tk::ping_probe_on_end(pending,&stale);CHECK(give_calls==gives);
            CHECK(!tk::s_wd.generation.ended(pending_generation));
        }
        if(step==4){
            CHECK(tk::s_wd.session==pending && new_calls==2 && delete_calls==1);
            pending->ended=true;pending->replies=0;
            pending->callbacks.on_ping_end(pending,pending->callbacks.cb_args);
            CHECK(tk::s_wd.generation.ended(pending_generation));
        }
    }
}
[[noreturn]] void finish(){
    CHECK(step==end_step && period_delays==end_step+1);
    CHECK(boundary_stops==0); // Catch diagnostics/clock must not end the task.
    CHECK(recoveries==expected_recoveries);
    for(auto d:delays)CHECK(d==30000 || d==200); // No immediate recovery loop.
    CHECK(tk::s_wd.done==&semaphore);
    CHECK(tk::s_wd.session==nullptr && tk::s_wd.session_generation==0 && tk::s_wd.generation.active()==0);
    CHECK(new_calls==delete_calls); // Exact late completion allows retirement, not abandonment.
    if(mode=="normal")CHECK(recovery_steps.size()==1 && recovery_steps[0]==3);
    if(mode=="partial-recovery")CHECK(recovery_steps.size()==2 && recovery_steps[0]==3 && recovery_steps[1]==8);
    if(mode=="both-baselines")CHECK(recovery_steps.size()==1 && recovery_steps[0]==10);
    if(mode=="pending-preserved")CHECK(recovery_steps.size()==1 && recovery_steps[0]==8 && stop_calls>0);
    if(mode=="clock-throw")CHECK(fault_warnings==0);
    if(mode=="log-throw" || mode=="observation-throw" || mode=="partial-recovery" || mode=="pending-preserved" || mode=="both-baselines")CHECK(fault_warnings==1);
    if(mode=="fault-quota")CHECK(fault_warnings==2); // A successful iteration cannot replenish the quota.
    CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::NetWatchdog) == 4096u);
    CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::PingNet) == 4096u);
    CHECK(f137_task_leak_check() == 0);
    std::printf("PASS %u actual network/watchdog/ping checks (%s); bounded passive task\n",checks,mode.c_str());std::fflush(stdout);std::_Exit(0);
}
}
int main(int argc,char**argv){
    CHECK(argc==2);fixture::mode=argv[1];tk::s_wd.done=&fixture::semaphore;
    if(fixture::mode=="normal"){fixture::end_step=3;fixture::expected_recoveries=1;}
    else if(fixture::mode=="partial-recovery"){fixture::end_step=8;fixture::expected_recoveries=2;fixture::throw_recover_once=true;}
    else if(fixture::mode=="both-baselines"){fixture::end_step=10;fixture::expected_recoveries=1;}
    else if(fixture::mode=="pending-preserved"){fixture::end_step=8;fixture::expected_recoveries=1;}
    else {fixture::end_step=4;fixture::expected_recoveries=0;}
    tk::net_watchdog_task(nullptr);CHECK(false);
}
