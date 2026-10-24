#!/usr/bin/env python3
"""Key-free, bounded served-content acceptance for signed PR previews and removal."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile
import time
from urllib.parse import quote, urlencode, urlsplit, urlunsplit
import uuid

MAX_MANIFEST = 131072
MAX_PART = 0x1F0000
TARGETS = (('esp32', ''), ('esp32s3', '-s3'), ('esp32c3', '-c3'), ('esp32c6', '-c6'))
PARTS = tuple(name for target, suffix in TARGETS for name in (
    f'bootloader-{target}.bin', f'partition-table-{target}.bin',
    f'tesla-key-esp32{suffix}.bin', f'ota_data_initial-{target}.bin'))


class AcceptanceError(RuntimeError):
    pass


def require(ok: bool, message: str) -> None:
    if not ok:
        raise AcceptanceError(message)


def load_module(file: str):
    spec = importlib.util.spec_from_file_location('preview_' + file.replace('-', '_'), Path(__file__).with_name(file))
    require(spec is not None and spec.loader is not None, 'trusted validator unavailable')
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def decode(raw: bytes):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'duplicate JSON field')
            result[key] = value
        return result
    return json.loads(raw, object_pairs_hook=pairs, parse_constant=lambda _: require(False, 'nonfinite JSON'))


def relative_path(name: str) -> str:
    require(isinstance(name, str) and 0 < len(name) <= 200 and
            re.fullmatch(r'[A-Za-z0-9_.\-/]+', name) is not None and
            all(part not in ('', '.', '..') for part in name.split('/')) and
            not PurePosixPath(name).is_absolute(), 'unsafe preview relative path')
    return name


def preview_base(metadata: dict, repository: str, pr: int) -> str:
    require(re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository) is not None and
            type(pr) is int and 0 < pr <= 2**31 - 1, 'invalid repository/PR')
    require(metadata.get('url') == f'https://api.github.com/repos/{repository}/pages',
            'Pages metadata is not bound to the expected repository')
    base = load_module('check-pages-source.py').validate(metadata)
    parsed = urlsplit(base)
    require(parsed.hostname is not None and parsed.port in (None, 443) and
            '%' not in parsed.path and '\\' not in parsed.path and
            all(part not in ('.', '..') for part in parsed.path.split('/')), 'unsafe Pages authority')
    return base + f'PR/{pr}/'


def url_for(base: str, name: str, attempt: str | None) -> str:
    relative_path(name)
    parsed = urlsplit(base)
    return urlunsplit(('https', parsed.netloc, parsed.path + quote(name, safe='/._-'),
                       urlencode({'accept': attempt}) if attempt is not None else '', ''))


def fetch_https(url: str, limit: int, seconds: float) -> tuple[int, bytes]:
    """One curl child, no credentials/proxy/redirect, bounded bytes and real elapsed time."""
    require(urlsplit(url).scheme == 'https' and 0 < seconds <= 10 and 0 < limit <= MAX_PART,
            'invalid bounded fetch')
    env = {key: value for key, value in os.environ.items() if key in ('PATH', 'LANG', 'LC_ALL', 'TMPDIR')}
    with tempfile.TemporaryDirectory(prefix='preview-served-') as directory:
        body = Path(directory) / 'body'
        command = ['/usr/bin/curl', '-q', '--silent', '--show-error', '--proxy', '', '--noproxy', '*',
                   '--proto', '=https', '--proto-redir', '=https', '--max-redirs', '0',
                   '--connect-timeout', str(min(3, seconds)), '--max-time', str(seconds),
                   '--max-filesize', str(limit), '--header', 'Cache-Control: no-cache',
                   '--output', str(body), '--write-out', '%{http_code}\n%{url_effective}', url]
        try:
            result = subprocess.run(command, capture_output=True, timeout=seconds + 0.25, env=env, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise AcceptanceError('bounded HTTPS child failed') from error
        require(result.returncode == 0 and len(result.stdout) <= len(url.encode()) + 16,
                'HTTPS transfer failed')
        lines = result.stdout.decode('ascii').splitlines()
        require(len(lines) == 2 and re.fullmatch(r'[0-9]{3}', lines[0]) is not None and lines[1] == url,
                'unexpected HTTP identity/redirect')
        require(body.is_file() and body.stat().st_size <= limit, 'HTTP body exceeds bound')
        return int(lines[0]), body.read_bytes()


def prepare_site(site: Path, source: str, version: str, pr: int, public_pin: Path) -> dict[str, bytes]:
    require(re.fullmatch(r'[0-9a-f]{40}', source) is not None and
            re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-PR-' + str(pr), version) is not None,
            'preview source/version/PR mismatch')
    manifest = site / 'manifest.json'
    require(manifest.is_file() and not manifest.is_symlink() and manifest.stat().st_size <= MAX_MANIFEST,
            'unsafe local preview manifest')
    raw = manifest.read_bytes()
    decode(raw)
    load_module('check-pages-manifest.py').validate(site, source, version)
    firmware = load_module('check-firmware-artifacts.py')
    pin = firmware.read_public_key_digest(public_pin)
    for target, suffix in TARGETS:
        firmware.validate_set(target, version, site / f'bootloader-{target}.bin',
                              site / f'tesla-key-esp32{suffix}.bin', signed_app=True,
                              expected_public_key_digest=pin)
    expected = {'manifest.json': raw}
    for name in PARTS:
        expected[name] = (site / name).read_bytes()
    require(sum(map(len, expected.values())) <= 4 * MAX_PART + MAX_MANIFEST + 200000,
            'local preview snapshot exceeds aggregate budget')
    return expected


def removal_paths(decision: dict, pr: int) -> list[str] | None:
    require(isinstance(decision, dict) and set(decision) == {'pr', 'result', 'paths'} and
            type(decision['pr']) is int and decision['pr'] == pr and isinstance(decision['paths'], list),
            'missing or malformed cleanup decision')
    require(decision['result'] in ('kept-eligible', 'already-absent', 'removed') and
            len(decision['paths']) <= 256, 'invalid cleanup decision')
    paths = [relative_path(name) for name in decision['paths']]
    require(len(set(paths)) == len(paths) and sum(map(len, paths)) <= 32768, 'invalid cleanup inventory')
    if decision['result'] == 'kept-eligible':
        require(not paths, 'kept preview has a deletion inventory')
        return None
    return sorted(set(('manifest.json', 'index.html', *PARTS, *paths)))


def verify(base: str, expected: dict[str, bytes] | None, absent: list[str] | None, *,
           fetcher=fetch_https, clock=time.monotonic, sleeper=time.sleep,
           attempts: int = 20, budget: float = 180, interval: float = 10) -> dict:
    require(1 <= attempts <= 20 and 1 <= budget <= 240 and 0 <= interval <= 10 and
            ((expected is not None) != (absent is not None)), 'invalid acceptance mode/bounds')
    deadline = clock() + budget
    attempted = 0
    for attempt in range(attempts):
        token = uuid.uuid4().hex
        checked = []
        checked_urls = []
        attempted += 1
        try:
            names = ['manifest.json', *PARTS, 'manifest.json'] if expected is not None else absent
            for name in names:
                # Removal must cover both the ordinary URL and a fresh cache key.
                # A fresh404 alone cannot establish that the ordinary cached URL is gone.
                for cache_key in ((token,) if expected is not None else (None, token)):
                    remaining = deadline - clock()
                    require(remaining > 0.25, 'overall acceptance deadline expired')
                    limit = len(expected[name]) if expected is not None else MAX_MANIFEST
                    url = url_for(base, name, cache_key)
                    status, raw = fetcher(url, limit, min(10, remaining - 0.25))
                    require(clock() <= deadline, 'overall acceptance deadline exceeded')
                    require(type(status) is int and isinstance(raw, bytes) and len(raw) <= limit,
                            'invalid or oversized HTTP response')
                    if expected is not None:
                        require(status == 200 and raw == expected[name], 'served preview bytes/status differ')
                    else:
                        require(status in (404, 410), 'preview file is still served or absence is unproved')
                    checked_urls.append(url)
                checked.append(name)
            return {'result': 'published-accepted' if expected is not None else 'absence-accepted',
                    'attempts': attempted, 'checkedPaths': checked,
                    'checkedUrls': checked_urls,
                    'snapshotSha256': hashlib.sha256(expected['manifest.json']).hexdigest() if expected else None}
        except (AcceptanceError, OSError, ValueError) as error:
            last = str(error)
        remaining = deadline - clock()
        if attempt + 1 >= attempts or remaining <= 0:
            break
        sleeper(min(interval, remaining))
    raise AcceptanceError(f'preview served acceptance incomplete after {attempted} attempts: {last}')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pages-metadata', required=True, type=Path)
    parser.add_argument('--repository', required=True)
    parser.add_argument('--pr', required=True, type=int)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument('--site', type=Path)
    modes.add_argument('--cleanup-decision', type=Path)
    parser.add_argument('--source-sha')
    parser.add_argument('--version')
    args = parser.parse_args()
    try:
        require(not any(os.environ.get(key) for key in ('OTA_SIGNING_KEY', 'OTA_SIGNING_KEY_FILE', 'GH_TOKEN', 'GITHUB_TOKEN')),
                'served acceptance must run without keys or repository tokens')
        base = preview_base(decode(args.pages_metadata.read_bytes()), args.repository, args.pr)
        if args.site is not None:
            require(args.source_sha is not None and args.version is not None, 'publication identity missing')
            expected = prepare_site(args.site, args.source_sha, args.version, args.pr,
                                    Path(__file__).with_name('ota-signing-public-key.sha256'))
            result = verify(base, expected, None)
        else:
            paths = removal_paths(decode(args.cleanup_decision.read_bytes()), args.pr)
            result = {'result': 'kept-eligible', 'checkedPaths': []} if paths is None else verify(base, None, paths)
        print(json.dumps(result, sort_keys=True))
        return 0
    except (AcceptanceError, OSError, ValueError, RuntimeError) as error:
        print(f'preview-served: FAIL: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
