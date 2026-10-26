#!/usr/bin/env python3
"""Shared exact-source admission for repository nanopb and Tesla consumers."""

from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import selectors
import signal
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any, Iterator


SHA1 = re.compile(r"[0-9a-f]{40}")
SHA256 = re.compile(r"[0-9a-f]{64}")
NANOPB_FILES = frozenset(("pb.h", "pb_common.c", "pb_common.h", "pb_decode.c",
                          "pb_decode.h", "pb_encode.c", "pb_encode.h"))
FILE_LIMIT = 16 * 1024 * 1024
TREE_LIMIT = 128 * 1024 * 1024
PATH_LIMIT = 10000
# Existing source repositories are never allowed to supply Git execution/configuration policy.
SOURCE_CONFIG = (b"[core]\n\trepositoryformatversion = 0\n\tfilemode = true\n"
                 b"\tbare = false\n\tlogallrefupdates = false\n\tautocrlf = false\n")
# Import records identity only. Kernel signal policy changes at the first effectful entry.
PROCESS_ID = os.getpid()
PROCESS_PARENT = os.getppid()
_parent_death_armed = False


class SourceError(RuntimeError):
    pass


class GitFailure(SourceError):
    def __init__(self, diagnostic: bytes):
        super().__init__("exact-source Git query failed")
        self.diagnostic = diagnostic


