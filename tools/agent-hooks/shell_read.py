#!/usr/bin/env python3
"""Recognize a bounded, non-executing read command before scanning its literal data.

This is only a publication classifier. Secret, partition and authorization guards still apply.
Unknown options, wrappers, shell operators and expansions retain conservative classification.
"""

from __future__ import annotations

import re
import shlex
import sys


def literal_argv(source: str) -> list[str] | None:
    # Keep quote state: a pipe in a search pattern is data, while an unquoted pipe executes.
    index = 0
    while index < len(source):
        char = source[index]
        if char in "'\"":
            quote = char
            index += 1
            while index < len(source) and source[index] != quote:
                if quote == '"':
                    if source[index] == "\\":
                        index += 2
                        continue
                    if source[index] in "$`":
                        return None
                index += 1
            if index == len(source):
                return None
        elif char in ";&|<>(){}\n\r$`\\*?[~#":
            return None
        index += 1
    try:
        return shlex.split(source)
    except ValueError:
        return None


def data_options(args: list[str], switches: set[str], values: set[str]) -> set[str] | None:
    seen: set[str] = set()
    index = 0
    while index < len(args):
        arg = args[index]
        if arg == "--":
            return seen
        if arg in values:
            if index + 1 == len(args) or args[index + 1].startswith("-"):
                return None
            seen.add(arg)
            index += 2
            continue
        if arg.startswith("--") and "=" in arg and arg.split("=", 1)[0] in values:
            seen.add(arg.split("=", 1)[0])
            index += 1
            continue
        if arg.startswith("-") and arg != "-" and arg not in switches:
            return None
        if arg in switches:
            seen.add(arg)
        index += 1
    return seen


def is_literal_read(source: str) -> bool:
    # Do not reinterpret a quoted executable, assignment or wrapper as a transparent command.
    if not re.match(r"^\s*(?:rg|gh)\s", source):
        return False
    args = literal_argv(source)
    if not args:
        return False
    if args[0] == "rg":
        options = data_options(args[1:], {
            "-n", "--line-number", "-F", "--fixed-strings", "-i", "--ignore-case",
            "-S", "--smart-case", "-s", "--case-sensitive", "-l", "--files-with-matches",
            "-c", "--count", "-o", "--only-matching", "-H", "--with-filename",
            "--no-heading", "--hidden", "--no-ignore", "--no-config", "--files",
        }, {
            "-e", "--regexp", "-g", "--glob", "--iglob", "-t", "--type", "-T", "--type-not",
            "-m", "--max-count", "-A", "--after-context", "-B", "--before-context",
            "-C", "--context", "--color", "--max-columns", "--maxdepth",
        })
        return options is not None and "--no-config" in options
    index = 1
    while index < len(args) and args[index] in {"--repo", "-R"}:
        if index + 1 == len(args):
            return False
        index += 2
    if tuple(args[index:index + 2]) not in {
        ("pr", "list"), ("pr", "view"), ("pr", "checks"), ("run", "list"), ("run", "view"),
    }:
        return False
    options = args[index + 2:]
    names = data_options(options, {
        "--comments", "--required", "--exit-status",
    }, {
        "--json", "--jq", "-q", "--state", "--author", "--assignee", "--base", "--head",
        "--label", "--limit", "--search", "--interval", "--job", "--status", "--workflow",
        "--branch", "--commit", "--event", "--created", "--repo", "-R",
    })
    return names is not None and "--json" in names and bool(names.intersection({"--jq", "-q"}))


if __name__ == "__main__":
    raise SystemExit(0 if is_literal_read(sys.stdin.read()) else 1)
