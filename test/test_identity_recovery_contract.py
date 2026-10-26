#!/usr/bin/env python3
"""Retained positive complete-body draft checks with the exact qualified owned-process layer.

Runtime/UI product-omission plans are intentionally not executed or credited here. Their
exact first oracles and a mutation/restoration operator require separate qualification.
"""
from __future__ import annotations
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import stat
import sys
import tempfile
import time
from identity_recovery_process import run_owned

ROOT = Path(__file__).resolve().parents[1]
FATAL_HOOK = 'tk::runtime_admission_mark_fatal(tk::runtime_fatal_component(component));'


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_fatal_first(body: str, parser: dict) -> None:
    require(body.count(FATAL_HOOK) == 1, 'boot fatal must publish the bounded component exactly once')
    require(re.match(r"\{\s*" + re.escape(FATAL_HOOK), parser['scrub_cpp'](body)) is not None,
            'boot fatal component must be the first executable statement')


def require_consumers(root: Path) -> None:
    # The main integration belongs to the lead's concurrent boot work. Refuse a partial overlay;
    # do not silently substitute a fabricated boot body to make this adapter green.
    parser = runpy.run_path(str(root / 'test/test_runtime_boundary_contract.py'))
    body = parser['function_body_in']((root / 'main/main.cpp').read_text().replace('[[noreturn]]', ''), 'boot_fatal')
    require_fatal_first(body, parser)
    main_source = (root / 'main/main.cpp').read_text()
    halt = 'if (initial_key_action == tk::InitialKeyBootAction::HaltAfterEmptyRecovery)'
    generate = 'if (initial_key_action == tk::InitialKeyBootAction::Generate)'
    require('vehicle.has_key(), vehicle.key_rotation_recovered_at_boot()' in main_source and
            main_source.count(halt) == 1 and main_source.count(generate) == 1 and
            main_source.index(halt) < main_source.index(generate) and
            'boot_fatal("initial key recovery found no durable key");' in main_source[
                main_source.index(halt):main_source.index(generate)],
            'recovered empty identity must halt before any new initial generation')
    boot = parser['function_body_in']((root / 'main/vehicle_ctrl.cpp').read_text(),
                                      'recover_pending_key_rotation_at_boot_')
    require('tk::run_key_rotation_boot_cleanup(*storage_)' in boot and
            'cleanup.state() != tk::KeyRotationBootState::Ready' in boot and
            boot.index('cleanup.state() != tk::KeyRotationBootState::Ready') <
            boot.index('key_rotation_recovered_at_boot_ = true;'),
            'boot journal must reach confirmed cleanup before publishing recovery')
    http = (root / 'main/http_status.cpp').read_text()
    require('const auto runtime = tk::runtime_admission_snapshot();' in http and
            'in.runtime_state = runtime.state;' in http and 'in.fatal_component = runtime.component;' in http,
            'HTTP must consume one cached runtime/component snapshot')
    mqtt = (root / 'main/mqtt_ha.cpp').read_text()
    require('runtime_admission_snapshot()' in mqtt and 'payload.pairing_block_reason = s_vehicle->pairing_block_reason();' in mqtt,
            'MQTT must consume cached runtime and identity reason')



