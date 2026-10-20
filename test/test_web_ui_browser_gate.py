#!/usr/bin/env python3
"""Controlled launch-policy tests; these never start a browser or prove DOM behavior."""

import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location("browser_gate", Path(__file__).with_name("web_ui_browser_gate.py"))
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


class BrowserLaunchPolicyTests(unittest.TestCase):
    def setUp(self):
        self.spawn = mock.patch.object(gate.subprocess, "Popen", side_effect=AssertionError("unexpected browser spawn")).start()
        self.addCleanup(mock.patch.stopall)

    def run_main(self, platform, strict=False):
        output = io.StringIO()
        with mock.patch.object(gate.sys, "platform", platform), \
                mock.patch.object(gate.sys, "argv", ["browser-gate"] + (["--require-browser"] if strict else [])), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            result = gate.main()
        return result, output.getvalue()

    def test_macos_normal_reports_skip_before_lookup_or_spawn(self):
        with mock.patch.object(gate, "browser_binary") as lookup:
            result, output = self.run_main("darwin")
        self.assertEqual(result, 0)
        self.assertIn("SKIP (no real-browser evidence)", output)
        lookup.assert_not_called()
        self.spawn.assert_not_called()

    def test_macos_strict_fails_before_lookup_or_spawn(self):
        with mock.patch.object(gate, "browser_binary") as lookup:
            result, output = self.run_main("darwin", strict=True)
        self.assertEqual(result, 2)
        self.assertIn("run-browser-tests-linux.sh", output)
        self.assertNotIn("PASS", output)
        lookup.assert_not_called()
        self.spawn.assert_not_called()

    def test_macos_explicit_browser_cannot_bypass_platform_policy(self):
        with mock.patch.object(gate.sys, "platform", "darwin"), \
                mock.patch.dict(gate.os.environ, {"BROWSER_BIN": "/configured/browser"}), \
                mock.patch.object(gate.shutil, "which", return_value="/configured/browser") as lookup:
            self.assertIsNone(gate.browser_binary())
        lookup.assert_not_called()
        self.spawn.assert_not_called()

    def test_direct_macos_profile_is_refused_before_spawn(self):
        with mock.patch.object(gate.sys, "platform", "darwin"):
            with self.assertRaisesRegex(RuntimeError, "only on Linux"):
                gate.run_profile("/configured/browser", Path("/tmp/page.html"), "desktop", "1200,900", Path("/tmp/profile"))
        self.spawn.assert_not_called()

    def test_missing_linux_browser_fails_strict(self):
        with mock.patch.object(gate, "browser_binary", return_value=None):
            result, output = self.run_main("linux", strict=True)
        self.assertEqual(result, 2)
        self.assertIn("no Chrome/Chromium", output)
        self.spawn.assert_not_called()

    def test_missing_linux_browser_reports_optional_skip(self):
        with mock.patch.object(gate, "browser_binary", return_value=None):
            result, output = self.run_main("linux")
        self.assertEqual(result, 0)
        self.assertIn("SKIP", output)
        self.spawn.assert_not_called()

    def test_linux_main_requires_both_real_profile_calls(self):
        with mock.patch.object(gate, "browser_binary", return_value="/usr/bin/chromium"), \
                mock.patch.object(gate, "assembled_page", return_value="<html>fixture</html>"), \
                mock.patch.object(gate, "run_profile") as profile:
            result, output = self.run_main("linux", strict=True)
        self.assertEqual(result, 0)
        self.assertEqual([call.args[2:4] for call in profile.call_args_list],
                         [("desktop", "1200,900"), ("mobile", "390,844")])
        self.assertIn("PASS", output)

    def test_linux_profile_retains_process_and_complete_dom_validation(self):
        def process(command, **kwargs):
            self.assertIn("--headless", command)
            self.assertIn("--dump-dom", command)
            self.assertTrue(command[-1].startswith("file://"))
            self.assertTrue(kwargs["start_new_session"])
            kwargs["stdout"].write('<html><div id="browser-gate-result" data-result="pass"></div></html>')
            return mock.Mock(wait=mock.Mock(return_value=0))
        self.spawn.side_effect = process
        with mock.patch.object(gate.sys, "platform", "linux"), tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            gate.run_profile("/usr/bin/chromium", root / "page.html", "desktop", "1200,900", root / "profile")
        self.spawn.assert_called_once()

    def test_linux_timeout_terminates_only_its_own_process_group(self):
        process = mock.Mock(pid=24680)
        process.wait.side_effect = [gate.subprocess.TimeoutExpired("controlled", 45), 0]
        self.spawn.side_effect = lambda *args, **kwargs: process
        with mock.patch.object(gate.sys, "platform", "linux"), \
                mock.patch.object(gate.os, "killpg") as kill, tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(RuntimeError, "timed out before a complete DOM"):
                gate.run_profile("/usr/bin/chromium", root / "page.html", "desktop", "1200,900", root / "profile")
        kill.assert_called_once_with(24680, gate.signal.SIGTERM)
        self.assertEqual(process.wait.call_args_list, [mock.call(timeout=45), mock.call(timeout=2)])

    def test_linux_stuck_process_group_has_bounded_kill_escalation(self):
        process = mock.Mock(pid=24680)
        process.wait.side_effect = [gate.subprocess.TimeoutExpired("controlled", 45),
                                    gate.subprocess.TimeoutExpired("controlled", 2), 0]
        self.spawn.side_effect = lambda *args, **kwargs: process
        with mock.patch.object(gate.sys, "platform", "linux"), \
                mock.patch.object(gate.os, "killpg") as kill, tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(RuntimeError, "timed out before a complete DOM"):
                gate.run_profile("/usr/bin/chromium", root / "page.html", "desktop", "1200,900", root / "profile")
        self.assertEqual(kill.call_args_list, [mock.call(24680, gate.signal.SIGTERM),
                                              mock.call(24680, gate.signal.SIGKILL)])
        self.assertEqual(process.wait.call_args_list, [mock.call(timeout=45), mock.call(timeout=2), mock.call(timeout=2)])

    def test_linux_profile_without_dom_marker_fails(self):
        self.spawn.side_effect = lambda *args, **kwargs: mock.Mock(wait=mock.Mock(return_value=0))
        with mock.patch.object(gate.sys, "platform", "linux"), tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(RuntimeError, "DOM/accessibility gate failed"):
                gate.run_profile("/usr/bin/chromium", root / "page.html", "desktop", "1200,900", root / "profile")


if __name__ == "__main__":
    unittest.main()
