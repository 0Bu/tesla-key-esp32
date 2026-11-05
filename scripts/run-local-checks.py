#!/usr/bin/env python3
"""Reuse a successful local strict host run for identical inputs; never attest CI or review."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import signal
import shutil
import stat
import subprocess
import sys
import tempfile
import time


TOOLS = ("git", "bash", "python3", "cmake", "ctest", "cc", "c++", "g++", "clang++",
         "gcc", "clang", "node", "ruby", "jq", "rsync", "docker", "google-chrome",
         "google-chrome-stable", "chromium", "chromium-browser")
TRANSIENT_RUNNER_ENVIRONMENT = {"CODEX_TOOL_CALL_ID"}
VOLATILE_ENVIRONMENT = {"_", "SHLVL", "PWD", "OLDPWD", "TESLA_LOCAL_CHECK_LOCK_FD"} | TRANSIENT_RUNNER_ENVIRONMENT
CHECK_TIMEOUT = 1200
BROWSER_IMAGE_REF = "tesla-key-esp32-host-browser:local"


class CheckInterrupted(Exception):
    """Do not use OSError: subprocess may consume it while waiting with kqueue."""


def digest(value: object) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def subprocess_environment() -> dict[str, str]:
    # Git hook selectors and per-call runner metadata must not alter snapshot/test execution.
    return {key: value for key, value in os.environ.items()
            if not key.startswith("GIT_") and key not in TRANSIENT_RUNNER_ENVIRONMENT}


def git(root: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", "-c", "core.fsmonitor=false", "-C", str(root), *args],
                                   env=subprocess_environment(), timeout=30)


def source_identity(root: Path) -> str:
    paths = sorted(set(git(root, "ls-files", "--cached", "--others", "--exclude-standard", "-z").split(b"\0")) - {b""})
    entries = []
    for raw in paths:
        relative = os.fsdecode(raw)
        path = root / relative
        if path.is_absolute() and not path.is_relative_to(root):
            raise ValueError("source path escaped the worktree")
        if path.suffix in (".pem", ".key") or (path.name.startswith("nvs") and path.suffix == ".bin"):
            raise ValueError("secret-shaped source path must be removed from the check inputs")
        try:
            info = path.lstat()
        except FileNotFoundError:
            entries.append([relative, "deleted"])
            continue
        if not stat.S_ISREG(info.st_mode) or path.resolve() != path:
            raise ValueError("local check inputs must be regular files inside the worktree")
        entries.append([relative, stat.S_IMODE(info.st_mode), hashlib.sha256(path.read_bytes()).hexdigest()])
    return digest({"head": git(root, "rev-parse", "HEAD").decode().strip(), "files": entries})


def cached_configuration(root: Path) -> tuple[bytes, dict[str, str]]:
    cache = root / "build_mock/CMakeCache.txt"
    if not cache.exists():
        return b"", {}
    if cache.is_symlink() or cache.resolve() != cache:
        raise ValueError("host CMake configuration must remain inside the worktree")
    content = cache.read_bytes()
    settings = dict(re.findall(r"^([^#/:=]+):[^=\n]+=(.*)$", content.decode(), re.MULTILINE))
    return content, settings


def runtime_identity(root: Path) -> str:
    tools = {}
    cache, settings = cached_configuration(root)
    retained_tools = tuple(value for name, value in settings.items() if value and
        (name in ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "CMAKE_MAKE_PROGRAM", "CMAKE_AR", "CMAKE_RANLIB", "CMAKE_LINKER")
         or name.endswith(("_COMPILER_AR", "_COMPILER_RANLIB"))))
    configured = tuple(os.environ[name] for name in ("CC", "CXX", "BROWSER_BIN") if os.environ.get(name))
    for name in (*TOOLS, sys.executable, *configured, *retained_tools):
        executable = shutil.which(name)
        if executable is None:
            tools[name] = None
            continue
        path = Path(executable).resolve()
        info = path.stat()
        tools[name] = [str(path), info.st_size, info.st_mtime_ns,
                       hashlib.sha256(path.read_bytes()).hexdigest()]
    # Keep only a combined digest, never environment values, in the receipt. Any new environment
    # input invalidates reuse by default rather than requiring another allowlist update.
    environment = {key: value for key, value in os.environ.items()
                   if key not in VOLATILE_ENVIRONMENT and not key.startswith("GIT_")}
    images = None
    if platform.system() == "Darwin" and shutil.which("docker"):
        # Observe the browser runner's own image; unrelated local builds are not host inputs.
        result = subprocess.run(["docker", "image", "ls", "--quiet", "--no-trunc", BROWSER_IMAGE_REF],
                                capture_output=True, timeout=15)
        if result.returncode:
            raise ValueError("Docker image identity unavailable; strict host evidence cannot be reused")
        images = sorted(set(result.stdout.decode().splitlines()))
    cmake = hashlib.sha256(cache).hexdigest()
    return digest({"tools": tools, "environment": environment, "images": images, "cmake": cmake,
                   "platform": platform.platform(), "python": sys.version})


def reusable_environment(root: Path) -> bool:
    # Startup files, module search paths and launcher+argv compiler overrides can consume inputs
    # outside the source snapshot. Execute normally, but never reuse their results.
    indirect = ("BASH_ENV", "ENV", "NODE_OPTIONS", "NODE_PATH", "RUBYOPT", "RUBYLIB", "PYTHONPATH",
                "LD_PRELOAD", "LD_LIBRARY_PATH", "DYLD_INSERT_LIBRARIES", "DYLD_LIBRARY_PATH",
                "CMAKE_TOOLCHAIN_FILE")
    literal_tools = all(not any(character.isspace() for character in os.environ.get(name, ""))
                        for name in ("CC", "CXX", "BROWSER_BIN"))
    _, settings = cached_configuration(root)
    indirect_cmake = any(value and ("TOOLCHAIN_FILE" in name or "LAUNCHER" in name
                         or "PROJECT_INCLUDE" in name or "USER_MAKE_RULES_OVERRIDE" in name)
                         for name, value in settings.items())
    return literal_tools and not indirect_cmake and not any(os.environ.get(name) for name in indirect)


def execute(command: list[str], root: Path, lock_fd: int) -> int:
    environment = subprocess_environment()
    environment["TESLA_LOCAL_CHECK_LOCK_FD"] = str(lock_fd)
    process = subprocess.Popen(command, cwd=root, env=environment, start_new_session=True,
                               pass_fds=(lock_fd,))
    def interrupted(signum, _frame):
        raise CheckInterrupted(f"local host run interrupted by signal {signum}")
    previous = {number: signal.signal(number, interrupted) for number in (signal.SIGTERM, signal.SIGINT)}
    try:
        return process.wait(timeout=CHECK_TIMEOUT)
    except BaseException:
        for number in previous:
            signal.signal(number, signal.SIG_IGN)
        # Wrappers receive TERM and clean their independently owned child sessions. The inherited
        # flock remains held by any surviving owned child even after coordinator/SIGKILL death.
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        cleanup_deadline = time.monotonic() + 3
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            pass
        time.sleep(max(0, cleanup_deadline - time.monotonic()))
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=3)
        raise
    finally:
        for number, handler in previous.items():
            signal.signal(number, handler)


def state_directory(root: Path) -> Path:
    state = root / ".tmp" / "local-checks"
    for directory in (root / ".tmp", state):
        directory.mkdir(mode=0o700, exist_ok=True)
        if directory.is_symlink() or directory.resolve() != directory:
            raise ValueError("local check state must stay inside the worktree")
    return state


def run(root: Path, *, force: bool = False, staged: bool = False) -> int:
    if os.environ.get("OTA_SIGNING_KEY") or os.environ.get("OTA_SIGNING_KEY_FILE"):
        raise ValueError("local host checks must not receive a signing key")
    command = [str(root / "scripts/run-mock-tests.sh"), "--require-all"]
    if not Path(command[0]).is_file() or Path(command[0]).is_symlink():
        raise ValueError("strict host runner missing or unsafe")
    state = state_directory(root)
    flags = os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW
    with os.fdopen(os.open(state / "host.lock", flags, 0o600), "r+") as lock:
        if not stat.S_ISREG(os.fstat(lock.fileno()).st_mode) or os.fstat(lock.fileno()).st_nlink != 1:
            raise ValueError("local check lock must be a private regular file")
        # A simultaneous Stop/commit waits, then checks whether that exact run already passed.
        fcntl.flock(lock, fcntl.LOCK_EX)
        source, runtime = source_identity(root), runtime_identity(root)
        key = digest({"schema": 1, "source": source, "runtime": runtime, "command": command})
        receipt = state / "host.json"
        index_matches = (not git(root, "diff", "--name-only", "-z")
                         and not git(root, "ls-files", "--others", "--exclude-standard", "-z"))
        index_reusable = not staged or index_matches
        if not index_reusable:
            print("local-checks: working tree differs from index; running fresh working-tree checks without reuse", flush=True)
        use_cache = (not force and index_reusable and reusable_environment(root) and not os.environ.get("CI")
                     and not os.environ.get("GITHUB_ACTIONS"))
        if use_cache and receipt.is_file() and not receipt.is_symlink() and receipt.stat().st_size <= 4096:
            try:
                previous = json.loads(receipt.read_text())
            except (ValueError, OSError):
                previous = None
            if previous == {"schema": 1, "key": key, "result": "passed"}:
                print("local-checks: strict host checks already passed for these exact inputs (local evidence only)", flush=True)
                return 0
        # Failed, interrupted and forced attempts cannot leave an older success reusable.
        if receipt.is_symlink():
            raise ValueError("local check receipt must not be a symlink")
        receipt.unlink(missing_ok=True)
        print("local-checks: running strict host checks", flush=True)
        started = time.monotonic()
        return_code = execute(command, root, lock.fileno())
        if return_code:
            return return_code if return_code > 0 else 1
        if source_identity(root) != source:
            raise ValueError("source changed during host checks; rerun the changed candidate")
        if runtime_identity(root) != runtime:
            print("local-checks: host checks passed; runtime changed, so result will not be reused", flush=True)
            return 0
        if index_reusable and reusable_environment(root) and not os.environ.get("CI") and not os.environ.get("GITHUB_ACTIONS"):
            fd, temporary = tempfile.mkstemp(prefix="host-", suffix=".json", dir=state)
            try:
                with os.fdopen(fd, "w") as output:
                    json.dump({"schema": 1, "key": key, "result": "passed"}, output)
                os.replace(temporary, receipt)
            finally:
                Path(temporary).unlink(missing_ok=True)
        print(f"local-checks: strict host checks passed in {time.monotonic() - started:.1f}s (local evidence only)", flush=True)
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-all", action="store_true", required=True)
    parser.add_argument("--force", action="store_true", help="execute even when identical local inputs passed")
    parser.add_argument("--staged", action="store_true", help="disable reuse when the working tree differs from the index")
    args = parser.parse_args()
    try:
        return run(Path(__file__).resolve().parents[1], force=args.force, staged=args.staged)
    except (OSError, ValueError, subprocess.SubprocessError, CheckInterrupted) as error:
        print(f"local-checks: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
