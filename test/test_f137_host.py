#!/usr/bin/env python3
"""UNRUN DRAFT: actual-body MQTT/network controlled host runner.

Nothing runs on import. --execute-controlled-host-only is mandatory. Compile only
in an exclusively owned compiler window. Real SDK registration/ABI, network,
resource fit, browser/hardware and tracker completion remain outside this layer.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import uuid
import time
from f137_process import run_owned
from f137_process_controls import run_controls

MQTT_CASES = (
    'helper-acquired', 'helper-adopted', 'helper-absent', 'helper-notfound',
    'helper-invalidstate', 'helper-unknown', 'helper-feed-failure',
    'publisher-owned', 'publisher-adopted', 'publisher-absent', 'publisher-park',
    'publisher-park-sink', 'publisher-iteration-throw', 'publisher-clock-throw',
)
NET_CASES = ('normal', 'observation-throw', 'partial-recovery', 'both-baselines',
             'pending-preserved', 'clock-throw', 'log-throw', 'fault-quota')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise ValueError('nonunique actual production mutation: '+before)
    return text.replace(before, after, 1)


def controls(mqtt, net, helper):
    prefix='''            watch = tk::LinkWatch{};
            watched_identity = {};
            baselines[0] = {};
            baselines[1] = {};'''
    diagnostic='''            try {
                tk::report_iteration_fault(faults,
                    static_cast<std::uint64_t>(esp_timer_get_time()) / 1000u,
                    [](std::uint32_t suppressed) {
                        ESP_LOGW(TAG, "network watchdog iteration failed; proof cleared (suppressed=%u)",
                                 static_cast<unsigned>(suppressed));
                    });
            } catch (...) {}'''
    # Exact matching text is owned source admission, not permissive fuzzy mutation.
    return (
        ('mqtt-no-explicit-release', 'mqtt', 'main/mqtt_ha.cpp',
         replace_once(mqtt, 'while (!watchdog.release()) {', 'while (false && !watchdog.release()) {'),
         'publisher-owned', 'CHECK'),
        ('mqtt-unknown-assumed-released', 'mqtt', 'main/task_watchdog_subscription.hpp',
         replace_once(helper, 'result != ESP_ERR_INVALID_STATE) return false;',
                      'result != ESP_ERR_INVALID_STATE) return true;'), 'publisher-park', 'CHECK'),
        ('mqtt-park-warning-invisible', 'mqtt', 'main/mqtt_ha.cpp',
         replace_once(mqtt, 'ESP_LOGE(TAG, "mqtt publisher parked:',
                      'ESP_LOGD(TAG, "mqtt publisher parked:'), 'publisher-park', 'CHECK'),
        ('mqtt-publish-replayed-while-parked', 'mqtt', 'main/mqtt_ha.cpp',
         replace_once(mqtt, 'watchdog.feed(true);', '(void)publish_discovery();\n      watchdog.feed(true);'),
         'publisher-park', 'CHECK'),
        ('net-stale-both-baselines', 'net', 'main/net.cpp',
         replace_once(net, '            baselines[0] = {};\n            baselines[1] = {};',
                      '            (void)baselines;'), 'both-baselines', 'CHECK'),
        ('net-live-ping-owner-discarded', 'net', 'main/net.cpp',
         replace_once(net, prefix, '            s_wd.session = nullptr;\n'+prefix),
         'pending-preserved', 'CHECK'),
        ('net-clock-catch-omitted', 'net', 'main/net.cpp',
         replace_once(net, diagnostic, diagnostic.replace('try {', '{', 1).replace('} catch (...) {}', '}')),
         'clock-throw', 'CHECK'),
        ('net-quota-refilled-after-success', 'net', 'main/net.cpp',
         replace_once(net, '        vTaskDelay(pdMS_TO_TICKS(kWdPeriodS * 1000));',
                      '        faults = {};\n        vTaskDelay(pdMS_TO_TICKS(kWdPeriodS * 1000));'),
         'fault-quota', 'CHECK'),
    )


def run(args):
    if not args.execute_controlled_host_only:
        raise SystemExit('explicit --execute-controlled-host-only required; untested draft')
    os.umask(0o077)
    started=datetime.datetime.now(datetime.timezone.utc).isoformat();begin=time.monotonic()
    deadline=begin+105.0  # Leave owned cleanup/receipts inside the unchanged outer120s contract.
    output=None;fixture=None;source=None;originals={};expected_fixture={};execution_inputs={};rows=[];stages=[]
    failure=None;restore_errors=[];readback_errors=[];processes_quiescent=True
    unchanged=None;restored=None;result=None
    def fail(error):
        return {'type':type(error).__name__,'message':str(error)}
    def identity(path):
        if path.is_symlink() or not path.is_file():raise ValueError('nonregular input: '+str(path))
        data=path.read_bytes()
        return {'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data),'executable':bool(path.stat().st_mode&0o111)}
    def verify_execution_inputs():
        errors=[]
        for name,expected in execution_inputs.items():
            try:
                if identity(Path(name))!=expected:errors.append(name+': drift')
            except BaseException as error:errors.append(name+': '+type(error).__name__)
        return errors
    def restore_one(name):
        if not processes_quiescent:raise RuntimeError('unknown owned group; fixture quarantined')
        path=fixture/name;path.write_bytes(originals[name])
        if path.read_bytes()!=originals[name]:raise RuntimeError('fixture restore readback: '+name)
        expected_fixture[name]=originals[name]
    try:
        source=args.source.resolve();tests=Path(__file__).resolve().parent
        output=(args.output or Path(tempfile.gettempdir())/('tesla-f137-'+uuid.uuid4().hex)).resolve()
        for protected in (source, tests):
            if output == protected or protected in output.parents:
                raise ValueError('test output must be outside qualified source/package')
        if output.exists():raise FileExistsError('refuse existing private output')
        output.mkdir(mode=0o700)
        fixture=output/'source';(fixture/'main').mkdir(parents=True)
        names=('main/mqtt_ha.cpp','main/net.cpp','main/task_watchdog_subscription.hpp','main/ble_client.cpp','main/ble_client.hpp')
        # No digest-only cache: snapshot actual include/source closure and all retained templates.
        input_paths={p for top in (source/'main',source/'test/stubs',tests)for p in top.rglob('*')if p.is_file() and p.suffix in ('.cpp','.hpp','.h','.py')}
        execution_inputs={str(p):identity(p)for p in sorted(input_paths)}
        for name in names:
            path=source/name;identity(path);originals[name]=path.read_bytes()
            (fixture/name).write_bytes(originals[name])
        expected_fixture.update(originals)
        env={k:v for k,v in os.environ.items() if not k.startswith(('GIT_','SSH_','IDF_','CCACHE_','DOCKER_','LD_','DYLD_','PYTHON','BASH_FUNC_')) and k not in ('OTA_SIGNING_KEY','OTA_SIGNING_KEY_FILE','GH_TOKEN','GITHUB_TOKEN','CPATH','CPLUS_INCLUDE_PATH','C_INCLUDE_PATH','CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS','SDKROOT','MACOSX_DEPLOYMENT_TARGET','ENV','BASH_ENV')}
        env['PYTHONDONTWRITEBYTECODE']='1'
        compiler=shutil.which(args.compiler)
        if not compiler:raise ValueError('host compiler missing')
        def command_stage(label,command,expected=0):
            nonlocal processes_quiescent
            prior_quiescence=processes_quiescent
            if not prior_quiescence:
                raise RuntimeError('refuse command launch without prior cleanup authority')
            # Revocation precedes launch and every receipt-processing/append failure window.
            processes_quiescent=False
            record=run_owned(command,cwd=source,env=env,output=output,label=label,
                             deadline=deadline,stage_seconds=60 if label.endswith(('generate','compile')) else 10)
            if (type(record) is not dict or type(record.get('processesQuiescent')) is not bool):
                raise ValueError('invalid owned-stage quiescence receipt')
            processes_quiescent=prior_quiescence and record['processesQuiescent']
            stages.append(record)
            if not processes_quiescent:raise RuntimeError('owned command quiescence unconfirmed; quarantine')
            if record['error'] or record['timeout'] or record['exit']!=expected:
                raise AssertionError((label,'stage failed',record['exit'],expected,record['error']))
            return record,Path(record['stdout']).read_bytes(),Path(record['stderr']).read_bytes()
        def phase(label,kind,cases,expected=0):
            for path,data in expected_fixture.items():
                if (fixture/path).read_bytes()!=data:raise AssertionError('unexpected pre-phase fixture drift: '+path)
            generated=output/(label+'.cpp');binary=output/(label+'.bin');phase_stages=[]
            generator=([sys.executable,'-B',str(tests/'f137-callback-probe-v1.py'),str(fixture),str(generated)] if kind=='callback' else [sys.executable,'-B',str(tests/'generate_f137_host_adapters.py'),str(fixture),str(generated),'--kind',kind])
            rec,_,_=command_stage(label+'-generate',generator);phase_stages.append(rec)
            command=[compiler,'-std=c++17','-Wall','-Wextra','-Werror','-I',str(tests),'-I',str(tests/'f137_stubs'),'-I',str(fixture/'main'),'-I',str(source/'test/stubs'),'-I',str(source/'main'),str(generated),str(source/'main/stack_watch.cpp'),'-o',str(binary)]
            if args.sanitize:command[1:1]=['-O1','-g','-fno-omit-frame-pointer','-fno-sanitize-recover=all','-fsanitize=address,undefined,leak','-DF137_WITH_LSAN=1']
            rec,_,_=command_stage(label+'-compile',command);phase_stages.append(rec)
            for case in cases:
                detector=case=='publisher-lsan-detector'
                wanted=42 if detector else expected
                rec,stdout,stderr=command_stage(label+'-'+(case or 'callback'),[str(binary)]+([case]if case else[]),wanted);phase_stages.append(rec)
                if wanted==42:
                    if b'FAIL line 'not in stderr:raise AssertionError('negative did not reach actual CHECK/detector')
                    if detector and (b'LeakSanitizer'not in stderr or b'F137 deliberate leak detector'not in stderr):
                        raise AssertionError('explicit recoverable LSan detector was ineffective')
                elif not stdout.startswith(b'PASS ') or stderr:raise AssertionError('positive missing complete host proof')
            for path,data in expected_fixture.items():
                if (fixture/path).read_bytes()!=data:raise AssertionError('unexpected post-phase fixture drift: '+path)
            rows.append({'label':label,'kind':kind,'expectedRuntimeExit':expected,'actualSourceIdentities':{n:sha(fixture/n)for n in names},'generatedSha256':sha(generated),'binarySha256':sha(binary),'stages':phase_stages})
        if args.controls:
            previous_quiescence=processes_quiescent
            # Missing/malformed control receipts cannot grant any restoration authority.
            processes_quiescent=False
            process_controls=run_controls(output,env,deadline)
            if (type(process_controls) is not dict or
                    type(process_controls.get('processesQuiescent')) is not bool or
                    type(process_controls.get('result')) is not str):
                raise ValueError('invalid inert process-controls receipt')
            processes_quiescent=previous_quiescence and process_controls['processesQuiescent']
            if not processes_quiescent:
                raise RuntimeError('inert controls quiescence unconfirmed; quarantine')
            if process_controls['result']!='PASS_INERT_PROCESS_CONTROLS_ONLY':
                raise AssertionError('inert owned-process controls failed')
        phase('candidate-callback','callback',(None,))
        phase('candidate-mqtt','mqtt',MQTT_CASES+(('publisher-lsan-detector',)if args.sanitize else()))
        phase('candidate-net','net',NET_CASES)
        if args.controls:
            text={n:d.decode()for n,d in originals.items()}
            for name,kind,path,mutation,case,_ in controls(text[names[0]],text[names[1]],text[names[2]]):
                if not processes_quiescent:raise RuntimeError('refuse next mutation without group quiescence')
                if mutation.encode()==originals[path]:raise AssertionError('ineffective source mutation')
                try:
                    expected_fixture[path]=mutation.encode()
                    (fixture/path).write_text(mutation)
                    phase(name,kind,(case,),42)
                finally:
                    # This executes for generator/compiler/runtime/check errors too, but never
                    # rewrites a fixture while an owned descendant may still be accessing it.
                    if processes_quiescent:restore_one(path)
                phase(name+'-restored',kind,(case,))
    except BaseException as error:
        failure=fail(error)
    finally:
        if fixture is not None and processes_quiescent:
            for name in originals:
                try:restore_one(name)
                except BaseException as error:restore_errors.append({'path':name,**fail(error)})
            checks=[]
            for name,data in originals.items():
                try:checks.append((fixture/name).read_bytes()==data)
                except BaseException as error:
                    checks.append(False);readback_errors.append({'path':name,**fail(error)})
            restored=len(originals)==5 and all(checks) and not restore_errors
        elif fixture is not None:
            restored=False  # Leave owned scratch quarantined; no speculative recovery.
        try:
            input_errors=verify_execution_inputs()
            unchanged=bool(execution_inputs) and not input_errors
        except BaseException as error:
            input_errors=[fail(error)];unchanged=False
        if restore_errors or readback_errors or input_errors or not processes_quiescent or (fixture is not None and not restored):
            if failure is None:failure={'type':'FinalizationFailure','message':'restore/source/quiescence failed'}
        result={'result':'FAIL'if failure else'PASS_HOST_LAYER_ONLY','failure':failure,'started':started,
                'finished':datetime.datetime.now(datetime.timezone.utc).isoformat(),'seconds':time.monotonic()-begin,
                'source':str(source)if source else None,'executionInputs':execution_inputs,
                'productionSourceUnchanged':unchanged,'fixtureRestored':restored,
                'processesQuiescent':processes_quiescent,'fixtureQuarantined':not processes_quiescent,
                'restoreErrors':restore_errors,'readbackErrors':readback_errors,'inputErrors':input_errors,
                'phases':rows,'allStages':stages,'negativeControlsRequested':args.controls,
                'explicitRecoverableLSanRequested':args.sanitize,
                'scope':'Actual complete task/helper/gateway/recovery with passive APIs. Sanitized task _Exit paths explicitly check recoverable leaks, with a deliberate detector; no SDK/ABI/network/resource/hardware/integration/head/closure proof.'}
        # Never overwrite an existing output directory's result after a refused fresh-output check.
        if output is not None and fixture is not None:
            (output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'result':result['result'],'phases':len(rows),'output':str(output)if output else None}))
    if failure:raise SystemExit(1)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,required=True);p.add_argument('--output',type=Path);p.add_argument('--compiler',default=os.environ.get('CXX','clang++'));p.add_argument('--controls',action='store_true');p.add_argument('--sanitize',action='store_true');p.add_argument('--execute-controlled-host-only',action='store_true');run(p.parse_args())


if __name__=='__main__':main()
