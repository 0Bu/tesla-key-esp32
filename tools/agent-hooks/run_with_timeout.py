#!/usr/bin/env python3
"""Run one hook subprocess with a portable, bounded wall-clock timeout."""

from __future__ import annotations

import os
import signal
import subprocess
import sys


MAX_TIMEOUT_SECONDS = 120.0


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
        process = subprocess.Popen(sys.argv[2:], start_new_session=True)
    except OSError:
        return 2
    try:
        return process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
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
