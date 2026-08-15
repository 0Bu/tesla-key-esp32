#!/usr/bin/env python3
"""Fail-closed validation for the signed four-target Web Serial manifest."""

from __future__ import annotations

import argparse
import hashlib
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


class ManifestError(RuntimeError):
    pass


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
    if not isinstance(version, str) or not re.fullmatch(
        r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?", version
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
            if part["offset"] != offset:
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
            if name in referenced:
                raise ManifestError(f"duplicate manifest path: {name}")
            referenced.add(name)

    emitted_bins = {path.name for path in site.glob("*.bin") if path.is_file()}
    if emitted_bins != referenced:
        raise ManifestError(
            f"unreferenced/missing binary files: emitted={sorted(emitted_bins)} referenced={sorted(referenced)}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("site", type=pathlib.Path)
    parser.add_argument("--source-sha")
    parser.add_argument("--version")
    args = parser.parse_args()
    try:
        validate(args.site, args.source_sha, args.version)
    except ManifestError as error:
        print(f"manifest validation failed: {error}", file=sys.stderr)
        return 1
    print(f"manifest validation passed: {args.site}/manifest.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
