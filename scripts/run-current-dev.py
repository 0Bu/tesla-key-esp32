#!/usr/bin/env python3
"""Stop one unprivileged CI command after a newer push to canonical main."""

import os
import re
import signal
import subprocess
import sys
import time


REPOSITORY = "0Bu/tesla-key-esp32"
MAIN_REF = "refs/heads/main"
POLL_SECONDS = 20
GIT_TIMEOUT = 5
TERMINATION_GRACE = 3
STALE = 75


class Interrupted(Exception):
    def __init__(self, signum):
        self.signum = signum


def interrupt(signum, _frame):
    raise Interrupted(signum)


def live_main():
    # Use the public Git ref. The probe needs no workflow credential or checkout config.
    environment = {
        key: value for key, value in os.environ.items() if not key.startswith("GIT_CONFIG")
    }
    environment.update(
        GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT="0"
    )
    try:
        probe = subprocess.Popen(
            ["git", "ls-remote", "--exit-code", f"https://github.com/{REPOSITORY}.git", MAIN_REF],
            cwd="/", env=environment, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
    except OSError:
        return None
    try:
        output, _ = probe.communicate(timeout=GIT_TIMEOUT)
    except subprocess.TimeoutExpired:
        stop(probe)
        return None
    except Interrupted:
        stop(probe)
        raise
    finally:
        probe.stdout.close()
    if probe.returncode != 0:
        return None
    match = re.fullmatch(rb"([a-f0-9]{40})\trefs/heads/main\n", output)
    return match[1].decode() if match else None


def superseded(expected):
    current = live_main()
    if current is None or current == expected:
        return False
    print(f"::notice::Superseded Dev work for {expected}. Current main is {current}.", flush=True)
    return True


def stop(process):
    try:
        os.killpg(process.pid, signal.SIGTERM)
        deadline = time.monotonic() + TERMINATION_GRACE
        while time.monotonic() < deadline:
            process.poll()
            # The shell can exit before its bounded test helpers finish their own cleanup.
            os.killpg(process.pid, 0)
            time.sleep(0.05)
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    finally:
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            pass


def run(command):
    if (
        os.environ.get("GITHUB_EVENT_NAME") != "push"
        or os.environ.get("GITHUB_REF") != MAIN_REF
        or os.environ.get("GITHUB_REPOSITORY") != REPOSITORY
    ):
        os.execvp(command[0], command)

    expected = os.environ.get("GITHUB_SHA", "")
    if not re.fullmatch(r"[a-f0-9]{40}", expected):
        print("Cannot watch Dev main without an exact push SHA.", file=sys.stderr)
        return 2
    signal.signal(signal.SIGINT, interrupt)
    signal.signal(signal.SIGTERM, interrupt)
    process = None
    try:
        if superseded(expected):
            return STALE
        process = subprocess.Popen(command, start_new_session=True)
        while True:
            try:
                status = process.wait(timeout=POLL_SECONDS)
                return status if status >= 0 else 128 - status
            except subprocess.TimeoutExpired:
                if superseded(expected):
                    return STALE
    except Interrupted as exc:
        return 128 + exc.signum
    finally:
        if process is not None and process.returncode is None:
            signal.signal(signal.SIGINT, signal.SIG_IGN)
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            stop(process)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: run-current-dev.py command [arguments ...]")
    raise SystemExit(run(sys.argv[1:]))
