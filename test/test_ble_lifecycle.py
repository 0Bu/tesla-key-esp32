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
for name in ('ensure_scanning_', 'service_pending', 'terminate_published_link_', 'has_gap_link_',
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
static bool lock_available=true;
static unsigned yields=0,terminates=0;
static int terminate_result=0;
static uint16_t terminated_handle=0;
static int64_t now_us=0;
static constexpr uint16_t BLE_HS_CONN_HANDLE_NONE=0xffff;
static constexpr int BLE_ERR_REM_USER_CONN_TERM=19;
static constexpr int BLE_HS_EALREADY=2;
static constexpr int BLE_HS_ENOTCONN=7;
static constexpr TickType_t pdMS_TO_TICKS(uint32_t ms){return ms;}
static int64_t esp_timer_get_time(){return now_us;}
static void taskYIELD(){++yields;}
static int ble_gap_terminate(uint16_t handle,int reason){assert(reason==19);++terminates;terminated_handle=handle;return terminate_result;}
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
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
    std::atomic<uint16_t> conn_handle_{17};
    std::atomic<uint32_t> terminate_pending_generation_{tk::ble::kNoReadyGeneration};
    std::atomic<uint16_t> pending_terminate_handle_{BLE_HS_CONN_HANDLE_NONE};
    std::atomic<bool> disconnecting_{true},scan_pending_{false},want_connect_{false},connecting_{false},scanning_{false};
    std::atomic<uint32_t> ready_generation_{tk::ble::kNoReadyGeneration},connect_fail_count_{0};
    std::atomic<uint16_t> write_handle_{0},write_payload_size_{tk::kBleDefaultWritePayload};
    uint16_t notify_val_handle_{0},cccd_handle_{0};
    std::atomic<bool> conn_rssi_valid_{false};
    std::array<char, 18> peer_addr_{};
    struct ScanEntry { uint8_t addr[6]; char name[24]; int8_t rssi; int64_t last_us;
                       int64_t connectable_us; bool connectable; };
    std::array<ScanEntry, 12> scan_{};
    size_t scan_count_{0};
    std::array<char, 19> target_name_{};
    unsigned down_callbacks=0;
    void ensure_scanning_(TickType_t timeout=0);
    void service_pending();
    void terminate_published_link_(uint32_t expected_generation);
    bool has_gap_link_()const;
    int target_connectable()const;
    int target_connectable_since(int64_t since_us)const;
    void ensure_scanning_locked_(){if(want_connect_ && !disconnecting_ && conn_handle_==BLE_HS_CONN_HANDLE_NONE && !connecting_ && !scanning_){++scans;scanning_=true;}}
    struct Event {
        struct {int status=1;uint16_t conn_handle=BLE_HS_CONN_HANDLE_NONE;} connect;
        struct {struct {uint16_t conn_handle=BLE_HS_CONN_HANDLE_NONE;} conn;int reason=0;} disconnect;
    };
    bool (*on_connected_)(void*,bool,uint16_t,uint32_t)noexcept=nullptr;
    void* connected_context_=nullptr;
    void failed_connect(){Event value;auto* event=&value;do{ FAILED }while(false);}
    void contested_connect(uint16_t handle){Event value;value.connect.status=0;value.connect.conn_handle=handle;auto* event=&value;do{ CONNECT_CONTESTED }while(false);}
    void run_disconnect(uint16_t handle,int reason=0){Event value;value.disconnect.conn.conn_handle=handle;value.disconnect.reason=reason;auto* event=&value;do{ DISCONNECT }while(false);}
};
DEFINITIONS
static void reset(){lock_available=true;terminates=yields=0;terminate_result=0;terminated_handle=0;now_us=0;}
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
    {reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;c.want_connect_=true;c.connected_context_=&c;c.on_connected_=[](void* p,bool,uint16_t,uint32_t)noexcept{++static_cast<BleClient*>(p)->down_callbacks;return true;};c.failed_connect();assert(c.down_callbacks==0&&c.connect_fail_count_==1&&c.scans==1);}
    // [F-19] Contested connect (!intent) clears connecting_, saves orphan handle on terminate failure, and retries until terminal status.
    {reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;c.connecting_=true;lock_available=false;terminate_result=1;c.contested_connect(42);assert(!c.connecting_&&c.scan_pending_&&c.scans==0&&terminates==1&&terminated_handle==42&&c.pending_terminate_handle_==42);lock_available=true;c.service_pending();assert(!c.scan_pending_&&c.scans==0&&terminates==2&&c.pending_terminate_handle_==42);terminate_result=0;c.service_pending();assert(terminates==3&&terminated_handle==42&&c.pending_terminate_handle_==BLE_HS_CONN_HANDLE_NONE&&c.scans==0);}
    // [F-19] Contested connect (!intent) while want_connect_==true defers scan_pending_ and restarts scanning via service_pending().
    {reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;c.want_connect_=true;c.connecting_=true;c.scanning_=false;lock_available=false;terminate_result=0;c.contested_connect(46);assert(!c.connecting_&&c.scan_pending_&&c.scans==0);lock_available=true;c.service_pending();assert(!c.scan_pending_&&c.scans==1&&c.scanning_);}
    {reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;c.pending_terminate_handle_=43;terminate_result=BLE_HS_ENOTCONN;c.service_pending();assert(terminates==1&&terminated_handle==43&&c.pending_terminate_handle_==BLE_HS_CONN_HANDLE_NONE);}
    {reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;c.pending_terminate_handle_=44;terminate_result=BLE_HS_EALREADY;c.service_pending();assert(terminates==1&&terminated_handle==44&&c.pending_terminate_handle_==BLE_HS_CONN_HANDLE_NONE);}
    {reset();BleClient c;c.conn_handle_=BLE_HS_CONN_HANDLE_NONE;c.disconnecting_=false;c.connecting_=true;lock_available=false;terminate_result=BLE_HS_ENOTCONN;c.contested_connect(45);assert(!c.connecting_&&terminates==1&&c.pending_terminate_handle_==BLE_HS_CONN_HANDLE_NONE);}
    // [F-04] Contested disconnect preserves want_connect_ and defers scan reconciliation to service_pending().
    {reset();BleClient c;c.conn_handle_=17;c.disconnecting_=false;c.want_connect_=true;c.connecting_=true;c.scanning_=true;c.connected_context_=&c;c.on_connected_=[](void* p,bool,uint16_t,uint32_t)noexcept{++static_cast<BleClient*>(p)->down_callbacks;return true;};lock_available=false;c.run_disconnect(17);assert(c.want_connect_&&c.scan_pending_&&c.conn_handle_==BLE_HS_CONN_HANDLE_NONE&&!c.disconnecting_&&!c.connecting_&&!c.scanning_&&c.scans==0&&c.down_callbacks==1);lock_available=true;c.service_pending();assert(!c.scan_pending_&&c.scans==1&&c.scanning_);}
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
}
'''.replace('DEFINITIONS', definitions).replace('FAILED', failed).replace('CONNECT_CONTESTED', connect_contested).replace('DISCONNECT', disconnect_body)

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
print('BLE lifecycle: PASS (21 actual-method interleavings/contracts plus stale-generation negative control; passive SDK/RTOS only)')
