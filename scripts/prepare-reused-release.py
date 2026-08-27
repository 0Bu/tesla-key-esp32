#!/usr/bin/env python3
"""Validate one immutable Release download and stage it for Pages without signing.

The GitHub Release directory is treated as inert, attacker-controlled bytes.  The validator binds
all 40 assets to immutable REST metadata, compares all 28 diagnostics to the current independent
build, proves each signed app's target/version/prefix/size, RSA-PSS signature and pinned production
key, validates the complete merged-image layout, then stages both the four per-target installer
parts and the twelve root signed/merged files used by the successful-run recovery artifact.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import stat
import struct
import sys
import tempfile
import zlib
from pathlib import Path
from types import ModuleType
from typing import Any, Callable


TARGETS = (
    ("esp32", "", 0x1000),
    ("esp32s3", "-s3", 0x0),
    ("esp32c3", "-c3", 0x0),
    ("esp32c6", "-c6", 0x0),
)
DIAGNOSTIC_NAMES = (
    "tesla-key-esp32-{target}.elf",
    "tesla-key-esp32-{target}.elf.sha256",
    "tesla-key-esp32-{target}.map",
    "sdkconfig.{target}",
    "dependencies.lock.{target}",
    "size-{target}.json",
    "size-{target}.md",
)
SIGNATURE_ALIGNMENT = 0x10000
SIGNATURE_SECTOR = 0x1000
APP_OFFSET = 0x20000
PARTITION_OFFSET = 0x8000
OTADATA_OFFSET = 0xF000
MAX_ASSET_BYTES = 64 * 1024 * 1024
PRODUCTION_KEY_DIGEST_PATH = Path(__file__).resolve().with_name(
    "ota-signing-public-key.sha256"
)


class ReuseError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ReuseError(message)


def load_sibling(filename: str, module_name: str) -> ModuleType:
    path = Path(__file__).resolve().with_name(filename)
    require(path.is_file() and not path.is_symlink(), f"missing/unsafe validator: {path}")
    spec = importlib.util.spec_from_file_location(module_name, path)
    require(spec is not None and spec.loader is not None, f"cannot load validator: {path}")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


RELEASE_CONTRACT = load_sibling("check-release-assets.py", "tesla_release_assets")
FIRMWARE_CONTRACT = load_sibling("check-firmware-artifacts.py", "tesla_firmware_artifacts")
SIGNED_ROOT_CONTRACT = load_sibling(
    "check-signed-root-inventory.py", "tesla_signed_root_inventory"
)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def safe_directory(path: Path, label: str) -> Path:
    absolute = Path(os.path.abspath(path))
    try:
        info = absolute.lstat()
    except OSError as exc:
        raise ReuseError(f"{label} is not accessible: {absolute}: {exc}") from exc
    require(stat.S_ISDIR(info.st_mode) and not stat.S_ISLNK(info.st_mode),
            f"{label} must be a real directory: {absolute}")
    return absolute


def safe_bytes(path: Path, label: str) -> bytes:
    """Read a current-build input defensively.

    Release downloads use the stricter directory-fd snapshot boundary below.  This helper remains
    for signer-owned current-build inputs, which are independently compared with that snapshot.
    """
    try:
        before = path.lstat()
    except OSError as exc:
        raise ReuseError(f"{label} is missing: {path}: {exc}") from exc
    require(stat.S_ISREG(before.st_mode) and not stat.S_ISLNK(before.st_mode),
            f"{label} must be a regular non-symlink: {path}")
    require(before.st_nlink == 1, f"{label} must not be hard-linked: {path}")
    require(0 < before.st_size <= MAX_ASSET_BYTES,
            f"{label} has invalid size {before.st_size}: {path}")
    data = path.read_bytes()
    after = path.lstat()
    require(
        (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
        == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
        and len(data) == before.st_size,
        f"{label} changed while being read: {path}",
    )
    return data


def _stat_identity(info: os.stat_result) -> tuple[int, int, int, int, int, int]:
    return (
        info.st_dev,
        info.st_ino,
        info.st_size,
        info.st_mtime_ns,
        info.st_ctime_ns,
        info.st_nlink,
    )


def read_snapshot_asset(directory_fd: int, name: str) -> bytes:
    """Open one allowlisted Release asset once, without following links, and freeze its bytes."""
    require(hasattr(os, "O_NOFOLLOW"), "platform has no fail-closed O_NOFOLLOW support")
    label = f"downloaded Release asset {name}"
    try:
        named = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
    except OSError as exc:
        raise ReuseError(f"{label} is missing: {name}: {exc}") from exc
    require(stat.S_ISREG(named.st_mode), f"{label} must be a regular non-symlink: {name}")
    require(named.st_nlink == 1, f"{label} must not be hard-linked: {name}")
    require(
        0 < named.st_size <= MAX_ASSET_BYTES,
        f"{label} has invalid size {named.st_size}: {name}",
    )
    flags = os.O_RDONLY | os.O_NOFOLLOW | getattr(os, "O_CLOEXEC", 0)
    try:
        asset_fd = os.open(name, flags, dir_fd=directory_fd)
    except OSError as exc:
        raise ReuseError(f"{label} cannot be opened safely: {name}: {exc}") from exc
    try:
        opened = os.fstat(asset_fd)
        require(
            stat.S_ISREG(opened.st_mode) and opened.st_nlink == 1,
            f"{label} changed before its no-follow open: {name}",
        )
        require(
            _stat_identity(named) == _stat_identity(opened),
            f"{label} changed before its no-follow open: {name}",
        )
        chunks: list[bytes] = []
        remaining = opened.st_size + 1
        while remaining:
            chunk = os.read(asset_fd, min(1024 * 1024, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        data = b"".join(chunks)
        after = os.fstat(asset_fd)
        require(
            _stat_identity(opened) == _stat_identity(after) and len(data) == opened.st_size,
            f"{label} changed while its immutable snapshot was read: {name}",
        )
        return data
    finally:
        os.close(asset_fd)


def expected_flat_names(version: str) -> set[str]:
    names: set[str] = set()
    for target, suffix, _boot_offset in TARGETS:
        names.update(
            {
                f"tesla-key-esp32{suffix}.bin",
                f"tesla-key-esp32{suffix}-{version}.bin",
                f"tesla-key-esp32{suffix}-{version}-merged.bin",
            }
        )
        names.update(pattern.format(target=target) for pattern in DIAGNOSTIC_NAMES)
    require(len(names) == 40, "internal immutable Release inventory count drifted")
    return names


def snapshot_flat_download(download: Path, version: str) -> dict[str, bytes]:
    """Take the sole immutable byte snapshot of the exact flat Release download."""
    download = safe_directory(download, "Release download directory")
    require(hasattr(os, "O_NOFOLLOW"), "platform has no fail-closed O_NOFOLLOW support")
    directory_flags = (
        os.O_RDONLY
        | os.O_NOFOLLOW
        | getattr(os, "O_DIRECTORY", 0)
        | getattr(os, "O_CLOEXEC", 0)
    )
    try:
        directory_fd = os.open(download, directory_flags)
    except OSError as exc:
        raise ReuseError(f"Release download directory cannot be opened safely: {download}: {exc}") from exc
    try:
        opened = os.fstat(directory_fd)
        require(
            stat.S_ISDIR(opened.st_mode),
            f"Release download directory must remain a real directory: {download}",
        )
        names = os.listdir(directory_fd)
        actual = set(names)
        expected = expected_flat_names(version)
        require(len(actual) == len(names), "Release download contains duplicate path names")
        require(
            actual == expected,
            "downloaded Release asset inventory drifted: "
            f"missing={sorted(expected - actual)} extra={sorted(actual - expected)}",
        )
        return {name: read_snapshot_asset(directory_fd, name) for name in sorted(expected)}
    finally:
        os.close(directory_fd)


def validate_remote_bytes(
    release: Any,
    snapshot: dict[str, bytes],
    version: str,
    source_sha: str,
) -> None:
    try:
        metadata = RELEASE_CONTRACT.validate_full_metadata(
            release, version, source_sha, "published-immutable"
        )
    except RELEASE_CONTRACT.ReleaseError as exc:
        raise ReuseError(str(exc)) from exc
    require(set(metadata) == set(snapshot), "Release metadata/download name sets differ")
    for name, data in snapshot.items():
        asset = metadata[name]
        require(asset.get("size") == len(data), f"Release asset size differs: {name}")
        require(
            asset.get("digest") == f"sha256:{sha256_bytes(data)}",
            f"Release asset SHA-256 differs: {name}",
        )


def require_erased_gap(merged: bytes, start: int, end: int, target: str) -> None:
    require(start <= end <= len(merged), f"{target} merged layout range is invalid")
    bad = next((index for index, value in enumerate(merged[start:end]) if value != 0xFF), None)
    if bad is not None:
        raise ReuseError(
            f"{target} undeclared merged byte at 0x{start + bad:x} is not erased 0xff"
        )


def validate_elf_checksum(target: str, elf: bytes, checksum: bytes) -> None:
    try:
        text = checksum.decode("ascii")
    except UnicodeDecodeError as exc:
        raise ReuseError(f"{target} .elf.sha256 is not ASCII") from exc
    name = f"tesla-key-esp32-{target}.elf"
    expected = f"{sha256_bytes(elf)}  dist/{target}/{name}\n"
    require(text == expected, f"{target} .elf.sha256 does not bind the ELF bytes")


def validate_signed_firmware_bytes(
    target: str,
    version: str,
    bootloader: bytes,
    signed_app: bytes,
    expected_public_key_digest: bytes,
) -> None:
    """Apply the shared firmware identity/signature contract directly to snapshot bytes."""
    try:
        boot_info = FIRMWARE_CONTRACT.parse_image(
            bootloader,
            signed=False,
            expect_app_desc=False,
        )
        app_info = FIRMWARE_CONTRACT.parse_image(
            signed_app,
            signed=True,
            expect_app_desc=True,
            expected_public_key_digest=expected_public_key_digest,
        )
    except FIRMWARE_CONTRACT.ArtifactError as exc:
        raise ReuseError(f"{target} signed firmware identity failed: {exc}") from exc
    expected_chip = FIRMWARE_CONTRACT.CHIP_IDS[target]
    require(
        boot_info.chip_id == expected_chip,
        f"{target} signed firmware identity failed: {target} bootloader advertises chip ID "
        f"{boot_info.chip_id:#x}, expected {expected_chip:#x}",
    )
    require(
        app_info.chip_id == expected_chip,
        f"{target} signed firmware identity failed: {target} app advertises chip ID "
        f"{app_info.chip_id:#x}, expected {expected_chip:#x}",
    )
    require(
        app_info.version == version,
        f"{target} signed firmware identity failed: {target} app version mismatch: "
        f"expected {version!r}, got {app_info.version!r}",
    )
    require(
        app_info.project == "tesla-key-esp32",
        f"{target} signed firmware identity failed: {target} app project mismatch: "
        f"expected 'tesla-key-esp32', got {app_info.project!r}",
    )


def validate_and_stage(
    release: Any,
    download: Path,
    artifact_root: Path,
    output: Path,
    version: str,
    source_sha: str,
    expected_public_key_digest: bytes | None = None,
    after_snapshot_validation: Callable[[], None] | None = None,
) -> int:
    artifact_root = safe_directory(artifact_root, "current build artifact root")
    require(
        Path(os.path.abspath(output)) == artifact_root / "_fw",
        "reuse output must be the exact _fw child of the current artifact root",
    )
    require(not output.exists() and not output.is_symlink(),
            f"reuse output must not already exist: {output}")
    try:
        SIGNED_ROOT_CONTRACT.require_empty(artifact_root)
    except SIGNED_ROOT_CONTRACT.InventoryError as exc:
        raise ReuseError(f"reuse root pre-stage inventory failed: {exc}") from exc
    snapshot = snapshot_flat_download(download, version)
    validate_remote_bytes(release, snapshot, version, source_sha)
    if after_snapshot_validation is not None:
        after_snapshot_validation()
    if expected_public_key_digest is None:
        try:
            expected_public_key_digest = FIRMWARE_CONTRACT.read_public_key_digest(
                PRODUCTION_KEY_DIGEST_PATH
            )
        except FIRMWARE_CONTRACT.ArtifactError as exc:
            raise ReuseError(f"production signing authority failed: {exc}") from exc

    staged: dict[str, dict[str, bytes]] = {}
    staged_root: dict[str, bytes] = {}
    for target, suffix, boot_offset in TARGETS:
        unsigned_dir = artifact_root / "_unsigned" / target
        boot = safe_bytes(unsigned_dir / "bootloader" / "bootloader.bin",
                          f"current {target} bootloader")
        partition = safe_bytes(
            unsigned_dir / "partition_table" / "partition-table.bin",
            f"current {target} partition table",
        )
        unsigned_app = safe_bytes(unsigned_dir / "tesla-key-esp32.bin",
                                  f"current {target} unsigned app")
        otadata = safe_bytes(unsigned_dir / "ota_data_initial.bin", f"current {target} otadata")

        unversioned_name = f"tesla-key-esp32{suffix}.bin"
        versioned_name = f"tesla-key-esp32{suffix}-{version}.bin"
        merged_name = f"tesla-key-esp32{suffix}-{version}-merged.bin"
        signed_app = snapshot[unversioned_name]
        require(
            snapshot[versioned_name] == signed_app,
            f"{target} unversioned and versioned signed app aliases differ",
        )
        expected_signed_size = (
            (len(unsigned_app) + SIGNATURE_ALIGNMENT - 1) // SIGNATURE_ALIGNMENT
        ) * SIGNATURE_ALIGNMENT + SIGNATURE_SECTOR
        require(
            len(signed_app) == expected_signed_size,
            f"{target} published signed app size differs from current unsigned projection: "
            f"expected={expected_signed_size} actual={len(signed_app)}",
        )
        require(
            signed_app[: len(unsigned_app)] == unsigned_app,
            f"{target} published signed app prefix differs from the current unsigned app",
        )
        validate_signed_firmware_bytes(
            target,
            version,
            boot,
            signed_app,
            expected_public_key_digest,
        )

        merged = snapshot[merged_name]
        payloads = sorted(
            (
                (boot_offset, boot_offset + len(boot), boot, "bootloader"),
                (PARTITION_OFFSET, PARTITION_OFFSET + len(partition), partition, "partition"),
                (OTADATA_OFFSET, OTADATA_OFFSET + len(otadata), otadata, "otadata"),
                (APP_OFFSET, APP_OFFSET + len(signed_app), signed_app, "signed app"),
            )
        )
        cursor = 0
        for start, end, expected_bytes, label in payloads:
            require(start >= cursor, f"{target} merged {label} overlaps a previous payload")
            require_erased_gap(merged, cursor, start, target)
            require(end <= len(merged), f"{target} merged image is truncated at {label}")
            require(
                merged[start:end] == expected_bytes,
                f"{target} merged {label} bytes differ at 0x{start:x}",
            )
            cursor = end
        require(
            len(merged) == cursor,
            f"{target} merged image has trailing or missing bytes: expected={cursor} actual={len(merged)}",
        )

        diagnostic_dir = artifact_root / "dist" / target
        for pattern in DIAGNOSTIC_NAMES:
            name = pattern.format(target=target)
            current = safe_bytes(diagnostic_dir / name, f"current {target} diagnostic {name}")
            published = snapshot[name]
            require(current == published, f"{target} published diagnostic differs: {name}")
        elf_name = f"tesla-key-esp32-{target}.elf"
        validate_elf_checksum(
            target,
            snapshot[elf_name],
            snapshot[f"{elf_name}.sha256"],
        )
        staged[target] = {
            "bootloader.bin": boot,
            "partition-table.bin": partition,
            "tesla-key-esp32.bin": signed_app,
            "ota_data_initial.bin": otadata,
        }
        for name in (
            unversioned_name,
            versioned_name,
            merged_name,
        ):
            staged_root[name] = snapshot[name]

    output_parent = artifact_root
    temporary = Path(tempfile.mkdtemp(prefix=".reuse-fw.", dir=output_parent))
    root_temporary = Path(tempfile.mkdtemp(prefix=".reuse-root.", dir=output_parent))
    created_root: list[Path] = []
    try:
        for target, payloads in staged.items():
            target_dir = temporary / target
            target_dir.mkdir(mode=0o755)
            for name, data in payloads.items():
                destination = target_dir / name
                with destination.open("xb") as handle:
                    handle.write(data)
        for name, data in staged_root.items():
            destination = artifact_root / name
            require(
                not destination.exists() and not destination.is_symlink(),
                f"reuse root output already exists: {destination}",
            )
            with (root_temporary / name).open("xb") as handle:
                handle.write(data)
        try:
            SIGNED_ROOT_CONTRACT.validate_inventory(root_temporary, version)
        except SIGNED_ROOT_CONTRACT.InventoryError as exc:
            raise ReuseError(f"reuse temporary root inventory failed: {exc}") from exc
        temporary.rename(output)
        for name in sorted(staged_root):
            destination = artifact_root / name
            (root_temporary / name).rename(destination)
            created_root.append(destination)
        root_temporary.rmdir()
        try:
            SIGNED_ROOT_CONTRACT.validate_inventory(artifact_root, version)
        except SIGNED_ROOT_CONTRACT.InventoryError as exc:
            raise ReuseError(f"reuse staged root inventory failed: {exc}") from exc
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        shutil.rmtree(root_temporary, ignore_errors=True)
        shutil.rmtree(output, ignore_errors=True)
        for path in created_root:
            try:
                path.unlink()
            except FileNotFoundError:
                pass
        raise
    return len(snapshot)


def make_release(files: dict[str, Path], version: str, source_sha: str) -> dict[str, Any]:
    assets = []
    for index, name in enumerate(sorted(files), 1):
        data = files[name].read_bytes()
        assets.append(
            {
                "id": index,
                "name": name,
                "size": len(data),
                "digest": f"sha256:{sha256_bytes(data)}",
            }
        )
    return {
        "id": 42,
        "tag_name": f"v{version}",
        "target_commitish": source_sha,
        "draft": False,
        "prerelease": False,
        "immutable": True,
        "assets": assets,
    }


def write_fixture(root: Path, version: str, source_sha: str) -> tuple[Path, Path, dict[str, Any]]:
    artifact = root / "artifact"
    download = root / "download"
    artifact.mkdir()
    download.mkdir()
    for target_index, (target, suffix, boot_offset) in enumerate(TARGETS):
        unsigned_dir = artifact / "_unsigned" / target
        diagnostic_dir = artifact / "dist" / target
        (unsigned_dir / "bootloader").mkdir(parents=True)
        (unsigned_dir / "partition_table").mkdir()
        diagnostic_dir.mkdir(parents=True)
        boot = FIRMWARE_CONTRACT.make_image(target, "ignored", app=False)
        unsigned_app = FIRMWARE_CONTRACT.make_image(target, version, app=True)
        signed_app = FIRMWARE_CONTRACT.fake_sign(unsigned_app)
        partition = (f"partition-{target}\n".encode("ascii") * 7)[:97]
        otadata = b"\xff" * 0x2000
        (unsigned_dir / "bootloader" / "bootloader.bin").write_bytes(boot)
        (unsigned_dir / "partition_table" / "partition-table.bin").write_bytes(partition)
        (unsigned_dir / "tesla-key-esp32.bin").write_bytes(unsigned_app)
        (unsigned_dir / "ota_data_initial.bin").write_bytes(otadata)

        (download / f"tesla-key-esp32{suffix}.bin").write_bytes(signed_app)
        (download / f"tesla-key-esp32{suffix}-{version}.bin").write_bytes(signed_app)
        merged = bytearray(b"\xff" * (APP_OFFSET + len(signed_app)))
        merged[boot_offset : boot_offset + len(boot)] = boot
        merged[PARTITION_OFFSET : PARTITION_OFFSET + len(partition)] = partition
        merged[OTADATA_OFFSET : OTADATA_OFFSET + len(otadata)] = otadata
        merged[APP_OFFSET : APP_OFFSET + len(signed_app)] = signed_app
        (download / f"tesla-key-esp32{suffix}-{version}-merged.bin").write_bytes(merged)

        elf_name = f"tesla-key-esp32-{target}.elf"
        elf = f"elf-{target}-{target_index}\n".encode("ascii")
        diagnostic_values = {
            elf_name: elf,
            f"{elf_name}.sha256": (
                f"{sha256_bytes(elf)}  dist/{target}/{elf_name}\n".encode("ascii")
            ),
            f"tesla-key-esp32-{target}.map": f"map-{target}\n".encode("ascii"),
            f"sdkconfig.{target}": f"sdkconfig-{target}\n".encode("ascii"),
            f"dependencies.lock.{target}": f"lock-{target}\n".encode("ascii"),
            f"size-{target}.json": b"{}\n",
            f"size-{target}.md": f"size-{target}\n".encode("ascii"),
        }
        for name, data in diagnostic_values.items():
            (diagnostic_dir / name).write_bytes(data)
            (download / name).write_bytes(data)
    files = {entry.name: entry for entry in download.iterdir()}
    return artifact, download, make_release(files, version, source_sha)


def fixture_key_digest(download: Path) -> bytes:
    data = (download / "tesla-key-esp32.bin").read_bytes()
    block_start = len(data) - FIRMWARE_CONTRACT.SIGNATURE_SECTOR
    return hashlib.sha256(
        data[
            block_start + FIRMWARE_CONTRACT.RSA_N_OFFSET :
            block_start + FIRMWARE_CONTRACT.RSA_PUBLIC_FIELDS_END
        ]
    ).digest()


def expect_rejected(
    label: str,
    mutate: Any,
    expected: str,
    version: str,
    source_sha: str,
) -> None:
    with tempfile.TemporaryDirectory(prefix=f"reuse-release-{label}-") as directory:
        root = Path(directory)
        artifact, download, release = write_fixture(root, version, source_sha)
        mutate(artifact, download, release)
        try:
            validate_and_stage(
                release,
                download,
                artifact,
                artifact / "_fw",
                version,
                source_sha,
                fixture_key_digest(download),
            )
        except ReuseError as exc:
            require(expected in str(exc), f"{label} failed for wrong reason: {exc}")
        else:
            raise ReuseError(f"self-test accepted mutation: {label}")


def self_test() -> None:
    version = "1.2.3"
    source_sha = "0123456789abcdef0123456789abcdef01234567"
    with tempfile.TemporaryDirectory(prefix="reuse-release-valid-") as directory:
        root = Path(directory)
        artifact, download, release = write_fixture(root, version, source_sha)
        count = validate_and_stage(
            release,
            download,
            artifact,
            artifact / "_fw",
            version,
            source_sha,
            fixture_key_digest(download),
        )
        require(count == 40, "valid fixture did not bind 40 Release assets")
        require(
            {path.name for path in (artifact / "_fw").iterdir()}
            == {target for target, _suffix, _offset in TARGETS},
            "valid fixture did not stage exactly four targets",
        )
        require(
            len(list(artifact.glob("tesla-key-esp32*.bin"))) == 12,
            "valid fixture did not stage twelve root recovery artifacts",
        )

    # The REST-bound download is untrusted mutable filesystem state.  Replace representative
    # signed-app, alias, merged and diagnostic paths after the metadata/digest pass.  Validation
    # and staging must remain bound to the one no-follow byte snapshot, never to these replacements.
    with tempfile.TemporaryDirectory(prefix="reuse-release-snapshot-swap-") as directory:
        root = Path(directory)
        artifact, download, release = write_fixture(root, version, source_sha)
        selected = (
            "tesla-key-esp32.bin",
            f"tesla-key-esp32-{version}.bin",
            f"tesla-key-esp32-{version}-merged.bin",
            "size-esp32.md",
        )
        original = {name: (download / name).read_bytes() for name in selected}
        displaced = root / "displaced-download-paths"
        displaced.mkdir()
        hook_calls = 0

        def replace_download_paths_after_metadata() -> None:
            nonlocal hook_calls
            hook_calls += 1
            for index, name in enumerate(selected, 1):
                path = download / name
                os.replace(path, displaced / name)
                prior = original[name]
                replacement = bytes([prior[0] ^ index]) + prior[1:]
                path.write_bytes(replacement)

        count = validate_and_stage(
            release,
            download,
            artifact,
            artifact / "_fw",
            version,
            source_sha,
            fixture_key_digest(download),
            after_snapshot_validation=replace_download_paths_after_metadata,
        )
        require(count == 40 and hook_calls == 1, "snapshot path-swap hook did not run exactly once")
        require(
            (artifact / "tesla-key-esp32.bin").read_bytes()
            == original["tesla-key-esp32.bin"],
            "post-metadata path swap changed staged signed-app snapshot bytes",
        )
        require(
            (artifact / f"tesla-key-esp32-{version}.bin").read_bytes()
            == original[f"tesla-key-esp32-{version}.bin"],
            "post-metadata path swap changed staged alias snapshot bytes",
        )
        require(
            (artifact / f"tesla-key-esp32-{version}-merged.bin").read_bytes()
            == original[f"tesla-key-esp32-{version}-merged.bin"],
            "post-metadata path swap changed staged merged snapshot bytes",
        )
        require(
            (artifact / "_fw" / "esp32" / "tesla-key-esp32.bin").read_bytes()
            == original["tesla-key-esp32.bin"],
            "post-metadata path swap changed installer staging snapshot bytes",
        )

    with tempfile.TemporaryDirectory(prefix="reuse-release-wrong-key-") as directory:
        root = Path(directory)
        artifact, download, release = write_fixture(root, version, source_sha)
        try:
            validate_and_stage(
                release,
                download,
                artifact,
                artifact / "_fw",
                version,
                source_sha,
                b"\x00" * 32,
            )
        except ReuseError as exc:
            require(
                "pinned production authority" in str(exc),
                f"wrong-key canary failed for wrong reason: {exc}",
            )
        else:
            raise ReuseError("self-test accepted a non-authoritative signing key")

    def refresh(release: dict[str, Any], download: Path) -> None:
        release.clear()
        release.update(make_release({entry.name: entry for entry in download.iterdir()}, version, source_sha))

    def hardlink_download_asset(download: Path, name: str, anchor_name: str) -> None:
        path = download / name
        anchor = download.parent / anchor_name
        anchor.write_bytes(path.read_bytes())
        path.unlink()
        os.link(anchor, path)

    def hardlink_diagnostic(
        _artifact: Path, download: Path, _release: dict[str, Any]
    ) -> None:
        hardlink_download_asset(download, "size-esp32.md", "diagnostic-hardlink-anchor")

    expect_rejected(
        "diagnostic-hardlink",
        hardlink_diagnostic,
        "must not be hard-linked",
        version,
        source_sha,
    )

    def hardlink_merged(
        _artifact: Path, download: Path, _release: dict[str, Any]
    ) -> None:
        hardlink_download_asset(
            download,
            f"tesla-key-esp32-{version}-merged.bin",
            "merged-hardlink-anchor",
        )

    expect_rejected(
        "merged-hardlink",
        hardlink_merged,
        "must not be hard-linked",
        version,
        source_sha,
    )

    def zero_diagnostic(
        _artifact: Path, download: Path, _release: dict[str, Any]
    ) -> None:
        (download / "size-esp32.md").write_bytes(b"")

    expect_rejected(
        "diagnostic-zero-byte",
        zero_diagnostic,
        "invalid size 0",
        version,
        source_sha,
    )

    def zero_merged(
        _artifact: Path, download: Path, _release: dict[str, Any]
    ) -> None:
        (download / f"tesla-key-esp32-{version}-merged.bin").write_bytes(b"")

    expect_rejected(
        "merged-zero-byte",
        zero_merged,
        "invalid size 0",
        version,
        source_sha,
    )

    def tamper_alias(_artifact: Path, download: Path, release: dict[str, Any]) -> None:
        path = download / f"tesla-key-esp32-s3-{version}.bin"
        path.write_bytes(path.read_bytes() + b"x")
        refresh(release, download)

    expect_rejected("alias", tamper_alias, "signed app aliases differ", version, source_sha)

    def tamper_signature(_artifact: Path, download: Path, release: dict[str, Any]) -> None:
        target = "esp32c3"
        suffix = "-c3"
        path = download / f"tesla-key-esp32{suffix}.bin"
        signed = bytearray(path.read_bytes())
        block_start = len(signed) - FIRMWARE_CONTRACT.SIGNATURE_SECTOR
        signed[block_start + FIRMWARE_CONTRACT.RSA_SIGNATURE_OFFSET] ^= 1
        crc = zlib.crc32(
            signed[
                block_start : block_start + FIRMWARE_CONTRACT.SIGNATURE_DATA_SIZE
            ]
        ) & 0xFFFFFFFF
        struct.pack_into(
            "<I",
            signed,
            block_start + FIRMWARE_CONTRACT.SIGNATURE_DATA_SIZE,
            crc,
        )
        for name in (
            f"tesla-key-esp32{suffix}.bin",
            f"tesla-key-esp32{suffix}-{version}.bin",
        ):
            (download / name).write_bytes(signed)
        merged_path = download / f"tesla-key-esp32{suffix}-{version}-merged.bin"
        merged = bytearray(merged_path.read_bytes())
        merged[APP_OFFSET:] = signed
        merged_path.write_bytes(merged)
        refresh(release, download)

    expect_rejected(
        "rsa-signature", tamper_signature, "RSA-PSS", version, source_sha
    )

    def swap_targets(artifact: Path, download: Path, release: dict[str, Any]) -> None:
        left_unsigned = artifact / "_unsigned" / "esp32s3" / "tesla-key-esp32.bin"
        right_unsigned = artifact / "_unsigned" / "esp32c3" / "tesla-key-esp32.bin"
        left_unsigned_bytes, right_unsigned_bytes = (
            left_unsigned.read_bytes(),
            right_unsigned.read_bytes(),
        )
        left_unsigned.write_bytes(right_unsigned_bytes)
        right_unsigned.write_bytes(left_unsigned_bytes)
        left = download / "tesla-key-esp32-s3.bin"
        right = download / "tesla-key-esp32-c3.bin"
        left_bytes, right_bytes = left.read_bytes(), right.read_bytes()
        for suffix, data in (("-s3", right_bytes), ("-c3", left_bytes)):
            (download / f"tesla-key-esp32{suffix}.bin").write_bytes(data)
            (download / f"tesla-key-esp32{suffix}-{version}.bin").write_bytes(data)
            merged_path = download / f"tesla-key-esp32{suffix}-{version}-merged.bin"
            merged = bytearray(merged_path.read_bytes())
            old_size = len(merged) - APP_OFFSET
            require(len(data) == old_size, "fixture signed app sizes unexpectedly differ")
            merged[APP_OFFSET:] = data
            merged_path.write_bytes(merged)
        refresh(release, download)

    expect_rejected("target-swap", swap_targets, "app advertises chip ID", version, source_sha)

    def wrong_version(artifact: Path, download: Path, release: dict[str, Any]) -> None:
        target = "esp32c6"
        suffix = "-c6"
        unsigned = FIRMWARE_CONTRACT.make_image(target, "1.2.4", app=True)
        signed = FIRMWARE_CONTRACT.fake_sign(unsigned)
        (artifact / "_unsigned" / target / "tesla-key-esp32.bin").write_bytes(unsigned)
        (download / f"tesla-key-esp32{suffix}.bin").write_bytes(signed)
        (download / f"tesla-key-esp32{suffix}-{version}.bin").write_bytes(signed)
        merged_path = download / f"tesla-key-esp32{suffix}-{version}-merged.bin"
        merged = bytearray(merged_path.read_bytes())
        require(len(merged) - APP_OFFSET == len(signed), "fixture signed size drifted")
        merged[APP_OFFSET:] = signed
        merged_path.write_bytes(merged)
        refresh(release, download)

    expect_rejected("version", wrong_version, "app version mismatch", version, source_sha)

    def tamper_gap(_artifact: Path, download: Path, release: dict[str, Any]) -> None:
        path = download / f"tesla-key-esp32-c6-{version}-merged.bin"
        data = bytearray(path.read_bytes())
        data[0x7000] = 0
        path.write_bytes(data)
        refresh(release, download)

    expect_rejected("merged-gap", tamper_gap, "not erased 0xff", version, source_sha)

    def tamper_diagnostic(_artifact: Path, download: Path, release: dict[str, Any]) -> None:
        path = download / "size-esp32.md"
        path.write_bytes(path.read_bytes() + b"tamper\n")
        refresh(release, download)

    expect_rejected("diagnostic", tamper_diagnostic, "published diagnostic differs", version, source_sha)

    def tamper_elf_checksum(artifact: Path, download: Path, release: dict[str, Any]) -> None:
        name = "tesla-key-esp32-esp32.elf.sha256"
        content = b"0" * 64 + b"  dist/esp32/tesla-key-esp32-esp32.elf\n"
        (artifact / "dist" / "esp32" / name).write_bytes(content)
        (download / name).write_bytes(content)
        refresh(release, download)

    expect_rejected("elf-checksum", tamper_elf_checksum, ".elf.sha256", version, source_sha)

    def tamper_metadata(_artifact: Path, _download: Path, release: dict[str, Any]) -> None:
        release["assets"][0]["digest"] = "sha256:" + "0" * 64

    expect_rejected("metadata", tamper_metadata, "SHA-256 differs", version, source_sha)

    def missing_asset(_artifact: Path, download: Path, _release: dict[str, Any]) -> None:
        (download / "size-esp32.md").unlink()

    expect_rejected("missing", missing_asset, "inventory drifted", version, source_sha)

    def symlink_asset(_artifact: Path, download: Path, _release: dict[str, Any]) -> None:
        path = download / "size-esp32.md"
        path.unlink()
        path.symlink_to(download / "size-esp32s3.md")

    expect_rejected(
        "symlink",
        symlink_asset,
        "regular non-symlink",
        version,
        source_sha,
    )

    def extra_asset(_artifact: Path, download: Path, _release: dict[str, Any]) -> None:
        (download / "extra.bin").write_bytes(b"x")

    expect_rejected("extra", extra_asset, "inventory drifted", version, source_sha)

    def preexisting_root_extra(
        artifact: Path, _download: Path, _release: dict[str, Any]
    ) -> None:
        (artifact / "tesla-key-esp32-surprise.bin").write_bytes(b"unexpected")

    expect_rejected(
        "preexisting-root-extra",
        preexisting_root_extra,
        "root pre-stage inventory failed",
        version,
        source_sha,
    )
    print(
        "immutable Release reuse staging self-test: PASS "
        "(40 one-read snapshots, path-swap/link/empty-file and mutation canaries)"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release_json", type=Path, nargs="?")
    parser.add_argument("download_dir", type=Path, nargs="?")
    parser.add_argument("artifact_root", type=Path, nargs="?")
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--version")
    parser.add_argument("--source-sha")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if None in (
        args.release_json,
        args.download_dir,
        args.artifact_root,
        args.output,
        args.version,
        args.source_sha,
    ):
        parser.error(
            "release_json, download_dir, artifact_root, output, --version and --source-sha are required"
        )
    try:
        release = json.loads(args.release_json.read_text(encoding="utf-8"))
        count = validate_and_stage(
            release,
            args.download_dir,
            args.artifact_root,
            args.output,
            args.version,
            args.source_sha,
        )
    except (OSError, UnicodeError, json.JSONDecodeError, ReuseError) as exc:
        print(f"immutable Release reuse failed: {exc}", file=sys.stderr)
        return 1
    print(f"immutable Release reuse: PASS ({count}/40 exact assets, four staged targets)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