class ColdSourceUnavailable(SourceError):
    """Only a failed fetch into a newly owned cold destination may be optional."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SourceError(message)


def parent_lifetime_boundary(*, cli: bool = False) -> None:
    """Never adopt a new owner after import, arming or a fork of this interpreter."""
    global _parent_death_armed
    require(os.getpid() == PROCESS_ID and os.getppid() == PROCESS_PARENT,
            "dependency admission process owner changed")
    # A dedicated CLI adopted by init before import has already lost its consumer. An
    # imported validator may legitimately be launched directly by a container's init.
    require(not cli or PROCESS_PARENT > 1, "dependency admission CLI has no live consumer")
    if sys.platform == "linux":
        try:
            import ctypes
            libc = ctypes.CDLL(None, use_errno=True)
            prctl = libc.prctl
            prctl.restype = ctypes.c_int
            prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong,
                             ctypes.c_ulong, ctypes.c_ulong]
            if not _parent_death_armed:
                require(prctl(1, signal.SIGKILL, 0, 0, 0) == 0,
                        "cannot arm dependency admission parent-death guard")
            configured = ctypes.c_int()
            require(prctl(2, ctypes.addressof(configured), 0, 0, 0) == 0
                    and configured.value == signal.SIGKILL,
                    "dependency admission parent-death guard differs")
        except (ImportError, OSError, AttributeError, TypeError, ValueError) as error:
            raise SourceError("dependency admission parent-death guard unavailable") from error
    # Linux can reparent between the initial identity check and prctl. Refuse before effects;
    # an inherited Python flag after fork never substitutes for the kernel guard.
    require(os.getpid() == PROCESS_ID and os.getppid() == PROCESS_PARENT,
            "dependency admission process owner changed while arming")
    _parent_death_armed = True


def real_directory(path: Path) -> Path:
    path = Path(os.path.abspath(path))
    for item in (*reversed(path.parents), path):
        info = item.lstat()
        require(stat.S_ISDIR(info.st_mode) and not stat.S_ISLNK(info.st_mode),
                "source directory is missing, aliased or not a directory")
    return path


def read_regular(path: Path, maximum: int = FILE_LIMIT) -> bytes:
    real_directory(path.parent)
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        before = os.fstat(fd)
        require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1
                and before.st_size <= maximum, "source leaf is not bounded, regular and unaliased")
        result = bytearray()
        while len(result) <= maximum:
            chunk = os.read(fd, min(65536, maximum + 1 - len(result)))
            if not chunk:
                break
            result.extend(chunk)
        after = os.fstat(fd)
        identity = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns)
        require(identity(before) == identity(after) and len(result) == before.st_size
                and len(result) <= maximum, "source leaf changed while reading")
        return bytes(result)
    finally:
        os.close(fd)


def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        require(key not in result, "duplicate provenance field")
        result[key] = value
    return result


def read_nanopb_record(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(read_regular(path, 16384), object_pairs_hook=unique_object)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise SourceError("invalid nanopb provenance JSON") from error
    require(type(value) is dict and set(value) == {"schema", "version", "commit", "repository", "files"},
            "unexpected nanopb provenance fields")
    require(type(value["schema"]) is int and value["schema"] == 1, "unsupported provenance schema")
    require(isinstance(value["version"], str) and
            re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", value["version"]) is not None,
            "invalid nanopb display version")
    require(isinstance(value["commit"], str) and SHA1.fullmatch(value["commit"]) is not None,
            "invalid nanopb commit")
    require(value["repository"] == "https://github.com/nanopb/nanopb.git",
            "unexpected nanopb repository")
    files = value["files"]
    require(type(files) is dict and set(files) == NANOPB_FILES, "unexpected nanopb runtime inventory")
    require(all(isinstance(digest, str) and SHA256.fullmatch(digest) is not None
                for digest in files.values()), "invalid nanopb runtime digest")
    return value


def git_environment() -> dict[str, str]:
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("GIT_", "SSH_", "BASH_FUNC_"))
                   and key not in ("ENV", "BASH_ENV", "GH_TOKEN", "GITHUB_TOKEN",
                                   "OTA_SIGNING_KEY", "OTA_SIGNING_KEY_FILE")}
    environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                       GIT_CONFIG_SYSTEM=os.devnull, GIT_TERMINAL_PROMPT="0",
                       GIT_NO_REPLACE_OBJECTS="1", GIT_NO_LAZY_FETCH="1",
                       GIT_ATTR_NOSYSTEM="1")
    return environment


def stop_owned_group(child: subprocess.Popen[bytes], *, atomic: bool = False) -> None:
    """Bound cleanup to the session/group created for this invocation, including descendants."""
    signals = (signal.SIGKILL,) if atomic else (signal.SIGTERM, signal.SIGKILL)
    for requested_signal in signals:
        try:
            os.killpg(child.pid, requested_signal)
        except ProcessLookupError:
            child.wait(timeout=1)
            return
        except OSError as error:
            raise SourceError("cannot signal the owned exact-source query group") from error
        deadline = time.monotonic() + 1
        while True:
            child.poll()  # Reap the leader even when a descendant keeps the group alive.
            try:
                os.killpg(child.pid, 0)
            except ProcessLookupError:
                child.wait(timeout=1)
                return
            except PermissionError:
                # Darwin can briefly report an orphaned group as EPERM while its last member
                # exits. This remains UNKNOWN; only a later ESRCH confirms disappearance.
                pass
            except OSError as error:
                raise SourceError("cannot observe the owned exact-source query group") from error
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(remaining, 0.02))
    raise SourceError("owned exact-source query group cleanup could not be confirmed")


# This session owns Git and its descendants. The parent alone holds the liveness writer;
# its death closes that pipe even when SIGKILL bypasses Python finally blocks. The guardian
# closes its own data descriptors immediately, reports the Git leader separately, and remains
# available for cancellation after that leader exits while a descendant still holds a pipe.
GIT_GUARDIAN = r'''
import json, os, select, signal, subprocess, sys
alive, status = int(sys.argv[1]), int(sys.argv[2])
try:
    child = subprocess.Popen(json.loads(sys.argv[3]), close_fds=True)
except OSError:
    os.write(status, b"127\n")
    os.close(status)
    child = None
os.close(1)
os.close(2)
try:
    while True:
        if select.select([alive], [], [], 0.02)[0]:
            break
        if child is not None:
            code = child.poll()
            if code is not None:
                os.write(status, (str(code) + "\n").encode("ascii"))
                os.close(status)
                child = None
finally:
    os.killpg(os.getpgrp(), signal.SIGKILL)
'''


def git_query(directory: Path, arguments: list[str], environment: dict[str, str],
              maximum: int = 2 * 1024 * 1024) -> bytes:
    parent_lifetime_boundary()
    # Only fixed plumbing commands and validated object IDs reach this reader.
    command = ["git", "--no-pager", "--no-optional-locks",
                              "--literal-pathspecs", "--git-dir=" + str(directory),
                              "-c", "core.hooksPath=" + os.devnull,
                              "-c", "core.fsmonitor=false",
                              "-c", "maintenance.auto=false", "-c", "gc.auto=0", *arguments]
    alive_read, alive_write = os.pipe()
    status_read, status_write = os.pipe()
    try:
        child = subprocess.Popen([sys.executable, "-I", "-B", "-c", GIT_GUARDIAN,
                                  str(alive_read), str(status_write), json.dumps(command)],
                             env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             start_new_session=True, close_fds=True,
                             pass_fds=(alive_read, status_write))
    except BaseException:
        os.close(alive_write)
        os.close(status_read)
        raise
    finally:
        os.close(alive_read)
        os.close(status_write)
    assert child.stdout is not None and child.stderr is not None
    status_stream = os.fdopen(status_read, "rb")
    buffers = {child.stdout: bytearray(), child.stderr: bytearray(), status_stream: bytearray()}
    limits = {child.stdout: maximum, child.stderr: 65536, status_stream: 16}
    deadline = time.monotonic() + 30
    returncode = None
    try:
        with selectors.DefaultSelector() as selector:
            for stream in buffers:
                selector.register(stream, selectors.EVENT_READ)
            while selector.get_map():
                parent_lifetime_boundary()
                remaining = deadline - time.monotonic()
                require(remaining > 0, "exact-source Git query exceeded its deadline")
                for key, _events in selector.select(min(remaining, 0.25)):
                    stream = key.fileobj
                    chunk = os.read(stream.fileno(), min(65536, limits[stream] + 1 - len(buffers[stream])))
                    if not chunk:
                        selector.unregister(stream)
                        if stream is status_stream:
                            raw_status = bytes(buffers[stream])
                            require(re.fullmatch(rb"-?[0-9]{1,3}\n", raw_status) is not None,
                                    "exact-source Git status is missing or invalid")
                            returncode = int(raw_status)
                            require(-255 <= returncode <= 255,
                                    "exact-source Git status is outside its bound")
                    else:
                        buffers[stream].extend(chunk)
                        require(len(buffers[stream]) <= limits[stream],
                                "exact-source Git query exceeded its output bound")
        require(returncode is not None, "exact-source Git status is missing")
        if returncode != 0:
            raise GitFailure(bytes(buffers[child.stderr]))
        return bytes(buffers[child.stdout])
    finally:
        try:
            # Keep the guardian alive while this parent confirms ordinary cleanup. EOF is
            # the independent fallback for an externally killed parent, not a competing
            # normal cleanup path that can race the parent's group observation.
            # An atomic group kill keeps the guardian available until every group member
            # is killed. TERM could remove that watcher before a resistant descendant.
            stop_owned_group(child, atomic=True)
        finally:
            os.close(alive_write)
            child.stdout.close()
            child.stderr.close()
            status_stream.close()


@contextlib.contextmanager
def object_context(source: Path, commit: str) -> Iterator[tuple[Path, dict[str, str]]]:
    """Read source objects through our own context, never the cache's Git configuration."""
    parent_lifetime_boundary()
    require(SHA1.fullmatch(commit) is not None, "invalid source commit")
    source = real_directory(source)
    metadata = real_directory(source / ".git")
    require(read_regular(metadata / "HEAD", 128) == (commit + "\n").encode(),
            "source HEAD differs from the exact commit")
    require(read_regular(metadata / "config", 4096) == SOURCE_CONFIG,
            "source Git configuration differs from the owned configuration")
    objects = real_directory(metadata / "objects")
    require(not os.path.lexists(objects / "info/alternates")
            and not os.path.lexists(objects / "info/http-alternates"),
            "alternate object stores are forbidden")
    for root, directories, names in os.walk(objects, followlinks=False):
        for name in directories:
            real_directory(Path(root) / name)
        for name in names:
            info = (Path(root) / name).lstat()
            require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
                    "source object store contains an alias or nonregular entry")
    with tempfile.TemporaryDirectory(prefix="tesla-source-object-reader-") as temporary:
        trusted = Path(temporary)
        (trusted / "objects").mkdir()
        (trusted / "refs").mkdir()
        (trusted / "HEAD").write_bytes(b"ref: refs/heads/unborn\n")
        (trusted / "config").write_bytes(SOURCE_CONFIG.replace(b"bare = false", b"bare = true"))
        if os.path.lexists(metadata / "shallow"):
            shallow = read_regular(metadata / "shallow", 128)
            require(shallow == (commit + "\n").encode(), "unexpected shallow history boundary")
            (trusted / "shallow").write_bytes(shallow)
        environment = git_environment()
        environment["GIT_OBJECT_DIRECTORY"] = str(objects)
        git_query(trusted, ["fsck", "--full", "--no-reflogs", "--no-dangling", commit], environment)
        yield trusted, environment