def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--compiler')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--negative-controls', action='store_true')
    args = parser.parse_args()
    require(not args.negative_controls,
            'runtime/UI omission operator is UNQUALIFIED; positive-only draft route')
    os.umask(0o077)
    begin = time.monotonic(); deadline = begin + 105
    output = None; owned_output = False; baseline = None; failure = None; stages = []; quiet = True
    initial = None; unchanged = False; generated = None; binary = None
    artifact_errors = []; tool_identities = {}
    compiler_selector = args.compiler or os.environ.get('CXX') or 'c++'
    env = {'PATH': '/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin',
           'LANG': 'C', 'LC_ALL': 'C', 'PYTHONDONTWRITEBYTECODE': '1'}
    if args.sanitize:
        env.update(ASAN_OPTIONS='detect_leaks=1:halt_on_error=1:abort_on_error=1',
                   UBSAN_OPTIONS='halt_on_error=1', LSAN_OPTIONS='exitcode=97')

    def identity(path):
        s = path.lstat()
        require(stat.S_ISREG(s.st_mode), 'nonregular identity-recovery input: '+str(path))
        body = path.read_bytes()
        return {'sha256': hashlib.sha256(body).hexdigest(), 'size': len(body),
                'mode': stat.S_IMODE(s.st_mode), 'executable': bool(s.st_mode & 0o111)}

    def snapshot():
        files = {}; directories = {}
        for root in (ROOT/'main', ROOT/'test/stubs'):
            for path in (root, *sorted(root.rglob('*'))):
                s = path.lstat()
                require(not stat.S_ISLNK(s.st_mode), 'source symlink is not an admitted recovery input')
                if stat.S_ISDIR(s.st_mode): directories[str(path)] = stat.S_IMODE(s.st_mode)
                else: files[str(path)] = identity(path)
        for name in ('test/test_identity_recovery_contract.py',
                     'test/generate_identity_recovery_adapter.py',
                     'test/test_identity_recovery.cpp', 'test/test_runtime_boundary_contract.py',
                     'test/identity_recovery_process.py', 'test/identity_recovery_omission_cases.json'):
            files[str(ROOT/name)] = identity(ROOT/name)
        return {'files': files, 'directories': directories}

    def command(label, argv, seconds):
        nonlocal quiet
        require(quiet, 'unknown process quiescence; refuse a next child')
        require(snapshot() == initial, 'source/include closure drift before owned command')
        prior = quiet; quiet = False
        try:
            record = run_owned(argv, cwd=ROOT, env=env, output=output, label=label,
                               deadline=deadline, stage_seconds=seconds)
            require(type(record) is dict and record.get('command') == argv and
                    type(record.get('processesQuiescent')) is bool,
                    'invalid owned command/quiescence receipt')
            cleanup = record.get('cleanup')
            require(type(cleanup) is dict and type(cleanup.get('reaped')) is bool and
                    type(cleanup.get('groupAbsent')) is bool and type(cleanup.get('errors')) is list,
                    'invalid owned cleanup receipt')
            if record['processesQuiescent']:
                require(cleanup['reaped'] is True and cleanup['groupAbsent'] is True and
                        cleanup['errors'] == [], 'false owned cleanup acknowledgement')
            for stream in ('stdout', 'stderr'):
                path = output/(label+'.'+stream)
                require(record.get(stream) == str(path), 'unexpected retained stream path')
                data = path.read_bytes()
                require(len(data) <= 256*1024 and
                        record.get('retainedBytes', {}).get(stream) == len(data) and
                        hashlib.sha256(data).hexdigest() == record.get(stream+'Sha256'),
                        'retained owned stream identity mismatch')
            require(json.loads((output/(label+'.stage.json')).read_text()) == record,
                    'owned terminal receipt readback mismatch')
            stages.append(record)
        except BaseException:
            quiet = False  # Returned/processing/append failures cannot grant cleanup authority.
            raise
        quiet = prior and record['processesQuiescent']
        require(quiet, 'owned group is unconfirmed; retain quarantined outputs')
        require(record.get('exit') == 0 and record.get('timeout') is False and
                record.get('error') is None, 'positive owned command failed: '+label)
        require(snapshot() == initial, 'source/include closure drift after owned command')
        return (output/(label+'.stdout')).read_bytes(), (output/(label+'.stderr')).read_bytes()

    try:
        if args.output is None:
            temporary_root = Path(tempfile.gettempdir()).resolve()
            require(temporary_root != ROOT and ROOT not in temporary_root.parents,
                    'temporary output root must be outside qualified source')
            output = Path(tempfile.mkdtemp(prefix='identity-recovery-positive-', dir=temporary_root)).resolve()
            owned_output = True
        else:
            output = args.output.resolve()
            require(output != ROOT and ROOT not in output.parents,
                    'retained output must be outside qualified source')
            output.mkdir(mode=0o700)
            owned_output = True
        baseline = output/'baseline'; baseline.mkdir(mode=0o700)
        initial = snapshot()
        require(initial['files'][str(ROOT/'test/identity_recovery_process.py')]['sha256'] ==
                '31dcf5236899f61f8f33177cf7dd34d20de5aeb90629b219e7e86fce7b47d40b',
                'owned-process helper differs from the separately qualified module')
        require_consumers(ROOT)
        # Actual source-only first-hook oracle, independent of compiling the Main shell.
        parsed = runpy.run_path(str(ROOT/'test/test_runtime_boundary_contract.py'))
        body = parsed['function_body_in']((ROOT/'main/main.cpp').read_text().replace('[[noreturn]]',''), 'boot_fatal')
        for label, mutant, expected in (
                ('missing', body.replace(FATAL_HOOK, '', 1), 'exactly once'),
                ('late', body.replace(FATAL_HOOK, '', 1).replace('fatal_startup_rollback();',
                          FATAL_HOOK+'\n    fatal_startup_rollback();', 1), 'first executable statement')):
            try: require_fatal_first(mutant, parsed)
            except AssertionError as error:
                require(expected in str(error), 'wrong first-hook source oracle: '+label)
            else: raise AssertionError('ineffective first-hook source canary: '+label)
        require_fatal_first(body, parsed)
        cxx = shutil.which(compiler_selector, path=env['PATH'])
        require(cxx is not None, 'identity recovery fixture requires an explicit C++17 compiler')
        tool_identities = {'compiler': {'path':cxx, **identity(Path(cxx).resolve())},
                           'python': {'path':sys.executable, **identity(Path(sys.executable).resolve())}}
        generated = baseline/'identity_recovery_adapter.generated.hpp'
        command('generate', [sys.executable, '-I', '-B',
                str(ROOT/'test/generate_identity_recovery_adapter.py'), str(ROOT), str(generated)], 20)
        flags = ['-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-pthread']
        if args.sanitize: flags += ['-fsanitize=address,undefined,leak','-fno-omit-frame-pointer','-fno-sanitize-recover=all']
        binary = baseline/'identity_recovery_tests'
        command('compile', [cxx,*flags,'-I'+str(ROOT/'test/stubs'),'-I'+str(ROOT/'main'),
                '-I'+str(baseline),str(ROOT/'test/test_identity_recovery.cpp'),'-o',str(binary)], 60)
        stdout, stderr = command('positive-runtime', [str(binary)], 15)
        require(stdout.startswith(b'identity recovery actual-body checks ') and not stderr,
                'actual positive matrix missing or diagnostic failure')
        unchanged = snapshot() == initial
        require(unchanged, 'source/include closure changed at positive terminal')
    except BaseException as error:
        failure = {'type':type(error).__name__,'message':str(error)}
    finally:
        if initial is not None:
            try: unchanged = snapshot() == initial
            except BaseException: unchanged = False
        if not quiet or not unchanged:
            if failure is None: failure = {'type':'FinalizationFailure','message':'unknown quiescence or source drift'}
        artifacts = {}
        for name, path in (('generated',generated), ('binary',binary)):
            if not quiet:
                artifacts[name] = {'unqualified':'unknown process quiescence'}
                continue
            try: artifacts[name] = identity(path) if path is not None and path.is_file() else None
            except BaseException as error:
                artifacts[name] = None
                artifact_errors.append({'artifact':name,'type':type(error).__name__})
                if failure is None: failure = {'type':'ArtifactReadbackFailure','message':name}
        result = {'result':'FAIL' if failure else 'POSITIVE_HOST_LAYER_ONLY',
                  'failure':failure,'source':str(ROOT),'sourceBefore':initial,
                  'sourceUnchanged':unchanged,'processesQuiescent':quiet,
                  'outputQuarantined':not quiet,'stages':stages,
                  'generated':artifacts['generated'],'binary':artifacts['binary'],
                  'artifactErrors':artifact_errors,'tools':tool_identities,
                  'runtimeAndUiOmissions':'NOT_RUN_UNQUALIFIED','sanitize':args.sanitize,
                  'seconds':time.monotonic()-begin,
                  'scope':'Positive actual-body and first-hook source canaries only. No SDK, runtime mutation effectiveness, UI negative, current-head, hardware or F142 closure.'}
        if owned_output:
            (output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        # Never restore/delete a source fixture or private output, including after timeout.
    print(json.dumps({'result':result['result'],'output':str(output),
                      'runtimeAndUiOmissions':result['runtimeAndUiOmissions']}))
    if failure: raise SystemExit(1)


if __name__ == '__main__': main()
