#!/usr/bin/env python3
"""Bind every Pages firmware part byte-for-byte to the exact GitHub Release merged assets."""

from __future__ import annotations

import argparse
import importlib.util
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


def load_otadata_contract():
    path = Path(__file__).with_name("check-otadata-contract.py")
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"missing/unsafe shared otadata validator: {path}")
    spec = importlib.util.spec_from_file_location("tesla_key_otadata_contract", path)
    if spec is None or spec.loader is None:
        raise ValueError(f"cannot load shared otadata validator: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


OTADATA_CONTRACT = load_otadata_contract()


def regular_file(path: Path, label: str) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"{label} is missing, non-regular or a symlink: {path}")
    return path.read_bytes()


def verify(site: Path, release_dir: Path, version: str) -> int:
    if not VERSION_RE.fullmatch(version) or len(version) > 31:
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
        if any(type(offset) is not int for offset in offsets) or offsets != EXPECTED_OFFSETS[family]:
            raise ValueError(f"{family} manifest part offsets/order drifted: {offsets!r}")

        declared_ranges: list[tuple[int, int, str]] = []
        for part in parts:
            name = part.get("path")
            size = part.get("size")
            offset = part.get("offset")
            if not isinstance(name, str) or Path(name).name != name:
                raise ValueError(f"unsafe Pages part path: {name!r}")
            if type(size) is not int or size <= 0 or type(offset) is not int or offset < 0:
                raise ValueError(f"invalid Pages part range for {name!r}")
            page_bytes = regular_file(site / name, f"Pages part {name}")
            if len(page_bytes) != size:
                raise ValueError(f"Pages part {name} length differs from manifest")
            if name.startswith("ota_data_initial-"):
                try:
                    OTADATA_CONTRACT.validate_bytes(page_bytes, f"Pages part {name}")
                except OTADATA_CONTRACT.OtadataError as error:
                    raise ValueError(str(error)) from error
            release_bytes = merged[offset : offset + size]
            if len(release_bytes) != size:
                raise ValueError(f"Release asset {merged_name} is too short for {name}@{offset}")
            if page_bytes != release_bytes:
                raise ValueError(
                    f"Pages part {name} differs from Release asset {merged_name}@{offset}"
                )
            declared_ranges.append((offset, offset + size, name))
            compared += 1

        # merge_bin's complete byte contract is minimal and erased outside the four declared
        # payloads. Checking only slices would allow attacker-controlled NVS/gap or trailing bytes
        # to ride inside an otherwise valid Release asset.
        ordered_ranges = sorted(declared_ranges)
        cursor = 0
        for start, end, name in ordered_ranges:
            if start < cursor:
                raise ValueError(f"{family} merged ranges overlap at {name}@{start}")
            gap = merged[cursor:start]
            bad = next((index for index, value in enumerate(gap) if value != 0xFF), None)
            if bad is not None:
                absolute = cursor + bad
                raise ValueError(
                    f"{family} undeclared merged byte at 0x{absolute:x} is not erased 0xff"
                )
            cursor = end
        if len(merged) != cursor:
            raise ValueError(
                f"{family} merged asset length must end exactly at 0x{cursor:x}, "
                f"got 0x{len(merged):x}"
            )

    if seen != set(MERGED_NAMES):
        raise ValueError(f"Pages chipFamily set is incomplete: {sorted(seen)!r}")
    return compared


def self_test() -> None:
    import tempfile

    # Reuse only deterministic fixture construction, never the local validator's verdict.
    spec = importlib.util.spec_from_file_location(
        "pages_fixture", Path(__file__).with_name("check-pages-manifest.py"))
    assert spec is not None and spec.loader is not None
    fixture = importlib.util.module_from_spec(spec); spec.loader.exec_module(fixture)
    with tempfile.TemporaryDirectory(prefix="release-pages-test-") as directory:
        root = Path(directory); site = root / "site"; release = root / "release"
        site.mkdir(); release.mkdir()
        manifest = fixture.make_test_site(site)
        manifest_path = site / "manifest.json"
        originals = {}
        for build in manifest["builds"]:
            merged = bytearray(b"\xff" * max(p["offset"] + p["size"] for p in build["parts"]))
            for part in build["parts"]:
                merged[part["offset"]:part["offset"] + part["size"]] = (site / part["path"]).read_bytes()
            name = MERGED_NAMES[build["chipFamily"]].format(version="1.2.3")
            originals[name] = bytes(merged); (release / name).write_bytes(merged)
        assert verify(site, release, "1.2.3") == 16

        def reject(reason: str) -> None:
            try:
                verify(site, release, "1.2.3")
            except ValueError as exc:
                assert reason in str(exc), (reason, str(exc))
            else:
                raise AssertionError(f"Release/Pages accepted: {reason}")

        for build_index in range(4):
            for part_index in range(4):
                bad = json.loads(json.dumps(manifest))
                bad["builds"][build_index]["parts"][part_index]["offset"] += 1
                manifest_path.write_text(json.dumps(bad), encoding="utf-8")
                reject("offsets/order")
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        name = MERGED_NAMES["ESP32"].format(version="1.2.3"); merged_path = release / name
        original = originals[name]
        for offset, reason in ((0x1000, "differs from Release"), (0x9000, "not erased")):
            changed = bytearray(original); changed[offset] ^= 1; merged_path.write_bytes(changed)
            reject(reason)
        merged_path.write_bytes(original + b"\xff"); reject("length must end exactly")
        merged_path.write_bytes(original[:0x20000]); reject("too short")
        merged_path.write_bytes(original)
        page = site / manifest["builds"][0]["parts"][2]["path"]
        old = page.read_bytes(); page.write_bytes(b"X" + old[1:])
        # Updating the manifest digest makes this a locally valid Pages site, but it must still
        # fail the independent Release binding.
        bad = json.loads(json.dumps(manifest))
        import hashlib
        bad["builds"][0]["parts"][2]["sha256"] = hashlib.sha256(page.read_bytes()).hexdigest()
        manifest_path.write_text(json.dumps(bad), encoding="utf-8")
        fixture.validate(site, "a" * 40, "1.2.3")
        reject("differs from Release")
        page.write_bytes(old); manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        merged_path.unlink(); merged_path.symlink_to(page); reject("symlink")
        merged_path.unlink(); merged_path.write_bytes(original)
        assert verify(site, release, "1.2.3") == 16
    print("Release/Pages byte binding self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("site", type=Path, nargs="?")
    parser.add_argument("release_dir", type=Path, nargs="?")
    parser.add_argument("--version")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if None in (args.site, args.release_dir, args.version):
        parser.error("site, release_dir and --version are required")
    try:
        count = verify(args.site, args.release_dir, args.version)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"release/Pages byte binding failed: {exc}", file=sys.stderr)
        return 1
    print(f"Release/Pages byte binding: PASS ({count}/16 parts)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
