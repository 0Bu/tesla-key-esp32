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
TESLA_VERSION = "v5.2.0"
TESLA_RESOLVED_COMMIT = "07a4ef503a52f736009fdeba953f185aecc863f3"
TESLA_COMPONENT_HASH = "fb55938820781e8a731fc1557c0c4542bbf6833729062cfdb86a978b07016025"
LOCK_MANIFEST_HASH = "237ea7c60435890a8296d77775999b9332cee898da572911ff4a75da4d33712f"

# These digests deliberately cover comments, ordering and every transitive resolution. A reviewed
# dependency update changes this validator together with the lockfiles; a normal build may not
# silently rewrite an otherwise semantically plausible lock.
FILE_DIGESTS = {
    "esp-idf-toolchain.txt": "92d5b9212bb54c107927f58ffd51511a00bd72d65836edf20cb3d23b8533d962",
    "main/idf_component.yml": "0924f7d5bb4e31165cbfbfc40c09a3b4643675e02dcb4770b71069d6df2dd857",
    "dependencies.lock.esp32": "50639308f06156f8dc5a733912846e5c8de2939a54519b0efb6fdef35b98f62f",
    "dependencies.lock.esp32s3": "944323fb0d8967bb031649e5e3c80c66f84818002e915d4debeea1e965d18c7b",
    "dependencies.lock.esp32c3": "8c3fbd5b84c9bb1ebe6a652debd06fab5f75a5989ae4b612e1dcdc62f687babe",
    "dependencies.lock.esp32c6": "14c14fd71331b6225bd24b984a1a7329736539856870edb5c7c84d43bdf03dc5",
}
PATCH_INVENTORY = (
    (
        "0002-report-key-regeneration-result.patch",
        "f8aed9724ea590dbb6ca7058212b7cf65869ee6b5abaa5d99d03349e3a1a09de",
    ),
    (
        "0003-rate-limit-rx-framing-recovery-logs.patch",
        "23b271a066d74e83e1f059237a5dddfa469f6967be64d5a09e7ec46b451217d3",
    ),
    (
        "0004-drop-unused-parental-controls-actions.patch",
        "6984321d34bdafe900244d0fe18052cc015a5b42ba0cf4e12e8ed9fb08791743",
    ),
    (
        "0005-align-session-counter-replay-with-signer-go.patch",
        "5e72f4e5a1493251457ad6dd7eb6720cec64e26107000e5dbf06ee0c33a4d532",
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
            "main/idf_component.yml exact IDF/mdns/tesla-ble v5.2.0 Git contract drifted")

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
        ("manifest-version", text("main/idf_component.yml", 'version: "v5.2.0"',
                                  'version: "v5.1.0"'), "v5.2.0 Git contract"),
        ("manifest-git", text("main/idf_component.yml", TESLA_GIT,
                              "https://example.invalid/tesla-ble.git"), "v5.2.0 Git contract"),
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
        ("patch-byte", text("patches/tesla-ble/0002-report-key-regeneration-result.patch",
                            "regenerate_key", "regen_key"), "patch digest drifted"),
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
        f"(ESP-IDF v5.5.5, {len(TARGETS)} locks, tesla-ble {TESLA_VERSION}, "
        f"{len(PATCH_INVENTORY)} patches"
        + (", mutation canaries" if args.self_test else "")
        + ")"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
