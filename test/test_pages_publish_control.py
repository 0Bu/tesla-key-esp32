#!/usr/bin/env python3
"""Exercise the actual no-follow marker owner, including unsafe-path refusals."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/pages_publish_control.py"
spec = importlib.util.spec_from_file_location("pages_control", SCRIPT)
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)


class Control(unittest.TestCase):
    def test_create_and_unchanged_inode(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            marker = root / control.MARKER
            control.check_marker(root)
            self.assertFalse(marker.exists(), "validation must not mutate a legacy original")
            control.ensure_static_site(root)
            before = marker.stat()
            control.ensure_static_site(root)
            self.assertEqual(marker.read_bytes(), b"")
            self.assertEqual((marker.stat().st_ino, marker.stat().st_nlink), (before.st_ino, 1))

    def test_unsafe_markers_refuse_without_changing_external_data(self):
        for kind in ("nonempty", "directory", "symlink", "broken", "hardlink", "fifo"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary); marker = root / control.MARKER
                outside = root / "untouched"; outside.write_bytes(b"")
                if kind == "nonempty": marker.write_bytes(b"do not truncate")
                elif kind == "directory": marker.mkdir()
                elif kind == "symlink": marker.symlink_to(outside)
                elif kind == "broken": marker.symlink_to(root / "missing")
                elif kind == "hardlink": os.link(outside, marker)
                else: os.mkfifo(marker)
                result = subprocess.run([sys.executable, str(SCRIPT), str(root)],
                                        capture_output=True, timeout=3)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(outside.read_bytes(), b"")
                if kind == "nonempty": self.assertEqual(marker.read_bytes(), b"do not truncate")
                if kind in ("symlink", "broken"): self.assertTrue(marker.is_symlink())

    def test_original_payload_is_not_mutated_or_generically_excluded(self):
        original = {"index.html": b"original", "manifest.json": b"bound", ".another": b"retained"}
        expected = control.expected_site(original)
        self.assertEqual(expected, {**original, ".nojekyll": b""})
        self.assertNotIn(".nojekyll", original)
        self.assertEqual(control.expected_site(expected), expected)
        with self.assertRaises(ValueError): control.expected_site({".nojekyll": b"unsafe"})


if __name__ == "__main__":
    unittest.main()
