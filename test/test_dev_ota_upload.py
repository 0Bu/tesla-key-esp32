#!/usr/bin/env python3
"""Offline source/reviewer/image/one-write controls for the actual local OTA helper.

Only disposable project-verifier RSA fixtures and passive transport/process doubles are used.
No board, vehicle, production signing key or publication is accessed.
"""
from __future__ import annotations

import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("dev_ota_upload", ROOT / "scripts/dev-ota-upload.py")
ota = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = ota
spec.loader.exec_module(ota)
artifact = ota.artifact_module(ROOT)


def unsigned_image(target="esp32s3", version="1.6.3"):
    value = bytearray(artifact.make_image(target, version, app=True))
    value[176:208] = hashlib.sha256(b"disposable fixture ELF").digest()
    size = struct.unpack_from("<I", value, 28)[0]
    checksum = artifact.CHECKSUM_SEED
    for byte in value[32:32 + size]:
        checksum ^= byte
    end = 32 + size + (15 - (32 + size) % 16)
    value[end] = checksum
    value[end + 1:] = hashlib.sha256(value[:end + 1]).digest()
    return bytes(value)


UNSIGNED = unsigned_image()
SIGNED = artifact.fake_sign(UNSIGNED)
sector = SIGNED[-artifact.SIGNATURE_SECTOR:]
PIN = hashlib.sha256(sector[artifact.RSA_N_OFFSET:artifact.RSA_PUBLIC_FIELDS_END]).hexdigest()


class LocalOtaTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tesla-local-ota-test-")
        self.base = Path(self.temporary.name).resolve()
        self.root = self.base / "repo"
        self.root.mkdir()
        (self.root / "scripts").mkdir()
        (self.root / ".agents").mkdir()
        (self.root / ".gitignore").write_text("__pycache__/\n*.pyc\n/build/\n")
        shutil.copy2(ROOT / "scripts/check-firmware-artifacts.py", self.root / "scripts/check-firmware-artifacts.py")
        shutil.copy2(ROOT / ".agents/subagents.json", self.root / ".agents/subagents.json")
        shutil.copy2(ROOT / "dependencies.lock.esp32s3", self.root / "dependencies.lock.esp32s3")
        (self.root / "scripts/ota-signing-public-key.sha256").write_text(PIN + "\n")
        (self.root / "logic.cpp").write_text("int sample = 1;\n")
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        subprocess.run(["git", "-C", str(self.root), "add", "."], check=True)
        self.inventory = self.base / "inventory.json"
        self.inventory.write_text(json.dumps({"boards": [{"id": "fixture-prod", "role": "production",
            "project": ota.PROJECT, "target": "esp32s3", "address": "192.0.2.17",
            "board_mac": "02:00:00:00:00:17"}]}))
        self.inventory.chmod(0o600)
        self.directory = self.base / "evidence"
        self.case = ota.Case.prepare(self.directory, self.inventory, "fixture-prod", "1.6.3", self.root)
        self.before_status = {"version": "1.6.3", "vin": "OFFLINE-FIXTURE", "key_present": True,
            "key_fingerprint": "fixture-key-id", "paired": True, "ota": {"channel": "dev"},
            "sys": {"board_mac": "02:00:00:00:00:17", "safe_mode": False,
                    "runtime_state": "ready", "largest_block": 65536, "uptime_s": 100}}
        self.before_ota = {"project": ota.PROJECT, "target": "esp32s3", "channel": "dev",
            "busy": False, "verification": "stable", "recovery_required_bytes": 59392,
            "boot_id": "0123456789abcdef", "elf_sha256": ota.image_identity(UNSIGNED)}

    def tearDown(self):
        self.temporary.cleanup()

    def rows(self, kind, commands):
        result = []
        for index, command in enumerate(commands):
            name = f"{kind}-{index}.log"
            ota.write_private(self.directory / name, b"PASS passive fixture\n")
            result.append({"argv": command, "exit": 0, "log": name,
                           "log_sha256": ota.digest((self.directory / name).read_bytes())})
        return result

    def build_fixture(self):
        """Seed bounded inert test-process evidence, never real deployment approval."""
        self.case.record("tests", commands=self.rows("tests", [["host"], ["lint"], ["contracts"]]))
        ota.write_private(self.directory / "unsigned.bin", UNSIGNED)
        gates = self.rows("build-gate", [[name] for name in ota.BUILD_GATES])
        for label, row in zip(ota.BUILD_GATES, gates):
            row["label"] = label
        self.case.record("build", commands=self.rows("build", [["pinned-wrapper", "idf.py"]]), gates=gates,
                         target="esp32s3", version="1.6.3", unsigned_sha256=ota.digest(UNSIGNED),
                         elf_sha256=ota.image_identity(UNSIGNED))
        image = self.base / "signed-app.bin"
        image.write_bytes(SIGNED)
        self.case.bind_image(image, ota.digest(SIGNED))

    def review_fixture(self, *, finding=None, outcome="ready", thread=True, change_source=False):
        report = {"outcome": outcome, "source_id": self.case.plan["source_id"],
            "image_sha256": ota.digest(SIGNED), "areas": list(ota.REVIEW_AREAS),
            "findings": [finding] if finding is not None else []}
        seen = []
        def passive_review(kind, commands, timeout):
            self.assertEqual(kind, "review")
            self.assertGreater(timeout, 0)
            seen.extend(commands)
            ota.write_private(self.directory / "review-result.json", ota.canonical(report))
            events = [{"type": "thread.started", "thread_id": "independent-fixture-reviewer"}] if thread else []
            log = b"\n".join(ota.canonical(row) for row in events) + b"\n"
            ota.write_private(self.directory / "review-0.log", log)
            if change_source:
                (self.root / "new.cpp").write_text("int unreviewed;\n")
            return [{"argv": commands[0], "exit": 0, "log": "review-0.log", "log_sha256": ota.digest(log)}]
        with patch.object(ota.shutil, "which", return_value="/fixture/codex"), \
             patch.object(self.case, "run_commands", side_effect=passive_review):
            self.case.review()
        self.assertEqual(len(seen), 1)
        return seen[0]

    def ready_fixture(self):
        self.build_fixture()
        self.review_fixture()
        return self.case.ready()

    def device_get(self, board, path, **kwargs):
        self.assertEqual(board, self.case.plan["board"])
        self.assertIn(path, ("/status", "/ota/status"))
        return copy.deepcopy(self.before_status if path == "/status" else self.before_ota)

    def test_snapshot_covers_new_and_deleted_files_without_commit(self):
        self.assertIsNone(self.case.load("source")["head"])
        (self.root / "new.cpp").write_text("int new_source;\n")
        with self.assertRaises(ota.Refused):
            self.case.unchanged()
        (self.root / "new.cpp").unlink()
        (self.root / "logic.cpp").unlink()
        with self.assertRaises(ota.Refused):
            self.case.unchanged()

    def test_snapshot_records_executable_mode(self):
        (self.root / "logic.cpp").chmod(0o755)
        with self.assertRaises(ota.Refused):
            self.case.unchanged()

    def test_snapshot_refuses_symlinks_and_untracked_secret_images(self):
        link = self.root / "untracked.cpp"
        link.symlink_to(self.base / "missing-external")
        with self.assertRaises(ota.Refused):
            ota.source_manifest(self.root)
        link.unlink()
        (self.root / "nvs.bin").write_bytes(b"private fixture must not be inspected")
        with self.assertRaises(ota.Refused):
            ota.source_manifest(self.root)

    def test_inventory_is_private_explicit_production_and_unique(self):
        self.inventory.chmod(0o644)
        with self.assertRaises(ota.Refused):
            ota.inventory(self.inventory, "fixture-prod", self.root)
        self.inventory.chmod(0o600)
        data = json.loads(self.inventory.read_text())
        data["boards"].append(data["boards"][0])
        self.inventory.write_text(json.dumps(data))
        with self.assertRaises(ota.Refused):
            ota.inventory(self.inventory, "fixture-prod", self.root)

    def test_tests_execute_fixed_project_entrypoints(self):
        seen = []
        def fake_commands(kind, commands, timeout):
            seen.extend(commands)
            return self.rows(kind, commands)
        with patch.object(self.case, "run_commands", side_effect=fake_commands):
            self.case.tests()
        self.assertEqual([Path(row[0]).name for row in seen],
                         ["run-mock-tests.sh", "repo-lint.sh", "test-build-contracts.sh"])
        self.assertEqual(seen[0][1:], ["--require-all"])

    def test_real_command_source_mutation_invalidates_evidence(self):
        command = [sys.executable, "-c", "from pathlib import Path; Path('logic.cpp').write_text('changed')"]
        with self.assertRaises(ota.Refused):
            self.case.run_commands("inert-source-mutation", [command], 10)

    def test_build_is_fresh_case_bound_with_own_sdkconfig(self):
        self.case.record("tests", commands=self.rows("tests", [["host"], ["lint"], ["contracts"]]))
        seen = []
        def build_effect(kind, commands, timeout):
            command = commands[0]
            seen.extend(commands)
            directory = self.root / command[command.index("--build-dir") + 1]
            self.assertIn(self.case.plan["case_id"], str(directory))
            self.assertEqual(command[-4:], ["--target", "esp32s3", "--version", "1.6.3"])
            self.assertIn("ci-build", command[3])  # Wrapper selects a fresh container, never a daemon.
            self.assertEqual(list(directory.iterdir()), [])
            with patch.object(ota.sys, "stdout", io.StringIO()):
                ota.container_build(directory.relative_to(self.root), "esp32s3", "1.6.3", root=self.root,
                                    runner=self.build_runner(), environment={"IDF_PATH": "/fixture-idf"})
            return self.rows(kind, commands)
        with patch.object(self.case, "run_commands", side_effect=build_effect):
            self.case.build()
            self.case.build()
        self.assertEqual(len(seen), 2)
        self.assertNotEqual(seen[0][seen[0].index("--build-dir") + 1], seen[1][seen[1].index("--build-dir") + 1])
        self.assertEqual(self.case.receipt("build")["unsigned_sha256"], ota.digest(UNSIGNED))

    def build_runner(self, failure=None, calls=None):
        calls = [] if calls is None else calls
        def run(command, **kwargs):
            label = ota.BUILD_GATES[len(calls)]
            calls.append((label, command))
            self.assertEqual(kwargs["env"]["IDF_CCACHE_ENABLE"], "0")
            self.assertEqual(kwargs["env"]["EXTRA_CFLAGS"], "-fstack-usage")
            self.assertEqual(kwargs["env"]["EXTRA_CXXFLAGS"], "-fstack-usage")
            if label == "configure":
                build = command[command.index("-B") + 1]
                self.assertIn(f"SDKCONFIG={build}/sdkconfig", command)
            if label == "compile":
                build = self.root / command[command.index("-B") + 1]
                (build / f"{ota.PROJECT}.bin").write_bytes(UNSIGNED)
            kwargs["stdout"].write(b'{}\n' if label == "idf_size" else b"PASS inert target gate\n")
            return subprocess.CompletedProcess(command, 37 if label == failure else 0)
        return run

    def test_each_executed_target_gate_failure_blocks_build_readiness(self):
        self.ready_fixture()
        for failure in ota.BUILD_GATES:
            with self.subTest(failure=failure):
                calls = []
                def rejected_build(kind, commands, timeout):
                    command = commands[0]
                    build = Path(command[command.index("--build-dir") + 1])
                    with patch.object(ota.sys, "stdout", io.StringIO()):
                        ota.container_build(build, "esp32s3", "1.6.3", root=self.root,
                                            runner=self.build_runner(failure, calls),
                                            environment={"IDF_PATH": "/fixture-idf", "IDF_CCACHE_ENABLE": "1"})
                    raise AssertionError("the rejected gate must stop before creating a build receipt")
                with patch.object(self.case, "run_commands", side_effect=rejected_build):
                    with self.assertRaises(ota.Refused):
                        self.case.build()
                self.assertEqual(calls[-1][0], failure)
                self.assertEqual([row[0] for row in calls], list(ota.BUILD_GATES[:ota.BUILD_GATES.index(failure) + 1]))
                self.assertFalse((self.directory / "build.json").exists())
                with self.assertRaises(OSError):
                    self.case.ready()

    def test_target_gate_plan_uses_actual_baselines_generated_outputs_and_budget_enforcement(self):
        build = Path("build") / f"ota-{'1' * 32}-{'2' * 16}-esp32s3"
        (self.root / build).mkdir(parents=True)
        calls = []
        with patch.object(ota.sys, "stdout", io.StringIO()):
            result = ota.container_build(build, "esp32s3", "1.6.3", root=self.root,
                                         runner=self.build_runner(calls=calls),
                                         environment={"IDF_PATH": "/fixture-idf", "IDF_CCACHE_ENABLE": "1"})
        commands = dict(calls)
        self.assertEqual(tuple(commands), ota.BUILD_GATES)
        self.assertIn("--enforce-budget", commands["resource_budget"])
        self.assertIn("scripts/firmware-size-baseline.json", commands["resource_budget"])
        self.assertIn("scripts/firmware-stack-baseline.json", commands["stack_budget"])
        self.assertIn(str(build / "sdkconfig"), commands["sdkconfig_defaults"])
        self.assertIn(str(build / "compile_commands.json"), commands["effective_build"])
        self.assertIn(str(build / "partition_table/partition-table.bin"), commands["partition_binary"])
        self.assertIn(str(build / "ota_data_initial.bin"), commands["otadata"])
        self.assertEqual(commands["idf_size"][1:5], ["-m", "esp_idf_size", "--format", "json2"])
        self.assertEqual(result["compiler_environment"]["IDF_CCACHE_ENABLE"], "0")

    def test_target_container_refuses_unbound_compiler_environment(self):
        build = Path("build") / f"ota-{'3' * 32}-{'4' * 16}-esp32s3"
        (self.root / build).mkdir(parents=True)
        for variable in ("SDKCONFIG_DEFAULTS", "CFLAGS", "IDF_COMPONENT_MANAGER", "TESLA_BLE_SOURCE"):
            with self.subTest(variable=variable):
                with self.assertRaises(ota.Refused):
                    ota.container_build(build, "esp32s3", "1.6.3", root=self.root,
                                        runner=lambda *args, **kwargs: self.fail("compiler override reached execution"),
                                        environment={"IDF_PATH": "/fixture-idf", variable: ""})

    def test_compiled_image_swap_during_target_validation_blocks_manifest(self):
        build = Path("build") / f"ota-{'5' * 32}-{'6' * 16}-esp32s3"
        (self.root / build).mkdir(parents=True)
        calls = []
        actual = self.build_runner(calls=calls)
        def changed_image(command, **kwargs):
            result = actual(command, **kwargs)
            if calls[-1][0] == "stack_budget":
                (self.root / build / f"{ota.PROJECT}.bin").write_bytes(unsigned_image(version="1.6.4"))
            return result
        with patch.object(ota.sys, "stdout", io.StringIO()):
            with self.assertRaises(ota.Refused):
                ota.container_build(build, "esp32s3", "1.6.3", root=self.root, runner=changed_image,
                                    environment={"IDF_PATH": "/fixture-idf"})
        self.assertFalse((self.root / build.with_name(build.name + "-checks") / "build-gates.json").exists())

    def test_validated_image_swap_before_host_freeze_blocks_build_receipt(self):
        self.case.record("tests", commands=self.rows("tests", [["host"], ["lint"], ["contracts"]]))
        def swapped_build(kind, commands, timeout):
            build = Path(commands[0][commands[0].index("--build-dir") + 1])
            with patch.object(ota.sys, "stdout", io.StringIO()):
                ota.container_build(build, "esp32s3", "1.6.3", root=self.root, runner=self.build_runner(),
                                    environment={"IDF_PATH": "/fixture-idf"})
            (self.root / build / f"{ota.PROJECT}.bin").write_bytes(unsigned_image(version="1.6.4"))
            return self.rows(kind, commands)
        with patch.object(self.case, "run_commands", side_effect=swapped_build):
            with self.assertRaises(ota.Refused):
                self.case.build()
        self.assertFalse((self.directory / "build.json").exists())

    def test_build_process_filters_credentials_and_disables_ccache(self):
        command = [sys.executable, "-c", "import os; print(os.environ.get('CCACHE_DISABLE')); print('OTA_SIGNING_KEY' in os.environ)"]
        with patch.dict(os.environ, {"OTA_SIGNING_KEY": "inert-fixture-not-a-key"}):
            rows = self.case.run_commands("build", [command], 10)
        self.assertEqual((self.directory / rows[0]["log"]).read_text(), "1\nFalse\n")

    def test_missing_or_manually_forged_review_is_refused(self):
        self.build_fixture()
        with self.assertRaises(OSError):
            self.case.ready()
        (self.directory / "review.json").write_text(json.dumps({"kind": "review", "payload": {
            "source_id": self.case.plan["source_id"], "sandbox": "read-only", "passed": True}}))
        with self.assertRaises(ota.Refused):
            self.case.ready()

    def test_actual_review_command_requires_read_only_and_new_execution_identity(self):
        self.build_fixture()
        command = self.review_fixture()
        self.assertIn("--ignore-user-config", command)
        self.assertEqual(command[command.index("--sandbox") + 1], "read-only")
        self.assertIn('approval_policy="never"', command)
        self.assertEqual(self.case.receipt("review")["reviewer_thread"], "independent-fixture-reviewer")

    def test_failed_missing_and_material_doc_reviews_are_refused(self):
        self.build_fixture()
        for finding in ({"severity": "P1", "blocking": True, "detail": "security fixture"},
                        {"severity": "P2", "blocking": False, "detail": "safety fixture"},
                        {"severity": "P3", "blocking": True, "detail": "material documentation fixture"}):
            with self.assertRaises(ota.Refused):
                self.review_fixture(finding=finding)
        with self.assertRaises(ota.Refused):
            self.review_fixture(thread=False)
        with self.assertRaises(ota.Refused):
            self.review_fixture(outcome="blocked")

    def test_edited_review_or_log_is_refused(self):
        self.ready_fixture()
        (self.directory / "review-0.log").write_text("manual replacement")
        with self.assertRaises(ota.Refused):
            self.case.ready()

    def test_failed_tests_after_prior_pass_with_identical_stdout_withdraw_all_authority(self):
        self.ready_fixture()
        previous = (self.directory / "tests-0.log").read_bytes()
        command = [sys.executable, "-c", "import sys; sys.stdout.write('PASS passive fixture\\n'); sys.exit(1)"]
        original = self.case.run_commands
        def failed(kind, commands, timeout):
            return original(kind, [command], timeout)
        with patch.object(self.case, "run_commands", side_effect=failed):
            with self.assertRaises(ota.Refused):
                self.case.tests()
        new_log = next(path for path in self.directory.glob("tests-*-0.log"))
        self.assertEqual(new_log.read_bytes(), previous)
        for name in ("tests", "build", "image", "review"):
            self.assertFalse((self.directory / f"{name}.json").exists())
        with self.assertRaises(OSError):
            self.case.ready()

    def test_failed_review_after_prior_pass_cannot_reuse_earlier_ready(self):
        self.ready_fixture()
        with self.assertRaises(ota.Refused):
            self.review_fixture(outcome="blocked")
        self.assertFalse((self.directory / "review.json").exists())
        with self.assertRaises(OSError):
            self.case.ready()

    def test_failed_bind_after_prior_pass_withdraws_image_and_review(self):
        self.ready_fixture()
        with self.assertRaises(ota.Refused):
            self.case.bind_image(self.base / "signed-app.bin", "0" * 64)
        self.assertFalse((self.directory / "image.json").exists())
        self.assertFalse((self.directory / "review.json").exists())

    def test_exact_signature_pin_chip_and_hash_are_required(self):
        self.build_fixture()
        image = self.base / "signed-app.bin"
        with self.assertRaises(ota.Refused):
            self.case.bind_image(image, "0" * 64)
        (self.root / "scripts/ota-signing-public-key.sha256").write_text("0" * 64 + "\n")
        with self.assertRaises(ota.Refused):  # Pin changes are source changes, not local authority.
            self.case.bind_image(image, ota.digest(SIGNED))
        verifier = ota.artifact_module(self.root)
        with self.assertRaises(verifier.ArtifactError):
            verifier.validate_app("esp32c3", "1.6.3", image, signed_app=True,
                                  expected_public_key_digest=bytes.fromhex(PIN))

    def test_actual_signature_verification_refuses_tampering_and_foreign_authority(self):
        bad = bytearray(SIGNED)
        bad[40] ^= 1
        with self.assertRaises(ValueError):
            self.case.validate_signed(bytes(bad))
        (self.root / "scripts/ota-signing-public-key.sha256").write_text("0" * 64 + "\n")
        with self.assertRaises(ValueError):
            self.case.validate_signed(SIGNED)

    def test_signed_image_swap_is_refused_before_network(self):
        self.ready_fixture()
        data = bytearray(SIGNED)
        data[40] ^= 1
        (self.directory / "signed.bin").write_bytes(data)
        with patch.object(ota, "exchange", side_effect=AssertionError("no network permitted")):
            with self.assertRaises(ota.Refused):
                ota.preflight(self.case)

    def test_wrong_board_stops_before_second_get(self):
        self.ready_fixture()
        self.before_status["sys"]["board_mac"] = "02:00:00:00:00:18"
        with patch.object(ota, "get_json", side_effect=self.device_get) as get:
            with self.assertRaises(ota.IdentityMismatch):
                ota.preflight(self.case)
        self.assertEqual(get.call_count, 1)

    def test_dev_stable_idle_runtime_and_internal_memory_are_mandatory(self):
        self.ready_fixture()
        for field, value in (("channel", "release"), ("configured_channel", "release"), ("busy", True), ("verification", "pending_verify"),
                             ("verification", "unknown"), ("recovery_required_bytes", 0), ("target", "esp32c3")):
            with self.subTest(field=field, value=value):
                candidate = copy.deepcopy(self.before_ota)
                candidate[field] = value
                with self.assertRaises(ota.Refused):
                    ota.validate_device(self.case.plan["board"], self.before_status, candidate)
        for field, value in (("safe_mode", True), ("runtime_state", "booting"), ("largest_block", 1)):
            candidate = copy.deepcopy(self.before_status)
            candidate["sys"][field] = value
            with self.assertRaises(ota.Refused):
                ota.validate_device(self.case.plan["board"], candidate, self.before_ota)

    def test_offline_ready_never_contacts_device(self):
        self.ready_fixture()
        with patch.object(ota, "exchange", side_effect=AssertionError("offline check contacted device")):
            self.case.ready()

    def test_saved_channel_is_distinct_from_checked_offer_source(self):
        self.before_ota.update(configured_channel="dev", channel="release")
        ota.validate_device(self.case.plan["board"], self.before_status, self.before_ota)
        self.before_ota.update(configured_channel="release", channel="dev")
        with self.assertRaises(ota.Refused):
            ota.validate_device(self.case.plan["board"], self.before_status, self.before_ota)

    def test_inventory_change_during_preflight_stops_before_upload_attempt(self):
        self.ready_fixture()
        def changed_inventory(board, path, **kwargs):
            result = self.device_get(board, path)
            if path == "/ota/status":
                data = json.loads(self.inventory.read_text())
                data["boards"][0]["address"] = "192.0.2.18"
                self.inventory.write_text(json.dumps(data))
            return result
        with patch.object(ota, "get_json", side_effect=changed_inventory), \
             patch.object(ota, "exchange", side_effect=AssertionError("upload must not occur")):
            with self.assertRaises(ota.Refused):
                ota.execute(self.case, self.case.plan["case_id"], observe_seconds=0)
        self.assertFalse((self.directory / "upload-attempt.json").exists())

    def test_timeout_consumes_only_attempt_and_never_reposts(self):
        self.ready_fixture()
        with patch.object(ota, "get_json", side_effect=self.device_get), \
             patch.object(ota, "exchange", side_effect=TimeoutError("inert lost reply")) as post:
            with self.assertRaises(ota.Refused):
                ota.execute(self.case, self.case.plan["case_id"], observe_seconds=0)
            with self.assertRaises(ota.Refused):
                ota.execute(self.case, self.case.plan["case_id"], observe_seconds=0)
        self.assertEqual(post.call_count, 1)
        self.assertEqual(post.call_args.args[1], "/ota/upload")
        self.assertEqual(post.call_args.kwargs["body"], SIGNED)
        self.assertTrue((self.directory / "upload-attempt.json").is_file())
        self.assertEqual(self.case.receipt("transfer")["result"], "unknown_or_refused")

    def test_case_specific_explicit_approval_precedes_network(self):
        self.ready_fixture()
        with patch.object(ota, "get_json", side_effect=AssertionError("no approved contact")):
            with self.assertRaises(ota.Refused):
                ota.execute(self.case, "wrong-case", observe_seconds=0)

    def test_acceptance_requires_new_boot_exact_build_health_and_retention(self):
        self.ready_fixture()
        calls = 0
        def observations(board, path, **kwargs):
            nonlocal calls
            calls += 1
            value = self.device_get(board, path)
            if calls > 2 and path == "/ota/status":
                value["boot_id"] = "1123456789abcdef"
            return value
        with patch.object(ota, "get_json", side_effect=observations), \
             patch.object(ota, "exchange", return_value=(200, b'{"result":true}')) as post:
            result = ota.execute(self.case, self.case.plan["case_id"], observe_seconds=1)
        self.assertEqual(post.call_count, 1)
        self.assertEqual(result["build"], "exact")
        self.assertEqual(result["changed_behavior"], "not_run")
        self.assertEqual(self.case.receipt("transfer")["result"], "reported_committed")

    def test_lost_http_reply_can_be_resolved_only_by_passive_exact_boot_evidence(self):
        self.ready_fixture()
        calls = 0
        def observations(board, path, **kwargs):
            nonlocal calls
            calls += 1
            value = self.device_get(board, path)
            if calls > 2 and path == "/ota/status":
                value["boot_id"] = "2123456789abcdef"
            return value
        with patch.object(ota, "get_json", side_effect=observations), \
             patch.object(ota, "exchange", side_effect=TimeoutError("inert reply loss")) as post:
            result = ota.execute(self.case, self.case.plan["case_id"], observe_seconds=1)
        self.assertEqual(post.call_count, 1)
        self.assertEqual(result["boot"], "new")
        self.assertEqual(result["transfer"], "unknown_or_refused")
        self.assertEqual(self.case.receipt("transfer")["result"], "unknown_or_refused")

    def test_retention_change_after_reboot_is_not_accepted(self):
        self.ready_fixture()
        calls = 0
        def observations(board, path, **kwargs):
            nonlocal calls
            calls += 1
            value = self.device_get(board, path)
            if calls > 2:
                if path == "/ota/status":
                    value["boot_id"] = "3123456789abcdef"
                else:
                    value["key_fingerprint"] = "unexpected-new-identity"
            return value
        with patch.object(ota, "get_json", side_effect=observations), \
             patch.object(ota, "exchange", return_value=(200, b'{"result":true}')) as post:
            with self.assertRaises(ota.IdentityMismatch):
                ota.execute(self.case, self.case.plan["case_id"], observe_seconds=1)
        self.assertEqual(post.call_count, 1)
        self.assertEqual(self.case.receipt("device")["result"], "IdentityMismatch")

    def test_actual_syslog_host_and_port_changes_and_disappearance_affect_retention(self):
        before = copy.deepcopy(self.before_status)
        before["syslog"] = {"configured": True, "host": "logs.example.invalid", "port": 514}
        for changed in ({"configured": True, "host": "other.example.invalid", "port": 514},
                        {"configured": True, "host": "logs.example.invalid", "port": 1514},
                        {"configured": False}, None):
            after = copy.deepcopy(before)
            if changed is None:
                after.pop("syslog")
            else:
                after["syslog"] = changed
            with self.subTest(changed=changed):
                self.assertNotEqual(ota.retention(before), ota.retention(after))

    def reject_syslog_after_reboot(self, changed):
        self.ready_fixture()
        self.before_status["syslog"] = {"configured": True, "host": "logs.example.invalid", "port": 514}
        calls = 0
        def observations(board, path, **kwargs):
            nonlocal calls
            calls += 1
            value = self.device_get(board, path)
            if calls > 2:
                if path == "/ota/status":
                    value["boot_id"] = "5123456789abcdef"
                elif changed is None:
                    value.pop("syslog")
                else:
                    value["syslog"] = changed
            return value
        with patch.object(ota, "get_json", side_effect=observations), \
             patch.object(ota, "exchange", return_value=(200, b'{"result":true}')) as post:
            with self.assertRaises(ota.IdentityMismatch):
                ota.execute(self.case, self.case.plan["case_id"], observe_seconds=1)
        self.assertEqual(post.call_count, 1)
        self.assertEqual(self.case.receipt("device")["result"], "IdentityMismatch")

    def test_syslog_host_change_after_reboot_is_rejected(self):
        self.reject_syslog_after_reboot({"configured": True, "host": "other.example.invalid", "port": 514})

    def test_syslog_destination_disappearance_after_reboot_is_rejected(self):
        self.reject_syslog_after_reboot(None)

    def test_new_boot_with_other_build_is_not_accepted(self):
        self.ready_fixture()
        calls = 0
        def observations(board, path, **kwargs):
            nonlocal calls
            calls += 1
            value = self.device_get(board, path)
            if calls > 2 and path == "/ota/status":
                value["boot_id"] = "4123456789abcdef"
                value["elf_sha256"] = "f" * 64
            return value
        with patch.object(ota, "get_json", side_effect=observations), \
             patch.object(ota, "exchange", return_value=(200, b'{"result":true}')) as post:
            with self.assertRaises(ota.IdentityMismatch):
                ota.execute(self.case, self.case.plan["case_id"], observe_seconds=1)
        self.assertEqual(post.call_count, 1)


