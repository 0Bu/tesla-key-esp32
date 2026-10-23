#!/usr/bin/env python3
"""Fail-closed validation for the signed four-target Web Serial manifest."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import pathlib
import re
import stat
import sys


TARGETS = (
    ("ESP32", "esp32", "", 0x1000),
    ("ESP32-S3", "esp32s3", "-s3", 0),
    ("ESP32-C3", "esp32c3", "-c3", 0),
    ("ESP32-C6", "esp32c6", "-c6", 0),
)
SOURCE_SHA = re.compile(r"^[0-9a-f]{40}$")
DIGEST = re.compile(r"^[0-9a-f]{64}$")
VERSION_RE = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$"
)


class ManifestError(RuntimeError):
    pass


def load_otadata_contract():
    path = pathlib.Path(__file__).with_name("check-otadata-contract.py")
    if path.is_symlink() or not path.is_file():
        raise ManifestError(f"missing/unsafe shared otadata validator: {path}")
    spec = importlib.util.spec_from_file_location("tesla_key_otadata_contract", path)
    if spec is None or spec.loader is None:
        raise ManifestError(f"cannot load shared otadata validator: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


OTADATA_CONTRACT = load_otadata_contract()


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate(site: pathlib.Path, expected_source: str | None, expected_version: str | None) -> None:
    manifest_path = site / "manifest.json"
    if manifest_path.is_symlink() or not manifest_path.is_file():
        raise ManifestError(f"missing/unsafe manifest: {manifest_path}")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ManifestError(f"cannot parse {manifest_path}: {error}") from error

    expected_top = {"name", "layoutVersion", "sourceSha", "version", "new_install_prompt_erase", "builds"}
    if not isinstance(manifest, dict) or set(manifest) != expected_top:
        raise ManifestError(f"top-level fields must be exactly {sorted(expected_top)}")
    if manifest["name"] != "tesla-key-esp32" or manifest["layoutVersion"] != 2:
        raise ManifestError("unexpected manifest name/layoutVersion")
    if manifest["new_install_prompt_erase"] is not True:
        raise ManifestError("new_install_prompt_erase must be true")
    source_sha = manifest["sourceSha"]
    if not isinstance(source_sha, str) or not SOURCE_SHA.fullmatch(source_sha):
        raise ManifestError("sourceSha must be 40 lowercase hexadecimal characters")
    if expected_source is not None and source_sha != expected_source:
        raise ManifestError(f"sourceSha mismatch: expected {expected_source}, got {source_sha}")
    version = manifest["version"]
    if (
        not isinstance(version, str)
        or not VERSION_RE.fullmatch(version)
        or len(version) > 31
    ):
        raise ManifestError("invalid version")
    if expected_version is not None and version != expected_version:
        raise ManifestError(f"version mismatch: expected {expected_version}, got {version}")

    builds = manifest["builds"]
    if not isinstance(builds, list) or len(builds) != len(TARGETS):
        raise ManifestError("manifest must contain exactly four builds")

    referenced: set[str] = set()
    for build, (chip, target, suffix, boot_offset) in zip(builds, TARGETS, strict=True):
        if not isinstance(build, dict) or set(build) != {"chipFamily", "parts"}:
            raise ManifestError(f"{chip}: build fields must be exactly chipFamily/parts")
        if build["chipFamily"] != chip:
            raise ManifestError(f"build order/chipFamily mismatch: expected {chip}")
        expected_parts = (
            (f"bootloader-{target}.bin", boot_offset, 1, 0x8000 - boot_offset),
            (f"partition-table-{target}.bin", 0x8000, 1, 0x1000),
            (f"tesla-key-esp32{suffix}.bin", 0x20000, 1, 0x1F0000),
            (f"ota_data_initial-{target}.bin", 0xF000, 0x2000, 0x2000),
        )
        parts = build["parts"]
        if not isinstance(parts, list) or len(parts) != 4:
            raise ManifestError(f"{chip}: expected bootloader, partition, app and otadata")
        for part, (name, offset, min_size, max_size) in zip(parts, expected_parts, strict=True):
            if not isinstance(part, dict) or set(part) != {"path", "offset", "size", "sha256"}:
                raise ManifestError(f"{chip}/{name}: invalid part fields")
            if part["path"] != name or pathlib.PurePosixPath(name).name != name:
                raise ManifestError(f"{chip}: unexpected/non-local part path {part['path']!r}")
            if type(part["offset"]) is not int or part["offset"] != offset:
                raise ManifestError(f"{chip}/{name}: expected offset {offset}, got {part['offset']}")
            size = part["size"]
            if not isinstance(size, int) or isinstance(size, bool) or not min_size <= size <= max_size:
                raise ManifestError(f"{chip}/{name}: invalid size {size!r}")
            digest = part["sha256"]
            if not isinstance(digest, str) or not DIGEST.fullmatch(digest):
                raise ManifestError(f"{chip}/{name}: invalid SHA-256")
            path = site / name
            if path.is_symlink() or not path.is_file() or not stat.S_ISREG(path.stat().st_mode):
                raise ManifestError(f"{chip}/{name}: file missing, symlinked or non-regular")
            if path.stat().st_size != size:
                raise ManifestError(f"{chip}/{name}: manifest length does not match file")
            if sha256(path) != digest:
                raise ManifestError(f"{chip}/{name}: manifest SHA-256 does not match file")
            if name == f"ota_data_initial-{target}.bin":
                try:
                    OTADATA_CONTRACT.validate_path(path)
                except OTADATA_CONTRACT.OtadataError as error:
                    raise ManifestError(f"{chip}/{name}: {error}") from error
            if name in referenced:
                raise ManifestError(f"duplicate manifest path: {name}")
            referenced.add(name)

    emitted_bins = {path.name for path in site.glob("*.bin") if path.is_file()}
    if emitted_bins != referenced:
        raise ManifestError(
            f"unreferenced/missing binary files: emitted={sorted(emitted_bins)} referenced={sorted(referenced)}"
        )


def make_test_site(site: pathlib.Path) -> dict:
    """Deterministic, non-flashable self-test data shared with the byte-binding self-test."""
    manifest = {"name": "tesla-key-esp32", "layoutVersion": 2, "sourceSha": "a" * 40,
                "version": "1.2.3", "new_install_prompt_erase": True, "builds": []}
    for chip, target, suffix, boot_offset in (
        ("ESP32", "esp32", "", 0x1000), ("ESP32-S3", "esp32s3", "-s3", 0),
        ("ESP32-C3", "esp32c3", "-c3", 0), ("ESP32-C6", "esp32c6", "-c6", 0),
    ):
        parts = []
        for name, offset, data in (
            (f"bootloader-{target}.bin", boot_offset, b"boot"),
            (f"partition-table-{target}.bin", 0x8000, b"partition"),
            (f"tesla-key-esp32{suffix}.bin", 0x20000, f"app-{target}".encode()),
            (f"ota_data_initial-{target}.bin", 0xF000, b"\xff" * 0x2000),
        ):
            (site / name).write_bytes(data)
            parts.append({"path": name, "offset": offset, "size": len(data),
                          "sha256": hashlib.sha256(data).hexdigest()})
        manifest["builds"].append({"chipFamily": chip, "parts": parts})
    (site / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return manifest


def self_test() -> None:
    import copy
    import tempfile

    with tempfile.TemporaryDirectory(prefix="pages-manifest-test-") as directory:
        site = pathlib.Path(directory)
        original = make_test_site(site)
        path = site / "manifest.json"

        def check() -> None:
            validate(site, "a" * 40, "1.2.3")

        def reject(manifest: dict, reason: str) -> None:
            path.write_text(json.dumps(manifest), encoding="utf-8")
            try:
                check()
            except ManifestError as exc:
                assert reason in str(exc), (reason, str(exc))
            else:
                raise AssertionError(f"Pages manifest accepted: {reason}")
            finally:
                path.write_text(json.dumps(original), encoding="utf-8")

        check()
        for build_index in range(4):
            for part_index in range(4):
                bad = copy.deepcopy(original)
                bad["builds"][build_index]["parts"][part_index]["offset"] += 1
                reject(bad, "expected offset")
        for field, value, reason in (("version", "1.2.4", "version mismatch"),
                                      ("sourceSha", "b" * 40, "sourceSha mismatch"),
                                      ("version", "1.2.3-" + "x" * 26, "invalid version"),
                                      ("name", "other", "name/layoutVersion"),
                                      ("extra", True, "top-level fields")):
            bad = copy.deepcopy(original); bad[field] = value; reject(bad, reason)
        for key, value, reason in (("path", "../app.bin", "part path"),
                                   ("size", True, "invalid size"),
                                   ("size", 99, "length does not match"),
                                   ("sha256", "g" * 64, "invalid SHA-256"),
                                   ("sha256", "0" * 64, "SHA-256 does not match"),
                                   ("extra", 0, "invalid part fields")):
            bad = copy.deepcopy(original); bad["builds"][0]["parts"][0][key] = value
            reject(bad, reason)
        bad = copy.deepcopy(original); bad["builds"][0]["extra"] = 0
        reject(bad, "build fields")
        bad = copy.deepcopy(original); bad["builds"].pop(); reject(bad, "exactly four")
        bad = copy.deepcopy(original); bad["builds"].reverse(); reject(bad, "chipFamily")
        part = original["builds"][0]["parts"][3]
        data_path = site / part["path"]; data = data_path.read_bytes()
        data_path.write_bytes(b"\x00" + data[1:])
        bad = copy.deepcopy(original)
        bad["builds"][0]["parts"][3]["sha256"] = sha256(data_path)
        reject(bad, "erased")
        data_path.write_bytes(data)
        extra = site / "extra.bin"; extra.write_bytes(b"extra")
        reject(original, "unreferenced/missing")
        extra.unlink()
        first = site / original["builds"][0]["parts"][0]["path"]
        data = first.read_bytes(); first.unlink(); first.symlink_to(data_path)
        reject(original, "symlinked")
        first.unlink(); first.write_bytes(data)
        check()
    print("Pages manifest self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("site", type=pathlib.Path, nargs="?")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--source-sha")
    parser.add_argument("--version")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.site is None:
        parser.error("site is required")
    try:
        validate(args.site, args.source_sha, args.version)
    except ManifestError as error:
        print(f"manifest validation failed: {error}", file=sys.stderr)
        return 1
    print(f"manifest validation passed: {args.site}/manifest.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
