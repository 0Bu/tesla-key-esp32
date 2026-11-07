#!/usr/bin/env python3
"""Prove real blobless Git preserves offline exact-source bytes and release metadata."""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class Checkout(unittest.TestCase):
    def test_full_history_without_unrelated_pages_blobs(self):
        with tempfile.TemporaryDirectory(prefix="ci-checkout-") as temporary:
            root = Path(temporary); seed = root / "seed"; remote = root / "remote.git"
            environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
            environment.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
            def git(where, *args, check=True, offline=False):
                return subprocess.run(["git", "-c", "core.hooksPath=/dev/null",
                    "-c", "protocol.allow=" + ("never" if offline else "always"),
                    "-C", str(where), *args], env=environment, capture_output=True, check=check)
            seed.mkdir(); git(seed, "init", "-q"); git(seed, "checkout", "-qb", "main")
            git(seed, "config", "user.name", "Fixture"); git(seed, "config", "user.email", "fixture@example.invalid")
            git(seed, "config", "commit.gpgsign", "false"); git(seed, "config", "tag.gpgsign", "false")
            (seed / "source.txt").write_bytes(b"old source")
            git(seed, "add", "."); git(seed, "commit", "-qm", "base")
            git(seed, "tag", "-a", "v1.0.0", "-m", "stable")
            (seed / "source.txt").write_bytes(b"exact current source")
            git(seed, "add", "."); git(seed, "commit", "-qm", "candidate")
            source = git(seed, "rev-parse", "HEAD").stdout.strip().decode()
            expected_archive = git(seed, "archive", source).stdout
            git(seed, "checkout", "--orphan", "gh-pages"); git(seed, "rm", "-rfq", ".")
            (seed / "large.bin").write_bytes(hashlib.shake_256(b"unrelated-pages-fixture").digest(2 * 1024 * 1024))
            git(seed, "add", "."); git(seed, "commit", "-qm", "binary site")
            payload = git(seed, "rev-parse", "HEAD:large.bin").stdout.strip().decode()
            git(root, "init", "--bare", "-q", str(remote))
            git(remote, "config", "uploadpack.allowFilter", "true")
            git(remote, "config", "uploadpack.allowAnySHA1InWant", "true")
            git(seed, "push", "-q", str(remote), "--all"); git(seed, "push", "-q", str(remote), "--tags")
            sizes = []
            for filtered in (False, True):
                checkout = root / ("filtered" if filtered else "full"); checkout.mkdir()
                git(checkout, "init", "-q"); git(checkout, "remote", "add", "origin", remote.as_uri())
                git(checkout, "fetch", "-q", "--no-tags", *( ["--filter=blob:none"] if filtered else []),
                    "origin", "+refs/heads/*:refs/remotes/origin/*", "+refs/tags/*:refs/tags/*")
                git(checkout, "checkout", "--detach", "-q", source)
                self.assertEqual(git(checkout, "rev-parse", "--is-shallow-repository", offline=True).stdout.strip(), b"false")
                git(checkout, "merge-base", "--is-ancestor", "v1.0.0", "HEAD", offline=True)
                self.assertEqual(git(checkout, "log", "--format=%s", "v1.0.0..HEAD", offline=True).stdout.strip(), b"candidate")
                self.assertEqual(git(checkout, "diff", "--name-only", "--no-renames", "v1.0.0", "HEAD", offline=True).stdout.strip(), b"source.txt")
                self.assertEqual(git(checkout, "archive", "HEAD", offline=True).stdout, expected_archive)
                self.assertEqual((checkout / "source.txt").read_bytes(), b"exact current source")
                present = git(checkout, "cat-file", "-e", payload, check=False, offline=True).returncode == 0
                self.assertEqual(present, not filtered, "historical Pages payload must not be downloaded")
                sizes.append(sum(path.stat().st_size for path in (checkout / ".git/objects").rglob("*") if path.is_file()))
            self.assertLess(sizes[1], sizes[0] // 10)


if __name__ == "__main__":
    unittest.main()
