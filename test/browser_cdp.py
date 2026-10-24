"""Bounded Linux Chrome DevTools pipe for the offline browser gate (no TCP listener)."""

from __future__ import annotations

from collections import deque
import json
import math
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import time


class CdpError(RuntimeError):
    pass


def strict_object(raw: bytes) -> dict:
    def unique(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("duplicate protocol key")
            result[key] = value
        return result

    def finite(text):
        value = float(text)
        if not math.isfinite(value):
            raise ValueError("non-finite protocol number")
        return value

    def constant(_):
        raise ValueError("non-finite protocol constant")

    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=unique,
                           parse_float=finite, parse_constant=constant)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise CdpError("invalid CDP JSON record") from error
    if not isinstance(value, dict):
        raise CdpError("CDP object required")
    return value


class Records:
    """NUL-framed JSON with independent per-record, aggregate and record-count bounds."""

    MAX_RECORD = 2 * 1024 * 1024
    MAX_TOTAL = 16 * 1024 * 1024
    MAX_RECORDS = 20000

    def __init__(self):
        self.buffer = bytearray()
        self.total = 0
        self.count = 0

    def feed(self, data: bytes) -> list[dict]:
        self.total += len(data)
        if self.total > self.MAX_TOTAL:
            raise CdpError("CDP aggregate byte limit")
        result = []
        for part in data.split(b"\0")[:-1]:
            # The final fragment is handled below; a record may span multiple reads.
            self.buffer.extend(part)
            if not self.buffer or len(self.buffer) > self.MAX_RECORD:
                raise CdpError("CDP empty or oversized record")
            self.count += 1
            if self.count > self.MAX_RECORDS:
                raise CdpError("CDP record flood")
            result.append(strict_object(bytes(self.buffer)))
            self.buffer.clear()
        self.buffer.extend(data.rsplit(b"\0", 1)[-1])
        if len(self.buffer) > self.MAX_RECORD:
            raise CdpError("CDP unterminated record limit")
        return result


# Popen owns a fresh process group. Only the two duplicated endpoints are inherited;
# an isolated single-threaded shim remaps those to Chrome's mandatory read3/write4.
# This avoids Python's unsafe threaded preexec_fn and descriptor3/4 collision ordering.
PIPE_SHIM = """import os,sys
r,w=int(sys.argv[1]),int(sys.argv[2])
assert r>=5 and w>=5 and r!=w
os.dup2(r,3,inheritable=True)
os.dup2(w,4,inheritable=True)
os.close(r);os.close(w)
os.execv(sys.argv[3],sys.argv[3:])
"""


