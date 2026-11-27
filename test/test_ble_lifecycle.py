#!/usr/bin/env python3
"""Compile actual lifecycle methods and force retirement/termination/scan interleavings."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_runtime_boundary_contract import function_body_in

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'main/ble_client.cpp').read_text()
methods = []
for name in ('ensure_scanning_', 'ensure_scanning_locked_', 'start_scan_locked_', 'cancel_scan_locked_',
             'cancel_connect_locked_', 'stop_connecting', 'service_pending',
             'terminate_published_link_', 'has_gap_link_',
             'target_connectable', 'target_connectable_since'):
    match = re.search(r'^(?:void|bool|int) BleClient::' + re.escape(name) + r'\(', source, re.M)
    body = function_body_in(source, name)
    methods.append(source[match.start():source.index(body, match.start())] + body)
definitions = '\n'.join(methods)
connect = source[source.index('case BLE_GAP_EVENT_CONNECT: {'):]
start = connect.index('if (event->connect.status != 0)')
end = connect.index('        bool canceled = false;')
failed = connect[start:end]
contested_start = connect.index('            tk::SemGuard intent(intent_mutex_, 0);')
contested_end = connect.index('            const tk::ble::ConnectLifecycle discovering =', contested_start)
connect_contested = connect[contested_start:contested_end]

disconnect_start = source.index('case BLE_GAP_EVENT_DISCONNECT: {')
disconnect_body_start = source.index('{', disconnect_start) + 1
disconnect_next = source.index('    case BLE_GAP_EVENT_NOTIFY_RX:', disconnect_body_start)
disconnect_body_end = source.rindex('}', disconnect_body_start, disconnect_next)
disconnect_body = source[disconnect_body_start:disconnect_body_end]

disc_complete_start = source.index('case BLE_GAP_EVENT_DISC_COMPLETE: {')
disc_complete_body_start = source.index('{', disc_complete_start) + 1
disc_complete_next = source.index('    default:', disc_complete_body_start)
disc_complete_body_end = source.rindex('}', disc_complete_body_start, disc_complete_next)
disc_complete_body = source[disc_complete_body_start:disc_complete_body_end]

facade = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include "logic/ble_readiness.hpp"
#include "logic/ble_chunk.hpp"
#include "logic/connect_outcome.hpp"
using TickType_t=uint32_t;
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
static int critical_depth=0;
static void portENTER_CRITICAL(portMUX_TYPE*){++critical_depth;}
static void portEXIT_CRITICAL(portMUX_TYPE*){assert(critical_depth>0);--critical_depth;}
static bool lock_available=true;
static unsigned yields=0,terminates=0,scan_attempts=0,scan_cancels=0,connect_cancels=0,warn_logs=0;
static int terminate_result=0,scan_start_result=0,scan_cancel_result=0,connect_cancel_result=0;
static uint16_t terminated_handle=0;
static int64_t now_us=0;
static constexpr uint16_t BLE_HS_CONN_HANDLE_NONE=0xffff;
static constexpr int BLE_ERR_REM_USER_CONN_TERM=19;
static constexpr int BLE_HS_EALREADY=2;
static constexpr int BLE_HS_EBUSY=6;
static constexpr int BLE_HS_ENOTCONN=7;
static constexpr uint8_t BLE_OWN_ADDR_PUBLIC=0;
static constexpr int32_t BLE_HS_FOREVER=0x7fffffff;
struct ble_gap_disc_params { uint8_t passive; uint8_t filter_duplicates; uint16_t itvl; uint16_t window; };
struct ble_gap_event;
static int gap_event_cb(ble_gap_event*, void*) { return 0; }
struct BleClient;
static int ble_gap_disc(uint8_t, int32_t, const ble_gap_disc_params*, int (*)(ble_gap_event*, void*), void*);
static int ble_gap_disc_cancel() { ++scan_cancels; return scan_cancel_result; }
static int ble_gap_conn_cancel() { ++connect_cancels; return connect_cancel_result; }
static constexpr TickType_t pdMS_TO_TICKS(uint32_t ms){return ms;}
static int64_t esp_timer_get_time(){return now_us;}
static void taskYIELD(){++yields;}
static int ble_gap_terminate(uint16_t handle,int reason){assert(reason==19);++terminates;terminated_handle=handle;return terminate_result;}
inline constexpr const char* TAG = "ble_client";
[[gnu::format(printf, 2, 3)]] static void test_logw(const char*, const char*, ...) { ++warn_logs; }
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(tag, ...) test_logw(tag, __VA_ARGS__)
namespace tk {struct SemGuard {bool acquired;explicit SemGuard(int,TickType_t=0):acquired(lock_available){} explicit operator bool()const{return acquired;}};}
struct ControlledGeneration {
    std::atomic<uint32_t> value{4};mutable unsigned reads=0;
    mutable std::function<void(unsigned)> hook;
    uint32_t load()const{++reads;if(hook)hook(reads);return value.load();}
    void store(uint32_t next){value.store(next);}
    uint32_t fetch_add(uint32_t delta){return value.fetch_add(delta);}
};
struct BleClient {
    int intent_mutex_=1;int scan_mutex_=1;int client_mutex_=1;unsigned scans=0;
    ControlledGeneration connection_generation_;
    portMUX_TYPE link_mux_=portMUX_INITIALIZER_UNLOCKED;
    std::atomic<uint16_t> conn_handle_{17};
    std::atomic<uint32_t> terminate_pending_generation_{tk::ble::kNoReadyGeneration};
    std::atomic<uint16_t> pending_terminate_handle_{BLE_HS_CONN_HANDLE_NONE};
    std::atomic<bool> disconnecting_{true},scan_pending_{false},want_connect_{false},connecting_{false},scanning_{false},host_synced_{true};
    std::atomic<int64_t> connect_started_us_{0};
    std::atomic<bool> cancel_connect_pending_{false};
    std::atomic<uint32_t> terminate_enotconn_streak_{0};
    std::atomic<int> last_scan_err_rc_{0};
    std::atomic<int64_t> last_scan_err_log_us_{0};
    std::atomic<uint32_t> ready_generation_{tk::ble::kNoReadyGeneration},connect_fail_count_{0};
    std::atomic<uint16_t> write_handle_{0},write_payload_size_{tk::kBleDefaultWritePayload};
    std::atomic<uint16_t> notify_val_handle_{0},cccd_handle_{0};
    std::atomic<bool> conn_rssi_valid_{false};
    std::array<char, 18> peer_addr_{};
    struct ScanEntry { uint8_t addr[6]; char name[24]; int8_t rssi; int64_t last_us;
                       int64_t connectable_us; bool connectable; };
    std::array<ScanEntry, 12> scan_{};
    size_t scan_count_{0};
    std::array<char, 19> target_name_{};
    unsigned down_callbacks=0;
    void ensure_scanning_(TickType_t timeout=0);
    void ensure_scanning_locked_();
    bool start_scan_locked_();
    bool cancel_scan_locked_();
    bool cancel_connect_locked_();
    void stop_connecting();
    void service_pending();
    void terminate_published_link_(uint32_t expected_generation);
    bool has_gap_link_()const;
    int target_connectable()const;
    int target_connectable_since(int64_t since_us)const;
    struct Event {
        struct {int status=1;uint16_t conn_handle=BLE_HS_CONN_HANDLE_NONE;} connect;
        struct {struct {uint16_t conn_handle=BLE_HS_CONN_HANDLE_NONE;} conn;int reason=0;} disconnect;
        struct {int reason=0;} disc_complete;
    };
    bool (*on_connected_)(void*,bool,uint16_t,uint32_t)noexcept=nullptr;
    void* connected_context_=nullptr;
    void failed_connect(){Event value;auto* event=&value;do{ FAILED }while(false);}
    void contested_connect(uint16_t handle){Event value;value.connect.status=0;value.connect.conn_handle=handle;auto* event=&value;do{ CONNECT_CONTESTED }while(false);}
    void run_disconnect(uint16_t handle,int reason=0){Event value;value.disconnect.conn.conn_handle=handle;value.disconnect.reason=reason;auto* event=&value;do{ DISCONNECT }while(false);}
    void run_disc_complete(int reason=0){Event value;value.disc_complete.reason=reason;auto* event=&value;(void)event;do{ __DISC_COMPLETE_BODY__ }while(false);}
};
static int ble_gap_disc(uint8_t, int32_t, const ble_gap_disc_params*, int (*)(ble_gap_event*, void*), void* arg) {
    ++scan_attempts;
    if (scan_start_result == 0) ++static_cast<BleClient*>(arg)->scans;
    return scan_start_result;
}
DEFINITIONS
static void reset(){lock_available=true;terminates=yields=scan_attempts=scan_cancels=connect_cancels=warn_logs=0;terminate_result=scan_start_result=scan_cancel_result=connect_cancel_result=0;terminated_handle=0;now_us=0;}
static void set_str(char* dst,const char* src){std::memcpy(dst,src,std::strlen(src)+1);}
int main(){
    {reset();BleClient c;c.terminate_published_link_(4);assert(terminates==1&&terminated_handle==17&&c.terminate_pending_generation_==tk::ble::kNoReadyGeneration);}
    {reset();BleClient c;terminate_result=1;c.terminate_published_link_(4);assert(c.terminate_pending_generation_==4&&c.disconnecting_);terminate_result=0;c.service_pending();assert(terminates==2&&c.terminate_pending_generation_==tk::ble::kNoReadyGeneration);}
    {reset();BleClient c;lock_available=false;c.terminate_published_link_(4);assert(terminates==0&&c.terminate_pending_generation_==4);lock_available=true;c.service_pending();assert(terminates==1);}
    // The old caller can resume only AFTER a new connection has already been published.
    {reset();BleClient c;c.connection_generation_.store(8);c.conn_handle_=99;c.disconnecting_=false;c.ready_generation_=8;c.terminate_published_link_(4);assert(terminates==0&&!c.disconnecting_&&c.ready_generation_==8);}
    // An old deferred request likewise cannot adopt a fresh link generation.
    {reset();BleClient c;c.terminate_pending_generation_=4;c.connection_generation_.store(8);c.conn_handle_=99;c.disconnecting_=false;c.ready_generation_=8;c.service_pending();assert(terminates==0&&!c.disconnecting_&&c.ready_generation_==8);}
    // Snapshot exhaustion retains an explicit owner; the next stable epoch can be terminated.
    {reset();BleClient c;c.connection_generation_.store(3);c.terminate_published_link_(3);assert(yields==8&&terminates==0&&c.terminate_pending_generation_==4);c.connection_generation_.store(4);c.service_pending();assert(terminates==1);}
    // Retirement after the last stable snapshot must not reassert a stranded disconnect flag.
    {reset();BleClient c;c.connection_generation_.store(3);c.connection_generation_.hook=[&](unsigned n){if(n==9)c.connection_generation_.store(4);if(n==10){c.connection_generation_.store(6);c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;}};c.terminate_published_link_(3);assert(terminates==0&&!c.disconnecting_);c.connection_generation_.hook={};c.service_pending();assert(!c.disconnecting_&&terminates==0);}
    // A stale task-side flag is repaired only with a stable absent handle, then a new intent scans.
    {reset();BleClient c;c.connection_generation_.store(6);c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.want_connect_=true;c.terminate_published_link_(4);c.service_pending();assert(!c.disconnecting_&&c.scans==1&&terminates==0);}
    // Zero-wait scan contention owns a retry, rather than orphaning the existing intent.
    {reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;c.want_connect_=true;lock_available=false;c.ensure_scanning_(0);assert(c.scan_pending_&&c.scans==0);lock_available=true;c.service_pending();assert(!c.scan_pending_&&c.scans==1);}
    // A connection that never came up must not emit a deferred LinkDown/session-reset callback.
    {reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;c.want_connect_=true;c.connect_started_us_=500;c.cancel_connect_pending_=true;c.connected_context_=&c;c.on_connected_=[](void* p,bool,uint16_t,uint32_t)noexcept{++static_cast<BleClient*>(p)->down_callbacks;return true;};c.failed_connect();assert(c.down_callbacks==0&&c.connect_fail_count_==1&&c.scans==1&&c.connect_started_us_==0&&!c.cancel_connect_pending_);}
    // [F-19] Contested connect (!intent) clears connecting_, saves orphan handle on terminate failure, and retries until terminal status.
    {reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;c.connecting_=true;c.connect_started_us_=500;c.cancel_connect_pending_=true;c.terminate_enotconn_streak_=5;lock_available=false;terminate_result=1;c.contested_connect(42);assert(!c.connecting_&&c.connect_started_us_==0&&!c.cancel_connect_pending_&&c.terminate_enotconn_streak_==0&&c.scan_pending_&&c.scans==0&&terminates==1&&terminated_handle==42&&c.pending_terminate_handle_==42);lock_available=true;c.service_pending();assert(!c.scan_pending_&&c.scans==0&&terminates==2&&c.pending_terminate_handle_==42);terminate_result=0;c.service_pending();assert(terminates==3&&terminated_handle==42&&c.pending_terminate_handle_==BLE_HS_CONN_HANDLE_NONE&&c.scans==0);}
    // [F-19] Contested connect (!intent) while want_connect_==true defers scan_pending_ and restarts scanning via service_pending().
    {reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;c.want_connect_=true;c.connecting_=true;c.scanning_=false;lock_available=false;terminate_result=0;c.contested_connect(46);assert(!c.connecting_&&c.scan_pending_&&c.scans==0);lock_available=true;c.service_pending();assert(!c.scan_pending_&&c.scans==1&&c.scanning_);}
    {reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;c.pending_terminate_handle_=43;terminate_result=BLE_HS_ENOTCONN;c.service_pending();assert(terminates==1&&terminated_handle==43&&c.pending_terminate_handle_==BLE_HS_CONN_HANDLE_NONE);}
    {reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;c.pending_terminate_handle_=44;terminate_result=BLE_HS_EALREADY;c.service_pending();assert(terminates==1&&terminated_handle==44&&c.pending_terminate_handle_==BLE_HS_CONN_HANDLE_NONE);}
    {reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;c.connecting_=true;lock_available=false;terminate_result=BLE_HS_ENOTCONN;c.contested_connect(45);assert(!c.connecting_&&terminates==1&&c.pending_terminate_handle_==BLE_HS_CONN_HANDLE_NONE);}
    // [F-04] Contested disconnect preserves want_connect_ and defers scan reconciliation to service_pending().
    {reset();BleClient c;c.conn_handle_=17;c.disconnecting_=false;c.want_connect_=true;c.connecting_=true;c.connect_started_us_=500;c.cancel_connect_pending_=true;c.terminate_enotconn_streak_=7;c.scanning_=true;c.connected_context_=&c;c.on_connected_=[](void* p,bool,uint16_t,uint32_t)noexcept{++static_cast<BleClient*>(p)->down_callbacks;return true;};lock_available=false;c.run_disconnect(17);assert(c.want_connect_&&c.scan_pending_&&c.conn_handle_==BLE_HS_CONN_HANDLE_NONE&&!c.disconnecting_&&!c.connecting_&&c.connect_started_us_==0&&!c.cancel_connect_pending_&&c.terminate_enotconn_streak_==0&&!c.scanning_&&c.scans==0&&c.down_callbacks==1);lock_available=true;c.service_pending();assert(!c.scan_pending_&&c.scans==1&&c.scanning_);}
    // [F-04] If stop_connecting()/disconnect() cleared want_connect_ while holding the lock, deferred reconcile stays idle.
    {reset();BleClient c;c.conn_handle_=17;c.disconnecting_=false;c.want_connect_=true;lock_available=false;c.run_disconnect(17);assert(c.want_connect_&&c.scan_pending_);c.want_connect_=false;lock_available=true;c.service_pending();assert(!c.scan_pending_&&c.scans==0&&!c.scanning_);}
    // [F-04] Disconnect of unpublished/orphan handle clears pending_terminate_handle_ without touching live state and schedules scan retry if want_connect_.
    {reset();BleClient c;c.conn_handle_=17;c.disconnecting_=false;c.want_connect_=true;c.scanning_=true;c.pending_terminate_handle_=42;c.connected_context_=&c;c.on_connected_=[](void* p,bool,uint16_t,uint32_t)noexcept{++static_cast<BleClient*>(p)->down_callbacks;return true;};c.run_disconnect(42);assert(c.pending_terminate_handle_==BLE_HS_CONN_HANDLE_NONE&&c.conn_handle_==17&&c.want_connect_&&c.scanning_&&c.scan_pending_&&c.down_callbacks==0&&c.connection_generation_.value==4);c.service_pending();assert(!c.scan_pending_&&c.scans==0);c.pending_terminate_handle_=99;c.run_disconnect(BLE_HS_CONN_HANDLE_NONE);assert(c.pending_terminate_handle_==99&&c.conn_handle_==17&&c.down_callbacks==0);}
    {reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;c.want_connect_=true;c.scanning_=false;c.pending_terminate_handle_=42;c.run_disconnect(42);assert(c.pending_terminate_handle_==BLE_HS_CONN_HANDLE_NONE&&c.scan_pending_&&c.scans==0);c.service_pending();assert(!c.scan_pending_&&c.scans==1&&c.scanning_);}
    // [F-05] target_connectable_since() picks newest valid slot across rotated RPA MACs regardless of array order.
    {
        reset();BleClient c;set_str(c.target_name_.data(),"S0123456789abcdefC");
        set_str(c.scan_[0].name,"S9999999999999999C");c.scan_[0].connectable=false;c.scan_[0].last_us=9000;c.scan_[0].connectable_us=9000;
        set_str(c.scan_[1].name,"S0123456789abcdefC");c.scan_[1].connectable=false;c.scan_[1].last_us=1000;c.scan_[1].connectable_us=1000;
        set_str(c.scan_[2].name,"S0123456789abcdefC");c.scan_[2].connectable=true;c.scan_[2].last_us=2000;c.scan_[2].connectable_us=2000;
        set_str(c.scan_[3].name,"S0123456789abcdefC");c.scan_[3].connectable=false;c.scan_[3].last_us=5000;c.scan_[3].connectable_us=0;
        c.scan_count_=4;
        assert(c.target_connectable_since(500)==1);
        std::swap(c.scan_[1],c.scan_[2]);
        assert(c.target_connectable_since(500)==1);
        c.scan_[1].connectable=false;c.scan_[1].connectable_us=3000;c.scan_[1].last_us=3000;
        assert(c.target_connectable_since(500)==0);
        std::swap(c.scan_[1],c.scan_[2]);
        assert(c.target_connectable_since(500)==0);
        // Tie-break equal connectable_us by highest last_us.
        c.scan_[1].connectable=true;c.scan_[1].connectable_us=4000;c.scan_[1].last_us=4000;
        c.scan_[2].connectable=false;c.scan_[2].connectable_us=4000;c.scan_[2].last_us=4500;
        assert(c.target_connectable_since(500)==0);
    }
    // [F-05] Uninitialized 0 timestamps must not match negative boot-window since_us.
    {
        reset();BleClient c;set_str(c.target_name_.data(),"S0123456789abcdefC");
        set_str(c.scan_[0].name,"S0123456789abcdefC");c.scan_[0].connectable=false;c.scan_[0].last_us=0;c.scan_[0].connectable_us=0;
        set_str(c.scan_[1].name,"S0123456789abcdefC");c.scan_[1].connectable=false;c.scan_[1].last_us=500;c.scan_[1].connectable_us=0;
        set_str(c.scan_[2].name,"S0123456789abcdefC");c.scan_[2].connectable=false;c.scan_[2].last_us=0;c.scan_[2].connectable_us=500;
        c.scan_count_=3;now_us=1000;
        assert(c.target_connectable_since(-89999000)==-1);
        assert(c.target_connectable()==-1);
        c.scan_[0].last_us=500;c.scan_[0].connectable_us=500;c.scan_[0].connectable=true;
        assert(c.target_connectable()==1);
    }
    // [#501] stop_connecting() cancels in-flight connecting_ across Canceled, AlreadyStopped, and Failed->service_pending() retry.
    {
        reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;
        c.want_connect_=true;c.connecting_=true;c.connect_started_us_=1000;
        connect_cancel_result=0;
        c.stop_connecting();
        assert(connect_cancels==1&&!c.want_connect_&&!c.connecting_&&c.connect_started_us_==0&&!c.cancel_connect_pending_&&terminates==0);
    }
    {
        reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;
        c.want_connect_=true;c.connecting_=true;c.connect_started_us_=1000;
        connect_cancel_result=BLE_HS_EALREADY;
        c.stop_connecting();
        assert(connect_cancels==1&&!c.want_connect_&&!c.connecting_&&c.connect_started_us_==0&&!c.cancel_connect_pending_);
    }
    {
        reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;
        c.want_connect_=true;c.connecting_=true;c.connect_started_us_=1000;
        connect_cancel_result=BLE_HS_ENOTCONN;
        c.stop_connecting();
        assert(connect_cancels==1&&!c.want_connect_&&!c.connecting_&&c.connect_started_us_==0&&!c.cancel_connect_pending_);
    }
    {
        reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;
        c.want_connect_=true;c.connecting_=true;c.connect_started_us_=1000;
        connect_cancel_result=1;
        c.stop_connecting();
        assert(connect_cancels==1&&warn_logs==1&&!c.want_connect_&&c.connecting_&&c.cancel_connect_pending_&&c.connect_started_us_==1000);
        c.service_pending();
        assert(connect_cancels==2&&warn_logs==1&&c.connecting_&&c.cancel_connect_pending_);
        connect_cancel_result=0;
        c.service_pending();
        assert(connect_cancels==3&&!c.connecting_&&!c.cancel_connect_pending_&&c.connect_started_us_==0&&c.scans==0);
    }
    // [#501] Stuck connecting_ >= 15 s is canceled in service_pending() and resumes scanning when want_connect_==true.
    {
        reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;
        c.want_connect_=true;c.connecting_=true;c.connect_started_us_=1000;
        now_us=1000+tk::ble::kStuckConnectingTimeoutUs-1;
        c.service_pending();
        assert(connect_cancels==0&&c.connecting_&&c.scans==0&&warn_logs==0);
        now_us=1000+tk::ble::kStuckConnectingTimeoutUs;
        c.service_pending();
        assert(connect_cancels==1&&warn_logs==1&&!c.connecting_&&c.connect_started_us_==0&&!c.cancel_connect_pending_&&c.scans==1&&c.scanning_);
    }
    // [#501] Even if ble_gap_conn_cancel() fails on stuck >= 15 s connecting_, service_pending() force-clears connecting_ so scanning recovers.
    {
        reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;
        c.want_connect_=true;c.connecting_=true;c.connect_started_us_=1000;
        connect_cancel_result=1;
        now_us=1000+tk::ble::kStuckConnectingTimeoutUs;
        c.service_pending();
        assert(connect_cancels==1&&warn_logs==2&&!c.connecting_&&c.connect_started_us_==0&&!c.cancel_connect_pending_&&c.scans==1&&c.scanning_);
    }
    // [#501] BLE_HS_EBUSY on start_scan_locked_() schedules ble_gap_conn_cancel() and restarts scan (with rate-limited warning).
    {
        reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;
        c.want_connect_=true;c.connecting_=false;c.scanning_=false;
        scan_start_result=BLE_HS_EBUSY;now_us=5000000;
        c.ensure_scanning_(0);
        assert(scan_attempts==1&&c.scans==0&&!c.scanning_&&c.cancel_connect_pending_&&warn_logs==1&&c.last_scan_err_rc_==BLE_HS_EBUSY);
        now_us=5000000+1000;
        c.ensure_scanning_(0);
        assert(scan_attempts==2&&warn_logs==1);
        now_us=5000000+tk::ble::kScanErrLogIntervalUs;
        c.ensure_scanning_(0);
        assert(scan_attempts==3&&warn_logs==2);
        scan_start_result=0;connect_cancel_result=0;
        c.service_pending();
        assert(connect_cancels==1&&!c.cancel_connect_pending_&&c.scans==1&&c.scanning_&&c.last_scan_err_rc_==0);
    }
    // [#501] Zombie conn_handle_ retires after kZombieTerminateThreshold BLE_HS_ENOTCONN responses, emits LinkDown, and resumes scanning.
    {
        reset();BleClient c;c.conn_handle_=17;c.disconnecting_=true;c.connection_generation_.store(4);
        c.terminate_pending_generation_=4;c.want_connect_=true;c.ready_generation_=4;
        c.write_handle_=9;c.notify_val_handle_=10;c.cccd_handle_=11;c.write_payload_size_=244;
        c.conn_rssi_valid_=true;c.scanning_=false;c.connecting_=false;
        uint32_t down_gen=0;
        c.connected_context_=&c;
        c.on_connected_=[](void* p,bool up,uint16_t h,uint32_t gen)noexcept{
            auto* self=static_cast<BleClient*>(p);
            assert(!up&&h==BLE_HS_CONN_HANDLE_NONE);
            ++self->down_callbacks;
            self->ready_generation_.store(gen);
            return true;
        };
        terminate_result=BLE_HS_ENOTCONN;
        for(uint32_t i=1;i<tk::ble::kZombieTerminateThreshold;++i){
            c.service_pending();
            assert(terminates==i&&c.terminate_enotconn_streak_==i&&c.conn_handle_==17&&c.disconnecting_&&c.down_callbacks==0&&c.scans==0);
        }
        c.service_pending();
        down_gen=c.ready_generation_.load();
        assert(terminates==tk::ble::kZombieTerminateThreshold&&down_gen==6&&warn_logs==1);
        assert(c.conn_handle_==BLE_HS_CONN_HANDLE_NONE&&c.write_handle_==0&&c.write_payload_size_==tk::kBleDefaultWritePayload);
        assert(c.notify_val_handle_==0&&c.cccd_handle_==0&&!c.connecting_&&c.connect_started_us_==0&&!c.cancel_connect_pending_);
        assert(!c.conn_rssi_valid_&&!c.disconnecting_&&c.terminate_enotconn_streak_==0&&c.terminate_pending_generation_==tk::ble::kNoReadyGeneration);
        assert(c.connection_generation_.value==6&&c.down_callbacks==1&&c.scans==1&&c.scanning_);
    }
    // [#501] Non-ENOTCONN terminate error resets ENOTCONN streak; contested lock defers zombie retirement cleanly.
    {
        reset();BleClient c;c.conn_handle_=17;c.disconnecting_=true;c.connection_generation_.store(4);
        c.terminate_pending_generation_=4;c.want_connect_=false;
        terminate_result=BLE_HS_ENOTCONN;
        for(uint32_t i=0;i<5;++i)c.service_pending();
        assert(c.terminate_enotconn_streak_==5);
        terminate_result=1;
        c.service_pending();
        assert(c.terminate_enotconn_streak_==0&&c.conn_handle_==17);
        terminate_result=BLE_HS_ENOTCONN;
        for(uint32_t i=0;i<tk::ble::kZombieTerminateThreshold-1;++i)c.service_pending();
        lock_available=false;
        c.service_pending();
        assert(c.conn_handle_==17&&c.disconnecting_);
        lock_available=true;
        c.service_pending();
        assert(c.conn_handle_==BLE_HS_CONN_HANDLE_NONE&&!c.disconnecting_&&c.scans==0&&!c.scanning_);
    }
    // [#501] BLE_GAP_EVENT_DISC_COMPLETE restarts scan only when idle with want_connect_.
    {
        reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;
        c.want_connect_=true;c.connecting_=false;c.scanning_=true;
        c.run_disc_complete(0);
        assert(c.scans==1&&c.scanning_&&!c.scan_pending_);
        // Contested lock defers to scan_pending_ and restarts on service_pending().
        lock_available=false;
        c.run_disc_complete(0);
        assert(!c.scanning_&&c.scan_pending_&&c.scans==1);
        lock_available=true;
        c.service_pending();
        assert(!c.scan_pending_&&c.scans==2&&c.scanning_);
        // When want_connect_ is false (manual discovery), DISC_COMPLETE leaves scanning idle.
        c.want_connect_=false;
        c.run_disc_complete(0);
        assert(!c.scanning_&&!c.scan_pending_&&c.scans==2);
        // When connecting_ or disconnecting_ or has_gap_link_, DISC_COMPLETE does not restart scan.
        c.want_connect_=true;c.connecting_=true;c.scanning_=true;
        c.run_disc_complete(0);
        assert(!c.scanning_&&!c.scan_pending_&&c.scans==2);
        c.connecting_=false;c.disconnecting_=true;c.scanning_=true;
        c.run_disc_complete(0);
        assert(!c.scanning_&&!c.scan_pending_&&c.scans==2);
        c.disconnecting_=false;c.conn_handle_=17;c.scanning_=true;
        c.run_disc_complete(0);
        assert(!c.scanning_&&!c.scan_pending_&&c.scans==2);
    }
    // [#501] Zombie retirement wins cleanly once: a late disconnect event for the retired handle does not double-retire or double-emit LinkDown.
    {
        reset();BleClient c;c.conn_handle_=29;c.connection_generation_.store(6);
        c.disconnecting_=true;c.terminate_enotconn_streak_=tk::ble::kZombieTerminateThreshold;
        c.notify_val_handle_=11;c.cccd_handle_=12;c.want_connect_=true;
        c.connected_context_=&c;
        c.on_connected_=[](void* p,bool up,uint16_t h,uint32_t gen)noexcept{
            assert(!up&&h==BLE_HS_CONN_HANDLE_NONE&&(gen%2)==0);
            ++static_cast<BleClient*>(p)->down_callbacks;
            return true;
        };
        c.service_pending();
        assert(c.conn_handle_==BLE_HS_CONN_HANDLE_NONE&&c.connection_generation_.load()==8);
        assert(c.notify_val_handle_==0&&c.cccd_handle_==0&&c.down_callbacks==1&&critical_depth==0);
        c.run_disconnect(29,0);
        assert(c.connection_generation_.load()==8&&c.down_callbacks==1&&c.scan_pending_&&critical_depth==0);
    }
    // [#501] Repeated cancel_connect_locked_() failures warn only once per failure sequence.
    {
        reset();BleClient c;c.connecting_=true;c.want_connect_=false;connect_cancel_result=-1;
        c.stop_connecting();
        assert(warn_logs==1&&c.cancel_connect_pending_);
        c.service_pending();
        assert(warn_logs==1&&c.cancel_connect_pending_);
    }
}
'''.replace('DEFINITIONS', definitions).replace('FAILED', failed).replace('CONNECT_CONTESTED', connect_contested).replace('DISCONNECT', disconnect_body).replace('__DISC_COMPLETE_BODY__', disc_complete_body)

compiler = shutil.which('c++')
assert compiler, 'C++ compiler is required'
with tempfile.TemporaryDirectory(prefix='ble-lifecycle-test-') as temporary:
    root = Path(temporary)
    def execute(code, name, succeeds):
        cpp = root / (name + '.cpp'); binary = root / name
        cpp.write_text(code)
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'main'),
                        str(cpp), '-o', str(binary)], check=True, timeout=60)
        result = subprocess.run([str(binary)], cwd=root, capture_output=True, timeout=10)
        assert (result.returncode == 0) == succeeds, (name, result.returncode, result.stderr.decode())
    execute(facade, 'actual', True)
    canary = 'expected_generation += expected_generation & 1U;'
    assert facade.count(canary) == 1
    execute(facade.replace(canary, 'expected_generation = connection_generation_.load();'), 'stale-adopts-new-link', False)
print('BLE lifecycle: PASS (33 actual-method interleavings/contracts plus stale-generation negative control; passive SDK/RTOS only)')
