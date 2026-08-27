#!/usr/bin/env python3
"""Verify the effective target and optimisation flags of an ESP-IDF firmware build."""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from collections import Counter
from collections.abc import Callable
from typing import Any


TARGETS = ("esp32", "esp32s3", "esp32c3", "esp32c6")
COMPILE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".s"}
MAIN_SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
MANAGED_COMPONENTS = {"espressif__mdns", "yoziru__tesla-ble"}
NANOPB_GENERATED_SOURCES = {
    "_deps/nanopb-src/pb_common.c":
        "8d2ec28baaaf2b7a5e90e4cb2fa9700d21cef7f826f051a637c30b7a1e6a0516",
    "_deps/nanopb-src/pb_decode.c":
        "6c2fc2f357bffdb774c1d329b533e981498d58405c0b1ef066f5a87fd46b5a17",
    "_deps/nanopb-src/pb_encode.c":
        "d8dff2a1acc58683095a41b0dc3103ba46248e4a8d8c4e20f5810be04127b650",
}
X509_BUNDLE_SHA256 = "d47b6376ca09b7c17d379e1e828aff8e28c4c0fb689e9e3128c7a3c063912592"
FORBIDDEN_FIRMWARE_FLAG_PREFIXES = (
    "-include",
    "-imacros",
    "-fplugin",
    "-specs",
    "--specs",
    "-wrapper",
    "-Xclang",
    "-Xpreprocessor",
    "-Wp,",
    "-iprefix",
    "-iwithprefix",
    "--sysroot",
    "-isysroot",
    "-I-",
    "-B",
    "-trigraphs",
)
EXPECTED_CXX_COMPILER = {
    "esp32": "xtensa-esp32-elf-g++",
    "esp32s3": "xtensa-esp32s3-elf-g++",
    "esp32c3": "riscv32-esp-elf-g++",
    "esp32c6": "riscv32-esp-elf-g++",
}
EXPECTED_C_COMPILER = {
    "esp32": "xtensa-esp32-elf-gcc",
    "esp32s3": "xtensa-esp32s3-elf-gcc",
    "esp32c3": "riscv32-esp-elf-gcc",
    "esp32c6": "riscv32-esp-elf-gcc",
}
EXPECTED_AR = {
    "esp32": "xtensa-esp32-elf-ar",
    "esp32s3": "xtensa-esp32s3-elf-ar",
    "esp32c3": "riscv32-esp-elf-ar",
    "esp32c6": "riscv32-esp-elf-ar",
}
EXPECTED_RANLIB = {
    "esp32": "xtensa-esp32-elf-ranlib",
    "esp32s3": "xtensa-esp32s3-elf-ranlib",
    "esp32c3": "riscv32-esp-elf-ranlib",
    "esp32c6": "riscv32-esp-elf-ranlib",
}
ALLOWED_GENERATED_MAIN_HEADERS = {
    "config/sdkconfig.h": None,
    "_deps/nanopb-src/pb.h":
        "a2ecdca9fdaeef5f4972ed983540c0d6fb0a5c402a2e0b0349d7e1bc5e188d29",
    "_deps/nanopb-src/pb_common.h":
        "6495a691aca68d6973f2274b5dd54b74fbb57f6b019c45fff255a857fe1abcfd",
    "_deps/nanopb-src/pb_decode.h":
        "1747746e5961de5789bcf0795588da0790cd18b2e4e706ad9c7099a0fa1cc83f",
    "_deps/nanopb-src/pb_encode.h":
        "9aa00fee4ff08adf0da16e33a55be08810ea657800a648dc78f82e89c60c10cf",
}
DependencyLoader = Callable[[Path, tuple[str, ...]], dict[str, tuple[Path, ...]]]
CommandLoader = Callable[[Path, tuple[str, ...]], dict[str, tuple[str, ...]]]
GraphValidator = Callable[
    [Path, str, Path, Path | None, Path, dict[str, Path], CommandLoader], None
]
NINJA_DEPENDENCY_FLAGS = ("-MD", "-MT", "{output}", "-MF", "{output}.d")
BUILD_INJECTION_ENV = (
    "CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
    "DEPENDENCIES_OUTPUT", "SUNPRO_DEPENDENCIES", "GCC_EXEC_PREFIX",
    "COMPILER_PATH", "LIBRARY_PATH",
)


class SemanticsError(ValueError):
    pass


def canonical_regular_file(path: Path, label: str) -> Path:
    if path.is_symlink() or not path.is_file():
        raise SemanticsError(f"{label} is missing, non-regular or symlinked: {path}")
    return path.resolve(strict=True)


def parse_ninja_dependencies(
    text: str, expected_outputs: tuple[str, ...]
) -> dict[str, tuple[Path, ...]]:
    """Parse `ninja -t deps` output for the exact requested main objects."""
    expected = set(expected_outputs)
    records: dict[str, tuple[Path, ...]] = {}
    lines = text.splitlines()
    position = 0
    header = re.compile(
        r"^(?P<output>[^\r\n]+): #deps (?P<count>\d+), deps mtime \d+ \((?P<state>[A-Z]+)\)$"
    )
    while position < len(lines):
        if not lines[position]:
            position += 1
            continue
        match = header.fullmatch(lines[position])
        if not match:
            raise SemanticsError(f"malformed Ninja dependency record: {lines[position]!r}")
        output = match.group("output")
        if output not in expected:
            raise SemanticsError(f"unexpected Ninja dependency target: {output}")
        if output in records:
            raise SemanticsError(f"duplicate Ninja dependency target: {output}")
        if match.group("state") != "VALID":
            raise SemanticsError(
                f"Ninja dependency record is not current for {output}: {match.group('state')}"
            )
        position += 1
        dependencies: list[Path] = []
        while position < len(lines) and lines[position]:
            line = lines[position]
            if not line.startswith("    ") or not line.strip():
                raise SemanticsError(f"malformed Ninja dependency path for {output}: {line!r}")
            dependencies.append(Path(line.strip()).resolve(strict=False))
            position += 1
        if len(dependencies) != int(match.group("count")):
            raise SemanticsError(
                f"Ninja dependency count mismatch for {output}: "
                f"declared {match.group('count')}, got {len(dependencies)}"
            )
        if len(set(dependencies)) != len(dependencies):
            raise SemanticsError(f"duplicate header dependency for {output}")
        records[output] = tuple(dependencies)
    missing = sorted(expected - set(records))
    if missing:
        raise SemanticsError(f"Ninja dependency records are missing firmware objects: {missing}")
    return records


