#!/usr/bin/env python3
"""Accept deployed dev Pages against staged local bytes from trusted build, or verify that the
live dev channel serves exactly the bytes its own manifest describes (--verify-live)."""

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
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
MANIFEST_MAX = 128 * 1024
PART_MAX = 4 * 1024 * 1024
Fetcher = Callable[[str, int], bytes]

TARGETS = (
    ("ESP32", "esp32", "", 0x1000),
    ("ESP32-S3", "esp32s3", "-s3", 0),
    ("ESP32-C3", "esp32c3", "-c3", 0),
    ("ESP32-C6", "esp32c6", "-c6", 0),
)


def expected_build_parts(target: str, suffix: str, boot_offset: int) -> tuple[tuple[str, int, int, int], ...]:
    return (
        (f"bootloader-{target}.bin", boot_offset, 1, 0x8000 - boot_offset),
        (f"partition-table-{target}.bin", 0x8000, 1, 0x1000),
        (f"tesla-key-esp32{suffix}.bin", 0x20000, 1, 0x1F0000),
        (f"ota_data_initial-{target}.bin", 0xF000, 0x2000, 0x2000),
    )


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


def parse_dev_manifest_parts(raw: bytes, version: str, source_sha: str) -> list[tuple[str, int, str]]:
    """Return (path, size, sha256) for every part of the four builds; the manifest is the contract.

    Enforces exactly the four targets in TARGETS order, each with the four canonical parts in
    expected_build_parts order, matching paths, offsets, size bounds, and SHA-256.
    """
    try:
        manifest = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise AcceptanceError(f"deployed dev manifest is not valid JSON: {exc}") from exc
    if not isinstance(manifest, dict):
        raise AcceptanceError("deployed dev manifest root is not an object")
    if manifest.get("version") != version or manifest.get("sourceSha") != source_sha:
        raise AcceptanceError("deployed dev manifest has not reached expected identity")
    builds = manifest.get("builds")
    if not isinstance(builds, list) or len(builds) != len(TARGETS):
        raise AcceptanceError("deployed dev manifest does not contain exactly four builds")
    parts: list[tuple[str, int, str]] = []
    for build, (chip, target, suffix, boot_offset) in zip(builds, TARGETS, strict=True):
        if not isinstance(build, dict):
            raise AcceptanceError("invalid build entry in dev manifest")
        if build.get("chipFamily") != chip:
            raise AcceptanceError(f"build order/chipFamily mismatch: expected {chip}, got {build.get('chipFamily')!r}")
        build_parts = build.get("parts")
        if not isinstance(build_parts, list) or len(build_parts) != 4:
            raise AcceptanceError(f"each dev manifest build must carry exactly four parts for {chip}")
        expected_parts = expected_build_parts(target, suffix, boot_offset)
        for part, (expected_name, expected_offset, min_size, max_size) in zip(build_parts, expected_parts, strict=True):
            if not isinstance(part, dict):
                raise AcceptanceError(f"invalid part entry in dev manifest for {chip}")
            path = part.get("path")
            offset = part.get("offset")
            size = part.get("size")
            digest = part.get("sha256")
            if path != expected_name or Path(path).name != path:
                raise AcceptanceError(f"unexpected part path for {chip}: expected {expected_name!r}, got {path!r}")
            if offset != expected_offset:
                raise AcceptanceError(f"unexpected offset for {chip}/{path}: expected {expected_offset}, got {offset}")
            if isinstance(size, bool) or not isinstance(size, int) or size < min_size or size > max_size:
                raise AcceptanceError(f"invalid part size in dev manifest for {chip}/{path}: {size!r}")
            if not isinstance(digest, str) or not SHA256_RE.match(digest):
                raise AcceptanceError(f"invalid part sha256 in dev manifest for {chip}/{path}")
            parts.append((path, size, digest))
    return parts


