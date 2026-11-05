#!/usr/bin/env python3
"""Run one hook subprocess with a portable, bounded wall-clock timeout."""

from __future__ import annotations

import os
from pathlib import Path
import signal
import stat
import subprocess
import sys


MAX_TIMEOUT_SECONDS = 120.0


class CheckInterrupted(Exception):
    """A cancellation that subprocess's platform wait code cannot consume as OSError."""


def inherited_lock() -> tuple[int, ...]:
    value = os.environ.get("TESLA_LOCAL_CHECK_LOCK_FD")
    if value is None:
        return ()
    if not value.isdecimal() or int(value) < 3:
        raise ValueError("invalid local check lock descriptor")
    descriptor = int(value)
    opened = os.fstat(descriptor)
    expected = Path(__file__).resolve().parents[2] / ".tmp/local-checks/host.lock"
    actual = expected.lstat()
    if (not stat.S_ISREG(opened.st_mode) or not stat.S_ISREG(actual.st_mode)
            or opened.st_nlink != 1 or (opened.st_dev, opened.st_ino) != (actual.st_dev, actual.st_ino)):
        raise ValueError("local check lock does not belong to this worktree")
    return (descriptor,)


def main() -> int:
    if len(sys.argv) < 3:
        return 2
    try:
        timeout = min(float(sys.argv[1]), MAX_TIMEOUT_SECONDS)
    except ValueError:
        return 2
    if timeout <= 0:
        return 2

    try:
        environment = {key: value for key, value in os.environ.items() if key != "TESLA_LOCAL_CHECK_LOCK_FD"}
        process = subprocess.Popen(sys.argv[2:], start_new_session=True, pass_fds=inherited_lock(),
                                   env=environment)
    except (OSError, ValueError):
        return 2
    def interrupted(_signum, _frame):
        raise CheckInterrupted("bounded host check interrupted")
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        return process.wait(timeout=timeout)
    except (subprocess.TimeoutExpired, CheckInterrupted):
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            pass
        # The group leader may exit on SIGTERM while a descendant ignores it.  Always follow with
        # SIGKILL for the original process group; otherwise the runner returns 124 while escaped
        # hook work continues in the background.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            return 124
        return 124


if __name__ == "__main__":
    raise SystemExit(main())
