#!/usr/bin/env python3
"""Bind every Pages firmware part byte-for-byte to the exact GitHub Release merged assets."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$")
MERGED_NAMES = {
    "ESP32": "tesla-key-esp32-{version}-merged.bin",
    "ESP32-S3": "tesla-key-esp32-s3-{version}-merged.bin",
    "ESP32-C3": "tesla-key-esp32-c3-{version}-merged.bin",
    "ESP32-C6": "tesla-key-esp32-c6-{version}-merged.bin",
}
EXPECTED_OFFSETS = {
    "ESP32": (0x1000, 0x8000, 0x20000, 0xF000),
    "ESP32-S3": (0, 0x8000, 0x20000, 0xF000),
    "ESP32-C3": (0, 0x8000, 0x20000, 0xF000),
    "ESP32-C6": (0, 0x8000, 0x20000, 0xF000),
}


def regular_file(path: Path, label: str) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"{label} is missing, non-regular or a symlink: {path}")
    return path.read_bytes()


def verify(site: Path, release_dir: Path, version: str) -> int:
    if not VERSION_RE.fullmatch(version):
        raise ValueError(f"invalid canonical release version: {version}")
    manifest_path = site / "manifest.json"
    manifest = json.loads(regular_file(manifest_path, "Pages manifest"))
    if manifest.get("layoutVersion") != 2 or manifest.get("version") != version:
        raise ValueError("Pages manifest layout/version is not bound to the requested Release")
    builds = manifest.get("builds")
    if not isinstance(builds, list) or len(builds) != 4:
        raise ValueError("Pages manifest must contain exactly four builds")

    seen: set[str] = set()
    compared = 0
    for build in builds:
        if not isinstance(build, dict):
            raise ValueError("invalid Pages build entry")
        family = build.get("chipFamily")
        if family not in MERGED_NAMES or family in seen:
            raise ValueError(f"missing, duplicate or unexpected chipFamily: {family!r}")
        seen.add(family)
        merged_name = MERGED_NAMES[family].format(version=version)
        merged = regular_file(release_dir / merged_name, f"Release asset {merged_name}")
        parts = build.get("parts")
        if not isinstance(parts, list) or len(parts) != 4:
            raise ValueError(f"{family} must contain exactly four manifest parts")
        offsets = tuple(part.get("offset") for part in parts if isinstance(part, dict))
        if offsets != EXPECTED_OFFSETS[family]:
            raise ValueError(f"{family} manifest part offsets/order drifted: {offsets!r}")

        for part in parts:
            name = part.get("path")
            size = part.get("size")
            offset = part.get("offset")
            if not isinstance(name, str) or Path(name).name != name:
                raise ValueError(f"unsafe Pages part path: {name!r}")
            if not isinstance(size, int) or size <= 0 or not isinstance(offset, int) or offset < 0:
                raise ValueError(f"invalid Pages part range for {name!r}")
            page_bytes = regular_file(site / name, f"Pages part {name}")
            if len(page_bytes) != size:
                raise ValueError(f"Pages part {name} length differs from manifest")
            release_bytes = merged[offset : offset + size]
            if len(release_bytes) != size:
                raise ValueError(f"Release asset {merged_name} is too short for {name}@{offset}")
            if page_bytes != release_bytes:
                raise ValueError(
                    f"Pages part {name} differs from Release asset {merged_name}@{offset}"
                )
            compared += 1

    if seen != set(MERGED_NAMES):
        raise ValueError(f"Pages chipFamily set is incomplete: {sorted(seen)!r}")
    return compared


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("site", type=Path)
    parser.add_argument("release_dir", type=Path)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    try:
        count = verify(args.site, args.release_dir, args.version)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"release/Pages byte binding failed: {exc}", file=sys.stderr)
        return 1
    print(f"Release/Pages byte binding: PASS ({count}/16 parts)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
