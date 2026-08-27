#!/usr/bin/env python3
"""Bind draft/published GitHub Release metadata to exact local release bytes."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
import sys
import tempfile
from pathlib import Path
from types import ModuleType
from typing import Any


VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$")
SOURCE_RE = re.compile(r"^[0-9a-f]{40}$")
NAMES = (
    "tesla-key-esp32-{version}-merged.bin",
    "tesla-key-esp32-s3-{version}-merged.bin",
    "tesla-key-esp32-c3-{version}-merged.bin",
    "tesla-key-esp32-c6-{version}-merged.bin",
)
TARGETS = (
    ("esp32", ""),
    ("esp32s3", "-s3"),
    ("esp32c3", "-c3"),
    ("esp32c6", "-c6"),
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
EXPECTED_FULL_ASSET_COUNT = 40
RELEASE_STATES = ("draft", "published-immutable")
BOOT_OFFSETS = {"esp32": 0x1000, "esp32s3": 0, "esp32c3": 0, "esp32c6": 0}
PARTITION_OFFSET = 0x8000
OTADATA_OFFSET = 0xF000
APP_OFFSET = 0x20000


class ReleaseError(ValueError):
    pass


def load_sibling(filename: str, module_name: str) -> ModuleType:
    path = Path(__file__).resolve().with_name(filename)
    if path.is_symlink() or not path.is_file():
        raise ReleaseError(f"missing/unsafe shared validator: {path}")
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise ReleaseError(f"cannot load shared validator: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


OTADATA_CONTRACT = load_sibling("check-otadata-contract.py", "tesla_release_otadata")
FIRMWARE_CONTRACT = load_sibling(
    "check-firmware-artifacts.py", "tesla_release_firmware"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_identity(
    release: Any, version: str, source_sha: str, expect_state: str
) -> list[Any]:
    if (
        not VERSION_RE.fullmatch(version)
        or len(version) > 31
        or not SOURCE_RE.fullmatch(source_sha)
    ):
        raise ReleaseError("invalid expected version or source SHA")
    if not isinstance(release, dict):
        raise ReleaseError("GitHub Release response must be an object")
    if release.get("tag_name") != f"v{version}" or release.get("target_commitish") != source_sha:
        raise ReleaseError("GitHub Release tag/target is not bound to the expected source")
    if expect_state == "draft":
        if (
            release.get("draft") is not True
            or release.get("prerelease") is not False
            or release.get("immutable") is not False
        ):
            raise ReleaseError("GitHub Release must be the exact mutable draft candidate")
    elif expect_state == "published-immutable":
        if release.get("draft") is not False or release.get("prerelease") is not False:
            raise ReleaseError("GitHub Release must be published and stable")
        if release.get("immutable") is not True:
            raise ReleaseError("GitHub Release must be immutable")
    else:
        raise ReleaseError(f"unsupported expected Release state: {expect_state!r}")
    assets = release.get("assets")
    if not isinstance(assets, list):
        raise ReleaseError("GitHub Release assets must be an array")
    return assets


def validate_metadata(release: Any, version: str, source_sha: str) -> dict[str, dict[str, Any]]:
    """Validate published authority for the four merged assets used by live acceptance."""
    assets = validate_identity(release, version, source_sha, "published-immutable")

    matched: dict[str, dict[str, Any]] = {}
    for pattern in NAMES:
        name = pattern.format(version=version)
        matches = [asset for asset in assets if isinstance(asset, dict) and asset.get("name") == name]
        if len(matches) != 1:
            raise ReleaseError(f"expected exactly one published asset named {name}")
        matched[name] = matches[0]
    return matched


def validate_full_metadata(
    release: Any, version: str, source_sha: str, expect_state: str
) -> dict[str, dict[str, Any]]:
    """Require the exact 40-name immutable/draft inventory before downloading any bytes."""
    assets = validate_identity(release, version, source_sha, expect_state)
    expected_names = set(expected_local_assets(Path("."), version))
    if len(expected_names) != EXPECTED_FULL_ASSET_COUNT:
        raise ReleaseError("internal full Release asset allowlist count drifted")
    names = [asset.get("name") for asset in assets if isinstance(asset, dict)]
    if len(names) != len(assets) or len(names) != len(set(names)):
        raise ReleaseError("GitHub Release assets must be uniquely named objects")
    if set(names) != expected_names:
        raise ReleaseError(
            "GitHub Release full asset inventory drifted: "
            f"missing={sorted(expected_names - set(names))} "
            f"extra={sorted(set(names) - expected_names)}"
        )
    by_name = {asset["name"]: asset for asset in assets}
    for name, asset in by_name.items():
        asset_id = asset.get("id")
        size = asset.get("size")
        digest = asset.get("digest")
        if not isinstance(asset_id, int) or isinstance(asset_id, bool) or asset_id <= 0:
            raise ReleaseError(f"GitHub Release asset id is invalid: {name}")
        if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
            raise ReleaseError(f"GitHub Release asset size is invalid: {name}")
        if not isinstance(digest, str) or not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
            raise ReleaseError(f"GitHub Release asset digest is invalid: {name}")
    return by_name


def expected_local_assets(local_dir: Path, version: str) -> dict[str, Path]:
    expected: dict[str, Path] = {}
    for target, suffix in TARGETS:
        root_names = (
            f"tesla-key-esp32{suffix}.bin",
            f"tesla-key-esp32{suffix}-{version}.bin",
            f"tesla-key-esp32{suffix}-{version}-merged.bin",
        )
        for name in root_names:
            expected[name] = local_dir / name
        for pattern in DIAGNOSTIC_NAMES:
            name = pattern.format(target=target)
            expected[name] = local_dir / "dist" / target / name
    if len(expected) != EXPECTED_FULL_ASSET_COUNT:
        raise ReleaseError("internal full Release asset allowlist count drifted")
    return expected


def regular_bytes(path: Path, label: str) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise ReleaseError(f"{label} is missing, non-regular or symlinked: {path}")
    data = path.read_bytes()
    if not data:
        raise ReleaseError(f"{label} is empty: {path}")
    return data


def require_erased_gap(merged: bytes, start: int, end: int, target: str) -> None:
    if not 0 <= start <= end <= len(merged):
        raise ReleaseError(f"{target} merged layout range is invalid")
    bad = next((index for index, value in enumerate(merged[start:end]) if value != 0xFF), None)
    if bad is not None:
        raise ReleaseError(
            f"{target} undeclared merged byte at 0x{start + bad:x} is not erased 0xff"
        )


def validate_staged_merged_layout(
    local_dir: Path, target: str, merged: bytes, signed_app: bytes
) -> None:
    """Bind every merged byte to signer-owned staging before a draft can be published."""
    stage = local_dir / "_fw" / target
    boot = regular_bytes(stage / "bootloader.bin", f"{target} staged bootloader")
    partition = regular_bytes(stage / "partition-table.bin", f"{target} staged partition table")
    otadata = regular_bytes(stage / "ota_data_initial.bin", f"{target} staged otadata")
    staged_app = regular_bytes(stage / "tesla-key-esp32.bin", f"{target} staged signed app")
    if staged_app != signed_app:
        raise ReleaseError(f"{target} staged and published signed app aliases differ")
    try:
        OTADATA_CONTRACT.validate_bytes(otadata, f"{target} staged otadata")
    except OTADATA_CONTRACT.OtadataError as exc:
        raise ReleaseError(str(exc)) from exc

    payloads = sorted(
        (
            (BOOT_OFFSETS[target], boot, "bootloader"),
            (PARTITION_OFFSET, partition, "partition table"),
            (OTADATA_OFFSET, otadata, "otadata"),
            (APP_OFFSET, signed_app, "signed app"),
        )
    )
    cursor = 0
    for start, expected, label in payloads:
        end = start + len(expected)
        if start < cursor:
            raise ReleaseError(f"{target} merged {label} overlaps a previous payload")
        require_erased_gap(merged, cursor, start, target)
        if end > len(merged):
            raise ReleaseError(f"{target} merged image is truncated at {label}")
        if merged[start:end] != expected:
            raise ReleaseError(f"{target} merged {label} bytes differ at 0x{start:x}")
        cursor = end
    if len(merged) != cursor:
        raise ReleaseError(
            f"{target} merged image has trailing or missing bytes: "
            f"expected={cursor} actual={len(merged)}"
        )


def validate_local_relationships(
    local_dir: Path,
    version: str,
    expected_public_key_digest: bytes | None = None,
) -> None:
    """Bind aliases, complete merged layouts and ELF checksums before publication."""
    for target, suffix in TARGETS:
        unversioned = local_dir / f"tesla-key-esp32{suffix}.bin"
        versioned = local_dir / f"tesla-key-esp32{suffix}-{version}.bin"
        merged = local_dir / f"tesla-key-esp32{suffix}-{version}-merged.bin"
        for label, path in (
            (f"{target} unversioned signed app", unversioned),
            (f"{target} versioned signed app", versioned),
            (f"{target} merged image", merged),
        ):
            if path.is_symlink() or not path.is_file():
                raise ReleaseError(f"{label} is missing, non-regular or symlinked: {path}")
        app_bytes = unversioned.read_bytes()
        if not app_bytes:
            raise ReleaseError(f"{target} signed app is empty")
        if versioned.read_bytes() != app_bytes:
            raise ReleaseError(
                f"{target} unversioned and versioned signed app aliases differ"
            )
        merged_bytes = merged.read_bytes()
        app_end = 0x20000 + len(app_bytes)
        if len(merged_bytes) < app_end or merged_bytes[0x20000:app_end] != app_bytes:
            raise ReleaseError(
                f"{target} merged app slice at 0x20000 differs from the signed app"
            )
        validate_staged_merged_layout(local_dir, target, merged_bytes, app_bytes)
        if expected_public_key_digest is not None:
            try:
                FIRMWARE_CONTRACT.validate_set(
                    target,
                    version,
                    local_dir / "_fw" / target / "bootloader.bin",
                    unversioned,
                    signed_app=True,
                    expected_public_key_digest=expected_public_key_digest,
                )
            except FIRMWARE_CONTRACT.ArtifactError as exc:
                raise ReleaseError(
                    f"{target} production signature authority failed: {exc}"
                ) from exc

        elf = local_dir / "dist" / target / f"tesla-key-esp32-{target}.elf"
        checksum = elf.with_name(elf.name + ".sha256")
        if elf.is_symlink() or not elf.is_file() or checksum.is_symlink() or not checksum.is_file():
            raise ReleaseError(f"{target} ELF/checksum pair is missing or unsafe")
        try:
            checksum_text = checksum.read_text(encoding="ascii")
        except UnicodeError as exc:
            raise ReleaseError(f"{target} ELF checksum is not ASCII") from exc
        expected_line = f"{sha256(elf)}  dist/{target}/{elf.name}\n"
        if checksum_text != expected_line:
            raise ReleaseError(f"{target} .elf.sha256 does not bind the published ELF bytes")


def validate_full_release(
    release: Any,
    local_dir: Path,
    version: str,
    source_sha: str,
    expect_state: str,
    expected_public_key_digest: bytes | None = None,
) -> int:
    """Require the exact 40-file Release set and bind every remote digest/size to local bytes."""
    expected = expected_local_assets(local_dir, version)
    by_name = validate_full_metadata(release, version, source_sha, expect_state)
    validate_local_relationships(local_dir, version, expected_public_key_digest)
    for name, local in expected.items():
        if local.is_symlink() or not local.is_file():
            raise ReleaseError(f"local Release artifact is missing, non-regular or symlinked: {local}")
        asset = by_name[name]
        asset_id = asset.get("id")
        if not isinstance(asset_id, int) or isinstance(asset_id, bool) or asset_id <= 0:
            raise ReleaseError(f"GitHub Release asset id is invalid: {name}")
        size = local.stat().st_size
        digest = sha256(local)
        if asset.get("size") != size:
            raise ReleaseError(
                f"Release asset {name} size differs: remote={asset.get('size')!r} local={size}"
            )
        if asset.get("digest") != f"sha256:{digest}":
            raise ReleaseError(f"Release asset {name} SHA-256 differs from local bytes")
    return len(expected)


def validate(release: Any, local_dir: Path, version: str, source_sha: str) -> int:
    matched = validate_metadata(release, version, source_sha)
    verified = 0
    for name, asset in matched.items():
        local = local_dir / name
        if local.is_symlink() or not local.is_file():
            raise ReleaseError(f"local merged artifact is missing, non-regular or symlinked: {local}")
        size = local.stat().st_size
        digest = sha256(local)
        if asset.get("size") != size:
            raise ReleaseError(
                f"published asset {name} size differs: remote={asset.get('size')!r} local={size}"
            )
        if asset.get("digest") != f"sha256:{digest}":
            raise ReleaseError(f"published asset {name} SHA-256 differs from local merged bytes")
        verified += 1
    return verified


def make_release(local_dir: Path, version: str, source_sha: str) -> dict[str, Any]:
    assets = []
    for pattern in NAMES:
        name = pattern.format(version=version)
        path = local_dir / name
        assets.append(
            {"id": len(assets) + 1, "name": name, "size": path.stat().st_size, "digest": f"sha256:{sha256(path)}"}
        )
    return {
        "tag_name": f"v{version}",
        "target_commitish": source_sha,
        "draft": False,
        "prerelease": False,
        "immutable": True,
        "assets": assets,
    }


def make_full_release(
    local_dir: Path, version: str, source_sha: str, expect_state: str
) -> dict[str, Any]:
    assets = []
    for name, path in expected_local_assets(local_dir, version).items():
        assets.append(
            {
                "id": len(assets) + 1,
                "name": name,
                "size": path.stat().st_size,
                "digest": f"sha256:{sha256(path)}",
            }
        )
    return {
        "tag_name": f"v{version}",
        "target_commitish": source_sha,
        "draft": expect_state == "draft",
        "prerelease": False,
        "immutable": expect_state == "published-immutable",
        "assets": assets,
    }


def self_test() -> None:
    version = "1.2.3"
    source_sha = "0123456789abcdef0123456789abcdef01234567"
    try:
        validate_metadata({}, "01.2.3", source_sha)
    except ReleaseError:
        pass
    else:
        raise AssertionError("non-canonical leading-zero release version was accepted")
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        for index, pattern in enumerate(NAMES):
            (root / pattern.format(version=version)).write_bytes(bytes([index]) * (index + 1))
        release = make_release(root, version, source_sha)
        assert validate(release, root, version, source_sha) == 4

        bad_digest = json.loads(json.dumps(release))
        bad_digest["assets"][0]["digest"] = "sha256:" + "0" * 64
        try:
            validate(bad_digest, root, version, source_sha)
        except ReleaseError:
            pass
        else:
            raise AssertionError("wrong remote digest was accepted")

        duplicate = json.loads(json.dumps(release))
        duplicate["assets"].append(dict(duplicate["assets"][0]))
        try:
            validate(duplicate, root, version, source_sha)
        except ReleaseError:
            pass
        else:
            raise AssertionError("duplicate remote asset was accepted")

        for label, value in (("false", False), ("missing", None)):
            mutable = json.loads(json.dumps(release))
            if value is None:
                del mutable["immutable"]
            else:
                mutable["immutable"] = value
            try:
                validate(mutable, root, version, source_sha)
            except ReleaseError:
                pass
            else:
                raise AssertionError(f"immutable={label} Release was accepted")

        for index, (target, suffix) in enumerate(TARGETS):
            app = f"signed-app-{target}\n".encode("ascii")
            (root / f"tesla-key-esp32{suffix}.bin").write_bytes(app)
            (root / f"tesla-key-esp32{suffix}-{version}.bin").write_bytes(app)
            stage = root / "_fw" / target
            stage.mkdir(parents=True, exist_ok=True)
            boot = f"boot-{target}".encode("ascii")
            partition = f"partition-{target}".encode("ascii")
            otadata = b"\xff" * 0x2000
            (stage / "bootloader.bin").write_bytes(boot)
            (stage / "partition-table.bin").write_bytes(partition)
            (stage / "ota_data_initial.bin").write_bytes(otadata)
            (stage / "tesla-key-esp32.bin").write_bytes(app)
            merged = bytearray(b"\xff" * (APP_OFFSET + len(app)))
            boot_offset = BOOT_OFFSETS[target]
            merged[boot_offset : boot_offset + len(boot)] = boot
            merged[PARTITION_OFFSET : PARTITION_OFFSET + len(partition)] = partition
            merged[OTADATA_OFFSET : OTADATA_OFFSET + len(otadata)] = otadata
            merged[APP_OFFSET : APP_OFFSET + len(app)] = app
            (root / f"tesla-key-esp32{suffix}-{version}-merged.bin").write_bytes(merged)
            diagnostic = root / "dist" / target
            diagnostic.mkdir(parents=True, exist_ok=True)
            elf = diagnostic / f"tesla-key-esp32-{target}.elf"
            elf.write_bytes(f"elf-{target}\n".encode("ascii"))
            (diagnostic / f"tesla-key-esp32-{target}.elf.sha256").write_text(
                f"{sha256(elf)}  dist/{target}/{elf.name}\n", encoding="ascii"
            )
            for pattern in DIAGNOSTIC_NAMES[2:]:
                path = diagnostic / pattern.format(target=target)
                path.write_bytes(f"release-fixture-{index}-{path.name}\n".encode("ascii"))
        draft = make_full_release(root, version, source_sha, "draft")
        published = make_full_release(root, version, source_sha, "published-immutable")
        assert len(validate_full_metadata(
            published, version, source_sha, "published-immutable"
        )) == EXPECTED_FULL_ASSET_COUNT
        assert validate_full_release(draft, root, version, source_sha, "draft") == 40
        assert validate_full_release(
            published, root, version, source_sha, "published-immutable"
        ) == 40

        wrong_state = json.loads(json.dumps(draft))
        wrong_state["draft"] = False
        wrong_state["immutable"] = True
        try:
            validate_full_release(wrong_state, root, version, source_sha, "draft")
        except ReleaseError:
            pass
        else:
            raise AssertionError("published state was accepted as the draft candidate")

        wrong_identity = json.loads(json.dumps(draft))
        wrong_identity["target_commitish"] = "f" * 40
        try:
            validate_full_release(wrong_identity, root, version, source_sha, "draft")
        except ReleaseError:
            pass
        else:
            raise AssertionError("wrong draft target identity was accepted")

        missing = json.loads(json.dumps(draft))
        missing["assets"].pop()
        try:
            validate_full_release(missing, root, version, source_sha, "draft")
        except ReleaseError:
            pass
        else:
            raise AssertionError("incomplete draft Release asset set was accepted")

        try:
            validate_full_metadata(missing, version, source_sha, "draft")
        except ReleaseError:
            pass
        else:
            raise AssertionError("metadata-only gate accepted a missing Release asset")

        extra = json.loads(json.dumps(draft))
        extra["assets"].append(
            {"id": 999, "name": "extra.bin", "size": 1, "digest": "sha256:" + "0" * 64}
        )
        try:
            validate_full_release(extra, root, version, source_sha, "draft")
        except ReleaseError:
            pass
        else:
            raise AssertionError("extra draft Release asset was accepted")

        try:
            validate_full_metadata(extra, version, source_sha, "draft")
        except ReleaseError:
            pass
        else:
            raise AssertionError("metadata-only gate accepted an extra Release asset")

        full_bad_digest = json.loads(json.dumps(draft))
        full_bad_digest["assets"][7]["digest"] = "sha256:" + "f" * 64
        try:
            validate_full_release(full_bad_digest, root, version, source_sha, "draft")
        except ReleaseError:
            pass
        else:
            raise AssertionError("wrong full Release asset digest was accepted")

        target, suffix = TARGETS[0]
        versioned = root / f"tesla-key-esp32{suffix}-{version}.bin"
        original_versioned = versioned.read_bytes()
        versioned.write_bytes(original_versioned + b"tamper")
        changed = make_full_release(root, version, source_sha, "draft")
        try:
            validate_full_release(changed, root, version, source_sha, "draft")
        except ReleaseError as exc:
            assert "signed app aliases differ" in str(exc)
        else:
            raise AssertionError("different signed app aliases were accepted")
        versioned.write_bytes(original_versioned)

        merged = root / f"tesla-key-esp32{suffix}-{version}-merged.bin"
        original_merged = merged.read_bytes()
        tampered_merged = bytearray(original_merged)
        tampered_merged[0x20000] ^= 1
        merged.write_bytes(tampered_merged)
        changed = make_full_release(root, version, source_sha, "draft")
        try:
            validate_full_release(changed, root, version, source_sha, "draft")
        except ReleaseError as exc:
            assert "merged app slice" in str(exc)
        else:
            raise AssertionError("merged/signed app byte drift was accepted")
        merged.write_bytes(original_merged)

        # These are the pre-publication safety properties: every signer-owned payload must match,
        # NVS/other undeclared gaps must remain erased, and no trailing byte may ride in the image.
        for label, mutate in (
            ("bootloader", lambda data: data.__setitem__(BOOT_OFFSETS[target], data[BOOT_OFFSETS[target]] ^ 1)),
            ("partition table", lambda data: data.__setitem__(PARTITION_OFFSET, data[PARTITION_OFFSET] ^ 1)),
            ("otadata", lambda data: data.__setitem__(OTADATA_OFFSET, 0)),
            ("undeclared merged byte", lambda data: data.__setitem__(0x9000, 0)),
        ):
            tampered = bytearray(original_merged)
            mutate(tampered)
            merged.write_bytes(tampered)
            changed = make_full_release(root, version, source_sha, "draft")
            try:
                validate_full_release(changed, root, version, source_sha, "draft")
            except ReleaseError as exc:
                assert label in str(exc), (label, str(exc))
            else:
                raise AssertionError(f"pre-publication {label} mutation was accepted")
        merged.write_bytes(original_merged + b"trailing")
        changed = make_full_release(root, version, source_sha, "draft")
        try:
            validate_full_release(changed, root, version, source_sha, "draft")
        except ReleaseError as exc:
            assert "trailing or missing bytes" in str(exc)
        else:
            raise AssertionError("pre-publication merged trailing bytes were accepted")
        merged.write_bytes(original_merged)

        elf = root / "dist" / target / f"tesla-key-esp32-{target}.elf"
        checksum = elf.with_name(elf.name + ".sha256")
        original_checksum = checksum.read_bytes()
        checksum.write_text("0" * 64 + f"  dist/{target}/{elf.name}\n", encoding="ascii")
        changed = make_full_release(root, version, source_sha, "draft")
        try:
            validate_full_release(changed, root, version, source_sha, "draft")
        except ReleaseError as exc:
            assert ".elf.sha256" in str(exc)
        else:
            raise AssertionError("ELF checksum drift was accepted")
        checksum.write_bytes(original_checksum)
    print("remote Release/local exact-asset byte contract self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release_json", type=Path, nargs="?")
    parser.add_argument("local_dir", type=Path, nargs="?")
    parser.add_argument("--version")
    parser.add_argument("--source-sha")
    parser.add_argument("--expect-state", choices=RELEASE_STATES, default="published-immutable")
    parser.add_argument("--expected-public-key-digest", type=Path)
    parser.add_argument("--metadata-only", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if None in (args.release_json, args.version, args.source_sha):
        parser.error("release_json, --version and --source-sha are required")
    if not args.metadata_only and args.local_dir is None:
        parser.error("local_dir is required unless --metadata-only is used")
    try:
        release = json.loads(args.release_json.read_text(encoding="utf-8"))
        if args.metadata_only:
            count = len(validate_full_metadata(
                release, args.version, args.source_sha, args.expect_state
            ))
        else:
            expected_public_key_digest = (
                FIRMWARE_CONTRACT.read_public_key_digest(
                    args.expected_public_key_digest
                )
                if args.expected_public_key_digest is not None
                else None
            )
            count = validate_full_release(
                release,
                args.local_dir,
                args.version,
                args.source_sha,
                args.expect_state,
                expected_public_key_digest,
            )
    except (
        OSError,
        UnicodeError,
        json.JSONDecodeError,
        ReleaseError,
        FIRMWARE_CONTRACT.ArtifactError,
    ) as exc:
        print(f"remote Release/local byte contract failed: {exc}", file=sys.stderr)
        return 1
    print(
        ("remote Release metadata contract: PASS " if args.metadata_only
         else "remote Release/local byte contract: PASS ")
        +
        f"({count}/{EXPECTED_FULL_ASSET_COUNT} exact assets, state={args.expect_state})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
