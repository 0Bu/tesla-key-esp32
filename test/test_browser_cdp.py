#!/usr/bin/env python3
"""Protocol/ownership tests with an owned Python pipe peer; never launch a browser."""

import importlib.util
import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location("browser_cdp", Path(__file__).with_name("browser_cdp.py"))
cdp = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cdp)

PEER = r'''
import json,os,sys,time
mode=sys.argv[1]
if mode == 'eof': sys.exit(0)
if mode == 'stall': time.sleep(10);sys.exit(0)
raw=b''
while True:
    part=os.read(3,65536)
    if not part: break
    raw+=part
    while b'\0' in raw:
        record,raw=raw.split(b'\0',1)
        request=json.loads(record)
        response={'id':request['id'],'result':{'seen':request['params']}}
        if 'sessionId' in request: response['sessionId']=request['sessionId']
        if mode=='id': response['id']+=1
        if mode=='boolean-id': response['id']=True
        if mode=='session': response['sessionId']='other'
        if mode=='error': response['error']={'message':'controlled'}
        if mode=='missing': del response['result']
        if mode=='env': response['result']={'leaked':[k for k in os.environ if k.startswith(('PYTHON','LD_','CHROME_')) or k in ('GH_TOKEN','OTA_SIGNING_KEY_FILE')]}
        if mode=='evaluate': response['result']={'result':{'value':42}}
        if mode=='exception': response['result']={'exceptionDetails':{}}
        if mode=='no-value': response['result']={'result':{'type':'undefined'}}
        if mode=='events':
            for i in range(40): os.write(4,json.dumps({'method':'passive.event','params':{'i':i}}).encode()+b'\0')
        encoded=json.dumps(response).encode()+b'\0'
        for byte in encoded: os.write(4,bytes([byte]))
'''


class RecordTests(unittest.TestCase):
    def test_fragmented_and_multiple_records(self):
        parser = cdp.Records()
        self.assertEqual(parser.feed(b'{"a":'), [])
        self.assertEqual(parser.feed(b'1}\0{"b":2}\0{"c"'), [{"a": 1}, {"b": 2}])
        self.assertEqual(parser.feed(b':3}\0'), [{"c": 3}])
        self.assertEqual(parser.count, 3)

    def test_invalid_records_are_refused(self):
        for raw in (b'', b'[]', b'{"a":1,"a":2}', b'{"a":NaN}',
                    b'{"a":Infinity}', b'{"a":1e999}', b'{"a":"\xff"}', b'{'):
            with self.subTest(raw=raw), self.assertRaises(cdp.CdpError):
                cdp.Records().feed(raw + b'\0')

    def test_separate_size_aggregate_and_flood_limits(self):
        parser = cdp.Records()
        parser.MAX_RECORD = 4
        with self.assertRaisesRegex(cdp.CdpError, 'unterminated'):
            parser.feed(b'12345')
        parser = cdp.Records()
        parser.MAX_RECORD = 4
        with self.assertRaisesRegex(cdp.CdpError, 'oversized'):
            parser.feed(b'{"a":1}\0')
        parser = cdp.Records()
        parser.MAX_TOTAL = 5
        parser.feed(b'{}\0')
        with self.assertRaisesRegex(cdp.CdpError, 'aggregate'):
            parser.feed(b'{}\0')
        parser = cdp.Records()
        parser.MAX_RECORDS = 1
        with self.assertRaisesRegex(cdp.CdpError, 'flood'):
            parser.feed(b'{}\0{}\0')


