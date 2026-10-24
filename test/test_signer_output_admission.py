#!/usr/bin/env python3
"""Run the real signer output preflight with inert inputs and a refusing key-tool spy."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SignerOutputAdmission(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="signer-output-admission-")
        cls.base = Path(cls.temporary.name)
        cls.key = cls.base / "throwaway-rsa3072.pem"
        subprocess.run(["openssl", "genrsa", "-out", str(cls.key), "3072"],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
        cls.key.chmod(0o600)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def exercise(self, collision: str | None, kind: str = "file", omission: str | None = None) -> bool:
        with tempfile.TemporaryDirectory(prefix="attempt-", dir=self.base) as temporary:
            root = Path(temporary)
            scripts = root / "scripts"
            scripts.mkdir()
            signer = (ROOT / "scripts/ci-sign-artifacts.sh").read_text()
            if omission == "root":
                needle = "python3 scripts/check-signed-root-inventory.py --require-empty ."
                self.assertEqual(signer.count(needle), 1)
                signer = signer.replace(needle, ": # valid omitted output inventory admission")
            elif omission == "directory":
                needle = '[[ ! -e "$output_dir" && ! -L "$output_dir" ]]'
                self.assertEqual(signer.count(needle), 1)
                signer = signer.replace(needle, "true")
            (scripts / "ci-sign-artifacts.sh").write_text(signer)
            shutil.copyfile(ROOT / "scripts/check-signed-root-inventory.py",
                            scripts / "check-signed-root-inventory.py")
            # These doubles admit only inert fixture inputs; they do not verify build artifacts.
            (scripts / "check-build-artifact-inventory.py").write_text(
                "import pathlib,sys\n"
                "root=pathlib.Path(sys.argv[sys.argv.index('--copy-to')+1])/'_unsigned'\n"
                "for t in ('esp32','esp32s3','esp32c3','esp32c6'):\n"
                " for n in ('bootloader/bootloader.bin','partition_table/partition-table.bin',"
                "'tesla-key-esp32.bin','ota_data_initial.bin'):\n"
                "  p=root/t/n;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'fixture')\n")
            for name in ("check-partition-contract.py", "check-otadata-contract.py", "check-firmware-artifacts.py"):
                (scripts / name).write_text("# Inert input precheck double.\n")
            inputs = root / "input/_unsigned"
            inputs.mkdir(parents=True)
            independent = root / "independent"
            independent.mkdir()
            tools = root / "bin"
            tools.mkdir()
            events = root / "key-tool-events"
            spy = tools / "espsecure.py"
            spy.write_text(f"#!{sys.executable}\nfrom pathlib import Path\n"
                           f"with Path({str(events)!r}).open('a') as f:f.write('key-tool-called\\n')\n"
                           "raise SystemExit(37)\n")
            spy.chmod(0o700)
            external = root / "untouched-sentinel"
            external.write_bytes(b"must remain unchanged")
            path = root / collision if collision is not None else None
            before = None
            if path is not None:
                if kind == "directory":
                    path.mkdir()
                elif kind == "symlink":
                    path.symlink_to(external)
                elif kind == "broken-symlink":
                    path.symlink_to(root / "absent-sentinel")
                else:
                    path.write_bytes(b"old caller-owned output")
                before = (path.lstat().st_mode, os.readlink(path) if path.is_symlink()
                          else hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else "directory")
            reserved_before = {name: os.path.lexists(root / name) for name in ("_fw", "_signed")}
            environment = dict(os.environ)
            environment.update(PATH=str(tools) + os.pathsep + environment["PATH"],
                               OTA_SIGNING_KEY_FILE=str(self.key), RUNNER_TEMP=str(root))
            result = subprocess.run(["bash", str(scripts / "ci-sign-artifacts.sh"), "1.2.3",
                                     str(inputs), "a" * 40, "--workflow-attested", str(independent)],
                                    cwd=root, env=environment, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            reached_key_tool = events.exists()
            if reached_key_tool:
                self.assertEqual(events.read_text(), "key-tool-called\n")
                self.assertIn(b"signing esp32 failed", result.stderr)
            self.assertEqual(external.read_bytes(), b"must remain unchanged")
            if path is not None:
                after = (path.lstat().st_mode, os.readlink(path) if path.is_symlink()
                         else hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else "directory")
                self.assertEqual(after, before)
            if collision in ("_fw", "_signed"):
                self.assertEqual({name: os.path.lexists(root / name) for name in reserved_before},
                                 reserved_before, "output namespace changed before signing")
            return reached_key_tool

    def test_fresh_output_reaches_refusing_sentinel(self) -> None:
        self.assertTrue(self.exercise(None))

    def test_existing_outputs_refuse_before_key_tool(self) -> None:
        names = ("tesla-key-esp32.bin", "tesla-key-esp32-s3.bin", "tesla-key-esp32-c3.bin",
                 "tesla-key-esp32-c6.bin", "tesla-key-esp32-1.2.3.bin",
                 "tesla-key-esp32-s3-1.2.3-merged.bin", "tesla-key-esp32-0.9.0.bin")
        for name in names:
            for kind in ("file", "symlink", "broken-symlink"):
                with self.subTest(name=name, kind=kind):
                    self.assertFalse(self.exercise(name, kind))
        for name in ("_fw", "_signed"):
            for kind in ("file", "directory", "symlink", "broken-symlink"):
                with self.subTest(name=name, kind=kind):
                    self.assertFalse(self.exercise(name, kind))

    def test_effective_root_guard_omission(self) -> None:
        self.assertTrue(self.exercise("tesla-key-esp32-0.9.0.bin", omission="root"))
        self.assertFalse(self.exercise("tesla-key-esp32-0.9.0.bin"))

    def test_effective_directory_guard_omission(self) -> None:
        # Exclusive mkdir still stops key use, but without the read-only guard it creates
        # the other reserved directory. Detect that real premature output mutation.
        with self.assertRaisesRegex(AssertionError, "output namespace changed before signing"):
            self.exercise("_fw", "broken-symlink", omission="directory")
        self.assertFalse(self.exercise("_fw", "broken-symlink"))


if __name__ == "__main__":
    os.umask(0o077)
    unittest.main()
