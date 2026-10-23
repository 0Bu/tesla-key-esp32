#!/usr/bin/env python3
"""Run the real four-target Pages assembler with both optional-ref argument shapes."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def check(shell: str) -> None:
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="pages-shell-contract-") as temporary:
        fixture = Path(temporary) / "repo"
        scripts = fixture / "scripts"
        scripts.mkdir(parents=True)
        for name in (
            "build-pages.sh", "verify-vendored-esptool-js.sh",
            "check-pages-manifest.py", "check-otadata-contract.py",
            "generate-ota-changelog.py",
        ):
            shutil.copy2(root / "scripts" / name, scripts / name)
        shutil.copytree(root / "docs", fixture / "docs")
        shutil.copy2(root / "LICENSE", fixture / "LICENSE")
        environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        # The disposable repository has no remote. These tests never need network access.
        environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)

        def git(*args: str) -> str:
            return subprocess.check_output(
                ["git", "-C", str(fixture), *args], env=environment, text=True,
                stderr=subprocess.PIPE,
            ).strip()

        git("init", "-q")
        git("-c", "user.name=Pages fixture", "-c", "user.email=pages@example.invalid",
            "-c", "commit.gpgsign=false", "commit", "--allow-empty", "-qm",
            "fix: Preserve changelog fallback")
        source_sha = git("rev-parse", "HEAD")
        stage = fixture / "firmware"
        for target in ("esp32", "esp32s3", "esp32c3", "esp32c6"):
            target_dir = stage / target
            target_dir.mkdir(parents=True)
            for name, size in (("bootloader.bin", 1024), ("partition-table.bin", 1024),
                               ("tesla-key-esp32.bin", 4096)):
                (target_dir / name).write_bytes(bytes(size))
            (target_dir / "ota_data_initial.bin").write_bytes(b"\xff" * 8192)
        environment.update(FIRMWARE_STAGE_DIR=str(stage), RUNNER_TEMP=temporary)
        for present in (False, True):
            if present:
                git("update-ref", "refs/remotes/origin/gh-pages", source_sha)
            output = Path(temporary) / "tesla-key-pages.shell-test"
            result = subprocess.run(
                [shell, str(scripts / "build-pages.sh"), str(output), "1.2.3", source_sha],
                cwd=fixture, env=environment, capture_output=True, text=True, timeout=60,
            )
            if result.returncode:
                raise AssertionError(
                    f"assembler failed with published_ref={present}: "
                    f"{result.stdout}\n{result.stderr}"
                )
            manifest = json.loads((output / "manifest.json").read_text())
            assert manifest["sourceSha"] == source_sha
            assert len(manifest["builds"]) == 4
            assert sum(len(build["parts"]) for build in manifest["builds"]) == 16
            assert json.loads((output / "changelog.json").read_text()) == {
                "version": "1.2.3", "changelog": "Preserve changelog fallback",
            }
            for relative, source in (
                ("LICENSE", root / "LICENSE"),
                ("THIRD_PARTY.md", root / "docs/THIRD_PARTY.md"),
                ("vendor/esptool-js-0.6.1.LICENSE", root / "docs/vendor/esptool-js-0.6.1.LICENSE"),
                ("vendor/pako-2.1.0.LICENSE", root / "docs/vendor/pako-2.1.0.LICENSE"),
                ("vendor/pako-2.1.0.zlib.README", root / "docs/vendor/pako-2.1.0.zlib.README"),
            ):
                assert (output / relative).read_bytes() == source.read_bytes(), relative
            footer = (output / "index.html").read_text().split('<footer class="installer-footer">', 1)[1].split("</footer>", 1)[0]
            assert 'href="./LICENSE">AGPL-3.0</a>' in footer
            assert 'href="./THIRD_PARTY.md"' in footer
            assert "MIT" not in footer
    print("pages-shell-contract: PASS (real assembler, four targets, absent/present ref, shipped licenses)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bash", default=shutil.which("bash"), required=False)
    arguments = parser.parse_args()
    if not arguments.bash:
        parser.error("bash is required")
    check(arguments.bash)
