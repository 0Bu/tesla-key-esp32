#!/usr/bin/env python3
"""Exercise real local check execution, invalidation, failures and concurrent consumers."""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import fcntl
import signal
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/run-local-checks.py"
spec = importlib.util.spec_from_file_location("local_checks", SCRIPT)
checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checks)


class LocalChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tesla-local-checks-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / "scripts").mkdir()
        (self.root / ".gitignore").write_text("/.tmp/\n")
        (self.root / "source.cpp").write_text("original\n")
        self.runner = self.root / "scripts/run-mock-tests.sh"
        self.runner.write_text("#!/bin/sh\nset -eu\n[ \"$#\" = 1 ] && [ \"$1\" = --require-all ]\n"
                               "printf 'executed\\n' >> .tmp/count\n")
        self.runner.chmod(0o755)
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.git("add", ".")
        self.git("-c", "core.hooksPath=/dev/null", "-c", "user.name=Fixture",
                 "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture")
        self.runtime = patch.object(checks, "runtime_identity", return_value="fixed-runtime")
        self.runtime.start()
        self.addCleanup(self.runtime.stop)
        self.environment = patch.dict(os.environ, dict.fromkeys(("OTA_SIGNING_KEY", "OTA_SIGNING_KEY_FILE", "CI", "GITHUB_ACTIONS", "BASH_ENV", "ENV",
            "NODE_OPTIONS", "NODE_PATH", "RUBYOPT", "RUBYLIB", "PYTHONPATH", "LD_PRELOAD", "LD_LIBRARY_PATH",
            "DYLD_INSERT_LIBRARIES", "DYLD_LIBRARY_PATH", "CMAKE_TOOLCHAIN_FILE", "CC", "CXX", "BROWSER_BIN"), ""))
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.root), *args], check=True, capture_output=True)

    def run_checks(self, **kwargs):
        return checks.run(self.root, **kwargs)

    def count(self):
        counter = self.root / ".tmp/count"
        return len(counter.read_text().splitlines()) if counter.exists() else 0

    def test_identical_inputs_reused_force_executes(self):
        self.assertEqual(self.run_checks(), 0)
        self.assertEqual(self.run_checks(), 0)
        self.assertEqual(self.count(), 1)
        self.assertEqual(self.run_checks(force=True), 0)
        self.assertEqual(self.count(), 2)

    def test_content_new_files_deletions_modes_and_runner_invalidate(self):
        self.run_checks()
        source = self.root / "source.cpp"
        source.write_text("changed\n")
        self.run_checks()
        extra = self.root / "new.hpp"
        extra.write_text("new\n")
        self.run_checks()
        extra.chmod(0o755)
        self.run_checks()
        extra.unlink()
        source.unlink()
        self.run_checks()
        self.runner.write_text(self.runner.read_text() + "# changed test runner\n")
        self.run_checks()
        self.assertEqual(self.count(), 6)

    def test_runtime_changes_invalidate(self):
        self.run_checks()
        with patch.object(checks, "runtime_identity", return_value="changed-toolchain"):
            self.run_checks()
        self.assertEqual(self.count(), 2)

    def test_actual_runtime_tracks_browser_environment_and_selected_compiler_bytes(self):
        self.runtime.stop()
        executable = self.root / ".tmp/compiler"
        executable.parent.mkdir(exist_ok=True)
        executable.write_text("#!/bin/sh\nexit 0\n")
        executable.chmod(0o755)
        with patch.dict(os.environ, {"CXX": str(executable)}), patch.object(checks.platform, "system", return_value="Linux"):
            before = checks.runtime_identity(self.root)
            executable.write_text("#!/bin/sh\nexit 37\n")
            self.assertNotEqual(checks.runtime_identity(self.root), before)
            before = checks.runtime_identity(self.root)
            with patch.dict(os.environ, {"BROWSER_BIN": str(executable)}):
                self.assertNotEqual(checks.runtime_identity(self.root), before)
        build = self.root / "build_mock"
        build.mkdir()
        (build / "CMakeCache.txt").write_text(f"CMAKE_CXX_COMPILER:FILEPATH={executable}\n")
        with patch.object(checks.platform, "system", return_value="Linux"):
            before = checks.runtime_identity(self.root)
            executable.write_text("#!/bin/sh\nexit 38\n")
            self.assertNotEqual(checks.runtime_identity(self.root), before)

    def test_indirect_environment_never_reused(self):
        for overrides in ({"BASH_ENV": "/synthetic/startup"}, {"NODE_OPTIONS": "--max-old-space-size=4096"},
                          {"CXX": "ccache clang++"}):
            with patch.dict(os.environ, overrides):
                # Exercise reuse policy without executing a shell startup file in the fixture.
                with patch.object(checks, "execute", return_value=0):
                    self.run_checks()
                    self.run_checks()
                    self.assertFalse((self.root / ".tmp/local-checks/host.json").exists())

    def test_actual_runtime_tracks_only_the_browser_image(self):
        self.runtime.stop()
        images = {"browser": "sha256:browser-before", "other": "sha256:unrelated-before"}
        which = checks.shutil.which
        def executable(name):
            return sys.executable if name == "docker" else which(name)
        def image_list(command, **_kwargs):
            self.assertEqual(command, ["docker", "image", "ls", "--quiet", "--no-trunc", checks.BROWSER_IMAGE_REF])
            identifiers = [images["browser"]] if command[-1] == checks.BROWSER_IMAGE_REF else list(images.values())
            identifiers = [value for value in identifiers if value]
            return subprocess.CompletedProcess(command, 0, stdout=("\n".join(identifiers) + "\n").encode(), stderr=b"")
        launcher = (ROOT / "scripts/run-browser-tests-linux.sh").read_text()
        self.assertIn(f'--tag "{checks.BROWSER_IMAGE_REF}"', launcher)
        with patch.object(checks.platform, "system", return_value="Darwin"), \
                patch.object(checks.platform, "platform", return_value="controlled-host"), \
                patch.object(checks.shutil, "which", side_effect=executable), \
                patch.object(checks.subprocess, "run", side_effect=image_list):
            before = checks.runtime_identity(self.root)
            images["other"] = "sha256:unrelated-after"
            self.assertEqual(checks.runtime_identity(self.root), before)
            images["browser"] = "sha256:browser-after"
            after = checks.runtime_identity(self.root)
            self.assertNotEqual(after, before)
            images["browser"] = None
            self.assertNotEqual(checks.runtime_identity(self.root), after)

    def test_actual_runtime_removes_call_metadata_and_tracks_other_environment(self):
        self.runtime.stop()
        self.runner.write_text(self.runner.read_text() + '[ -z "${CODEX_TOOL_CALL_ID+x}" ]\n')
        with patch.object(checks.platform, "system", return_value="Linux"):
            with patch.dict(os.environ, {"CODEX_TOOL_CALL_ID": "first-call"}):
                before = checks.runtime_identity(self.root)
                self.assertNotIn("CODEX_TOOL_CALL_ID", checks.subprocess_environment())
                self.assertEqual(self.run_checks(), 0)
            with patch.dict(os.environ, {"CODEX_TOOL_CALL_ID": "second-call"}):
                self.assertEqual(checks.runtime_identity(self.root), before)
                self.assertEqual(self.run_checks(), 0)
                self.assertEqual(self.count(), 1)
                with patch.dict(os.environ, {"TESLA_LOCAL_CHECK_TEST_VARIANT": "changed"}):
                    self.assertNotEqual(checks.runtime_identity(self.root), before)
                    self.assertEqual(self.run_checks(), 0)
                    self.assertEqual(self.count(), 2)

    def test_partial_staging_never_reuses_a_working_tree_result(self):
        (self.root / "source.cpp").write_text("unstaged\n")
        self.run_checks()
        self.run_checks(staged=True)
        self.run_checks(staged=True)
        self.assertEqual(self.count(), 3)
        self.assertFalse((self.root / ".tmp/local-checks/host.json").exists())

    def test_executed_git_environment_matches_the_sanitized_snapshot(self):
        self.runner.write_text(self.runner.read_text() +
            '[ -z "${GIT_DIR-}" ] && [ -z "${GIT_CONFIG_COUNT-}" ] && [ -z "${CODEX_TOOL_CALL_ID+x}" ]\n')
        with patch.dict(os.environ, {"GIT_DIR": "/synthetic/foreign", "GIT_CONFIG_COUNT": "99",
                                    "CODEX_TOOL_CALL_ID": "synthetic-call"}):
            self.assertEqual(self.run_checks(force=True), 0)

    def test_failure_does_not_leave_success(self):
        self.run_checks()
        self.runner.write_text(self.runner.read_text() + "exit 37\n")
        self.assertEqual(self.run_checks(force=True), 37)
        self.assertEqual(self.run_checks(), 37)
        self.assertFalse((self.root / ".tmp/local-checks/host.json").exists())
        self.assertEqual(self.count(), 3)

    def test_source_changed_during_execution_is_rejected(self):
        self.runner.write_text(self.runner.read_text() + "printf changed > source.cpp\n")
        with self.assertRaisesRegex(ValueError, "source changed"):
            self.run_checks()
        self.assertFalse((self.root / ".tmp/local-checks/host.json").exists())

    def test_ci_always_executes_and_never_publishes_local_receipt(self):
        self.run_checks()
        with patch.dict(os.environ, {"CI": "true"}):
            self.run_checks()
            self.run_checks()
        self.assertEqual(self.count(), 3)
        self.assertFalse((self.root / ".tmp/local-checks/host.json").exists())

    def test_corrupt_receipt_is_a_miss(self):
        self.run_checks()
        (self.root / ".tmp/local-checks/host.json").write_text('{"result":"passed"}')
        self.run_checks()
        self.assertEqual(self.count(), 2)

    def test_symlink_state_and_secret_inputs_are_rejected(self):
        (self.root / "private.key").write_text("synthetic do-not-read fixture")
        with self.assertRaisesRegex(ValueError, "secret-shaped"):
            self.run_checks()
        (self.root / "private.key").unlink()
        (self.root / "linked.cpp").symlink_to("source.cpp")
        with self.assertRaisesRegex(ValueError, "regular files"):
            self.run_checks()
        (self.root / "linked.cpp").unlink()
        self.run_checks()
        receipt = self.root / ".tmp/local-checks/host.json"
        receipt.unlink()
        outside = self.root / ".tmp/outside"
        outside.write_text("untouched")
        receipt.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.run_checks()
        self.assertEqual(outside.read_text(), "untouched")

    def test_concurrent_consumers_execute_once(self):
        self.runner.write_text(self.runner.read_text() + "sleep 0.3\n")
        program = ("import importlib.util,pathlib;"
                   f"s=importlib.util.spec_from_file_location('checks',{str(SCRIPT)!r});"
                   "m=importlib.util.module_from_spec(s);s.loader.exec_module(m);"
                   "m.runtime_identity=lambda _root:'fixed-runtime';"
                   f"raise SystemExit(m.run(pathlib.Path({str(self.root)!r})))")
        processes = [subprocess.Popen([sys.executable, "-c", program], stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE) for _ in range(2)]
        try:
            for process in processes:
                stdout, stderr = process.communicate(timeout=20)
                self.assertEqual(process.returncode, 0, (stdout, stderr))
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.wait()
        self.assertEqual(self.count(), 1)

    def controlled_subgate(self):
        hooks = self.root / "tools/agent-hooks"
        hooks.mkdir(parents=True)
        shutil.copyfile(ROOT / "tools/agent-hooks/run_with_timeout.py", hooks / "run_with_timeout.py")
        scratch = self.root / ".tmp"
        scratch.mkdir(exist_ok=True)
        (scratch / "child.py").write_text("import os,pathlib,signal,time\n"
            "pathlib.Path('.tmp/child.pid').write_text(str(os.getpid()))\n"
            "signal.signal(signal.SIGTERM,signal.SIG_IGN)\ntime.sleep(60)\n")
        self.runner.write_text("#!/bin/sh\nset -eu\nprintf '%s' $$ > .tmp/runner.pid\n"
            f"{sys.executable} tools/agent-hooks/run_with_timeout.py 30 {sys.executable} .tmp/child.py\n")

    def test_timeout_cleans_nested_session_before_releasing_lock(self):
        self.controlled_subgate()
        with patch.object(checks, "CHECK_TIMEOUT", 0.5):
            with self.assertRaises(subprocess.TimeoutExpired):
                self.run_checks()
        child = int((self.root / ".tmp/child.pid").read_text())
        with self.assertRaises(ProcessLookupError):
            os.kill(child, 0)
        with (self.root / ".tmp/local-checks/host.lock").open("r+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.assertFalse((self.root / ".tmp/local-checks/host.json").exists())

    def test_killed_coordinator_cannot_release_a_running_subgate_lock(self):
        self.controlled_subgate()
        program = ("import importlib.util,pathlib;"
                   f"s=importlib.util.spec_from_file_location('checks',{str(SCRIPT)!r});"
                   "m=importlib.util.module_from_spec(s);s.loader.exec_module(m);"
                   "m.runtime_identity=lambda _root:'fixed-runtime';"
                   f"raise SystemExit(m.run(pathlib.Path({str(self.root)!r})))")
        process = subprocess.Popen([sys.executable, "-c", program], stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 10
            while not (self.root / ".tmp/child.pid").exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue((self.root / ".tmp/child.pid").exists())
            process.kill()
            process.wait(timeout=5)
            with (self.root / ".tmp/local-checks/host.lock").open("r+") as lock:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            runner_pid = self.root / ".tmp/runner.pid"
            if runner_pid.exists():
                try:
                    os.killpg(int(runner_pid.read_text()), signal.SIGTERM)
                except ProcessLookupError:
                    pass
            # The real bounded wrapper kills/reaps its TERM-resistant child within two seconds.
            time.sleep(3)


if __name__ == "__main__":
    unittest.main()