class Cdp:
    COMMAND_SECONDS = 5.0

    def __init__(self, browser: str, arguments: list[str], deadline: float,
                 stderr: Path, clock=time.monotonic):
        if sys.platform != "linux":
            raise CdpError("automatic browser processes are supported only on Linux")
        import fcntl
        self.clock = clock
        self.deadline = deadline
        self.serial = 0
        self.records = Records()
        self.pending = deque()
        self.events = deque(maxlen=32)
        self.process = None
        self.read_fd = self.write_fd = None
        if self.clock() >= deadline:
            raise CdpError("browser work deadline expired before launch")
        owned = set()
        try:
            command_read, command_write = os.pipe()
            owned.update((command_read, command_write))
            response_read, response_write = os.pipe()
            owned.update((response_read, response_write))
            child_read = fcntl.fcntl(command_read, fcntl.F_DUPFD_CLOEXEC, 5)
            owned.add(child_read)
            child_write = fcntl.fcntl(response_write, fcntl.F_DUPFD_CLOEXEC, 5)
            owned.add(child_write)
            environment = {name: value for name, value in os.environ.items()
                           if name not in ("GH_TOKEN", "GITHUB_TOKEN", "OTA_SIGNING_KEY",
                                           "OTA_SIGNING_KEY_FILE")
                           and not name.startswith(("PYTHON", "LD_", "CHROME_"))}
            with stderr.open("xb") as log:
                self.process = subprocess.Popen(
                    [sys.executable, "-I", "-c", PIPE_SHIM,
                     str(child_read), str(child_write), browser, *arguments],
                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=log,
                    pass_fds=(child_read, child_write), close_fds=True,
                    start_new_session=True, env=environment,
                )
            self.read_fd, self.write_fd = response_read, command_write
            owned.difference_update((response_read, command_write))
            os.set_blocking(self.read_fd, False)
            os.set_blocking(self.write_fd, False)
        except BaseException:
            self.close()
            raise
        finally:
            for descriptor in owned:
                os.close(descriptor)

    def remaining(self, deadline: float) -> float:
        remaining = min(deadline, self.deadline) - self.clock()
        if remaining <= 0:
            raise CdpError("CDP absolute deadline expired")
        return remaining

    def ready(self, descriptor: int, event: int, deadline: float) -> None:
        with selectors.DefaultSelector() as selector:
            selector.register(descriptor, event)
            if not selector.select(self.remaining(deadline)):
                raise CdpError("CDP pipe deadline expired")
        self.remaining(deadline)

    def call(self, method: str, parameters: dict | None = None,
             session: str | None = None, seconds: float = COMMAND_SECONDS) -> dict:
        if self.read_fd is None or self.write_fd is None:
            raise CdpError("CDP connection is closed")
        if not 0 < seconds <= 45:
            raise CdpError("CDP command timeout outside its bound")
        self.serial += 1
        if self.serial > 1000:
            raise CdpError("CDP command count limit")
        deadline = min(self.deadline, self.clock() + seconds)
        self.remaining(deadline)
        message = {"id": self.serial, "method": method, "params": parameters or {}}
        if session is not None:
            message["sessionId"] = session
        raw = json.dumps(message, allow_nan=False, separators=(",", ":")).encode() + b"\0"
        if len(raw) > Records.MAX_RECORD:
            raise CdpError("CDP request size limit")
        view = memoryview(raw)
        try:
            while view:
                self.ready(self.write_fd, selectors.EVENT_WRITE, deadline)
                try:
                    written = os.write(self.write_fd, view)
                except BlockingIOError:
                    continue
                if written <= 0:
                    raise CdpError("CDP write made no progress")
                view = view[written:]
            while True:
                self.remaining(deadline)
                if not self.pending:
                    self.ready(self.read_fd, selectors.EVENT_READ, deadline)
                    try:
                        chunk = os.read(self.read_fd, 65536)
                    except BlockingIOError:
                        continue
                    if not chunk:
                        raise CdpError("CDP EOF before command response")
                    self.pending.extend(self.records.feed(chunk))
                    continue
                response = self.pending.popleft()
                if "id" not in response:
                    if not isinstance(response.get("method"), str):
                        raise CdpError("CDP event method required")
                    self.events.append(response)
                    continue
                if type(response["id"]) is not int or response["id"] != self.serial:
                    raise CdpError("CDP unexpected response ID")
                if response.get("sessionId") != session:
                    raise CdpError("CDP response session mismatch")
                if "error" in response:
                    raise CdpError("CDP command failed: " + method)
                if not isinstance(response.get("result"), dict):
                    raise CdpError("CDP response result required")
                return response["result"]
        except OSError as error:
            raise CdpError("CDP pipe I/O failed") from error

    def evaluate(self, expression: str, session: str):
        response = self.call("Runtime.evaluate", {
            "expression": expression, "returnByValue": True, "awaitPromise": True,
        }, session)
        if "exceptionDetails" in response or not isinstance(response.get("result"), dict):
            raise CdpError("browser evaluation failed")
        result = response["result"]
        if "value" not in result:
            raise CdpError("browser evaluation produced no value")
        return result["value"]

    def close(self):
        process, self.process = self.process, None
        try:
            if process is not None:
                def signal_group(sig):
                    try:
                        os.killpg(process.pid, sig)
                    except ProcessLookupError:
                        pass
                    except PermissionError as error:
                        # A peer can exit between poll() and killpg(). Darwin may
                        # report EPERM for its unreaped, empty process group. Reap
                        # that exact child, then retry once; a live or inaccessible
                        # group must still fail cleanup rather than be ignored.
                        try:
                            process.wait(timeout=0.25)
                        except subprocess.TimeoutExpired:
                            raise CdpError("owned browser group cannot be signaled") from error
                        try:
                            os.killpg(process.pid, sig)
                        except ProcessLookupError:
                            pass
                        except PermissionError as retry_error:
                            raise CdpError("owned browser group cannot be signaled") from retry_error

                process.poll()  # Reap an already exited pipe/exec peer before signaling its group.
                signal_group(signal.SIGTERM)
                try:
                    process.wait(timeout=0.75)
                except subprocess.TimeoutExpired:
                    pass
                # The group may outlive its leader. Always kill remaining owned children,
                # even after a successful wait; never inspect or terminate other groups.
                signal_group(signal.SIGKILL)
                try:
                    process.wait(timeout=0.75)
                except subprocess.TimeoutExpired as error:
                    raise CdpError("owned browser did not terminate") from error
        finally:
            for name in ("read_fd", "write_fd"):
                descriptor = getattr(self, name, None)
                if descriptor is not None:
                    setattr(self, name, None)
                    os.close(descriptor)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
