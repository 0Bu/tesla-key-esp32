#!/usr/bin/env python3
"""Validate the exact pinned ESP-IDF/tesla-ble dependency and patch contract."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import shutil
import stat
import sys
import tempfile
from collections.abc import Callable


TARGETS = ("esp32", "esp32s3", "esp32c3", "esp32c6")
TOOLCHAIN = (
    "v5.5.5@sha256:"
    "a9231d0697ab8f7517cc072e93b7c83e04907bfbfba80b6440d7dbbf90665cf2\n"
)
TESLA_GIT = "https://github.com/yoziru/tesla-ble.git"
TESLA_VERSION = "v5.1.2"
TESLA_RESOLVED_COMMIT = "a0e5efa610e7ee93ca04fd36bed72e9aac03f008"
TESLA_COMPONENT_HASH = "1ae0006ec68f649d13b58671950971e29186873114168756bcc4fd832fc5826f"
LOCK_MANIFEST_HASH = "1655b3b3b8e45dd7e3ecc5b9129b2b447762c76715d9a1668186434482084bdc"

# These digests deliberately cover comments, ordering and every transitive resolution. A reviewed
# dependency update changes this validator together with the lockfiles; a normal build may not
# silently rewrite an otherwise semantically plausible lock.
FILE_DIGESTS = {
    "esp-idf-toolchain.txt": "92d5b9212bb54c107927f58ffd51511a00bd72d65836edf20cb3d23b8533d962",
    "main/idf_component.yml": "ac4fa7c2d0a1e4ecd9bda0be0136f91659537f4165df0fa34fc83b8f9a329a00",
    "dependencies.lock.esp32": "eeab4a18a51e20b9541cdb9bf03b2c52cb3607fb658cc40ee1085df25c41ff9e",
    "dependencies.lock.esp32s3": "7b6aa6349aa9cbf284fef2d330268092b7db7a62000261564d2df9ff395edfec",
    "dependencies.lock.esp32c3": "7fa2bee50f7d3acd057d3e0fd58b7af05f4e2b6e207ed1141aa7b9793c4a0722",
    "dependencies.lock.esp32c6": "231ae00025aeb421ce2fed32b7df38095dea26df011877f7685f236162bc39be",
}
PATCH_INVENTORY = (
    (
        "0001-reject-replayed-carserver-responses.patch",
        "3b2a8db400bff98dff1ccde0253147f8ccc91df3a16b965f657e6740311cc5b6",
    ),
    (
        "0002-report-key-regeneration-result.patch",
        "fd0494a669fd61cd678193d79f15c5acb99f518fe7e7ed483e904ace147aa42a",
    ),
    (
        "0003-rate-limit-rx-framing-recovery-logs.patch",
        "09d6ce7e859d9c0ce71b64c337c01792e0a48cc73266f0fe9ee95d190902b92a",
    ),
    (
        "0004-drop-unused-parental-controls-actions.patch",
        "6984321d34bdafe900244d0fe18052cc015a5b42ba0cf4e12e8ed9fb08791743",
    ),
)
MANIFEST_LOGICAL_LINES = (
    "dependencies:",
    '  idf: ">=5.5,<6.0"',
    '  espressif/mdns: "^1.2.0"',
    "  yoziru/tesla-ble:",
    f'    git: "{TESLA_GIT}"',
    f'    version: "{TESLA_VERSION}"',
)
LOCK_TESLA_BLOCK = (
    "  yoziru/tesla-ble:\n"
    f"    component_hash: {TESLA_COMPONENT_HASH}\n"
    "    dependencies:\n"
    "    - name: idf\n"
    "      version: '>=5.0.1'\n"
    "    source:\n"
    f"      git: {TESLA_GIT}\n"
    "      path: .\n"
    "      type: git\n"
    "    targets:\n"
    "    - esp32\n"
    "    - esp32s3\n"
    "    - esp32c3\n"
    "    - esp32c6\n"
    f"    version: {TESLA_RESOLVED_COMMIT}\n"
)


class DependencyError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise DependencyError(message)


def read_regular(path: Path) -> bytes:
    require(not path.is_symlink() and path.is_file(), f"missing/unsafe dependency input: {path}")
    try:
        mode = path.stat().st_mode
    except OSError as exc:
        raise DependencyError(f"cannot stat dependency input {path}: {exc}") from exc
    require(stat.S_ISREG(mode), f"dependency input is not a regular file: {path}")
    try:
        return path.read_bytes()
    except OSError as exc:
        raise DependencyError(f"cannot read dependency input {path}: {exc}") from exc


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def decode(data: bytes, label: str) -> str:
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise DependencyError(f"{label} is not UTF-8 text") from exc


def logical_yaml_lines(text: str) -> tuple[str, ...]:
    return tuple(
        line.rstrip()
        for line in text.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    )


def validate(root: Path) -> None:
    require(root.is_dir() and not root.is_symlink(), f"dependency root is missing/unsafe: {root}")

    toolchain_data = read_regular(root / "esp-idf-toolchain.txt")
    require(decode(toolchain_data, "esp-idf-toolchain.txt") == TOOLCHAIN,
            "esp-idf-toolchain.txt must pin exact ESP-IDF v5.5.5 and image digest")

    manifest_data = read_regular(root / "main/idf_component.yml")
    manifest_text = decode(manifest_data, "main/idf_component.yml")
    require(logical_yaml_lines(manifest_text) == MANIFEST_LOGICAL_LINES,
            "main/idf_component.yml exact IDF/mdns/tesla-ble v5.1.2 Git contract drifted")

    actual_locks = tuple(sorted(path.name for path in root.glob("dependencies.lock.*")))
    expected_locks = tuple(f"dependencies.lock.{target}" for target in TARGETS)
    require(actual_locks == tuple(sorted(expected_locks)),
            f"target lockfile inventory drifted: expected {sorted(expected_locks)}, got {list(actual_locks)}")

    for target in TARGETS:
        relative = f"dependencies.lock.{target}"
        lock_data = read_regular(root / relative)
        lock_text = decode(lock_data, relative)
        require(lock_text.count(LOCK_TESLA_BLOCK) == 1,
                f"{relative}: tesla-ble resolved commit/component hash/Git/targets drifted")
        require(lock_text.count("  idf:\n    source:\n      type: idf\n    version: 5.5.5\n") == 1,
                f"{relative}: resolved ESP-IDF version must be exactly 5.5.5")
        require(lock_text.count(f"target: {target}\n") == 1,
                f"{relative}: lock target must be exactly {target}")
        require(lock_text.count(f"manifest_hash: {LOCK_MANIFEST_HASH}\n") == 1,
                f"{relative}: manifest hash drifted")
        require(
            lock_text.count(
                "direct_dependencies:\n"
                "- espressif/mdns\n"
                "- idf\n"
                "- yoziru/tesla-ble\n"
            ) == 1,
            f"{relative}: direct dependency inventory/order drifted",
        )

    patch_dir = root / "patches/tesla-ble"
    require(patch_dir.is_dir() and not patch_dir.is_symlink(),
            f"tesla-ble patch directory is missing/unsafe: {patch_dir}")
    entries = tuple(sorted(path.name for path in patch_dir.iterdir()))
    expected_patch_names = tuple(name for name, _ in PATCH_INVENTORY)
    require(entries == expected_patch_names,
            f"tesla-ble ordered patch filename inventory drifted: {entries}")
    for name, expected_digest in PATCH_INVENTORY:
        actual_digest = sha256(read_regular(patch_dir / name))
        require(actual_digest == expected_digest,
                f"tesla-ble patch digest drifted: {name}")

    for relative, expected_digest in FILE_DIGESTS.items():
        actual_digest = sha256(read_regular(root / relative))
        require(actual_digest == expected_digest,
                f"reviewed dependency file byte digest drifted: {relative}")


def copy_fixture(root: Path, destination: Path) -> None:
    for relative in FILE_DIGESTS:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / relative, target)
    patch_dir = destination / "patches/tesla-ble"
    patch_dir.mkdir(parents=True, exist_ok=True)
    for name, _ in PATCH_INVENTORY:
        shutil.copy2(root / "patches/tesla-ble" / name, patch_dir / name)


def mutate_text(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    require(text.count(old) >= 1, f"self-test mutation source missing: {old}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def self_test(root: Path) -> None:
    validate(root)

    def text(relative: str, old: str, new: str) -> Callable[[Path], None]:
        return lambda fixture: mutate_text(fixture / relative, old, new)

    mutations: tuple[tuple[str, Callable[[Path], None], str], ...] = (
        ("toolchain-version", text("esp-idf-toolchain.txt", "v5.5.5", "v5.5.4"),
         "exact ESP-IDF v5.5.5"),
        ("toolchain-image", text("esp-idf-toolchain.txt", "a9231", "b9231"),
         "exact ESP-IDF v5.5.5"),
        ("manifest-version", text("main/idf_component.yml", 'version: "v5.1.2"',
                                  'version: "v5.1.0"'), "v5.1.2 Git contract"),
        ("manifest-git", text("main/idf_component.yml", TESLA_GIT,
                              "https://example.invalid/tesla-ble.git"), "v5.1.2 Git contract"),
        ("resolved-commit", text("dependencies.lock.esp32", TESLA_RESOLVED_COMMIT,
                                 "0" * 40), "resolved commit/component hash"),
        ("component-hash", text("dependencies.lock.esp32s3", TESLA_COMPONENT_HASH,
                                "0" * 64), "resolved commit/component hash"),
        ("resolved-target", text("dependencies.lock.esp32c3", "    - esp32c6\n", ""),
         "resolved commit/component hash"),
        ("lock-target", text("dependencies.lock.esp32c6", "target: esp32c6",
                             "target: esp32c5"), "lock target must be exactly"),
        ("transitive-drift", text("dependencies.lock.esp32", "version: 1.11.3",
                                  "version: 1.11.2"), "file byte digest drifted"),
        ("patch-byte", text("patches/tesla-ble/0001-reject-replayed-carserver-responses.patch",
                            "CarServer", "Carserver"), "patch digest drifted"),
        ("missing-lock", lambda fixture: (fixture / "dependencies.lock.esp32c6").unlink(),
         "lockfile inventory drifted"),
        ("extra-lock", lambda fixture: shutil.copy2(fixture / "dependencies.lock.esp32",
                                                    fixture / "dependencies.lock.esp32c5"),
         "lockfile inventory drifted"),
        ("renamed-patch", lambda fixture: (fixture / "patches/tesla-ble" / PATCH_INVENTORY[0][0]).rename(
            fixture / "patches/tesla-ble/0004-renamed.patch"), "patch filename inventory drifted"),
        ("missing-patch", lambda fixture: (fixture / "patches/tesla-ble" / PATCH_INVENTORY[2][0]).unlink(),
         "patch filename inventory drifted"),
        ("extra-patch", lambda fixture: shutil.copy2(
            fixture / "patches/tesla-ble" / PATCH_INVENTORY[0][0],
            fixture / "patches/tesla-ble/0004-extra.patch"), "patch filename inventory drifted"),
    )

    for name, mutate, expected in mutations:
        with tempfile.TemporaryDirectory(prefix=f"dependency-contract-{name}-") as directory:
            fixture = Path(directory)
            copy_fixture(root, fixture)
            mutate(fixture)
            try:
                validate(fixture)
            except DependencyError as exc:
                require(expected in str(exc),
                        f"self-test {name} failed for the wrong reason: {exc}")
            else:
                raise DependencyError(f"self-test accepted dependency mutation: {name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = args.root.absolute()
    try:
        validate(root)
        if args.self_test:
            self_test(root)
    except (DependencyError, OSError, UnicodeError) as exc:
        print(f"dependency-contract: {exc}", file=sys.stderr)
        return 1
    print(
        "dependency-contract: PASS "
        f"(ESP-IDF v5.5.5, {len(TARGETS)} locks, tesla-ble v5.1.2, "
        f"{len(PATCH_INVENTORY)} patches"
        + (", mutation canaries" if args.self_test else "")
        + ")"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
