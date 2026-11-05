#!/usr/bin/env python3
"""Exercise the actual prefetch shell with a passive Docker double; never use a key or daemon."""
from pathlib import Path
import copy
import importlib.util
import json
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/prefetch-idf-image.sh"
IMAGE = subprocess.check_output([str(ROOT / "scripts/idf-version.sh"), "--image"], text=True).strip()

# Exercise the actual YAML consumer independently of the privileged raw-job digest pin.
spec = importlib.util.spec_from_file_location("prefetch_policy", ROOT / "scripts/check-workflow-policy.py")
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)
workflow = ROOT / ".github/workflows/build.yml"
steps = policy.workflow_model(workflow, workflow.read_text())["jobs"]["publish"]["steps"]
policy.validate_signing_image_prefetch(steps)
start_index = next(i for i, step in enumerate(steps) if step.get("name") == "Start pinned Dev signing image pull")
wait_index = next(i for i, step in enumerate(steps) if step.get("name") == "Require pinned Dev signing image before key provisioning")
def refused(changed):
    try:
        policy.validate_signing_image_prefetch(changed)
    except policy.PolicyError:
        return
    raise AssertionError("unsafe signing image prefetch workflow admitted")
for index in (start_index, wait_index):
    for edit in (lambda step: step.update({"if": False}),
                 lambda step: step.update({"continue-on-error": True}),
                 lambda step: step.update(run=step["run"] + " || true"),
                 lambda step: step.update(env={"UNEXPECTED": "value"})):
        changed = copy.deepcopy(steps)
        edit(changed[index])
        refused(changed)
    changed = copy.deepcopy(steps)
    changed.pop(index)
    refused(changed)
    changed = copy.deepcopy(steps)
    changed.insert(index, copy.deepcopy(steps[index]))
    refused(changed)
changed = copy.deepcopy(steps)
consumer = changed.pop(wait_index)
key_index = next(i for i, step in enumerate(changed) if step.get("name") == "Provision OTA signing key")
changed.insert(key_index + 1, consumer)
refused(changed)
changed = copy.deepcopy(steps)
changed[wait_index]["run"] = changed[wait_index]["run"].splitlines()[0] + "\n"
refused(changed)  # No stale-source admission after a long pull.


with tempfile.TemporaryDirectory(prefix="signing-prefetch-") as temporary:
    root = Path(temporary)
    tools = root / "tools"
    tools.mkdir()
    docker = tools / "docker"
    docker.write_text("#!" + sys.executable + "\n" + '''
from pathlib import Path
import json, os, sys, time
args = sys.argv[1:]
with Path(os.environ['DOCKER_CALLS']).open('a') as out:
    out.write(json.dumps(args) + '\\n')
if args == ['pull', os.environ['EXPECTED_IMAGE']]:
    if os.environ.get('HOLD_PULL') == '1':
        end = time.monotonic() + 5
        while not Path(os.environ['RELEASE_PULL']).exists():
            if time.monotonic() >= end:
                sys.exit(99)
            time.sleep(.01)
    result = int(os.environ.get('PULL_EXIT', '0'))
    if result:
        print('x' * 6000 + '\\nfixture: registry unavailable', file=sys.stderr)
    sys.exit(result)
if args == ['image', 'inspect', os.environ['EXPECTED_IMAGE']]:
    sys.exit(int(os.environ.get('INSPECT_EXIT', '0')))
sys.exit(98)
''')
    docker.chmod(0o700)
    calls = root / "calls"
    release = root / "release"
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("GIT_", "PYTHON", "LD_", "DYLD_"))
           and k not in ("GH_TOKEN", "GITHUB_TOKEN", "OTA_SIGNING_KEY", "OTA_SIGNING_KEY_FILE", "ENV", "BASH_ENV")}
    env.update(PATH=str(tools) + os.pathsep + os.environ.get("PATH", ""),
               DOCKER_CALLS=str(calls), EXPECTED_IMAGE=IMAGE, RELEASE_PULL=str(release))

    def run(mode, state, *, extra=None, seconds=None, expected=0):
        command = ["bash", str(SCRIPT), mode, str(state)]
        if seconds is not None:
            command.append(str(seconds))
        result = subprocess.run(command, env={**env, **(extra or {})},
                                capture_output=True, text=True, timeout=4)
        assert result.returncode == expected, (command, result.returncode, result.stderr)
        return result

    state = root / "success"
    # Start must return while Docker is deliberately held, allowing the next workflow step to run.
    run("start", state, extra={"HOLD_PULL": "1"})
    assert not (state / "result").exists(), "prefetch blocked on the pull"
    assert state.stat().st_mode & 0o777 == 0o700
    run("start", state, expected=1)  # No stale-state overwrite/reuse.
    release.touch()
    run("wait", state, seconds=3)
    assert [json.loads(line) for line in calls.read_text().splitlines()] == [
        ["pull", IMAGE], ["image", "inspect", IMAGE]]
    assert (state / "image").read_text().strip() == IMAGE
    run("wait", state, extra={"INSPECT_EXIT": "37"}, seconds=1, expected=37)

    failed = root / "failed"
    run("start", failed, extra={"PULL_EXIT": "37"})
    diagnostic = run("wait", failed, seconds=2, expected=1)
    assert "fixture: registry unavailable" in diagnostic.stderr and len(diagnostic.stderr) < 4500
    assert (failed / "result").read_text().strip() == "37"

    missing = root / "missing"
    run("wait", missing, seconds=1, expected=1)
    missing.mkdir()
    (missing / "image").write_text(IMAGE + "\n")
    run("wait", missing, seconds=1, expected=1)  # Bounded wait, no false-ready marker.
    for value in ("37\n", "ready\n", "0\n37\n", ""):
        (missing / "result").write_text(value)
        run("wait", missing, seconds=1, expected=1)
    (missing / "result").unlink()
    (missing / "result").symlink_to(state / "result")
    run("wait", missing, seconds=1, expected=1)
    (missing / "result").unlink()
    (missing / "result").write_text("0\n")
    (missing / "image").write_text("espressif/idf:stale\n")
    run("wait", missing, seconds=1, expected=1)
    (missing / "image").unlink()
    (missing / "image").symlink_to(state / "image")
    run("wait", missing, seconds=1, expected=1)
    (failed / "pull.log").unlink()
    diagnostic_target = root / "diagnostic-target"
    diagnostic_target.write_text("unrelated diagnostic content")
    (failed / "pull.log").symlink_to(diagnostic_target)
    diagnostic = run("wait", failed, seconds=1, expected=1)
    assert "unrelated diagnostic content" not in diagnostic.stderr
    linked = root / "linked"
    linked.symlink_to(state, target_is_directory=True)
    run("start", linked, expected=1)
    run("wait", linked, seconds=1, expected=1)
    for seconds in (0, 601, -1, "1x", "0001"):
        run("wait", state, seconds=seconds, expected=2)
    run("start", root / "extra-argument", seconds=1, expected=2)
    run("unknown", state, expected=2)
print("signing image prefetch: PASS (actual asynchronous shell, exact pin, failures, stale/symlink state and bounded waits; passive Docker only)")