def safe_relative(raw: bytes) -> str:
    try:
        name = raw.decode("utf-8")
    except UnicodeError as error:
        raise SourceError("source path is not UTF-8") from error
    path = PurePosixPath(name)
    require(name and "\\" not in name and not path.is_absolute()
            and all(part not in ("", ".", "..") for part in name.split("/"))
            and all(ord(character) >= 0x20 for character in name)
            and ".git" not in path.parts, "unsafe exact-source path")
    return name


def git_tree(trusted: Path, environment: dict[str, str], commit: str) -> dict[str, tuple[str, str]]:
    raw = git_query(trusted, ["ls-tree", "-r", "-z", commit], environment)
    require(raw.endswith(b"\0"), "empty or invalid exact Git inventory")
    result: dict[str, tuple[str, str]] = {}
    for entry in raw[:-1].split(b"\0"):
        require(b"\t" in entry, "invalid exact Git inventory")
        metadata, raw_name = entry.split(b"\t", 1)
        fields = metadata.split(b" ")
        require(len(fields) == 3, "invalid exact Git inventory metadata")
        mode, kind, object_id = fields
        require(mode in (b"100644", b"100755", b"120000") and kind == b"blob"
                and re.fullmatch(b"[0-9a-f]{40}", object_id) is not None,
                "source tree contains an unsupported mode or object")
        name = safe_relative(raw_name)
        require(name not in result and len(result) < PATH_LIMIT, "invalid source path inventory")
        result[name] = (object_id.decode("ascii"), mode.decode("ascii"))
    return result


