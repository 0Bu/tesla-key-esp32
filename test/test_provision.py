#!/usr/bin/env python3
import io
import json
import os
import pathlib
import re
import stat
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import provision  # noqa: E402


def firmware_management_reply(ok, command, vin="", reason="test reply"):
    """Derive the fixture keys from the actual straight-line make_response producer."""
    root = pathlib.Path(__file__).resolve().parents[1]
    source = (root / "main/http_common.cpp").read_text(encoding="utf-8")
    match = re.search(r"cJSON\* make_response\([^{}]+\)\s*\{(.*?)^}", source, re.S | re.M)
    if not match:
        raise AssertionError("cannot locate production management reply producer")
    body = match.group(1)
    wrappers = re.findall(r'cJSON\* response = json\.object\(json\.root\(\), "([^"\n]+)"\);', body)
    flags = re.findall(r'json\.boolean\(response, "([^"\n]+)", result\);', body)
    strings = re.findall(r'json\.string\(response, "([^"\n]+)", (command|vin|reason)\);', body)
    if len(wrappers) != 1 or len(flags) != 1 or len(strings) != 3:
        raise AssertionError("management reply producer changed; review the fixture contract")
    expected = (f' tk::JsonBuilder json; cJSON* response = json.object(json.root(), "{wrappers[0]}"); '
                f'json.boolean(response, "{flags[0]}", result); ' +
                ' '.join(f'json.string(response, "{key}", {value});' for key, value in strings) +
                ' return json.release(); ')
    if re.sub(r"\s+", "", expected) != re.sub(r"\s+", "", body):
        raise AssertionError("management reply producer is no longer the reviewed straight-line shape")
    values = {"command": command, "vin": vin, "reason": reason}
    payload = {flags[0]: ok, **{key: values[value] for key, value in strings}}
    return {wrappers[0]: payload}


