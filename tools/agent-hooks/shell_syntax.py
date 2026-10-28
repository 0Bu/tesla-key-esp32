#!/usr/bin/env python3
"""Non-executing shell boundaries shared by the safety and publication guards.

Keep original quotes and bytes for every downstream argument/target check. Opaque
shell-code wrappers and active substitutions retain the former conservative scan.
This is a lexical guard, not a shell interpreter or an execution sandbox.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
import shlex
import sys

SHELLS = {"bash", "dash", "sh", "zsh"}
OPAQUE = SHELLS | {"eval", "exec", "sudo", "nohup", "nice", "time", "timeout", "xargs",
                    "env", "command", "builtin", "python", "python3", "node", "perl", "ruby"}


@dataclass(frozen=True)
class Segment:
    source: str


def active_expansion(source: str, *, substitutions_only: bool = False) -> bool:
    quote = None
    escaped = False
    for index, char in enumerate(source):
        if escaped:
            escaped = False
            continue
        if char == "\\" and quote != "'":
            escaped = True
            continue
        if quote == "'":
            if char == "'":
                quote = None
            continue
        if char == "'" and quote is None:
            quote = char
        elif char == '"':
            quote = None if quote == '"' else '"'
        elif char == "`" or (char == "$" and (not substitutions_only or source[index:index + 2] == "$(")):
            return True
        elif quote is None and not substitutions_only and char in "*?[]{}~":
            return True
    return False


def active_substitution(source: str) -> bool:
    return active_expansion(source, substitutions_only=True)


def literal_segments(source: str) -> tuple[list[Segment], bool]:
    """Split active control operators only; quoted/escaped punctuation stays data."""
    parts = []
    quote = None
    escaped = False
    start = index = 0
    operators = False
    while index < len(source):
        char = source[index]
        if escaped:
            escaped = False
        elif char == "\\" and quote != "'":
            escaped = True
        elif quote:
            if char == quote:
                quote = None
        elif char in "'\"":
            quote = char
        elif char in "<>":
            operators = True
        elif char in ";|&()\n" or (
            char in "{}" and (index == 0 or source[index - 1].isspace() or source[index - 1] in ";|&()")
            and (index + 1 == len(source) or source[index + 1].isspace() or source[index + 1] in ";|&()")
        ):
            operators = True
            text = source[start:index].strip()
            if text:
                parts.append(Segment(text))
            end = index + 1
            if char in "|&;":
                while end < len(source) and source[end] in "|&;":
                    end += 1
            start = index = end
            continue
        index += 1
    if quote or escaped:
        raise ValueError("unterminated shell quoting or escape")
    text = source[start:].strip()
    if text:
        parts.append(Segment(text))
    return parts, operators


PREFIXES = (
    r"^(?:then|do|else)\s+",
    r"^!+\s*",
    r"^command(?:\s+--)?\s+",
    r"^env(?:\s+(?:-[i0]|--ignore-environment|--null|--|(?:-u|-C|-P|--unset|--chdir)\s+[^\s]+|--(?:unset|chdir)=[^\s]+|[A-Za-z_][A-Za-z0-9_]*=[^\s]*))*\s+",
    r"^(?:[A-Za-z_][A-Za-z0-9_]*=[^\s]*\s+)+",
)


def normalize_prefix(source: str) -> str:
    value = source.lstrip()
    for _ in range(32):
        previous = value
        for pattern in PREFIXES:
            if pattern.startswith("^env") and re.search(r"^env\s+.*(?:-S|--split-string)(?:=|\s|$)", value):
                continue
            value = re.sub(pattern, "", value, count=1)
        if value == previous:
            return value
    raise ValueError("too many shell prefixes")


def gate_segments(source: str) -> tuple[list[str], int]:
    # Do not reinterpret potentially executable argument strings as literal data.
    # Preserve the old fail-closed view for these contexts, including nested code.
    opaque = active_substitution(source)
    if opaque:
        raw = [part for part in re.split(r"[;|&(){}\n]", source) if part.strip()]
        return [normalize_prefix(part) for part in raw], max(2, len(raw))
    parts, operators = literal_segments(source)
    for part in parts:
        words = shlex.split(normalize_prefix(part.source), posix=True)
        if words and Path(words[0]).name in OPAQUE:
            opaque = True
    if opaque:
        raw = [part for part in re.split(r"[;|&(){}\n]", source) if part.strip()]
        return [normalize_prefix(part) for part in raw], max(2, len(raw))
    return [normalize_prefix(part.source) for part in parts], max(2 if operators else 0, len(parts))


def legacy_stdin_execution(source: str) -> bool:
    shell = r"(?:bash|dash|sh|zsh)"
    return bool(re.search(rf"\|[^|;\n]*\b{shell}\b", source)
                or re.search(rf"\b{shell}\b[^;&|\n]*(?:<<<|(?<!<)<(?!<))", source)
                or re.search(rf"\b{shell}\b(?:\s+-[^;&|\n]*)*\s+-s(?:\s|$)", source))


def shell_stdin_execution(source: str) -> bool:
    # Preserve the former deny set for execution contexts, including unknown
    # wrappers and pipelines into groups. Exempt only standalone literal data
    # readers responsible for the demonstrated regex/jq false positives.
    try:
        parts, operators = literal_segments(source)
        if not operators and len(parts) == 1 and not active_expansion(source):
            words = shlex.split(parts[0].source, posix=True)
            if words and words[0] == "rg" and not any(
                word == "--pre" or word.startswith("--pre=") for word in words[1:]
            ):
                return False
            if words[:3] == ["gh", "run", "view"]:
                return False
    except ValueError:
        return True
    return legacy_stdin_execution(source)


def main() -> int:
    if len(sys.argv) != 3 or sys.argv[1] not in {"segments", "first", "count"}:
        return 2
    try:
        parts, count = gate_segments(sys.argv[2])
    except ValueError:
        return 2
    if sys.argv[1] == "count":
        print(count)
    elif sys.argv[1] == "first":
        sys.stdout.write(parts[0] if parts else "")
    else:
        for part in parts:
            sys.stdout.buffer.write(part.encode() + b"\0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