def literal_link(path: Path) -> bytes:
    real_directory(path.parent)
    before = path.lstat()
    require(stat.S_ISLNK(before.st_mode) and before.st_nlink == 1 and before.st_size <= 4096,
            "source link is not a bounded, unaliased literal link")
    raw = os.readlink(os.fsencode(path))
    after = path.lstat()
    identity = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns)
    require(identity(before) == identity(after) and len(raw) == before.st_size,
            "literal source link changed while reading")
    return raw


def validate_link(name: str, raw: bytes, tree: dict[str, tuple[str, str]]) -> str:
    try:
        target = raw.decode("utf-8")
    except UnicodeError as error:
        raise SourceError("source link target is not UTF-8") from error
    require(target and len(raw) <= 4096 and not target.startswith("/") and "\\" not in target
            and all(ord(character) >= 0x20 for character in target)
            and all(part not in ("", ".git") for part in target.split("/")),
            "unsafe literal source link target")
    parts = list(PurePosixPath(name).parent.parts)
    for part in target.split("/"):
        if part == ".":
            continue
        if part == "..":
            require(bool(parts), "source link escapes its admitted root")
            parts.pop()
        else:
            parts.append(part)
    resolved = "/".join(parts)
    require(resolved in tree and tree[resolved][1] in ("100644", "100755"),
            "source link does not directly select an admitted regular file")
    return target


def pristine_inventory(source: Path, commit: str,
                       required_digests: dict[str, str] | None = None) -> dict[str, dict[str, Any]]:
    """Require every materialized body/mode to equal the exact Git tree, including extras."""
    source = real_directory(source)
    with object_context(source, commit) as (trusted, environment):
        tree = git_tree(trusted, environment, commit)
    result: dict[str, dict[str, Any]] = {}
    total = 0
    for name, (object_id, mode) in tree.items():
        path = source / name
        executable = mode == "100755"
        if mode == "120000":
            data = literal_link(path)
            target = validate_link(name, data, tree)
        else:
            data = read_regular(path)
            require(bool(path.stat().st_mode & 0o111) == executable,
                    "source executable mode differs")
        actual_object = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
        require(actual_object == object_id, "source body differs from its exact Git blob")
        total += len(data)
        require(total <= TREE_LIMIT, "source tree exceeds its bounded inventory")
        result[name] = {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data),
                        "executable": executable}
        if mode == "120000":
            result[name]["symlinkTarget"] = target
    actual_paths: set[str] = set()
    actual_directories: set[str] = set()
    for root, directories, names in os.walk(source, followlinks=False):
        if Path(root) == source:
            directories.remove(".git")
        for directory in directories:
            real_directory(Path(root) / directory)
            actual_directories.add(str((Path(root) / directory).relative_to(source)))
        for name in names:
            actual_paths.add(str((Path(root) / name).relative_to(source)))
    require(actual_paths == set(result), "missing or extra materialized source path")
    expected_directories = {str(parent) for name in result for parent in PurePosixPath(name).parents
                            if str(parent) != "."}
    require(actual_directories == expected_directories, "missing or extra materialized source directory")
    for name, expected in (required_digests or {}).items():
        require(name in result and "symlinkTarget" not in result[name]
                and result[name]["sha256"] == expected,
                "admitted regular source differs from a required runtime digest")
    return result


def inventory_digest(inventory: dict[str, dict[str, Any]]) -> str:
    return hashlib.sha256(json.dumps(inventory, sort_keys=True,
                                     separators=(",", ":")).encode()).hexdigest()


