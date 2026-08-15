#!/usr/bin/env python3
import pathlib
import stat
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))
import capture_wake  # noqa: E402


class CaptureWakePrivacyTests(unittest.TestCase):
    VIN = "5YJ3E1EA1JF000001"

    def test_default_redacts_vin_and_long_hex_frames(self):
        line = f"vin={self.VIN} RX notify len=16: 00112233445566778899aabbccddeeff"
        redacted = capture_wake.redact_capture_line(line, self.VIN)
        self.assertNotIn(self.VIN, redacted)
        self.assertNotIn("00112233445566778899aabbccddeeff", redacted)
        self.assertIn("<VIN-redacted>", redacted)
        self.assertIn("<frame-redacted>", redacted)

    def test_default_redacts_firmware_spaced_hex_frame(self):
        frame = "00 11 22 33 44 55 66 77 88 99 aa bb cc dd ee ff"
        redacted = capture_wake.redact_capture_line(f"RX notify len=16: {frame}", self.VIN)
        self.assertNotIn(frame, redacted)
        self.assertEqual(redacted, "RX notify len=16: <frame-redacted>")

    def test_sensitive_opt_in_preserves_original(self):
        line = f"vin={self.VIN} frame=0011223344556677"
        self.assertEqual(capture_wake.redact_capture_line(line, self.VIN, True), line)

    def test_explicit_log_is_mode_0600_and_non_overwriting(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = pathlib.Path(temp_dir, "wake-capture-test.log")
            returned, handle = capture_wake.open_private_log(path)
            handle.write("redacted\n")
            handle.close()
            self.assertEqual(returned, path.resolve())
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            with self.assertRaises(FileExistsError):
                capture_wake.open_private_log(path)

    def test_default_log_has_private_parent(self):
        path, handle = capture_wake.open_private_log()
        try:
            handle.close()
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(path.parent.stat().st_mode), 0o700)
        finally:
            path.unlink(missing_ok=True)
            path.parent.rmdir()

    def test_checkout_output_must_use_ignored_capture_name(self):
        repo_root = pathlib.Path(__file__).resolve().parents[1]
        unsafe = repo_root / "diagnostic-secret.log"
        with self.assertRaisesRegex(OSError, "wake-capture"):
            capture_wake.open_private_log(unsafe)
        self.assertFalse(unsafe.exists())


if __name__ == "__main__":
    unittest.main()
