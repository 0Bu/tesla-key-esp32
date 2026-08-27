#!/usr/bin/env python3
"""Require the exact twelve signed/merged root firmware files for one display version."""

from __future__ import annotations

import argparse
import fnmatch
import os
import re
import stat
import sys
import tempfile
from pathlib import Path


TARGET_SUFFIXES = ("", "-s3", "-c3", "-c6")
VERSION_RE = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$"
)
ROOT_PATTERN = "tesla-key-esp32*.bin"


class InventoryError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise InventoryError(message)


def validate_version(version: str) -> None:
    require(
        VERSION_RE.fullmatch(version) is not None and len(version) <= 31,
        f"invalid display version: {version!r}",
    )


def expected_names(version: str) -> frozenset[str]:
    validate_version(version)
    names = {
        name
        for suffix in TARGET_SUFFIXES
        for name in (
            f"tesla-key-esp32{suffix}.bin",
            f"tesla-key-esp32{suffix}-{version}.bin",
            f"tesla-key-esp32{suffix}-{version}-merged.bin",
        )
    }
    require(len(names) == 12, "internal signed root inventory count drifted")
    return frozenset(names)


def safe_root(root: Path) -> Path:
    absolute = Path(os.path.abspath(root))
    try:
        info = absolute.lstat()
    except OSError as exc:
        raise InventoryError(f"artifact root is not accessible: {absolute}: {exc}") from exc
    require(
        stat.S_ISDIR(info.st_mode) and not stat.S_ISLNK(info.st_mode),
        f"artifact root must be a real directory: {absolute}",
    )
    return absolute


def matching_entries(root: Path) -> dict[str, Path]:
    root = safe_root(root)
    entries = {
        entry.name: entry
        for entry in root.iterdir()
        if fnmatch.fnmatchcase(entry.name, ROOT_PATTERN)
    }
    return entries


def require_empty(root: Path) -> None:
    entries = matching_entries(root)
    require(
        not entries,
        f"signed root output must start empty; extra={sorted(entries)}",
    )


def validate_inventory(root: Path, version: str) -> int:
    entries = matching_entries(root)
    expected = expected_names(version)
    actual = set(entries)
    require(
        actual == expected,
        "signed root firmware inventory drifted: "
        f"missing={sorted(expected - actual)} extra={sorted(actual - expected)}",
    )
    payloads: dict[str, bytes] = {}
    for name in sorted(expected):
        path = entries[name]
        try:
            before = path.lstat()
        except OSError as exc:
            raise InventoryError(f"signed root artifact is inaccessible: {name}: {exc}") from exc
        require(
            stat.S_ISREG(before.st_mode) and not stat.S_ISLNK(before.st_mode),
            f"signed root artifact must be a regular non-symlink: {name}",
        )
        require(before.st_nlink == 1, f"signed root artifact must not be hard-linked: {name}")
        require(before.st_size > 0, f"signed root artifact must be non-empty: {name}")
        payloads[name] = path.read_bytes()
        after = path.lstat()
        require(
            (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
            == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns),
            f"signed root artifact changed during inventory validation: {name}",
        )
        require(
            len(payloads[name]) == before.st_size,
            f"signed root artifact read was truncated: {name}",
        )
    for suffix in TARGET_SUFFIXES:
        unversioned = f"tesla-key-esp32{suffix}.bin"
        versioned = f"tesla-key-esp32{suffix}-{version}.bin"
        require(
            payloads[versioned] == payloads[unversioned],
            f"versioned app alias differs from unversioned app: {versioned}",
        )
    return len(entries)


def expect_rejected(label: str, mutate: object, expected: str) -> None:
    version = "1.2.3"
    with tempfile.TemporaryDirectory(prefix=f"signed-root-{label}-") as directory:
        root = Path(directory)
        for name in expected_names(version):
            (root / name).write_bytes(b"signed-firmware\n")
        assert callable(mutate)
        mutate(root, version)
        try:
            validate_inventory(root, version)
        except InventoryError as exc:
            require(expected in str(exc), f"{label} failed for the wrong reason: {exc}")
        else:
            raise InventoryError(f"self-test accepted mutation: {label}")


def self_test() -> None:
    version = "1.2.3"
    try:
        validate_version("01.2.3")
    except InventoryError:
        pass
    else:
        raise InventoryError("self-test accepted non-canonical leading-zero version")
    with tempfile.TemporaryDirectory(prefix="signed-root-valid-") as directory:
        root = Path(directory)
        require_empty(root)
        for name in expected_names(version):
            (root / name).write_bytes(b"signed-firmware\n")
        require(validate_inventory(root, version) == 12, "valid twelve-file inventory failed")
        try:
            require_empty(root)
        except InventoryError as exc:
            require("must start empty" in str(exc), "nonempty pre-stage canary failed")
        else:
            raise InventoryError("self-test accepted a nonempty pre-stage root")

    expect_rejected(
        "extra",
        lambda root, _version: (root / "tesla-key-esp32-surprise.bin").write_bytes(b"x"),
        "extra=",
    )
    expect_rejected(
        "missing",
        lambda root, _version: (root / "tesla-key-esp32.bin").unlink(),
        "missing=",
    )

    def symlink(root: Path, _version: str) -> None:
        path = root / "tesla-key-esp32.bin"
        path.unlink()
        path.symlink_to(root / "tesla-key-esp32-s3.bin")

    expect_rejected("symlink", symlink, "regular non-symlink")
    expect_rejected(
        "zero",
        lambda root, _version: (root / "tesla-key-esp32.bin").write_bytes(b""),
        "non-empty",
    )
    expect_rejected(
        "versioned-alias-bytes",
        lambda root, version: (root / f"tesla-key-esp32-s3-{version}.bin").write_bytes(
            b"different-signed-firmware\n"
        ),
        "versioned app alias differs from unversioned app",
    )

    def hardlink(root: Path, _version: str) -> None:
        path = root / "tesla-key-esp32.bin"
        path.unlink()
        os.link(root / "tesla-key-esp32-s3.bin", path)

    expect_rejected("hardlink", hardlink, "hard-linked")

    def wrong_version(root: Path, _version: str) -> None:
        path = root / "tesla-key-esp32-c6-1.2.3.bin"
        path.rename(root / "tesla-key-esp32-c6-1.2.4.bin")

    expect_rejected("wrong-version", wrong_version, "inventory drifted")
    print("signed root inventory self-test: PASS (12 exact files, mutation canaries)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, nargs="?")
    parser.add_argument("--version")
    parser.add_argument("--require-empty", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.root is None:
        parser.error("root is required")
    try:
        if args.require_empty:
            require(args.version is None, "--version is incompatible with --require-empty")
            require_empty(args.root)
            print("signed root inventory: PASS (empty)")
        else:
            require(args.version is not None, "--version is required")
            count = validate_inventory(args.root, args.version)
            print(f"signed root inventory: PASS ({count}/12 exact files)")
    except (OSError, InventoryError) as exc:
        print(f"signed root inventory failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