def read_tesla_record(root: Path) -> dict[str, Any]:
    """Derive the selected commit from all four byte-validated lockfiles."""
    root = real_directory(root)
    spec = importlib.util.spec_from_file_location("tesla_source_dependency_contract",
                                               root / "scripts/check-dependency-contract.py")
    require(spec is not None and spec.loader is not None, "missing dependency contract")
    dependency = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(dependency)
    try:
        dependency.validate(root)
    except dependency.DependencyError as error:
        raise SourceError("repository dependency contract failed") from error
    commits = []
    lock_digests = {}
    for target in dependency.TARGETS:
        name = "dependencies.lock." + target
        raw = read_regular(root / name, 65536)
        block = re.search(rb"^  yoziru/tesla-ble:\n(.*?)(?=^[^ ]|^  [^ ]|\Z)", raw,
                          re.MULTILINE | re.DOTALL)
        require(block is not None, "missing Tesla lock entry")
        versions = re.findall(rb"^    version: ([0-9a-f]{40})$", block[1], re.MULTILINE)
        require(len(versions) == 1, "invalid Tesla lock commit")
        commits.append(versions[0].decode("ascii"))
        lock_digests[name] = hashlib.sha256(raw).hexdigest()
    require(len(commits) == 4 and len(set(commits)) == 1, "Tesla lock commits disagree")
    return {"schema": 1, "version": dependency.TESLA_VERSION,
            "commit": commits[0], "repository": dependency.TESLA_GIT,
            "locks": lock_digests}


def owner_path(source: Path) -> Path:
    return source.parent / ("." + source.name + ".tesla-source.json")


def source_identity(source: Path, role: str, record: dict[str, Any]) -> dict[str, Any]:
    require(role in ("nanopb", "tesla-ble"), "unknown dependency source role")
    expected_repository = {"nanopb": "https://github.com/nanopb/nanopb.git",
                           "tesla-ble": "https://github.com/yoziru/tesla-ble.git"}[role]
    require(record.get("repository") == expected_repository
            and isinstance(record.get("commit"), str)
            and SHA1.fullmatch(record["commit"]) is not None, "invalid dependency source identity")
    return {"schema": 1, "root": str(source), "role": role,
            "commit": record["commit"], "repository": record["repository"],
            "recordSha256": hashlib.sha256(json.dumps(record, sort_keys=True,
                                   separators=(",", ":")).encode()).hexdigest()}


def write_owner(fd: int, value: dict[str, Any]) -> None:
    data = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
    require(len(data) <= 16384, "oversized source ownership record")
    os.lseek(fd, 0, os.SEEK_SET)
    os.ftruncate(fd, 0)
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        require(written > 0, "cannot write source ownership record")
        view = view[written:]
    os.fsync(fd)


def materialize(source: Path, commit: str) -> None:
    """Write only new raw regular Git blobs; no checkout filters or attributes execute."""
    total = 0
    with object_context(source, commit) as (trusted, environment):
        tree = git_tree(trusted, environment, commit)
        for name, (object_id, mode) in tree.items():
            executable = mode == "100755"
            raw_size = git_query(trusted, ["cat-file", "-s", object_id], environment, 32)
            require(re.fullmatch(rb"[0-9]+\n", raw_size) is not None, "invalid source blob size")
            size = int(raw_size)
            require(size <= FILE_LIMIT and total + size <= TREE_LIMIT,
                    "source blob/tree exceeds its bounded inventory")
            data = git_query(trusted, ["cat-file", "blob", object_id], environment, FILE_LIMIT)
            require(len(data) == size, "source blob changed while materializing")
            path = source / name
            path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            real_directory(path.parent)
            if mode == "120000":
                os.symlink(validate_link(name, data, tree), path)
            else:
                fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                             0o700 if executable else 0o600)
                with os.fdopen(fd, "wb") as output:
                    output.write(data)
            total += size


def populate_cold(source: Path, commit: str, repository: str) -> None:
    """Only called after exclusive ownership/directory creation; failed state is retained."""
    metadata = source / ".git"
    metadata.mkdir(mode=0o700)
    (metadata / "objects").mkdir(mode=0o700)
    (metadata / "refs").mkdir(mode=0o700)
    (metadata / "config").write_bytes(SOURCE_CONFIG)
    (metadata / "HEAD").write_bytes(b"ref: refs/heads/unborn\n")
    try:
        git_query(metadata, ["fetch", "--quiet", "--no-tags", "--depth=1", repository, commit],
                  git_environment())
    except GitFailure as error:
        # Only clear cold transport absence may be optional. Schema, objects, authentication,
        # unknown commits, query bounds/deadlines and every existing source mismatch stay errors.
        diagnostic = error.diagnostic.lower()
        transport_absence = (b"could not resolve host:", b"failed to connect to github.com",
                             b"network is unreachable", b"connection timed out")
        if any(marker in diagnostic for marker in transport_absence):
            raise ColdSourceUnavailable("cold dependency transport is unavailable") from error
        raise
    (metadata / "HEAD").write_bytes((commit + "\n").encode())
    materialize(source, commit)


