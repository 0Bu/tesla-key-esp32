#!/usr/bin/env python3
"""Validate repository-local Markdown link targets without network access."""

from __future__ import annotations

import argparse
import html
from html.parser import HTMLParser
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unicodedata
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
            # The index still lists unstaged deletions. Keep broken symlinks visible.
            return [path for path in paths if path.exists() or path.is_symlink()]
    except (OSError, subprocess.CalledProcessError, UnicodeDecodeError):
        pass
    return [
        path for path in root.rglob("*.md")
        if not any(part in {".git", "build", "build_mock"} or part.startswith("build_boundary_") for part in path.parts)
    ]


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
    return re.sub(r"<!--[\s\S]*?(?:-->|$)", "", "\n".join(output))


def targets(text: str) -> list[str]:
    visible = visible_markdown(text)
    return [match.group(1) for match in INLINE_LINK.finditer(visible)] + [
        match.group(1) for match in REFERENCE_LINK.finditer(visible)
    ]


def anchors(text: str) -> set[str]:
    visible = visible_markdown(text)
    class ExplicitAnchors(HTMLParser):
        def __init__(self):
            super().__init__(convert_charrefs=True)
            self.found: set[str] = set()

        def handle_starttag(self, tag, attrs):
            for name, value in attrs:
                if value and (name == "id" or (tag == "a" and name == "name")):
                    self.found.add(value)

        handle_startendtag = handle_starttag

    parser = ExplicitAnchors()
    # Inline code is visible heading text but never creates an HTML anchor.
    markup = re.sub(r"(?<!`)(`+)(?!`)[\s\S]*?(?<!`)\1(?!`)", "", visible)
    parser.feed(re.sub(r"\\<", "&lt;", markup))
    result = parser.found
    counts: dict[str, int] = {}
    lines = visible.splitlines()
    for index, line in enumerate(lines):
        match = re.match(r"^ {0,3}#{1,6}\s+(.+?)(?:\s+#+\s*)?$", line)
        heading = match.group(1) if match else None
        if heading is None and index and re.fullmatch(r" {0,3}(?:=+|-+)\s*", line):
            heading = lines[index - 1].strip()
        if not heading:
            continue
        heading = re.sub(r"<[^>]*>", "", heading)
        heading = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", heading)
        heading = html.unescape(heading).lower().replace("`", "")
        slug = "".join(c for c in heading if c in "-_ " or
                       unicodedata.category(c)[0] in "LNM").replace(" ", "-")
        base = slug
        suffix = counts.get(base, 0)
        while slug in result:
            suffix += 1
            slug = f"{base}-{suffix}"
        counts[base] = suffix
        result.add(slug)
    return result


def validate(root: Path) -> tuple[int, int]:
    checked = 0
    documents = markdown_files(root)
    failures: list[str] = []
    anchor_cache: dict[Path, set[str]] = {}
    for document in documents:
        try:
            text = document.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            failures.append(f"{document.relative_to(root)}: unreadable: {exc}")
            continue
        for raw in targets(text):
            target = raw[1:-1] if raw.startswith("<") and raw.endswith(">") else raw
            target = unquote(target)
            if (not target or target.startswith(("/", "//")) or EXTERNAL_SCHEME.match(target)
                    or "${{" in target or "<" in target or ">" in target):
                continue
            path_text, _, fragment = target.partition("#")
            path_text = path_text.split("?", 1)[0]
            checked += 1
            resolved = (document.parent / path_text).resolve() if path_text else document.resolve()
            try:
                resolved.relative_to(root.resolve())
            except ValueError:
                failures.append(f"{document.relative_to(root)}: link escapes repository: {raw}")
                continue
            if not resolved.exists():
                failures.append(f"{document.relative_to(root)}: missing link target: {raw}")
            elif fragment and resolved.is_file() and resolved.suffix.lower() in {".md", ".html"}:
                try:
                    if resolved not in anchor_cache:
                        anchor_cache[resolved] = anchors(resolved.read_text(encoding="utf-8"))
                    if fragment not in anchor_cache[resolved]:
                        failures.append(f"{document.relative_to(root)}: missing link anchor: {raw}")
                except (OSError, UnicodeError) as exc:
                    failures.append(f"{document.relative_to(root)}: unreadable anchor target: {exc}")
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
            "# Local\n[ok](../target.md#target) [anchor](#local) [web](https://example.invalid)\n"
            "```md\n[ignored](missing-in-fence.md)\n```\n",
            encoding="utf-8",
        )
        validate(root)
        source.write_text("# **A** `code`\n# **A** `code`\n<a id='explicit'></a>\n"
                          "[first](#a-code) [duplicate](#a-code-1) [id](#explicit)\n", encoding="utf-8")
        validate(root)
        for decoy in ("`<a id='phantom'>`", "``<a id='phantom'>` ``",
                      "<!-- <a id='phantom'> -->", "<span data-id='phantom'></span>",
                      "<div name='phantom'></div>", "\\<a id='phantom'>"):
            source.write_text(decoy + "\n[broken](#phantom)\n", encoding="utf-8")
            try:
                validate(root)
            except LinkError as exc:
                if "missing link anchor" not in str(exc):
                    raise
            else:
                raise LinkError(f"non-anchor decoy was accepted: {decoy}")
        source.write_text("# Über `code`\n<a name='named'></a>\n[unicode](#%C3%BCber-code) [named](#named)\n", encoding="utf-8")
        validate(root)
        source.write_text("# Present\n[broken](#missing)\n```\n# Missing\n```\n", encoding="utf-8")
        try:
            validate(root)
        except LinkError as exc:
            if "missing link anchor" not in str(exc):
                raise
        else:
            raise LinkError("missing/fenced anchor canary was accepted")
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
