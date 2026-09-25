#!/usr/bin/env python3
"""Generate the public, version-bound changelog.json beside one OTA feed.

Only explicitly classified commit subjects are published. GitHub Pages and therefore
changelog.json are public by project policy. Commit bodies, authors and repository metadata
never enter this document; the selected normalized subjects are themselves public text and are
screened for obvious Git/issue references.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


MAX_CHANGELOG_BYTES = 960
MAX_DOCUMENT_BYTES = 1025
MAX_ITEM_CHARS = 180
MAX_ITEMS = 12
PUBLIC_TYPES = {"feat", "fix", "perf", "refactor", "revert"}
CONVENTIONAL = re.compile(r"^([a-z]+)(?:\([^)]*\))?(!)?:\s*(.+)$", re.IGNORECASE)
PR_SUFFIX = re.compile(r"\s+\(#\d+\)$")
PRIVATE_REFERENCE = re.compile(
    r"(?:\brefs/(?:heads|tags)/\S+|\borigin/\S+|(?<![\w])#\d+\b|\b[0-9a-f]{7,40}\b)",
    re.IGNORECASE,
)
STRICT_VERSION = re.compile(r"^[0-9A-Za-z][0-9A-Za-z.+-]{0,30}$")
STRICT_SHA = re.compile(r"^[0-9a-f]{40}$")
STRICT_DEV_VERSION = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-dev\.(0|[1-9][0-9]*)$"
)
STRICT_RELEASE_VERSION = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"
)
HISTORY_SEPARATOR = " — "


class ChangelogError(RuntimeError):
    pass


def git(*args: str, check: bool = True) -> str:
    result = subprocess.run(
        ["git", *args],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if check and result.returncode != 0:
        raise ChangelogError(result.stderr.strip() or f"git {' '.join(args)} failed")
    return result.stdout.strip() if result.returncode == 0 else ""


def read_json_at(ref: str, path: str) -> dict[str, Any] | None:
    present = subprocess.run(
        ["git", "cat-file", "-e", f"{ref}:{path}"],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if present.returncode != 0:
        return None
    raw = git("show", f"{ref}:{path}")
    try:
        value = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise ChangelogError(f"published {path} is not valid JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise ChangelogError(f"published {path} is not a JSON object")
    return value


def normalize_subject(subject: str) -> str | None:
    subject = " ".join(subject.strip().split())
    if not subject or subject.startswith("Merge "):
        return None
    match = CONVENTIONAL.match(subject)
    if not match or match.group(1).lower() not in PUBLIC_TYPES:
        return None
    subject = match.group(3)
    subject = PR_SUFFIX.sub("", subject).strip()
    if not subject or PRIVATE_REFERENCE.search(subject):
        return None
    if len(subject) > MAX_ITEM_CHARS:
        subject = subject[: MAX_ITEM_CHARS - 1].rstrip() + "…"
    return subject[0].upper() + subject[1:]


def encoded_document_size(version: str, changelog: str) -> int:
    payload = json.dumps(
        {"version": version, "changelog": changelog},
        ensure_ascii=False,
        separators=(",", ":"),
    ) + "\n"
    return len(payload.encode("utf-8"))


def notes_fit(version: str, text: str) -> bool:
    return (
        bool(text)
        and len(text.encode("utf-8")) <= MAX_CHANGELOG_BYTES
        and encoded_document_size(version, text) <= MAX_DOCUMENT_BYTES
    )


def fit_notes(subjects: list[str], version: str) -> str:
    notes: list[str] = []
    omitted = max(0, len(subjects) - MAX_ITEMS)
    for subject in subjects[:MAX_ITEMS]:
        candidate = "\n".join([*notes, subject])
        if notes_fit(version, candidate):
            notes.append(subject)
        else:
            omitted += 1

    if not notes:
        notes = ["Maintenance and reliability improvements."]
    if omitted:
        tail = f"And {omitted} more user-facing change{'s' if omitted != 1 else ''}."
        while notes and not notes_fit(version, "\n".join([*notes, tail])):
            notes.pop()
            omitted += 1
            tail = f"And {omitted} more user-facing changes."
        notes.append(tail)
    text = "\n".join(notes)
    if not notes_fit(version, text):
        raise ChangelogError("generated changelog exceeds the firmware text/document budget")
    return text


def dev_version_key(version: str) -> tuple[int, int, int, int]:
    match = STRICT_DEV_VERSION.fullmatch(version)
    if not match:
        raise ChangelogError(f"development history contains invalid version {version!r}")
    return tuple(int(value) for value in match.groups())


def dev_core(version: str) -> str:
    key = dev_version_key(version)
    return ".".join(str(value) for value in key[:3])


def format_dev_history(entries: list[tuple[str, str]]) -> str:
    return "\n".join(f"v{version}{HISTORY_SEPARATOR}{note}" for version, note in entries)


def parse_dev_history(text: str) -> list[tuple[str, str]] | None:
    lines = text.splitlines()
    if not lines:
        raise ChangelogError("development changelog is empty")
    if not lines[0].startswith("v") or HISTORY_SEPARATOR not in lines[0]:
        return None

    entries: list[tuple[str, str]] = []
    previous: tuple[int, int, int, int] | None = None
    for line in lines:
        if not line.startswith("v") or HISTORY_SEPARATOR not in line:
            raise ChangelogError("development history mixes versioned and legacy lines")
        version, note = line[1:].split(HISTORY_SEPARATOR, 1)
        note = note.strip()
        key = dev_version_key(version)
        if previous is not None and key < previous:
            raise ChangelogError("development history is not ordered")
        if not note or any(ord(char) < 0x20 for char in note):
            raise ChangelogError("development history contains an invalid note")
        entries.append((version, note))
        previous = key
    return entries


def dev_history_before_current(
    published_ref: str, prior_version: str
) -> list[tuple[str, str]]:
    existing = read_json_at(published_ref, "dev/changelog.json")
    text = existing.get("changelog") if existing else None
    if (
        not isinstance(existing, dict)
        or existing.get("version") != prior_version
        or not isinstance(text, str)
        or not notes_fit(prior_version, text)
    ):
        raise ChangelogError("published dev feed has no reusable bounded changelog")
    parsed = parse_dev_history(text)
    if parsed is None or parsed[-1][0] != prior_version:
        raise ChangelogError("published development history does not end at its feed version")
    return parsed


def published_feed_identity(channel: str, published_ref: str) -> tuple[str | None, str | None]:
    if not git("rev-parse", f"{published_ref}^{{commit}}", check=False):
        return None, None
    manifest_path = "dev/manifest.json" if channel == "dev" else "manifest.json"
    manifest = read_json_at(published_ref, manifest_path)
    if manifest is None:
        return None, None
    prior = manifest.get("sourceSha")
    version = manifest.get("version")
    if not isinstance(prior, str) or not STRICT_SHA.fullmatch(prior):
        raise ChangelogError(f"published {manifest_path} has no valid source provenance")
    if not isinstance(version, str) or not STRICT_VERSION.fullmatch(version):
        raise ChangelogError(f"published {manifest_path} has no valid version")
    return prior, version


def require_ancestor(older: str, newer: str, description: str) -> None:
    ancestry = subprocess.run(
        ["git", "merge-base", "--is-ancestor", older, newer],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if ancestry.returncode != 0:
        raise ChangelogError(description)


def release_baseline_for_new_dev_core(
    published_ref: str, prior_dev_version: str, prior_dev_source: str, source_sha: str
) -> str:
    release_source, release_version = published_feed_identity("release", published_ref)
    expected_release = dev_core(prior_dev_version)
    if (
        not release_source
        or not release_version
        or not STRICT_RELEASE_VERSION.fullmatch(release_version)
        or release_version != expected_release
    ):
        raise ChangelogError(
            f"development core transition requires published release {expected_release}"
        )
    require_ancestor(
        prior_dev_source,
        release_source,
        "published release baseline does not contain the previous development source",
    )
    require_ancestor(
        release_source,
        source_sha,
        "new development source does not descend from the published release baseline",
    )
    return release_source


def previous_source(channel: str, source_sha: str, published_ref: str) -> str | None:
    prior, _ = published_feed_identity(channel, published_ref)
    if prior:
        return prior

    # First publication of a feed: use the last release tag before this source when possible.
    parent = git("rev-parse", f"{source_sha}^", check=False)
    if not parent:
        return None
    tag = git("describe", "--tags", "--match", "v[0-9]*", "--abbrev=0", parent, check=False)
    return git("rev-parse", f"{tag}^{{commit}}") if tag else parent


def build(channel: str, version: str, source_sha: str, published_ref: str) -> dict[str, str]:
    resolved = git("rev-parse", f"{source_sha}^{{commit}}", check=False)
    if not resolved:
        prefix = f"v{version} — " if channel == "dev" else ""
        return {"version": version, "changelog": f"{prefix}Firmware update {version}"}
    if resolved != source_sha:
        raise ChangelogError(f"source SHA resolves to {resolved}, expected {source_sha}")

    published_source, published_version = published_feed_identity(channel, published_ref)
    prior = previous_source(channel, source_sha, published_ref)
    changelog_path = "dev/changelog.json" if channel == "dev" else "changelog.json"
    if prior == source_sha:
        existing = read_json_at(published_ref, changelog_path)
        if (
            existing
            and existing.get("version") == version
            and isinstance(existing.get("changelog"), str)
        ):
            text = existing["changelog"]
            if notes_fit(version, text):
                if channel == "dev":
                    if published_source != source_sha or published_version != version:
                        raise ChangelogError("same-source dev resume has inconsistent identity")
                    parsed = parse_dev_history(text)
                    if parsed is None or parsed[-1][0] != version:
                        raise ChangelogError("development history does not match version")
                return {"version": version, "changelog": text}
        raise ChangelogError("same-source release resume has no reusable matching changelog")

    carry_dev_history = False
    if channel == "dev":
        target_core = dev_core(version)
        if published_source:
            if not published_version or not STRICT_DEV_VERSION.fullmatch(published_version):
                raise ChangelogError("published dev identity has no strict development version")
            prior_core = dev_core(published_version)
            if target_core == prior_core:
                carry_dev_history = True
            else:
                prior = release_baseline_for_new_dev_core(
                    published_ref, published_version, published_source, source_sha
                )

    if prior:
        require_ancestor(
            prior, source_sha, f"published source {prior} is not an ancestor of {source_sha}"
        )
        raw_subjects = git(
            "log", "--first-parent", "--format=%s", f"{prior}..{source_sha}"
        ).splitlines()
    else:
        raw_subjects = [git("show", "-s", "--format=%s", source_sha)]
    subjects = [note for raw in raw_subjects if (note := normalize_subject(raw))]
    current_notes = fit_notes(subjects, version)
    if channel != "dev":
        return {"version": version, "changelog": current_notes}

    entries: list[tuple[str, str]] = []
    if carry_dev_history:
        if prior != published_source or not published_version:
            raise ChangelogError("published dev identity is incomplete")
        entries = dev_history_before_current(published_ref, published_version)
    entries.extend((version, note) for note in current_notes.splitlines())
    text = format_dev_history(entries)
    if not notes_fit(version, text):
        raise ChangelogError(
            "cumulative development changelog exceeds the legacy firmware budget; "
            "publish the current core as a release before starting the next development core"
        )
    return {"version": version, "changelog": text}


def self_test() -> None:
    assert normalize_subject("feat(ui): add OTA notes (#12)") == "Add OTA notes"
    assert normalize_subject("fix!: prevent reboot loop") == "Prevent reboot loop"
    assert normalize_subject("chore(repo): shuffle files") is None
    assert normalize_subject("Add an unclassified internal change") is None
    assert normalize_subject("Merge branch 'main'") is None
    assert normalize_subject("fix: retain issue #441 details") is None
    assert normalize_subject("fix: pin deadbeef while testing") is None
    assert normalize_subject("fix: read refs/heads/private") is None
    assert fit_notes([], "1.2.3") == "Maintenance and reliability improvements."
    crowded = fit_notes(["Ä" * MAX_ITEM_CHARS] * 20, "1.2.3-dev.4")
    assert len(crowded.encode("utf-8")) <= MAX_CHANGELOG_BYTES
    assert encoded_document_size("1.2.3-dev.4", crowded) <= MAX_DOCUMENT_BYTES
    assert "more user-facing changes" in crowded
    encoded = json.dumps(
        {"version": "1.2.3-dev.4", "changelog": "Quote \" and ä\nNext"},
        ensure_ascii=False,
        separators=(",", ":"),
    )
    assert "\\u" not in encoded and "\\n" in encoded and "ä" in encoded

    test_entries = [
        ("1.0.3-dev.1", "First change"),
        ("1.0.3-dev.2", "Second change"),
    ]
    formatted = format_dev_history(test_entries)
    assert parse_dev_history(formatted) == test_entries
    assert parse_dev_history("Target-only legacy note") is None

    # Exercise build() with temporary git repository fixture
    def fixture_git(*args: str) -> str:
        result = subprocess.run(
            ["git", *args],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        return result.stdout.strip()

    def expect_build_error(*args: str) -> None:
        try:
            build(*args)
        except ChangelogError:
            pass
        else:
            raise AssertionError(f"accepted invalid published build fixture: {args!r}")

    original_cwd = Path.cwd()
    with tempfile.TemporaryDirectory(prefix="ota-changelog-selftest-") as temporary:
        os.chdir(temporary)
        try:
            fixture_git("init", "-b", "main")
            fixture_git("config", "user.name", "OTA changelog self-test")
            fixture_git("config", "user.email", "ota-changelog-selftest@example.invalid")
            fixture_git("config", "commit.gpgsign", "false")

            def write_json(path: str, value: dict[str, Any]) -> None:
                target = Path(path)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(
                    json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n",
                    encoding="utf-8",
                )

            def commit_file(subject: str, content: str) -> str:
                Path("firmware.txt").write_text(content, encoding="utf-8")
                fixture_git("add", "firmware.txt")
                fixture_git("commit", "-m", subject)
                return fixture_git("rev-parse", "HEAD")

            def commit_pages(subject: str) -> str:
                fixture_git("add", "dev/manifest.json", "dev/changelog.json", "manifest.json")
                fixture_git("commit", "-m", subject)
                return fixture_git("rev-parse", "HEAD")

            source1 = commit_file("fix: initial release baseline", "v1.0.2\n")
            fixture_git("switch", "-c", "pages")
            write_json("manifest.json", {"version": "1.0.2", "sourceSha": source1})
            fixture_git("add", "manifest.json")
            fixture_git("commit", "-m", "test: root-only Pages fixture")
            pages_without_dev = fixture_git("rev-parse", "HEAD")

            dev1_history = "v1.0.3-dev.1 — Initial dev change"
            write_json("dev/manifest.json", {"version": "1.0.3-dev.1", "sourceSha": source1})
            write_json(
                "dev/changelog.json", {"version": "1.0.3-dev.1", "changelog": dev1_history}
            )
            pages1 = commit_pages("test: publish dev1 fixture")

            fixture_git("switch", "main")
            source2 = commit_file("feat(ota): show cumulative changelog", "dev2\n")
            assert build("dev", "1.0.3-dev.2", source2, pages_without_dev) == {
                "version": "1.0.3-dev.2",
                "changelog": "v1.0.3-dev.2 — Show cumulative changelog",
            }
            continued = build("dev", "1.0.3-dev.2", source2, pages1)
            assert continued == {
                "version": "1.0.3-dev.2",
                "changelog": dev1_history + "\nv1.0.3-dev.2 — Show cumulative changelog",
            }

            fixture_git("switch", "pages")
            write_json("dev/manifest.json", {"version": "1.0.3-dev.2", "sourceSha": source2})
            write_json("dev/changelog.json", continued)
            pages2 = commit_pages("test: publish cumulative dev2 fixture")
            assert build("dev", "1.0.3-dev.2", source2, pages2) == continued
        finally:
            os.chdir(original_cwd)

    print(
        "OTA changelog generator: normalization, build, provenance, and byte-budget checks passed"
    )


def validate_document(path: Path, version: str) -> None:
    raw = path.read_bytes()
    if len(raw) > MAX_DOCUMENT_BYTES:
        raise ChangelogError("document exceeds firmware fetch budget")
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ChangelogError(f"document is not valid UTF-8 JSON: {exc}") from exc
    if not isinstance(value, dict) or value.get("version") != version:
        raise ChangelogError("document version does not match the built firmware")
    text = value.get("changelog")
    if not isinstance(text, str) or not text or len(text.encode("utf-8")) > MAX_CHANGELOG_BYTES:
        raise ChangelogError("document has no bounded changelog string")
    if any(ord(char) < 0x20 and char not in "\n\t" for char in text):
        raise ChangelogError("changelog contains unsupported control characters")
    if STRICT_DEV_VERSION.fullmatch(version) and text.startswith("v"):
        entries = parse_dev_history(text)
        if not entries or entries[-1][0] != version:
            raise ChangelogError("development history does not end at the built firmware")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--channel", choices=("release", "dev"))
    parser.add_argument("--version")
    parser.add_argument("--source-sha")
    parser.add_argument("--published-ref", default="FETCH_HEAD")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--validate", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.validate:
        if not args.version or not STRICT_VERSION.fullmatch(args.version):
            parser.error("--validate requires a compact --version")
        try:
            validate_document(args.validate, args.version)
        except (ChangelogError, OSError) as exc:
            print(f"generate-ota-changelog: {exc}", file=sys.stderr)
            return 1
        print(f"validated OTA changelog for {args.version}: {args.validate}")
        return 0
    if not all((args.channel, args.version, args.source_sha, args.output)):
        parser.error("--channel, --version, --source-sha and --output are required")
    if not STRICT_VERSION.fullmatch(args.version):
        parser.error("--version must be a compact firmware version")
    if not STRICT_SHA.fullmatch(args.source_sha):
        parser.error("--source-sha must be a lowercase 40-character Git SHA")
    try:
        document = build(args.channel, args.version, args.source_sha, args.published_ref)
    except ChangelogError as exc:
        print(f"generate-ota-changelog: {exc}", file=sys.stderr)
        return 1
    payload = json.dumps(document, ensure_ascii=False, separators=(",", ":")) + "\n"
    if len(payload.encode("utf-8")) > MAX_DOCUMENT_BYTES:
        print("generate-ota-changelog: document exceeds firmware fetch budget", file=sys.stderr)
        return 1
    args.output.write_text(payload, encoding="utf-8")
    print(
        f"generated {args.channel} changelog for {args.version}: "
        f"{len(document['changelog'].splitlines())} item(s), {len(payload.encode('utf-8'))} bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