def admit_source(source: Path, role: str, record: dict[str, Any], *,
                 existing_only: bool = False) -> dict[str, dict[str, Any]]:
    """Admit an exact owned source without adopting, deleting or repairing existing state."""
    parent_lifetime_boundary()
    source = Path(os.path.abspath(source))
    real_directory(source.parent)
    require(source.name not in ("", ".", "..") and not source.name.startswith("."),
            "invalid source destination")
    identity = source_identity(source, role, record)
    marker = owner_path(source)
    required_digests = record["files"] if role == "nanopb" else None
    if os.path.lexists(source) or os.path.lexists(marker):
        require(os.path.lexists(source) and os.path.lexists(marker),
                "partial or foreign dependency source destination")
        try:
            owner = json.loads(read_regular(marker, 16384), object_pairs_hook=unique_object)
        except (UnicodeError, json.JSONDecodeError) as error:
            raise SourceError("invalid source ownership record") from error
        require(type(owner) is dict and set(owner) == {*identity, "state", "inventorySha256"}
                and all(owner.get(key) == value for key, value in identity.items())
                and owner["state"] == "ready", "foreign or failed dependency source ownership")
        inventory = pristine_inventory(source, record["commit"], required_digests)
        require(owner["inventorySha256"] == inventory_digest(inventory),
                "source ownership inventory differs")
        return inventory
    require(not existing_only, "existing dependency source is missing; no population during build")
    fd = os.open(marker, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        write_owner(fd, {**identity, "state": "populating"})
        source.mkdir(mode=0o700)
        populate_cold(source, record["commit"], record["repository"])
        inventory = pristine_inventory(source, record["commit"], required_digests)
        write_owner(fd, {**identity, "state": "ready", "inventorySha256": inventory_digest(inventory)})
        return inventory
    finally:
        os.close(fd)


def materialized_inventory(source: Path, links: dict[str, str] | None = None) -> dict[str, dict[str, Any]]:
    """Inventory a work copy with no Git/archive/version fallback or permitted extra paths."""
    source = real_directory(source)
    result: dict[str, dict[str, Any]] = {}
    directories_seen: set[str] = set()
    total = 0
    for root, directories, names in os.walk(source, followlinks=False):
        require(len(result) + len(names) <= PATH_LIMIT
                and len(directories_seen) + len(directories) <= PATH_LIMIT,
                "work-copy path inventory exceeds its bound")
        for name in directories:
            path = real_directory(Path(root) / name)
            relative = safe_relative(str(path.relative_to(source)).encode())
            directories_seen.add(relative)
        for name in names:
            path = Path(root) / name
            relative = safe_relative(str(path.relative_to(source)).encode())
            if relative in (links or {}):
                data = literal_link(path)
                require(data == links[relative].encode(), "work-copy literal link differs")
                executable = False
            else:
                data = read_regular(path)
                executable = bool(path.stat().st_mode & 0o111)
            total += len(data)
            require(total <= TREE_LIMIT, "work-copy content inventory exceeds its bound")
            result[relative] = {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data),
                                "executable": executable}
            if relative in (links or {}):
                result[relative]["symlinkTarget"] = links[relative]
    expected_directories = {str(parent) for name in result for parent in PurePosixPath(name).parents
                            if str(parent) != "."}
    require(directories_seen == expected_directories, "work-copy has an extra empty directory")
    return result


def patched_inventory(source: Path, pristine: dict[str, dict[str, Any]],
                      manifest: dict[str, Any], marker: bytes) -> dict[str, dict[str, Any]]:
    observed = materialized_inventory(source, {n: r["symlinkTarget"] for n, r in pristine.items()
                                               if "symlinkTarget" in r})
    expected = {name: dict(row) for name, row in pristine.items()}
    for row in manifest["files"]:
        name = row["path"]
        require(name in pristine and pristine[name]["sha256"] == row["beforeSha256"],
                "patch preimage differs from the exact pristine source")
        require(name in observed and observed[name]["sha256"] == row["afterSha256"],
                "patch postimage differs from the reviewed transformation")
        expected[name] = {"sha256": row["afterSha256"], "size": observed[name]["size"],
                          "executable": pristine[name]["executable"]}
    expected[".tesla-key-patch-series"] = {"sha256": hashlib.sha256(marker).hexdigest(),
                                           "size": len(marker), "executable": False}
    require(observed == expected, "patched workspace differs from the complete permitted inventory")
    return observed


