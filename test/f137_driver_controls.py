#!/usr/bin/env python3
"""Inert error-path controls of the complete actual F137 outer driver.

Only disposable fixtures and Python operator copies are modified. No compiler,
command child, firmware, SDK or device runs. Imports only happen on explicit call.
"""
import argparse
import ast
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import types


def run_driver_controls(root):
    root = Path(root)
    root.mkdir(mode=0o700)
    driver = Path(__file__).with_name('test_f137_host.py')
    original = driver.read_bytes()
    old = b'            processes_quiescent=False\n            process_controls=run_controls(output,env,deadline)'
    if original.count(old) != 1:
        raise ValueError('nonunique pessimistic receipt-guard anchor')
    mutant = original.replace(old, b'            # Valid omission of pessimistic quarantine.\n            process_controls=run_controls(output,env,deadline)', 1)
    package = root / 'package'
    package.mkdir(mode=0o700)
    copy = package / 'driver-copy.py'
    source = root / 'source-inputs'
    (source / 'main').mkdir(parents=True)
    (source / 'test/stubs').mkdir(parents=True)
    names = ('mqtt_ha.cpp', 'net.cpp', 'task_watchdog_subscription.hpp', 'ble_client.cpp', 'ble_client.hpp')
    originals = {name: ('// inert input '+name+'\n').encode() for name in names}
    for name, body in originals.items():
        (source / 'main' / name).write_bytes(body)
    rows = []
    module_counter = 0

    def case(label, mode, expect_quiet, expect_restored):
        nonlocal module_counter
        output = root / label
        writes = []
        stages = []
        module_counter += 1
        spec = importlib.util.spec_from_file_location('f137_driver_control_'+str(module_counter), copy)
        module = importlib.util.module_from_spec(spec)
        # Do not reuse a stale pyc when the valid source omission is restored.
        code = compile(copy.read_bytes(), str(copy), 'exec')
        exec(code, module.__dict__)
        orig_write, orig_read = Path.write_bytes, Path.read_bytes
        orig_which = module.shutil.which
        fixture_main = output / 'source/main'
        witness = b'quarantined private fixture witness\n'
        stdout = io.StringIO()

        def fixture(path):
            return path.parent == fixture_main and path.name in originals

        def write_spy(path, body):
            if fixture(path):
                writes.append(path.name)
                if mode == 'restore-write-error' and len(writes) > 5:
                    raise OSError('inert restoration write failure')
            return orig_write(path, body)

        def read_spy(path):
            if mode == 'restore-readback-error' and fixture(path) and len(writes) > 5:
                raise OSError('inert restoration readback failure')
            return orig_read(path)

        def controls(_output, _env, _deadline):
            if mode in ('raise-unknown', 'returned-false', 'malformed', 'receipt-write-error', 'receipt-read-error'):
                orig_write(fixture_main / names[0], witness)
            if mode == 'raise-unknown':
                raise RuntimeError('inert control observed unknown cleanup then lost receipt')
            if mode == 'receipt-write-error':
                blocked = output / 'receipt-is-directory'
                blocked.mkdir()
                blocked.write_text('cannot write this receipt')
            if mode == 'receipt-read-error':
                missing = output / 'removed-receipt.json'
                missing.write_text('{}')
                missing.unlink()
                missing.read_text()
            return {'processesQuiescent': False if mode == 'returned-false' else 'False' if mode == 'malformed' else True,
                    'result': 'FAIL' if mode == 'returned-known-failure' else 'PASS_INERT_PROCESS_CONTROLS_ONLY'}

        def no_command(command, **kwargs):
            if mode == 'stage-receipt-interruption':
                orig_write(fixture_main / names[0], witness)
            stages.append(command)
            out, err = output / 'inert-stage.stdout', output / 'inert-stage.stderr'
            out.write_bytes(b'')
            err.write_bytes(b'inert confirmed pre-compiler stop\n')
            return {'processesQuiescent': mode != 'stage-receipt-interruption',
                    'error': {'message': 'inert stop'},
                    'timeout': False, 'exit': 1, 'stdout': str(out), 'stderr': str(err)}

        module.run_controls = controls
        module.run_owned = no_command
        module.shutil.which = lambda _name: None if mode == 'setup-missing-compiler' else '/inert/compiler-never-executed'
        try:
            Path.write_bytes, Path.read_bytes = write_spy, read_spy
            args = types.SimpleNamespace(execute_controlled_host_only=True, source=source, output=output,
                                         compiler='inert-compiler', controls=True, sanitize=False)
            with contextlib.redirect_stdout(stdout):
                try:
                    module.run(args)
                except SystemExit as error:
                    if error.code != 1:
                        raise AssertionError('actual driver unexpected terminal exit')
                else:
                    raise AssertionError('inert injected driver failure was swallowed')
        finally:
            Path.write_bytes, Path.read_bytes = orig_write, orig_read
            module.shutil.which = orig_which
        receipt = json.loads((output / 'result.json').read_text())
        record = {'label': label, 'mode': mode, 'driverSha256': hashlib.sha256(copy.read_bytes()).hexdigest(),
                  'fixtureWriteCalls': len(writes), 'commandSpyCalls': len(stages),
                  'actualDriverResult': receipt, 'actualDriverStdout': stdout.getvalue()}
        rows.append(record)
        if receipt['result'] != 'FAIL' or receipt['productionSourceUnchanged'] is not True:
            raise AssertionError('actual driver must retain failed unchanged-source receipt')
        if receipt['processesQuiescent'] is not expect_quiet or receipt['fixtureQuarantined'] is expect_quiet:
            raise AssertionError('missing receipt must quarantine before any restoration')
        if receipt['fixtureRestored'] is not expect_restored:
            raise AssertionError('actual driver restoration state disagrees with oracle')
        if not expect_quiet:
            expected_stages = 1 if mode == 'stage-receipt-interruption' else 0
            if len(writes) != 5 or len(stages) != expected_stages:
                raise AssertionError('restoration or next stage ran without cleanup authority')
            if (fixture_main / names[0]).read_bytes() != witness:
                raise AssertionError('quarantined fixture was overwritten')
        elif expect_restored:
            if len(writes) != 10:
                raise AssertionError('confirmed fixture restoration was not performed exactly once')
            if any((fixture_main / name).read_bytes() != body for name, body in originals.items()):
                raise AssertionError('confirmed restoration failed actual readback')
        elif not receipt['restoreErrors']:
            raise AssertionError('inert restoration failure lost its receipt')
        return record

    def stage_processing_case(label, omit_revocation=False):
        # Inject a receipt-processing exception at the actual pre-validation boundary.
        # The command spy returns an honest False receipt and never starts a child.
        revocation = (b'            processes_quiescent=False\n'
                      b'            record=run_owned(command,cwd=source,env=env,output=output,label=label,')
        boundary = b'            if (type(record) is not dict or type(record.get('
        if original.count(revocation) != 1 or original.count(boundary) != 1:
            raise ValueError('unique actual command receipt boundary required')
        variant = original.replace(boundary, b'            raise OSError("inert receipt-processing interruption")\n'+boundary, 1)
        if omit_revocation:
            variant = variant.replace(revocation, revocation.replace(b'            processes_quiescent=False\n', b''), 1)
        ast.parse(variant)
        copy.write_bytes(variant)
        try:
            return case(label, 'stage-receipt-interruption', False, False)
        finally:
            copy.write_bytes(original)

    failure = None
    restored = False
    try:
        ast.parse(original)
        copy.write_bytes(original)
        for mode, quiet, restored_expected in (
                ('raise-unknown', False, False), ('returned-false', False, False),
                ('malformed', False, False), ('receipt-write-error', False, False),
                ('receipt-read-error', False, False), ('setup-missing-compiler', True, True),
                ('success', True, True), ('returned-known-failure', True, True),
                ('restore-write-error', True, False), ('restore-readback-error', True, False)):
            case('baseline-'+mode, mode, quiet, restored_expected)
        stage_processing_case('baseline-command-receipt-interruption')
        try:
            stage_processing_case('omitted-command-revocation', True)
        except AssertionError as error:
            if str(error) != 'missing receipt must quarantine before any restoration':
                raise
            rows[-1]['expectedIndependentOracleFailure'] = str(error)
        else:
            raise AssertionError('valid command revocation omission escaped actual-driver oracle')
        stage_processing_case('restored-command-revocation')
        ast.parse(mutant)
        copy.write_bytes(mutant)
        try:
            case('omitted-quarantine', 'raise-unknown', False, False)
        except AssertionError as error:
            if str(error) != 'missing receipt must quarantine before any restoration':
                raise
            rows[-1]['expectedIndependentOracleFailure'] = str(error)
        else:
            raise AssertionError('valid quarantine omission escaped the actual-driver oracle')
    except BaseException as error:
        failure = {'type': type(error).__name__, 'message': str(error)}
    finally:
        copy.write_bytes(original)
        restored = copy.read_bytes() == original
    if failure is None:
        try:
            case('restored-quarantine', 'raise-unknown', False, False)
        except BaseException as error:
            failure = {'type': type(error).__name__, 'message': str(error)}
    result = {'result': 'FAIL' if failure or not restored else 'PASS_INERT_DRIVER_CONTROLS_ONLY',
              'failure': failure, 'rows': rows, 'privateDriverRestored': restored,
              'sourceDriverUnchanged': driver.read_bytes() == original,
              'scope': 'Actual outer driver receipt/restore error paths only. No actual child, compiler, firmware, SDK or hardware.'}
    (root / 'result.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--execute-inert-driver-controls', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not args.execute_inert_driver_controls:
        raise SystemExit('explicit inert-driver control scope required')
    result = run_driver_controls(args.output)
    print(json.dumps({'result': result['result'], 'rows': len(result['rows'])}))
    raise SystemExit(0 if result['result'] == 'PASS_INERT_DRIVER_CONTROLS_ONLY' else 1)


if __name__ == '__main__':
    main()
