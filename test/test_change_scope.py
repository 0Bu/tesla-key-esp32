#!/usr/bin/env python3
"""Conservative diff selection and executable strict-profile positive/negative controls."""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("change_scope", ROOT / "scripts/check-change-scope.py")
scope = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scope)


class ChangeScope(unittest.TestCase):
    def test_conservative_selection(self):
        for paths, expected in (
            (["README.md", "docs/API.md"], "docs"),
            (["main/www/app.js", "docs/API.md"], "browser"),
            (["tools/browser/Dockerfile"], "browser"),
            (["main/http_api.cpp", "main/logic/result.hpp"], "runtime"),
            (["main/http_api.cpp", "main/www/app.js"], "full"),
            (["AGENTS.md"], "full"), (["main/AGENTS.md"], "full"),
            ([".agents/skills/mock-test/SKILL.md"], "full"),
            (["main/idf_component.yml"], "full"), (["sdkconfig.defaults"], "full"),
            (["test/test_logic.cpp"], "full"), (["scripts/new-check.py"], "full"),
            (["partitions.csv"], "full"), (["../outside.md"], "full"), ([], "full"),
        ):
            with self.subTest(paths=paths):
                self.assertEqual(scope.classify(paths), expected)

    def test_unavailable_diff_and_non_pr_events_are_full(self):
        self.assertEqual(scope.changed_scope(ROOT, "bad", "bad"), "full")
        self.assertEqual(scope.changed_scope(ROOT, "0" * 40, "1" * 40), "full")
        for event in ("push", "workflow_dispatch", "unexpected"):
            result = subprocess.run([sys.executable, str(ROOT / "scripts/check-change-scope.py"),
                                     "--event", event, "--github-output"], check=True,
                                    capture_output=True, text=True)
            self.assertEqual(result.stdout, "profile=full\n")

    def run_profile(self, profile: str | None, failure: str = ""):
        source = (ROOT / "scripts/run-mock-tests.sh").read_text()
        inventory = re.search(r"for required in (.*?)\s*; do", source, re.S)
        required = shlex.split(inventory.group(1).replace("\\\n", " "))
        with tempfile.TemporaryDirectory(prefix="tesla-profile-flow-") as temporary:
            root = Path(temporary)
            for name in [*required, "tools/agent-hooks/run_with_timeout.py"]:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            script = root / "scripts/run-mock-tests.sh"
            script.write_text(source)
            binaries = root / "bin"
            binaries.mkdir()
            fake = "#!" + sys.executable + '''
import json,os,sys
args=sys.argv[1:]
if args[:1] == ['-c']: raise SystemExit(0)
if args[:2] == ['tools/agent-hooks/run_with_timeout.py','120']: args=args[2:]
with open(os.environ['PROFILE_TRACE'],'a') as out: out.write(json.dumps(args)+'\\n')
raise SystemExit(37 if os.environ.get('PROFILE_FAIL') in args else 0)
'''
            python = binaries / "python3"
            python.write_text(fake)
            python.chmod(0o755)
            for name in ("cmake", "ctest", "node", "ruby", "jq"):
                file = binaries / name
                file.write_text("#!/bin/sh\nexit 0\n")
                file.chmod(0o755)
            uname = binaries / "uname"
            uname.write_text("#!/bin/sh\nprintf 'Linux\\n'\n")
            uname.chmod(0o755)
            trace = root / "trace.jsonl"
            environment = {key: value for key, value in os.environ.items()
                           if key not in ("OTA_SIGNING_KEY", "OTA_SIGNING_KEY_FILE", "BASH_ENV", "ENV", "TESLA_LOCAL_CHECK_LOCK_FD")}
            environment.update(PATH=str(binaries) + os.pathsep + os.defpath,
                               PROFILE_TRACE=str(trace), PROFILE_FAIL=failure)
            arguments = ["--require-all"] + (["--profile", profile] if profile is not None else [])
            result = subprocess.run(["/bin/bash", str(script), *arguments],
                                    cwd=root, env=environment, capture_output=True, timeout=30)
            calls = [json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []
            return result.returncode, calls

    def test_selected_profiles_keep_their_actual_gates_and_failures(self):
        runtime = "test/test_runtime_boundary_contract.py"
        browser = "test/web_ui_browser_gate.py"
        policy = "test/test_local_checks.py"
        for profile, present, absent in (
            (None, (runtime, browser, policy), ()),
            ("full", (runtime, browser, policy), ()),
            ("runtime", (runtime, "test/test_f137_host.py", "scripts/check-ble-row-parity.sh"), (browser, policy)),
            ("browser", (browser, "test/web_ui_http.test.mjs"), (runtime, policy)),
        ):
            with self.subTest(profile=profile):
                code, calls = self.run_profile(profile)
                self.assertEqual(code, 0)
                values = {value for args in calls for value in args}
                self.assertTrue(set(present) <= values, (present, values))
                self.assertTrue(not set(absent) & values, (absent, values))
                self.assertEqual(self.run_profile(profile, present[0])[0], 1)
        self.assertEqual(self.run_profile("unknown")[0], 2)

    def test_actual_bash_lint_modes_keep_validators_and_propagate_failure(self):
        with tempfile.TemporaryDirectory(prefix="tesla-lint-mode-") as temporary:
            root = Path(temporary)
            (root / "scripts").mkdir()
            script = root / "scripts/repo-lint.sh"
            script.write_text((ROOT / "scripts/repo-lint.sh").read_text())
            (root / "workflow.yml").touch()
            binaries = root / "bin"
            binaries.mkdir()
            fake = "#!" + sys.executable + '''
import json,os,pathlib,sys
args=sys.argv[1:]
if pathlib.Path(sys.argv[0]).name == 'git':
    if '*.yml' in args: sys.stdout.buffer.write(b'workflow.yml\\0')
    raise SystemExit(0)
if args[:1] == ['-c']: raise SystemExit(0)
with open(os.environ['PROFILE_TRACE'],'a') as out: out.write(json.dumps(args)+'\\n')
raise SystemExit(37 if os.environ.get('PROFILE_FAIL') in args else 0)
'''
            for name in ("python3", "node", "ruby", "git"):
                tool = binaries / name
                tool.write_text(fake)
                tool.chmod(0o755)
            trace = root / "trace.jsonl"
            environment = {key: value for key, value in os.environ.items()
                           if key not in ("BASH_ENV", "ENV", "OTA_SIGNING_KEY", "OTA_SIGNING_KEY_FILE")}
            environment.update(PATH=str(binaries) + os.pathsep + os.defpath, PROFILE_TRACE=str(trace))
            for arguments, expected_flags in (([], ["--self-test"]), (["--quick"], [])):
                trace.unlink(missing_ok=True)
                result = subprocess.run(["/bin/bash", str(script), *arguments], cwd=root,
                                        env=environment, capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)
                calls = [json.loads(line) for line in trace.read_text().splitlines()]
                validators = [args for args in calls if args and args[0].startswith("scripts/check-") and args[0].endswith(".py")]
                self.assertEqual(len(validators), 7)
                self.assertTrue(all(args[1:] == expected_flags for args in validators), validators)
                result = subprocess.run(["/bin/bash", str(script), *arguments], cwd=root,
                    env={**environment, "PROFILE_FAIL": "scripts/check-host-gate-contract.py"},
                    capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 37)


if __name__ == "__main__":
    unittest.main()