def load_ninja_dependencies(
    build_root: Path, outputs: tuple[str, ...]
) -> dict[str, tuple[Path, ...]]:
    ninja_name = shutil.which("ninja")
    if not ninja_name:
        raise SemanticsError("pinned Ninja executable is unavailable")
    ninja = Path(ninja_name).resolve(strict=False)
    if ninja != Path("/usr/bin/ninja"):
        raise SemanticsError(f"Ninja dependency reader is outside the pinned toolchain: {ninja}")
    try:
        completed = subprocess.run(
            [str(ninja), "-t", "deps", *outputs],
            cwd=build_root,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise SemanticsError(f"cannot read Ninja header dependencies: {exc}") from exc
    if completed.returncode != 0 or completed.stderr.strip():
        detail = completed.stderr.strip() or f"exit {completed.returncode}"
        raise SemanticsError(f"Ninja header dependency query failed: {detail}")
    return parse_ninja_dependencies(completed.stdout, outputs)


def load_ninja_commands(
    build_root: Path, outputs: tuple[str, ...]
) -> dict[str, tuple[str, ...]]:
    """Read the final expanded Ninja command for each main object, excluding its dependency DAG."""
    ninja_name = shutil.which("ninja")
    if not ninja_name:
        raise SemanticsError("pinned Ninja executable is unavailable")
    ninja = Path(ninja_name).resolve(strict=False)
    if ninja != Path("/usr/bin/ninja"):
        raise SemanticsError(f"Ninja command reader is outside the pinned toolchain: {ninja}")
    records: dict[str, tuple[str, ...]] = {}
    for output in outputs:
        try:
            completed = subprocess.run(
                [str(ninja), "-t", "commands", "-s", output],
                cwd=build_root,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=30,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise SemanticsError(f"cannot read Ninja compile command for {output}: {exc}") from exc
        if completed.returncode != 0 or completed.stderr.strip():
            detail = completed.stderr.strip() or f"exit {completed.returncode}"
            raise SemanticsError(f"Ninja compile command query failed for {output}: {detail}")
        lines = completed.stdout.splitlines()
        if len(lines) != 1 or not lines[0].strip():
            raise SemanticsError(
                f"Ninja must expose exactly one final compile command for {output}"
            )
        try:
            tokens = tuple(shlex.split(lines[0], posix=True))
        except ValueError as exc:
            raise SemanticsError(f"malformed Ninja compile command for {output}: {exc}") from exc
        if not tokens:
            raise SemanticsError(f"empty Ninja compile command for {output}")
        records[output] = tokens
    return records


def load_ninja_default_commands(build_root: Path) -> tuple[tuple[str, ...], ...]:
    """Read every command reachable from Ninja's default target.

    Target-local ``-t commands -s`` is ideal for one producer, but deliberately omits commands
    attached to a later ``ALL`` target.  The default closure is the evidence that no post-build
    command can replace the app after the audited elf2image rule.
    """
    ninja_name = shutil.which("ninja")
    if not ninja_name:
        raise SemanticsError("pinned Ninja executable is unavailable")
    ninja = Path(ninja_name).resolve(strict=False)
    if ninja != Path("/usr/bin/ninja"):
        raise SemanticsError(f"Ninja default-command reader is outside the pinned toolchain: {ninja}")
    try:
        completed = subprocess.run(
            [str(ninja), "-t", "commands"], cwd=build_root, check=False,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=60,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise SemanticsError(f"cannot read Ninja default command closure: {exc}") from exc
    if completed.returncode != 0 or completed.stderr.strip():
        detail = completed.stderr.strip() or f"exit {completed.returncode}"
        raise SemanticsError(f"Ninja default command query failed: {detail}")
    records: list[tuple[str, ...]] = []
    for line in completed.stdout.splitlines():
        if not line.strip():
            raise SemanticsError("Ninja default command closure contains an empty command")
        try:
            tokens = tuple(shlex.split(line, posix=True))
        except ValueError as exc:
            raise SemanticsError(f"malformed Ninja default command: {exc}") from exc
        if not tokens:
            raise SemanticsError("Ninja default command closure contains an empty command")
        records.append(tokens)
    if not records:
        raise SemanticsError("Ninja default command closure is empty")
    return tuple(records)


def expected_ninja_command(
    output: str, compile_database_tokens: tuple[str, ...]
) -> tuple[str, ...]:
    dependency_controls = {
        token
        for token in compile_database_tokens
        if token in {"-M", "-MM", "-MG", "-MP", "-MD", "-MMD"}
        or token.startswith(("-MF", "-MT", "-MQ"))
    }
    if dependency_controls:
        raise SemanticsError(
            f"compile_commands.json controls Ninja dependency output for {output}: "
            f"{sorted(dependency_controls)!r}"
        )
    output_positions = [
        index for index, token in enumerate(compile_database_tokens) if token == "-o"
    ]
    if (
        len(output_positions) != 1
        or output_positions[0] + 1 >= len(compile_database_tokens)
        or compile_database_tokens[output_positions[0] + 1] != output
    ):
        raise SemanticsError(
            f"compile_commands.json has an ambiguous output token for {output}"
        )
    output_position = output_positions[0]
    dependency_tokens = tuple(
        token.format(output=output) for token in NINJA_DEPENDENCY_FLAGS
    )
    # CMake deliberately omits Ninja's depfile plumbing from compile_commands.json. The pinned
    # generator adds exactly this target-bound five-token block immediately before `-o`; no
    # launcher or other actual-command drift is normalized away.
    return (
        *compile_database_tokens[:output_position],
        *dependency_tokens,
        *compile_database_tokens[output_position:],
    )


def validate_actual_compile_commands(
    actual: dict[str, tuple[str, ...]],
    expected: dict[str, tuple[str, ...]],
) -> None:
    if set(actual) != set(expected):
        raise SemanticsError(
            "Ninja compile command inventory drifted: "
            f"missing={sorted(set(expected) - set(actual))} "
            f"extra={sorted(set(actual) - set(expected))}"
        )
    for output, expected_tokens in expected.items():
        actual_tokens = actual[output]
        if not actual_tokens or not expected_tokens or actual_tokens[0] != expected_tokens[0]:
            actual_first = actual_tokens[0] if actual_tokens else "<empty>"
            expected_first = expected_tokens[0] if expected_tokens else "<empty>"
            raise SemanticsError(
                f"actual Ninja command for {output} has a launcher before the pinned compiler: "
                f"actual={actual_first!r} expected={expected_first!r}"
            )
        expected_actual = expected_ninja_command(output, expected_tokens)
        if actual_tokens == expected_actual:
            continue
        mismatch = next(
            (
                index
                for index, (actual_token, expected_token) in enumerate(
                    zip(actual_tokens, expected_actual, strict=False)
                )
                if actual_token != expected_token
            ),
            min(len(actual_tokens), len(expected_actual)),
        )
        actual_token = actual_tokens[mismatch] if mismatch < len(actual_tokens) else "<missing>"
        expected_token = (
            expected_actual[mismatch] if mismatch < len(expected_actual) else "<missing>"
        )
        raise SemanticsError(
            f"actual Ninja command for {output} differs from compile_commands.json plus the "
            "exact Ninja depfile block: "
            f"token[{mismatch}] actual={actual_token!r} expected={expected_token!r}"
        )


def raw_sdkconfig_values(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("CONFIG_") or "=" not in line:
            continue
        name, value = line.split("=", 1)
        values[name] = "1" if value == "y" else value
    return values


def strip_c_comments(text: str) -> str:
    output: list[str] = []
    position = 0
    state = "code"
    quote = ""
    while position < len(text):
        char = text[position]
        following = text[position + 1] if position + 1 < len(text) else ""
        if state == "line-comment":
            if char == "\n":
                output.append(char)
                state = "code"
            else:
                output.append(" ")
        elif state == "block-comment":
            if char == "*" and following == "/":
                output.extend((" ", " "))
                position += 1
                state = "code"
            else:
                output.append("\n" if char == "\n" else " ")
        elif state == "quoted":
            output.append(char)
            if char == "\\" and following:
                output.append(following)
                position += 1
            elif char == quote:
                state = "code"
        elif char == "/" and following == "/":
            output.extend((" ", " "))
            position += 1
            state = "line-comment"
        elif char == "/" and following == "*":
            output.extend((" ", " "))
            position += 1
            state = "block-comment"
        else:
            output.append(char)
            if char in {'"', "'"}:
                quote = char
                state = "quoted"
        position += 1
    return "".join(output)


def validate_sdkconfig_header(path: Path, sdkconfig: Path, target: str) -> None:
    """Keep the one mutable generated config header declarative and tied to sdkconfig."""
    data = canonical_regular_file(path, "generated sdkconfig header").read_text(encoding="utf-8")
    if re.search(r"\\\r?$", data, re.MULTILINE):
        raise SemanticsError("generated sdkconfig header contains a continued directive")
    without_comments = strip_c_comments(data)
    definitions: dict[str, str] = {}
    pragma_once = 0
    value_pattern = re.compile(
        r'(?:-?(?:0[xX][0-9A-Fa-f]+|\d+)|"(?:\\.|[^"\\])*"|CONFIG_[A-Z0-9_]+)'
    )
    for number, raw_line in enumerate(without_comments.splitlines(), 1):
        line = raw_line.strip()
        if not line:
            continue
        if line == "#pragma once":
            pragma_once += 1
            continue
        match = re.fullmatch(r"#define (CONFIG_[A-Z0-9_]+) (.+)", line)
        if not match or not value_pattern.fullmatch(match.group(2)):
            raise SemanticsError(
                f"generated sdkconfig header has executable/unsupported line {number}: {line!r}"
            )
        name, value = match.groups()
        if name in definitions:
            raise SemanticsError(f"generated sdkconfig header repeats {name}")
        definitions[name] = value
    if pragma_once != 1:
        raise SemanticsError("generated sdkconfig header must contain exactly one #pragma once")

    def resolved_value(name: str) -> str | None:
        seen: set[str] = set()
        value = definitions.get(name)
        while value is not None and value.startswith("CONFIG_"):
            if value in seen:
                raise SemanticsError(f"generated sdkconfig header has an alias cycle at {name}")
            seen.add(value)
            value = definitions.get(value)
        return value

    def same_scalar(actual: str | None, expected: str) -> bool:
        if actual is None:
            return False
        integer = re.compile(r"-?(?:0[xX][0-9A-Fa-f]+|\d+)")
        if integer.fullmatch(actual) and integer.fullmatch(expected):
            return int(actual, 0) == int(expected, 0)
        return actual == expected

    expected = raw_sdkconfig_values(sdkconfig)
    mismatches = sorted(
        name for name, value in expected.items() if not same_scalar(resolved_value(name), value)
    )
    if mismatches:
        raise SemanticsError(
            f"generated sdkconfig header disagrees with effective sdkconfig: {mismatches[:8]}"
        )
    if resolved_value("CONFIG_IDF_TARGET") != f'"{target}"':
        raise SemanticsError("generated sdkconfig header target string drifted")


def validate_firmware_dependencies(
    records: dict[str, tuple[Path, ...]],
    outputs: dict[str, Path],
    source_root: Path,
    build_root: Path,
    idf_root: Path | None,
    sdkconfig: Path,
    target: str,
) -> None:
    source_root = source_root.resolve(strict=False)
    main_root = (source_root / "main").resolve(strict=False)
    managed_root = (source_root / "managed_components").resolve(strict=False)
    build_root = build_root.resolve(strict=False)
    tool_root = Path("/opt/esp/tools").resolve(strict=False)
    reviewed_suffixes = reviewed_runtime_code_suffixes(source_root)
    seen_generated: set[str] = set()
    for output, source in outputs.items():
        canonical_source = canonical_regular_file(source, f"source of {output}")
        source_in_main = relative_to(canonical_source, main_root) is not None
        source_in_managed = relative_to(canonical_source, managed_root) is not None
        dependencies = records.get(output)
        if dependencies is None:
            raise SemanticsError(f"missing dependency record for firmware object: {output}")
        if canonical_source not in dependencies:
            raise SemanticsError(f"firmware object dependency record omits its source: {output}")
        for dependency in dependencies:
            dependency = canonical_regular_file(dependency, f"dependency of {output}")
            if dependency == canonical_source:
                continue
            build_relative = relative_to(dependency, build_root)
            if build_relative is not None:
                relative_name = build_relative.as_posix()
                expected_digest = ALLOWED_GENERATED_MAIN_HEADERS.get(relative_name, "missing")
                if expected_digest == "missing":
                    raise SemanticsError(
                        f"firmware object uses unreviewed build-generated header: {relative_name}"
                    )
                if expected_digest is not None:
                    actual = hashlib.sha256(dependency.read_bytes()).hexdigest()
                    if actual != expected_digest:
                        raise SemanticsError(
                            f"pinned generated header digest drifted: {relative_name}"
                        )
                seen_generated.add(relative_name)
                continue
            local_relative = relative_to(dependency, source_root)
            if local_relative is not None:
                if relative_to(dependency, main_root) is not None:
                    if source_in_main and dependency.suffix.lower() in reviewed_suffixes:
                        continue
                    raise SemanticsError(
                        "firmware object uses a local main dependency outside the runtime boundary "
                        f"inventory: {local_relative.as_posix()}"
                    )
                managed_relative = relative_to(dependency, managed_root)
                if (
                    managed_relative is not None
                    and managed_relative.parts
                    and managed_relative.parts[0] in MANAGED_COMPONENTS
                    and (source_in_main or source_in_managed)
                ):
                    continue
                raise SemanticsError(
                    f"firmware object uses unreviewed repository header: {local_relative.as_posix()}"
                )
            if idf_root is not None and relative_to(dependency, idf_root / "components") is not None:
                continue
            if relative_to(dependency, tool_root) is not None:
                continue
            raise SemanticsError(f"firmware object uses untrusted external header: {dependency}")
    if "config/sdkconfig.h" not in seen_generated:
        raise SemanticsError("main dependency graph omits generated config/sdkconfig.h")
    validate_sdkconfig_header(build_root / "config/sdkconfig.h", sdkconfig, target)


def parse_sdkconfig(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("CONFIG_") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key] = value.strip().strip('"')
    return values


def command_tokens(entry: dict[str, Any]) -> list[str]:
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(isinstance(item, str) for item in arguments):
        return list(arguments)
    command = entry.get("command")
    if isinstance(command, str):
        try:
            return shlex.split(command)
        except ValueError as exc:
            raise SemanticsError(f"invalid compile command quoting: {exc}") from exc
    raise SemanticsError("compile_commands entry has neither string arguments nor command")


def expand_response_files(tokens: list[str], directory: Path, depth: int = 0) -> list[str]:
    if depth > 4:
        raise SemanticsError("compile command response-file nesting is too deep")
    expanded: list[str] = []
    for token in tokens:
        if not token.startswith("@"):
            expanded.append(token)
            continue
        response = Path(token[1:])
        if not response.is_absolute():
            response = directory / response
        if response.is_symlink() or not response.is_file():
            raise SemanticsError(f"compile response file is missing or symlinked: {response}")
        try:
            text = response.read_text(encoding="utf-8")
            nested = shlex.split(text)
        except (OSError, UnicodeError, ValueError) as exc:
            raise SemanticsError(f"cannot parse compile response file {response}: {exc}") from exc
        expanded.extend(expand_response_files(nested, response.parent, depth + 1))
    return expanded


def resolved_source(file_name: str, directory: Path) -> Path:
    path = Path(file_name)
    if not path.is_absolute():
        path = directory / path
    return path.resolve(strict=False)


def is_under_main(path: Path, source_root: Path) -> bool:
    try:
        path.relative_to((source_root / "main").resolve(strict=False))
    except ValueError:
        return False
    return True


def is_main_component_output(output: str) -> bool:
    normalized = "/" + output.replace("\\", "/").lstrip("/")
    return "/esp-idf/main/CMakeFiles/__idf_main.dir/" in normalized


def relative_to(path: Path, root: Path) -> Path | None:
    try:
        return path.relative_to(root.resolve(strict=False))
    except ValueError:
        return None


def literal_main_sources(source_root: Path) -> set[Path]:
    cmake_path = source_root / "main" / "CMakeLists.txt"
    cmake = cmake_path.read_text(encoding="utf-8")
    cmake_code = re.sub(r"#[^\n]*", "", cmake)
    blocks = re.findall(
        r"idf_component_register\s*\(\s*SRCS\s*(.*?)\n\s*"
        r"(?:INCLUDE_DIRS|PRIV_INCLUDE_DIRS|REQUIRES|PRIV_REQUIRES|EMBED_FILES)\b",
        cmake_code,
        re.IGNORECASE | re.DOTALL,
    )
    if len(blocks) != 1:
        raise SemanticsError("main/CMakeLists.txt must expose exactly one literal SRCS block")
    sources: set[Path] = set()
    for raw_line in blocks[0].splitlines():
        line = raw_line.strip()
        if not line:
            continue
        match = re.fullmatch(r'"([^"\n]+\.(?:c|cc|cpp|cxx))"', line, re.IGNORECASE)
        if not match:
            raise SemanticsError(f"unsupported/non-literal main source entry: {line!r}")
        relative = Path(match.group(1))
        if relative.is_absolute() or ".." in relative.parts:
            raise SemanticsError(f"main source escapes the literal inventory: {relative}")
        source = (source_root / "main" / relative).resolve(strict=False)
        if source in sources:
            raise SemanticsError(f"duplicate literal main source: {relative}")
        if source.is_symlink() or not source.is_file():
            raise SemanticsError(f"literal main source is missing or symlinked: {relative}")
        sources.add(source)
    if not sources:
        raise SemanticsError("literal main source inventory is empty")
    return sources


def reviewed_runtime_code_suffixes(source_root: Path) -> frozenset[str]:
    """Read the runtime scanner's literal local-code suffix contract without importing its tests."""
    contract_path = canonical_regular_file(
        source_root / "test" / "test_runtime_boundary_contract.py",
        "runtime boundary inventory contract",
    )
    try:
        tree = ast.parse(contract_path.read_text(encoding="utf-8"), filename=str(contract_path))
    except (OSError, UnicodeError, SyntaxError) as exc:
        raise SemanticsError(f"cannot parse runtime boundary inventory contract: {exc}") from exc
    assignments = [
        node.value
        for node in tree.body
        if isinstance(node, ast.Assign)
        and any(isinstance(target, ast.Name) and target.id == "REVIEWED_LOCAL_CODE_SUFFIXES"
                for target in node.targets)
    ]
    if len(assignments) != 1:
        raise SemanticsError(
            "runtime boundary inventory must define one literal REVIEWED_LOCAL_CODE_SUFFIXES tuple"
        )
    try:
        value = ast.literal_eval(assignments[0])
    except (ValueError, TypeError) as exc:
        raise SemanticsError("runtime boundary code suffixes must be a literal tuple") from exc
    if (
        not isinstance(value, tuple)
        or not value
        or any(not isinstance(suffix, str) or not re.fullmatch(r"\.[a-z0-9]+", suffix)
               for suffix in value)
        or len(set(value)) != len(value)
    ):
        raise SemanticsError(
            "runtime boundary code suffixes must be unique lowercase literal extensions"
        )
    return frozenset(value)


def validate_firmware_compile_boundary(
    file_name: str,
    tokens: list[str],
    target: str,
    source_suffix: str,
) -> None:
    compiler_raw = Path(tokens[0]) if tokens else Path()
    compiler = compiler_raw.resolve(strict=False)
    expected = (
        EXPECTED_CXX_COMPILER[target]
        if source_suffix.lower() in {".cc", ".cpp", ".cxx"}
        else EXPECTED_C_COMPILER[target]
    )
    if (
        not compiler_raw.is_absolute()
        or relative_to(compiler, Path("/opt/esp/tools")) is None
        or compiler.name != expected
    ):
        raise SemanticsError(
            f"{file_name} compile command must invoke the pinned target compiler directly"
        )
    for token in tokens:
        if token.startswith(FORBIDDEN_FIRMWARE_FLAG_PREFIXES):
            raise SemanticsError(
                f"{file_name} uses forbidden source-injection compiler flag: {token}"
            )


def validate_compile_include_paths(
    file_name: str,
    tokens: list[str],
    directory: Path,
    source_root: Path,
    build_root: Path,
    idf_root: Path | None,
    target: str,
) -> None:
    # A repository-local include search path must be exactly main/, the current build tree, or one
    # allowlisted dependency-manager component subtree.  Accepting a main/ subdirectory would let
    # an angled include resolve a fragment that the lexical include resolver cannot see.
    include_paths: list[str] = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        value: str | None = None
        if token in {"-I", "-isystem", "-iquote", "-idirafter"}:
            if index + 1 >= len(tokens):
                raise SemanticsError(f"{file_name} has an include flag without a path: {token}")
            value = tokens[index + 1]
            index += 2
        else:
            for prefix in ("-isystem", "-iquote", "-idirafter", "-I"):
                if token.startswith(prefix) and token != prefix:
                    value = token[len(prefix):]
                    break
            index += 1
        if value is not None:
            include_paths.append(value)

    source_root_resolved = source_root.resolve(strict=False)
    main_root = (source_root / "main").resolve(strict=False)
    managed_root = (source_root / "managed_components").resolve(strict=False)
    for include_path in include_paths:
        resolved = Path(include_path)
        if not resolved.is_absolute():
            resolved = directory / resolved
        resolved = resolved.resolve(strict=False)
        if relative_to(resolved, source_root_resolved) is not None:
            if resolved == main_root or relative_to(resolved, build_root) is not None:
                continue
            managed_relative = relative_to(resolved, managed_root)
            if (
                managed_relative is not None
                and managed_relative.parts
                and managed_relative.parts[0] in MANAGED_COMPONENTS
            ):
                continue
            raise SemanticsError(
                f"{file_name} has repository-local include path outside reviewed roots: {include_path}"
            )
        if relative_to(resolved, build_root) is not None:
            continue
        if idf_root is not None and relative_to(resolved, idf_root / "components") is not None:
            continue
        raise SemanticsError(
            f"{file_name} has untrusted external include path: {include_path}"
        )


def validate_local_non_main_source(
    file_name: str,
    source: Path,
    output: str | None,
    relative: Path | None,
    target: str,
    build_root: Path,
) -> None:
    if relative is not None and relative.parts and relative.parts[0] == "managed_components":
        if len(relative.parts) < 3 or relative.parts[1] not in MANAGED_COMPONENTS:
            raise SemanticsError(f"unreviewed managed component source: {file_name}")
        if output is None or f"/esp-idf/{relative.parts[1]}/CMakeFiles/__idf_{relative.parts[1]}.dir/" not in (
            "/" + output.replace("\\", "/").lstrip("/")
        ):
            raise SemanticsError(f"managed component source has unexpected output owner: {file_name}")
        return

    build_relative = relative_to(source, build_root)
    if build_relative is not None:
        normalized = build_relative.as_posix()
        expected_project_elf = f"project_elf_src_{target}.c"
        if normalized == expected_project_elf:
            if output != f"CMakeFiles/tesla-key-esp32.elf.dir/{expected_project_elf}.obj":
                raise SemanticsError(f"generated project ELF source has unexpected output: {file_name}")
            return
        if normalized in NANOPB_GENERATED_SOURCES:
            normalized_output = "/" + (output or "").replace("\\", "/").lstrip("/")
            if "/esp-idf/yoziru__tesla-ble/CMakeFiles/__idf_yoziru__tesla-ble.dir/" not in normalized_output:
                raise SemanticsError(f"generated nanopb source has unexpected output owner: {file_name}")
            return
        if normalized == "x509_crt_bundle.S":
            normalized_output = "/" + (output or "").replace("\\", "/").lstrip("/")
            if "/esp-idf/mbedtls/CMakeFiles/__idf_mbedtls.dir/" not in normalized_output:
                raise SemanticsError(f"generated certificate bundle has unexpected output owner: {file_name}")
            return
        if normalized in {"index.html.gz.S", "setup.html.gz.S"}:
            if not isinstance(output, str) or not is_main_component_output(output):
                raise SemanticsError(f"embedded web asset has unexpected output owner: {file_name}")
            return
        raise SemanticsError(f"unreviewed build-generated firmware source: {file_name}")

    if relative is not None:
        raise SemanticsError(
            f"repository-local firmware source is outside main/ and reviewed dependencies: {file_name}"
        )
    raise SemanticsError(f"untrusted external firmware source: {file_name}")


def pinned_tool(path_text: str, root: Path, expected_name: str, label: str) -> Path:
    path = Path(path_text)
    try:
        path.relative_to(root)
        inside = True
    except ValueError:
        inside = False
    # ESP-IDF's pinned Python executable is itself a symlink into the digest-pinned container
    # environment.  Bind the command's lexical absolute path; resolving that trusted symlink would
    # misleadingly escape to /usr/local even though no build-controlled path is involved.
    if (
        not path.is_absolute() or ".." in path.parts or not inside
        or path.name != expected_name
    ):
        raise SemanticsError(f"{label} is outside the pinned toolchain: {path_text}")
    return path


def validate_embedded_binary_assembly(source: Path, payload: Path, symbol: str) -> None:
    source = canonical_regular_file(source, f"generated embedded source {symbol}")
    payload = canonical_regular_file(payload, f"embedded payload {symbol}")
    lines = source.read_text(encoding="utf-8").splitlines()
    byte_lines = [line for line in lines if line.startswith(".byte ")]
    if not byte_lines:
        raise SemanticsError(f"generated embedded source has no bytes: {source}")
    decoded = bytearray()
    for line in byte_lines:
        if not re.fullmatch(r"\.byte 0x[0-9a-f]{2}(?:, 0x[0-9a-f]{2}){0,15}", line):
            raise SemanticsError(f"generated embedded source has malformed byte row: {source}")
        decoded.extend(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", line))
    if bytes(decoded) != payload.read_bytes():
        raise SemanticsError(f"generated embedded source bytes differ from payload: {source}")

    non_byte_lines = [line for line in lines if not line.startswith(".byte ")]
    expected_non_byte_lines = [
        f"/* * Data converted from {payload}",
        " */",
        ".data",
        "#if !defined (__APPLE__) && !defined (__linux__)",
        ".section .rodata.embedded",
        "#endif",
        "",
        f".global {symbol}",
        f"{symbol}:",
        "",
        f".global _binary_{symbol}_start",
        f"_binary_{symbol}_start: /* for objcopy compatibility */",
        "",
        f".global _binary_{symbol}_end",
        f"_binary_{symbol}_end: /* for objcopy compatibility */",
        "",
        "",
        f".global {symbol}_length",
        f"{symbol}_length:",
        f".long {payload.stat().st_size}",
        "",
        "#if defined (__linux__)",
        '.section .note.GNU-stack,"",@progbits',
        "#endif",
    ]
    if non_byte_lines != expected_non_byte_lines:
        raise SemanticsError(f"generated embedded source contains executable/extra assembly: {source}")


def deterministic_gzip(payload: bytes) -> bytes:
    gzip_path = canonical_regular_file(Path("/usr/bin/gzip"), "pinned gzip")
    try:
        completed = subprocess.run(
            [str(gzip_path), "-9", "-n", "-c"],
            input=payload,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=30,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise SemanticsError(f"cannot reproduce embedded gzip payload: {exc}") from exc
    if completed.returncode != 0 or completed.stderr:
        raise SemanticsError("pinned gzip could not reproduce embedded payload")
    return completed.stdout


def validate_generated_firmware_sources(
    firmware_outputs: dict[str, Path], source_root: Path, build_root: Path, target: str
) -> None:
    generated = {
        relative.as_posix(): source
        for source in firmware_outputs.values()
        if (relative := relative_to(source, build_root)) is not None
    }
    expected = {
        f"project_elf_src_{target}.c",
        *NANOPB_GENERATED_SOURCES,
        "x509_crt_bundle.S",
        "index.html.gz.S",
        "setup.html.gz.S",
    }
    if set(generated) != expected:
        raise SemanticsError(
            "build-generated firmware source inventory drifted: "
            f"missing={sorted(expected - set(generated))} extra={sorted(set(generated) - expected)}"
        )

    project_source = canonical_regular_file(
        generated[f"project_elf_src_{target}.c"], "generated project ELF source"
    )
    if project_source.read_bytes() != b"":
        raise SemanticsError("generated project ELF source must be the pinned empty anchor")
    for relative_name, expected_digest in NANOPB_GENERATED_SOURCES.items():
        actual = hashlib.sha256(
            canonical_regular_file(generated[relative_name], relative_name).read_bytes()
        ).hexdigest()
        if actual != expected_digest:
            raise SemanticsError(f"pinned nanopb generated source drifted: {relative_name}")

    page = canonical_regular_file(source_root / "main/www/index.html", "web index source").read_text(
        encoding="utf-8"
    )
    css = canonical_regular_file(source_root / "main/www/style.css", "web style source").read_text(
        encoding="utf-8"
    )
    js = canonical_regular_file(source_root / "main/www/app.js", "web script source").read_text(
        encoding="utf-8"
    )
    style_marker = "/*@@INLINE:style.css@@*/\n"
    script_marker = "//@@INLINE:app.js@@\n"
    if page.count(style_marker) != 1 or page.count(script_marker) != 1 or any(
        marker.rstrip("\n") in asset
        for asset in (css, js)
        for marker in (style_marker, script_marker)
    ):
        raise SemanticsError("web inline marker contract drifted")
    expected_page = page.replace(style_marker, css).replace(script_marker, js).encode("utf-8")
    generated_page = canonical_regular_file(
        build_root / "esp-idf/main/index_inlined.html", "generated inlined web page"
    )
    if generated_page.read_bytes() != expected_page:
        raise SemanticsError("generated inlined web page differs from repository inputs")

    index_gzip = canonical_regular_file(
        build_root / "esp-idf/main/index.html.gz", "generated index gzip"
    )
    if index_gzip.read_bytes() != deterministic_gzip(expected_page):
        raise SemanticsError("generated index gzip differs from pinned gzip over repository inputs")
    setup_source = canonical_regular_file(
        source_root / "main/www/setup.html", "setup page source"
    ).read_bytes()
    setup_gzip = canonical_regular_file(
        build_root / "esp-idf/main/setup.html.gz", "generated setup gzip"
    )
    if setup_gzip.read_bytes() != deterministic_gzip(setup_source):
        raise SemanticsError("generated setup gzip differs from pinned gzip over repository input")

    x509_bundle = canonical_regular_file(
        build_root / "esp-idf/mbedtls/x509_crt_bundle", "generated X509 bundle"
    )
    if hashlib.sha256(x509_bundle.read_bytes()).hexdigest() != X509_BUNDLE_SHA256:
        raise SemanticsError("generated X509 bundle differs from the pinned ESP-IDF bundle")
    validate_embedded_binary_assembly(generated["index.html.gz.S"], index_gzip, "index_html_gz")
    validate_embedded_binary_assembly(generated["setup.html.gz.S"], setup_gzip, "setup_html_gz")
    validate_embedded_binary_assembly(generated["x509_crt_bundle.S"], x509_bundle, "x509_crt_bundle")


def validate_root_cmake_link_surface(source_root: Path) -> None:
    cmake = canonical_regular_file(source_root / "CMakeLists.txt", "root CMake contract").read_text(
        encoding="utf-8"
    )
    code = re.sub(r"#[^\n]*", "", cmake)
    forbidden = (
        "add_custom_command", "add_custom_target", "add_executable", "add_library",
        "target_link_options", "target_link_libraries", "link_directories", "target_sources",
        "set_property", "set_target_properties", "cmake_language", "configure_file",
    )
    for command in forbidden:
        if re.search(rf"\b{re.escape(command)}\s*\(", code, re.IGNORECASE):
            raise SemanticsError(f"root CMake exposes forbidden link/source seam: {command}")
    for token in ("RULE_LAUNCH_LINK", "PRE_LINK", "POST_BUILD", "LINK_FLAGS", "LINK_OPTIONS"):
        if token in code:
            raise SemanticsError(f"root CMake exposes forbidden link mutation token: {token}")
    execute_blocks = re.findall(r"\bexecute_process\s*\((.*?)\)", code, re.I | re.S)
    expected_execute = (
        'COMMAND "${CMAKE_SOURCE_DIR}/scripts/apply-tesla-ble-patches.sh"\n'
        "    RESULT_VARIABLE TESLA_BLE_PATCH_RESULT"
    )
    if len(execute_blocks) != 1 or execute_blocks[0].strip() != expected_execute:
        raise SemanticsError("root CMake execute_process surface drifted")


def validate_main_cmake_build_surface(source_root: Path) -> None:
    """Pin every command-capable seam in the repository-owned firmware component."""
    cmake = canonical_regular_file(
        source_root / "main/CMakeLists.txt", "main CMake contract"
    ).read_text(encoding="utf-8")
    code = re.sub(r"#[^\n]*", "", cmake)
    commands = tuple(
        match.group(1).lower()
        for match in re.finditer(r"(?im)^[ \t]*([A-Za-z_]\w*)\s*\(", code)
    )
    expected = (
        "idf_component_register", "target_compile_options",
        "add_custom_command", "add_custom_command", "add_custom_target", "add_dependencies",
        "add_custom_command", "add_custom_target", "add_dependencies",
    )
    if commands != expected:
        raise SemanticsError(
            f"main CMake command surface drifted: expected={expected!r} got={commands!r}"
        )
    target_blocks = re.findall(r"\badd_custom_target\s*\((.*?)\)", code, re.I | re.S)
    if len(target_blocks) != 2 or [block.split()[0] for block in target_blocks] != [
        "gen_index_gz", "gen_setup_gz"
    ]:
        raise SemanticsError("main CMake custom-target inventory drifted")
    if any(re.search(r"(?:^|\s)ALL(?:\s|$)", block) for block in target_blocks):
        raise SemanticsError("main CMake custom target may not join the default build independently")
    dependency_blocks = re.findall(r"\badd_dependencies\s*\((.*?)\)", code, re.I | re.S)
    if [block.split() for block in dependency_blocks] != [
        ["${COMPONENT_LIB}", "gen_index_gz"],
        ["${COMPONENT_LIB}", "gen_setup_gz"],
    ]:
        raise SemanticsError("main CMake custom dependency inventory drifted")


def validate_generated_producer_commands(
    build_root: Path, source_root: Path, idf_root: Path, target: str,
    command_loader: CommandLoader,
) -> None:
    project_source = f"project_elf_src_{target}.c"
    targets = (
        project_source,
        "esp-idf/main/index_inlined.html",
        "esp-idf/main/index.html.gz",
        "index.html.gz.S",
        "esp-idf/main/setup.html.gz",
        "setup.html.gz.S",
        "esp-idf/mbedtls/x509_crt_bundle",
        "x509_crt_bundle.S",
    )
    commands = command_loader(build_root, targets)
    if set(commands) != set(targets):
        raise SemanticsError("generated firmware producer command inventory drifted")

    def cmake_at(tokens: tuple[str, ...], index: int) -> str:
        pinned_tool(tokens[index], Path("/opt/esp/tools/cmake"), "cmake", "generated-source CMake")
        return tokens[index]

    def python_at(tokens: tuple[str, ...], index: int) -> str:
        pinned_tool(tokens[index], Path("/opt/esp/python_env"), "python", "generated-source Python")
        return tokens[index]

    def exact(target_name: str, expected: tuple[str, ...]) -> None:
        if commands[target_name] != expected:
            raise SemanticsError(f"generated firmware producer drifted: {target_name}")

    build = str(build_root)
    main_build = str(build_root / "esp-idf/main")
    mbedtls_build = str(build_root / "esp-idf/mbedtls")
    project_tokens = commands[project_source]
    project_cmake = cmake_at(project_tokens, 3)
    exact(project_source, (
        "cd", build, "&&", project_cmake, "-E", "touch", str(build_root / project_source)
    ))

    inline_tokens = commands["esp-idf/main/index_inlined.html"]
    inline_cmake = cmake_at(inline_tokens, 3)
    exact("esp-idf/main/index_inlined.html", (
        "cd", main_build, "&&", inline_cmake,
        f"-DHTML={source_root / 'main/www/index.html'}",
        f"-DCSS={source_root / 'main/www/style.css'}",
        f"-DJS={source_root / 'main/www/app.js'}",
        f"-DOUT={build_root / 'esp-idf/main/index_inlined.html'}",
        "-P", str(source_root / "main/www/inline_assets.cmake"),
    ))
    exact("esp-idf/main/index.html.gz", (
        "cd", main_build, "&&", "/usr/bin/gzip", "-9", "-n", "-c",
        str(build_root / "esp-idf/main/index_inlined.html"), ">",
        str(build_root / "esp-idf/main/index.html.gz"),
    ))
    exact("esp-idf/main/setup.html.gz", (
        "cd", main_build, "&&", "/usr/bin/gzip", "-9", "-n", "-c",
        str(source_root / "main/www/setup.html"), ">",
        str(build_root / "esp-idf/main/setup.html.gz"),
    ))

    for assembly, payload in (
        ("index.html.gz.S", "esp-idf/main/index.html.gz"),
        ("setup.html.gz.S", "esp-idf/main/setup.html.gz"),
        ("x509_crt_bundle.S", "esp-idf/mbedtls/x509_crt_bundle"),
    ):
        tokens = commands[assembly]
        cmake = cmake_at(tokens, 3)
        exact(assembly, (
            "cd", build, "&&", cmake, "-D", f"DATA_FILE={build_root / payload}",
            "-D", f"SOURCE_FILE={build_root / assembly}", "-D", "FILE_TYPE=BINARY",
            "-P", str(idf_root / "tools/cmake/scripts/data_file_embed_asm.cmake"),
        ))

    x509_tokens = commands["esp-idf/mbedtls/x509_crt_bundle"]
    python = python_at(x509_tokens, 3)
    generator = idf_root / "components/mbedtls/esp_crt_bundle/gen_crt_bundle.py"
    ca_root = idf_root / "components/mbedtls/esp_crt_bundle"
    exact("esp-idf/mbedtls/x509_crt_bundle", (
        "cd", mbedtls_build, "&&", python, str(generator), "--input",
        str(ca_root / "cacrt_all.pem"), str(ca_root / "cacrt_local.pem"),
        "-q", "--max-certs", "200",
    ))


def validate_archive_command(
    archive: str, tokens: tuple[str, ...], firmware_outputs: set[str], target: str
) -> set[str]:
    if tokens[:2] != (":", "&&") or tokens[-2:] != ("&&", ":"):
        raise SemanticsError(f"archive command has pre/post launcher chain: {archive}")
    body = tokens[2:-2]
    if len(body) < 13 or body[1:5] != ("-E", "rm", "-f", archive) or body[5] != "&&":
        raise SemanticsError(f"archive cleanup command drifted: {archive}")
    pinned_tool(body[0], Path("/opt/esp/tools/cmake"), "cmake", f"archive CMake {archive}")
    if body[7:9] != ("qc", archive):
        raise SemanticsError(f"archive creation command drifted: {archive}")
    pinned_tool(body[6], Path("/opt/esp/tools"), EXPECTED_AR[target], f"archiver {archive}")
    try:
        member_end = body.index("&&", 9)
    except ValueError as exc:
        raise SemanticsError(f"archive member boundary is missing: {archive}") from exc
    members = body[9:member_end]
    tail = body[member_end + 1:]
    if len(tail) != 2 or tail[1] != archive:
        raise SemanticsError(f"archive ranlib command drifted: {archive}")
    pinned_tool(tail[0], Path("/opt/esp/tools"), EXPECTED_RANLIB[target], f"ranlib {archive}")
    if not members or len(set(members)) != len(members):
        raise SemanticsError(f"archive has empty/duplicate member inventory: {archive}")
    unknown = sorted(set(members) - firmware_outputs)
    if unknown:
        raise SemanticsError(f"archive contains non-compile-database members: {archive}: {unknown}")
    return set(members)


def validate_linker_script(path: Path, build_root: Path, idf_root: Path) -> None:
    path = canonical_regular_file(path, "linker script")
    if relative_to(path, build_root) is None and relative_to(path, idf_root / "components") is None:
        raise SemanticsError(f"linker script is outside build/pinned IDF roots: {path}")
    code = strip_c_comments(path.read_text(encoding="utf-8"))
    if re.search(r"(?im)^\s*(?:INPUT|GROUP|SEARCH_DIR|INCLUDE)\b", code):
        raise SemanticsError(f"linker script contains indirect external-input directive: {path}")


def validate_wl_option(token: str, build_root: Path, target: str) -> None:
    """Allow only the literal, non-input-bearing GNU-ld options emitted by pinned IDF 5.5.5."""
    allowed = {
        "-Wl,--cref",
        "-Wl,--no-warn-rwx-segments",
        "-Wl,--orphan-handling=warn",
        "-Wl,--gc-sections",
        "-Wl,--warn-common",
        "-Wl,--wrap=longjmp",
        "-Wl,--undefined=FreeRTOS_openocd_params",
        f"-Wl,--defsym=IDF_TARGET_{target.upper()}=0",
        f"-Wl,--Map={build_root / 'tesla-key-esp32.map'}",
    }
    if token not in allowed:
        raise SemanticsError(f"final ELF link uses unreviewed/path-bearing -Wl option: {token}")


def resolve_link_library(
    library: str, search_roots: list[Path], compiler: Path,
    build_root: Path, idf_root: Path,
) -> tuple[Path, str | None]:
    """Resolve one ``-l`` exactly and return an auditable build archive when applicable."""
    exact = library.startswith(":")
    name = library[1:] if exact else library
    if not name or "/" in name or "\\" in name or not re.fullmatch(r"[A-Za-z0-9_.+\-]+", name):
        raise SemanticsError(f"final ELF link has malformed library selector: -l{library}")
    filename = name if exact else f"lib{name}.a"

    selected: Path | None = None
    for root in search_roots:
        if not exact:
            shared = root / f"lib{name}.so"
            if shared.exists():
                raise SemanticsError(f"final ELF link could select a shared library: {shared}")
        candidate = root / filename
        if candidate.exists():
            selected = candidate
            break

    if selected is None:
        # GCC's own search roots contain libc/libm/libstdc++/libgcc. Ask the exact pinned driver,
        # after the caller boundary has rejected LIBRARY_PATH/COMPILER_PATH/GCC_EXEC_PREFIX.
        try:
            completed = subprocess.run(
                [str(compiler), f"-print-file-name={filename}"], check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=30,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise SemanticsError(f"cannot resolve link library -l{library}: {exc}") from exc
        result = completed.stdout.strip()
        if completed.returncode != 0 or completed.stderr.strip() or not result or result == filename:
            raise SemanticsError(f"pinned compiler cannot resolve link library: -l{library}")
        selected = Path(result)
        if not exact:
            try:
                shared_query = subprocess.run(
                    [str(compiler), f"-print-file-name=lib{name}.so"], check=False,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=30,
                )
            except (OSError, subprocess.SubprocessError) as exc:
                raise SemanticsError(f"cannot exclude shared link library -l{library}: {exc}") from exc
            shared_result = shared_query.stdout.strip()
            if (
                shared_query.returncode != 0 or shared_query.stderr.strip()
                or (shared_result != f"lib{name}.so" and Path(shared_result).is_file())
            ):
                raise SemanticsError(f"pinned compiler could select shared library for -l{library}")

    canonical = canonical_regular_file(selected, f"resolved link library -l{library}")
    build_relative = relative_to(canonical, build_root)
    if build_relative is not None:
        if canonical.suffix != ".a":
            raise SemanticsError(f"build-root link library is not a static archive: {canonical}")
        return canonical, build_relative.as_posix()
    if (
        relative_to(canonical, idf_root / "components") is None
        and relative_to(canonical, Path("/opt/esp/tools")) is None
    ):
        raise SemanticsError(f"resolved link library is outside pinned roots: {canonical}")
    if canonical.suffix != ".a":
        raise SemanticsError(f"resolved link library is not a static archive: {canonical}")
    return canonical, None


def validate_link_and_archives(
    build_root: Path, idf_root: Path, target: str, firmware_outputs: dict[str, Path],
    command_loader: CommandLoader,
) -> None:
    elf = "tesla-key-esp32.elf"
    raw = command_loader(build_root, (elf,))[elf]
    if raw[:2] != (":", "&&") or raw[-2:] != ("&&", ":"):
        raise SemanticsError("final ELF link has a launcher, PRE_LINK or POST_BUILD command")
    body_raw = raw[2:-2]
    if any(token in {"&&", "||", ";", "|", ">", "<"} for token in body_raw):
        raise SemanticsError("final ELF link contains an additional command chain")
    expected_responses = {
        str(build_root / "toolchain/cxxflags"), str(build_root / "toolchain/ldflags")
    }
    response_paths = {
        str((build_root / token[1:]).resolve(strict=False)) if not Path(token[1:]).is_absolute()
        else str(Path(token[1:]).resolve(strict=False))
        for token in body_raw if token.startswith("@")
    }
    if response_paths != expected_responses:
        raise SemanticsError("final ELF link response-file inventory drifted")
    body = expand_response_files(list(body_raw), build_root)
    if not body:
        raise SemanticsError("final ELF link command is empty")
    pinned_tool(body[0], Path("/opt/esp/tools"), EXPECTED_CXX_COMPILER[target], "ELF linker driver")
    if any(token.startswith("@") for token in body):
        raise SemanticsError("final ELF link contains an unexpanded response file")
    forbidden = ("-fplugin", "-specs", "--specs", "-wrapper", "-B", "-Xlinker", "-R")
    if any(token.startswith(forbidden) for token in body):
        raise SemanticsError("final ELF link contains a forbidden injection option")
    output_positions = [index for index, token in enumerate(body) if token == "-o"]
    if len(output_positions) != 1 or output_positions[0] + 1 >= len(body) or \
            body[output_positions[0] + 1] != elf:
        raise SemanticsError("final ELF link output token drifted")

    search_roots: list[Path] = [build_root]
    script_names: list[str] = []
    library_names: list[str] = []
    consumed: set[int] = {0, output_positions[0], output_positions[0] + 1}
    index = 1
    while index < len(body):
        token = body[index]
        if token.startswith("-Wl,"):
            validate_wl_option(token, build_root, target)
            consumed.add(index)
            index += 1
            continue
        if token == "-L":
            if index + 1 >= len(body): raise SemanticsError("final link has empty -L")
            value = body[index + 1]
            consumed.update((index, index + 1))
            index += 2
        elif token.startswith("-L") and token != "-L":
            value = token[2:]
            consumed.add(index)
            index += 1
        else:
            value = None
        if value is not None:
            root = Path(value)
            if not root.is_absolute(): root = build_root / root
            root = root.resolve(strict=False)
            if relative_to(root, build_root) is None and relative_to(root, idf_root / "components") is None:
                raise SemanticsError(f"final ELF link uses external library search root: {value}")
            search_roots.append(root)
            continue
        if token == "-T":
            if index + 1 >= len(body): raise SemanticsError("final link has empty -T")
            script_names.append(body[index + 1])
            consumed.update((index, index + 1))
            index += 2
            continue
        if token.startswith("-T") and token != "-T":
            script_names.append(token[2:])
            consumed.add(index)
            index += 1
            continue
        if token == "-l":
            if index + 1 >= len(body): raise SemanticsError("final link has empty -l")
            library_names.append(body[index + 1])
            consumed.update((index, index + 1))
            index += 2
            continue
        if token.startswith("-l") and token != "-l":
            library_names.append(token[2:])
            consumed.add(index)
            index += 1
            continue
        if token == "-u":
            if index + 1 >= len(body): raise SemanticsError("final link has empty -u")
            consumed.update((index, index + 1))
            index += 2
            continue
        if token.startswith("-"):
            if token.startswith("--"):
                raise SemanticsError(f"final ELF link uses unreviewed direct linker option: {token}")
            consumed.add(index)  # ordinary compiler-driver flag, with no separate path argument
        index += 1

    archives: set[str] = set()
    direct_objects: set[str] = set()
    for token_index, token in enumerate(body):
        if token_index in consumed:
            continue
        if token.endswith(".a"):
            if token.startswith("-"):
                raise SemanticsError(f"combined archive linker option is forbidden: {token}")
            path = Path(token)
            if path.is_absolute():
                canonical = canonical_regular_file(path, "pinned IDF prebuilt archive")
                build_relative = relative_to(canonical, build_root)
                if build_relative is not None:
                    archives.add(build_relative.as_posix())
                elif relative_to(canonical, idf_root / "components") is None:
                    raise SemanticsError(f"final ELF link uses external prebuilt archive: {token}")
            else:
                canonical_regular_file(build_root / path, "build component archive")
                archives.add(token)
        elif token.endswith((".obj", ".o")):
            if token not in firmware_outputs:
                raise SemanticsError(f"final ELF link uses non-compile-database object: {token}")
            direct_objects.add(token)
        elif token.endswith((".ld", ".lds")):
            raise SemanticsError(f"final ELF link uses a bare linker script input: {token}")
        elif token.endswith((".so", ".dylib")):
            raise SemanticsError(f"final ELF link uses a dynamic library input: {token}")
        else:
            raise SemanticsError(f"final ELF link has an unclassified bare input token: {token}")

    compiler = Path(body[0]).resolve(strict=False)
    for library in library_names:
        _, build_archive = resolve_link_library(
            library, search_roots, compiler, build_root, idf_root
        )
        if build_archive is not None:
            archives.add(build_archive)

    archive_commands = command_loader(build_root, tuple(sorted(archives)))
    if set(archive_commands) != archives:
        raise SemanticsError("archive producer command inventory drifted")
    archived_objects: set[str] = set()
    archive_owner: dict[str, str] = {}
    for archive in sorted(archives):
        members = validate_archive_command(
            archive, archive_commands[archive], set(firmware_outputs), target
        )
        for member in members:
            previous = archive_owner.get(member)
            if previous is not None and previous != archive:
                raise SemanticsError(
                    f"firmware object occurs in multiple archives: {member}: {previous}, {archive}"
                )
            archive_owner[member] = archive
        archived_objects.update(members)
    if archived_objects & direct_objects:
        raise SemanticsError("firmware object is linked both directly and through an archive")
    observed_objects = archived_objects | direct_objects
    if observed_objects != set(firmware_outputs):
        raise SemanticsError(
            "final ELF object provenance inventory drifted: "
            f"missing={sorted(set(firmware_outputs) - observed_objects)} "
            f"extra={sorted(observed_objects - set(firmware_outputs))}"
        )

    for script_name in script_names:
        script = Path(script_name)
        if script.suffix.lower() not in {".ld", ".lds"}:
            raise SemanticsError(f"final ELF -T input is not a linker script: {script_name}")
        candidates = [script] if script.is_absolute() else [root / script for root in search_roots]
        existing = next((candidate.resolve(strict=False) for candidate in candidates
                         if candidate.is_file()), None)
        if existing is None:
            raise SemanticsError(f"final ELF linker script cannot be resolved: {script_name}")
        validate_linker_script(existing, build_root, idf_root)


def validate_app_binary_producer(
    build_root: Path, idf_root: Path, sdkconfig: Path, target: str,
    command_loader: CommandLoader,
    *,
    process_runner: Callable[..., subprocess.CompletedProcess[bytes]] = subprocess.run,
) -> None:
    tokens = command_loader(build_root, (".bin_timestamp",))[".bin_timestamp"]
    if len(tokens) < 20 or tokens[:3] != ("cd", str(build_root), "&&"):
        raise SemanticsError("ELF-to-app producer has a launcher or wrong working directory")
    python = tokens[3]
    pinned_tool(python, Path("/opt/esp/python_env"), "python", "elf2image Python")
    try:
        first_chain = tokens.index("&&", 4)
        second_chain = tokens.index("&&", first_chain + 1)
    except ValueError as exc:
        raise SemanticsError("ELF-to-app producer command chain drifted") from exc
    if tokens.count("&&") != 3:
        raise SemanticsError("ELF-to-app producer has an extra command chain")
    elf2image = list(tokens[3:first_chain])
    config = parse_sdkconfig(sdkconfig)
    expected_flash_frequency = {
        "esp32": "40m",
        "esp32s3": "80m",
        "esp32c3": "80m",
        "esp32c6": "80m",
    }[target]
    expected_flash_config = {
        "CONFIG_ESPTOOLPY_FLASHMODE": "dio",
        "CONFIG_ESPTOOLPY_FLASHFREQ": expected_flash_frequency,
        "CONFIG_ESPTOOLPY_FLASHSIZE": "4MB",
    }
    observed_flash_config = {
        key: config.get(key) for key in expected_flash_config
    }
    if observed_flash_config != expected_flash_config:
        raise SemanticsError(
            "target-specific app flash geometry drifted: "
            f"expected={expected_flash_config!r} observed={observed_flash_config!r}"
        )
    try:
        min_revision_full = int(config["CONFIG_ESP_REV_MIN_FULL"])
        max_revision_full = int(config["CONFIG_ESP_REV_MAX_FULL"])
    except (KeyError, ValueError) as exc:
        raise SemanticsError("target revision bounds are missing or malformed") from exc
    if min_revision_full < 0 or max_revision_full < min_revision_full:
        raise SemanticsError("target revision bounds are invalid")

    # ESP-IDF 5.5.5 retains the legacy image-header --min-rev only for ESP32 (major revision)
    # and ESP32-C3 (minor revision), deriving both from the canonical full revision. Other
    # supported targets carry only the full min/max pair. Mirror that exact pinned producer.
    legacy_min_revision = 0
    if target == "esp32":
        legacy_min_revision = min_revision_full // 100
    elif target == "esp32c3":
        legacy_min_revision = min_revision_full % 100
    revision_args: list[str] = []
    if legacy_min_revision:
        revision_args.extend(("--min-rev", str(legacy_min_revision)))
    revision_args.extend((
        "--min-rev-full", str(min_revision_full),
        "--max-rev-full", str(max_revision_full),
    ))
    expected_elf2image = [
        python, str(idf_root / "components/esptool_py/esptool/esptool.py"),
        "--chip", target, "elf2image", "--flash_mode", "dio",
        "--flash_freq", expected_flash_frequency,
        "--flash_size", "4MB", "--elf-sha256-offset", "0xb0", "--secure-pad-v2",
        *revision_args, "-o", str(build_root / "tesla-key-esp32.bin"),
        str(build_root / "tesla-key-esp32.elf"),
    ]
    if elf2image != expected_elf2image:
        raise SemanticsError("ELF-to-app elf2image command/arguments drifted")

    cmake_echo = tokens[first_chain + 1:second_chain]
    if len(cmake_echo) != 4:
        raise SemanticsError("ELF-to-app diagnostic command drifted")
    cmake = cmake_echo[0]
    pinned_tool(cmake, Path("/opt/esp/tools/cmake"), "cmake", "elf2image CMake")
    if cmake_echo[1:] != ("-E", "echo", f"Generated {build_root / 'tesla-key-esp32.bin'}"):
        raise SemanticsError("ELF-to-app diagnostic command drifted")
    final = tokens[second_chain + 1:]
    if final != (
        cmake, "-E", "md5sum", str(build_root / "tesla-key-esp32.bin"), ">",
        str(build_root / ".bin_timestamp"),
    ):
        raise SemanticsError("ELF-to-app timestamp producer drifted")

    # Re-run the exact pinned elf2image invocation into an isolated target and compare bytes. This
    # binds the checked app to the checked ELF even if a later deterministic command had replaced
    # tesla-key-esp32.bin and updated its timestamp.
    app = canonical_regular_file(build_root / "tesla-key-esp32.bin", "built unsigned app")
    elf = canonical_regular_file(build_root / "tesla-key-esp32.elf", "linked firmware ELF")
    if expected_elf2image[-1] != str(elf):
        raise SemanticsError("ELF-to-app input does not resolve to the audited ELF")
    with tempfile.TemporaryDirectory(prefix=".elf2image-check-", dir=build_root) as directory:
        reproduced = Path(directory) / "tesla-key-esp32.bin"
        reproduce_command = list(expected_elf2image)
        output_index = reproduce_command.index("-o") + 1
        reproduce_command[output_index] = str(reproduced)
        try:
            completed = process_runner(
                reproduce_command, cwd=build_root, check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise SemanticsError(f"cannot reproduce app from audited ELF: {exc}") from exc
        if completed.returncode != 0:
            detail = completed.stderr.decode("utf-8", errors="replace").strip()
            raise SemanticsError(f"pinned elf2image reproduction failed: {detail}")
        reproduced = canonical_regular_file(reproduced, "reproduced unsigned app")
        if reproduced.read_bytes() != app.read_bytes():
            raise SemanticsError("built app bytes are not the pinned elf2image output of audited ELF")

    timestamp = canonical_regular_file(build_root / ".bin_timestamp", "app timestamp record")
    expected_timestamp = f"{hashlib.md5(app.read_bytes()).hexdigest()}  {app}\n".encode("ascii")
    if timestamp.read_bytes() != expected_timestamp:
        raise SemanticsError("app timestamp record is not bound to the audited app bytes")


def validate_default_app_command_inventory(
    commands: tuple[tuple[str, ...], ...], producer: tuple[str, ...],
    build_root: Path, idf_root: Path,
) -> None:
    """Pin every default-build command that names the final app or its timestamp."""
    app = str(build_root / "tesla-key-esp32.bin")
    timestamp = str(build_root / ".bin_timestamp")
    relevant = tuple(
        tokens for tokens in commands
        if any("tesla-key-esp32.bin" in token or ".bin_timestamp" in token for token in tokens)
    )
    python_candidates = [
        token for tokens in relevant for token in tokens
        if token.endswith("/bin/python") and token.startswith("/opt/esp/python_env/")
    ]
    cmake_candidates = [
        token for tokens in relevant for token in tokens
        if token.endswith("/bin/cmake") and token.startswith("/opt/esp/tools/cmake/")
    ]
    if not python_candidates or not cmake_candidates:
        raise SemanticsError("default app command closure lacks pinned Python/CMake")
    python = python_candidates[0]
    cmake = cmake_candidates[0]
    pinned_tool(python, Path("/opt/esp/python_env"), "python", "default app Python")
    pinned_tool(cmake, Path("/opt/esp/tools/cmake"), "cmake", "default app CMake")
    size_check = (
        "cd", str(build_root / "esp-idf/esptool_py"), "&&", python,
        str(idf_root / "components/partition_table/check_sizes.py"),
        "--offset", "0x8000", "partition", "--type", "app",
        str(build_root / "partition_table/partition-table.bin"), app,
    )
    unsigned_notice = (
        "cd", str(build_root), "&&", cmake, "-E", "echo",
        "App built but not signed. Sign app before flashing", "&&", cmake, "-E", "echo",
        "\t" + python + " "
        + str(idf_root / "components/esptool_py/esptool/espsecure.py")
        + " sign_data --keyfile KEYFILE --version 2                 " + app,
    )
    expected = Counter((producer, size_check, unsigned_notice))
    if Counter(relevant) != expected:
        raise SemanticsError(
            "default Ninja app command inventory drifted; possible post-build app overwrite"
        )


def validate_build_graph_provenance(
    build_root: Path, target: str, source_root: Path, idf_root: Path | None,
    sdkconfig: Path, firmware_outputs: dict[str, Path], command_loader: CommandLoader,
) -> None:
    if idf_root is None:
        raise SemanticsError("IDF_PATH is required for link/app provenance validation")
    validate_root_cmake_link_surface(source_root)
    validate_main_cmake_build_surface(source_root)
    validate_generated_firmware_sources(firmware_outputs, source_root, build_root, target)
    validate_generated_producer_commands(build_root, source_root, idf_root, target, command_loader)
    validate_link_and_archives(build_root, idf_root, target, firmware_outputs, command_loader)
    validate_app_binary_producer(build_root, idf_root, sdkconfig, target, command_loader)
    producer = command_loader(build_root, (".bin_timestamp",))[".bin_timestamp"]
    validate_default_app_command_inventory(
        load_ninja_default_commands(build_root), producer, build_root, idf_root
    )


def validate(
    target: str,
    sdkconfig: Path,
    compile_commands: Path,
    source_root: Path,
    *,
    dependency_loader: DependencyLoader = load_ninja_dependencies,
    command_loader: CommandLoader = load_ninja_commands,
    graph_validator: GraphValidator | None = validate_build_graph_provenance,
) -> int:
    injected = [name for name in BUILD_INJECTION_ENV if name in os.environ]
    if injected:
        raise SemanticsError(
            f"compiler/linker injection environment variables are set: {injected}"
        )
    if target not in TARGETS:
        raise SemanticsError(f"unsupported target: {target}")
    config = parse_sdkconfig(sdkconfig)
    if config.get("CONFIG_IDF_TARGET") != target:
        raise SemanticsError(
            f"effective sdkconfig target is {config.get('CONFIG_IDF_TARGET')!r}, expected {target!r}"
        )
    if config.get("CONFIG_COMPILER_OPTIMIZATION_DEBUG") != "y":
        raise SemanticsError("effective sdkconfig does not select CONFIG_COMPILER_OPTIMIZATION_DEBUG=y")
    for key in (
        "CONFIG_COMPILER_OPTIMIZATION_SIZE",
        "CONFIG_COMPILER_OPTIMIZATION_PERF",
        "CONFIG_COMPILER_OPTIMIZATION_NONE",
    ):
        if config.get(key) == "y":
            raise SemanticsError(f"effective sdkconfig unexpectedly enables {key}")

    try:
        database = json.loads(compile_commands.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SemanticsError(f"compile_commands.json is invalid JSON: {exc}") from exc
    if not isinstance(database, list):
        raise SemanticsError("compile_commands.json root must be an array")

    source_root = source_root.resolve(strict=False)
    build_root = compile_commands.parent.resolve(strict=False)
    expected_main_sources = literal_main_sources(source_root)
    observed_main_sources: dict[Path, int] = {}
    main_outputs: dict[str, Path] = {}
    firmware_outputs: dict[str, Path] = {}
    firmware_compile_commands: dict[str, tuple[str, ...]] = {}
    idf_path_text = os.environ.get("IDF_PATH")
    idf_root = Path(idf_path_text).resolve(strict=False) if idf_path_text else None
    checked = 0
    for entry in database:
        if not isinstance(entry, dict):
            raise SemanticsError("compile_commands.json contains a non-object entry")
        file_name = entry.get("file")
        directory_text = entry.get("directory", str(source_root))
        if not isinstance(directory_text, str):
            raise SemanticsError("compile_commands entry has a non-string directory")
        if not isinstance(file_name, str):
            continue
        source = resolved_source(file_name, Path(directory_text))
        if source.suffix.lower() not in COMPILE_SUFFIXES:
            continue
        local_relative = relative_to(source, source_root)
        source_under_main = is_under_main(source, source_root)
        output = entry.get("output")
        if output is not None and not isinstance(output, str):
            raise SemanticsError("compile_commands entry has a non-string output")
        main_component_output = isinstance(output, str) and is_main_component_output(output)
        is_literal_main_source = False
        if local_relative is not None:
            if source_under_main:
                if source not in expected_main_sources:
                    raise SemanticsError(
                        f"{file_name} is under main/ but absent from the literal main source inventory"
                    )
                if not main_component_output:
                    raise SemanticsError(
                        f"{file_name} is under main/ but is not compiled into __idf_main"
                    )
                source_relative = source.relative_to((source_root / "main").resolve(strict=False))
                expected_output = (
                    "esp-idf/main/CMakeFiles/__idf_main.dir/"
                    f"{source_relative.as_posix()}.obj"
                )
                if output != expected_output:
                    raise SemanticsError(
                        f"{file_name} has unexpected main object output: {output!r}"
                    )
                if output in main_outputs:
                    raise SemanticsError(f"duplicate main object output: {output}")
                main_outputs[output] = source
                is_literal_main_source = True
            else:
                validate_local_non_main_source(
                    file_name, source, output, local_relative, target, build_root
                )
        elif relative_to(source, build_root) is not None:
            validate_local_non_main_source(
                file_name, source, output, None, target, build_root
            )
        else:
            if idf_root is None or relative_to(source, idf_root / "components") is None:
                raise SemanticsError(f"compile database contains untrusted external source: {file_name}")
        if not isinstance(output, str) or not output:
            raise SemanticsError(f"firmware translation unit has no object output: {file_name}")
        if output in firmware_outputs:
            raise SemanticsError(f"duplicate firmware object output: {output}")

        raw_tokens = command_tokens(entry)
        tokens = expand_response_files(raw_tokens, Path(directory_text))
        validate_firmware_compile_boundary(file_name, tokens, target, source.suffix.lower())
        validate_compile_include_paths(
            file_name, tokens, Path(directory_text), source_root, build_root, idf_root, target
        )
        firmware_outputs[output] = source
        firmware_compile_commands[output] = tuple(raw_tokens)

        if not is_literal_main_source:
            continue
        if source.suffix.lower() not in MAIN_SOURCE_SUFFIXES:
            raise SemanticsError(f"literal main source is not C/C++: {file_name}")
        optimisation = [token for token in tokens if token.startswith("-O")]
        if optimisation != ["-Og"]:
            raise SemanticsError(
                f"{file_name} effective optimisation must be exactly -Og, got {optimisation!r}"
            )
        if tokens.count("-fstack-usage") != 1:
            raise SemanticsError(
                f"{file_name} must be compiled exactly once with -fstack-usage"
            )
        observed_main_sources[source] = observed_main_sources.get(source, 0) + 1
        if observed_main_sources[source] != 1:
            raise SemanticsError(f"literal main source appears more than once: {file_name}")
        checked += 1
    if set(observed_main_sources) != expected_main_sources:
        missing = sorted(str(path.relative_to(source_root / "main")) for path in expected_main_sources - set(observed_main_sources))
        extra = sorted(str(path) for path in set(observed_main_sources) - expected_main_sources)
        raise SemanticsError(
            f"compile database main inventory mismatch: missing={missing!r}, extra={extra!r}"
        )
    firmware_output_names = tuple(sorted(firmware_outputs))
    validate_actual_compile_commands(
        command_loader(build_root, firmware_output_names),
        {
            output: firmware_compile_commands[output]
            for output in firmware_output_names
        },
    )
    dependency_outputs = {
        output: source
        for output, source in firmware_outputs.items()
        if relative_to(source, source_root) is not None or relative_to(source, build_root) is not None
    }
    outputs = tuple(sorted(dependency_outputs))
    dependency_records = dependency_loader(build_root, outputs)
    validate_firmware_dependencies(
        dependency_records,
        dependency_outputs,
        source_root,
        build_root,
        idf_root,
        sdkconfig,
        target,
    )
    if graph_validator is not None:
        graph_validator(
            build_root, target, source_root, idf_root, sdkconfig,
            firmware_outputs, command_loader,
        )
    return checked


def self_test_build_graph_contract(repository_root: Path) -> None:
    """Exercise the real build-graph validators with positive and mutated fixtures.

    The compile-database fixture below deliberately has no ESP-IDF build tree.  Keep this
    independent graph fixture so link/archive/app provenance cannot silently become production-only
    code that ``--self-test`` never executes.
    """

    def rejected(label: str, expected: str, action: Callable[[], None]) -> None:
        try:
            action()
        except SemanticsError as exc:
            if expected not in str(exc):
                raise AssertionError(
                    f"{label} failed for the wrong reason: {exc}"
                ) from exc
        else:
            raise AssertionError(f"{label} mutation was accepted")

    with tempfile.TemporaryDirectory(prefix="build-graph-contract-") as directory:
        fixture_root = Path(directory).resolve(strict=True)

        # Pin repository-owned CMake command surfaces and prove that their link mutation seams are
        # active canaries rather than unexecuted production checks.
        cmake_source = fixture_root / "cmake-source"
        (cmake_source / "main").mkdir(parents=True)
        shutil.copy2(repository_root / "CMakeLists.txt", cmake_source / "CMakeLists.txt")
        shutil.copy2(
            repository_root / "main/CMakeLists.txt", cmake_source / "main/CMakeLists.txt"
        )
        validate_root_cmake_link_surface(cmake_source)
        validate_main_cmake_build_surface(cmake_source)
        root_cmake = cmake_source / "CMakeLists.txt"
        root_cmake_text = root_cmake.read_text(encoding="utf-8")
        for label, mutation, expected in (
            (
                "CMake RULE_LAUNCH_LINK canary",
                '\nset(RULE_LAUNCH_LINK "/tmp/unreviewed-link-launcher")\n',
                "forbidden link mutation token",
            ),
            (
                "CMake PRE_LINK canary",
                "\nadd_custom_command(TARGET app PRE_LINK COMMAND unreviewed)\n",
                "forbidden link/source seam",
            ),
            (
                "CMake POST_BUILD canary",
                "\nadd_custom_command(TARGET app POST_BUILD COMMAND unreviewed)\n",
                "forbidden link/source seam",
            ),
        ):
            root_cmake.write_text(root_cmake_text + mutation, encoding="utf-8")
            rejected(label, expected, lambda: validate_root_cmake_link_surface(cmake_source))
            root_cmake.write_text(root_cmake_text, encoding="utf-8")

        # Reproduce the exact generated binary-assembly grammar and bind every emitted byte to the
        # payload.  A syntactically valid byte overwrite must fail independently of compilation.
        payload = fixture_root / "embedded.bin"
        payload.write_bytes(bytes(range(1, 34)))
        payload = payload.resolve(strict=True)
        assembly = fixture_root / "embedded.S"
        symbol = "fixture_payload"
        rows = [
            ".byte " + ", ".join(f"0x{value:02x}" for value in payload.read_bytes()[start:start + 16])
            for start in range(0, payload.stat().st_size, 16)
        ]
        assembly_lines = [
            f"/* * Data converted from {payload}",
            " */",
            ".data",
            "#if !defined (__APPLE__) && !defined (__linux__)",
            ".section .rodata.embedded",
            "#endif",
            "",
            f".global {symbol}",
            f"{symbol}:",
            *rows,
            "",
            f".global _binary_{symbol}_start",
            f"_binary_{symbol}_start: /* for objcopy compatibility */",
            "",
            f".global _binary_{symbol}_end",
            f"_binary_{symbol}_end: /* for objcopy compatibility */",
            "",
            "",
            f".global {symbol}_length",
            f"{symbol}_length:",
            f".long {payload.stat().st_size}",
            "",
            "#if defined (__linux__)",
            '.section .note.GNU-stack,"",@progbits',
            "#endif",
        ]
        assembly.write_text("\n".join(assembly_lines) + "\n", encoding="utf-8")
        validate_embedded_binary_assembly(assembly, payload, symbol)
        assembly_text = assembly.read_text(encoding="utf-8")
        assembly.write_text(assembly_text.replace("0x01", "0xfe", 1), encoding="utf-8")
        rejected(
            "generated .S byte-overwrite canary",
            "bytes differ from payload",
            lambda: validate_embedded_binary_assembly(assembly, payload, symbol),
        )
        assembly.write_text(assembly_text, encoding="utf-8")

        # A synthetic but structurally exact Ninja link graph: two reviewed objects, one directly
        # named component archive, one build-root -l archive, the two pinned response files and a
        # build-root linker script.  No fake compiler execution is needed because -l resolves in
        # the explicit build search root.
        build_root = fixture_root / "build"
        idf_root = fixture_root / "esp-idf"
        (build_root / "toolchain").mkdir(parents=True)
        (build_root / "esp-idf/main").mkdir(parents=True)
        (build_root / "libs").mkdir()
        (idf_root / "components").mkdir(parents=True)
        main_object = "esp-idf/main/CMakeFiles/__idf_main.dir/main.cpp.obj"
        dependency_object = "esp-idf/dependency/CMakeFiles/__idf_dependency.dir/dependency.c.obj"
        for output in (main_object, dependency_object):
            output_path = build_root / output
            output_path.parent.mkdir(parents=True, exist_ok=True)
            output_path.write_bytes(b"object")
        main_archive = "esp-idf/main/libmain.a"
        library_archive = "libs/libfixture_gate.a"
        (build_root / main_archive).write_bytes(b"archive-main")
        (build_root / library_archive).write_bytes(b"archive-library")
        linker_script = build_root / "fixture.ld"
        linker_script.write_text("SECTIONS { .text : { *(.text) } }\n", encoding="utf-8")
        cxx_response = build_root / "toolchain/cxxflags"
        ld_response = build_root / "toolchain/ldflags"
        cxx_response_text = (
            f"-Wl,--gc-sections -L{build_root / 'libs'} -lfixture_gate "
            f"-T{linker_script}\n"
        )
        cxx_response.write_text(cxx_response_text, encoding="utf-8")
        ld_response.write_text("-Og\n", encoding="utf-8")
        compiler = (
            "/opt/esp/tools/riscv32-esp-elf/fixture/"
            "riscv32-esp-elf/bin/riscv32-esp-elf-g++"
        )
        cmake = "/opt/esp/tools/cmake/fixture/bin/cmake"
        archiver = (
            "/opt/esp/tools/riscv32-esp-elf/fixture/"
            "riscv32-esp-elf/bin/riscv32-esp-elf-ar"
        )
        ranlib = (
            "/opt/esp/tools/riscv32-esp-elf/fixture/"
            "riscv32-esp-elf/bin/riscv32-esp-elf-ranlib"
        )
        link_command = (
            ":", "&&", compiler, f"@{cxx_response}", f"@{ld_response}",
            "-o", "tesla-key-esp32.elf", main_archive, "&&", ":",
        )

        def archive_command(archive: str, members: tuple[str, ...]) -> tuple[str, ...]:
            return (
                ":", "&&", cmake, "-E", "rm", "-f", archive, "&&",
                archiver, "qc", archive, *members, "&&", ranlib, archive, "&&", ":",
            )

        base_commands = {
            "tesla-key-esp32.elf": link_command,
            main_archive: archive_command(main_archive, (main_object,)),
            library_archive: archive_command(library_archive, (dependency_object,)),
        }
        firmware_outputs = {
            main_object: fixture_root / "main.cpp",
            dependency_object: fixture_root / "dependency.c",
        }

        def link_loader(
            records: dict[str, tuple[str, ...]],
        ) -> CommandLoader:
            def load(
                _build_root: Path, outputs: tuple[str, ...]
            ) -> dict[str, tuple[str, ...]]:
                return {output: records[output] for output in outputs}
            return load

        def validate_link(records: dict[str, tuple[str, ...]] = base_commands) -> None:
            validate_link_and_archives(
                build_root, idf_root, "esp32c3", firmware_outputs, link_loader(records)
            )

        validate_link()  # Positive build-root -l archive fixture.

        def changed_link(command: tuple[str, ...]) -> dict[str, tuple[str, ...]]:
            return {**base_commands, "tesla-key-esp32.elf": command}

        link_body = link_command[2:-2]
        rejected(
            "link launcher canary",
            "launcher, PRE_LINK or POST_BUILD",
            lambda: validate_link(changed_link(("/tmp/launcher", *link_command))),
        )
        rejected(
            "link PRE_LINK chain canary",
            "additional command chain",
            lambda: validate_link(changed_link(
                (":", "&&", "/usr/bin/true", "&&", *link_body, "&&", ":")
            )),
        )
        rejected(
            "link POST_BUILD chain canary",
            "additional command chain",
            lambda: validate_link(changed_link(
                (":", "&&", *link_body, "&&", "/usr/bin/true", "&&", ":")
            )),
        )

        external_root = fixture_root / "external"
        external_root.mkdir()
        external_archive = external_root / "libexternal.a"
        external_archive.write_bytes(b"external")
        rejected(
            "external archive canary",
            "external prebuilt archive",
            lambda: validate_link(changed_link(
                (*link_command[:-2], str(external_archive), *link_command[-2:])
            )),
        )

        def validate_with_cxx_response(text: str) -> None:
            cxx_response.write_text(text, encoding="utf-8")
            try:
                validate_link()
            finally:
                cxx_response.write_text(cxx_response_text, encoding="utf-8")

        rejected(
            "external -L canary",
            "external library search root",
            lambda: validate_with_cxx_response(
                f"-Wl,--gc-sections -L{external_root} -lfixture_gate -T{linker_script}\n"
            ),
        )
        shared_library = build_root / "libs/libfixture_gate.so"
        shared_library.write_bytes(b"shared")
        rejected(
            "build-root -l shared-library canary",
            "could select a shared library",
            validate_link,
        )
        shared_library.unlink()

        for label, token, expected in (
            ("bare linker-script canary", str(linker_script), "bare linker script input"),
            ("bare shared-library canary", str(shared_library), "dynamic library input"),
            (
                "direct --just-symbols canary",
                f"--just-symbols={linker_script}",
                "unreviewed direct linker option",
            ),
            (
                "direct --version-script canary",
                f"--version-script={linker_script}",
                "unreviewed direct linker option",
            ),
            ("combined -Wl,-l canary", "-Wl,-lfixture_gate", "unreviewed/path-bearing -Wl"),
        ):
            rejected(
                label,
                expected,
                lambda token=token: validate_link(changed_link(
                    (*link_command[:-2], token, *link_command[-2:])
                )),
            )

        extra_response = build_root / "toolchain/extra"
        extra_response.write_text("-Og\n", encoding="utf-8")
        rejected(
            "link response-file inventory canary",
            "response-file inventory drifted",
            lambda: validate_link(changed_link(
                (*link_command[:4], f"@{extra_response}", *link_command[4:])
            )),
        )
        linker_script_text = linker_script.read_text(encoding="utf-8")
        linker_script.write_text(linker_script_text + "INPUT(/tmp/external.o)\n", encoding="utf-8")
        rejected(
            "linker-script indirect-input canary",
            "indirect external-input directive",
            validate_link,
        )
        linker_script.write_text(linker_script_text, encoding="utf-8")

        extra_object = "external-extra.obj"
        extra_records = {
            **base_commands,
            library_archive: archive_command(
                library_archive, (dependency_object, extra_object)
            ),
        }
        rejected(
            "archive extra-object canary",
            "non-compile-database members",
            lambda: validate_link(extra_records),
        )
        duplicate_records = {
            **base_commands,
            library_archive: archive_command(
                library_archive, (dependency_object, main_object)
            ),
        }
        rejected(
            "cross-archive duplicate-object canary",
            "occurs in multiple archives",
            lambda: validate_link(duplicate_records),
        )

        # Bind the exact elf2image command, its output bytes and the complete default Ninja closure.
        app_sdkconfig = fixture_root / "app-sdkconfig"
        app_sdkconfig_text = (
            'CONFIG_IDF_TARGET="esp32c3"\n'
            'CONFIG_ESPTOOLPY_FLASHMODE="dio"\n'
            'CONFIG_ESPTOOLPY_FLASHFREQ="80m"\n'
            'CONFIG_ESPTOOLPY_FLASHSIZE="4MB"\n'
            'CONFIG_ESP_REV_MIN_FULL=3\n'
            'CONFIG_ESP_REV_MAX_FULL=199\n'
        )
        app_sdkconfig.write_text(app_sdkconfig_text, encoding="utf-8")
        app = build_root / "tesla-key-esp32.bin"
        elf = build_root / "tesla-key-esp32.elf"
        app_bytes = b"fixture-app-from-elf"
        app.write_bytes(app_bytes)
        elf.write_bytes(b"fixture-elf")
        timestamp = build_root / ".bin_timestamp"
        timestamp.write_bytes(
            f"{hashlib.md5(app_bytes).hexdigest()}  {app}\n".encode("ascii")
        )
        python = "/opt/esp/python_env/idf5.5_py3.12_env/bin/python"
        app_cmake = "/opt/esp/tools/cmake/fixture/bin/cmake"
        producer = (
            "cd", str(build_root), "&&", python,
            str(idf_root / "components/esptool_py/esptool/esptool.py"),
            "--chip", "esp32c3", "elf2image", "--flash_mode", "dio",
            "--flash_freq", "80m", "--flash_size", "4MB", "--elf-sha256-offset", "0xb0",
            "--secure-pad-v2", "--min-rev", "3", "--min-rev-full", "3",
            "--max-rev-full", "199", "-o", str(app), str(elf), "&&",
            app_cmake, "-E", "echo", f"Generated {app}", "&&",
            app_cmake, "-E", "md5sum", str(app), ">", str(timestamp),
        )

        def producer_loader(tokens: tuple[str, ...]) -> CommandLoader:
            def load(
                _build_root: Path, outputs: tuple[str, ...]
            ) -> dict[str, tuple[str, ...]]:
                if outputs != (".bin_timestamp",):
                    raise SemanticsError("unexpected app fixture target")
                return {".bin_timestamp": tokens}
            return load

        def reproducer(payload_bytes: bytes) -> Callable[..., subprocess.CompletedProcess[bytes]]:
            def run(command: list[str], **_kwargs: Any) -> subprocess.CompletedProcess[bytes]:
                output = Path(command[command.index("-o") + 1])
                output.write_bytes(payload_bytes)
                return subprocess.CompletedProcess(command, 0, stdout=b"", stderr=b"")
            return run

        def validate_producer(
            tokens: tuple[str, ...] = producer, reproduced_bytes: bytes = app_bytes
        ) -> None:
            validate_app_binary_producer(
                build_root, idf_root, app_sdkconfig, "esp32c3", producer_loader(tokens),
                process_runner=reproducer(reproduced_bytes),
            )

        validate_producer()
        changed_elf2image = list(producer)
        changed_elf2image[changed_elf2image.index("80m")] = "40m"
        rejected(
            "altered elf2image command canary",
            "elf2image command/arguments drifted",
            lambda: validate_producer(tuple(changed_elf2image)),
        )
        changed_revision = list(producer)
        changed_revision[changed_revision.index("--min-rev") + 1] = "2"
        rejected(
            "altered legacy revision argument canary",
            "elf2image command/arguments drifted",
            lambda: validate_producer(tuple(changed_revision)),
        )
        app_sdkconfig.write_text(
            app_sdkconfig_text.replace('FLASHFREQ="80m"', 'FLASHFREQ="40m"'),
            encoding="utf-8",
        )
        rejected(
            "target-specific flash-frequency config canary",
            "target-specific app flash geometry drifted",
            validate_producer,
        )
        app_sdkconfig.write_text(app_sdkconfig_text, encoding="utf-8")
        rejected(
            "elf2image reproduced-app byte-drift canary",
            "not the pinned elf2image output",
            lambda: validate_producer(reproduced_bytes=b"different-app"),
        )
        rejected(
            "ELF-to-app POST_BUILD chain canary",
            "extra command chain",
            lambda: validate_producer((*producer, "&&", "/usr/bin/true")),
        )
        app.write_bytes(b"post-build-overwrite")
        rejected(
            "built app-byte overwrite canary",
            "not the pinned elf2image output",
            validate_producer,
        )
        app.write_bytes(app_bytes)

        size_check = (
            "cd", str(build_root / "esp-idf/esptool_py"), "&&", python,
            str(idf_root / "components/partition_table/check_sizes.py"),
            "--offset", "0x8000", "partition", "--type", "app",
            str(build_root / "partition_table/partition-table.bin"), str(app),
        )
        unsigned_notice = (
            "cd", str(build_root), "&&", app_cmake, "-E", "echo",
            "App built but not signed. Sign app before flashing", "&&", app_cmake, "-E", "echo",
            "\t" + python + " "
            + str(idf_root / "components/esptool_py/esptool/espsecure.py")
            + " sign_data --keyfile KEYFILE --version 2                 " + str(app),
        )
        default_commands = (producer, size_check, unsigned_notice)
        validate_default_app_command_inventory(
            default_commands, producer, build_root, idf_root
        )
        rejected(
            "default-closure ALL app overwrite canary",
            "possible post-build app overwrite",
            lambda: validate_default_app_command_inventory(
                (*default_commands, ("cp", "replacement.bin", str(app))),
                producer, build_root, idf_root,
            ),
        )


def self_test() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        (root / "main").mkdir()
        (root / "test").mkdir()
        (root / "test" / "test_runtime_boundary_contract.py").write_text(
            "REVIEWED_LOCAL_CODE_SUFFIXES = ('.h', '.hpp', '.inc', '.inl')\n",
            encoding="utf-8",
        )
        source = root / "main" / "sample.cpp"
        source.write_text("int sample;\n", encoding="utf-8")
        (root / "main" / "CMakeLists.txt").write_text(
            'idf_component_register(\n    SRCS\n        "sample.cpp"\n'
            '    INCLUDE_DIRS\n        "."\n)\n',
            encoding="utf-8",
        )
        sdkconfig = root / "sdkconfig"
        sdkconfig.write_text(
            'CONFIG_IDF_TARGET="esp32c3"\nCONFIG_COMPILER_OPTIMIZATION_DEBUG=y\n',
            encoding="utf-8",
        )
        (root / "build").mkdir()
        commands = root / "build" / "compile_commands.json"
        sample_output = "esp-idf/main/CMakeFiles/__idf_main.dir/sample.cpp.obj"
        sample_compiler = (
            "/opt/esp/tools/riscv32-esp-elf/test/"
            "riscv32-esp-elf/bin/riscv32-esp-elf-g++"
        )
        sample_c_compiler = (
            "/opt/esp/tools/riscv32-esp-elf/test/"
            "riscv32-esp-elf/bin/riscv32-esp-elf-gcc"
        )

        def fixture_dependency_loader(
            build_root: Path, outputs: tuple[str, ...]
        ) -> dict[str, tuple[Path, ...]]:
            config_header = build_root / "config" / "sdkconfig.h"
            config_header.parent.mkdir(parents=True, exist_ok=True)
            config_header.write_text(
                "#pragma once\n"
                '#define CONFIG_IDF_TARGET "esp32c3"\n'
                "#define CONFIG_COMPILER_OPTIMIZATION_DEBUG 1\n",
                encoding="utf-8",
            )
            payload = json.loads((build_root / "compile_commands.json").read_text(encoding="utf-8"))
            sources_by_output = {
                entry["output"]: resolved_source(
                    entry["file"], Path(entry.get("directory", str(root)))
                )
                for entry in payload
                if isinstance(entry, dict) and isinstance(entry.get("output"), str)
                and isinstance(entry.get("file"), str)
            }
            return {
                output: (sources_by_output[output].resolve(), config_header.resolve())
                for output in outputs
            }

        def fixture_validate(
            fixture_target: str,
            fixture_sdkconfig: Path,
            fixture_commands: Path,
            fixture_root: Path,
            *,
            loader: DependencyLoader = fixture_dependency_loader,
            actual_loader: CommandLoader | None = None,
            fixture_graph_validator: GraphValidator | None = None,
        ) -> int:
            def commands_from_fixture(
                _build_root: Path, outputs: tuple[str, ...]
            ) -> dict[str, tuple[str, ...]]:
                payload = json.loads(fixture_commands.read_text(encoding="utf-8"))
                records: dict[str, tuple[str, ...]] = {}
                for entry in payload:
                    output = entry.get("output")
                    if output in outputs:
                        if output in records:
                            raise SemanticsError(f"duplicate fixture command output: {output}")
                        records[output] = expected_ninja_command(
                            output, tuple(command_tokens(entry))
                        )
                return records

            return validate(
                fixture_target,
                fixture_sdkconfig,
                fixture_commands,
                fixture_root,
                dependency_loader=loader,
                command_loader=actual_loader or commands_from_fixture,
                graph_validator=fixture_graph_validator,
            )

        commands.write_text(
            json.dumps(
                [
                    {
                        "file": str(source),
                        "output": sample_output,
                        "arguments": [
                            sample_compiler, "-Wall", "-Og", "-fstack-usage",
                            "-o", sample_output, "-c", str(source),
                        ],
                    }
                ]
            ),
            encoding="utf-8",
        )
        positive_graph_runs = 0

        def positive_graph_validator(
            _build_root: Path, _target: str, _source_root: Path, _idf_root: Path | None,
            _sdkconfig: Path, _firmware_outputs: dict[str, Path],
            _command_loader: CommandLoader,
        ) -> None:
            nonlocal positive_graph_runs
            positive_graph_runs += 1
            self_test_build_graph_contract(Path(__file__).resolve().parent.parent)

        assert fixture_validate(
            "esp32c3", sdkconfig, commands, root,
            fixture_graph_validator=positive_graph_validator,
        ) == 1
        assert positive_graph_runs == 1

        previous_library_path = os.environ.pop("LIBRARY_PATH", None)
        os.environ["LIBRARY_PATH"] = str(root / "untrusted-libraries")
        try:
            try:
                fixture_validate("esp32c3", sdkconfig, commands, root)
            except SemanticsError as exc:
                assert "compiler/linker injection environment" in str(exc)
            else:
                raise AssertionError("LIBRARY_PATH environment mutation was accepted")
        finally:
            os.environ.pop("LIBRARY_PATH", None)
            if previous_library_path is not None:
                os.environ["LIBRARY_PATH"] = previous_library_path

        def prefixed_ninja_command(
            _build_root: Path, outputs: tuple[str, ...]
        ) -> dict[str, tuple[str, ...]]:
            payload = json.loads(commands.read_text(encoding="utf-8"))
            records = {
                entry["output"]: (
                    str(root / "compiler-wrapper"),
                    *expected_ninja_command(
                        entry["output"], tuple(command_tokens(entry))
                    ),
                )
                for entry in payload
                if entry.get("output") in outputs
            }
            return records

        try:
            fixture_validate(
                "esp32c3", sdkconfig, commands, root,
                actual_loader=prefixed_ninja_command,
            )
        except SemanticsError as exc:
            assert "launcher before the pinned compiler" in str(exc)
        else:
            raise AssertionError("Ninja compile launcher mutation was accepted")

        def wrong_ninja_dependency_target(
            _build_root: Path, outputs: tuple[str, ...]
        ) -> dict[str, tuple[str, ...]]:
            payload = json.loads(commands.read_text(encoding="utf-8"))
            records: dict[str, tuple[str, ...]] = {}
            for entry in payload:
                output = entry.get("output")
                if output not in outputs:
                    continue
                tokens = list(expected_ninja_command(
                    output, tuple(command_tokens(entry))
                ))
                target_index = tokens.index("-MT") + 1
                tokens[target_index] = f"{output}.substituted"
                records[output] = tuple(tokens)
            return records

        try:
            fixture_validate(
                "esp32c3", sdkconfig, commands, root,
                actual_loader=wrong_ninja_dependency_target,
            )
        except SemanticsError as exc:
            assert "exact Ninja depfile block" in str(exc)
        else:
            raise AssertionError("Ninja dependency-output mutation was accepted")

        managed_source = (
            root / "managed_components" / "yoziru__tesla-ble" / "src" / "managed.c"
        )
        managed_source.parent.mkdir(parents=True)
        managed_source.write_text("int managed;\n", encoding="utf-8")
        managed_output = (
            "esp-idf/yoziru__tesla-ble/CMakeFiles/"
            "__idf_yoziru__tesla-ble.dir/src/managed.c.obj"
        )
        commands.write_text(
            json.dumps([
                {
                    "file": str(source),
                    "output": sample_output,
                    "arguments": [
                        sample_compiler, "-Wall", "-Og", "-fstack-usage",
                        "-o", sample_output, "-c", str(source),
                    ],
                },
                {
                    "file": str(managed_source),
                    "output": managed_output,
                    "arguments": [
                        sample_c_compiler, "-Og", "-fstack-usage",
                        "-o", managed_output, "-c", str(managed_source),
                    ],
                },
            ]),
            encoding="utf-8",
        )
        assert fixture_validate("esp32c3", sdkconfig, commands, root) == 1

        def managed_prefixed_ninja_command(
            _build_root: Path, outputs: tuple[str, ...]
        ) -> dict[str, tuple[str, ...]]:
            payload = json.loads(commands.read_text(encoding="utf-8"))
            records: dict[str, tuple[str, ...]] = {}
            for entry in payload:
                output = entry.get("output")
                if output not in outputs:
                    continue
                compiled = expected_ninja_command(output, tuple(command_tokens(entry)))
                records[output] = (
                    (str(root / "managed-compiler-wrapper"), *compiled)
                    if output == managed_output else compiled
                )
            return records

        try:
            fixture_validate(
                "esp32c3", sdkconfig, commands, root,
                actual_loader=managed_prefixed_ninja_command,
            )
        except SemanticsError as exc:
            assert "launcher before the pinned compiler" in str(exc)
        else:
            raise AssertionError("managed-component Ninja launcher mutation was accepted")

        payload = json.loads(commands.read_text(encoding="utf-8"))
        payload[1]["arguments"].insert(1, f"-include{root / 'managed-injection.h'}")
        commands.write_text(json.dumps(payload), encoding="utf-8")
        try:
            fixture_validate("esp32c3", sdkconfig, commands, root)
        except SemanticsError as exc:
            assert "forbidden source-injection compiler flag" in str(exc)
        else:
            raise AssertionError("managed-component forced-include mutation was accepted")

        managed_fixture = [
            {
                "file": str(source),
                "output": sample_output,
                "arguments": [
                    sample_compiler, "-Wall", "-Og", "-fstack-usage",
                    "-o", sample_output, "-c", str(source),
                ],
            },
            {
                "file": str(managed_source),
                "output": managed_output,
                "arguments": [
                    sample_c_compiler, "-Og", "-fstack-usage",
                    "-o", managed_output, "-c", str(managed_source),
                ],
            },
        ]
        with tempfile.TemporaryDirectory(prefix="managed-external-include-") as external_directory:
            managed_include_fixture = json.loads(json.dumps(managed_fixture))
            managed_include_fixture[1]["arguments"].insert(1, f"-I{external_directory}")
            commands.write_text(json.dumps(managed_include_fixture), encoding="utf-8")
            try:
                fixture_validate("esp32c3", sdkconfig, commands, root)
            except SemanticsError as exc:
                assert "untrusted external include path" in str(exc)
            else:
                raise AssertionError("managed external include path mutation was accepted")

        commands.write_text(json.dumps(managed_fixture), encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="managed-external-dependency-") as external_directory:
            external_header = Path(external_directory) / "external.hpp"
            external_header.write_text("#define EXTERNAL 1\n", encoding="utf-8")

            def managed_external_dependency_loader(
                build_root: Path, outputs: tuple[str, ...]
            ) -> dict[str, tuple[Path, ...]]:
                records = fixture_dependency_loader(build_root, outputs)
                return {
                    output: (
                        (*dependencies, external_header.resolve())
                        if output == managed_output else dependencies
                    )
                    for output, dependencies in records.items()
                }

            try:
                fixture_validate(
                    "esp32c3", sdkconfig, commands, root,
                    loader=managed_external_dependency_loader,
                )
            except SemanticsError as exc:
                assert "untrusted external header" in str(exc)
            else:
                raise AssertionError("managed external dependency mutation was accepted")

        commands.write_text(
            json.dumps([{
                "file": str(source),
                "output": sample_output,
                "arguments": [
                    sample_compiler, "-Wall", "-Og", "-fstack-usage",
                    "-o", sample_output, "-c", str(source),
                ],
            }]),
            encoding="utf-8",
        )

        valid_dependencies = (
            f"{sample_output}: #deps 2, deps mtime 1 (VALID)\n"
            f"    {source}\n"
            f"    {root / 'build/config/sdkconfig.h'}\n\n"
        )
        parsed_dependencies = parse_ninja_dependencies(
            valid_dependencies, (sample_output,)
        )
        assert len(parsed_dependencies[sample_output]) == 2
        try:
            parse_ninja_dependencies(
                valid_dependencies.replace("(VALID)", "(STALE)"), (sample_output,)
            )
        except SemanticsError as exc:
            assert "not current" in str(exc)
        else:
            raise AssertionError("stale Ninja dependency mutation was accepted")

        generated_callback = root / "build" / "config" / "generated_callback.hpp"
        generated_callback.write_text(
            "void hidden_callback(void*) {}\n"
            "void hidden_start() { xTaskCreate(hidden_callback, nullptr, 1, nullptr, 1, nullptr); }\n",
            encoding="utf-8",
        )

        def generated_callback_loader(
            build_root: Path, outputs: tuple[str, ...]
        ) -> dict[str, tuple[Path, ...]]:
            records = fixture_dependency_loader(build_root, outputs)
            return {
                output: (*dependencies, generated_callback.resolve())
                for output, dependencies in records.items()
            }

        try:
            fixture_validate(
                "esp32c3", sdkconfig, commands, root, loader=generated_callback_loader
            )
        except SemanticsError as exc:
            assert "unreviewed build-generated header" in str(exc)
        else:
            raise AssertionError("generated callback header dependency was accepted")

        hidden_fragment = root / "main" / "hidden.fragment"
        hidden_fragment.write_text(
            "void hidden_task(void*) {}\n"
            "void hidden_start() { xTaskCreate(hidden_task, nullptr, 1, nullptr, 1, nullptr); }\n",
            encoding="utf-8",
        )

        def hidden_fragment_loader(
            build_root: Path, outputs: tuple[str, ...]
        ) -> dict[str, tuple[Path, ...]]:
            records = fixture_dependency_loader(build_root, outputs)
            return {
                output: (*dependencies, hidden_fragment.resolve())
                for output, dependencies in records.items()
            }

        try:
            fixture_validate(
                "esp32c3", sdkconfig, commands, root, loader=hidden_fragment_loader
            )
        except SemanticsError as exc:
            assert "outside the runtime boundary inventory" in str(exc)
        else:
            raise AssertionError("unreviewed main dependency suffix mutation was accepted")

        commands.write_text(
            json.dumps([{
                "file": str(source),
                "output": sample_output,
                "arguments": [
                    "env", f"CPATH={root}", sample_compiler, "-Og", "-fstack-usage",
                    "-c", str(source),
                ],
            }]),
            encoding="utf-8",
        )
        try:
            fixture_validate("esp32c3", sdkconfig, commands, root)
        except SemanticsError as exc:
            assert "pinned target compiler directly" in str(exc)
        else:
            raise AssertionError("compiler environment-wrapper mutation was accepted")

        escaped_compiler = (
            "/opt/esp/tools/../../../"
            + str(root).lstrip("/")
            + "/riscv32-esp-elf-g++"
        )
        commands.write_text(
            json.dumps([{
                "file": str(source),
                "output": sample_output,
                "arguments": [
                    escaped_compiler, "-Og", "-fstack-usage", "-c", str(source)
                ],
            }]),
            encoding="utf-8",
        )
        try:
            fixture_validate("esp32c3", sdkconfig, commands, root)
        except SemanticsError as exc:
            assert "pinned target compiler directly" in str(exc)
        else:
            raise AssertionError("dot-dot compiler-path escape mutation was accepted")

        outside = root / "outside.cpp"
        outside.write_text("int outside;\n", encoding="utf-8")
        commands.write_text(
            json.dumps(
                [{
                    "file": str(outside),
                    "output": "esp-idf/main/CMakeFiles/__idf_main.dir/__/outside.cpp.obj",
                    "arguments": ["cc", "-Og", "-fstack-usage", "-c", str(outside)],
                }]
            ),
            encoding="utf-8",
        )
        try:
            fixture_validate("esp32c3", sdkconfig, commands, root)
        except SemanticsError as exc:
            assert "outside main/ and reviewed dependencies" in str(exc)
        else:
            raise AssertionError("outside-main __idf_main translation unit was accepted")

        commands.write_text(
            json.dumps(
                [{
                    "file": str(source),
                    "output": "esp-idf/other/CMakeFiles/__idf_other.dir/sample.cpp.obj",
                    "arguments": [sample_compiler, "-Og", "-fstack-usage", "-c", str(source)],
                }]
            ),
            encoding="utf-8",
        )
        try:
            fixture_validate("esp32c3", sdkconfig, commands, root)
        except SemanticsError as exc:
            assert "not compiled into __idf_main" in str(exc)
        else:
            raise AssertionError("in-main source outside __idf_main was accepted")

        extra_component = root / "components" / "extra" / "extra.cpp"
        extra_component.parent.mkdir(parents=True)
        extra_component.write_text("int extra;\n", encoding="utf-8")
        commands.write_text(
            json.dumps([
                {
                    "file": str(source),
                    "output": sample_output,
                    "arguments": [sample_compiler, "-Og", "-fstack-usage", "-c", str(source)],
                },
                {
                    "file": str(extra_component),
                    "output": "esp-idf/extra/CMakeFiles/__idf_extra.dir/extra.cpp.obj",
                    "arguments": ["cc", "-O2", "-c", str(extra_component)],
                },
            ]),
            encoding="utf-8",
        )
        try:
            fixture_validate("esp32c3", sdkconfig, commands, root)
        except SemanticsError as exc:
            assert "outside main/ and reviewed dependencies" in str(exc)
        else:
            raise AssertionError("local non-main IDF component was accepted")

        outside_header = root / "outside.hpp"
        outside_header.write_text("#define OUTSIDE 1\n", encoding="utf-8")
        for label, forced_tokens in (
            ("inside forced include", ["-include", str(root / "main" / "inside.hpp")]),
            ("outside forced include", ["-include", str(outside_header)]),
            ("imacros", ["-imacros", str(outside_header)]),
            ("preprocessor forced include", [f"-Wp,-include,{outside_header}"]),
            ("include prefix", ["-iprefix", str(root), "-iwithprefix", "headers"]),
            ("sysroot", [f"--sysroot={root / 'fake-sysroot'}"]),
            ("compiler plugin", [f"-fplugin={root / 'plugin.so'}"]),
            ("compiler specs", [f"-specs={root / 'evil.specs'}"]),
            ("trigraph preprocessing", ["-trigraphs"]),
        ):
            commands.write_text(
                json.dumps([{
                    "file": str(source),
                    "output": sample_output,
                    "arguments": [
                        sample_compiler, "-Og", "-fstack-usage", *forced_tokens, "-c", str(source)
                    ],
                }]),
                encoding="utf-8",
            )
            try:
                fixture_validate("esp32c3", sdkconfig, commands, root)
            except SemanticsError as exc:
                assert "forbidden source-injection compiler flag" in str(exc), (label, exc)
            else:
                raise AssertionError(f"{label} compile mutation was accepted")

        commands.write_text(
            json.dumps([{
                "file": str(source),
                "output": sample_output,
                "arguments": [
                    sample_compiler, "-Og", "-fstack-usage", f"-I{root}", "-c", str(source)
                ],
            }]),
            encoding="utf-8",
        )
        try:
            fixture_validate("esp32c3", sdkconfig, commands, root)
        except SemanticsError as exc:
            assert "include path outside reviewed roots" in str(exc)
        else:
            raise AssertionError("repository-root include path mutation was accepted")

        (root / "main" / "subdir").mkdir()
        commands.write_text(
            json.dumps([{
                "file": str(source),
                "output": sample_output,
                "arguments": [
                    sample_compiler, "-Og", "-fstack-usage",
                    f"-I{root / 'main' / 'subdir'}", "-c", str(source),
                ],
            }]),
            encoding="utf-8",
        )
        try:
            fixture_validate("esp32c3", sdkconfig, commands, root)
        except SemanticsError as exc:
            assert "include path outside reviewed roots" in str(exc)
        else:
            raise AssertionError("alternate main include-root mutation was accepted")

        with tempfile.TemporaryDirectory(prefix="external-include-") as external_directory:
            commands.write_text(
                json.dumps([{
                    "file": str(source),
                    "output": sample_output,
                    "arguments": [
                        sample_compiler, "-Og", "-fstack-usage", f"-I{external_directory}",
                        "-c", str(source),
                    ],
                }]),
                encoding="utf-8",
            )
            try:
                fixture_validate("esp32c3", sdkconfig, commands, root)
            except SemanticsError as exc:
                assert "untrusted external include path" in str(exc)
            else:
                raise AssertionError("external include path mutation was accepted")

        with tempfile.TemporaryDirectory(prefix="external-idf-build-") as external_directory:
            external_build = Path(external_directory)
            external_commands = external_build / "compile_commands.json"
            generated = external_build / "project_elf_src_esp32c3.c"
            generated.write_text("int project_elf;\n", encoding="utf-8")
            external_commands.write_text(
                json.dumps([
                    {
                        "file": str(source),
                        "output": sample_output,
                        "arguments": [
                            sample_compiler, "-Og", "-fstack-usage",
                            "-o", sample_output, "-c", str(source),
                        ],
                    },
                    {
                        "file": str(generated),
                        "output": (
                            "CMakeFiles/tesla-key-esp32.elf.dir/"
                            "project_elf_src_esp32c3.c.obj"
                        ),
                        "arguments": [
                            sample_c_compiler, "-Og", "-o",
                            "CMakeFiles/tesla-key-esp32.elf.dir/"
                            "project_elf_src_esp32c3.c.obj",
                            "-c", str(generated),
                        ],
                    },
                ]),
                encoding="utf-8",
            )
            assert fixture_validate("esp32c3", sdkconfig, external_commands, root) == 1

            generated_evil = external_build / "unreviewed.cpp"
            generated_evil.write_text("int generated_evil;\n", encoding="utf-8")
            payload = json.loads(external_commands.read_text(encoding="utf-8"))
            payload.append({
                "file": str(generated_evil),
                "output": "esp-idf/evil/CMakeFiles/__idf_evil.dir/unreviewed.cpp.obj",
                "arguments": ["cc", "-O2", "-c", str(generated_evil)],
            })
            external_commands.write_text(json.dumps(payload), encoding="utf-8")
            try:
                fixture_validate("esp32c3", sdkconfig, external_commands, root)
            except SemanticsError as exc:
                assert "unreviewed build-generated firmware source" in str(exc)
            else:
                raise AssertionError("unreviewed external build source mutation was accepted")

        flags = root / "flags.rsp"
        flags.write_text("-O3\n", encoding="utf-8")
        commands.write_text(
            json.dumps(
                [
                    {
                        "file": str(source),
                        "output": sample_output,
                        "directory": str(root),
                        "arguments": [
                            sample_compiler,
                            "@flags.rsp",
                            "-Og",
                            "-fstack-usage",
                            "-c",
                            str(source),
                        ],
                    }
                ]
            ),
            encoding="utf-8",
        )
        try:
            fixture_validate("esp32c3", sdkconfig, commands, root)
        except SemanticsError:
            pass
        else:
            raise AssertionError("response-file -O3 override was accepted")

        commands.write_text(
            json.dumps(
                [
                    {
                        "file": str(source),
                        "output": sample_output,
                        "arguments": [sample_compiler, "-Og", "-Os", "-fstack-usage", "-c", str(source)],
                    }
                ]
            ),
            encoding="utf-8",
        )
        try:
            fixture_validate("esp32c3", sdkconfig, commands, root)
        except SemanticsError:
            pass
        else:
            raise AssertionError("conflicting -Os override was accepted")

        commands.write_text(
            json.dumps([{
                "file": str(source),
                "output": sample_output,
                "arguments": [sample_compiler, "-Og", "-c", str(source)],
            }]),
            encoding="utf-8",
        )
        try:
            fixture_validate("esp32c3", sdkconfig, commands, root)
        except SemanticsError:
            pass
        else:
            raise AssertionError("missing -fstack-usage was accepted")

        sdkconfig.write_text(
            'CONFIG_IDF_TARGET="esp32c6"\nCONFIG_COMPILER_OPTIMIZATION_DEBUG=y\n',
            encoding="utf-8",
        )
        try:
            fixture_validate("esp32c3", sdkconfig, commands, root)
        except SemanticsError:
            pass
        else:
            raise AssertionError("wrong effective target was accepted")
    print("effective build semantics self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=TARGETS)
    parser.add_argument("--sdkconfig", type=Path)
    parser.add_argument("--compile-commands", type=Path)
    parser.add_argument("--source-root", type=Path, default=Path.cwd())
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if None in (args.target, args.sdkconfig, args.compile_commands):
        parser.error("--target, --sdkconfig and --compile-commands are required")
    try:
        checked = validate(
            args.target,
            args.sdkconfig,
            args.compile_commands,
            args.source_root,
        )
    except (OSError, UnicodeError, SemanticsError) as exc:
        print(f"effective build semantics failed: {exc}", file=sys.stderr)
        return 1
    print(f"effective build semantics: PASS ({args.target}, {checked} main translation units at -Og)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
