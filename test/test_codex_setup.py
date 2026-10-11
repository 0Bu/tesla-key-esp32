#!/usr/bin/env python3
"""Exercise native hook payloads and blocking results without executing a tool or contacting a device."""
import importlib.util
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import os
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("codex_hook", ROOT / "tools/agent-config/codex_hook.py")
HOOK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HOOK)
checks = 0


def run(event, payload):
    global checks
    raw = payload if isinstance(payload, str) else json.dumps(payload)
    p = subprocess.run([sys.executable, str(ROOT / 'tools/agent-config/codex_hook.py'), event],
                       input=raw, text=True, capture_output=True, check=True, timeout=30)
    checks += 1
    return json.loads(p.stdout)


def tool(name, args):
    return {'hook_event_name': 'PreToolUse', 'cwd': str(ROOT), 'tool_name': name, 'tool_input': args}


def denied(value):
    return value['hookSpecificOutput']['permissionDecision'] == 'deny'


assert run('PreToolUse', tool('Bash', {'command': 'git status --short'})) == {}
assert denied(run('PreToolUse', tool('Bash', {'command': 'printenv'})))
assert denied(run('PreToolUse', tool('apply_patch', {'command': '*** Begin Patch\n*** Update File: partitions.csv\n@@\n-old\n+new\n*** End Patch'})))
assert denied(run('PreToolUse', tool('Bash', {'command': 'gh --repo github.com/0Bu/tesla-key-esp32 pr merge 1 --admin'})))
assert denied(run('PreToolUse', {'hook_event_name': 'PreToolUse', 'tool_name': 'Bash'}))
assert denied(run('PreToolUse', '{bad'))
assert denied(run('PreToolUse', tool('Unknown', {})))
assert denied(run('PreToolUse', {**tool('Bash', {'command': 'true'}), 'hook_event_name': 'PostToolUse'}))
assert run('Stop', {'hook_event_name': 'Stop', 'stop_hook_active': True}) == {}
assert run('Stop', '{bad')['decision'] == 'block'
sentinel = 'UNTRUSTED_VALUE_MUST_NOT_BE_LOGGED'
value = run('PreToolUse', tool('Read', {'file_path': f'/tmp/{sentinel}/ota_signing_key.pem'}))
assert denied(value) and sentinel not in json.dumps(value)

# A denied secret/partition call must never start the independent PR gate.
with patch.object(HOOK, 'invoke', side_effect=AssertionError('PR gate started after safety denial')):
    with patch.object(HOOK, 'core', return_value=json.dumps({'hookSpecificOutput': {
            'hookEventName': 'PreToolUse', 'permissionDecision': 'deny', 'permissionDecisionReason': 'blocked'}})):
        assert denied(HOOK.dispatch('PreToolUse', tool('Bash', {'command': 'printenv'})))
        checks += 1

# Even a structural/runtime bug must emit a blocking native result, rather than exit 1
# (which Codex treats as an ordinary hook error and continues past).
for error in [KeyError('missing'), TypeError('shape'), subprocess.TimeoutExpired('core', 15)]:
    captured = io.StringIO()
    with patch.object(HOOK, 'dispatch', side_effect=error), patch.object(sys, 'argv', ['hook', 'PreToolUse']):
        with patch.object(sys, 'stdin', io.StringIO(json.dumps(tool('Bash', {'command': 'true'})))):
            with contextlib.redirect_stdout(captured):
                assert HOOK.main() == 0
    assert denied(json.loads(captured.getvalue()))
    checks += 1

# Convert the neutral Stop result to Codex's actual continuation shape.
with patch.object(HOOK, 'core', return_value=json.dumps({'decision': 'block', 'reason': 'red tests',
        'hookSpecificOutput': {'hookEventName': 'Stop', 'permissionDecision': 'block'}})):
    assert HOOK.dispatch('Stop', {'hook_event_name': 'Stop'}) == {'decision': 'block', 'reason': 'red tests'}
    checks += 1
with patch.object(HOOK, 'core', return_value='context'):
    value = HOOK.dispatch('SessionStart', {'hook_event_name': 'SessionStart'})
    assert value['hookSpecificOutput'] == {'hookEventName': 'SessionStart', 'additionalContext': 'context\ncontext'}
    checks += 1
with patch.object(HOOK, 'invoke', side_effect=subprocess.TimeoutExpired('core', 15)):
    try:
        HOOK.dispatch('PreToolUse', tool('Bash', {'command': 'true'}))
    except subprocess.TimeoutExpired:
        checks += 1
    else:
        raise AssertionError('timeout was swallowed')

# Execute the registered shell commands, not just the Python implementation. Neither case
# reaches a tool; exit 2 is the native blocking signal when bootstrap cannot produce JSON.
registered = json.loads((ROOT / '.codex/hooks.json').read_text())['hooks']
with tempfile.TemporaryDirectory(prefix='codex-bootstrap-canary-') as directory:
    tmp = Path(directory)
    reject_python = tmp / 'python3'
    reject_python.write_text('#!/bin/sh\nexit 1\n')
    reject_python.chmod(0o755)
    fallback = tmp / 'python3.14'
    fallback.write_text('#!/bin/sh\n[ "$1" = "-c" ] && exit 0\nexit 1\n')
    fallback.chmod(0o755)
    for event in ('PreToolUse', 'Stop'):
        command = registered[event][0]['hooks'][0]['command']
        payload = tool('Bash', {'command': 'printenv'}) if event == 'PreToolUse' else {'hook_event_name': 'Stop'}
        cases = [
            (command, {**os.environ, 'PATH': str(tmp) + ':/usr/bin:/bin', 'TMPDIR': str(tmp / 'missing')}),
            (command, {**os.environ, 'PATH': str(tmp) + ':/usr/bin:/bin'}),
            (command.replace('/tools/agent-config/codex_hook.py', '/tools/agent-config/missing-canary.py'), os.environ),
        ]
        for source, env in cases:
            p = subprocess.run(['bash', '-c', source], input=json.dumps(payload), text=True,
                               capture_output=True, cwd=ROOT, env=env, timeout=30)
            assert p.returncode == 2 and not p.stdout, (event, p.returncode)
            checks += 1

print(f'codex-setup: PASS ({checks} native payload, bootstrap, ordering, redaction and continuation checks)')
