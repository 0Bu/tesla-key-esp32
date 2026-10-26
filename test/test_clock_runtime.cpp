// Actual extracted clock shell and date getters. RTOS/libc/storage are passive deterministic
// doubles. This does not emulate SDK timekeeping, NVS durability or a live SNTP connection.
#include "stack_watch.hpp"
#include "time_sync.hpp"
#include "logic/nvs_contract.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/time.h>

static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
// Passive own-task SDK I/O only: the real stack_watch.cpp owns encoding/CAS/minimum logic.
// The host stub ABI is UBaseType_t=unsigned, TaskHandle_t=void*; no SDK/target ABI is claimed.
static unsigned f135_hwm_queries = 0;
extern "C" unsigned uxTaskGetStackHighWaterMark(void* task) {
    CHECK(task == nullptr); ++f135_hwm_queries; return 4096;
}
static bool held, busy, set_fail, set_throw, save_fail, save_throw;
static int takes, gives, sets, saves, logs, weak_notices;
static int64_t monotonic_us, wall_seconds, save_elapsed_us;
static int32_t wall_microseconds;
static std::function<void()> before_take, after_time_sample;
using StaticSemaphore_t = int;
using SemaphoreHandle_t = StaticSemaphore_t*;
constexpr int pdTRUE = 1;
#define pdMS_TO_TICKS(x) (x)
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) { return storage; }
static bool xSemaphoreTake(SemaphoreHandle_t, int timeout) {
    CHECK(timeout == 100);
    if (before_take) { auto callback = std::move(before_take); before_take = {}; callback(); }
    ++takes;
    if (busy || held) return false;
    held = true; return true;
}
static void xSemaphoreGive(SemaphoreHandle_t) { CHECK(held); held = false; ++gives; }
static int64_t esp_timer_get_time() { return monotonic_us; }
static const char* TAG = "clock-test";
struct AppDescription { const char* date = "Oct 20 2026"; };
static const AppDescription* esp_app_get_description() { static AppDescription value; return &value; }
[[noreturn]] static void boot_fatal(const char*) { std::abort(); }
template <typename... Args> static void clock_log(Args...) { CHECK(!held); ++logs; }
#define ESP_LOGI(...) clock_log(__VA_ARGS__)
#define ESP_LOGW(...) clock_log(__VA_ARGS__)
#define ESP_LOGE(...) clock_log(__VA_ARGS__)
extern "C" int clock_test_settimeofday(const timeval* value, const void*) {
    ++sets;
    if (set_throw) throw std::bad_alloc();
    if (set_fail) return -1;
    wall_seconds=value->tv_sec; wall_microseconds=value->tv_usec; return 0;
}
extern "C" int clock_test_gettimeofday(timeval* out, void*) {
    out->tv_sec=wall_seconds; out->tv_usec=wall_microseconds; return 0;
}
extern "C" int clock_test_adjtime(const timeval*, timeval*) { std::abort(); }
static time_t clock_test_time(time_t*) {
    const time_t sampled=wall_seconds;
    if (after_time_sample) { auto callback=std::move(after_time_sample); after_time_sample={}; callback(); }
    return sampled;
}
#ifdef CLOCK_TEST_PINNED_SNTP
extern "C" {
int sntp_get_sync_mode(void);
void sntp_set_sync_mode(int);
void sntp_set_sync_status(int);
void sntp_set_time_sync_notification_cb(void (*)(timeval*));
void sntp_set_system_time(uint32_t, uint32_t);
int clock_fixture_status(void);
}
#else
static int sync_mode, sync_status;
static int sntp_get_sync_mode() { return sync_mode; }
static void sntp_set_sync_mode(int mode) { sync_mode=mode; }
static void sntp_set_sync_status(int status) { CHECK(!held); sync_status=status; }
static int clock_fixture_status() { return sync_status; }
#endif
constexpr int SNTP_SYNC_MODE_IMMED=0, SNTP_SYNC_MODE_SMOOTH=1;
constexpr int SNTP_SYNC_STATUS_RESET=0, SNTP_SYNC_STATUS_COMPLETED=1;
struct NvsStorageAdapter {
    std::map<std::string,std::string> text;
    bool load_str(const char* key,std::string& out) { CHECK(!held); auto it=text.find(key); if (it==text.end()) return false; out=it->second; return true; }
    bool save_str(const char* key,const std::string& value) {
        CHECK(!held); ++saves;
        if (save_throw) throw std::bad_alloc();
        if (save_fail) return false;
        text[key]=value; monotonic_us+=save_elapsed_us; return true;
    }
};
struct VehicleController {
    NvsStorageAdapter* storage_;
    std::atomic<bool> key_created_untrusted_{false};
    bool session{true};
    bool has_session() { return session; }
    time_t key_created_at();
    time_t paired_at();
};
#define settimeofday clock_test_settimeofday
#define gettimeofday clock_test_gettimeofday
#define time clock_test_time
#include "clock_adapters.generated.hpp"
#undef time
#undef gettimeofday
#undef settimeofday

