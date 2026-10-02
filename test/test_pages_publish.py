#!/usr/bin/env python3
"""Run the actual publisher against local bare Git repositories and fault shims."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
PUBLISHER = REPO / "scripts/publish-pages-branch.sh"


class PagesPublishTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pages-publish-contract-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.remote = self.root / "remote.git"
        self.seed = self.root / "seed"
        self.site = self.root / "site"
        self.bin = self.root / "bin"
        self.calls = self.root / "calls"
        for path in [self.site, self.bin, self.root / "work"]:
            path.mkdir()
        (self.site / "index.html").write_text("new preview\n")
        self.calls.write_text("")
        self.git = shutil.which("git")
        # Git exports repository context to hooks. None may escape into a fixture.
        inherited = {key: value for key, value in os.environ.items()
                     if not key.startswith("GIT_")}
        self.env = dict(inherited, GIT_CONFIG_GLOBAL=os.devnull,
                        GIT_CONFIG_NOSYSTEM="1", GITHUB_TOKEN="offline-dummy",
                        GH_TOKEN="offline-dummy", GITHUB_REPOSITORY="owner/repo",
                        TEST_REMOTE=str(self.remote), TEST_CALLS=str(self.calls),
                        TEST_REAL_GIT=self.git, TEST_FAULT="",
                        TMPDIR=str(self.root / "work"))
        for name in ["rm", "mkdir", "mv"]:
            self.env["TEST_REAL_" + name.upper()] = shutil.which(name)
        self.git_run("init", "--bare", "--quiet", str(self.remote))
        self.git_run("init", "--quiet", str(self.seed))
        self.git_run("-C", str(self.seed), "symbolic-ref", "HEAD", "refs/heads/main")
        self.git_run("-C", str(self.seed), "config", "user.name", "github-actions[bot]")
        self.git_run("-C", str(self.seed), "config", "user.email",
                     "41898282+github-actions[bot]@users.noreply.github.com")
        (self.seed / "SOURCE_ONLY").write_text("must not enter Pages\n")
        self.seed_commit("main fixture")
        self.git_run("-C", str(self.seed), "push", "--quiet", str(self.remote), "HEAD:main")
        self.git_run("--git-dir", str(self.remote), "symbolic-ref", "HEAD", "refs/heads/main")
        self.write_shim("git", """
import os, sys
args = sys.argv[1:]
cmd = args[2] if args[:1] == ["-C"] else args[0]
with open(os.environ["TEST_CALLS"], "a") as log:
    log.write("git " + cmd + "\\n")
if cmd in ["clone", "push"] and os.environ["TEST_REMOTE"] not in args:
    raise SystemExit("refusing non-fixture Git remote")
fault = os.environ.get("TEST_FAULT", "")
if (fault, cmd) in [("clone", "clone"), ("checkout", "checkout"),
                    ("git_rm", "rm"), ("ls_files", "ls-files")]:
    sys.exit(1)
if fault == "diff" and cmd == "diff":
    sys.exit(2)
os.execv(os.environ["TEST_REAL_GIT"], [os.environ["TEST_REAL_GIT"]] + args)
""")
        for name in ["rm", "mkdir", "mv"]:
            self.write_shim(name, """
import os, sys
from pathlib import Path
cmd = Path(sys.argv[0]).name
args = sys.argv[1:]
fault = os.environ.get("TEST_FAULT", "")
preview = any(arg.endswith("/PR/42") for arg in args)
stage = any(arg.endswith("/PR/42.tmp") for arg in args)
if cmd == "rm" and preview and fault == "preview_remove_noop":
    sys.exit(0)
if ((cmd == "rm" and preview and fault == "preview_remove") or
    (cmd == "rm" and stage and fault == "stage_remove") or
    (cmd == "mkdir" and stage and fault == "stage_mkdir") or
    (cmd == "mkdir" and any(a.endswith("/dev") for a in args) and fault == "dev_mkdir") or
    (cmd == "mv" and stage and fault == "stage_move")):
    sys.exit(1)
real = os.environ["TEST_REAL_" + cmd.upper()]
os.execv(real, [real] + args)
""")
        self.write_shim("gh", """
import os, sys
assert sys.argv[1:] == ["api", "--method", "POST", "repos/owner/repo/pages/builds"]
with open(os.environ["TEST_CALLS"], "a") as log:
    log.write("pages POST\\n")
""")
        self.write_shim("rsync", """
import shutil, sys
from pathlib import Path
args = sys.argv[1:]
src, dst = Path(args[-2]), Path(args[-1])
excluded = {a[len("--exclude="):].rstrip("/") for a in args if a.startswith("--exclude=")}
dst.mkdir(parents=True, exist_ok=True)
for child in dst.iterdir():
    if child.name not in excluded:
        if child.is_dir(): shutil.rmtree(child)
        else: child.unlink()