def parse_dev_manifest(raw: bytes, version: str, source_sha: str) -> list[str]:
    parts = parse_dev_manifest_parts(raw, version, source_sha)
    return [p[0] for p in parts]


def verify_dev_channel_live(
    pages_base_url: str,
    version: str,
    source_sha: str,
    attempts: int,
    interval: float,
    fetcher: Fetcher,
    sleeper: Callable[[float], None] = time.sleep,
) -> int:
    """Fetch the served dev manifest and EVERY part; each must match its declared size and SHA-256.

    A dev baseline whose manifest identity is current but whose served binary is old or damaged
    (a lagging or partially propagated deploy) is not a baseline; the caller must keep the
    reconciliation open instead of skipping it.
    """
    if not VERSION_RE.match(version):
        raise AcceptanceError(f"invalid version format: {version!r}")
    if not SOURCE_RE.match(source_sha):
        raise AcceptanceError(f"invalid source SHA format: {source_sha!r}")
    dev_base = pages_base_url.rstrip("/") + "/dev"
    manifest_url = exact_url(dev_base, "manifest.json", source_sha)

    parts: list[tuple[str, int, str]] | None = None
    last_error: AcceptanceError | None = None
    for attempt in range(1, attempts + 1):
        try:
            parts = parse_dev_manifest_parts(fetcher(manifest_url, MANIFEST_MAX), version, source_sha)
            break
        except AcceptanceError as exc:
            last_error = exc
            if attempt < attempts:
                sleeper(interval)
    if parts is None:
        raise AcceptanceError(f"dev manifest failed to settle after {attempts} attempts: {last_error}")
    if len(parts) != 16:
        raise AcceptanceError(f"dev manifest must contain exactly 16 parts, got {len(parts)}")

    verified_count = 0
    for path, size, digest in parts:
        data = fetcher(exact_url(dev_base, path, source_sha), PART_MAX)
        if len(data) != size:
            raise AcceptanceError(f"dev Pages part {path} is {len(data)} bytes, manifest declares {size}")
        if hashlib.sha256(data).hexdigest() != digest:
            raise AcceptanceError(f"dev Pages part {path} does not match its manifest SHA-256")
        verified_count += 1
    return verified_count


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
    if len(parts) != 16:
        raise AcceptanceError(f"dev manifest must contain exactly 16 parts, got {len(parts)}")

    verified_count = 0
    for part_name in parts:
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

    def build_test_manifest(file_dict: dict[str, bytes], overrides: dict[str, dict[str, Any]] | None = None) -> dict[str, Any]:
        builds = []
        for chip, target, suffix, boot_offset in TARGETS:
            parts = []
            for name, offset, min_size, max_size in expected_build_parts(target, suffix, boot_offset):
                content = file_dict[name]
                part_entry: dict[str, Any] = {
                    "path": name,
                    "offset": offset,
                    "size": len(content),
                    "sha256": hashlib.sha256(content).hexdigest(),
                }
                if overrides and name in overrides:
                    part_entry.update(overrides[name])
                parts.append(part_entry)
            builds.append({"chipFamily": chip, "parts": parts})
        return {
            "name": "Tesla BLE Key (ESP32)",
            "version": version,
            "sourceSha": source_sha,
            "builds": builds,
        }

    # Generate valid files for all 16 parts
    files: dict[str, bytes] = {}
    for chip, target, suffix, boot_offset in TARGETS:
        for name, offset, min_size, max_size in expected_build_parts(target, suffix, boot_offset):
            size = min_size if min_size > 1 else 64
            files[name] = ((f"content_of_{name}".encode("utf-8") * 500) + (b"0" * size))[:size]

    manifest_data = build_test_manifest(files)

    with tempfile.TemporaryDirectory(prefix="dev-pages-test-") as tmpdir:
        staged = Path(tmpdir)
        (staged / "manifest.json").write_text(json.dumps(manifest_data), encoding="utf-8")
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
        assert count == 16, f"expected 16 verified parts, got {count}"

        # Tampered sourceSha
        manifest_url_path = urllib.parse.urlsplit(exact_url(dev_base, "manifest.json", source_sha)).path
        remote_files[manifest_url_path] = json.dumps({**manifest_data, "sourceSha": "b" * 40}).encode("utf-8")
        try:
            verify_dev_pages(pages_base, staged, version, source_sha, attempts=1, interval=0, fetcher=fake_fetch)
        except AcceptanceError:
            pass
        else:
            raise AssertionError("stale sourceSha dev manifest was accepted")

    # --verify-live: the served bytes must match the manifest's own size/SHA-256, all 16 parts.
    live_files = dict(files)

    def live_manifest(overrides: dict[str, dict[str, Any]] | None = None) -> dict[str, Any]:
        return build_test_manifest(live_files, overrides)

    def live_fetcher(manifest: dict[str, Any], served: dict[str, bytes]) -> Fetcher:
        def fetch(url: str, max_bytes: int) -> bytes:
            name = urllib.parse.urlsplit(url).path.rsplit("/", 1)[-1]
            if name == "manifest.json":
                return json.dumps(manifest).encode("utf-8")
            if name in served:
                return served[name]
            raise AcceptanceError(f"404 not found: {url}")
        return fetch

    assert verify_dev_channel_live(
        pages_base, version, source_sha, 1, 0, live_fetcher(live_manifest(), live_files)
    ) == 16

    def must_fail(manifest: dict[str, Any], served: dict[str, bytes], label: str) -> None:
        try:
            verify_dev_channel_live(pages_base, version, source_sha, 1, 0, live_fetcher(manifest, served))
        except AcceptanceError:
            return
        raise AssertionError(f"--verify-live accepted: {label}")

    part_names = list(files.keys())
    stale = dict(live_files)
    stale[part_names[7]] = b"old-binary-still-served" * 10
    must_fail(live_manifest(), stale, "a stale served part behind a current manifest")

    flipped = dict(live_files)
    p3 = part_names[3]
    flipped[p3] = bytes([flipped[p3][0] ^ 1]) + flipped[p3][1:]
    must_fail(live_manifest(), flipped, "a same-length corrupted part")

    missing = dict(live_files)
    del missing[part_names[15]]
    must_fail(live_manifest(), missing, "a part that is not served")

    p0 = part_names[0]
    must_fail(live_manifest({p0: {"size": len(live_files[p0]) + 1}}),
              live_files, "a manifest size that differs from the served bytes")

    short = live_manifest()
    short["builds"][2]["parts"].pop()
    must_fail(short, live_files, "a build with fewer than four parts")

    nodigest = live_manifest()
    del nodigest["builds"][1]["parts"][0]["sha256"]
    must_fail(nodigest, live_files, "a part without a SHA-256")

    wrong_family = live_manifest()
    wrong_family["builds"][1]["chipFamily"] = "ESP32-WRONG"
    must_fail(wrong_family, live_files, "a build with wrong chipFamily")

    wrong_identity = live_manifest()
    wrong_identity["sourceSha"] = "c" * 40
    must_fail(wrong_identity, live_files, "a manifest for another source SHA")

    print("check-dev-pages self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pages-base-url")
    parser.add_argument("--staged-site", type=Path)
    parser.add_argument("--verify-live", action="store_true",
                        help="verify the served dev channel against its own manifest (no staged site)")
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

    if args.verify_live:
        if None in (args.pages_base_url, args.version, args.source_sha) or args.staged_site is not None:
            parser.error("--verify-live needs --pages-base-url, --version and --source-sha, "
                         "and no --staged-site")
    elif None in (args.pages_base_url, args.staged_site, args.version, args.source_sha):
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
        if args.verify_live:
            count = verify_dev_channel_live(
                args.pages_base_url,
                args.version,
                args.source_sha,
                attempts=args.attempts,
                interval=args.interval,
                fetcher=network_fetch,
            )
        else:
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