class ProvisionTests(unittest.TestCase):
    def test_wifi_password_contract_and_utf8_byte_boundaries(self):
        with self.assertRaises(provision.ProvisionError):
            provision.validate_wifi("wifi", "x" * 7)
        provision.validate_wifi("wifi", "x" * 8)
        provision.validate_wifi("wifi", "x" * 63)
        provision.validate_wifi("wifi", "a1" * 32)  # exactly 64 ASCII hex = raw PSK
        with self.assertRaises(provision.ProvisionError):
            provision.validate_wifi("wifi", "g" * 64)

        provision.validate_wifi("wifi", "é" * 31 + "a")  # 63 UTF-8 bytes
        with self.assertRaises(provision.ProvisionError):
            provision.validate_wifi("wifi", "é" * 32)  # 64 bytes, but not an ASCII raw PSK
        provision.validate_wifi("é" * 16, "password")  # 32-byte SSID
        with self.assertRaises(provision.ProvisionError):
            provision.validate_wifi("é" * 16 + "a", "password")

    def test_legacy_serial_path_fails_before_any_write(self):
        with mock.patch.object(provision, "request") as request:
            self.assertEqual(provision.main(["--port", "/dev/ttyUSB0"]), 2)
            request.assert_not_called()

    def test_password_on_argv_is_refused(self):
        with mock.patch.object(provision, "request") as request:
            rc = provision.main(
                ["--url", "http://device", "--mode", "lan", "--ssid", "wifi", "--password", "secret123"]
            )
            self.assertEqual(rc, 2)
            request.assert_not_called()

    def test_password_file_must_be_private(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = pathlib.Path(temp_dir, "pass")
            path.write_text("secret123\n", encoding="utf-8")
            path.chmod(0o644)
            args = provision.build_parser().parse_args(
                ["--url", "http://device", "--ssid", "wifi", "--password-file", str(path)]
            )
            with self.assertRaises(provision.ProvisionError):
                provision.read_password(args)
            path.chmod(0o600)
            self.assertEqual(provision.read_password(args), "secret123")

    def test_setup_uses_atomic_form_api_and_never_logs_password(self):
        calls = []

        def fake_request(base, path, **kwargs):
            calls.append((base, path, kwargs))
            return "<h2>Saved &#9989;</h2>", "text/html"

        stdout = io.StringIO()
        with mock.patch.object(provision, "request", side_effect=fake_request), mock.patch(
            "sys.stdin", io.StringIO("secret123\n")
        ), mock.patch("sys.stdout", stdout):
            rc = provision.main(
                [
                    "--url",
                    "http://192.168.4.1",
                    "--mode",
                    "setup",
                    "--ssid",
                    "wifi",
                    "--password-stdin",
                    "--vin",
                    "5YJ3E1EA1JF000001",
                ]
            )
        self.assertEqual(rc, 0)
        self.assertEqual(calls[0][1], "/save")
        self.assertTrue(calls[0][2]["form"])
        self.assertEqual(calls[0][2]["payload"]["pass"], "secret123")
        self.assertNotIn("secret123", stdout.getvalue())

    def test_setup_preserves_firmware_warning_without_retry(self):
        for warning in ("configuration saved for the next boot; firmware remains unconfirmed",
                        "configuration saved for the next boot; firmware verification is uncertain"):
            body = "<h2>Saved</h2><p id=firmware-warning>" + warning + "</p>"
            stdout = io.StringIO()
            with mock.patch.object(provision, "request", return_value=(body, "text/html")) as request, \
                    mock.patch("sys.stdout", stdout):
                result = provision.main(["--url", "http://device.invalid", "--mode", "setup",
                                         "--ssid", "offline", "--open-network"])
            self.assertEqual(result, 0)
            self.assertEqual(request.call_count, 1)
            self.assertEqual(request.call_args.args[1], "/save")
            self.assertIn(warning, stdout.getvalue())

    def test_lan_wifi_uses_rollback_endpoint(self):
        calls = []

        def fake_request(base, path, **kwargs):
            calls.append((path, kwargs))
            return json.dumps(firmware_management_reply(True, "set_wifi")), "application/json"

        stdout = io.StringIO()
        with mock.patch.object(provision, "request", side_effect=fake_request), mock.patch("sys.stdout", stdout):
            rc = provision.main(
                ["--url", "http://device", "--mode", "lan", "--ssid", "wifi", "--open-network"]
            )
        self.assertEqual(rc, 0)
        self.assertIn("after restart", stdout.getvalue())
        self.assertNotIn("is rebooting", stdout.getvalue())
        self.assertEqual(calls, [("/set_wifi", {"payload": {"ssid": "wifi", "pass": ""}, "timeout": 15.0})])

    def test_lan_management_refusals_never_retry(self):
        for endpoint in ("set_wifi", "set_vin"):
            rejected = [
                firmware_management_reply(False, endpoint),
                {"result": True},
                {"result": True, "response": {"result": False}},
                {"response": {"result": 1}},
                {"response": {"result": "true"}},
                {"response": None},
                {"response": []},
                [],
            ]
            for body in rejected:
                with self.subTest(endpoint=endpoint, body=body), mock.patch.object(
                    provision, "request", return_value=(json.dumps(body), "application/json")
                ) as request:
                    with self.assertRaises(provision.ProvisionError):
                        if endpoint == "set_wifi":
                            provision.provision_wifi("http://device", "wifi", "", 15.0)
                        else:
                            provision.provision_vin("http://device", "TESTVIN0000000001", 15.0)
                    self.assertEqual(request.call_count, 1)

    def test_degraded_management_outcomes_preserve_uncertainty_without_retry(self):
        for endpoint in ("set_wifi", "set_vin"):
            for outcome in ("unknown", "applied_recovery_pending"):
                body = {"response": {"result": False, "outcome": outcome,
                                     "reply_degraded": True, "reason": "identity saved; recovery pending"}}
                with self.subTest(endpoint=endpoint, outcome=outcome), mock.patch.object(
                    provision, "request", return_value=(json.dumps(body), "application/json")
                ) as request:
                    with self.assertRaisesRegex(provision.ProvisionError, "outcome not confirmed.*inspect device status") as error:
                        if endpoint == "set_wifi":
                            provision.provision_wifi("http://device", "wifi", "", 15.0)
                        else:
                            provision.provision_vin("http://device", "TESTVIN0000000001", 15.0)
                    self.assertNotIn("refused", str(error.exception))
                    self.assertIn(body["response"]["reason"], str(error.exception))
                    self.assertEqual(request.call_count, 1)

    def test_vin_change_needs_explicit_confirmation(self):
        with mock.patch.object(provision, "request") as request:
            rc = provision.main(
                ["--url", "http://device", "--mode", "lan", "--vin-only", "--vin", "5YJ3E1EA1JF000001"]
            )
            self.assertEqual(rc, 2)
            request.assert_not_called()

    def test_confirmed_vin_change_uses_only_set_vin(self):
        stdout = io.StringIO()
        with mock.patch.object(
            provision, "request", return_value=(json.dumps(firmware_management_reply(True, "set_vin")), "application/json")
        ) as request, mock.patch("sys.stdout", stdout):
            rc = provision.main(
                [
                    "--url",
                    "http://device",
                    "--mode",
                    "lan",
                    "--vin-only",
                    "--confirm-vin-change",
                    "--vin",
                    "5YJ3E1EA1JF000001",
                ]
            )
        self.assertEqual(rc, 0)
        self.assertEqual(request.call_args.args[1], "/set_vin")
        self.assertIn("a changed VIN clears pairing", stdout.getvalue())
        self.assertNotIn("NVS was not overwritten", stdout.getvalue())
        self.assertNotIn("is rebooting", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