for child in src.iterdir():
    if child.name not in excluded:
        if child.is_dir(): shutil.copytree(child, dst / child.name)
        else: shutil.copy2(child, dst / child.name)
""")
        self.write_shim("sleep", "")
        self.env["PATH"] = str(self.bin) + os.pathsep + os.environ["PATH"]

    def write_shim(self, name, code):
        path = self.bin / name
        path.write_text("#!" + sys.executable + "\n" + code)
        path.chmod(0o700)

    def git_run(self, *args, check=True):
        return subprocess.run([self.git, *args], env=self.env, check=check,
                              capture_output=True, text=True)

    def seed_commit(self, message):
        self.git_run("-C", str(self.seed), "add", "-A")
        self.git_run("-C", str(self.seed), "commit", "--quiet", "-m", message)

    def existing_pages(self):
        self.git_run("-C", str(self.seed), "checkout", "--quiet", "--orphan", "gh-pages")
        self.git_run("-C", str(self.seed), "rm", "-rfq", ".")
        for name, body in [("index.html", "old root\n"),
                           ("PR/42/index.html", "old preview\n"),
                           ("dev/index.html", "other channel\n")]:
            path = self.seed / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(body)
        self.seed_commit("Pages fixture")
        self.git_run("-C", str(self.seed), "push", "--quiet", str(self.remote), "HEAD:gh-pages")

    def pages_ref(self):
        result = self.git_run("--git-dir", str(self.remote), "rev-parse", "--verify",
                              "refs/heads/gh-pages", check=False)
        return result.stdout.strip() if result.returncode == 0 else None

    def publish(self, mode="pr", fault=""):
        args = [str(PUBLISHER), mode]
        if mode != "rm":
            args.append(str(self.site))
        if mode in ["pr", "rm"]:
            args.append("42")
        return subprocess.run(args, env=dict(self.env, TEST_FAULT=fault),
                              capture_output=True, text=True, timeout=30)

    def assert_failure_before_publish(self, mode, fault):
        before = self.pages_ref()
        self.calls.write_text("")
        result = self.publish(mode, fault)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.pages_ref(), before)
        self.assertNotIn("git push", self.calls.read_text())
        self.assertNotIn("pages POST", self.calls.read_text())

    def test_preview_filesystem_failures(self):
        self.existing_pages()
        for fault in ["preview_remove", "preview_remove_noop", "stage_remove",
                      "stage_mkdir", "stage_move"]:
            with self.subTest(fault=fault):
                self.assert_failure_before_publish("pr", fault)

    def test_dev_directory_failure(self):
        self.existing_pages()
        self.assert_failure_before_publish("dev", "dev_mkdir")

    def test_cleanup_removal_failure(self):
        self.existing_pages()
        self.assert_failure_before_publish("rm", "preview_remove")

    def test_unreadable_staged_diff(self):
        self.existing_pages()
        self.assert_failure_before_publish("pr", "diff")

    def test_failed_bootstrap_steps(self):
        for fault in ["clone", "checkout", "git_rm", "ls_files"]:
            with self.subTest(fault=fault):
                self.assert_failure_before_publish("pr", fault)

    def test_successful_preview_replacement(self):
        self.existing_pages()
        self.assertEqual(self.publish().returncode, 0)
        body = self.git_run("--git-dir", str(self.remote), "show",
                            "gh-pages:PR/42/index.html").stdout
        self.assertEqual(body, "new preview\n")
        paths = self.git_run("--git-dir", str(self.remote), "ls-tree", "-r", "--name-only", "gh-pages").stdout
        self.assertNotIn("42.tmp", paths)
        self.assertIn("dev/index.html", paths)

    def test_successful_first_preview_has_no_main_files(self):
        self.assertEqual(self.publish().returncode, 0)
        paths = self.git_run("--git-dir", str(self.remote), "ls-tree", "-r", "--name-only", "gh-pages").stdout
        self.assertEqual(paths, "PR/42/index.html\n")

    def test_successful_empty_remote(self):
        empty = self.root / "empty.git"
        self.git_run("init", "--bare", "--quiet", str(empty))
        self.remote = empty
        self.env["TEST_REMOTE"] = str(empty)
        self.assertEqual(self.publish("root").returncode, 0)
        paths = self.git_run("--git-dir", str(empty), "ls-tree", "-r", "--name-only", "gh-pages").stdout
        self.assertEqual(paths, "index.html\n")


if __name__ == "__main__":
    unittest.main()
