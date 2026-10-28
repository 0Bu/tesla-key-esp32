// UNCOMPILED/UNRUN DRAFT: complete actual publisher + actual helper, passive APIs.
#include <atomic>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <utility>
#include <string>
#include <type_traits>
#include <vector>
#include "f137_task_lsan.hpp"
#include "freertos/task.h"
#include "task_watchdog_subscription.hpp"
#include "mqtt_publish_sequence.hpp"
#include "stack_watch.hpp"

static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(42);}}while(false)
namespace fixture {
std::string mode;
std::vector<std::string> trace;
unsigned adds{},statuses{},deletes{},feeds{},samples{},discoveries{},states{},availability{},delays{},park_warnings{},stopped_warnings{},subscribe_warnings{};
bool present{},throw_feed{},throw_sink{};
TickType_t ticks{};
std::vector<int> remove_results;
std::size_t remove_index{};
constexpr int throwing=-999;
void record(const char* s){trace.emplace_back(s);}
void log(int level,const char*,const char* format,...){
    if(level<2)return;
    if(std::strstr(format,"parked:")){++park_warnings;record("park-warning");}
    if(std::strstr(format,"stopped after")){CHECK(!present);++stopped_warnings;record("stopped-warning");}
    if(std::strstr(format,"could not subscribe"))++subscribe_warnings;
    if(throw_sink)throw std::runtime_error("passive logger");
}
[[noreturn]] void finish(){
    CHECK(!present); // Must be true BEFORE actual self-delete; no destructor may fix it.
    CHECK(samples >= 3);
    CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::Mqtt) == 4096u);
    CHECK(discoveries<2 && states<2); // Cleanup cannot replay a round.
    if(mode=="publisher-park" || mode=="publisher-park-sink"){
        CHECK(deletes==3 && park_warnings==1 && stopped_warnings==1);
        CHECK(delays==2 && discoveries==0 && availability==0 && states==0);
        CHECK(feeds==3); // One initial feed and two uncertain-cleanup attempts.
        CHECK(trace[trace.size()-1]=="stopped-warning");
    }else{
        CHECK(deletes==1 && park_warnings==0 && stopped_warnings==1);
        if(mode=="publisher-absent")CHECK(subscribe_warnings==1 && feeds==0);
        if(mode=="publisher-adopted")CHECK(statuses==1 && feeds==1);
        if(mode=="publisher-iteration-throw")CHECK(discoveries==1 && availability==0 && states==0);
        if(mode=="publisher-clock-throw")CHECK(discoveries==0 && states==0);
    }
    const bool detector = mode == "publisher-lsan-detector";
    const int leaks = f137_task_leak_check(detector);
    if (detector && leaks != 0) {
        std::fprintf(stderr, "FAIL line %d: F137 deliberate leak detector confirmed before _Exit\n", __LINE__);
        std::fflush(stderr); std::_Exit(42); // Avoid an extra atexit pass changing this witness exit.
    }
    CHECK(leaks == 0);
    if (detector) CHECK(false); // An ineffective leak probe is never accepted as a passing detector.
    std::printf("PASS %u actual MQTT/WDThelper checks (%s); self-delete exits without unwinding\n",checks,mode.c_str());
    std::fflush(stdout);std::_Exit(0);
}
}
#define ESP_LOGE(tag,...) fixture::log(3,tag,__VA_ARGS__)
#define ESP_LOGW(tag,...) fixture::log(2,tag,__VA_ARGS__)
#define ESP_LOGD(tag,...) fixture::log(0,tag,__VA_ARGS__)
static const char* TAG="mqtt";
static unsigned s_interval_s=5;
static std::atomic<bool> s_connected{false},s_need_discovery{false};
static std::string s_avail="public-test/availability";

