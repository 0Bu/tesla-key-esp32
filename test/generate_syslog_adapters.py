#!/usr/bin/env python3
"""Extract actual firmware formatter/replay bodies for passive host I/O tests."""
import pathlib
import re
import runpy
import sys
root = pathlib.Path(__file__).resolve().parents[1]
body_in = runpy.run_path(str(root / 'test/test_runtime_boundary_contract.py'))['function_body_in']
source = (root / 'main/syslog.cpp').read_text()
parts = [re.search(r'struct SyslogMsg\s*\{.*?^\};', source, re.M | re.S).group(0), re.search(r'enum class SendResult\s*\{[^}]+\};', source).group(0)]
for name in ('syslog_sendto', 'syslog_replay_boot', 'syslog_send'):
    start = re.search(r'^(?:static )?(?:SendResult|void) ' + name + r'\b', source, re.M)
    if not start:
        raise AssertionError('missing production syslog function: ' + name)
    body = body_in(source, name)
    prefix = source[start.start():source.index(body, start.start())]
    parts.append(prefix + body)
pathlib.Path(sys.argv[1]).write_text('\n\n'.join(parts))
