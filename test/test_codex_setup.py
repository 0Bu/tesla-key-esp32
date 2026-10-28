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

# Literal jq/search syntax must remain data through both independent guards. These
# payloads are inspected only; no GitHub, device, partition or shell command runs.
literal_reads = [
    "gh run view 40698697009 --json status,conclusion,jobs --jq '{status,conclusion,jobs:[.jobs[]|{name,status,conclusion,startedAt,completedAt}]}'",
    r"rg -n 'otaCheck|ota/check|set_time|vehicle_data|setInterval|fetch\(|query.*pr|URLSearchParams|ota-channel' main/www/app.js",
    "rg -n 'check-partition-contract|partition.*protected|PR policy could|require-pr' tools/agent-hooks/codex_hook.py",
    "rg -n 'partition|def .*table|md5|argparse' scripts/check-firmware-artifacts.py scripts/check-bootloader-compat.py scripts/ci-build-verify.sh",
    "rg -n 'first\nquery.*pr|sh < input' docs/DEVELOPMENT.md",
    r'rg -n "literal\|text\{x\}" main/www/app.js',
    "rg -n '$RG_ARGS|sh < input' scripts/ci-build-verify.sh",
]
validator = 'python3 scripts/check-partition-contract.py --csv partitions.csv'
for command in [*literal_reads, validator, validator + ' --binary .tmp/table.bin']:
    assert run('PreToolUse', tool('exec_command', {'cmd': command, 'workdir': str(ROOT)})) == {}, command

for command in [
    validator + ' --self-test',
    validator + ' --csv partitions.csv',
    validator + ' --binary a.bin --binary b.bin',
    validator + ' --unknown value',
    validator.replace('--csv partitions.csv', '--csv other.csv --binary partitions.csv'),
    validator.replace('scripts/check-', './scripts/check-'),
    validator.replace('python3 ', 'python3 -c '),
    validator.replace('python3 ', 'env python3 '),
    validator + ' > output.txt',
    validator + ' && true',
    validator + ' | cat',
    validator + ' --binary "${TABLE}"',
    validator + ' --binary "$(echo table.bin)"',
    validator + ' --binary ota_signing_key.pem',
    literal_reads[0] + '; git push origin main',
    literal_reads[1] + ' && gh pr merge 1 --admin',
    "printf 'harmless' | sh",
    'sh < input.sh',
    'sh -s',
    'command -p sh -s',
    'builtin command -p sh -s',
    'env -S "sh -s"',
    'printf harmless | (sh)',
    'printf harmless | { sh; }',
    'cat input | stdbuf -oL sh',
    'unknown-wrapper sh -s',
    "rg ${RG_ARGS} 'sh < input' file",
    'rg "$RG_ARGS" \'sh < input\' file',
    "rg --pre* 'sh < input' file",
    "rg --pre=sh 'sh < input' file",
    "bash -c 'cat input | sh'",
    'gh pr create --body-file "${BODY_FILE}"',
    'git push origin "${BRANCH}"',
    'bash -c \'echo x; g"h" pr merge 1 --admin\'',
    'rg -n "$(gh pr merge 1 --admin)" main/www/app.js',
]:
    value = run('PreToolUse', tool('exec_command', {'cmd': command, 'workdir': str(ROOT)}))
    assert value and denied(value), command

for args in [
    {'cmd': validator, 'workdir': '/tmp'},
    {'cmd': validator, 'workdir': str(ROOT), 'cwd': '/tmp'},
    {'cmd': validator, 'workdir': 7},
]:
    assert denied(run('PreToolUse', tool('exec_command', args))), args

# Exercise validator identity independently of filename, without modifying the
# repository's validator or executing any copy of it.
sys.path.insert(0, str(ROOT / 'tools/agent-hooks'))
import agent_hook as safety_core
import shell_syntax
with tempfile.TemporaryDirectory(prefix='partition-validator-canary-') as directory:
    fixture_root = Path(directory).resolve()
    fixture_script = fixture_root / 'scripts/check-partition-contract.py'
    fixture_script.parent.mkdir()
    fixture_csv = fixture_root / 'partitions.csv'
    fixture_csv.write_bytes((ROOT / 'partitions.csv').read_bytes())
    fixture_script.write_bytes((ROOT / 'scripts/check-partition-contract.py').read_bytes())
    payload = {'cwd': str(fixture_root), 'tool_input': {'cmd': validator, 'workdir': str(fixture_root)}}
    with patch.object(safety_core, 'HOOK_ROOT', fixture_root):
        assert safety_core.trusted_partition_validation(validator, payload)
        fixture_script.write_text('print("replacement")\n')
        assert not safety_core.trusted_partition_validation(validator, payload)
        fixture_script.unlink()
        fixture_script.symlink_to(ROOT / 'scripts/check-partition-contract.py')
        assert not safety_core.trusted_partition_validation(validator, payload)
        fixture_script.unlink()
        assert not safety_core.trusted_partition_validation(validator, payload)
        checks += 4

# Downstream target/body validators must receive the original expansion quotes.
for source in ['gh pr create --body-file "${BODY_FILE}"', 'git push origin "${BRANCH}"',
               "gh pr create --body 'line one\nline two | {}'", "rg 'x|query.*pr' file"]:
    parts, count = shell_syntax.gate_segments(source)
    assert parts == [source] and count == 1, (source, parts, count)
    checks += 1

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
