#!/usr/bin/env python3
"""Fail closed when a pure-logic header has no explicit host-test owner."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


MANIFEST = Path("test/logic_test_ownership.json")
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx"}

# An ownership record is useful only when its translation unit is part of a runner that is
# actually executed. Keep the three supported bindings explicit and fail closed when CMake, the
# cmake-less fallback, or the pinned-cJSON external gate drifts.
OWNER_BINDINGS = {
    "test/test_logic.cpp": ("cmake", "logic_tests"),
    "test/test_nvs_storage.cpp": ("cmake", "nvs_storage_tests"),
    "test/test_mqtt_json_publish.cpp": ("external", "test/run-mqtt-json-publish-tests.sh"),
}


class ContractError(RuntimeError):
    pass


def cpp_code_only(text: str) -> str:
    """Blank comments and string/character literals before locating test entry points."""
    out: list[str] = []
    i = 0
    state = "code"
    quote = ""
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if ch == "/" and nxt == "/":
                out.extend("  ")
                i += 2
                state = "line"
                continue
            if ch == "/" and nxt == "*":
                out.extend("  ")
                i += 2
                state = "block"
                continue
            if ch in {'"', "'"}:
                quote = ch
                out.append(" ")
                i += 1
                state = "literal"
                continue
            out.append(ch)
            i += 1
            continue
        if state == "line":
            out.append("\n" if ch == "\n" else " ")
            if ch == "\n":
                state = "code"
            i += 1
            continue
        if state == "block":
            if ch == "*" and nxt == "/":
                out.extend("  ")
                i += 2
                state = "code"
            else:
                out.append("\n" if ch == "\n" else " ")
                i += 1
            continue
        if ch == "\\" and nxt:
            out.extend("  ")
            i += 2
        elif ch == quote:
            out.append(" ")
            i += 1
            state = "code"
        else:
            out.append("\n" if ch == "\n" else " ")
            i += 1
    return "".join(out)


def load_manifest(root: Path) -> dict[str, dict[str, str]]:
    path = root / MANIFEST
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ContractError(f"ownership manifest is unreadable: {exc}") from exc
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise ContractError("ownership manifest needs schema_version 1")
    headers = value.get("headers")
    if not isinstance(headers, dict):
        raise ContractError("ownership manifest headers must be an object")
    return headers


def logic_headers(root: Path) -> dict[str, Path]:
    base = root / "main/logic"
    return {
        path.relative_to(base).as_posix(): path
        for path in base.rglob("*")
        if path.is_file() and path.suffix.lower() in HEADER_SUFFIXES
    }


def braced_body(code: str, opening: int, label: str) -> str:
    depth = 0
    for pos in range(opening, len(code)):
        if code[pos] == "{":
            depth += 1
        elif code[pos] == "}":
            depth -= 1
            if depth == 0:
                return code[opening + 1 : pos]
    raise ContractError(f"unterminated function body: {label}")


def function_body(code: str, name: str, return_type: str) -> str:
    matches = list(re.finditer(
        rf"(?m)^\s*(?:static\s+)?{return_type}\s+{re.escape(name)}\s*\(\s*\)\s*\{{",
        code,
    ))
    if len(matches) != 1:
        raise ContractError(f"{name} must have exactly one zero-argument definition")
    opening = code.find("{", matches[0].start())
    return braced_body(code, opening, name)


def direct_test_registrations(main_body: str) -> list[str]:
    """Return explicit top-level `test_name();` registrations from main().

    Calls nested under a branch/loop (including `if (false)`) are deliberately not registrations.
    This makes execution wiring auditable without pretending that a loose call regex proves reachability.
    """
    registrations: list[str] = []
    depth = 0
    offset = 0
    for line in main_body.splitlines(keepends=True):
        stripped = line.strip()
        if depth == 0:
            match = re.fullmatch(r"(test_[A-Za-z0-9_]+)\s*\(\s*\)\s*;", stripped)
            if match:
                prefix = main_body[:offset]
                if re.search(r"(?m)^\s*(?:return|throw|goto)\b[^;]*;\s*$", prefix):
                    raise ContractError(
                        f"{match.group(1)} registration appears after an unconditional exit"
                    )
                registrations.append(match.group(1))
        depth += line.count("{") - line.count("}")
        if depth < 0:
            raise ContractError("malformed main() while reading test registrations")
        offset += len(line)
    if depth != 0:
        raise ContractError("malformed nested block in main() test runner")
    return registrations


def require_owner_binding(root: Path, owner: str) -> None:
    binding = OWNER_BINDINGS.get(owner)
    if binding is None:
        raise ContractError(f"{owner}: owner file has no reviewed execution binding")
    kind, value = binding
    if kind == "cmake":
        cmake = (root / "test/CMakeLists.txt").read_text(encoding="utf-8")
        run_mock = (root / "scripts/run-mock-tests.sh").read_text(encoding="utf-8")
        source_name = Path(owner).name
        target_match = re.search(
            rf"add_executable\(\s*{re.escape(value)}\b(.*?)\)", cmake, re.DOTALL
        )
        if not target_match or source_name not in target_match.group(1):
            raise ContractError(f"{owner}: CMake target {value} does not compile the owner")
        if not re.search(
            rf"add_test\(\s*NAME\s+{re.escape(value)}\s+COMMAND\s+{re.escape(value)}\s*\)",
            cmake,
        ):
            raise ContractError(f"{owner}: CMake target {value} is not executed by CTest")
        compile_marker = f'-o "$BUILD_DIR/{value}"'
        run_marker = f'run_gate "execute {"pure-logic" if value == "logic_tests" else "NVS adapter"} tests" 120 "$BUILD_DIR/{value}"'
        if source_name not in run_mock or compile_marker not in run_mock or run_marker not in run_mock:
            raise ContractError(f"{owner}: cmake-less fallback does not compile and run {value}")
        return

    script = (root / value).read_text(encoding="utf-8")
    invoker = (root / "scripts/ci-build-verify.sh").read_text(encoding="utf-8")
    binary = Path(owner).stem
    if (owner not in script or f'-o "$work/{binary}"' not in script or
            not re.search(rf'(?m)^\s*"\$work/{re.escape(binary)}"\s*$', script)):
        raise ContractError(f"{owner}: external gate does not compile and run its owner binary")
    invocation = f"bash ./{value}"
    if invocation not in invoker:
        raise ContractError(f"{owner}: external gate is not executed by pinned build verification")


def included_files(root: Path, entry: Path) -> set[Path]:
    """Resolve repository-local quoted includes transitively from a host-test owner."""
    main = (root / "main").resolve()
    pending = [entry.resolve()]
    seen: set[Path] = set()
    while pending:
        path = pending.pop()
        if path in seen or not path.is_file():
            continue
        seen.add(path)
        source = path.read_text(encoding="utf-8")
        for include in re.findall(r'(?m)^\s*#\s*include\s*"([^"\n]+)"', source):
            candidates = [path.parent / include, main / include]
            for candidate in candidates:
                resolved = candidate.resolve()
                if resolved.is_file() and (resolved == main or main in resolved.parents):
                    pending.append(resolved)
                    break
    return seen


def validate(root: Path) -> list[str]:
    headers = load_manifest(root)
    actual_paths = logic_headers(root)
    actual = set(actual_paths)
    declared = set(headers)
    missing = sorted(actual - declared)
    stale = sorted(declared - actual)
    if missing:
        raise ContractError("pure-logic headers missing a test owner: " + ", ".join(missing))
    if stale:
        raise ContractError("ownership entries name missing headers: " + ", ".join(stale))

    owners: list[str] = []
    checked_bindings: set[str] = set()
    owner_cache: dict[str, tuple[str, list[str]]] = {}
    for header in sorted(headers):
        record = headers[header]
        if not isinstance(record, dict) or set(record) != {"test", "evidence"}:
            raise ContractError(f"{header}: owner record must contain exactly test and evidence")
        owner = record["test"]
        evidence = record["evidence"]
        if not isinstance(owner, str) or not owner.startswith("test/") or ".." in Path(owner).parts:
            raise ContractError(f"{header}: unsafe or non-test owner path")
        if not isinstance(evidence, str) or not re.fullmatch(r"test_[A-Za-z0-9_]+\(", evidence):
            raise ContractError(
                f"{header}: evidence must name a concrete zero-argument test function"
            )
        owner_path = root / owner
        if not owner_path.is_file() or owner_path.is_symlink():
            raise ContractError(f"{header}: owner test is missing or a symlink: {owner}")
        function = evidence[:-1]
        source = owner_path.read_text(encoding="utf-8")
        if actual_paths[header].resolve() not in included_files(root, owner_path):
            raise ContractError(
                f"{header}: owner {owner} does not include the production header directly or transitively"
            )
        if owner not in owner_cache:
            code = cpp_code_only(source)
            main_body = function_body(code, "main", "int")
            owner_cache[owner] = (code, direct_test_registrations(main_body))
        code, registrations = owner_cache[owner]
        try:
            function_body(code, function, "void")
        except ContractError as exc:
            raise ContractError(f"{header}: {exc} in {owner}") from exc
        if registrations.count(function) != 1:
            raise ContractError(
                f"{header}: {function} must be registered exactly once directly in main() of {owner}"
            )
        if owner not in checked_bindings:
            require_owner_binding(root, owner)
            checked_bindings.add(owner)
        owners.append(owner)
    return owners


def compiler() -> str:
    configured = os.environ.get("CXX", "")
    if configured:
        resolved = shutil.which(configured)
        if resolved:
            return resolved
        raise ContractError(f"configured CXX was not found: {configured}")
    for candidate in ("g++", "clang++"):
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
    raise ContractError("standalone header compilation needs g++ or clang++")


def compile_headers(root: Path) -> None:
    cxx = compiler()
    headers = logic_headers(root)
    with tempfile.TemporaryDirectory(prefix="tesla-key-header-contract-") as directory:
        source = Path(directory) / "header.cpp"
        for relative, header in sorted(headers.items()):
            source.write_text(
                f'#include "logic/{relative}"\nint main() {{ return 0; }}\n',
                encoding="utf-8",
            )
            result = subprocess.run(
                [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                 "-I", str(root / "main"), str(source)],
                text=True,
                capture_output=True,
                timeout=20,
                check=False,
            )
            if result.returncode != 0:
                details = (result.stdout + result.stderr).strip()
                raise ContractError(f"{relative}: standalone compilation failed:\n{details}")


def self_test(root: Path) -> None:
    validate(root)

    def make_fixture(directory: str) -> Path:
        fixture = Path(directory)
        shutil.copytree(root / "main", fixture / "main")
        for relative in (
            MANIFEST.as_posix(),
            "test/test_logic.cpp",
            "test/test_nvs_storage.cpp",
            "test/test_mqtt_json_publish.cpp",
            "test/CMakeLists.txt",
            "test/run-mqtt-json-publish-tests.sh",
            "scripts/run-mock-tests.sh",
            "scripts/ci-build-verify.sh",
        ):
            destination = fixture / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / relative, destination)
        return fixture

    def expect_failure(fixture: Path, expected: str, label: str) -> None:
        try:
            validate(fixture)
        except ContractError as exc:
            if expected not in str(exc):
                raise ContractError(f"{label} failed for the wrong reason: {exc}") from exc
        else:
            raise ContractError(f"{label} mutation was accepted")

    # Recursive inventory must notice every shipped header suffix, not just top-level *.hpp.
    with tempfile.TemporaryDirectory(prefix="tesla-key-ownership-nested-") as directory:
        fixture = make_fixture(directory)
        nested = fixture / "main/logic/nested/unowned_canary.hh"
        nested.parent.mkdir(parents=True)
        nested.write_text("#pragma once\n", encoding="utf-8")
        expect_failure(fixture, "nested/unowned_canary.hh", "nested-header canary")

    # Moving a header to a test TU that does not include it is not ownership, even if that TU has
    # a real, registered test with a plausible name.
    with tempfile.TemporaryDirectory(prefix="tesla-key-ownership-unrelated-") as directory:
        fixture = make_fixture(directory)
        manifest = json.loads((fixture / MANIFEST).read_text(encoding="utf-8"))
        manifest["headers"]["mqtt_discovery_registry.hpp"] = {
            "test": "test/test_nvs_storage.cpp",
            "evidence": "test_nvs_contract(",
        }
        (fixture / MANIFEST).write_text(json.dumps(manifest), encoding="utf-8")
        expect_failure(fixture, "does not include", "unrelated-owner canary")

    # Definition text, comments and string literals cannot replace an explicit top-level runner
    # registration; a registration under if(false) is equally unreachable.
    for replacement, label in (
        ('    const char* decoy = "test_http_route();"; // test_http_route();',
         "invocation-removal canary"),
        ("    if (false) {\n        test_http_route();\n    }", "if-false canary"),
    ):
        with tempfile.TemporaryDirectory(prefix="tesla-key-ownership-invocation-") as directory:
            fixture = make_fixture(directory)
            logic = fixture / "test/test_logic.cpp"
            source = logic.read_text(encoding="utf-8")
            marker = "    test_http_route();"
            if source.count(marker) != 1:
                raise ContractError("invocation canary fixture cannot identify test_http_route call")
            logic.write_text(source.replace(marker, replacement, 1), encoding="utf-8")
            expect_failure(fixture, "registered exactly once directly", label)

    # A compiled target that is no longer a CTest, and an external owner binary that is no longer
    # run, must both invalidate every ownership record bound to them.
    for relative, old, new, expected, label in (
        ("test/CMakeLists.txt", "add_test(NAME nvs_storage_tests COMMAND nvs_storage_tests)",
         "# removed nvs_storage_tests CTest", "not executed by CTest", "CTest-wiring canary"),
        ("test/run-mqtt-json-publish-tests.sh", '\n"$work/test_mqtt_json_publish"\n',
         "\n# removed external owner execution\n", "does not compile and run",
         "external-run canary"),
    ):
        with tempfile.TemporaryDirectory(prefix="tesla-key-ownership-wiring-") as directory:
            fixture = make_fixture(directory)
            path = fixture / relative
            source = path.read_text(encoding="utf-8")
            if source.count(old) != 1:
                raise ContractError(f"{label} fixture marker is not unique: {old}")
            path.write_text(source.replace(old, new, 1), encoding="utf-8")
            expect_failure(fixture, expected, label)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--compile", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        owners = validate(root)
        if args.compile:
            compile_headers(root)
        if args.self_test:
            self_test(root)
    except (ContractError, subprocess.TimeoutExpired) as exc:
        print(f"logic-test-ownership: {exc}", file=sys.stderr)
        return 1
    print(
        f"logic-test-ownership: PASS ({len(load_manifest(root))} headers, "
        f"{len(set(owners))} owner files" + (", standalone compile" if args.compile else "") + ")"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
