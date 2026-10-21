#!/usr/bin/env python3
"""Bind public diagnostic-sharing guidance to the actual bug-form field."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
EXPORT_URL = "https://github.com/0Bu/tesla-key-esp32/blob/main/docs/API.md#diagnostic-export-and-heap-trend"


def validate(template: str) -> None:
    field = re.search(r"(?ms)^  - type: textarea\n    id: diag\n(.*?)(?=^  - type:|\Z)", template)
    if not field:
        raise AssertionError("diagnostic field missing from bug form")
    description = re.search(r"(?ms)^      description: >-\n((?:        [^\n]*\n)+)", field.group(1))
    if not description:
        raise AssertionError("diagnostic sharing description missing")
    text = " ".join(description.group(1).split())
    required = (
        "GET /diag?redact=1", EXPORT_URL,
        "inspect it manually before posting", "remove the board MAC", "VIN",
        "WiFi/network names", "addresses and any other private identifiers",
        "Redaction is not complete sanitation.",
        "Apply the same manual sanitation to serial-monitor excerpts.",
    )
    for phrase in required:
        if phrase not in text:
            raise AssertionError(f"diagnostic sharing guidance omits {phrase!r}")
    if re.search(r"GET /diag(?!\?redact=1)", text):
        raise AssertionError("diagnostic field requests an unredacted export")


def main() -> None:
    template = (ROOT / ".github/ISSUE_TEMPLATE/bug_report.yml").read_text(encoding="utf-8")
    validate(template)
    replacements = (
        ("GET /diag?redact=1", "GET /diag"),
        (EXPORT_URL, "https://example.invalid/"),
        ("inspect it manually before posting", "post it"),
        ("remove the board MAC", "retain board MAC"),
        ("WiFi/network names", "WiFi"),
        ("addresses and any other private identifiers", "other fields"),
        ("Redaction is not complete sanitation.", "Redaction is complete sanitation."),
        ("Apply the same manual sanitation to serial-monitor excerpts.", "Paste serial-monitor excerpts."),
        ("    id: diag", "    id: unrelated"),
    )
    for before, after in replacements:
        if template.count(before) != 1:
            raise AssertionError(f"mutation anchor missing or ambiguous: {before!r}")
        changed = template.replace(before, after, 1)
        # The original correct wording outside the field cannot rescue a broken description.
        changed += "\n# Unrelated historical wording: " + before + "\n"
        try:
            validate(changed)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"sharing-guidance mutation was accepted: {before!r}")
    print(f"diagnostic export guidance: PASS ({len(replacements)} rejected mutations)")


if __name__ == "__main__":
    main()
