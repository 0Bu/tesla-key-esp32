#!/usr/bin/env python3
import io
import json
import os
import pathlib
import stat
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import provision  # noqa: E402


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

    def test_lan_wifi_uses_rollback_endpoint(self):
        calls = []

        def fake_request(base, path, **kwargs):
            calls.append((path, kwargs))
            return json.dumps({"result": True}), "application/json"

        with mock.patch.object(provision, "request", side_effect=fake_request):
            rc = provision.main(
                ["--url", "http://device", "--mode", "lan", "--ssid", "wifi", "--open-network"]
            )
        self.assertEqual(rc, 0)
        self.assertEqual(calls, [("/set_wifi", {"payload": {"ssid": "wifi", "pass": ""}, "timeout": 15.0})])

    def test_vin_change_needs_explicit_confirmation(self):
        with mock.patch.object(provision, "request") as request:
            rc = provision.main(
                ["--url", "http://device", "--mode", "lan", "--vin-only", "--vin", "5YJ3E1EA1JF000001"]
            )
            self.assertEqual(rc, 2)
            request.assert_not_called()

    def test_confirmed_vin_change_uses_only_set_vin(self):
        with mock.patch.object(
            provision, "request", return_value=(json.dumps({"result": True}), "application/json")
        ) as request:
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


if __name__ == "__main__":
    unittest.main()
