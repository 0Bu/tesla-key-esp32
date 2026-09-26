#!/usr/bin/env python3
"""Accept deployed dev Pages against staged local bytes from trusted build."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, Callable


VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$")
SOURCE_RE = re.compile(r"^[0-9a-f]{40}$")
MANIFEST_MAX = 128 * 1024
PART_MAX = 4 * 1024 * 1024
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
            "User-Agent": "tesla-key-esp32-dev-pages-acceptance/1",
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


def parse_dev_manifest(raw: bytes, version: str, source_sha: str) -> list[str]:
    try:
        manifest = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise AcceptanceError(f"deployed dev manifest is not valid JSON: {exc}") from exc
    if not isinstance(manifest, dict):
        raise AcceptanceError("deployed dev manifest root is not an object")
    if manifest.get("version") != version or manifest.get("sourceSha") != source_sha:
        raise AcceptanceError("deployed dev manifest has not reached expected identity")
    builds = manifest.get("builds")
    if not isinstance(builds, list) or len(builds) != 4:
        raise AcceptanceError("deployed dev manifest does not contain exactly four builds")
    parts: list[str] = []
    for build in builds:
        if not isinstance(build, dict):
            raise AcceptanceError("invalid build entry in dev manifest")
        build_parts = build.get("parts")
        if not isinstance(build_parts, list):
            raise AcceptanceError("invalid parts in dev manifest build")
        for part in build_parts:
            if not isinstance(part, dict):
                raise AcceptanceError("invalid part entry in dev manifest")
            path = part.get("path")
            if not isinstance(path, str) or not path or Path(path).name != path:
                raise AcceptanceError(f"invalid part path in dev manifest: {path!r}")
            parts.append(path)
    return parts


def verify_dev_pages(
    pages_base_url: str,
    staged_site: Path,
    version: str,
    source_sha: str,
    attempts: int,
    interval: float,
    fetcher: Fetcher,
    sleeper: Callable[[float], None] = time.sleep,
) -> int:
    if not VERSION_RE.match(version):
        raise AcceptanceError(f"invalid version format: {version!r}")
    if not SOURCE_RE.match(source_sha):
        raise AcceptanceError(f"invalid source SHA format: {source_sha!r}")
    if staged_site.is_symlink() or not staged_site.is_dir():
        raise AcceptanceError(f"staged site must be a valid directory: {staged_site}")

    dev_base = pages_base_url.rstrip("/") + "/dev"
    manifest_url = exact_url(dev_base, "manifest.json", source_sha)

    parts: list[str] | None = None
    last_error: AcceptanceError | None = None

    for attempt in range(1, attempts + 1):
        try:
            raw_manifest = fetcher(manifest_url, MANIFEST_MAX)
            parts = parse_dev_manifest(raw_manifest, version, source_sha)
            break
        except AcceptanceError as exc:
            last_error = exc
            if attempt < attempts:
                sleeper(interval)

    if parts is None:
        raise AcceptanceError(f"dev manifest failed to settle after {attempts} attempts: {last_error}")

    verified_count = 0
    seen_paths: set[str] = set()
    for part_name in parts:
        if part_name in seen_paths:
            continue
        seen_paths.add(part_name)
        local_path = staged_site / part_name
        if local_path.is_symlink() or not local_path.is_file():
            raise AcceptanceError(f"missing local staged part: {local_path}")
        local_bytes = local_path.read_bytes()
        part_url = exact_url(dev_base, part_name, source_sha)
        remote_bytes = fetcher(part_url, PART_MAX)
        if hashlib.sha256(remote_bytes).digest() != hashlib.sha256(local_bytes).digest():
            raise AcceptanceError(f"byte mismatch for dev Pages part {part_name}")
        verified_count += 1

    return verified_count


def self_test() -> None:
    import tempfile

    version = "1.5.0-dev.1"
    source_sha = "a" * 40
    pages_base = "https://0bu.github.io/tesla-key-esp32"
    dev_base = pages_base + "/dev"

    manifest_data = {
        "name": "Tesla BLE Key (ESP32)",
        "version": version,
        "sourceSha": source_sha,
        "builds": [
            {"chipFamily": "ESP32", "parts": [{"path": "bootloader.bin", "offset": 4096}, {"path": "app.bin", "offset": 131072}]},
            {"chipFamily": "ESP32-S3", "parts": [{"path": "bootloader-s3.bin", "offset": 0}, {"path": "app-s3.bin", "offset": 131072}]},
            {"chipFamily": "ESP32-C3", "parts": [{"path": "bootloader-c3.bin", "offset": 0}, {"path": "app-c3.bin", "offset": 131072}]},
            {"chipFamily": "ESP32-C6", "parts": [{"path": "bootloader-c6.bin", "offset": 0}, {"path": "app-c6.bin", "offset": 131072}]},
        ],
    }

    with tempfile.TemporaryDirectory(prefix="dev-pages-test-") as tmpdir:
        staged = Path(tmpdir)
        (staged / "manifest.json").write_text(json.dumps(manifest_data), encoding="utf-8")
        files = {
            "bootloader.bin": b"bl_esp32",
            "app.bin": b"app_esp32",
            "bootloader-s3.bin": b"bl_s3",
            "app-s3.bin": b"app_s3",
            "bootloader-c3.bin": b"bl_c3",
            "app-c3.bin": b"app_c3",
            "bootloader-c6.bin": b"bl_c6",
            "app-c6.bin": b"app_c6",
        }
        for name, content in files.items():
            (staged / name).write_bytes(content)

        remote_files: dict[str, bytes] = {
            urllib.parse.urlsplit(exact_url(dev_base, "manifest.json", source_sha)).path: json.dumps(manifest_data).encode("utf-8")
        }
        for name, content in files.items():
            remote_files[urllib.parse.urlsplit(exact_url(dev_base, name, source_sha)).path] = content

        def fake_fetch(url: str, max_bytes: int) -> bytes:
            path = urllib.parse.urlsplit(url).path
            if path in remote_files:
                data = remote_files[path]
                if len(data) > max_bytes:
                    raise AcceptanceError(f"exceeds max bytes: {len(data)} > {max_bytes}")
                return data
            raise AcceptanceError(f"404 not found: {url}")

        count = verify_dev_pages(
            pages_base,
            staged,
            version,
            source_sha,
            attempts=2,
            interval=0,
            fetcher=fake_fetch,
        )
        assert count == 8, f"expected 8 verified parts, got {count}"

        # Tampered content
        manifest_url_path = urllib.parse.urlsplit(exact_url(dev_base, "manifest.json", source_sha)).path
        remote_files[manifest_url_path] = json.dumps({**manifest_data, "sourceSha": "b" * 40}).encode("utf-8")
        try:
            verify_dev_pages(pages_base, staged, version, source_sha, attempts=1, interval=0, fetcher=fake_fetch)
        except AcceptanceError:
            pass
        else:
            raise AssertionError("stale sourceSha dev manifest was accepted")

    print("check-dev-pages self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pages-base-url")
    parser.add_argument("--staged-site", type=Path)
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

    if None in (args.pages_base_url, args.staged_site, args.version, args.source_sha):
        parser.error("--pages-base-url, --staged-site, --version and --source-sha are required")

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
        count = verify_dev_pages(
            args.pages_base_url,
            args.staged_site,
            args.version,
            args.source_sha,
            attempts=args.attempts,
            interval=args.interval,
            fetcher=network_fetch,
        )
    except (OSError, UnicodeError, AcceptanceError) as exc:
        print(f"dev Pages acceptance failed: {exc}", file=sys.stderr)
        return 1
    print(f"dev Pages acceptance: PASS ({count} byte-bound parts)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
