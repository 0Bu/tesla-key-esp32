#!/usr/bin/env python3
"""Create and verify the exact source -> unsigned -> diagnostic artifact inventory.

The producing build is untrusted at the signing boundary.  Its inventory becomes evidence only
after this trusted validator independently recomputes the expected checkout fingerprint, rejects
any path-shape drift, and copies every allowlisted regular file through no-follow file descriptors
into a private signer-owned directory.  The signed-preview workflow has a narrower exception: its
protected job must not check out PR code, so two exact-SHA, secret-free workflow jobs attest the
source identity and the signer compares their complete inventories without executing either one.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from typing import Any


TARGETS = ("esp32", "esp32s3", "esp32c3", "esp32c6")
UNSIGNED_TARGET_FILES = (
    "bootloader/bootloader.bin",
    "partition_table/partition-table.bin",
    "tesla-key-esp32.bin",
    "ota_data_initial.bin",
)
DIST_TARGET_FILES = (
    "tesla-key-esp32-{target}.elf",
    "tesla-key-esp32-{target}.elf.sha256",
    "tesla-key-esp32-{target}.map",
    "sdkconfig.{target}",
    "dependencies.lock.{target}",
    "size-{target}.json",
    "size-{target}.md",
    "projected-signed-size.txt",
    "stack-usage-{target}.json",
)
DIST_GLOBAL_FILES = ("build-metadata.txt",)
MANIFEST_PATH = "dist/build-artifact-inventory.json"
MANIFEST_KIND = "tesla-key-esp32-build-artifact-inventory"
SOURCE_FINGERPRINT_KIND = "git-working-tree-sha256-v1"
# version.txt is a workflow-generated display-version stamp, not commit-tree provenance.  Its
# effective value is independently bound by displayVersion, build-metadata.txt and each app's
# embedded ESP application descriptor.
SOURCE_EXCLUSIONS = ("version.txt",)
EXPECTED_PAYLOAD_COUNT = 53
MAX_FILE_BYTES = 64 * 1024 * 1024
MAX_TOTAL_BYTES = 256 * 1024 * 1024
MAX_MANIFEST_BYTES = 1024 * 1024
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SOURCE_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
VERSION_RE = re.compile(
    r"^(?:local|(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?)$"
)


def payload_paths() -> tuple[str, ...]:
    paths: list[str] = []
    for target in TARGETS:
        paths.extend(f"_unsigned/{target}/{name}" for name in UNSIGNED_TARGET_FILES)
        paths.extend(f"dist/{target}/{name.format(target=target)}" for name in DIST_TARGET_FILES)
    paths.extend(f"dist/{name}" for name in DIST_GLOBAL_FILES)
    return tuple(sorted(paths))


PAYLOAD_PATHS = payload_paths()


class InventoryError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise InventoryError(message)


def clean_relative(path: str) -> str:
    pure = PurePosixPath(path)
    require(
        path == pure.as_posix()
        and not pure.is_absolute()
        and path not in {"", "."}
        and ".." not in pure.parts,
        f"unsafe inventory path: {path!r}",
    )
    return path


def ensure_root(root: Path, label: str, *, empty: bool = False) -> Path:
    absolute = Path(os.path.abspath(root))
    try:
        root_stat = absolute.lstat()
    except OSError as exc:
        raise InventoryError(f"{label} is not accessible: {absolute}: {exc}") from exc
    require(stat.S_ISDIR(root_stat.st_mode) and not stat.S_ISLNK(root_stat.st_mode),
            f"{label} must be a real directory, not a symlink: {absolute}")
    if empty:
        require(not any(absolute.iterdir()), f"{label} must start empty: {absolute}")
    return absolute


def open_root(root: Path) -> int:
    nofollow = getattr(os, "O_NOFOLLOW", 0)
    require(nofollow != 0, "platform lacks O_NOFOLLOW required by signer artifact validation")
    try:
        return os.open(root, os.O_RDONLY | os.O_DIRECTORY | nofollow)
    except OSError as exc:
        raise InventoryError(f"cannot open artifact root without following links: {root}: {exc}") from exc


def open_relative(root_fd: int, relative: str, *, single_link: bool) -> int:
    parts = PurePosixPath(clean_relative(relative)).parts
    directory_fd = os.dup(root_fd)
    nofollow = os.O_NOFOLLOW
    try:
        for component in parts[:-1]:
            next_fd = os.open(
                component,
                os.O_RDONLY | os.O_DIRECTORY | nofollow,
                dir_fd=directory_fd,
            )
            os.close(directory_fd)
            directory_fd = next_fd
        file_fd = os.open(parts[-1], os.O_RDONLY | nofollow, dir_fd=directory_fd)
    except OSError as exc:
        raise InventoryError(f"cannot open without following links: {relative}: {exc}") from exc
    finally:
        os.close(directory_fd)
    info = os.fstat(file_fd)
    if not stat.S_ISREG(info.st_mode) or (single_link and info.st_nlink != 1):
        os.close(file_fd)
        qualifier = "single-link regular file" if single_link else "regular file"
        raise InventoryError(f"inventory leaf must be a {qualifier}: {relative}")
    return file_fd


def digest_fd(file_fd: int, relative: str, maximum: int) -> tuple[int, str]:
    before = os.fstat(file_fd)
    require(before.st_size <= maximum, f"artifact file exceeds {maximum} bytes: {relative}")
    digest = hashlib.sha256()
    total = 0
    while True:
        chunk = os.read(file_fd, 1024 * 1024)
        if not chunk:
            break
        total += len(chunk)
        require(total <= maximum, f"artifact file grew beyond {maximum} bytes: {relative}")
        digest.update(chunk)
    after = os.fstat(file_fd)
    require(
        (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
        == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
        and total == before.st_size,
        f"artifact file changed while hashing: {relative}",
    )
    return total, digest.hexdigest()


def digest_relative(
    root_fd: int, relative: str, *, maximum: int = MAX_FILE_BYTES, single_link: bool = True
) -> tuple[int, str]:
    file_fd = open_relative(root_fd, relative, single_link=single_link)
    try:
        return digest_fd(file_fd, relative, maximum)
    finally:
        os.close(file_fd)


def read_relative(root_fd: int, relative: str, maximum: int) -> bytes:
    file_fd = open_relative(root_fd, relative, single_link=True)
    try:
        before = os.fstat(file_fd)
        require(before.st_size <= maximum, f"control file exceeds {maximum} bytes: {relative}")
        chunks: list[bytes] = []
        total = 0
        while True:
            chunk = os.read(file_fd, min(1024 * 1024, maximum + 1 - total))
            if not chunk:
                break
            chunks.append(chunk)
            total += len(chunk)
            require(total <= maximum, f"control file exceeds {maximum} bytes: {relative}")
        after = os.fstat(file_fd)
        require(
            (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
            == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
            and total == before.st_size,
            f"control file changed while reading: {relative}",
        )
        return b"".join(chunks)
    finally:
        os.close(file_fd)


def expected_directories(paths: set[str]) -> set[str]:
    directories: set[str] = set()
    for path in paths:
        parent = PurePosixPath(path).parent
        while parent.as_posix() not in {"", "."}:
            directories.add(parent.as_posix())
            parent = parent.parent
    return directories


def scan_exact_artifact(root: Path, *, include_manifest: bool) -> None:
    expected_files = set(PAYLOAD_PATHS)
    if include_manifest:
        expected_files.add(MANIFEST_PATH)
    expected_dirs = expected_directories(expected_files)
    actual_files: set[str] = set()
    actual_dirs: set[str] = set()
    for base in ("_unsigned", "dist"):
        base_path = root / base
        try:
            base_stat = base_path.lstat()
        except OSError as exc:
            raise InventoryError(f"artifact subtree is missing: {base}: {exc}") from exc
        require(stat.S_ISDIR(base_stat.st_mode) and not stat.S_ISLNK(base_stat.st_mode),
                f"artifact subtree root must be a real directory: {base}")
        for directory, names, files in os.walk(base_path, topdown=True, followlinks=False):
            directory_path = Path(directory)
            relative_dir = directory_path.relative_to(root).as_posix()
            actual_dirs.add(relative_dir)
            for name in list(names):
                child = directory_path / name
                info = child.lstat()
                require(stat.S_ISDIR(info.st_mode) and not stat.S_ISLNK(info.st_mode),
                        f"artifact path ancestor must be a real directory: {child.relative_to(root)}")
            for name in files:
                child = directory_path / name
                info = child.lstat()
                relative = child.relative_to(root).as_posix()
                require(stat.S_ISREG(info.st_mode) and not stat.S_ISLNK(info.st_mode),
                        f"artifact leaf must be regular and not symlinked: {relative}")
                require(info.st_nlink == 1, f"artifact leaf must not be hard-linked: {relative}")
                actual_files.add(relative)
    require(
        actual_dirs == expected_dirs,
        "artifact directory inventory drifted: "
        f"missing={sorted(expected_dirs - actual_dirs)} extra={sorted(actual_dirs - expected_dirs)}",
    )
    require(
        actual_files == expected_files,
        "artifact file inventory drifted: "
        f"missing={sorted(expected_files - actual_files)} extra={sorted(actual_files - expected_files)}",
    )


def git_output(source_root: Path, *args: str) -> bytes:
    command = ["git", "-c", "core.fsmonitor=false", "-C", str(source_root), *args]
    try:
        return subprocess.check_output(command, stderr=subprocess.PIPE)
    except (OSError, subprocess.CalledProcessError) as exc:
        detail = getattr(exc, "stderr", b"").decode("utf-8", "replace").strip()
        raise InventoryError(f"cannot inspect source checkout with git: {detail or exc}") from exc


def source_state(source_root: Path, expected_source_sha: str) -> dict[str, Any]:
    require(SOURCE_SHA_RE.fullmatch(expected_source_sha) is not None,
            "expected source SHA must be 40 lowercase hexadecimal characters")
    source_root = ensure_root(source_root, "source root")
    actual_commit = git_output(source_root, "rev-parse", "HEAD").decode("ascii").strip()
    require(actual_commit == expected_source_sha,
            f"source checkout commit mismatch: expected={expected_source_sha} actual={actual_commit}")
    raw_paths = [
        item
        for item in git_output(
            source_root, "ls-files", "--cached", "--others", "--exclude-standard", "-z"
        ).split(b"\0")
        if item and os.fsdecode(item) not in SOURCE_EXCLUSIONS
    ]
    require(len(raw_paths) == len(set(raw_paths)), "source checkout contains duplicate path records")
    digest = hashlib.sha256(b"tesla-key-source-tree-sha256-v1\0")
    root_fd = open_root(source_root)
    total_bytes = 0
    try:
        for raw_path in sorted(raw_paths):
            relative = os.fsdecode(raw_path)
            clean_relative(relative)
            size, file_digest = digest_relative(
                root_fd, relative, maximum=MAX_FILE_BYTES, single_link=False
            )
            file_info = (source_root / relative).lstat()
            require(stat.S_ISREG(file_info.st_mode) and not stat.S_ISLNK(file_info.st_mode),
                    f"source path must be a regular file, not a symlink: {relative}")
            executable = b"x" if file_info.st_mode & 0o111 else b"-"
            digest.update(len(raw_path).to_bytes(8, "big"))
            digest.update(raw_path)
            digest.update(executable)
            digest.update(size.to_bytes(8, "big"))
            digest.update(bytes.fromhex(file_digest))
            total_bytes += size
    finally:
        os.close(root_fd)
    return {
        "commit": actual_commit,
        "fingerprintKind": SOURCE_FINGERPRINT_KIND,
        "sha256": digest.hexdigest(),
        "fileCount": len(raw_paths),
        "totalBytes": total_bytes,
        "excludedGeneratedPaths": list(SOURCE_EXCLUSIONS),
    }


def canonical_bytes(document: dict[str, Any]) -> bytes:
    return (
        json.dumps(document, ensure_ascii=True, sort_keys=True, separators=(",", ":")) + "\n"
    ).encode("utf-8")


def inventory_document(
    artifact_root: Path, source: dict[str, Any], version: str
) -> dict[str, Any]:
    require(len(PAYLOAD_PATHS) == EXPECTED_PAYLOAD_COUNT,
            "internal artifact payload allowlist count drifted")
    root_fd = open_root(artifact_root)
    files: list[dict[str, Any]] = []
    total = 0
    try:
        for relative in PAYLOAD_PATHS:
            size, digest = digest_relative(root_fd, relative)
            total += size
            require(total <= MAX_TOTAL_BYTES,
                    f"artifact payload exceeds {MAX_TOTAL_BYTES} bytes")
            files.append({"path": relative, "sha256": digest, "size": size})
    finally:
        os.close(root_fd)
    return {
        "schemaVersion": 1,
        "manifestKind": MANIFEST_KIND,
        "displayVersion": version,
        "targets": list(TARGETS),
        "source": source,
        "files": files,
    }


def write_inventory(
    artifact_root: Path, source_root: Path, expected_source_sha: str, version: str
) -> dict[str, Any]:
    require(
        VERSION_RE.fullmatch(version) is not None and len(version) <= 31,
        f"invalid display version: {version!r}",
    )
    artifact_root = ensure_root(artifact_root, "artifact root")
    scan_exact_artifact(artifact_root, include_manifest=False)
    source = source_state(source_root, expected_source_sha)
    document = inventory_document(artifact_root, source, version)
    encoded = canonical_bytes(document)
    require(len(encoded) <= MAX_MANIFEST_BYTES, "generated artifact inventory is unexpectedly large")
    manifest = artifact_root / MANIFEST_PATH
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
    try:
        file_fd = os.open(manifest, flags, 0o644)
        try:
            view = memoryview(encoded)
            while view:
                written = os.write(file_fd, view)
                require(written > 0, "short write while creating artifact inventory")
                view = view[written:]
            os.fsync(file_fd)
        finally:
            os.close(file_fd)
    except OSError as exc:
        raise InventoryError(f"cannot create canonical artifact inventory: {exc}") from exc
    scan_exact_artifact(artifact_root, include_manifest=True)
    return document


def parse_manifest(raw: bytes) -> dict[str, Any]:
    try:
        document = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise InventoryError(f"artifact inventory is not valid UTF-8 JSON: {exc}") from exc
    require(isinstance(document, dict), "artifact inventory root must be an object")
    require(raw == canonical_bytes(document), "artifact inventory is not canonical JSON")
    require(
        set(document) == {
            "schemaVersion", "manifestKind", "displayVersion", "targets", "source", "files"
        },
        "artifact inventory top-level fields drifted",
    )
    require(document["schemaVersion"] == 1 and document["manifestKind"] == MANIFEST_KIND,
            "artifact inventory schema/kind drifted")
    require(document["targets"] == list(TARGETS),
            "artifact inventory target set/order must be exactly the four supported targets")
    source = document["source"]
    require(
        isinstance(source, dict)
        and set(source)
        == {
            "commit", "fingerprintKind", "sha256", "fileCount", "totalBytes",
            "excludedGeneratedPaths",
        },
        "artifact inventory source fields drifted",
    )
    require(source["fingerprintKind"] == SOURCE_FINGERPRINT_KIND,
            "artifact inventory source fingerprint kind drifted")
    require(SOURCE_SHA_RE.fullmatch(source["commit"]) is not None,
            "artifact inventory source commit is invalid")
    require(SHA256_RE.fullmatch(source["sha256"]) is not None,
            "artifact inventory source fingerprint is invalid")
    require(source["excludedGeneratedPaths"] == list(SOURCE_EXCLUSIONS),
            "artifact inventory source exclusions drifted")
    for key in ("fileCount", "totalBytes"):
        require(isinstance(source[key], int) and not isinstance(source[key], bool) and source[key] >= 0,
                f"artifact inventory source {key} is invalid")
    files = document["files"]
    require(
        len(PAYLOAD_PATHS) == EXPECTED_PAYLOAD_COUNT
        and isinstance(files, list)
        and len(files) == EXPECTED_PAYLOAD_COUNT,
            "artifact inventory file-record count drifted")
    require([entry.get("path") for entry in files if isinstance(entry, dict)] == list(PAYLOAD_PATHS),
            "artifact inventory paths/order must match the exact allowlist")
    for entry in files:
        require(isinstance(entry, dict) and set(entry) == {"path", "sha256", "size"},
                "artifact inventory file record fields drifted")
        require(SHA256_RE.fullmatch(entry["sha256"]) is not None,
                f"artifact inventory digest is invalid: {entry.get('path')!r}")
        require(
            isinstance(entry["size"], int)
            and not isinstance(entry["size"], bool)
            and 0 < entry["size"] <= MAX_FILE_BYTES,
            f"artifact inventory size is invalid: {entry.get('path')!r}",
        )
    return document


def verify_inventory(
    artifact_root: Path,
    source_root: Path | None,
    expected_source_sha: str,
    version: str,
    *,
    accept_workflow_attested_source: bool = False,
) -> dict[str, Any]:
    require(
        VERSION_RE.fullmatch(version) is not None and len(version) <= 31,
        f"invalid display version: {version!r}",
    )
    artifact_root = ensure_root(artifact_root, "artifact root")
    scan_exact_artifact(artifact_root, include_manifest=True)
    root_fd = open_root(artifact_root)
    try:
        raw = read_relative(root_fd, MANIFEST_PATH, MAX_MANIFEST_BYTES)
    finally:
        os.close(root_fd)
    document = parse_manifest(raw)
    require(document["displayVersion"] == version,
            "artifact inventory display version differs from trusted expectation")
    if accept_workflow_attested_source:
        require(
            document["source"]["commit"] == expected_source_sha,
            "workflow-attested source commit differs from the exact expected PR head",
        )
        trusted_source = document["source"]
    else:
        require(source_root is not None,
                "source root is required without exact workflow source attestation")
        trusted_source = source_state(source_root, expected_source_sha)
        require(
            document["source"] == trusted_source,
            "builder-claimed source fingerprint differs from the independently inspected checkout",
        )
    expected_document = inventory_document(artifact_root, trusted_source, version)
    require(document == expected_document,
            "artifact payload size/digest differs from its canonical inventory")
    return document


def compare_relative(left_fd: int, right_fd: int, relative: str, maximum: int) -> None:
    left = open_relative(left_fd, relative, single_link=True)
    right = open_relative(right_fd, relative, single_link=True)
    left_before = os.fstat(left)
    right_before = os.fstat(right)
    require(left_before.st_size == right_before.st_size,
            f"independent artifact size differs: {relative}")
    require(left_before.st_size <= maximum,
            f"independent artifact exceeds comparison bound: {relative}")
    total = 0
    try:
        while True:
            left_chunk = os.read(left, 1024 * 1024)
            right_chunk = os.read(right, 1024 * 1024)
            require(left_chunk == right_chunk,
                    f"independent artifact bytes differ: {relative}")
            if not left_chunk:
                break
            total += len(left_chunk)
            require(total <= maximum,
                    f"independent artifact grew beyond comparison bound: {relative}")
        left_after = os.fstat(left)
        right_after = os.fstat(right)
        require(
            (left_before.st_dev, left_before.st_ino, left_before.st_size, left_before.st_mtime_ns)
            == (left_after.st_dev, left_after.st_ino, left_after.st_size, left_after.st_mtime_ns)
            and (right_before.st_dev, right_before.st_ino, right_before.st_size,
                 right_before.st_mtime_ns)
            == (right_after.st_dev, right_after.st_ino, right_after.st_size,
                right_after.st_mtime_ns)
            and total == left_before.st_size,
            f"independent artifact changed during byte comparison: {relative}",
        )
    finally:
        os.close(left)
        os.close(right)


def compare_inventories(
    artifact_root: Path,
    comparison_root: Path,
    source_root: Path | None,
    expected_source_sha: str,
    version: str,
    *,
    accept_workflow_attested_source: bool = False,
) -> dict[str, Any]:
    artifact_root = ensure_root(artifact_root, "primary artifact root")
    comparison_root = ensure_root(comparison_root, "independent artifact root")
    require(not os.path.samefile(artifact_root, comparison_root),
            "primary and independent artifact roots must be distinct directories")
    primary = verify_inventory(
        artifact_root,
        source_root,
        expected_source_sha,
        version,
        accept_workflow_attested_source=accept_workflow_attested_source,
    )
    independent = verify_inventory(
        comparison_root,
        source_root,
        expected_source_sha,
        version,
        accept_workflow_attested_source=accept_workflow_attested_source,
    )
    require(primary == independent,
            "primary and independent canonical artifact inventories differ")
    left_fd = open_root(artifact_root)
    right_fd = open_root(comparison_root)
    try:
        for relative in PAYLOAD_PATHS:
            compare_relative(left_fd, right_fd, relative, MAX_FILE_BYTES)
        compare_relative(left_fd, right_fd, MANIFEST_PATH, MAX_MANIFEST_BYTES)
    finally:
        os.close(left_fd)
        os.close(right_fd)
    return primary


def copy_file_no_follow(source_fd: int, destination: Path, relative: str) -> None:
    input_fd = open_relative(source_fd, relative, single_link=True)
    destination_path = destination / relative
    destination_path.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
    try:
        output_fd = os.open(destination_path, flags, 0o600)
    except OSError as exc:
        os.close(input_fd)
        raise InventoryError(f"cannot create private-stage file {relative}: {exc}") from exc
    digest = hashlib.sha256()
    total = 0
    before = os.fstat(input_fd)
    try:
        while True:
            chunk = os.read(input_fd, 1024 * 1024)
            if not chunk:
                break
            total += len(chunk)
            require(total <= MAX_FILE_BYTES, f"artifact file grew while copying: {relative}")
            digest.update(chunk)
            view = memoryview(chunk)
            while view:
                written = os.write(output_fd, view)
                require(written > 0, f"short write while staging artifact file: {relative}")
                view = view[written:]
        os.fsync(output_fd)
    finally:
        os.close(input_fd)
        os.close(output_fd)
    after = destination_path.stat()
    require(
        total == before.st_size == after.st_size,
        f"artifact size changed during private-stage copy: {relative}",
    )


def copy_verified_inventory(
    artifact_root: Path,
    destination: Path,
    source_root: Path | None,
    expected_source_sha: str,
    version: str,
    comparison_root: Path | None = None,
    *,
    accept_workflow_attested_source: bool = False,
) -> dict[str, Any]:
    if comparison_root is None:
        document = verify_inventory(
            artifact_root,
            source_root,
            expected_source_sha,
            version,
            accept_workflow_attested_source=accept_workflow_attested_source,
        )
    else:
        document = compare_inventories(
            artifact_root,
            comparison_root,
            source_root,
            expected_source_sha,
            version,
            accept_workflow_attested_source=accept_workflow_attested_source,
        )
    destination = ensure_root(destination, "private signer stage", empty=True)
    source_fd = open_root(ensure_root(artifact_root, "artifact root"))
    try:
        for relative in (*PAYLOAD_PATHS, MANIFEST_PATH):
            copy_file_no_follow(source_fd, destination, relative)
    finally:
        os.close(source_fd)
    # This second full validation rehashes the private copy and closes the pre-copy/post-copy gap.
    copied = verify_inventory(
        destination,
        source_root,
        expected_source_sha,
        version,
        accept_workflow_attested_source=accept_workflow_attested_source,
    )
    require(copied == document, "private signer stage inventory differs after verified copy")
    return copied


def manifest_fingerprint(document: dict[str, Any]) -> str:
    return hashlib.sha256(canonical_bytes(document)).hexdigest()


def fixture_artifact(root: Path) -> None:
    for index, relative in enumerate(PAYLOAD_PATHS):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(f"fixture:{index}:{relative}\n".encode("utf-8"))


def self_test(source_root: Path) -> None:
    source_sha = git_output(ensure_root(source_root, "source root"), "rev-parse", "HEAD").decode().strip()
    try:
        write_inventory(Path("."), source_root, source_sha, "01.2.3")
    except InventoryError:
        pass
    else:
        raise AssertionError("self-test accepted non-canonical leading-zero version")
    with tempfile.TemporaryDirectory(prefix="build-inventory-") as directory:
        base = Path(directory)
        artifact = base / "artifact"
        artifact.mkdir()
        fixture_artifact(artifact)
        document = write_inventory(artifact, source_root, source_sha, "1.2.3-test")
        assert verify_inventory(artifact, source_root, source_sha, "1.2.3-test") == document
        private = base / "private"
        private.mkdir()
        assert copy_verified_inventory(
            artifact, private, source_root, source_sha, "1.2.3-test"
        ) == document
        assert compare_inventories(
            artifact, private, source_root, source_sha, "1.2.3-test"
        ) == document
        workflow_private = base / "workflow-private"
        workflow_private.mkdir()
        assert copy_verified_inventory(
            artifact,
            workflow_private,
            None,
            source_sha,
            "1.2.3-test",
            private,
            accept_workflow_attested_source=True,
        ) == document
        try:
            compare_inventories(
                artifact,
                private,
                None,
                "f" * 40,
                "1.2.3-test",
                accept_workflow_attested_source=True,
            )
        except InventoryError as exc:
            assert "workflow-attested source commit" in str(exc)
        else:
            raise AssertionError("workflow attestation accepted the wrong exact PR head")

        def rejected(mutator: Any, expected: str) -> None:
            mutated = base / "mutated"
            if mutated.exists():
                shutil.rmtree(mutated)
            shutil.copytree(artifact, mutated)
            mutator(mutated)
            try:
                verify_inventory(mutated, source_root, source_sha, "1.2.3-test")
            except InventoryError as exc:
                assert expected in str(exc), (expected, str(exc))
            else:
                raise AssertionError(f"artifact inventory mutation was accepted: {expected}")

        rejected(lambda root: (root / "dist/extra.bin").write_bytes(b"extra"), "extra=")
        rejected(lambda root: (root / "dist/extra-dir").mkdir(), "directory inventory drifted")
        rejected(lambda root: (root / PAYLOAD_PATHS[0]).unlink(), "missing=")

        def rename(root: Path) -> None:
            (root / PAYLOAD_PATHS[1]).rename(root / "_unsigned/esp32/renamed.bin")

        rejected(rename, "inventory drifted")

        def swap(root: Path) -> None:
            left = root / "_unsigned/esp32/tesla-key-esp32.bin"
            right = root / "_unsigned/esp32c6/tesla-key-esp32.bin"
            first, second = left.read_bytes(), right.read_bytes()
            left.write_bytes(second)
            right.write_bytes(first)

        rejected(swap, "size/digest")

        def tamper(root: Path) -> None:
            path = root / "dist/esp32/size-esp32.json"
            path.write_bytes(path.read_bytes() + b"x")

        rejected(tamper, "size/digest")

        def noncanonical(root: Path) -> None:
            path = root / MANIFEST_PATH
            path.write_bytes(b" " + path.read_bytes())

        rejected(noncanonical, "not canonical")

        def leaf_symlink(root: Path) -> None:
            path = root / PAYLOAD_PATHS[2]
            path.unlink()
            path.symlink_to("ota_data_initial.bin")

        rejected(leaf_symlink, "not symlinked")

        def ancestor_symlink(root: Path) -> None:
            path = root / "_unsigned/esp32/bootloader"
            shutil.rmtree(path)
            path.symlink_to(root / "_unsigned/esp32c6/bootloader", target_is_directory=True)

        rejected(ancestor_symlink, "ancestor must be a real directory")

        compare_bad = base / "compare-bad"
        shutil.copytree(artifact, compare_bad)
        compare_path = compare_bad / "_unsigned/esp32c3/tesla-key-esp32.bin"
        compare_path.write_bytes(compare_path.read_bytes() + b"changed")
        compare_document = json.loads((compare_bad / MANIFEST_PATH).read_text(encoding="utf-8"))
        for entry in compare_document["files"]:
            if entry["path"] == "_unsigned/esp32c3/tesla-key-esp32.bin":
                entry["size"] = compare_path.stat().st_size
                entry["sha256"] = hashlib.sha256(compare_path.read_bytes()).hexdigest()
        (compare_bad / MANIFEST_PATH).write_bytes(canonical_bytes(compare_document))
        try:
            compare_inventories(
                artifact, compare_bad, source_root, source_sha, "1.2.3-test"
            )
        except InventoryError as exc:
            assert "inventories differ" in str(exc)
        else:
            raise AssertionError("self-consistent independent-build byte drift was accepted")

        linked_root = base / "linked-root"
        linked_root.symlink_to(artifact, target_is_directory=True)
        try:
            verify_inventory(linked_root, source_root, source_sha, "1.2.3-test")
        except InventoryError as exc:
            assert "not a symlink" in str(exc)
        else:
            raise AssertionError("symlinked artifact root was accepted")

        try:
            verify_inventory(artifact, source_root, "f" * 40, "1.2.3-test")
        except InventoryError as exc:
            assert "commit mismatch" in str(exc)
        else:
            raise AssertionError("wrong trusted source commit was accepted")
    print(
        "build artifact inventory self-test: PASS "
        "(shape, source, swap, tamper, root/ancestor/leaf symlink, copy)"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--verify", action="store_true")
    mode.add_argument("--copy-to", type=Path)
    parser.add_argument("--compare-to", type=Path)
    parser.add_argument("--artifact-root", type=Path, default=Path.cwd())
    parser.add_argument("--source-root", type=Path, default=Path.cwd())
    parser.add_argument("--accept-workflow-attested-source", action="store_true")
    parser.add_argument("--expected-source-sha")
    parser.add_argument("--version")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            self_test(args.source_root)
            return 0
        if not (args.write or args.verify or args.copy_to is not None):
            parser.error("one of --write, --verify or --copy-to is required")
        if args.expected_source_sha is None or args.version is None:
            parser.error("--expected-source-sha and --version are required")
        if args.write and args.compare_to is not None:
            parser.error("--compare-to is valid only with --verify or --copy-to")
        if args.accept_workflow_attested_source and (
            args.write or args.compare_to is None
        ):
            parser.error(
                "--accept-workflow-attested-source requires two compared verify/copy artifacts"
            )
        if args.write:
            document = write_inventory(
                args.artifact_root, args.source_root, args.expected_source_sha, args.version
            )
            action = "created"
        elif args.copy_to is not None:
            document = copy_verified_inventory(
                args.artifact_root,
                args.copy_to,
                args.source_root,
                args.expected_source_sha,
                args.version,
                args.compare_to,
                accept_workflow_attested_source=args.accept_workflow_attested_source,
            )
            action = (
                "independently compared + privately staged"
                if args.compare_to is not None
                else "verified + privately staged"
            )
        else:
            if args.compare_to is not None:
                document = compare_inventories(
                    args.artifact_root,
                    args.compare_to,
                    args.source_root,
                    args.expected_source_sha,
                    args.version,
                    accept_workflow_attested_source=args.accept_workflow_attested_source,
                )
                action = "independently byte-compared"
            else:
                document = verify_inventory(
                    args.artifact_root, args.source_root, args.expected_source_sha, args.version
                )
                action = "verified"
    except (InventoryError, OSError, UnicodeError, ValueError) as exc:
        print(f"build artifact inventory failed: {exc}", file=sys.stderr)
        return 1
    print(
        f"build artifact inventory: PASS ({action}, {len(document['files'])} payload files, "
        f"source={document['source']['sha256']}, manifest={manifest_fingerprint(document)})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
