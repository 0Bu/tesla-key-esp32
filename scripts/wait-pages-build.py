#!/usr/bin/env python3
"""Wait for the latest branch-backed Pages build before independent byte acceptance."""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from typing import Any, Callable


class BuildError(RuntimeError):
    pass


class BuildReadError(BuildError):
    pass


def wait_for_build(
    fetcher: Callable[[], Any], *, attempts: int = 8, interval: float = 10,
    sleeper: Callable[[float], None] = time.sleep, reporter: Callable[[str], None] = print,
) -> None:
    if type(attempts) is not int or not 1 <= attempts <= 20 or not 0 <= interval <= 60:
        raise BuildError("Pages build polling bounds are invalid")
    last = "status unavailable"
    for attempt in range(1, attempts + 1):
        try:
            build = fetcher()
        except BuildReadError:
            last = "status unavailable"
            reporter("Pages build status unavailable; byte acceptance has not started")
        else:
            if not isinstance(build, dict) or not isinstance(build.get("commit"), str) or not re.fullmatch(r"[0-9a-f]{40}", build["commit"]):
                raise BuildError("Pages build status has no valid commit identity")
            status = build.get("status")
            if status == "built":
                reporter("Latest Pages build finished; served-byte acceptance starts separately")
                return
            if status == "errored":
                # The latest response may still describe an older failed build. It is not
                # bound to this publication, so retain its diagnostic and wait for a successor.
                last = "latest build errored"
                reporter("Latest Pages build errored; waiting within the bound for a newer observation")
            elif status in ("queued", "building"):
                last = "build still running"
                reporter(f"Pages build still running ({status}); byte acceptance has not started")
            else:
                raise BuildError("Pages build status is unrecognized; byte acceptance has not started")
        if attempt < attempts:
            sleeper(min(interval * (2 ** (attempt - 1)), 60))
    raise BuildError(f"Pages {last} after {attempts} bounded observations; byte acceptance has not started")


def read_latest(repository: str, timeout: float) -> Any:
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise BuildError("invalid Pages repository")
    if not 1 <= timeout <= 60:
        raise BuildError("Pages API timeout must be between 1 and 60 seconds")
    try:
        result = subprocess.run(
            ["gh", "api", "--method", "GET", f"repos/{repository}/pages/builds/latest"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise BuildReadError("Pages build status request failed") from exc
    if result.returncode != 0 or len(result.stdout) > 128 * 1024:
        raise BuildReadError("Pages build status response failed or exceeded its bound")
    try:
        return json.loads(result.stdout)
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise BuildReadError("Pages build status response is invalid") from exc


def self_test() -> None:
    from unittest.mock import patch
    commit = "a" * 40
    def build(status: str) -> dict:
        return {"status": status, "commit": commit}
    sleeps: list[float] = []
    reports: list[str] = []
    states = iter([build("queued"), build("building"), build("building"), build("built")])
    wait_for_build(lambda: next(states), sleeper=sleeps.append, reporter=reports.append)
    assert sleeps == [10, 20, 40] and len(reports) == 4
    assert "still running" in reports[0] and "finished" in reports[-1]
    for status, message in [("queued", "build still running"), ("building", "build still running"), ("errored", "errored"), ("unknown", "unrecognized")]:
        sleeps = []
        try:
            wait_for_build(lambda: build(status), sleeper=sleeps.append, reporter=lambda _: None)
        except BuildError as error:
            assert message in str(error)
        else:
            raise AssertionError("non-built Pages state admitted byte acceptance")
        assert sleeps == ([10, 20, 40, 60, 60, 60, 60] if status in ("queued", "building", "errored") else [])
    # A stale old failure must not prevent observing the newly queued/built successor.
    sleeps = []
    reports = []
    states = iter([build("errored"), {"status": "queued", "commit": "b" * 40}, {"status": "built", "commit": "b" * 40}])
    wait_for_build(lambda: next(states), sleeper=sleeps.append, reporter=reports.append)
    assert sleeps == [10, 20] and len(reports) == 3
    assert "errored" in reports[0] and "still running" in reports[1] and "finished" in reports[2]
    sleeps = []
    def unavailable():
        raise BuildReadError("offline fixture")
    try:
        wait_for_build(unavailable, attempts=3, interval=.25, sleeper=sleeps.append, reporter=lambda _: None)
    except BuildError as error:
        assert "status unavailable" in str(error) and "still running" not in str(error)
    else:
        raise AssertionError("unknown Pages build status admitted byte acceptance")
    assert sleeps == [.25, .5]
    for value in (None, {}, {"status": "built", "commit": "x"}, {"status": "built", "commit": commit + "\n"}):
        try:
            wait_for_build(lambda: value, reporter=lambda _: None)
        except BuildError:
            pass
        else:
            raise AssertionError("malformed Pages build status accepted")
    calls = []
    def run(command, **kwargs):
        calls.append(command)
        assert command == ["gh", "api", "--method", "GET", "repos/example/firmware/pages/builds/latest"]
        assert kwargs["timeout"] == 15 and kwargs["check"] is False
        return subprocess.CompletedProcess(command, 0, json.dumps(build("built")).encode(), b"")
    with patch.object(subprocess, "run", run):
        assert read_latest("example/firmware", 15) == build("built")
        for repository in ("example/firmware/extra", "example/firmware?x", "--method POST", ""):
            try:
                read_latest(repository, 15)
            except BuildError:
                pass
            else:
                raise AssertionError("unsafe Pages repository accepted")
    assert len(calls) == 1
    module = sys.modules[__name__]
    original_wait = wait_for_build
    observed = []
    states = iter([build("queued"), build("built")])
    def fetch(repository, timeout):
        assert (repository, timeout) == ("example/firmware", 15)
        observed.append("GET")
        return next(states)
    def wait(fetcher, **kwargs):
        assert kwargs == {"attempts": 8, "interval": 10}
        return original_wait(fetcher, **kwargs, sleeper=lambda _: None, reporter=lambda _: None)
    with patch.object(sys, "argv", ["readiness", "--repository", "example/firmware"]), patch.object(module, "read_latest", fetch), patch.object(module, "wait_for_build", wait):
        assert main() == 0
    assert observed == ["GET", "GET"], "CLI omitted the actual build readiness consumer"
    print("Pages build readiness self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository")
    parser.add_argument("--attempts", type=int, default=8)
    parser.add_argument("--interval", type=float, default=10)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.repository is None:
        parser.error("--repository is required")
    try:
        wait_for_build(lambda: read_latest(args.repository, args.timeout), attempts=args.attempts, interval=args.interval)
    except BuildError as error:
        print(f"Pages build readiness failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