def admit_tesla_workspace(root: Path, pristine_source: Path,
                          workspace: Path, *, existing_only: bool = False
                          ) -> tuple[dict[str, dict[str, Any]], dict[str, Any]]:
    parent_lifetime_boundary()
    record = read_tesla_record(root)
    pristine = admit_source(pristine_source, "tesla-ble", record, existing_only=existing_only)
    spec = importlib.util.spec_from_file_location("tesla_source_patch_contract",
                                               root / "scripts/apply-tesla-ble-patches.py")
    require(spec is not None and spec.loader is not None, "missing patch contract")
    applier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(applier)
    manifest, _dependency = applier.load_canonical(root)
    require(manifest["sourceCommit"] == record["commit"], "patch and lock source commits disagree")
    marker_bytes = applier.series_marker(manifest)
    workspace = Path(os.path.abspath(workspace))
    real_directory(workspace.parent)
    identity = {"schema": 1, "role": "tesla-ble-patched", "root": str(workspace),
                "pristineSha256": inventory_digest(pristine),
                "recordSha256": hashlib.sha256(json.dumps(record, sort_keys=True,
                                       separators=(",", ":")).encode()).hexdigest(),
                "patchSha256": hashlib.sha256(read_regular(
                    root / "scripts/tesla-ble-patch-state.json", 16384)).hexdigest()}
    marker = owner_path(workspace)
    if os.path.lexists(workspace) or os.path.lexists(marker):
        require(os.path.lexists(workspace) and os.path.lexists(marker),
                "partial or foreign patched workspace")
        try:
            owner = json.loads(read_regular(marker, 16384), object_pairs_hook=unique_object)
        except (UnicodeError, json.JSONDecodeError) as error:
            raise SourceError("invalid patched-workspace ownership record") from error
        require(type(owner) is dict and set(owner) == {*identity, "state", "inventorySha256"}
                and all(owner.get(key) == value for key, value in identity.items())
                and owner["state"] == "ready", "foreign or failed patched workspace ownership")
        inventory = patched_inventory(workspace, pristine, manifest, marker_bytes)
        require(owner["inventorySha256"] == inventory_digest(inventory),
                "patched-workspace ownership inventory differs")
        return inventory, identity
    require(not existing_only, "existing patched workspace is missing; no population during preflight")
    fd = os.open(marker, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        write_owner(fd, {**identity, "state": "populating"})
        workspace.mkdir(mode=0o700)
        for name, row in pristine.items():
            data = (literal_link(pristine_source / name) if "symlinkTarget" in row
                    else read_regular(pristine_source / name))
            require(hashlib.sha256(data).hexdigest() == row["sha256"],
                    "pristine source changed before work-copy materialization")
            path = workspace / name
            path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            real_directory(path.parent)
            if "symlinkTarget" in row:
                os.symlink(row["symlinkTarget"], path)
            else:
                destination = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                                      0o700 if row["executable"] else 0o600)
                with os.fdopen(destination, "wb") as output:
                    output.write(data)
        require(materialized_inventory(workspace, {n: r["symlinkTarget"] for n, r in pristine.items()
                                                  if "symlinkTarget" in r}) == pristine,
                "work copy differs before the patch transformation")
        applier.apply_series(workspace, root / "patches/tesla-ble", manifest, quiet=True)
        inventory = patched_inventory(workspace, pristine, manifest, marker_bytes)
        write_owner(fd, {**identity, "state": "ready", "inventorySha256": inventory_digest(inventory)})
        return inventory, identity
    finally:
        os.close(fd)


def configure_inputs(root: Path, source: Path) -> list[str]:
    paths = {root / "scripts/dependency_source.py", root / "scripts/nanopb-source.json",
             owner_path(source), source}
    for directory, directories, names in os.walk(source, followlinks=False):
        paths.add(Path(directory))
        paths.update(Path(directory) / name for name in (*directories, *names))
        require(len(paths) <= 2 * PATH_LIMIT, "configure source inventory exceeds its bound")
    require(all(not any(character in str(path) for character in (";", "\r", "\n"))
                for path in paths), "unsafe CMake source input path")
    return sorted(str(path) for path in paths)


