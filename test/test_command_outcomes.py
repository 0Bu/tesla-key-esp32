#!/usr/bin/env python3
"""Run actual charging producers and enqueue helpers with a passive native facade."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile

from test_runtime_boundary_contract import function_body_in

ROOT = Path(__file__).resolve().parents[1]


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
static uint32_t xTaskGetTickCount(){return now_ms;}
#define pdTICKS_TO_MS(x) (x)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
namespace tk {
struct SemGuard { explicit SemGuard(int, uint32_t=0){} explicit operator bool() const{return true;} };
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
    struct CommandOutcome {bool success=false;std::string error;};
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
    std::function<void(tk::OperationResult)> make_result_cb_(std::shared_ptr<CommandCompletion> completion,uint32_t){
        return [this,completion](tk::OperationResult result){++callback_count;completion->outcome.success=result.is_success();if(result.error())completion->outcome.error=result.error()->message();};
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
    bool charge_start(int timeout_ms);
    bool charge_stop(int timeout_ms);
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
        const bool actual=kind==0?controller.charge_start(20000):kind==1?controller.charge_stop(20000):controller.set_charge_limit(80,20000);
        const bool expected=authentication && counter && (kind==0?sample.start:kind==1?sample.stop:false);
        assert(actual==expected && controller.published.success==expected);
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
    return checks==74?0:1;
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
    print("command outcomes: PASS (74 actual native producer/helper/runner cases; passive transport/storage/RTOS)")


if __name__ == "__main__":
    main()
