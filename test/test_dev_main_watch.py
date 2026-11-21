#!/usr/bin/env python3
"""Offline supervision fixtures for the read-only Dev work guard."""

import fcntl
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/run-current-dev.py"
SPEC = importlib.util.spec_from_file_location("dev_main_watch", SCRIPT)
watch = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(watch)
SOURCE = "a" * 40
NEWER = "b" * 40
DEV = {
    "GITHUB_EVENT_NAME": "push",
    "GITHUB_REF": "refs/heads/main",
    "GITHUB_REPOSITORY": "0Bu/tesla-key-esp32",
    "GITHUB_SHA": SOURCE,
}


class DevMainWatchTest(unittest.TestCase):
    def run_dev(self, command, probe):
        with (
            patch.dict(os.environ, DEV),
            patch.object(watch, "live_main", side_effect=probe),
            patch.object(watch, "POLL_SECONDS", 0.05),
            patch.object(watch, "TERMINATION_GRACE", 0.15),
            patch.object(watch.signal, "signal"),
        ):
            return watch.run(command)

    def test_probe_requires_exact_ref(self):
        for output, expected in (
            (f"{SOURCE}\trefs/heads/main\n", SOURCE),
            (f"{NEWER}\trefs/heads/main\n", NEWER),
            (f"{NEWER}\trefs/tags/main\n", None),
            (f"{NEWER}\trefs/heads/main\nextra\n", None),
            ("bad ref\n", None),
        ):
            with self.subTest(output=output), patch.object(watch.subprocess, "Popen") as git:
                git.return_value.communicate.return_value = (output.encode(), None)
                git.return_value.returncode = 0
                self.assertEqual(watch.live_main(), expected)
                args, kwargs = git.call_args
                self.assertEqual(args[0][-2:], ["https://github.com/0Bu/tesla-key-esp32.git", DEV["GITHUB_REF"]])
                self.assertTrue(kwargs["start_new_session"])
                git.return_value.communicate.assert_called_once_with(timeout=5)

    def test_timed_out_ref_probe_stops_its_descendant(self):
        child = (
            "import fcntl, signal, sys, time; from pathlib import Path; "
            "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
            "lock = open(sys.argv[1], 'w'); fcntl.flock(lock, fcntl.LOCK_EX); "
            "Path(sys.argv[2]).touch(); time.sleep(120)"
        )
        with tempfile.TemporaryDirectory() as directory:
            lock, marker = (Path(directory) / name for name in ("locked", "ready"))
            # The Git leader exits, but a transport child leaves stdout open.
            git_program = "import subprocess, sys; subprocess.Popen(sys.argv[1:])"
            fake_git = [sys.executable, "-c", git_program, sys.executable, "-c", child, str(lock), str(marker)]
            popen = subprocess.Popen
            with (
                patch.object(watch.subprocess, "Popen", side_effect=lambda _args, **kwargs: popen(fake_git, **kwargs)),
                patch.object(watch, "GIT_TIMEOUT", 1),
                patch.object(watch, "TERMINATION_GRACE", 0.15),
            ):
                self.assertIsNone(watch.live_main())
            self.assertTrue(marker.exists())
            with lock.open() as stream:
                deadline = time.monotonic() + 2
                while True:
                    try:
                        fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        break
                    except BlockingIOError:
                        if time.monotonic() >= deadline:
                            self.fail("timed-out Git transport is still holding the lock")
                        time.sleep(0.05)

    def test_initial_stale_ref_never_executes_work(self):
        with patch.object(watch.subprocess, "Popen") as child:
            self.assertEqual(self.run_dev(["unstarted"], [NEWER]), watch.STALE)
            child.assert_not_called()

    def test_current_and_unknown_ref_preserve_command_result(self):
        command = [sys.executable, "-c", "raise SystemExit(23)"]
        for head in (SOURCE, None):
            with self.subTest(head=head):
                self.assertEqual(self.run_dev(command, lambda: head), 23)

    def test_newer_head_stops_running_work_even_when_term_is_ignored(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "pid"
            command = [sys.executable, "-c", (
                "import os, signal, sys, time; from pathlib import Path; "
                "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                "Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(120)"
            ), str(marker)]
            probes = 0

            def head():
                nonlocal probes
                probes += 1
                if probes == 1:
                    return SOURCE
                deadline = time.monotonic() + 3
                while not marker.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(marker.exists())
                return NEWER

            self.assertEqual(self.run_dev(command, head), watch.STALE)
            with self.assertRaises(ProcessLookupError):
                os.kill(int(marker.read_text()), 0)

    def test_outside_canonical_dev_is_an_exact_passthrough(self):
        for override in (
            {"GITHUB_EVENT_NAME": "workflow_dispatch"},
            {"GITHUB_EVENT_NAME": "pull_request", "GITHUB_REF": "refs/pull/1/merge"},
            {"GITHUB_REPOSITORY": "example/tesla-key-esp32"},
        ):
            with (
                self.subTest(override=override),
                patch.dict(os.environ, DEV | override),
                patch.object(watch.os, "execvp", side_effect=SystemExit(23)) as execute,
                patch.object(watch, "live_main") as probe,
            ):
                with self.assertRaises(SystemExit) as exit_status:
                    watch.run(["bash", "-e", "/path with spaces/step.sh"])
                self.assertEqual(exit_status.exception.code, 23)
                execute.assert_called_once_with("bash", ["bash", "-e", "/path with spaces/step.sh"])
                probe.assert_not_called()


if __name__ == "__main__":
    unittest.main()
