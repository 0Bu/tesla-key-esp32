#!/usr/bin/env python3
"""Extract actual firmware formatter/replay bodies for passive host I/O tests."""
import pathlib
import re
import runpy
import sys
root = pathlib.Path(__file__).resolve().parents[1]
body_in = runpy.run_path(str(root / 'test/test_runtime_boundary_contract.py'))['function_body_in']
header = (root / 'main/syslog.hpp').read_text()
source = (root / 'main/syslog.cpp').read_text()
msg_match = re.search(r'struct SyslogMsg\s*\{.*?^\};', header, re.M | re.S)
if not msg_match:
    raise AssertionError('missing production SyslogMsg struct in main/syslog.hpp')
result_match = re.search(r'enum class SendResult\s*\{[^}]+\};', source)
if not result_match:
    raise AssertionError('missing production SendResult enum in main/syslog.cpp')
parts = [msg_match.group(0), result_match.group(0)]
for name in ('syslog_send_msg', 'syslog_sendto', 'syslog_replay_boot', 'syslog_send'):
    start = re.search(
        r'^(?:\[\[gnu::noinline\]\]\s+)?(?:static )?(?:SendResult|void) ' + name + r'\b',
        source,
        re.M,
    )
    if not start:
        raise AssertionError('missing production syslog function: ' + name)
    body = body_in(source, name)
    prefix = source[start.start():source.index(body, start.start())]
    parts.append(prefix + body)
pathlib.Path(sys.argv[1]).write_text('\n\n'.join(parts))
