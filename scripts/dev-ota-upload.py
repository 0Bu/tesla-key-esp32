#!/usr/bin/env python3
"""Prepare and execute one explicitly authorized, source-bound local OTA test.

Only preflight/execute contact the selected inventory board. No channel change, signing,
publication, vehicle command, retry or fallback target is implemented. Local authenticated
receipts detect stale/edited evidence; they are not attestation against a malicious local user.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import hmac
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
TARGETS = ("esp32", "esp32s3", "esp32c3", "esp32c6")
PROJECT = "tesla-key-esp32"
MAX_IMAGE = 0x1F0000
MAX_JSON = 65536
HEX64 = re.compile(r"[0-9a-f]{64}\Z")
VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z.-]+)?\Z")
REVIEW_AREAS = ("ota_heap_security", "persistence_pairing", "ble_evcc", "documentation_ui")
BUILD_GATES = ("dependency_contract", "configuration_source", "partition_source", "configure",
               "sdkconfig_defaults", "application_role", "compile", "otadata", "effective_build",
               "partition_binary", "firmware_identity", "stack_observed", "idf_size",
               "resource_budget", "stack_budget")


class Refused(RuntimeError):
    pass


class IdentityMismatch(Refused):
    pass


class SourceChanged(Refused):
    pass


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise Refused(reason)


def canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def regular(path: Path, limit: int = 128 * 1024 * 1024) -> bytes:
    """Open the leaf without following a substituted symlink; bound and recheck its identity."""
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    with os.fdopen(fd, "rb") as stream:
        before = os.fstat(stream.fileno())
        require(stat.S_ISREG(before.st_mode) and before.st_size <= limit, "unsafe or oversized input")
        data = stream.read(limit + 1)
        after = os.fstat(stream.fileno())
    require(len(data) == before.st_size and len(data) <= limit and
            (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
            (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns), "input changed while reading")
    return data


def write_private(path: Path, data: bytes, *, exclusive: bool = False) -> None:
    flags = os.O_WRONLY | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0)
    flags |= os.O_EXCL if exclusive else os.O_TRUNC
    fd = os.open(path, flags, 0o600)
    with os.fdopen(fd, "wb") as out:
        out.write(data)
        out.flush()
        os.fsync(out.fileno())


def git(root: Path, *args: str) -> bytes:
    environment = {key: value for key, value in clean_environment().items() if not key.startswith("GIT_")}
    return subprocess.check_output(["git", "-c", "core.fsmonitor=false", "-c", f"safe.directory={root}",
                                    "-C", str(root), *args], stderr=subprocess.DEVNULL, env=environment)


def source_manifest(root: Path) -> dict:
    require(not git(root, "ls-files", "-u"), "unresolved source conflicts")
    names = set(git(root, "ls-files", "-z", "--cached").split(b"\0"))
    untracked = set(git(root, "ls-files", "-z", "--others", "--exclude-standard").split(b"\0"))
    rows = []
    total = 0
    for raw in sorted((names | untracked) - {b""}):
        name = raw.decode("utf-8", "strict")
        rel = Path(name)
        require(not rel.is_absolute() and ".." not in rel.parts, "unsafe source path")
        path = root / rel
        if not path.exists() and not path.is_symlink():
            rows.append({"path": name, "state": "deleted"})
            continue
        # A source snapshot must not dereference an external/secret link or casually read NVS.
        require(not path.is_symlink() and all(not (root / Path(*rel.parts[:i])).is_symlink()
                for i in range(1, len(rel.parts))), "symlink in intended source")
        if raw in untracked:
            require(path.suffix.lower() not in (".bin", ".pem", ".key") and
                    path.name not in (".env", ".git-credentials"),
                    "untracked secret/image input must be kept outside the source tree")
        data = regular(path, 16 * 1024 * 1024)
        total += len(data)
        require(total <= 128 * 1024 * 1024, "intended source exceeds snapshot bound")
        rows.append({"path": name, "state": "file", "bytes": len(data), "sha256": digest(data),
                     "executable": bool(path.stat().st_mode & 0o111)})
    try:
        head = git(root, "rev-parse", "HEAD").decode().strip()
    except subprocess.CalledProcessError:
        head = None  # An initial local checkout need not have a commit.
    payload = {"schema": 1, "head": head, "files": rows}
    return {**payload, "source_id": digest(canonical(payload))}


def artifact_module(root: Path):
    spec = importlib.util.spec_from_file_location("ota_artifact_verifier", root / "scripts/check-firmware-artifacts.py")
    require(spec is not None and spec.loader is not None, "artifact verifier unavailable")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def image_identity(data: bytes) -> str:
    require(len(data) >= 208, "app descriptor has no ELF identity")
    value = data[176:208].hex()
    require(value not in ("0" * 64, "f" * 64), "app ELF identity is unavailable")
    return value


def inventory(path: Path, board_id: str, root: Path) -> tuple[dict, str]:
    require(path.is_absolute() and not path.is_symlink() and not path.resolve().is_relative_to(root.resolve()),
            "inventory must be a private external regular file")
    require(not (path.stat().st_mode & 0o077), "inventory permissions must be private")
    raw = regular(path, MAX_JSON)
    data = json.loads(raw)
    require(isinstance(data, dict) and isinstance(data.get("boards"), list), "invalid private board inventory")
    matches = [row for row in data["boards"] if isinstance(row, dict) and row.get("id") == board_id]
    require(len(matches) == 1, "production board selection is absent or ambiguous")
    row = matches[0]
    require(row.get("role") == "production" and row.get("project") == PROJECT and
            row.get("target") in TARGETS, "inventory role/project/target mismatch")
    address = ipaddress.ip_address(row.get("address", ""))
    require(not (address.is_unspecified or address.is_multicast or address.is_loopback),
            "inventory requires the selected board's unicast IP address")
    mac = row.get("board_mac", "")
    require(isinstance(mac, str) and re.fullmatch(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}", mac) is not None and
            mac not in ("00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff"), "invalid inventory hardware identity")
    selected = {key: row[key] for key in ("id", "role", "project", "target", "board_mac")}
    selected["address"] = str(address)
    return selected, digest(raw)


def clean_environment() -> dict[str, str]:
    return {key: value for key, value in os.environ.items()
            if not key.startswith(("OTA_SIGNING_KEY", "GH_", "GITHUB_", "LD_", "DYLD_")) and
            "SIGNING_KEY" not in key and key not in ("BASH_ENV", "ENV", "PYTHONPATH", "PYTHONHOME")}


def container_build(build: Path, target: str, version: str, *, root: Path = ROOT,
                    runner=None, environment: dict | None = None) -> dict:
    """Run the existing target acceptance validators inside the pinned wrapper, without signing."""
    require(target in TARGETS and VERSION.fullmatch(version) is not None and len(version.encode()) <= 31,
            "invalid local build target/version")
    require(re.fullmatch(r"build/ota-[0-9a-f]{32}-[0-9a-f]{16}-" + re.escape(target), str(build)) is not None,
            "invalid case build directory")
    require(not (root / "build").is_symlink() and not (root / build).is_symlink() and
            (root / build).is_dir() and not any((root / build).iterdir()), "target build must start empty")
    env = dict(os.environ if environment is None else environment)
    forbidden = {"CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
                 "DEPENDENCIES_OUTPUT", "SUNPRO_DEPENDENCIES", "GCC_EXEC_PREFIX", "COMPILER_PATH",
                 "LIBRARY_PATH", "SDKCONFIG_DEFAULTS", "EXTRA_COMPONENT_DIRS", "CFLAGS", "CXXFLAGS",
                 "CPPFLAGS", "LDFLAGS"}
    require(not any(key in forbidden or key.startswith(("IDF_COMPONENT_", "TESLA_BLE_")) for key in env),
            "unbound compiler/component environment override")
    require(bool(env.get("IDF_PATH")), "local target gates require the pinned ESP-IDF container")
    # The image and an existing wrapper daemon may enable ccache. Override inside the actual
    # container, rather than assuming an unforwarded host variable changes the compiler launcher.
    env = {key: value for key, value in env.items() if not key.startswith("CCACHE_")}
    env.pop("IDF_TARGET", None)
    env.pop("SDKCONFIG", None)
    env.update(IDF_CCACHE_ENABLE="0", EXTRA_CFLAGS="-fstack-usage", EXTRA_CXXFLAGS="-fstack-usage")
    runner = subprocess.run if runner is None else runner
    diagnostic = build.with_name(build.name + "-checks")
    (root / diagnostic).mkdir(exist_ok=False)
    lock = root / f"dependencies.lock.{target}"
    lock_sha256 = digest(regular(lock))
    py = sys.executable
    config = str(build / "sdkconfig")
    app = str(build / f"{PROJECT}.bin")
    stack = str(diagnostic / f"stack-usage-{target}.json")
    size = str(diagnostic / f"size-{target}.json")
    idf = ["idf.py", "-B", str(build), "-D", f"SDKCONFIG={config}", "-D", f"PROJECT_VER={version}"]
    rows = []

    def run(label: str, command: list[str], output: str | None = None) -> None:
        log = Path(output).name if output is not None else f"gate-{label}.log"
        path = root / diagnostic / log
        print(json.dumps({"local_ota_build_gate": label, "argv": command}), flush=True)
        with path.open("wb") as out:
            result = runner(command, cwd=root, env=env, stdout=out,
                            stderr=None if label == "idf_size" else subprocess.STDOUT, timeout=3600)
        data = regular(path)
        # Retain complete bounded logs, but keep the enclosing progress log compact.
        print(data[-8192:].decode("utf-8", "replace"), end="", flush=True)
        rows.append({"label": label, "argv": command, "exit": result.returncode,
                     "log": log, "log_sha256": digest(data)})
        require(result.returncode == 0, f"target build gate failed: {label}")

    run("dependency_contract", [py, "scripts/check-dependency-contract.py", "--root", ".", "--idf-path", env["IDF_PATH"]])
    run("configuration_source", [py, "scripts/check-config-repair-contract.py"])
    run("partition_source", [py, "scripts/check-partition-contract.py", "--csv", "partitions.csv"])
    run("configure", [*idf, "set-target", target])
    run("sdkconfig_defaults", [py, "scripts/check-sdkconfig-defaults.py", "--target", target, "--generated", config])
    run("application_role", [py, "scripts/check-config-repair-contract.py", "--target", target,
                             "--sdkconfig", config, "--application-config"])
    run("compile", [*idf, "build"])
    compiled = regular(root / app, MAX_IMAGE)
    compiled_sha256 = digest(compiled)
    run("otadata", [py, "scripts/check-otadata-contract.py", str(build / "ota_data_initial.bin")])
    run("effective_build", [py, "scripts/check-build-semantics.py", "--target", target, "--sdkconfig", config,
                            "--compile-commands", str(build / "compile_commands.json"), "--source-root", str(root)])
    run("partition_binary", [py, "scripts/check-partition-contract.py", "--csv", "partitions.csv",
                             "--binary", str(build / "partition_table/partition-table.bin")])
    run("firmware_identity", [py, "scripts/check-firmware-artifacts.py", "--target", target, "--version", version,
                              "--bootloader", str(build / "bootloader/bootloader.bin"), "--app", app])
    require(digest(regular(lock)) == lock_sha256, "target lock changed during the build")
    run("stack_observed", [py, "scripts/check-stack-usage.py", "--target", target,
                           "--stack-root", str(build / "esp-idf/main"), "--write-observed", stack])
    run("idf_size", [py, "-m", "esp_idf_size", "--format", "json2", str(build / f"{PROJECT}.map")], size)
    verifier = artifact_module(root)
    unsigned_size = len(compiled)
    projected = ((unsigned_size + verifier.SIGNATURE_ALIGNMENT - 1) // verifier.SIGNATURE_ALIGNMENT *
                 verifier.SIGNATURE_ALIGNMENT + verifier.SIGNATURE_SECTOR)
    run("resource_budget", [py, "scripts/report-firmware-size.py", "--idf-size", size, "--unsigned-app", app,
                            "--projected-signed-size", str(projected), "--policy-limit", str(verifier.APP_POLICY_LIMIT),
                            "--target", target, "--budget-baseline", "scripts/firmware-size-baseline.json",
                            "--enforce-budget"], str(diagnostic / f"size-{target}.md"))
    run("stack_budget", [py, "scripts/check-stack-usage.py", "--target", target, "--observed-json", stack,
                         "--baseline", "scripts/firmware-stack-baseline.json"])
    require(digest(regular(lock)) == lock_sha256, "target lock changed during build validation")
    require(digest(regular(root / app, MAX_IMAGE)) == compiled_sha256, "compiled image changed during target validation")
    manifest = {"schema": 1, "target": target, "version": version, "directory": str(build),
                "compiler_environment": {key: env[key] for key in ("IDF_CCACHE_ENABLE", "EXTRA_CFLAGS", "EXTRA_CXXFLAGS")},
                "lock_sha256": lock_sha256, "unsigned_sha256": compiled_sha256,
                "elf_sha256": image_identity(compiled), "projected_signed_size": projected, "gates": rows}
    write_private(root / diagnostic / "build-gates.json", canonical(manifest))
    return manifest


class Case:
    def __init__(self, directory: Path, root: Path = ROOT):
        self.root = root.resolve()
        self.directory = directory.resolve()
        require(directory.is_absolute() and not directory.is_symlink() and
                not self.directory.is_relative_to(self.root), "evidence must be outside the checkout")
        require(stat.S_ISDIR(directory.stat().st_mode) and not (directory.stat().st_mode & 0o077),
                "evidence directory must be private")
        self.key = regular(self.directory / "journal-secret", 32)
        require(len(self.key) == 32, "invalid local evidence journal")
        self.plan = self.load("plan")

    @classmethod
    def prepare(cls, directory: Path, inventory_path: Path, board_id: str, version: str,
                root: Path = ROOT) -> "Case":
        root = root.resolve()
        require(directory.is_absolute() and not directory.resolve().is_relative_to(root),
                "evidence must be outside the checkout")
        require(VERSION.fullmatch(version) is not None and len(version.encode()) <= 31,
                "invalid app version")
        selected, inventory_sha = inventory(inventory_path, board_id, root)
        manifest = source_manifest(root)
        directory.mkdir(mode=0o700, parents=False, exist_ok=False)
        write_private(directory / "journal-secret", secrets.token_bytes(32), exclusive=True)
        instance = object.__new__(cls)
        instance.root = root
        instance.directory = directory.resolve()
        instance.key = regular(directory / "journal-secret", 32)
        instance.plan = {"case_id": secrets.token_hex(16), "source_id": manifest["source_id"],
                         "version": version, "target": selected["target"], "board": selected,
                         "inventory": str(inventory_path.resolve()), "inventory_sha256": inventory_sha}
        instance.save("source", manifest)
        instance.save("plan", instance.plan)
        return instance

    def save(self, kind: str, payload: dict) -> None:
        envelope = {"kind": kind, "payload": payload}
        envelope["mac"] = hmac.new(self.key, canonical(envelope), hashlib.sha256).hexdigest()
        write_private(self.directory / f"{kind}.json", canonical(envelope) + b"\n")

    def load(self, kind: str) -> dict:
        envelope = json.loads(regular(self.directory / f"{kind}.json", 16 * 1024 * 1024))
        mac = envelope.pop("mac", None)
        require(envelope.get("kind") == kind and isinstance(mac, str) and
                hmac.compare_digest(mac, hmac.new(self.key, canonical(envelope), hashlib.sha256).hexdigest()),
                "missing, edited or unauthenticated local evidence")
        require(isinstance(envelope.get("payload"), dict), "invalid evidence payload")
        return envelope["payload"]

    def unchanged(self) -> None:
        if source_manifest(self.root)["source_id"] != self.plan["source_id"]:
            raise SourceChanged("source changed; prepare a new case")

    def record(self, kind: str, **fields: Any) -> None:
        self.save(kind, {"source_id": self.plan["source_id"], "created": time.time(), **fields})

    def invalidate(self, kind: str) -> None:
        """Withdraw old PASS authority before a new attempt, retaining its private history."""
        phases = ("tests", "build", "image", "review")
        kinds = phases[phases.index(kind):]
        history = self.directory / "history"
        require(not history.is_symlink(), "unsafe evidence history")
        history.mkdir(mode=0o700, exist_ok=True)
        for name in [f"{phase}.json" for phase in kinds] + (["review-result.json"] if "review" in kinds else []):
            path = self.directory / name
            if path.exists() or path.is_symlink():
                require(not path.is_symlink() and path.is_file(), "unsafe prior evidence")
                path.rename(history / f"{time.time_ns()}-{secrets.token_hex(4)}-{name}")

    def receipt(self, kind: str) -> dict:
        value = self.load(kind)
        require(value.get("source_id") == self.plan["source_id"], "evidence belongs to a different source")
        for row in [*value.get("commands", []), *value.get("gates", [])]:
            require(row.get("exit") == 0 and digest(regular(self.directory / row["log"])) == row["log_sha256"],
                    "command failed or its log changed")
        return value

    def run_commands(self, kind: str, commands: list[list[str]], timeout: int) -> list[dict]:
        rows = []
        for index, command in enumerate(commands):
            self.unchanged()
            log = f"{kind}-{secrets.token_hex(8)}-{index}.log"
            started = time.time()
            environment = clean_environment()
            if kind == "build":
                environment["CCACHE_DISABLE"] = "1"
            with (self.directory / log).open("wb") as out:
                result = subprocess.run(command, cwd=self.root, env=environment, stdout=out,
                                        stderr=subprocess.STDOUT, timeout=timeout)
            self.unchanged()
            require(result.returncode == 0, f"{kind} command failed; see private evidence log")
            rows.append({"argv": command, "exit": result.returncode, "started": started,
                         "finished": time.time(), "log": log, "log_sha256": digest(regular(self.directory / log))})
        return rows

    def tests(self) -> None:
        self.invalidate("tests")
        commands = [[str(self.root / "scripts/run-mock-tests.sh"), "--require-all"],
                    [str(self.root / "scripts/repo-lint.sh")],
                    [str(self.root / "scripts/test-build-contracts.sh")]]
        self.record("tests", commands=self.run_commands("tests", commands, 1800))

    def build(self) -> None:
        self.invalidate("build")
        with self.build_lock():
            self.build_owned()

    @contextmanager
    def build_lock(self):
        require(not (self.root / "build").is_symlink(), "build root must not be a symlink")
        (self.root / "build").mkdir(exist_ok=True)
        fd = os.open(self.root / "build/.local-ota-build.lock", os.O_RDWR | os.O_CREAT |
                     getattr(os, "O_NOFOLLOW", 0), 0o600)
        try:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise Refused("another local OTA build owns the build window") from exc
            yield
        finally:
            os.close(fd)

    def build_owned(self) -> None:
        self.unchanged()
        self.receipt("tests")
        build = Path("build") / f"ota-{self.plan['case_id']}-{secrets.token_hex(8)}-{self.plan['target']}"
        # Never inherit an old SDKCONFIG/CMake object tree, even if source mtimes were kept.
        (self.root / build).mkdir(exist_ok=False)
        command = [str(self.root / "scripts/idf-docker.sh"), "python3", "scripts/dev-ota-upload.py",
                   "_ci-build-in-container", "--build-dir", str(build), "--target", self.plan["target"],
                   "--version", self.plan["version"]]
        commands = self.run_commands("build", [command], 3600)
        diagnostic = self.root / build.with_name(build.name + "-checks")
        require(not diagnostic.is_symlink(), "unsafe target diagnostics")
        validation = json.loads(regular(diagnostic / "build-gates.json", MAX_JSON))
        require(validation.get("target") == self.plan["target"] and validation.get("version") == self.plan["version"] and
                validation.get("directory") == str(build) and
                validation.get("compiler_environment") == {"IDF_CCACHE_ENABLE": "0", "EXTRA_CFLAGS": "-fstack-usage", "EXTRA_CXXFLAGS": "-fstack-usage"} and
                tuple(row.get("label") for row in validation.get("gates", [])) == BUILD_GATES,
                "target acceptance gate evidence is missing or differs")
        gates = []
        for row in validation["gates"]:
            require(row.get("exit") == 0 and isinstance(row.get("argv"), list) and
                    isinstance(row.get("log"), str) and Path(row["log"]).name == row["log"], "unsafe or failed target gate")
            data = regular(diagnostic / row["log"])
            require(digest(data) == row.get("log_sha256"), "target gate log changed")
            log = f"build-gate-{secrets.token_hex(8)}-{row['label']}.log"
            write_private(self.directory / log, data, exclusive=True)
            gates.append({**row, "log": log})
        data = regular(self.root / build / f"{PROJECT}.bin", MAX_IMAGE)
        require(digest(data) == validation.get("unsigned_sha256") and
                image_identity(data) == validation.get("elf_sha256"), "validated build image changed before freezing")
        destination = self.directory / "unsigned.bin"
        write_private(destination, data)
        verifier = artifact_module(self.root)
        verifier.validate_app(self.plan["target"], self.plan["version"], destination, signed_app=False)
        self.record("build", commands=commands, gates=gates, target=self.plan["target"], version=self.plan["version"],
                    directory=str(build), unsigned_sha256=digest(data), elf_sha256=image_identity(data))

    def bind_image(self, image: Path, expected_sha256: str) -> None:
        self.invalidate("image")
        self.unchanged()
        build = self.receipt("build")
        require(HEX64.fullmatch(expected_sha256) is not None, "exact lowercase signed-image SHA-256 required")
        data = regular(image, MAX_IMAGE)
        require(digest(data) == expected_sha256, "signed-image SHA-256 mismatch")
        unsigned = regular(self.directory / "unsigned.bin", MAX_IMAGE)
        require(digest(unsigned) == build["unsigned_sha256"] and data.startswith(unsigned),
                "signed image differs from the tested unsigned build")
        destination = self.directory / "signed.bin"
        write_private(destination, data)
        self.validate_signed(data)
        require(image_identity(data) == build["elf_sha256"], "signed build identity differs")
        self.record("image", sha256=expected_sha256, bytes=len(data), elf_sha256=image_identity(data),
                    target=self.plan["target"], version=self.plan["version"])

    def validate_signed(self, data: bytes) -> None:
        verifier = artifact_module(self.root)
        pin = verifier.read_public_key_digest(self.root / "scripts/ota-signing-public-key.sha256")
        with tempfile.TemporaryDirectory(prefix="tesla-ota-verification-") as tmp:
            frozen = Path(tmp) / "app.bin"
            write_private(frozen, data, exclusive=True)
            verifier.validate_app(self.plan["target"], self.plan["version"], frozen,
                                  signed_app=True, expected_public_key_digest=pin)

    def review(self) -> None:
        self.invalidate("review")
        self.unchanged()
        self.receipt("tests")
        self.receipt("build")
        image = self.receipt("image")
        data = regular(self.directory / "signed.bin", MAX_IMAGE)
        require(digest(data) == image["sha256"], "signed image changed")
        self.validate_signed(data)
        registry = json.loads(regular(self.root / ".agents/subagents.json"))
        roles = {row["TypeName"]: row for row in registry["subagents"]}
        selected = ("heap_safety_reviewer", "multi_target_build_reviewer", "doc_drift_checker", "agent_config_reviewer")
        require(all(name in roles and roles[name].get("SandboxMode") == "read-only" for name in selected),
                "required read-only reviewer registrations are missing")
        schema = {"type": "object", "additionalProperties": False,
                  "properties": {"outcome": {"type": "string", "enum": ["ready", "blocked"]},
                                 "source_id": {"type": "string"}, "image_sha256": {"type": "string"},
                                 "areas": {"type": "array", "items": {"type": "string"}},
                                 "findings": {"type": "array", "items": {"type": "object",
                                     "additionalProperties": False, "properties": {
                                         "severity": {"type": "string", "enum": ["P0", "P1", "P2", "P3"]},
                                         "detail": {"type": "string"}, "blocking": {"type": "boolean"}},
                                     "required": ["severity", "detail", "blocking"]}}},
                  "required": ["outcome", "source_id", "image_sha256", "areas", "findings"]}
        write_private(self.directory / "review-schema.json", canonical(schema))
        prompt = ("Perform an independent read-only review of the complete current local intended source, "
                  "including untracked files, against HEAD. Never edit, publish, sign, access secrets/NVS, "
                  "contact a board/vehicle/cluster, or send commands. Use the existing project-review and "
                  "vehicle-command-audit instructions where relevant. Review actual code and tests; do not "
                  "treat successful compilation as review proof. Mark material documentation quality "
                  "findings blocking even if severity is P3. Include OTA/heap/security, persistence/"
                  "NVS/pairing, BLE/evcc meaning, documentation quality and UI where relevant. Read the "
                  "private evidence source/tests/build/image receipts and logs as needed; inventory is not "
                  "part of review. Report ready only with no blocking findings or missing required evidence.\n"
                  f"Source ID: {self.plan['source_id']}\nSigned image SHA-256: {image['sha256']}\n"
                  f"Evidence directory: {self.directory}\nRequired areas: {', '.join(REVIEW_AREAS)}\n" +
                  "\n\n".join(roles[name]["Prompt"] for name in selected))
        executable = shutil.which("codex")
        require(executable is not None, "Codex CLI is required for an actual independent read-only review")
        command = [executable, "exec", "--ignore-user-config", "--sandbox", "read-only", "--config", 'approval_policy="never"',
                   "--ephemeral", "--color", "never", "--json", "--output-schema",
                   str(self.directory / "review-schema.json"), "--output-last-message",
                   str(self.directory / "review-result.json"), "-C", str(self.root), prompt]
        rows = self.run_commands("review", [command], 3600)
        report = json.loads(regular(self.directory / "review-result.json", MAX_JSON))
        events = []
        for line in regular(self.directory / rows[0]["log"]).splitlines():
            try:
                events.append(json.loads(line))
            except (json.JSONDecodeError, UnicodeDecodeError):
                pass
        threads = [row.get("thread_id") for row in events if isinstance(row, dict) and row.get("type") == "thread.started"]
        require(len(threads) == 1 and isinstance(threads[0], str) and bool(threads[0]), "reviewer execution identity missing")
        require(report.get("outcome") == "ready" and report.get("source_id") == self.plan["source_id"] and
                report.get("image_sha256") == image["sha256"] and
                set(report.get("areas", [])) == set(REVIEW_AREAS) and isinstance(report.get("findings"), list) and
                all(isinstance(row, dict) and row.get("severity") == "P3" and row.get("blocking") is False
                    for row in report["findings"]), "independent review is blocked or incomplete")
        self.record("review", commands=rows, reviewer_thread=threads[0], sandbox="read-only", approval="never",
                    image_sha256=image["sha256"], report_sha256=digest(canonical(report)))

    def ready(self) -> tuple[dict, bytes]:
        self.unchanged()
        tests, build, image, review = (self.receipt(kind) for kind in ("tests", "build", "image", "review"))
        require(len(tests.get("commands", [])) == 3 and len(build.get("commands", [])) == 1 and
                tuple(row.get("label") for row in build.get("gates", [])) == BUILD_GATES and
                len(review.get("commands", [])) == 1 and review.get("sandbox") == "read-only" and
                review.get("approval") == "never" and review.get("reviewer_thread") and
                review.get("image_sha256") == image.get("sha256"), "required executed evidence is missing")
        require(digest(canonical(json.loads(regular(self.directory / "review-result.json", MAX_JSON)))) ==
                review.get("report_sha256"), "review result changed")
        require(build.get("target") == image.get("target") == self.plan["target"] and
                build.get("version") == image.get("version") == self.plan["version"], "artifact target/version differs")
        data = regular(self.directory / "signed.bin", MAX_IMAGE)
        require(digest(data) == image["sha256"] and len(data) == image["bytes"] and
                image_identity(data) == image["elf_sha256"] == build["elf_sha256"], "signed artifact changed")
        unsigned = regular(self.directory / "unsigned.bin", MAX_IMAGE)
        require(digest(unsigned) == build["unsigned_sha256"] and data.startswith(unsigned), "built bytes changed")
        self.validate_signed(data)
        selected, inventory_sha = inventory(Path(self.plan["inventory"]), self.plan["board"]["id"], self.root)
        require(selected == self.plan["board"] and inventory_sha == self.plan["inventory_sha256"], "private board binding changed")
        return image, data


def exchange(board: dict, path: str, *, body: bytes | None = None, timeout: float = 5) -> tuple[int, bytes]:
    """One numeric-address connection and one request, with an absolute deadline and no retry."""
    address = ipaddress.ip_address(board["address"])
    deadline = time.monotonic() + timeout
    sock = socket.socket(socket.AF_INET6 if address.version == 6 else socket.AF_INET, socket.SOCK_STREAM)
    def remaining() -> float:
        value = deadline - time.monotonic()
        require(value > 0, "HTTP deadline reached; installation outcome may be unknown")
        sock.settimeout(value)
        return value
    try:
        remaining()
        sock.connect((str(address), 80))
        host = f"[{address}]" if address.version == 6 else str(address)
        headers = [f"{'POST' if body is not None else 'GET'} {path} HTTP/1.1", f"Host: {host}",
                   "User-Agent: tesla-local-ota/1", "Accept: application/json", "Connection: close"]
        if body is not None:
            headers += ["Content-Type: application/octet-stream", f"Content-Length: {len(body)}"]
        remaining()
        sock.sendall(("\r\n".join(headers) + "\r\n\r\n").encode("ascii"))
        if body is not None:
            for offset in range(0, len(body), 65536):
                remaining()
                sock.sendall(body[offset:offset + 65536])
        response = bytearray()
        while b"\r\n\r\n" not in response:
            remaining()
            chunk = sock.recv(1024)
            require(bool(chunk), "HTTP response interrupted; installation outcome may be unknown")
            response.extend(chunk)
            require(len(response) <= 8192, "HTTP response headers exceed limit")
        header, payload = bytes(response).split(b"\r\n\r\n", 1)
        lines = header.decode("ascii").split("\r\n")
        match = re.fullmatch(r"HTTP/1\.[01] ([0-9]{3})(?: .*)?", lines.pop(0))
        require(match is not None, "invalid HTTP response status")
        fields = {}
        for line in lines:
            name, separator, value = line.partition(":")
            name = name.lower()
            require(separator == ":" and name not in fields, "invalid/duplicate HTTP response header")
            fields[name] = value.strip()
        length = fields.get("content-length", "")
        require("transfer-encoding" not in fields and length.isdecimal() and int(length) <= MAX_JSON,
                "unbounded or unsupported HTTP response")
        count = int(length)
        require(len(payload) <= count, "HTTP response exceeds declared length")
        while len(payload) < count:
            remaining()
            chunk = sock.recv(min(4096, count - len(payload)))
            require(bool(chunk), "HTTP response body interrupted")
            payload += chunk
        remaining()
        return int(match[1]), payload
    finally:
        sock.close()


def get_json(board: dict, path: str, *, timeout: float = 5) -> dict:
    code, raw = exchange(board, path, timeout=timeout)
    require(code == 200, "selected board refused passive preflight")
    value = json.loads(raw)
    require(isinstance(value, dict), "invalid device response")
    return value


def validate_device(board: dict, status: dict, ota: dict) -> None:
    system = status.get("sys", {})
    if not (isinstance(system, dict) and system.get("board_mac") == board["board_mac"] and
            ota.get("project") == PROJECT and ota.get("target") == board["target"]):
        raise IdentityMismatch("physical board/project/chip identity mismatch")
    require(status.get("ota", {}).get("channel") == "dev" and ota.get("channel") == "dev", "saved OTA channel must already be dev")
    require(ota.get("busy") is False and ota.get("verification") == "stable", "OTA is busy or firmware verification is pending/unknown")
    require(system.get("safe_mode") is False and system.get("runtime_state") == "ready", "board boot health is not ready")
    largest, required = system.get("largest_block"), ota.get("recovery_required_bytes")
    require(type(largest) is int and type(required) is int and required > 0 and largest >= required,
            "insufficient or unavailable INTERNAL contiguous recovery memory")
    require(isinstance(ota.get("boot_id"), str) and re.fullmatch(r"[0-9a-f]{16}", ota["boot_id"]) is not None and
            isinstance(ota.get("elf_sha256"), str) and HEX64.fullmatch(ota["elf_sha256"]) is not None,
            "exact boot/build identity is unavailable")


def retention(status: dict) -> str:
    # Retain only a digest of observable durable identity/configuration, never raw VIN/SSID/URI.
    fields = {key: status.get(key) for key in ("vin", "key_present", "key_fingerprint", "key_created", "paired", "paired_at")}
    for key, names in (("wifi", ("ssid",)), ("mqtt", ("broker",)), ("syslog", ("host", "port"))):
        source = status.get(key, {})
        fields[key] = {name: source.get(name) for name in names} if isinstance(source, dict) else None
    return digest(canonical(fields))


def read_device(board: dict, *, deadline: float | None = None) -> tuple[dict, dict]:
    def read(path: str) -> dict:
        if deadline is None:
            return get_json(board, path)
        remaining = deadline - time.monotonic()
        require(remaining > 0, "passive observation deadline reached")
        return get_json(board, path, timeout=min(5, remaining))
    status = read("/status")
    if not isinstance(status.get("sys"), dict) or status["sys"].get("board_mac") != board["board_mac"]:
        raise IdentityMismatch("physical board identity mismatch")
    ota = read("/ota/status")
    if ota.get("project") != PROJECT or ota.get("target") != board["target"]:
        raise IdentityMismatch("device project/chip identity mismatch")
    return status, ota


def preflight(case: Case) -> tuple[dict, dict, dict, bytes]:
    image, data = case.ready()
    board = case.plan["board"]
    status, ota = read_device(board)
    validate_device(board, status, ota)
    current = VERSION.fullmatch(str(status.get("version", "")))
    candidate = VERSION.fullmatch(case.plan["version"])
    require(current is not None and candidate is not None and
            tuple(map(int, candidate.groups())) >= tuple(map(int, current.groups())), "upload would downgrade the version core")
    return status, ota, image, data


def execute(case: Case, approved_case: str, *, observe_seconds: float = 180) -> dict:
    require(approved_case == case.plan["case_id"], "explicit approval must name this exact prepared case")
    require(not (case.directory / "upload-attempt.json").exists(), "this case already consumed its one upload attempt")
    before, old_ota, image, data = preflight(case)
    # Recheck source, inventory, receipts and artifact after the live GET window too.
    current_image, current_data = case.ready()
    require(current_image == image and current_data == data, "candidate changed after preflight")
    # Consume before connecting: an interrupted write or lost HTTP reply never grants a retry.
    write_private(case.directory / "upload-attempt.json", canonical({"case_id": approved_case,
                  "source_id": case.plan["source_id"], "image_sha256": image["sha256"],
                  "inventory_sha256": case.plan["inventory_sha256"], "started": time.time()}), exclusive=True)
    board = case.plan["board"]
    try:
        code, raw = exchange(board, "/ota/upload", body=data, timeout=180)
        reply = json.loads(raw)
        require(code == 200 and isinstance(reply, dict) and reply.get("result") is True,
                "upload did not report a committed update; inspect passively, do not retry")
        transfer = {"http_status": code, "result": "reported_committed"}
    except (OSError, ValueError, Refused) as error:
        transfer = {"result": "unknown_or_refused", "error": type(error).__name__}
    case.record("transfer", **transfer, image_sha256=image["sha256"])
    until = time.monotonic() + observe_seconds
    while time.monotonic() < until:
        try:
            status, ota = read_device(board, deadline=until)
            if ota.get("boot_id") != old_ota["boot_id"]:
                if ota.get("elf_sha256") != image["elf_sha256"] or status.get("version") != image["version"]:
                    raise IdentityMismatch("new boot is not the selected build; rollback or installation mismatch")
            validate_device(board, status, ota)
            if ota["boot_id"] != old_ota["boot_id"] and ota["elf_sha256"] == image["elf_sha256"] and status.get("version") == image["version"]:
                if retention(status) != retention(before):
                    case.record("device", result="observable_retention_changed", image_sha256=image["sha256"])
                    raise IdentityMismatch("observable identity/configuration changed")
                result = {"boot": "new", "build": "exact", "health": "ready_stable",
                          "observable_retention": "unchanged", "changed_behavior": "not_run",
                          "transfer": transfer["result"],
                          "image_sha256": image["sha256"]}
                case.unchanged()
                case.record("device", **result)
                return result
        except (IdentityMismatch, SourceChanged) as error:
            case.record("device", result=type(error).__name__, image_sha256=image["sha256"])
            raise
        except (OSError, ValueError, Refused):
            pass  # Passive observation only; never repeat the POST or change the channel.
        time.sleep(2)
    case.record("device", result="unaccepted", image_sha256=image["sha256"], changed_behavior="not_run")
    raise Refused("upload attempt consumed; exact reboot/build/health acceptance is unproven")


def main(argv: list[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments and arguments[0] == "_ci-build-in-container":
        internal = argparse.ArgumentParser(description="Internal target build acceptance; invoke through the pinned wrapper")
        internal.add_argument("--build-dir", required=True, type=Path)
        internal.add_argument("--target", required=True, choices=TARGETS)
        internal.add_argument("--version", required=True)
        selected = internal.parse_args(arguments[1:])
        container_build(selected.build_dir, selected.target, selected.version)
        return 0
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "tests", "build", "bind-image", "review", "check", "preflight", "execute"))
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--inventory", type=Path)
    parser.add_argument("--board")
    parser.add_argument("--version")
    parser.add_argument("--image", type=Path)
    parser.add_argument("--sha256")
    parser.add_argument("--execute", action="store_true", help="explicitly permit the sole OTA POST and passive acceptance")
    parser.add_argument("--case-id", help="exact case named by the user's current upload approval")
    args = parser.parse_args(arguments)
    if args.command == "prepare":
        require(args.inventory is not None and bool(args.board) and bool(args.version), "prepare requires private inventory, board ID and version")
        case = Case.prepare(args.evidence, args.inventory, args.board, args.version)
        print(json.dumps({"case_id": case.plan["case_id"], "source_id": case.plan["source_id"], "target": case.plan["target"]}))
        return 0
    case = Case(args.evidence)
    if args.command in ("tests", "build", "review"):
        getattr(case, args.command)()
    elif args.command == "bind-image":
        require(args.image is not None and args.sha256 is not None, "bind-image requires file and exact SHA-256")
        case.bind_image(args.image, args.sha256)
    elif args.command == "check":
        case.ready()
    elif args.command == "preflight":
        preflight(case)
    else:
        require(args.execute and args.case_id is not None, "upload requires --execute and the approved exact --case-id")
        print(json.dumps(execute(case, args.case_id), sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (Refused, OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        print(f"local-ota: refused ({type(exc).__name__}): {exc}", file=sys.stderr)
        raise SystemExit(1)
