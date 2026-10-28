#!/usr/bin/env python3
"""UNEXECUTED F137 test operator: owned command group, liveness guardian and receipts.

No operation on import. The guardian is the process-group leader and outlives the command.
The parent never reaps it before atomic group cleanup. Parent death closes the liveness pipe;
the guardian then kills its own group. No fixture restoration is performed by this module.
"""
import errno
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import time

STREAM_LIMIT = 256 * 1024
CLEANUP_SECONDS = 3.0


def _guardian(alive_fd, status_fd, command):
    """Only the fresh isolated child enters here. No caller-supplied shell is used."""
    child = None
    sent = False
    try:
        try:
            child = subprocess.Popen(command, stdin=subprocess.DEVNULL, close_fds=True)
        except BaseException as error:
            packet = {'returnCode': None, 'launchError': type(error).__name__}
        else:
            packet = None
        # The actual command alone owns these data pipes; guardian lifetime cannot hold EOF open.
        for fd in (1, 2):
            try:
                os.close(fd)
            except OSError:
                pass
        while True:
            ready, _, _ = select.select([alive_fd], [], [], 0.01)
            if ready and os.read(alive_fd, 1) == b'':
                break
            if packet is None and child is not None:
                code = child.poll()
                if code is not None:
                    packet = {'returnCode': code, 'launchError': None}
            if packet is not None and not sent:
                wire = (json.dumps(packet, separators=(',', ':')) + '\n').encode()
                if len(wire) > 256:
                    raise RuntimeError('guardian status bound')
                # One bounded atomic write; no user output is mixed into the status channel.
                if os.write(status_fd, wire) != len(wire):
                    raise RuntimeError('guardian status short write')
                os.close(status_fd)
                status_fd = -1
                sent = True
    finally:
        # Covers liveness EOF, an I/O error and normal parent release. No TERM grace window.
        os.killpg(os.getpgrp(), signal.SIGKILL)


def _cleanup(process):
    details = {'groupSignalPermissionError': False, 'reaped': False,
               'groupAbsent': False, 'errors': []}
    if process is None:
        return {**details, 'reaped': True, 'groupAbsent': True}
    # The guardian has not been polled/reaped, preserving the owned PGID until this kill.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    except PermissionError:
        # Darwin can deny signalling an already exited unreaped leader. Only later ESRCH wins.
        details['groupSignalPermissionError'] = True
    except BaseException as error:
        details['errors'].append('kill-group:' + type(error).__name__)
    deadline = time.monotonic() + CLEANUP_SECONDS
    try:
        process.wait(timeout=max(0.001, deadline - time.monotonic()))
        details['reaped'] = True
    except BaseException as error:
        details['errors'].append('reap:' + type(error).__name__)
    while time.monotonic() < deadline:
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            details['groupAbsent'] = True
            break
        except PermissionError:
            details['errors'].append('group-absence:PermissionError')
            break
        except BaseException as error:
            details['errors'].append('group-absence:' + type(error).__name__)
            break
        time.sleep(0.01)
    if not details['groupAbsent']:
        details['errors'].append('owned-group-absence-unconfirmed')
    return details


