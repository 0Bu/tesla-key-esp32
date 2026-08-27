#!/usr/bin/env python3
"""Accept deployed Pages against bytes from an API-confirmed immutable GitHub Release."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, Callable


VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$")
SOURCE_RE = re.compile(r"^[0-9a-f]{40}$")
MANIFEST_MAX = 128 * 1024
FLASH_SIZE = 0x400000
TARGETS = (
    ("ESP32", "esp32", "", 0x1000),
    ("ESP32-S3", "esp32s3", "-s3", 0),
    ("ESP32-C3", "esp32c3", "-c3", 0),
    ("ESP32-C6", "esp32c6", "-c6", 0),
)
Fetcher = Callable[[str, int], bytes]


class AcceptanceError(RuntimeError):
    pass


def exact_url(base: str, name: str, cache_key: str | None = None) -> str:
    parsed = urllib.parse.urlsplit(base)
    if (
        parsed.scheme != "https"
        or not parsed.netloc
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
    ):
        raise AcceptanceError(f"base URL must be credential-free HTTPS without query/fragment: {base!r}")
    if Path(name).name != name or name in {"", ".", ".."}:
        raise AcceptanceError(f"unsafe remote file name: {name!r}")
    path = parsed.path.rstrip("/") + "/" + urllib.parse.quote(name, safe="-._~")
    query = urllib.parse.urlencode({"accept": cache_key}) if cache_key is not None else ""
    return urllib.parse.urlunsplit(("https", parsed.netloc, path, query, ""))


def fetch_https(url: str, max_bytes: int, timeout: float) -> bytes:
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/octet-stream, application/json",
            "Cache-Control": "no-cache",
            "User-Agent": "tesla-key-esp32-release-acceptance/1",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:  # noqa: S310 - HTTPS checked
            final = urllib.parse.urlsplit(response.geturl())
            if final.scheme != "https" or not final.netloc:
                raise AcceptanceError("remote fetch redirected away from HTTPS")
            length_header = response.headers.get("Content-Length")
            if length_header is not None:
                try:
                    declared = int(length_header)
                except ValueError as exc:
                    raise AcceptanceError("remote Content-Length is invalid") from exc
                if declared < 0 or declared > max_bytes:
                    raise AcceptanceError(
                        f"remote Content-Length {declared} exceeds bounded maximum {max_bytes}"
                    )
            data = response.read(max_bytes + 1)
    except (OSError, urllib.error.URLError) as exc:
        raise AcceptanceError(f"remote fetch failed for {url}: {exc}") from exc
    if len(data) > max_bytes:
        raise AcceptanceError(f"remote response exceeds bounded maximum {max_bytes}: {url}")
    return data


def parse_ready_manifest(raw: bytes, version: str, source_sha: str) -> tuple[dict[str, Any], list[tuple[str, int]]]:
    try:
        manifest = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise AcceptanceError(f"deployed manifest is not valid JSON: {exc}") from exc
    if not isinstance(manifest, dict):
        raise AcceptanceError("deployed manifest root is not an object")
    if manifest.get("version") != version or manifest.get("sourceSha") != source_sha:
        raise AcceptanceError("deployed manifest has not reached the expected release identity")
    builds = manifest.get("builds")
    if not isinstance(builds, list) or len(builds) != len(TARGETS):
        raise AcceptanceError("deployed manifest does not contain exactly four builds")
    downloads: list[tuple[str, int]] = []
    for build, (family, target, suffix, _boot_offset) in zip(builds, TARGETS, strict=True):
        if not isinstance(build, dict) or build.get("chipFamily") != family:
            raise AcceptanceError(f"deployed manifest build order/family drifted at {family}")
        parts = build.get("parts")
        names = (
            f"bootloader-{target}.bin",
            f"partition-table-{target}.bin",
            f"tesla-key-esp32{suffix}.bin",
            f"ota_data_initial-{target}.bin",
        )
        if not isinstance(parts, list) or len(parts) != len(names):
            raise AcceptanceError(f"{family} deployed manifest does not contain four parts")
        for part, name in zip(parts, names, strict=True):
            if not isinstance(part, dict) or part.get("path") != name:
                raise AcceptanceError(f"{family} deployed manifest part path drifted: {name}")
            size = part.get("size")
            if not isinstance(size, int) or isinstance(size, bool) or not 1 <= size <= 0x1F0000:
                raise AcceptanceError(f"{family}/{name} deployed size is invalid")
            if name.startswith("ota_data_initial-") and size != 0x2000:
                raise AcceptanceError(f"{family}/{name} must be exactly 0x2000 bytes")
            downloads.append((name, size))
    return manifest, downloads


def load_module(path: Path, name: str) -> Any:
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise AcceptanceError(f"cannot load trusted validator: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def verify_snapshot(
    pages_base: str,
    release_base: str,
    version: str,
    source_sha: str,
    release_metadata: Any,
    *,
    attempts: int,
    interval: float,
    fetcher: Fetcher,
    sleeper: Callable[[float], None] = time.sleep,
) -> int:
    if (
        not VERSION_RE.fullmatch(version)
        or len(version) > 31
        or not SOURCE_RE.fullmatch(source_sha)
    ):
        raise AcceptanceError("invalid expected release version or source SHA")
    if not 1 <= attempts <= 20 or not 0 <= interval <= 60:
        raise AcceptanceError("acceptance polling bounds are invalid")

    scripts_dir = Path(__file__).resolve().parent
    release_validator = load_module(scripts_dir / "check-release-assets.py", "release_asset_validator")
    try:
        release_validator.validate_metadata(release_metadata, version, source_sha)
    except (TypeError, ValueError, RuntimeError) as exc:
        raise AcceptanceError(f"published Release metadata is not authoritative: {exc}") from exc

    manifest_raw: bytes | None = None
    manifest: dict[str, Any] | None = None
    downloads: list[tuple[str, int]] | None = None
    last_error: Exception | None = None
    for attempt in range(1, attempts + 1):
        url = exact_url(pages_base, "manifest.json", f"{source_sha}-{attempt}")
        try:
            candidate = fetcher(url, MANIFEST_MAX)
            parsed, planned = parse_ready_manifest(candidate, version, source_sha)
            manifest_raw, manifest, downloads = candidate, parsed, planned
            break
        except AcceptanceError as exc:
            last_error = exc
            if attempt < attempts:
                sleeper(interval)
    if manifest_raw is None or manifest is None or downloads is None:
        raise AcceptanceError(
            f"deployed manifest did not converge after {attempts} bounded attempts: {last_error}"
        )

    manifest_validator = load_module(scripts_dir / "check-pages-manifest.py", "pages_manifest_validator")
    byte_validator = load_module(scripts_dir / "check-release-pages-bytes.py", "release_pages_validator")
    with tempfile.TemporaryDirectory(prefix="tesla-published-acceptance-") as directory:
        root = Path(directory)
        site = root / "site"
        release = root / "release"
        site.mkdir()
        release.mkdir()
        (site / "manifest.json").write_bytes(manifest_raw)
        for name, expected_size in downloads:
            data = fetcher(exact_url(pages_base, name, source_sha), expected_size)
            if len(data) != expected_size:
                raise AcceptanceError(
                    f"deployed Pages part {name} length differs: expected={expected_size} actual={len(data)}"
                )
            (site / name).write_bytes(data)
        for _family, _target, suffix, _boot_offset in TARGETS:
            name = f"tesla-key-esp32{suffix}-{version}-merged.bin"
            data = fetcher(exact_url(release_base, name), FLASH_SIZE)
            if not data:
                raise AcceptanceError(f"published Release asset is empty: {name}")
            (release / name).write_bytes(data)

        try:
            manifest_validator.validate(site, source_sha, version)
            count = byte_validator.verify(site, release, version)
            if release_validator.validate(release_metadata, release, version, source_sha) != len(TARGETS):
                raise AcceptanceError("published Release metadata did not bind exactly four assets")
        except (OSError, ValueError, RuntimeError) as exc:
            raise AcceptanceError(f"deployed Release/Pages byte acceptance failed: {exc}") from exc
    return count


def self_test() -> None:
    version = "1.2.3"
    source_sha = "0123456789abcdef0123456789abcdef01234567"
    pages_base = "https://pages.example.invalid/firmware"
    release_base = "https://release.example.invalid/v1.2.3"
    try:
        verify_snapshot(
            pages_base,
            release_base,
            "01.2.3",
            source_sha,
            {},
            attempts=1,
            interval=0,
            fetcher=lambda _url, _timeout: b"",
        )
    except AcceptanceError:
        pass
    else:
        raise AssertionError("non-canonical leading-zero release version was accepted")
    mappings: dict[str, bytes] = {}
    builds: list[dict[str, Any]] = []
    release_assets: list[dict[str, Any]] = []
    for index, (family, target, suffix, boot_offset) in enumerate(TARGETS):
        parts = []
        definitions = (
            (f"bootloader-{target}.bin", boot_offset, bytes([0x10 + index]) * 64),
            (f"partition-table-{target}.bin", 0x8000, bytes([0x20 + index]) * 32),
            (f"tesla-key-esp32{suffix}.bin", 0x20000, bytes([0x30 + index]) * 128),
            (f"ota_data_initial-{target}.bin", 0xF000, b"\xff" * 0x2000),
        )
        merged_end = max(offset + len(data) for _name, offset, data in definitions)
        merged = bytearray(b"\xff" * merged_end)
        for name, offset, data in definitions:
            mappings[urllib.parse.urlsplit(exact_url(pages_base, name, source_sha)).path] = data
            merged[offset : offset + len(data)] = data
            parts.append(
                {
                    "path": name,
                    "offset": offset,
                    "size": len(data),
                    "sha256": hashlib.sha256(data).hexdigest(),
                }
            )
        merged_name = f"tesla-key-esp32{suffix}-{version}-merged.bin"
        merged_bytes = bytes(merged)
        mappings[urllib.parse.urlsplit(exact_url(release_base, merged_name)).path] = merged_bytes
        release_assets.append(
            {
                "id": index + 1,
                "name": merged_name,
                "size": len(merged_bytes),
                "digest": f"sha256:{hashlib.sha256(merged_bytes).hexdigest()}",
            }
        )
        builds.append({"chipFamily": family, "parts": parts})
    release_metadata = {
        "tag_name": f"v{version}",
        "target_commitish": source_sha,
        "draft": False,
        "prerelease": False,
        "immutable": True,
        "assets": release_assets,
    }
    manifest = {
        "name": "tesla-key-esp32",
        "layoutVersion": 2,
        "sourceSha": source_sha,
        "version": version,
        "new_install_prompt_erase": True,
        "builds": builds,
    }
    mappings[urllib.parse.urlsplit(exact_url(pages_base, "manifest.json", source_sha)).path] = (
        json.dumps(manifest).encode("utf-8")
    )

    def fake_fetch(url: str, maximum: int) -> bytes:
        path = urllib.parse.urlsplit(url).path
        data = mappings.get(path)
        if data is None or len(data) > maximum:
            raise AcceptanceError(f"unexpected/bounded fake fetch: {url}")
        return data

    assert verify_snapshot(
        pages_base,
        release_base,
        version,
        source_sha,
        release_metadata,
        attempts=2,
        interval=0,
        fetcher=fake_fetch,
    ) == 16

    for label, value in (("false", False), ("missing", None)):
        invalid_release = json.loads(json.dumps(release_metadata))
        if value is None:
            del invalid_release["immutable"]
        else:
            invalid_release["immutable"] = value
        try:
            verify_snapshot(
                pages_base,
                release_base,
                version,
                source_sha,
                invalid_release,
                attempts=1,
                interval=0,
                fetcher=fake_fetch,
            )
        except AcceptanceError:
            pass
        else:
            raise AssertionError(f"immutable={label} Release metadata was accepted")

    manifest_path = urllib.parse.urlsplit(
        exact_url(pages_base, "manifest.json", source_sha)
    ).path
    ready_manifest = mappings[manifest_path]
    stale_manifest = json.loads(ready_manifest)
    stale_manifest["sourceSha"] = "f" * 40
    mappings[manifest_path] = json.dumps(stale_manifest).encode("utf-8")
    sleeps: list[float] = []
    try:
        verify_snapshot(
            pages_base,
            release_base,
            version,
            source_sha,
            release_metadata,
            attempts=3,
            interval=0.25,
            fetcher=fake_fetch,
            sleeper=sleeps.append,
        )
    except AcceptanceError:
        pass
    else:
        raise AssertionError("stale deployed manifest was accepted")
    assert sleeps == [0.25, 0.25], "manifest polling did not stop at the configured bound"
    mappings[manifest_path] = ready_manifest

    first_release = urllib.parse.urlsplit(
        exact_url(release_base, f"tesla-key-esp32-{version}-merged.bin")
    ).path
    tampered = bytearray(mappings[first_release])
    tampered[0x20000] ^= 1
    mappings[first_release] = bytes(tampered)
    try:
        verify_snapshot(
            pages_base,
            release_base,
            version,
            source_sha,
            release_metadata,
            attempts=1,
            interval=0,
            fetcher=fake_fetch,
        )
    except AcceptanceError:
        pass
    else:
        raise AssertionError("deployed Release/Pages byte substitution was accepted")

    try:
        exact_url("http://pages.example.invalid", "manifest.json")
    except AcceptanceError:
        pass
    else:
        raise AssertionError("non-HTTPS acceptance endpoint was accepted")
    print("published Release/Pages acceptance self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pages-base-url")
    parser.add_argument("--release-base-url")
    parser.add_argument("--release-json", type=Path)
    parser.add_argument("--version")
    parser.add_argument("--source-sha")
    parser.add_argument("--attempts", type=int, default=6)
    parser.add_argument("--interval", type=float, default=10.0)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if None in (
        args.pages_base_url,
        args.release_base_url,
        args.release_json,
        args.version,
        args.source_sha,
    ):
        parser.error("Pages/Release URLs and JSON, --version and --source-sha are required")
    if not 1 <= args.timeout <= 60:
        parser.error("--timeout must be between 1 and 60 seconds")

    def network_fetch(url: str, maximum: int) -> bytes:
        last_error: AcceptanceError | None = None
        for attempt in range(1, 4):
            try:
                return fetch_https(url, maximum, args.timeout)
            except AcceptanceError as exc:
                last_error = exc
                if attempt < 3:
                    time.sleep(min(2.0 * attempt, 5.0))
        raise AcceptanceError(f"remote object fetch failed after three attempts: {last_error}")

    try:
        if args.release_json.is_symlink() or not args.release_json.is_file():
            raise AcceptanceError("Release JSON must be a regular non-symlink file")
        release_metadata = json.loads(args.release_json.read_text(encoding="utf-8"))
        count = verify_snapshot(
            args.pages_base_url,
            args.release_base_url,
            args.version,
            args.source_sha,
            release_metadata,
            attempts=args.attempts,
            interval=args.interval,
            fetcher=network_fetch,
        )
    except (OSError, UnicodeError, json.JSONDecodeError, AcceptanceError) as exc:
        print(f"published Release/Pages acceptance failed: {exc}", file=sys.stderr)
        return 1
    print(f"published Release/Pages acceptance: PASS ({count}/16 byte-bound parts)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
