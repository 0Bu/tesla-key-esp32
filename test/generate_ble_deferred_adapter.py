#!/usr/bin/env python3
"""Extract complete production BLE mailbox programs; generate no decision-model substitute.

Usage: generate_ble_deferred_adapter.py ROOT OUTPUT_DIRECTORY
The caller must fail on an unsuccessful generation before compiling the native PSA harness.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile


def read_source(path: Path) -> str:
    if not stat.S_ISREG(path.lstat().st_mode) or path.is_symlink():
        raise ValueError(f"source must be a direct regular file: {path.name}")
    body = path.read_bytes()
    if len(body) > 2 * 1024 * 1024 or b"\x00" in body:
        raise ValueError("source is oversized or contains NUL")
    return body.decode("utf-8")


def close_brace(text: str, start: int) -> int:
    """Balanced C++ subset scanner, ignoring quoted strings and both comment forms.

    Raw literals are refused in the extracted region rather than guessed. The actual programs
    currently contain no raw strings. An unsupported lexical change needs explicit review.
    """
    depth, i = 0, start
    state = "code"
    while i < len(text):
        pair = text[i:i + 2]
        char = text[i]
        if state == "line":
            if char == "\n":
                state = "code"
        elif state == "block":
            if pair == "*/":
                state = "code"
                i += 1
        elif state in {'"', "'"}:
            if char == "\\":
                i += 1
            elif char == state:
                state = "code"
        elif pair == "//":
            state = "line"
            i += 1
        elif pair == "/*":
            state = "block"
            i += 1
        elif pair == 'R"':
            raise ValueError("raw literal in adapter requires a reviewed scanner extension")
        elif char in {'"', "'"}:
            state = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError("unterminated production definition")


def method(source: str, result: str, name: str) -> str:
    marker = result + " VehicleController::" + name + "("
    if source.count(marker) != 1:
        raise ValueError("expected exactly one whole definition: " + name)
    start = source.index(marker)
    line_start = source.rfind("\n", 0, start) + 1
    if source[line_start:start].strip():
        raise ValueError("unexpected declaration prefix requires explicit extraction review: " + name)
    opened = source.index("{", start)
    return source[start:close_brace(source, opened)]


def atomic_write(path: Path, data: bytes) -> None:
    if path.exists() and (path.is_symlink() or not path.is_file()):
        raise ValueError("generated output is not a direct regular file")
    descriptor, temporary = tempfile.mkstemp(prefix=".ble-deferred-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def generate(root: Path, out: Path) -> None:
    root = root.resolve(strict=True)
    if out.is_symlink():
        raise ValueError("output directory must not be a symlink")
    out = out.resolve(strict=True)
    if not out.is_dir() or any(parent == root / "main" or parent == root / "test"
                               for parent in (out, *out.parents)):
        raise ValueError("output must be a separate generated directory")
    inputs = {relative: read_source(root / relative) for relative in (
        "main/vehicle_ctrl.hpp", "main/vehicle_ctrl.cpp", "main/vehicle_telemetry.cpp",
        "main/logic/ble_deferred_queue.hpp", "main/logic/rx_framing.hpp",
    )}
    header = inputs["main/vehicle_ctrl.hpp"]
    marker = "    using BleHostEvent = tk::BleDeferredQueue::Event;"
    if header.count(marker) != 1:
        raise ValueError("expected one production mailbox type")
    start = header.index(marker)
    struct_marker = "    struct BleHostMailbox {"
    if header.count(struct_marker) != 1:
        raise ValueError("expected one production mailbox structure")
    opened = header.index("{", header.index(struct_marker))
    end = close_brace(header, opened)
    if header[end:end + 1] != ";":
        raise ValueError("mailbox structure terminator missing")
    declarations = [header[start:end + 1]]
    for name in ("ble_events_", "auth_fail_streak_", "ble_fault_", "rx_framer_reset_requested_"):
        matches = re.findall(r"(?m)^    (?:BleHostMailbox|std::atomic<(?:int|bool)>)\s+" +
                             re.escape(name) + r"(?:\{[^\n;]*\})?;", header)
        if len(matches) != 1:
            raise ValueError("expected exactly one production field: " + name)
        declarations.append(matches[0])
    signatures = (
        ("bool", "ble_link_event_cb_"), ("bool", "ble_rx_event_cb_"),
        ("bool", "enqueue_ble_link_event_"), ("bool", "enqueue_ble_rx_event_"),
        ("bool", "ble_host_events_ready_"), ("bool", "pop_ble_host_event_"),
        ("void", "clear_ble_host_events_"), ("void", "process_ble_host_events_"),
    )
    bodies, programs = [], []
    for result, name in signatures:
        matches = re.findall(r"(?m)^    (?:static )?" + result + " " + re.escape(name) +
                             r"\([^;{}]*\)(?: noexcept)?;", header)
        if len(matches) != 1:
            raise ValueError("expected exactly one production declaration: " + name)
        declarations.append(matches[0])
        relative = "main/vehicle_telemetry.cpp" if name == "process_ble_host_events_" else "main/vehicle_ctrl.cpp"
        body = method(inputs[relative], result, name)
        bodies.append(body)
        programs.append({"source": relative, "method": name,
                         "sha256": hashlib.sha256(body.encode()).hexdigest(),
                         "bytes": len(body.encode())})
    # No replacement, wrapper, body slicing, injected early-return or class rename is permitted.
    generated = {
        "ble-deferred-types.inc": ("\n".join(declarations) + "\n").encode(),
        "ble-deferred-native.inc": ("\n\n".join(bodies) + "\n").encode(),
    }
    proof = {"schema": "whole-production-ble-mailbox-extraction-v1", "programs": programs,
             "sources": {name: hashlib.sha256(text.encode()).hexdigest()
                         for name, text in inputs.items()},
             "outputs": {name: hashlib.sha256(body).hexdigest() for name, body in generated.items()},
             "classRenamingOrBodyRewriting": False}
    generated["ble-deferred-extraction.json"] = (json.dumps(proof, indent=2) + "\n").encode()
    for name, body in generated.items():
        destination = out / name
        if destination.resolve() in {root / relative for relative in inputs}:
            raise ValueError("output aliases source")
        atomic_write(destination, body)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: generate_ble_deferred_adapter.py ROOT OUTPUT_DIRECTORY")
    generate(Path(sys.argv[1]), Path(sys.argv[2]))
