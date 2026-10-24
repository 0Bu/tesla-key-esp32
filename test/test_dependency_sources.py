#!/usr/bin/env python3
"""Exercise the real dependency reader with owned local Git trees; no network or compiler."""
from pathlib import Path
from unittest.mock import patch
import hashlib
import importlib.util
import json
import os
import subprocess
import shutil
import sys
import tempfile
import unittest
import ctypes

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("dependency_sources_under_test", ROOT / "scripts/dependency_source.py")
SOURCE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SOURCE)


class ParentBoundaryTests(unittest.TestCase):
    """Actual boundary function with controlled kernel/identity errors, never real signals."""
    def setUp(self):
        spec = importlib.util.spec_from_file_location("fresh_parent_boundary",
                                                     ROOT / "scripts/dependency_source.py")
        self.module = importlib.util.module_from_spec(spec)
        # Import must neither load ctypes nor change kernel signal policy.
        with patch.dict(sys.modules, {"ctypes": None}):
            spec.loader.exec_module(self.module)
        self.module.PROCESS_ID, self.module.PROCESS_PARENT = 100, 200
        self.calls = []
        self.configured = 0
        self.failed_option = None

        def prctl(option, value, *_unused):
            self.calls.append(option)
            if self.failed_option == option:
                return -1
            if option == 1:
                self.configured = value
            elif option == 2:
                ctypes.c_int.from_address(value).value = self.configured
            return 0
        self.prctl = prctl
        self.libc = type("InertLibc", (), {})()
        self.libc.prctl = self.prctl
        for target, value in (("getpid", lambda: 100), ("getppid", lambda: 200)):
            control = patch.object(self.module.os, target, value)
            control.start()
            self.addCleanup(control.stop)
        control = patch.object(self.module.sys, "platform", "linux")
        control.start()
        self.addCleanup(control.stop)
        control = patch.object(ctypes, "CDLL", return_value=self.libc)
        control.start()
        self.addCleanup(control.stop)

    def test_arm_once_read_back_and_never_rearm_changed_policy(self):
        m = self.module
        m.parent_lifetime_boundary()
        m.parent_lifetime_boundary()
        self.assertEqual(self.calls, [1, 2, 2])
        self.configured = 0
        with self.assertRaisesRegex(m.SourceError, "guard differs"):
            m.parent_lifetime_boundary()
        self.assertEqual(self.calls, [1, 2, 2, 2])

    def test_identity_changes_and_inherited_fork_state_refuse_without_rearming(self):
        m = self.module
        for armed in (False, True):
            m._parent_death_armed = armed
            for target in ("getpid", "getppid"):
                with self.subTest(armed=armed, target=target), patch.object(m.os, target, return_value=999):
                    with self.assertRaisesRegex(m.SourceError, "owner changed"):
                        m.parent_lifetime_boundary()
        self.assertEqual(self.calls, [])
        with patch.object(m.os, "getppid", side_effect=[200, 201]):
            m._parent_death_armed = False
            with self.assertRaisesRegex(m.SourceError, "while arming"):
                m.parent_lifetime_boundary()
        self.assertFalse(m._parent_death_armed)

    def test_setup_and_readback_failure_are_closed(self):
        m = self.module
        for failure in (1, 2):
            self.failed_option = failure
            with self.subTest(failure=failure), self.assertRaises(m.SourceError):
                m.parent_lifetime_boundary()
            self.assertFalse(m._parent_death_armed)
        for error in (OSError, AttributeError, TypeError, ValueError):
            with patch.object(ctypes, "CDLL", side_effect=error), self.assertRaisesRegex(m.SourceError, "unavailable"):
                m.parent_lifetime_boundary()
        with patch.dict(sys.modules, {"ctypes": None}), self.assertRaisesRegex(m.SourceError, "unavailable"):
            m.parent_lifetime_boundary()

    def test_cli_init_adoption_refuses_but_direct_init_library_owner_is_valid(self):
        m = self.module
        m.PROCESS_PARENT = 1
        with patch.object(m.os, "getppid", return_value=1):
            with self.assertRaisesRegex(m.SourceError, "no live consumer"):
                m.parent_lifetime_boundary(cli=True)
            self.assertEqual(self.calls, [])
            m.parent_lifetime_boundary()
            self.assertEqual(self.calls, [1, 2])

    def test_every_effectful_entry_refuses_before_any_filesystem_change(self):
        m = self.module
        with tempfile.TemporaryDirectory(prefix="dependency-owner-refusal-") as temporary:
            root = Path(temporary)
            def refuse():
                raise m.SourceError("inert owner refusal")
            with patch.object(m, "parent_lifetime_boundary", side_effect=refuse):
                entries = [lambda: m.git_query(root, [], {}),
                           lambda: m.admit_source(root / "source", "nanopb", {}),
                           lambda: m.admit_tesla_workspace(root, root / "source", root / "work"),
                           lambda: m.object_directory(root, "nanopb", {}, "salt")]
                for entry in entries:
                    with self.assertRaisesRegex(m.SourceError, "inert owner refusal"):
                        entry()
                with self.assertRaisesRegex(m.SourceError, "inert owner refusal"):
                    with m.object_context(root, "0" * 40):
                        self.fail("refused context yielded")
            self.assertEqual(list(root.iterdir()), [])


class SourceAdmissionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="dependency-source-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.origin = self.root / "origin"
        self.origin.mkdir()
        self.git("init", "--template=", "--object-format=sha1", "-q")
        for name in SOURCE.NANOPB_FILES:
            (self.origin / name).write_bytes(("inert fixture " + name + "\n").encode())
        (self.origin / "VERSION").write_text("0.4.9.1\n")
        (self.origin / ".gitignore").write_text("ignored.tmp\n")
        (self.origin / "tools").mkdir()
        (self.origin / "tools/inert").write_text("inert fixture\n")
        (self.origin / "tools/inert").chmod(0o700)
        self.git("add", ".")
        self.git("commit", "-qm", "Create owned inert fixture")
        self.commit = self.git("rev-parse", "HEAD")
        self.record = {"schema": 1, "version": "0.4.9.1", "commit": self.commit,
                       "repository": "https://github.com/nanopb/nanopb.git",
                       "files": {name: hashlib.sha256((self.origin / name).read_bytes()).hexdigest()
                                 for name in SOURCE.NANOPB_FILES}}

    def git(self, *arguments):
        result = subprocess.run(["git", "-c", "core.hooksPath=" + os.devnull,
            "-c", "core.fsmonitor=false", "-c", "commit.gpgsign=false",
            "-c", "maintenance.auto=false", "-c", "gc.auto=0", "-c", "user.name=Fixture",
            "-c", "user.email=fixture@example.invalid", "-C", str(self.origin), *arguments],
            env=SOURCE.git_environment(), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=10, check=True)
        return result.stdout.decode().strip()

    def prepare_pristine(self):
        (self.origin / ".git/HEAD").write_text(self.commit + "\n")
        (self.origin / ".git/config").write_bytes(SOURCE.SOURCE_CONFIG)
        return SOURCE.pristine_inventory(self.origin, self.commit)

    def test_raw_tree_and_restored_mtime_refusals(self):
        baseline = self.prepare_pristine()
        leaf = self.origin / "pb.h"
        body, mode, info = leaf.read_bytes(), leaf.stat().st_mode, leaf.stat()
        outside = self.root / "outside"
        outside.write_bytes(body)
        cases = [
            ("same-size-and-mtime", lambda: (leaf.write_bytes(b"X" + body[1:]),
                 os.utime(leaf, ns=(info.st_atime_ns, info.st_mtime_ns))),
                 lambda: leaf.write_bytes(body)),
            ("ignored-extra", lambda: (self.origin / "ignored.tmp").write_text("extra"),
                 lambda: (self.origin / "ignored.tmp").unlink()),
            ("extra-directory", lambda: (self.origin / "extra").mkdir(),
                 lambda: (self.origin / "extra").rmdir()),
            ("execute-mode", lambda: leaf.chmod(0o700), lambda: leaf.chmod(mode)),
            ("symbolic-alias", lambda: (leaf.unlink(), leaf.symlink_to(outside)),
                 lambda: (leaf.unlink(), leaf.write_bytes(body))),
            ("hardlink-alias", lambda: (leaf.unlink(), os.link(outside, leaf)),
                 lambda: (leaf.unlink(), leaf.write_bytes(body))),
            ("wrong-head", lambda: (self.origin / ".git/HEAD").write_text("0" * 40 + "\n"),
                 lambda: (self.origin / ".git/HEAD").write_text(self.commit + "\n")),
        ]
        for name, change, restore in cases:
            with self.subTest(name=name):
                change()
                try:
                    with self.assertRaises((SOURCE.SourceError, OSError)):
                        SOURCE.pristine_inventory(self.origin, self.commit)
                finally:
                    restore()
                self.assertEqual(SOURCE.pristine_inventory(self.origin, self.commit), baseline)
        self.assertEqual(outside.read_bytes(), body)

    def test_untrusted_config_and_parent_git_never_execute(self):
        self.prepare_pristine()
        config = self.origin / ".git/config"
        config.write_bytes(config.read_bytes() + b"[include]\n path = /forbidden-fixture-config\n")
        with patch.object(SOURCE.subprocess, "Popen", side_effect=AssertionError("untrusted Git executed")):
            with self.assertRaises(SOURCE.SourceError):
                SOURCE.pristine_inventory(self.origin, self.commit)
            child = self.origin / "nested"
            child.mkdir()
            with self.assertRaises(OSError):
                SOURCE.pristine_inventory(child, self.commit)

    def test_environment_and_moved_unselected_tag(self):
        baseline = self.prepare_pristine()
        with patch.dict(os.environ, {"GIT_DIR": "/forbidden", "GIT_OBJECT_DIRECTORY": "/forbidden",
                "GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "core.fsmonitor",
                "GIT_CONFIG_VALUE_0": "forbidden"}):
            self.assertEqual(SOURCE.pristine_inventory(self.origin, self.commit), baseline)
        tags = self.origin / ".git/refs/tags"
        tags.mkdir(exist_ok=True)
        (tags / "moved").write_text("f" * 40 + "\n")
        self.assertEqual(SOURCE.pristine_inventory(self.origin, self.commit), baseline)

    def test_record_schema_and_exact_seven_runtime_files(self):
        actual = SOURCE.read_nanopb_record(ROOT / "scripts/nanopb-source.json")
        self.assertEqual(set(actual["files"]), set(SOURCE.NANOPB_FILES))
        record = self.root / "record.json"
        cases = [b'{"schema":1,"schema":1}',
                 json.dumps({**actual, "files": {}}).encode(),
                 json.dumps({**actual, "extra": True}).encode(),
                 json.dumps({**actual, "commit": actual["commit"] + "\n"}).encode(),
                 json.dumps({**actual, "files": {**actual["files"], "pb.h": "x" * 64}}).encode()]
        for raw in cases:
            with self.subTest(raw=raw[:48]):
                record.write_bytes(raw)
                with self.assertRaises(SOURCE.SourceError):
                    SOURCE.read_nanopb_record(record)

    def admit_local(self, destination):
        actual_query = SOURCE.git_query
        fetches = []
        def query(directory, arguments, environment, maximum=2 * 1024 * 1024):
            if arguments[0] == "fetch":
                self.assertEqual(arguments, ["fetch", "--quiet", "--no-tags", "--depth=1",
                                             self.record["repository"], self.commit])
                fetches.append(True)
                arguments = [*arguments[:-2], self.origin.as_uri(), self.commit]
            return actual_query(directory, arguments, environment, maximum)
        with patch.object(SOURCE, "git_query", query):
            inventory = SOURCE.admit_source(destination, "nanopb", self.record)
        self.assertEqual(len(fetches), 1)
        return inventory

    def test_actual_cold_warm_existing_only_and_no_repair(self):
        destination = self.root / "admitted"
        expected = self.admit_local(destination)
        self.assertTrue((destination / "tools/inert").stat().st_mode & 0o111)
        with patch.object(SOURCE, "populate_cold", side_effect=AssertionError("cold/repair effect")):
            self.assertEqual(SOURCE.admit_source(destination, "nanopb", self.record,
                                                existing_only=True), expected)
            leaf = destination / "pb.h"
            body, info = leaf.read_bytes(), leaf.stat()
            leaf.write_bytes(b"X" + body[1:])
            os.utime(leaf, ns=(info.st_atime_ns, info.st_mtime_ns))
            before = leaf.read_bytes()
            with self.assertRaises(SOURCE.SourceError):
                SOURCE.admit_source(destination, "nanopb", self.record, existing_only=True)
            self.assertEqual(leaf.read_bytes(), before)
            leaf.write_bytes(body)
            owner = SOURCE.owner_path(destination)
            owner_bytes = owner.read_bytes()
            owner.unlink()
            with self.assertRaises(SOURCE.SourceError):
                SOURCE.admit_source(destination, "nanopb", self.record, existing_only=True)
            self.assertFalse(owner.exists())
            owner.write_bytes(owner_bytes)
            absent = self.root / "absent"
            with self.assertRaises(SOURCE.SourceError):
                SOURCE.admit_source(absent, "nanopb", self.record, existing_only=True)
            self.assertFalse(absent.exists())
            self.assertFalse(SOURCE.owner_path(absent).exists())
            foreign = self.root / "foreign"
            foreign.mkdir()
            sentinel = foreign / "sentinel"
            sentinel.write_bytes(b"unchanged")
            with self.assertRaises(SOURCE.SourceError):
                SOURCE.admit_source(foreign, "nanopb", self.record)
            self.assertEqual(sentinel.read_bytes(), b"unchanged")

    def test_failed_population_and_orphan_are_never_retried(self):
        destination = self.root / "failed"
        with patch.object(SOURCE, "populate_cold", side_effect=SOURCE.SourceError("inert failure")) as cold:
            with self.assertRaises(SOURCE.SourceError):
                SOURCE.admit_source(destination, "nanopb", self.record)
            self.assertEqual(cold.call_count, 1)
        owner = SOURCE.owner_path(destination)
        before = owner.read_bytes()
        with patch.object(SOURCE, "populate_cold", side_effect=AssertionError("retry effect")):
            for existing_only in (False, True):
                with self.assertRaises(SOURCE.SourceError):
                    SOURCE.admit_source(destination, "nanopb", self.record, existing_only=existing_only)
            orphan = self.root / "orphan"
            SOURCE.owner_path(orphan).write_bytes(b"{}\n")
            with self.assertRaises(SOURCE.SourceError):
                SOURCE.admit_source(orphan, "nanopb", self.record)
            self.assertFalse(orphan.exists())
            self.assertEqual(SOURCE.owner_path(orphan).read_bytes(), b"{}\n")
        self.assertEqual(owner.read_bytes(), before)

    def test_literal_links_are_confined_to_direct_tracked_regular_targets(self):
        tree = {"spm_headers/nanopb/pb.h": ("1" * 40, "120000"),
                "pb.h": ("2" * 40, "100644"), "other-link": ("3" * 40, "120000")}
        self.assertEqual(SOURCE.validate_link("spm_headers/nanopb/pb.h", b"../../pb.h", tree), "../../pb.h")
        for raw in (b"/pb.h", b"../../../escape", b"../../.git/HEAD", b"../../other-link",
                    b"../../missing", b"../../dir", b"..\\..\\pb.h", b"../../pb.h\0"):
            with self.subTest(raw=raw), self.assertRaises(SOURCE.SourceError):
                SOURCE.validate_link("spm_headers/nanopb/pb.h", raw, tree)

    def test_trusted_git_disables_actual_automatic_maintenance(self):
        actual_git = shutil.which("git")
        self.assertIsNotNone(actual_git)
        tools = self.root / "tools"
        tools.mkdir()
        wrapper = tools / "git"
        # The owned proxy only adds a local Trace2 observer and, for one counterfactual,
        # removes the complete two-setting maintenance policy. All Git work stays local.
        wrapper.write_text("#!" + sys.executable + "\nimport os,sys\nargs=sys.argv[1:]\n"
            "if os.environ.get('FIXTURE_OMIT_MAINTENANCE')=='1':\n"
            " for index in range(len(args)-2,-1,-1):\n"
            "  if args[index]=='-c' and args[index+1] in ('maintenance.auto=false','gc.auto=0'):\n"
            "   del args[index:index+2]\n"
            "args=['-c','maintenance.autoDetach=false','-c','gc.autoDetach=false',*args]\n"
            "os.environ['GIT_TRACE2_EVENT']=os.environ['FIXTURE_TRACE']\n"
            "os.execv(" + repr(actual_git) + ",[" + repr(actual_git) + ",*args])\n")
        wrapper.chmod(0o700)
        counts = []
        for omitted in (False, True):
            bare = self.root / ("omitted-git" if omitted else "guarded-git")
            subprocess.run([actual_git, "init", "--bare", "--template=", "-q", str(bare)],
                           env=SOURCE.git_environment(), check=True, capture_output=True, timeout=10)
            trace = self.root / ("omitted-trace.jsonl" if omitted else "guarded-trace.jsonl")
            env = SOURCE.git_environment()
            env.update(PATH=str(tools) + os.pathsep + os.environ["PATH"],
                       FIXTURE_TRACE=str(trace), FIXTURE_OMIT_MAINTENANCE="1" if omitted else "0")
            SOURCE.git_query(bare, ["fetch", "--quiet", "--no-tags", "--depth=1",
                                   self.origin.as_uri(), self.commit], env)
            events = [json.loads(line) for line in trace.read_text().splitlines()]
            children = [event for event in events if event.get("event") == "child_start"
                        and any(value in ("maintenance", "gc") for value in event.get("argv", []))]
            counts.append(len(children))
        self.assertEqual(counts[0], 0, "guarded fetch started maintenance")
        self.assertGreater(counts[1], 0, "maintenance-policy omission did not reach an actual maintenance child")

    def test_successful_flow_cache_is_bound_to_full_source_and_never_caches_errors(self):
        spec = importlib.util.spec_from_file_location("actual_host_flow_contract",
                                                     ROOT / "scripts/check-host-gate-contract.py")
        gate = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gate)
        source = (ROOT / "scripts/run-mock-tests.sh").read_text()
        actual_run = subprocess.run
        effects = []
        def observed_run(*arguments, **options):
            effects.append(True)
            return actual_run(*arguments, **options)
        gate.probe_browser_invocations.cache_clear()
        self.assertFalse(hasattr(gate.validate, "cache_info"))
        try:
            with patch.object(gate.subprocess, "run", observed_run):
                gate.probe_browser_invocations(source)
                first = len(effects)
                self.assertGreater(first, 0)
                gate.probe_browser_invocations(source)
                self.assertEqual(len(effects), first)
                gate.probe_browser_invocations(source + "\n# changed complete source control\n")
                self.assertGreater(len(effects), first)
                call = 'run_gate "exact dependency source admission" 120 python3 test/test_dependency_sources.py'
                self.assertEqual(source.count(call), 1)
                for replacement in ("# " + call, "if false; then\n" + call + "\nfi", "exit 0\n" + call):
                    changed = source.replace(call, replacement, 1)
                    for repeat in range(2):
                        with self.subTest(replacement=replacement, repeat=repeat):
                            before = len(effects)
                            with self.assertRaises(gate.ContractError):
                                gate.probe_browser_invocations(changed)
                            self.assertGreater(len(effects), before)
                before = len(effects)
                gate.probe_browser_invocations(source)
                self.assertEqual(len(effects), before)
        finally:
            gate.probe_browser_invocations.cache_clear()

    def test_owned_object_namespace_binds_content_and_salt(self):
        objects = self.root / "objects"
        objects.mkdir()
        proof = {"source": "/inert-fixture", "inventorySha256": "a" * 64, "identitySha256": "b" * 64}
        first = SOURCE.object_directory(objects, "nanopb", proof, "compiler-A")
        self.assertEqual(SOURCE.object_directory(objects, "nanopb", proof, "compiler-A"), first)
        self.assertNotEqual(SOURCE.object_directory(objects, "nanopb", proof, "compiler-B"), first)
        self.assertNotEqual(SOURCE.object_directory(objects, "nanopb",
            {**proof, "inventorySha256": "c" * 64}, "compiler-A"), first)
        outside = self.root / "outside-object"
        outside.write_bytes(b"inert")
        (first / "source.o").symlink_to(outside)
        with self.assertRaises(SOURCE.SourceError):
            SOURCE.object_directory(objects, "nanopb", proof, "compiler-A")
        self.assertEqual(outside.read_bytes(), b"inert")


if __name__ == "__main__":
    unittest.main()