def preflight_harness(root: Path, cache: Path) -> None:
    """Refuse every existing selected cache before a different cold source can skip offline."""
    cache = real_directory(cache)
    nanopb = read_nanopb_record(root / "scripts/nanopb-source.json")
    tesla = read_tesla_record(root)
    sources = ((cache / "nanopb-pristine", "nanopb", nanopb),
               (cache / "tesla-pristine", "tesla-ble", tesla))
    for path, role, record in sources:
        if os.path.lexists(path) or os.path.lexists(owner_path(path)):
            admit_source(path, role, record, existing_only=True)
    workspace = cache / "tesla-patched"
    if os.path.lexists(workspace) or os.path.lexists(owner_path(workspace)):
        require(os.path.lexists(cache / "tesla-pristine")
                and os.path.lexists(owner_path(cache / "tesla-pristine")),
                "existing patched workspace has no admitted pristine cache")
        admit_tesla_workspace(root, cache / "tesla-pristine", workspace, existing_only=True)


def object_directory(root: Path, role: str, proof: dict[str, str], salt: str) -> Path:
    parent_lifetime_boundary()
    root = real_directory(root)
    key = hashlib.sha256(json.dumps({**proof, "salt": salt}, sort_keys=True,
                                    separators=(",", ":")).encode()).hexdigest()
    path = root / (role + "-" + key)
    marker = owner_path(path)
    identity = {"schema": 1, "role": "harness-objects", "root": str(path), "key": key}
    if os.path.lexists(path) or os.path.lexists(marker):
        require(os.path.lexists(path) and os.path.lexists(marker), "partial or foreign object cache")
        require(read_regular(marker, 16384) == (json.dumps(identity, sort_keys=True,
                    separators=(",", ":")) + "\n").encode(), "foreign object-cache ownership")
        real_directory(path)
        names = list(path.iterdir())
        require(len(names) <= PATH_LIMIT, "oversized object cache")
        for leaf in names:
            require(re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*\.o", leaf.name) is not None,
                    "unexpected object-cache entry")
            info = leaf.lstat()
            require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
                    "aliased or nonregular object-cache entry")
    else:
        fd = os.open(marker, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        try:
            write_owner(fd, identity)
            path.mkdir(mode=0o700)
        finally:
            os.close(fd)
    return path


def main() -> int:
    import argparse
    import sys
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("role", choices=("nanopb", "tesla-ble", "harness-preflight"))
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--workspace", type=Path)
    parser.add_argument("--configure-inputs", action="store_true")
    parser.add_argument("--existing-only", action="store_true")
    parser.add_argument("--object-root", type=Path)
    parser.add_argument("--cache-salt", default="")
    options = parser.parse_args()
    try:
        parent_lifetime_boundary(cli=True)
        root = real_directory(options.root)
        require(options.source.is_absolute(), "source destination must be absolute")
        if options.role == "harness-preflight":
            require(options.workspace is None and options.object_root is None
                    and not options.configure_inputs and not options.cache_salt
                    and not options.existing_only,
                    "unsupported harness-preflight option")
            preflight_harness(root, options.source)
            print(json.dumps({"preflight": "existing selected caches admitted"}))
            return 0
        if options.role == "nanopb":
            require(options.workspace is None, "nanopb has no patched workspace")
            read_tesla_record(root)  # Validate all existing pins/locks/patches before cold effects.
            record = read_nanopb_record(root / "scripts/nanopb-source.json")
            inventory = admit_source(options.source, "nanopb", record,
                                     existing_only=options.existing_only)
            identity = source_identity(options.source, "nanopb", record)
        else:
            require(options.workspace is not None and options.workspace.is_absolute()
                    and not options.configure_inputs and not options.existing_only,
                    "Tesla requires one absolute patched workspace")
            inventory, identity = admit_tesla_workspace(root, options.source, options.workspace)
        if options.configure_inputs:
            print(record["commit"])
            print("\n".join(configure_inputs(root, options.source)))
        else:
            proof = {"source": str(options.workspace or options.source),
                     "inventorySha256": inventory_digest(inventory),
                     "identitySha256": hashlib.sha256(json.dumps(identity, sort_keys=True,
                                        separators=(",", ":")).encode()).hexdigest()}
            if options.object_root is not None:
                proof["objectDirectory"] = str(object_directory(options.object_root, options.role,
                                                                 proof, options.cache_salt))
            print(json.dumps(proof, sort_keys=True))
        return 0
    except ColdSourceUnavailable:
        print("dependency source: cold transport unavailable; incomplete owned state retained",
              file=sys.stderr)
        return 75
    except SourceError as error:
        # SourceError messages are fixed admission diagnostics, never Git stderr/source bodies.
        print("dependency source admission failed: " + str(error), file=sys.stderr)
        return 1
    except (OSError, ValueError, subprocess.SubprocessError):
        print("dependency source admission failed; existing state was not repaired", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
