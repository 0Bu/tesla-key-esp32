#!/usr/bin/env python3
"""Run actual charging producers and enqueue helpers with a passive native facade."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile

from test_runtime_boundary_contract import balanced_end, function_body_in

ROOT = Path(__file__).resolve().parents[1]


def check_enqueue_deadlines(compiler: str, env: dict) -> None:
    """Execute all three actual admission blocks after a positive budget expires under the lock."""
    commands = (ROOT / 'main/vehicle_commands.cpp').read_text()
    telemetry = (ROOT / 'main/vehicle_telemetry.cpp').read_text()
    blocks = []
    for name, source in (('send_vcsec_locked_', commands), ('send_infotainment_locked_', commands),
                         ('get_vehicle_status', telemetry)):
        body = function_body_in(source, name)
        match = re.search(r'try\s*\{\s*tk::SemGuard g\(vehicle_mutex_\);', body)
        assert match, name
        opening = body.index('{', match.start())
        block = body[opening:balanced_end(body, opening, '{', '}', name) + 1]
        assert block.count('if (timeout_ms == 0)') == 1, name
        blocks.append(block)
    facade = r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <string>
#include "logic/command_runner.hpp"
static uint32_t clock_ms=1000, lock_delay=0;
static uint32_t xTaskGetTickCount(){return clock_ms;}
#define pdTICKS_TO_MS(x) (x)
namespace tk {struct SemGuard{explicit SemGuard(int){clock_ms+=lock_delay;}};}
static constexpr int VCSEC_InformationRequestType_INFORMATION_REQUEST_TYPE_GET_STATUS=1;
namespace TeslaBLE {struct Client{int build_vcsec_information_request_message(int,uint8_t*,size_t*){return 0;}};}
struct Probe{
 struct Outcome{bool success=false;std::string error;};
 struct Completion{uint32_t command_id=0,cmd_id=0;std::atomic<bool> completed{false};std::string error;int sem=1;};
 using Builder=std::function<int(TeslaBLE::Client*,uint8_t*,size_t*)>;
 int vehicle_mutex_=1;unsigned callback_installs=0;
 uint32_t deadline=1100;std::string name="probe";
 tk::WakePolicy wp=tk::WakePolicy::NoWakeFail;
 bool auth_fail_is_health_probe=false,completes_on_transmit=false;
 uint32_t max_retries=3;tk::VcsecReplyKind kind=tk::VcsecReplyKind::Action;
 tk::CommandEffect effect=tk::CommandEffect::Mutating;
 tk::CommandOutcomePolicy policy=tk::CommandOutcomePolicy::AlreadySetOnly;
 tk::CommandRunner command_runner_;
 std::array<Builder,tk::CommandRunner::kMaxQueueSize> command_builders_;
 std::function<void()> vehicle_status_callback_;
 uint32_t remaining_ms_(uint32_t d){return clock_ms>=d?0:d-clock_ms;}
 template<class... A> auto make_result_cb_(A&&...){++callback_installs;return [](bool,const std::string&,tk::CommandFailureInfo){};}
 static void xSemaphoreGive(int){}
 METHODS
};
int main(){
 for(int kind=0;kind<3;++kind)for(bool expire:{false,true}){
  clock_ms=1000;lock_delay=expire?100:10;Probe p;
  bool ok=kind==0?p.vcsec().success:kind==1?p.info().success:p.status();
  assert(ok==!expire);
  assert(bool(p.command_runner_.current_command())==!expire);
  assert(bool(p.vehicle_status_callback_)==(!expire && kind==2));
  assert(p.callback_installs==unsigned(!expire && kind!=2));
  unsigned builders=0;for(const auto& b:p.command_builders_)builders+=bool(b);
  assert(builders==unsigned(!expire));
 }
}
'''
    methods = []
    for index, (name, block) in enumerate(zip(('vcsec', 'info', 'status'), blocks)):
        result_type = 'bool' if index == 2 else 'Outcome'
        prefix = ('[[maybe_unused]] Outcome out; [[maybe_unused]] uint32_t cmd_id=0,generation=1; '
                  'auto completion=std::make_shared<Completion>(); '
                  '[[maybe_unused]] Builder builder=[](TeslaBLE::Client*,uint8_t*,size_t*){return 0;}; '
                  '[[maybe_unused]] std::function<void()> callback=[]{}; ')
        methods.append(f'{result_type} {name}(){{ {prefix}\n{block}\n' +
                       ('return true;' if index == 2 else 'return {true,{}};') + '}')
    with tempfile.TemporaryDirectory(prefix='enqueue-deadlines-') as temporary:
        cpp=Path(temporary)/'probe.cpp'; binary=Path(temporary)/'probe'
        for omitted in (-1, 0, 1, 2):
            selected = list(methods)
            if omitted >= 0:
                selected[omitted] = selected[omitted].replace('if (timeout_ms == 0)', 'if (false)', 1)
            cpp.write_text(facade.replace('METHODS', '\n'.join(selected)))
            subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),str(cpp),'-o',str(binary)],
                           env=env,check=True,capture_output=True,timeout=60)
            result=subprocess.run([str(binary)],cwd=temporary,env=env,capture_output=True,timeout=10)
            assert (result.returncode==0)==(omitted==-1), (omitted,result.stderr)
    print('enqueue deadlines: PASS (all three actual admission blocks, positive budget expired during mutex wait; three omission controls)')


def main() -> None:
    source = (ROOT / "main/vehicle_commands.cpp").read_text()
    header = (ROOT / "main/vehicle_ctrl.hpp").read_text()
    # The two actual producers own the only explicit non-default policies in this shell.
    start_body = function_body_in(source, "charge_start")
    stop_body = function_body_in(source, "charge_stop")
    outside = source.replace(start_body, "", 1).replace(stop_body, "", 1)
    assert not re.search(r"CommandOutcomePolicy::(?:ChargeStart|ChargeStop)", outside)
    definitions = []
    for name in ("send_infotainment_", "send_infotainment_locked_", "charge_start", "charge_stop", "set_charge_limit"):
        body = function_body_in(source, name)
        matches = list(re.finditer(r"(?m)^(?:bool|VehicleController::CommandOutcome) VehicleController::" + re.escape(name) + r"\([^{};]+\)\s*\{", source))
        assert len(matches) == 1, name
        match = matches[0]
        signature = source[match.start():match.end() - 1]
        definitions.append(signature + body)
    declarations = []
    for name in ("send_infotainment_", "send_infotainment_locked_"):
        matches = list(re.finditer(r"(?m)^\s*(?:bool|CommandOutcome) " + re.escape(name) + r"\([^;]+;", header))
        assert len(matches) == 1, name
        declarations.append(matches[0][0])
    facade = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "logic/command_runner.hpp"
#include "logic/connect_outcome.hpp"
using WakePolicy=tk::WakePolicy;
static uint32_t now_ms=1000;
static uint32_t vehicle_lock_delay=0;
static uint32_t xTaskGetTickCount(){return now_ms;}
#define pdTICKS_TO_MS(x) (x)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
namespace tk {
struct SemGuard { explicit SemGuard(int which, uint32_t=0){if(which==2)now_ms+=vehicle_lock_delay;} explicit operator bool() const{return true;} };
struct InFlightGuard { std::atomic<bool>& flag; explicit InFlightGuard(std::atomic<bool>& value):flag(value){assert(!flag);flag=true;} ~InFlightGuard(){flag=false;} };
}
static constexpr int CarServer_VehicleAction_chargingStartStopAction_tag=1;
static constexpr int CarServer_VehicleAction_chargingSetLimitAction_tag=2;
namespace TeslaBLE {
struct Client {
    int calls=0, tag=0, value=0;
    int build_car_server_vehicle_action_message(uint8_t*,size_t* length,int action,const void* input){
        ++calls;tag=action;value=action==1?int(*static_cast<const bool*>(input)):*static_cast<const int32_t*>(input);*length=1;return 0;
    }
};
}
struct VehicleController {
    using Builder=std::function<int(TeslaBLE::Client*,uint8_t*,size_t*)>;
    struct CommandOutcome {bool success=false;std::string error;tk::CommandNoOp no_op=tk::CommandNoOp::None;};
    struct CommandCompletion {void* sem=reinterpret_cast<void*>(1);uint32_t command_id=0;CommandOutcome outcome;};
    int command_mutex_=1,vehicle_mutex_=2;
    std::atomic<bool> cmd_in_flight_{false};std::atomic<uint32_t> last_cmd_ticks_{0};
    tk::CommandRunner command_runner_;
    std::array<Builder,tk::CommandRunner::kMaxQueueSize> command_builders_;
    TeslaBLE::Client client;
    tk::CommandOutcomePolicy observed=tk::CommandOutcomePolicy::AlreadySetOnly;
    CommandOutcome published;unsigned published_count=0,callback_count=0,payload_count=0;
    std::string reason="complete";bool authenticated=true,has_counter=true,reply_success=false;
    uint32_t deadline_in_(uint32_t ms){return now_ms+ms;}
    uint32_t ticks_until_(uint32_t deadline){return deadline-now_ms;}
    int remaining_ms_(uint32_t deadline){return static_cast<int>(deadline-now_ms);}
    uint32_t capped_deadline_(uint32_t deadline,uint32_t){return deadline;}
    bool command_identity_ready_(){return true;}
    bool ensure_connected_until_(uint32_t,tk::ConnectOrigin){return true;}
    void publish_command_outcome_(const CommandOutcome& out){published=out;++published_count;}
    std::shared_ptr<CommandCompletion> begin_completion_(uint32_t& generation){generation=1;return std::make_shared<CommandCompletion>();}
    std::function<void(bool,const std::string&,tk::CommandFailureInfo)> make_result_cb_(std::shared_ptr<CommandCompletion> completion,uint32_t){
        return [this,completion](bool success,const std::string& reason,tk::CommandFailureInfo info){++callback_count;completion->outcome.success=success;completion->outcome.no_op=success?info.no_op:tk::CommandNoOp::None;if(!success)completion->outcome.error=reason;};
    }
    void invalidate_and_flush_(uint32_t){assert(false);}
    CommandOutcome await_completion_(std::shared_ptr<CommandCompletion> completion,uint32_t,uint32_t,const char*,tk::CompletionTimeoutPolicy){
        assert(cmd_in_flight_);
        auto* command=command_runner_.current_command();assert(command && command->id==completion->command_id);
        observed=command->outcome_policy;
        assert(command->effect==tk::CommandEffect::Mutating);
        command_runner_.vcsec_session().set_established({1},10,now_ms);
        command_runner_.info_session().set_established({2},20,now_ms);
        assert(command_runner_.tick(now_ms,true,true)==tk::TxAction::SendCommandPayload);
        uint8_t passive[1]{};size_t length=sizeof(passive);
        assert(command_builders_[command->id%tk::CommandRunner::kMaxQueueSize](&client,passive,&length)==0);
        assert(client.calls==1 && length==1);++payload_count;
        command_runner_.notify_tx_complete(tk::TxAction::SendCommandPayload,now_ms+10);
        const auto uuid=command->uuid;
        assert(command_runner_.handle_response(tk::BleDomain::Infotainment,uuid.data(),uuid.size(),has_counter,100,reply_success,reason,authenticated).routed);
        assert(callback_count==1);
        assert(command_runner_.tick(now_ms+20,true,true)==tk::TxAction::None);
        return completion->outcome;
    }
    bool charge_start(int timeout_ms,tk::CommandNoOp* no_op=nullptr);
    bool charge_stop(int timeout_ms,tk::CommandNoOp* no_op=nullptr);
    bool set_charge_limit(int percent,int timeout_ms);
DECLARATIONS
};
DEFINITIONS
int main(){
    using Policy=tk::CommandOutcomePolicy;
    struct Case{const char* reason;bool start,stop;};
    const Case cases[]={{"is_charging",true,false},{"complete",true,false},{"not_charging",false,true},
        {"Infotainment action failed: complete",true,false},{"requested",false,false},{"complete_suffix",false,false}};
    unsigned checks=0;
    for(const auto& sample:cases)for(bool authentication:{false,true})for(bool counter:{false,true})for(int kind=0;kind<3;++kind){
        VehicleController controller;controller.reason=sample.reason;controller.authenticated=authentication;controller.has_counter=counter;
        tk::CommandNoOp no_op=tk::CommandNoOp::AlreadySet;
        const bool actual=kind==0?controller.charge_start(20000,&no_op):kind==1?controller.charge_stop(20000,&no_op):controller.set_charge_limit(80,20000);
        const bool expected=authentication && counter && (kind==0?sample.start:kind==1?sample.stop:false);
        assert(actual==expected && controller.published.success==expected);
        if(kind!=2)assert(no_op==(expected?tk::admitted_command_noop(sample.reason):tk::CommandNoOp::None));
        assert(controller.observed==(kind==0?Policy::ChargeStart:kind==1?Policy::ChargeStop:Policy::AlreadySetOnly));
        assert(controller.client.tag==(kind==2?2:1) && controller.client.value==(kind==0?1:kind==1?0:80));
        assert(controller.published_count==1 && controller.callback_count==1 && controller.payload_count==1);
        assert(!controller.command_runner_.current_command() && !controller.cmd_in_flight_);
        for(const auto& builder:controller.command_builders_)assert(!builder);
        ++checks;
    }
    for(bool success:{false,true}){
        VehicleController controller;controller.reply_success=success;controller.reason=success?"":"already_set";
        assert(controller.set_charge_limit(80,20000));assert(controller.observed==Policy::AlreadySetOnly);
        ++checks;
    }
    for(int kind=0;kind<2;++kind){
        now_ms=1000;vehicle_lock_delay=20000;
        VehicleController controller;tk::CommandNoOp no_op=tk::CommandNoOp::AlreadySet;
        assert(!(kind==0?controller.charge_start(20000,&no_op):controller.charge_stop(20000,&no_op)));
        assert(no_op==tk::CommandNoOp::None && !controller.command_runner_.current_command());
        assert(controller.client.calls==0 && controller.callback_count==0);
        assert(controller.published.error=="command deadline exhausted");
        ++checks;
    }
    return checks==76?0:1;
}
'''.replace("DECLARATIONS", "\n".join(declarations)).replace("DEFINITIONS", "\n".join(definitions))
    compiler = shutil.which("c++")
    assert compiler, "C++ compiler required"
    env = os.environ.copy()
    for name in tuple(env):
        if name in ("OTA_SIGNING_KEY", "OTA_SIGNING_KEY_FILE", "GH_TOKEN", "GITHUB_TOKEN", "CXX", "CXXFLAGS", "CPPFLAGS", "CPATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH"):
            env.pop(name, None)
    with tempfile.TemporaryDirectory(prefix="command-outcomes-") as tmp:
        cpp=Path(tmp)/"test.cpp";binary=Path(tmp)/"test"
        cpp.write_text(facade)
        subprocess.run([compiler,"-std=c++17","-Wall","-Wextra","-Werror","-I",str(ROOT/"main"),str(cpp),"-o",str(binary)],env=env,check=True,timeout=60)
        subprocess.run([str(binary)],env=env,check=True,timeout=10)
    check_enqueue_deadlines(compiler, env)
    print("command outcomes: PASS (76 actual native producer/helper/runner cases, request-owned no-op and mutex-wait expiry; passive transport/storage/RTOS)")


if __name__ == "__main__":
    main()
