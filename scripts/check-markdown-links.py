#!/usr/bin/env python3
"""Validate repository-local Markdown link targets without network access."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from urllib.parse import unquote


INLINE_LINK = re.compile(r"!?\[[^\]]*\]\((<[^>]+>|[^\s)]+)(?:\s+['\"(][^)]*)?\)")
REFERENCE_LINK = re.compile(r"^\s*\[[^\]]+\]:\s*(<[^>]+>|\S+)", re.MULTILINE)
EXTERNAL_SCHEME = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")


class LinkError(RuntimeError):
    pass


def markdown_files(root: Path) -> list[Path]:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "ls-files", "--cached", "--others", "--exclude-standard",
             "-z", "--", "*.md"],
            capture_output=True,
            check=True,
        )
        paths = [root / value.decode("utf-8") for value in result.stdout.split(b"\0") if value]
        if paths:
            return paths
    except (OSError, subprocess.CalledProcessError, UnicodeDecodeError):
        pass
    return [path for path in root.rglob("*.md") if ".git" not in path.parts]


def visible_markdown(text: str) -> str:
    output: list[str] = []
    fence: str | None = None
    for line in text.splitlines():
        stripped = line.lstrip()
        marker = "```" if stripped.startswith("```") else "~~~" if stripped.startswith("~~~") else None
        if marker:
            if fence is None:
                fence = marker
            elif fence == marker:
                fence = None
            output.append("")
        elif fence is None:
            output.append(line)
        else:
            output.append("")
    return "\n".join(output)


def targets(text: str) -> list[str]:
    visible = visible_markdown(text)
    return [match.group(1) for match in INLINE_LINK.finditer(visible)] + [
        match.group(1) for match in REFERENCE_LINK.finditer(visible)
    ]


def validate(root: Path) -> tuple[int, int]:
    checked = 0
    documents = markdown_files(root)
    failures: list[str] = []
    for document in documents:
        try:
            text = document.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            failures.append(f"{document.relative_to(root)}: unreadable: {exc}")
            continue
        for raw in targets(text):
            target = raw[1:-1] if raw.startswith("<") and raw.endswith(">") else raw
            target = unquote(target)
            if (not target or target.startswith(("#", "/", "//")) or EXTERNAL_SCHEME.match(target)
                    or "${{" in target or "<" in target or ">" in target):
                continue
            path_text = target.split("#", 1)[0].split("?", 1)[0]
            if not path_text:
                continue
            checked += 1
            resolved = (document.parent / path_text).resolve()
            try:
                resolved.relative_to(root.resolve())
            except ValueError:
                failures.append(f"{document.relative_to(root)}: link escapes repository: {raw}")
                continue
            if not resolved.exists():
                failures.append(f"{document.relative_to(root)}: missing link target: {raw}")
    if failures:
        raise LinkError("\n".join(failures))
    return len(documents), checked


def self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="markdown-link-selftest-") as directory:
        root = Path(directory)
        (root / "docs").mkdir()
        (root / "target.md").write_text("# Target\n", encoding="utf-8")
        source = root / "docs/source.md"
        source.write_text(
            "[ok](../target.md) [anchor](#local) [web](https://example.invalid)\n"
            "```md\n[ignored](missing-in-fence.md)\n```\n",
            encoding="utf-8",
        )
        validate(root)
        source.write_text("[broken](missing.md)\n", encoding="utf-8")
        try:
            validate(root)
        except LinkError as exc:
            if "missing.md" not in str(exc):
                raise LinkError(f"missing-link canary failed for wrong reason: {exc}") from exc
        else:
            raise LinkError("missing local Markdown link was accepted")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        documents, checked = validate(args.root.resolve())
        if args.self_test:
            self_test()
    except LinkError as exc:
        print(f"markdown-links: {exc}", file=sys.stderr)
        return 1
    print(f"markdown-links: PASS ({documents} documents, {checked} local targets"
          + (", mutation canary" if args.self_test else "") + ")")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
