#!/usr/bin/env python3
"""Controlled launch-policy tests; these never start a browser or prove DOM behavior."""

import contextlib
import importlib.util
import io
import json
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
                gate.run_profile(None, Path("/tmp/page.html"), gate.PROFILES[0])
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

    def test_linux_main_requires_real_browser_runner(self):
        with mock.patch.object(gate, "browser_binary", return_value="/usr/bin/chromium"), \
                mock.patch.object(gate, "assembled_page", return_value="<html>fixture</html>"), \
                mock.patch.object(gate, "run_browser") as browser:
            result, output = self.run_main("linux", strict=True)
        self.assertEqual(result, 0)
        self.assertEqual(browser.call_count, 1)
        self.assertIn("PASS", output)

    def test_linux_browser_uses_one_bounded_pipe_owner_and_three_profiles(self):
        connection = mock.MagicMock()
        owner = mock.MagicMock()
        owner.return_value.__enter__.return_value = connection
        with mock.patch.object(gate.sys, "platform", "linux"), \
                mock.patch.object(gate, "cdp_type", return_value=owner), \
                mock.patch.object(gate, "run_profile") as profile, \
                mock.patch.object(gate, "report"), tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            gate.run_browser("/usr/bin/chromium", root / "page.html", root)
        self.assertEqual(owner.call_count, 1)
        self.assertIn("--remote-debugging-pipe", owner.call_args.args[1])
        self.assertNotIn("--dump-dom", owner.call_args.args[1])
        self.assertEqual([call.args[2] for call in profile.call_args_list], list(gate.PROFILES))
        self.assertEqual([p[1:] for p in gate.PROFILES], [(390,844,"light"),(768,900,"light"),(1200,900,"dark")])
        connection.call.assert_called_once_with("Browser.getVersion", seconds=45)
        owner.return_value.__exit__.assert_called_once()

    def test_profile_omission_and_failure_cannot_pass(self):
        owner = mock.MagicMock()
        with mock.patch.object(gate.sys, "platform", "linux"), \
                mock.patch.object(gate, "cdp_type", return_value=owner), \
                mock.patch.object(gate, "report"), tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(gate, "run_profile"), mock.patch.object(gate,"PROFILES",gate.PROFILES[:2]):
                with self.assertRaisesRegex(RuntimeError,"three populated"):
                    gate.run_browser("/usr/bin/chromium",root/"page.html",root)
            with mock.patch.object(gate,"run_profile",side_effect=RuntimeError("controlled DOM failure")):
                with self.assertRaisesRegex(RuntimeError,"controlled DOM failure"):
                    gate.run_browser("/usr/bin/chromium",root/"page.html",root)
        self.assertEqual(owner.return_value.__exit__.call_count, 2)

    def test_runner_failure_is_reported_without_pass(self):
        with mock.patch.object(gate, "browser_binary", return_value="/usr/bin/chromium"), \
                mock.patch.object(gate, "assembled_page", return_value="<html>fixture</html>"), \
                mock.patch.object(gate, "run_browser", side_effect=RuntimeError("controlled deadline")):
            result, output = self.run_main("linux", strict=True)
        self.assertEqual(result, 1)
        self.assertIn("controlled deadline",output)
        self.assertNotIn("PASS",output)

    def test_each_keyboard_key_is_sent_as_distinct_down_up_with_default_text(self):
        connection=mock.Mock()
        gate.dispatch_key(connection,"owned","Enter","Enter",13,"\r")
        gate.dispatch_key(connection,"owned"," ","Space",32," ")
        calls=connection.call.call_args_list
        self.assertEqual([call.args[1]["type"] for call in calls],["keyDown","keyUp","keyDown","keyUp"])
        self.assertEqual([call.args[1]["key"] for call in calls],["Enter","Enter"," "," "])
        self.assertEqual(calls[0].args[1]["text"],"\r")
        self.assertEqual(calls[2].args[1]["text"]," ")
        self.assertTrue(all(call.args[0]=="Input.dispatchKeyEvent" and call.args[2]=="owned" for call in calls))


class ProfileProtocolTests(unittest.TestCase):
    """Controlled CDP state, separate from actual Linux/browser acceptance."""

    class Peer:
        def __init__(self, fault=None):
            self.fault=fault
            self.calls=[]
            self.actions=0
            self.keys=[]
            self.clicks=[]

        def call(self, method, parameters=None, session=None):
            self.calls.append((method,parameters,session))
            if method=="Target.createTarget": return {"targetId":"owned"}
            if method=="Target.attachToTarget": return {"sessionId":"session"}
            if method=="Input.dispatchKeyEvent":
                key=parameters["key"]
                self.keys.append({"type":parameters["type"].lower(),
                                  "key":key,"trusted":self.fault!="untrusted"})
                if parameters["type"]=="keyUp" and key!="Tab":
                    count=0 if self.fault=="missing-action" else (2 if self.fault=="double-action" else 1)
                    self.actions+=count
                    self.clicks.extend([{"trusted":True}]*count)
            return {}

        def evaluate(self, expression, session):
            if expression==gate.KEYBOARD_SETUP:
                return {"bodyFocused":self.fault!="body","requests":2}
            if expression==gate.KEYBOARD_SNAPSHOT:
                result={"actions":self.actions,"keys":self.keys,"clicks":self.clicks,"requests":2,
                        "errors":[],"viewport":390,"documentWidth":390,
                        "focus":{"active":True,"visible":self.fault!="focus","name":"Start charging",
                                 "style":"solid","width":2,"contrast":5},
                        "boxes":[{"left":0,"right":390,"width":390,"scrollWidth":390,"clientWidth":390}]*4}
                if self.fault=="request": result["requests"]=3
                if self.fault=="overflow": result["documentWidth"]=500
                return json.loads(json.dumps(result))
            if "document.activeElement===" in expression: return True
            return {"result":"fail" if self.fault=="dom" else "pass","text":"controlled","width":390,"dark":False}

    def test_full_phase_sequence_and_both_native_keys_are_required(self):
        peer=self.Peer()
        with mock.patch.object(gate.sys,"platform","linux"),mock.patch.object(gate,"report") as report:
            gate.run_profile(peer,Path("/tmp/offline.html"),gate.PROFILES[0])
        self.assertEqual([call.args[1] for call in report.call_args_list],
                         ["assembled-dom","keyboard-setup","trusted-tab","trusted-Enter","trusted-Space"])
        self.assertEqual(peer.actions,2)
        self.assertEqual([event["key"] for event in peer.keys],["Tab","Tab","Enter","Enter"," "," "])
        self.assertEqual(peer.calls[-1][0],"Target.closeTarget")
        self.assertTrue(any(call[0]=="Emulation.setDeviceMetricsOverride" for call in peer.calls))
        self.assertTrue(any(call[0]=="Emulation.setEmulatedMedia" for call in peer.calls))

    def test_each_required_runtime_refusal_is_effective(self):
        for fault in ("dom","body","focus","missing-action","double-action","untrusted","request","overflow"):
            with self.subTest(fault=fault),mock.patch.object(gate.sys,"platform","linux"), \
                    mock.patch.object(gate,"report"),self.assertRaises(RuntimeError):
                gate.run_profile(self.Peer(fault),Path("/tmp/offline.html"),gate.PROFILES[0])


if __name__ == "__main__":
    unittest.main()