class TransportTests(unittest.TestCase):
    def test_actual_wire_sends_one_exact_binary_without_redirect_follow(self):
        class PassiveSocket:
            def __init__(self):
                self.sent = bytearray()
                self.connects = []
                self.closed = False
                self.response = b'HTTP/1.1 302 Found\r\nContent-Length: 2\r\nLocation: http://198.51.100.20/\r\n\r\n{}'
            def settimeout(self, value):
                assert value > 0
            def connect(self, address):
                self.connects.append(address)
            def sendall(self, data):
                self.sent.extend(data)
            def recv(self, count):
                result, self.response = self.response[:count], self.response[count:]
                return result
            def close(self):
                self.closed = True
        sock = PassiveSocket()
        with patch.object(ota.socket, "socket", return_value=sock):
            code, body = ota.exchange({"address": "192.0.2.17"}, "/ota/upload", body=SIGNED)
        self.assertEqual(code, 302)
        self.assertEqual(body, b"{}")
        self.assertEqual(sock.connects, [("192.0.2.17", 80)])
        headers, data = bytes(sock.sent).split(b"\r\n\r\n", 1)
        self.assertIn(b"POST /ota/upload HTTP/1.1", headers)
        self.assertIn(f"Content-Length: {len(SIGNED)}".encode(), headers)
        self.assertEqual(data, SIGNED)
        self.assertTrue(sock.closed)


if __name__ == "__main__":
    unittest.main()