static NvsStorageAdapter storage;
static void observe_weak(timeval*) { ++weak_notices; }
static void deliver(int64_t seconds=1800000000, int32_t micros=123000) {
#ifdef CLOCK_TEST_PINNED_SNTP
    // The actual pinned IDF caller resolves its weak seam to our production strong function.
    sntp_set_system_time(static_cast<uint32_t>(seconds),static_cast<uint32_t>(micros));
#else
    timeval sample{static_cast<time_t>(seconds),static_cast<suseconds_t>(micros)};
    sntp_sync_time(&sample);
#endif
}
static void reset() {
    held=busy=set_fail=set_throw=save_fail=save_throw=false;
    takes=gives=sets=saves=logs=weak_notices=0;
    wall_seconds=1700000000; wall_microseconds=0; monotonic_us=1000000;
    before_take={}; after_time_sample={}; storage.text.clear(); save_elapsed_us=0;
    s_ntp_synced.store(false); s_clock_checkpoint={}; s_cfg_store=&storage;
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    initialize_clock_gate_();
#ifdef CLOCK_TEST_PINNED_SNTP
    sntp_set_time_sync_notification_cb(observe_weak);
#else
    (void)observe_weak;
#endif
}
int main() {
    CHECK(!tk::stack_watch_min_free_bytes(tk::StackWatch::SntpSync).has_value());
    reset();
    auto browser=apply_browser_clock(1750000000123.0);
    CHECK(browser.status==BrowserClockStatus::Applied && sets==1 && saves==0);
    CHECK(wall_seconds==1750000000 && wall_microseconds==123000 && !clock_is_authoritative());
    deliver();
    CHECK(sets==2 && saves==1 && weak_notices==0 && clock_is_authoritative());
    CHECK(clock_fixture_status()==SNTP_SYNC_STATUS_COMPLETED && !held && takes==gives);
    CHECK(wall_seconds==1800000000 && wall_microseconds==123000);
    browser=apply_browser_clock(1750000000000.0);
    CHECK(browser.status==BrowserClockStatus::AlreadyNtpSynced && sets==2);
    CHECK(apply_browser_clock(0).status==BrowserClockStatus::AlreadyNtpSynced && sets==2);

    reset(); before_take=[] { deliver(); };
    CHECK(apply_browser_clock(1750000000000.0).status==BrowserClockStatus::AlreadyNtpSynced);
    CHECK(sets==1 && saves==1 && wall_seconds==1800000000);
    reset(); before_take=[] { monotonic_us+=250; };
    deliver(1800000000,999900);
    CHECK(sets==1 && wall_seconds==1800000001 && wall_microseconds==150 && clock_is_authoritative());
    reset(); before_take=[] { monotonic_us+=200; };
    deliver(clock_plausibility_ceiling()-1,999900);
    CHECK(sets==0 && saves==0 && !clock_is_authoritative() && !held);
    reset(); before_take=[] { --monotonic_us; }; deliver();
    CHECK(sets==0 && saves==0 && !clock_is_authoritative() && !held);
    reset(); before_take=[] {
        CHECK(apply_browser_clock(1750000000000.0).status==BrowserClockStatus::Applied);
        CHECK(!clock_is_authoritative());
    };
    deliver();
    CHECK(sets==2 && saves==1 && wall_seconds==1800000000 && clock_is_authoritative());
    reset(); deliver(); deliver(1699999999);
    CHECK(sets==1 && saves==1 && clock_is_authoritative() && clock_fixture_status()==SNTP_SYNC_STATUS_RESET);
    CHECK(apply_browser_clock(1750000000000.0).status==BrowserClockStatus::AlreadyNtpSynced);
    reset(); save_elapsed_us=2000000; deliver();
    CHECK(s_clock_checkpoint.last_success_us==static_cast<uint64_t>(monotonic_us) && saves==1);
    monotonic_us+=tk::ClockCheckpointPolicy::kSuccessIntervalUs-1; deliver(); CHECK(saves==1);
    ++monotonic_us; deliver(); CHECK(saves==2);
    reset(); busy=true; deliver();
    CHECK(!clock_is_authoritative() && sets==0 && saves==0 && !held);
    CHECK(apply_browser_clock(1750000000000.0).status==BrowserClockStatus::Busy);
    reset(); CHECK(apply_browser_clock(0).status==BrowserClockStatus::Invalid && sets==0);
    for (int fault : {1,2}) {
        reset(); set_fail=fault==1; set_throw=fault==2; deliver();
        CHECK(sets==1 && saves==0 && !clock_is_authoritative() && !held);
        CHECK(clock_fixture_status()==SNTP_SYNC_STATUS_RESET);
        CHECK(apply_browser_clock(1750000000000.0).status==BrowserClockStatus::SetFailed && !held);
    }
    reset(); deliver(); set_fail=true; deliver();
    CHECK(clock_is_authoritative() && sets==2 && saves==1);
    CHECK(apply_browser_clock(1750000000000.0).status==BrowserClockStatus::AlreadyNtpSynced);
    for (int mode : {0,1,2}) {
        reset();
        if (mode==0) sntp_sync_time(nullptr);
        if (mode==1) deliver(1800000000,1000000);
        if (mode==2) { sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH); deliver(); }
        CHECK(sets==0 && saves==0 && !clock_is_authoritative() && weak_notices==0);
    }
    for (bool throws : {false,true}) {
        reset(); save_throw=throws; save_fail=!throws; deliver();
        CHECK(saves==1 && clock_is_authoritative() && !s_clock_checkpoint.succeeded);
        monotonic_us+=tk::ClockCheckpointPolicy::kAttemptIntervalUs-1; deliver(); CHECK(saves==1);
        ++monotonic_us; save_throw=save_fail=false; deliver();
        CHECK(saves==2 && s_clock_checkpoint.succeeded);
        monotonic_us+=tk::ClockCheckpointPolicy::kSuccessIntervalUs-1; deliver(); CHECK(saves==2);
        ++monotonic_us; deliver(); CHECK(saves==3);
    }
    reset(); storage.text[tk::nvs_contract::kLastTime]="1800000000";
    restore_clock_from_nvs(storage); CHECK(sets==0); // warm time is never moved to the cache
    wall_seconds=0; restore_clock_from_nvs(storage);
    CHECK(sets==1 && wall_seconds==1800000000 && !clock_is_authoritative());
    for (const char* bad : {"", "0", "-1", "1800000000x", "99999999999999999999", "2082758400"}) {
        reset(); wall_seconds=0; storage.text[tk::nvs_contract::kLastTime]=bad;
        restore_clock_from_nvs(storage); CHECK(sets==0 && !clock_is_authoritative());
    }
    reset(); wall_seconds=0; set_fail=true; storage.text[tk::nvs_contract::kLastTime]="1800000000";
    restore_clock_from_nvs(storage); CHECK(sets==1 && wall_seconds==0 && !clock_is_authoritative());

    reset(); VehicleController vehicle{&storage};
    after_time_sample=[] { deliver(); }; // old getter sampled browser time, then NTP committed
    CHECK(vehicle.paired_at()==0 && saves==0 && !clock_is_authoritative());
    after_time_sample={}; deliver();
    CHECK(vehicle.paired_at()==1800000000 && saves==2);
    CHECK(storage.text[tk::nvs_contract::kPairedAt]=="1800000000");
    for (const char* bad : {"-1", "1600000000", "1800000000x", "2082758400"}) {
        reset(); vehicle.key_created_untrusted_.store(false);
        storage.text[tk::nvs_contract::kKeyCreated]=bad;
        storage.text[tk::nvs_contract::kPairedAt]=bad;
        CHECK(vehicle.key_created_at()==0 && vehicle.paired_at()==0 && saves==0);
    }
    reset(); storage.text[tk::nvs_contract::kKeyCreated]="1600000001";
    storage.text[tk::nvs_contract::kPairedAt]="1600000001";
    CHECK(vehicle.key_created_at()==1600000001 && vehicle.paired_at()==1600000001 && saves==0);
    vehicle.key_created_untrusted_.store(true); CHECK(vehicle.key_created_at()==0);
    CHECK(f135_hwm_queries > 0);
    CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::SntpSync) == 4096u);
    std::printf("OK %d actual clock/getter checks; pinned caller=%d\n",checks,
#ifdef CLOCK_TEST_PINNED_SNTP
        1
#else
        0
#endif
    );
}
