#!/usr/bin/env python3
"""Fail closed unless GitHub Pages serves the gh-pages branch root."""

from __future__ import annotations

import argparse
import copy
import json
import sys
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit, urlunsplit


class PagesSourceError(ValueError):
    pass


def validate(payload: Any) -> str:
    if not isinstance(payload, dict):
        raise PagesSourceError("GitHub Pages response must be an object")
    if payload.get("build_type") != "legacy":
        raise PagesSourceError("GitHub Pages must use branch-backed legacy publication")
    source = payload.get("source")
    if not isinstance(source, dict):
        raise PagesSourceError("GitHub Pages source must be an object")
    if source.get("branch") != "gh-pages" or source.get("path") != "/":
        raise PagesSourceError("GitHub Pages source must be gh-pages at /")

    raw_url = payload.get("html_url")
    if not isinstance(raw_url, str):
        raise PagesSourceError("GitHub Pages html_url must be a string")
    parsed = urlsplit(raw_url)
    if (
        parsed.scheme != "https"
        or not parsed.netloc
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
    ):
        raise PagesSourceError(
            "GitHub Pages html_url must be credential-free HTTPS without query or fragment"
        )
    return urlunsplit(("https", parsed.netloc, parsed.path.rstrip("/") + "/", "", ""))


def load(path: str) -> Any:
    try:
        if path == "-":
            return json.load(sys.stdin)
        candidate = Path(path)
        if candidate.is_symlink() or not candidate.is_file():
            raise PagesSourceError("Pages metadata path must be a regular non-symlink file")
        return json.loads(candidate.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise PagesSourceError(f"cannot read GitHub Pages metadata: {exc}") from exc


def self_test() -> None:
    valid = {
        "build_type": "legacy",
        "source": {"branch": "gh-pages", "path": "/"},
        "html_url": "https://owner.example/project",
    }
    if validate(valid) != "https://owner.example/project/":
        raise PagesSourceError("valid branch-backed Pages response was not normalized")

    mutations = (
        ("root", []),
        ("workflow-mode", [("build_type", "workflow")]),
        ("missing-mode", [("build_type", None)]),
        ("wrong-branch", [("source.branch", "main")]),
        ("wrong-path", [("source.path", "/docs")]),
        ("insecure-url", [("html_url", "http://owner.example/project")]),
        ("credential-url", [("html_url", "https://user@owner.example/project")]),
        ("query-url", [("html_url", "https://owner.example/project?stale=1")]),
    )
    for name, edits in mutations:
        candidate: Any = copy.deepcopy(valid)
        if name == "root":
            candidate = []
        for key, value in edits:
            if "." in key:
                parent, child = key.split(".", 1)
                candidate[parent][child] = value
            elif value is None:
                candidate.pop(key, None)
            else:
                candidate[key] = value
        try:
            validate(candidate)
        except PagesSourceError:
            continue
        raise PagesSourceError(f"self-test mutation accepted: {name}")
    print(f"pages-source: self-test PASS ({len(mutations)} mutations)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("metadata", nargs="?", help="Pages API JSON path, or - for stdin")
    parser.add_argument("--print-url", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            if args.metadata is not None or args.print_url:
                raise PagesSourceError("--self-test does not accept metadata or --print-url")
            self_test()
            return 0
        if args.metadata is None:
            raise PagesSourceError("metadata path is required")
        url = validate(load(args.metadata))
        if args.print_url:
            print(url)
        else:
            print("pages-source: PASS (legacy gh-pages /)")
        return 0
    except PagesSourceError as exc:
        print(f"pages-source: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
