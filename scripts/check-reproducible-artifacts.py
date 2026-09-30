#!/usr/bin/env python3
"""Compare the primary firmware builds with their isolated reproduction copies, byte for byte.

CI builds every target twice: the primary build (`ci-build-all.sh`) and a second, fresh, isolated
copy (`check-reproducible-build.sh --emit`). The two run as parallel jobs, so the reproducibility
contract is enforced here, in the job that already holds the primary artifacts: for each of the four
targets the unsigned app and the unstripped ELF must be byte-identical to the reproduction copy.

Both sides arrive as downloaded Actions artifacts, so they are handled strictly as data: only regular,
non-empty, non-symlinked files are read, the reproduction directory must hold exactly the eight
expected files, and any missing, extra or differing file fails closed.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import stat
import sys
import tempfile
from pathlib import Path


TARGETS = ("esp32", "esp32s3", "esp32c3", "esp32c6")
CHUNK = 1 << 20


class ReproError(ValueError):
    pass


def regular_file(path: Path, label: str) -> Path:
    try:
        info = os.lstat(path)
    except OSError as exc:
        raise ReproError(f"{label} is missing: {path} ({exc.strerror})") from exc
    if not stat.S_ISREG(info.st_mode):
        raise ReproError(f"{label} is not a regular file (symlinks are refused): {path}")
    if info.st_size == 0:
        raise ReproError(f"{label} is empty: {path}")
    return path


def digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(CHUNK), b""):
            sha.update(block)
    return sha.hexdigest()


def first_difference(left: Path, right: Path) -> int | None:
    """Byte offset of the first difference, or None when the files are identical."""
    offset = 0
    with left.open("rb") as a, right.open("rb") as b:
        while True:
            block_a = a.read(CHUNK)
            block_b = b.read(CHUNK)
            if block_a != block_b:
                for index, (x, y) in enumerate(zip(block_a, block_b, strict=False)):
                    if x != y:
                        return offset + index
                return offset + min(len(block_a), len(block_b))
            if not block_a:
                return None
            offset += len(block_a)


def expected_pairs(artifact_root: Path, repro_dir: Path) -> list[tuple[str, str, Path, Path]]:
    pairs: list[tuple[str, str, Path, Path]] = []
    for target in TARGETS:
        pairs.append((
            target, "unsigned app",
            artifact_root / "_unsigned" / target / "tesla-key-esp32.bin",
            repro_dir / f"app-{target}.bin",
        ))
        pairs.append((
            target, "ELF",
            artifact_root / "dist" / target / f"tesla-key-esp32-{target}.elf",
            repro_dir / f"app-{target}.elf",
        ))
    return pairs


def check(artifact_root: Path, repro_dir: Path) -> list[str]:
    if repro_dir.is_symlink() or not repro_dir.is_dir():
        raise ReproError(f"reproduction directory is missing or not a plain directory: {repro_dir}")
    pairs = expected_pairs(artifact_root, repro_dir)
    expected_names = {repro.name for _target, _kind, _primary, repro in pairs}
    present = {entry.name for entry in repro_dir.iterdir()}
    if present != expected_names:
        raise ReproError(
            "reproduction directory must hold exactly the eight expected files: "
            f"missing={sorted(expected_names - present)} extra={sorted(present - expected_names)}"
        )
    report: list[str] = []
    for target, kind, primary, repro in pairs:
        primary = regular_file(primary, f"{target} primary {kind}")
        repro = regular_file(repro, f"{target} reproduction {kind}")
        offset = first_difference(primary, repro)
        if offset is not None:
            raise ReproError(
                f"{target}: {kind} is NOT reproducible; first difference at byte {offset} "
                f"(primary sha256 {digest(primary)}, reproduction sha256 {digest(repro)})"
            )
        report.append(f"{target} {kind}: {digest(primary)}")
    return report


def write_fixture(root: Path, contents: dict[str, bytes]) -> tuple[Path, Path]:
    """Create a primary artifact tree plus reproduction dir holding identical, per-target bytes."""
    repro_dir = root / "_repro"
    repro_dir.mkdir(parents=True)
    for target in TARGETS:
        (root / "_unsigned" / target).mkdir(parents=True)
        (root / "dist" / target).mkdir(parents=True)
        app = contents[f"{target}.bin"]
        elf = contents[f"{target}.elf"]
        (root / "_unsigned" / target / "tesla-key-esp32.bin").write_bytes(app)
        (root / "dist" / target / f"tesla-key-esp32-{target}.elf").write_bytes(elf)
        (repro_dir / f"app-{target}.bin").write_bytes(app)
        (repro_dir / f"app-{target}.elf").write_bytes(elf)
    return root, repro_dir


def self_test() -> None:
    contents = {}
    for index, target in enumerate(TARGETS):
        contents[f"{target}.bin"] = bytes([index + 1]) * 4096 + target.encode()
        contents[f"{target}.elf"] = bytes([index + 101]) * 8192 + target.encode()

    def expect_rejected(label: str, mutate, fragment: str) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root, repro_dir = write_fixture(Path(directory), contents)
            mutate(root, repro_dir)
            try:
                check(root, repro_dir)
            except ReproError as exc:
                if fragment not in str(exc):
                    raise AssertionError(f"{label}: wrong rejection reason: {exc}") from exc
                return
            raise AssertionError(f"{label}: mutation was accepted")

    with tempfile.TemporaryDirectory() as directory:
        root, repro_dir = write_fixture(Path(directory), contents)
        report = check(root, repro_dir)
        if len(report) != 8:
            raise AssertionError("identical builds did not report all eight comparisons")

    def flip(path: Path, offset: int) -> None:
        data = bytearray(path.read_bytes())
        data[offset] ^= 0x01
        path.write_bytes(bytes(data))

    def flip_repro_app(root: Path, repro: Path) -> None:
        flip(repro / "app-esp32s3.bin", 100)

    def flip_repro_elf(root: Path, repro: Path) -> None:
        flip(repro / "app-esp32c6.elf", 8000)

    def flip_primary_app(root: Path, repro: Path) -> None:
        flip(root / "_unsigned" / "esp32c3" / "tesla-key-esp32.bin", 4096)

    def flip_last_byte(root: Path, repro: Path) -> None:
        path = repro / "app-esp32.bin"
        flip(path, len(path.read_bytes()) - 1)

    def truncate_repro(root: Path, repro: Path) -> None:
        path = repro / "app-esp32.elf"
        path.write_bytes(path.read_bytes()[:-1])

    def swap_targets(root: Path, repro: Path) -> None:
        a, b = repro / "app-esp32.bin", repro / "app-esp32s3.bin"
        data_a, data_b = a.read_bytes(), b.read_bytes()
        a.write_bytes(data_b)
        b.write_bytes(data_a)

    def drop_repro_file(root: Path, repro: Path) -> None:
        (repro / "app-esp32c3.bin").unlink()

    def drop_primary_file(root: Path, repro: Path) -> None:
        (root / "dist" / "esp32" / "tesla-key-esp32-esp32.elf").unlink()

    def drop_target(root: Path, repro: Path) -> None:
        (repro / "app-esp32c6.bin").unlink()
        (repro / "app-esp32c6.elf").unlink()

    def extra_file(root: Path, repro: Path) -> None:
        (repro / "app-esp32c2.bin").write_bytes(b"x")

    def empty_both(root: Path, repro: Path) -> None:
        (repro / "app-esp32.bin").write_bytes(b"")
        (root / "_unsigned" / "esp32" / "tesla-key-esp32.bin").write_bytes(b"")

    def symlink_repro(root: Path, repro: Path) -> None:
        target = repro / "app-esp32.bin"
        real = repro.parent / "real.bin"
        real.write_bytes(target.read_bytes())
        target.unlink()
        target.symlink_to(real)

    def symlink_primary(root: Path, repro: Path) -> None:
        primary = root / "_unsigned" / "esp32s3" / "tesla-key-esp32.bin"
        real = root / "real-primary.bin"
        real.write_bytes(primary.read_bytes())
        primary.unlink()
        primary.symlink_to(real)

    def repro_dir_is_symlink(root: Path, repro: Path) -> None:
        moved = root / "_repro-real"
        repro.rename(moved)
        repro.symlink_to(moved, target_is_directory=True)

    def nested_dir(root: Path, repro: Path) -> None:
        (repro / "sub").mkdir()

    expect_rejected("reproduction app bit flip", flip_repro_app, "esp32s3: unsigned app is NOT reproducible")
    expect_rejected("reproduction ELF bit flip", flip_repro_elf, "esp32c6: ELF is NOT reproducible")
    expect_rejected("primary app bit flip", flip_primary_app, "esp32c3: unsigned app is NOT reproducible")
    expect_rejected("last-byte flip", flip_last_byte, "first difference at byte")
    expect_rejected("truncated reproduction", truncate_repro, "esp32: ELF is NOT reproducible")
    expect_rejected("target bytes swapped", swap_targets, "NOT reproducible")
    expect_rejected("missing reproduction file", drop_repro_file, "missing=['app-esp32c3.bin']")
    expect_rejected("missing primary file", drop_primary_file, "primary ELF is missing")
    expect_rejected("missing whole target", drop_target, "missing=['app-esp32c6.bin', 'app-esp32c6.elf']")
    expect_rejected("unexpected extra file", extra_file, "extra=['app-esp32c2.bin']")
    expect_rejected("empty files", empty_both, "is empty")
    expect_rejected("symlinked reproduction file", symlink_repro, "symlinks are refused")
    expect_rejected("symlinked primary file", symlink_primary, "symlinks are refused")
    expect_rejected("symlinked reproduction directory", repro_dir_is_symlink, "not a plain directory")
    expect_rejected("nested directory", nested_dir, "extra=['sub']")

    with tempfile.TemporaryDirectory() as directory:
        try:
            check(Path(directory), Path(directory) / "_repro")
        except ReproError as exc:
            if "reproduction directory is missing" not in str(exc):
                raise AssertionError("missing reproduction directory: wrong rejection reason") from exc
        else:
            raise AssertionError("missing reproduction directory was accepted")
    print("reproducible-artifacts self-test: PASS (identical, bit-flip, truncation, swap, missing, "
          "extra, empty, symlink and nested-directory canaries)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--artifact-root", type=Path, default=Path("."))
    parser.add_argument("--repro-dir", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    repro_dir = args.repro_dir if args.repro_dir is not None else args.artifact_root / "_repro"
    try:
        report = check(args.artifact_root, repro_dir)
    except (ReproError, OSError) as exc:
        print(f"reproducible-artifacts: FAIL: {exc}", file=sys.stderr)
        return 1
    for line in report:
        print(f"  {line}")
    print(f"reproducible-artifacts: PASS ({len(TARGETS)} targets, unsigned app + ELF byte-identical "
          "to their isolated reproduction copies)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
