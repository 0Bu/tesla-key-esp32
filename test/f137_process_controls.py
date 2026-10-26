#!/usr/bin/env python3
"""UNEXECUTED inert process controls. No compiler, network or qualified-source edits.

These validate actual liveness-EOF delegation, not an independently observed external SIGKILL.
The omission/restoration changes only a private copy of the test operator itself.
"""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from f137_process import run_owned, _cleanup


def run_controls(output, env, deadline):
    output = Path(output)
    root = output / 'operator-controls'
    root.mkdir(mode=0o700)
    rows = []
    failure = None
    quiet = True
    helper = Path(__file__).with_name('f137_process.py')
    original = helper.read_bytes()
    copy = root / 'guardian-copy.py'
    copy.write_bytes(original)
    restored = False
    def need(value, message):
        if not value:
            raise AssertionError(message)
    def owned(*args, **kwargs):
        nonlocal quiet
        try:
            return run_owned(*args, **kwargs)
        except BaseException:
            quiet = False  # A missing stage receipt never grants restore authority.
            raise
    def eof_case(label, module, should_survive):
        nonlocal quiet
        marker, ready = root / (label + '.marker'), root / (label + '.ready')
        child = "import pathlib,sys,time,signal;signal.signal(signal.SIGTERM,signal.SIG_IGN);pathlib.Path(sys.argv[1]).write_text('ready');time.sleep(.25);pathlib.Path(sys.argv[2]).write_text('late');time.sleep(3)"
        command = [sys.executable, '-I', '-B', '-c', child, str(ready), str(marker)]
        alive_read, alive_write = os.pipe(); status_read, status_write = os.pipe()
        process = None; error = None; cleanup = None
        try:
            if time.monotonic() + 1.0 >= deadline:
                raise TimeoutError('operator-controls whole budget')
            process = subprocess.Popen([sys.executable, '-I', '-B', str(module), '--guardian',
                                        str(alive_read), str(status_write), json.dumps(command)],
                                       env=env, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                       pass_fds=(alive_read, status_write), close_fds=True,
                                       start_new_session=True)
            end = min(deadline, time.monotonic() + 1.0)
            while not ready.exists() and time.monotonic() < end:
                time.sleep(.01)
            need(ready.exists(), label + ': inert child was not ready')
            os.close(alive_write); alive_write = None  # Exactly the EOF produced by parent death.
            end = min(deadline, time.monotonic() + .4)
            while time.monotonic() < end:
                time.sleep(.01)
            need(marker.exists() == should_survive, label + ': incorrect actual liveness witness')
        except BaseException as exc:
            error = {'type': type(exc).__name__, 'message': str(exc)}
        finally:
            # Guardian has not been polled/reaped. Even an ineffective omission remains owned.
            cleanup = _cleanup(process)
            quiet = quiet and cleanup['reaped'] and cleanup['groupAbsent'] and not cleanup['errors']
            for fd in (alive_read, alive_write, status_read, status_write):
                if fd is not None:
                    try: os.close(fd)
                    except OSError: pass
            record = {'label': label, 'error': error, 'cleanup': cleanup,
                      'lateMarkerObserved': marker.exists(), 'expectedLateMarker': should_survive}
            rows.append(record)
            (root / (label + '.json')).write_text(json.dumps(record, indent=2) + '\n')
        need(quiet and error is None, label + ': cleanup/control failed')
    try:
        normal = owned([sys.executable, '-I', '-B', '-c', "print('PASS inert command')"],
                           cwd=root, env=env, output=root, label='normal', deadline=deadline, stage_seconds=3)
        rows.append(normal); quiet = quiet and normal['processesQuiescent']
        need(quiet and normal['exit'] == 0 and not normal['error'], 'normal owned-command result')
        marker = root / 'timeout.marker'; ready = root / 'timeout.ready'
        child = "import pathlib,sys,time,signal;signal.signal(signal.SIGTERM,signal.SIG_IGN);pathlib.Path(sys.argv[1]).write_text('ready');time.sleep(1.2);pathlib.Path(sys.argv[2]).write_text('late');time.sleep(3)"
        leader = "import subprocess,sys,time;subprocess.Popen([sys.executable,'-I','-B','-c',sys.argv[1],sys.argv[2],sys.argv[3]]);time.sleep(4)"
        timed = owned([sys.executable, '-I', '-B', '-c', leader, child, str(ready), str(marker)],
                          cwd=root, env=env, output=root, label='timeout-tree', deadline=deadline, stage_seconds=.8)
        rows.append(timed); quiet = quiet and timed['processesQuiescent']
        need(quiet and timed['timeout'] and ready.exists() and not marker.exists(), 'timeout descendant cleanup')
        full = owned([sys.executable, '-I', '-B', '-c', "import os;os.write(1,b'x'*(1024*1024))"],
                         cwd=root, env=env, output=root, label='bounded-output', deadline=deadline, stage_seconds=3)
        rows.append(full); quiet = quiet and full['processesQuiescent']
        need(quiet and full['error'] and 'output-cap' in full['error']['message'] and
             full['retainedBytes']['stdout'] <= 256*1024, 'bounded output control')
        eof_case('eof-positive', copy, False)
        old = "if ready and os.read(alive_fd, 1) == b'':\n                break"
        text = original.decode(); need(text.count(old) == 1, 'valid guardian omission anchor')
        need(quiet, 'no mutation without quiescence')
        try:
            copy.write_text(text.replace(old, "if ready and os.read(alive_fd, 1) == b'':\n                pass  # valid omission of liveness delegation", 1))
            eof_case('eof-omitted', copy, True)
        finally:
            if quiet:
                copy.write_bytes(original)
                need(copy.read_bytes() == original, 'guardian restore readback')
        eof_case('eof-restored', copy, False)
        restored = copy.read_bytes() == original
        driver_control = owned([sys.executable, '-B', str(Path(__file__).with_name('f137_driver_controls.py')),
                                '--execute-inert-driver-controls', '--output', str(root / 'driver-controls')],
                               cwd=root, env=env, output=root, label='outer-driver-controls',
                               deadline=deadline, stage_seconds=10)
        rows.append(driver_control); quiet = quiet and driver_control['processesQuiescent']
        need(quiet and not driver_control['error'] and not driver_control['timeout'] and
             driver_control['exit'] == 0, 'actual outer-driver receipt/quarantine controls')
    except BaseException as error:
        failure = {'type': type(error).__name__, 'message': str(error)}
    finally:
        if quiet:
            try:
                copy.write_bytes(original)
                restored = copy.read_bytes() == original
            except BaseException as error:
                restored = False
                if failure is None: failure = {'type': type(error).__name__, 'message': 'restore:' + str(error)}
        result = {'result': 'FAIL' if failure or not quiet or not restored else 'PASS_INERT_PROCESS_CONTROLS_ONLY',
                  'failure': failure, 'processesQuiescent': quiet, 'privateHelperRestored': restored,
                  'sourceHelperUnchanged': helper.read_bytes() == original, 'rows': rows,
                  'scope': 'Actual owned-group timeout/output/liveness-EOF sensitivity only; no external-parent-kill, compiler, SDK or firmware proof.'}
        (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result