def run_owned(command, *, cwd, env, output, label, deadline, stage_seconds):
    """Return a retained stage receipt; errors never authorize mutation/restoration.

    stdout/stderr each have a strict retained-byte cap. The command may transiently fill its
    finite OS pipe before cleanup; no unbounded capture_output allocation is used. An outer
    process kill closes the guardian pipe, including while compiler helpers are running.
    """
    output = Path(output)
    stdout_path, stderr_path = (output / (label + '.' + s) for s in ('stdout', 'stderr'))
    stage_path = output / (label + '.stage.json')
    process = None
    alive_read = alive_write = status_read = status_write = None
    files = {}
    channels = {}
    packet = None
    wire = bytearray()
    failure = None
    timed_out = False
    cleanup = None
    start = time.monotonic()
    streams = {'stdout': 0, 'stderr': 0}
    record = {'label': label, 'command': list(command), 'exit': None, 'timeout': False,
              'seconds': None, 'stdout': str(stdout_path), 'stderr': str(stderr_path),
              'stdoutSha256': None, 'stderrSha256': None, 'processStarted': False,
              'processesQuiescent': False, 'error': None}

    def consume(fd, name):
        data = os.read(fd, 65536)
        if not data:
            channels.pop(fd, None)
            return
        left = STREAM_LIMIT - streams[name]
        if left > 0:
            files[name].write(data[:left])
            streams[name] += min(left, len(data))
        if len(data) > left:
            raise RuntimeError(name + '-retained-output-cap')

    try:
        # No child or signal/PGID operation exists on a refused pre-launch deadline/open path.
        remaining = min(float(stage_seconds), deadline - time.monotonic())
        if remaining <= 0:
            raise TimeoutError('whole-host-budget-exhausted-before-launch')
        for name, path in (('stdout', stdout_path), ('stderr', stderr_path)):
            files[name] = path.open('xb')
        alive_read, alive_write = os.pipe()
        status_read, status_write = os.pipe()
        launcher = [sys.executable, '-I', '-B', str(Path(__file__).resolve()), '--guardian',
                    str(alive_read), str(status_write), json.dumps(list(command))]
        process = subprocess.Popen(launcher, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   pass_fds=(alive_read, status_write), close_fds=True,
                                   start_new_session=True)
        record['processStarted'] = True
        record['ownedGroup'] = process.pid
        os.close(alive_read); alive_read = None
        os.close(status_write); status_write = None
        for fd, name in ((process.stdout.fileno(), 'stdout'),
                         (process.stderr.fileno(), 'stderr')):
            os.set_blocking(fd, False)
            channels[fd] = name
        os.set_blocking(status_read, False)
        (output / (label + '.started.json')).write_text(json.dumps(
            {'ownedGroup': process.pid, 'guardianHasLivenessReader': True}) + '\n')
        end = min(deadline, start + remaining)
        while packet is None:
            now = time.monotonic()
            if now >= end:
                timed_out = True
                raise TimeoutError('owned-stage-deadline')
            ready, _, _ = select.select([status_read, *channels], [], [], min(0.02, end - now))
            for fd in ready:
                if fd != status_read:
                    consume(fd, channels[fd])
                    continue
                data = os.read(fd, 512)
                if not data:
                    raise RuntimeError('guardian-status-eof-before-packet')
                wire.extend(data)
                if len(wire) > 256:
                    raise RuntimeError('guardian-status-bound')
                if b'\n' in wire:
                    if wire.count(b'\n') != 1 or not wire.endswith(b'\n'):
                        raise RuntimeError('guardian-status-framing')
                    packet = json.loads(wire)
                    if (set(packet) != {'returnCode', 'launchError'} or
                        type(packet['returnCode']) not in (int, type(None)) or
                        (packet['launchError'] is not None and not isinstance(packet['launchError'], str))):
                        raise RuntimeError('guardian-status-schema')
        record['exit'] = packet['returnCode']
        if packet['launchError'] is not None or packet['returnCode'] is None:
            raise RuntimeError('command-launch-error:' + str(packet['launchError']))
    except BaseException as error:
        failure = {'type': type(error).__name__, 'message': str(error)}
    finally:
        # Keep the liveness writer OPEN during atomic group cleanup. Parent death before this
        # group kill still reaches a living guardian; after it the entire group is gone.
        cleanup = _cleanup(process)
        for fd in (alive_write, alive_read, status_write, status_read):
            if fd is not None:
                try:
                    os.close(fd)
                except OSError:
                    pass
        # Read the finite remaining pipe buffers after termination. No restored source is touched.
        drain_end = time.monotonic() + 0.5
        while channels and time.monotonic() < drain_end:
            try:
                ready, _, _ = select.select(list(channels), [], [], 0.01)
                for fd in ready:
                    consume(fd, channels[fd])
            except BaseException as error:
                if failure is None:
                    failure = {'type': type(error).__name__, 'message': str(error)}
                break
        if channels and failure is None:
            failure = {'type': 'UnconfirmedDataEOF', 'message': 'post-cleanup pipe EOF missing'}
        for stream in (getattr(process, 'stdout', None), getattr(process, 'stderr', None)):
            if stream is not None:
                try:
                    stream.close()
                except OSError:
                    pass
        for name, stream in files.items():
            try:
                stream.close()
            except BaseException as error:
                if failure is None:
                    failure = {'type': type(error).__name__, 'message': 'close-' + name}
        for name, path in (('stdout', stdout_path), ('stderr', stderr_path)):
            try:
                record[name + 'Sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
            except BaseException as error:
                if failure is None:
                    failure = {'type': type(error).__name__, 'message': 'digest-' + name}
        record.update(seconds=time.monotonic() - start, timeout=timed_out, cleanup=cleanup,
                      processesQuiescent=cleanup['reaped'] and cleanup['groupAbsent'] and not cleanup['errors'],
                      retainedBytes=streams, error=failure)
        # If this directory itself cannot be written, the caller receives that explicit failure;
        # a failed receipt cannot turn into success or silently authorize fixture restoration.
        stage_path.write_text(json.dumps(record, indent=2) + '\n')
    return record


if __name__ == '__main__':
    if len(sys.argv) != 5 or sys.argv[1] != '--guardian':
        raise SystemExit('internal isolated guardian invocation required')
    _guardian(int(sys.argv[2]), int(sys.argv[3]), json.loads(sys.argv[4]))