esp_err_t esp_task_wdt_add(TaskHandle_t task){
    CHECK(task==nullptr);++fixture::adds;fixture::record("add");
    if(fixture::mode.find("adopted")!=std::string::npos){fixture::present=true;return ESP_FAIL;}
    if(fixture::mode.find("absent")!=std::string::npos)return ESP_FAIL;
    fixture::present=true;return ESP_OK;
}
esp_err_t esp_task_wdt_status(TaskHandle_t task){CHECK(task==nullptr);++fixture::statuses;return fixture::present?ESP_OK:ESP_ERR_NOT_FOUND;}
esp_err_t esp_task_wdt_reset(){++fixture::feeds;fixture::record("feed");if(fixture::throw_feed)throw std::runtime_error("feed");return fixture::present?ESP_OK:ESP_ERR_NOT_FOUND;}
esp_err_t esp_task_wdt_delete(TaskHandle_t task){
    CHECK(task==nullptr);++fixture::deletes;fixture::record("delete-attempt");
    int result=ESP_OK;
    if(fixture::remove_index<fixture::remove_results.size())result=fixture::remove_results[fixture::remove_index++];
    if(result==fixture::throwing)throw std::runtime_error("delete");
    if(result==ESP_OK || result==ESP_ERR_NOT_FOUND || result==ESP_ERR_INVALID_STATE)fixture::present=false;
    return result;
}
extern "C" TickType_t xTaskGetTickCount(){if(fixture::mode=="publisher-clock-throw")throw std::runtime_error("clock");return fixture::ticks;}
extern "C" void vTaskDelay(TickType_t ticks){
    fixture::ticks+=ticks;fixture::record(ticks==1000?"park-delay":"iteration-delay");
    if(ticks==1000){++fixture::delays;CHECK(fixture::deletes<3);}
    if(ticks==500)throw std::runtime_error("controlled outer boundary from platform delay");
}
extern "C" void vTaskDelete(TaskHandle_t task){CHECK(task==nullptr);fixture::finish();}
extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task){
    CHECK(task==nullptr);++fixture::samples;return 4096u;
}
static bool publish_discovery(){++fixture::discoveries;fixture::record("discovery");if(fixture::mode=="publisher-iteration-throw")throw std::bad_alloc();return true;}
static bool publish_state(){++fixture::states;fixture::record("state");return true;}
static bool pub(const char*,const char*){++fixture::availability;fixture::record("availability");return true;}

// ACTUAL_COMPLETE_BODIES

static int helper_case(){
    using tk::TaskWatchdogSubscription;
    static_assert(!std::is_copy_constructible<TaskWatchdogSubscription>::value);
    static_assert(!std::is_move_constructible<TaskWatchdogSubscription>::value);
    static_assert(noexcept(std::declval<TaskWatchdogSubscription&>().subscribe()));
    static_assert(noexcept(std::declval<TaskWatchdogSubscription&>().release()));
    TaskWatchdogSubscription guard;
    const bool subscribed=guard.subscribe();
    if(fixture::mode=="helper-absent")CHECK(!subscribed);else CHECK(subscribed);
    CHECK(fixture::adds==1);
    CHECK(!guard.subscribe() && fixture::adds==1);
    guard.feed();CHECK(fixture::feeds==(subscribed?1u:0u));
    if(fixture::mode=="helper-adopted")CHECK(fixture::statuses==1);
    if(fixture::mode=="helper-unknown"){
        fixture::remove_results={ESP_FAIL,fixture::throwing,ESP_OK};
        CHECK(!guard.release());CHECK(fixture::present);
        guard.feed(true);CHECK(!guard.release());CHECK(fixture::present);
        guard.feed(true);CHECK(guard.release());
    }else{
        if(fixture::mode=="helper-notfound")fixture::remove_results={ESP_ERR_NOT_FOUND};
        if(fixture::mode=="helper-invalidstate")fixture::remove_results={ESP_ERR_INVALID_STATE};
        CHECK(guard.release());
    }
    CHECK(!fixture::present);const unsigned deleted=fixture::deletes;
    CHECK(guard.release() && fixture::deletes==deleted);
    CHECK(!guard.subscribe() && fixture::adds==1);
    const unsigned fed=fixture::feeds;guard.feed(true);CHECK(fixture::feeds==fed);
    std::printf("PASS %u actual helper checks (%s)\n",checks,fixture::mode.c_str());return 0;
}
int main(int argc,char**argv){
    CHECK(argc==2);fixture::mode=argv[1];
    if(fixture::mode.find("helper-")==0){if(fixture::mode=="helper-feed-failure")fixture::throw_feed=true;return helper_case();}
    if(fixture::mode=="publisher-park"||fixture::mode=="publisher-park-sink"){
        fixture::remove_results={ESP_FAIL,fixture::throwing,ESP_OK};fixture::throw_feed=true;
        fixture::throw_sink=fixture::mode=="publisher-park-sink";
    }
    if(fixture::mode=="publisher-iteration-throw"){s_connected=true;s_need_discovery=true;}
    if(fixture::mode=="publisher-clock-throw"){s_connected=true;s_need_discovery=false;}
    if(fixture::mode=="publisher-absent")fixture::remove_results={ESP_ERR_NOT_FOUND};
    publisher_task(nullptr);
    CHECK(false); // Real publisher cannot return past task self-delete.
}