@unittest.skipUnless(os.name == 'posix', 'controlled POSIX descriptor peer')
class PipeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='cdp-peer-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def peer(self, mode, seconds=3):
        # Only the owned Python peer uses a platform mock on macOS. Actual Chrome remains Linux-only.
        with mock.patch.object(cdp.sys, 'platform', 'linux'):
            connection = cdp.Cdp(sys.executable, ['-I', '-c', PEER, mode],
                                 time.monotonic() + seconds, self.root / (mode + '.log'))
        self.addCleanup(connection.close)
        return connection

    def test_fd_remap_fragmented_response_and_session(self):
        connection = self.peer('normal')
        self.assertEqual(connection.call('Owned.probe', {'x': 1}, 'owned'), {'seen': {'x': 1}})
        self.assertEqual(connection.call('Owned.probe'), {'seen': {}})
        self.assertFalse(os.get_blocking(connection.read_fd))
        self.assertFalse(os.get_blocking(connection.write_fd))
        pid = connection.process.pid
        self.assertEqual(os.getpgid(pid), pid)
        connection.close()
        self.assertIsNone(connection.read_fd)
        self.assertIsNone(connection.write_fd)
        connection.close()

    def test_failures_do_not_become_success(self):
        for mode, message in [('eof', 'EOF'), ('id', 'ID'), ('boolean-id', 'ID'),
                              ('session', 'session'), ('error', 'failed'), ('missing', 'result')]:
            with self.subTest(mode=mode):
                connection = self.peer(mode)
                with self.assertRaisesRegex(cdp.CdpError, message):
                    connection.call('Owned.probe')
                connection.close()

    def test_missing_executable_is_eof(self):
        with mock.patch.object(cdp.sys, 'platform', 'linux'):
            connection = cdp.Cdp('/nonexistent-owned-browser', [], time.monotonic() + 2,
                                 self.root / 'exec.log')
        self.addCleanup(connection.close)
        with self.assertRaisesRegex(cdp.CdpError, 'EOF|I/O'):
            connection.call('Owned.probe')

    def test_absolute_deadline_not_reset_by_commands(self):
        connection = self.peer('stall', seconds=0.15)
        started = time.monotonic()
        with self.assertRaisesRegex(cdp.CdpError, 'deadline'):
            connection.call('Owned.probe', seconds=5)
        self.assertLess(time.monotonic() - started, 1)
        with self.assertRaisesRegex(cdp.CdpError, 'deadline'):
            connection.call('Owned.probe')

    def test_secret_and_injection_environment_is_not_inherited(self):
        with mock.patch.dict(os.environ, {'GH_TOKEN': 'test-only', 'OTA_SIGNING_KEY_FILE': 'test-only',
                                         'PYTHONPATH': 'test-only', 'LD_PRELOAD': '', 'CHROME_EXTRA': 'test-only'}):
            connection = self.peer('env')
        self.assertEqual(connection.call('Owned.probe'), {'leaked': []})

    def test_bounded_events_and_evaluate(self):
        connection = self.peer('events')
        connection.call('Owned.probe')
        self.assertEqual(len(connection.events), 32)
        self.assertEqual(connection.events[0]['params']['i'], 8)
        self.assertEqual(self.peer('evaluate').evaluate('40+2', 'owned'), 42)
        for mode in ('exception', 'no-value'):
            with self.subTest(mode=mode), self.assertRaises(cdp.CdpError):
                self.peer(mode).evaluate('undefined', 'owned')

    def test_non_linux_refused_before_pipe_or_spawn(self):
        with mock.patch.object(cdp.sys, 'platform', 'darwin'), \
                mock.patch.object(cdp.os, 'pipe') as pipes, mock.patch.object(cdp.subprocess, 'Popen') as spawn:
            with self.assertRaisesRegex(cdp.CdpError, 'only on Linux'):
                cdp.Cdp('/unused', [], time.monotonic() + 2, self.root / 'unused.log')
        pipes.assert_not_called()
        spawn.assert_not_called()

    def test_owned_group_cleanup_even_when_leader_exits(self):
        connection = cdp.Cdp.__new__(cdp.Cdp)
        connection.process = mock.Mock(pid=24680)
        connection.read_fd = connection.write_fd = None
        with mock.patch.object(cdp.os, 'killpg') as kill:
            connection.close()
        self.assertEqual(kill.call_args_list, [mock.call(24680, signal.SIGTERM), mock.call(24680, signal.SIGKILL)])

    def test_exited_peer_permission_race_is_reaped(self):
        connection = cdp.Cdp.__new__(cdp.Cdp)
        connection.process = mock.Mock(pid=24680)
        process = connection.process
        connection.read_fd, connection.write_fd = os.pipe()
        with mock.patch.object(cdp.os, 'killpg', side_effect=[
                PermissionError(), ProcessLookupError(), ProcessLookupError()]) as kill:
            connection.close()
        self.assertEqual(kill.call_args_list, [mock.call(24680, signal.SIGTERM),
                                              mock.call(24680, signal.SIGTERM),
                                              mock.call(24680, signal.SIGKILL)])
        self.assertEqual(process.wait.call_args_list[0], mock.call(timeout=0.25))
        self.assertIsNone(connection.read_fd)
        self.assertIsNone(connection.write_fd)

    def test_inaccessible_live_group_remains_failure(self):
        for still_alive in (True, False):
            with self.subTest(still_alive=still_alive):
                connection = cdp.Cdp.__new__(cdp.Cdp)
                connection.process = mock.Mock(pid=24680)
                if still_alive:
                    connection.process.wait.side_effect = cdp.subprocess.TimeoutExpired('owned', 0.25)
                connection.read_fd, connection.write_fd = os.pipe()
                descriptors = (connection.read_fd, connection.write_fd)
                with mock.patch.object(cdp.os, 'killpg', side_effect=PermissionError()):
                    with self.assertRaisesRegex(cdp.CdpError, 'cannot be signaled'):
                        connection.close()
                for descriptor in descriptors:
                    with self.assertRaises(OSError):
                        os.fstat(descriptor)

    def test_cleanup_failure_still_closes_descriptors(self):
        connection = cdp.Cdp.__new__(cdp.Cdp)
        connection.process = mock.Mock(pid=24680)
        connection.process.wait.side_effect = cdp.subprocess.TimeoutExpired('owned', 0.75)
        connection.read_fd, connection.write_fd = os.pipe()
        descriptors = (connection.read_fd, connection.write_fd)
        with mock.patch.object(cdp.os, 'killpg'):
            with self.assertRaisesRegex(cdp.CdpError, 'did not terminate'):
                connection.close()
        for descriptor in descriptors:
            with self.assertRaises(OSError):
                os.fstat(descriptor)


if __name__ == '__main__':
    unittest.main()
