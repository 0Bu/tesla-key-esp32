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
for name in ('ensure_scanning_', 'service_pending', 'terminate_published_link_', 'has_gap_link_'):
    match = re.search(r'^(?:void|bool) BleClient::' + re.escape(name) + r'\(', source, re.M)
    body = function_body_in(source, name)
    methods.append(source[match.start():source.index(body, match.start())] + body)
definitions = '\n'.join(methods)
connect = source[source.index('case BLE_GAP_EVENT_CONNECT: {'):]
start = connect.index('if (event->connect.status != 0)')
end = connect.index('        bool canceled = false;')
failed = connect[start:end]

facade = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include "logic/ble_readiness.hpp"
#include "logic/ble_chunk.hpp"
using TickType_t=uint32_t;
static bool lock_available=true;
static unsigned yields=0,terminates=0;
static int terminate_result=0;
static uint16_t terminated_handle=0;
static constexpr uint16_t BLE_HS_CONN_HANDLE_NONE=0xffff;
static constexpr int BLE_ERR_REM_USER_CONN_TERM=19;
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
};
struct BleClient {
    int intent_mutex_=1;unsigned scans=0;
    ControlledGeneration connection_generation_;
    std::atomic<uint16_t> conn_handle_{17};
    std::atomic<uint32_t> terminate_pending_generation_{tk::ble::kNoReadyGeneration};
    std::atomic<bool> disconnecting_{true},scan_pending_{false},want_connect_{false},connecting_{false},scanning_{false};
    std::atomic<uint32_t> ready_generation_{tk::ble::kNoReadyGeneration},connect_fail_count_{0};
    unsigned down_callbacks=0;
    void ensure_scanning_(TickType_t timeout=0);
    void service_pending();
    void terminate_published_link_(uint32_t expected_generation);
    bool has_gap_link_()const;
    void ensure_scanning_locked_(){if(want_connect_ && !disconnecting_ && conn_handle_==BLE_HS_CONN_HANDLE_NONE && !connecting_ && !scanning_){++scans;scanning_=true;}}
    struct Event {struct {int status=1;} connect;};
    bool (*on_connected_)(void*,bool,uint16_t,uint32_t)noexcept=nullptr;
    void* connected_context_=nullptr;
    void failed_connect(){Event value;auto* event=&value;do{ FAILED }while(false);}
};
DEFINITIONS
static void reset(){lock_available=true;terminates=yields=0;terminate_result=0;terminated_handle=0;}
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
}
'''.replace('DEFINITIONS', definitions).replace('FAILED', failed)

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
print('BLE lifecycle: PASS (10 actual-method interleavings plus stale-generation negative control; passive SDK/RTOS only)')
