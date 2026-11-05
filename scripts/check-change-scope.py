#!/usr/bin/env python3
"""Select PR feedback scope conservatively; publication and local handoff remain full."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys


BROWSER_FILES = {
    "docs/index.html", "docs/installer-bootstrap.mjs", "docs/serial-port-release.mjs",
    "docs/web-installer.mjs", "scripts/run-browser-tests-linux.sh",
    "test/test_web_ui_browser_gate.py", "test/test_browser_cdp.py", "test/browser_cdp.py",
    "test/web_ui_browser_gate.py", "test/serial_port_release.test.mjs",
    "test/web_installer.test.mjs", "test/web_ui_http.test.mjs",
}

# These Markdown files are executable checker inputs, so prose-only feedback cannot validate them.
CHECKED_MARKDOWN = {".github/PULL_REQUEST_TEMPLATE.md", "docs/FEATURES.md", "docs/THIRD_PARTY.md"}


def classify(paths: list[str]) -> str:
    groups = set()
    for path in paths:
        if not path or path.startswith("/") or ".." in Path(path).parts:
            return "full"
        if path.endswith("AGENTS.md") or path.startswith((".agents/", ".codex/", "tools/agent-", ".githooks/")):
            return "full"
        if path in CHECKED_MARKDOWN:
            return "full"
        if path.endswith(".md"):
            groups.add("docs")
        elif path in BROWSER_FILES or path.startswith(("main/www/", "tools/browser/", "docs/vendor/")):
            groups.add("browser")
        elif path.startswith("main/") and Path(path).suffix in (".cpp", ".hpp", ".h"):
            groups.add("runtime")
        else:
            # Tests, configuration, dependency/toolchain, workflow and unknown changes are full.
            return "full"
    if not groups:
        return "full"
    groups.discard("docs")
    if not groups:
        return "docs"
    return next(iter(groups)) if len(groups) == 1 else "full"


def changed_scope(root: Path, base: str, head: str) -> str:
    if not all(re.fullmatch(r"[0-9a-f]{40}", value) for value in (base, head)):
        return "full"
    try:
        # No rename collapsing: both the removed contract and its new path select coverage.
        raw = subprocess.check_output(["git", "-c", "core.fsmonitor=false", "-C", str(root),
            "diff", "--no-renames", "--name-only", "-z", base, head], timeout=30, stderr=subprocess.DEVNULL)
        return classify([value for value in raw.decode("utf-8").split("\0") if value])
    except (subprocess.SubprocessError, OSError, UnicodeError):
        return "full"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event", default="pull_request")
    parser.add_argument("--base", default="")
    parser.add_argument("--head", default="")
    parser.add_argument("--github-output", action="store_true")
    args = parser.parse_args()
    profile = changed_scope(Path(__file__).resolve().parents[1], args.base, args.head) if args.event == "pull_request" else "full"
    print(("profile=" if args.github_output else "") + profile)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
