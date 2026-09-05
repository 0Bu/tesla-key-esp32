#!/usr/bin/env python3
"""Runner-neutral exporter and validator for tesla-key-esp32 specialized subagents.

Translates .codex/agents/*.toml definitions into runner-neutral JSON/markdown specs
for multi-agent environments (Antigravity/Gemini CLI, Claude, Cursor, Windsurf).
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys
import tomllib
from typing import Any


def fail(message: str, code: int = 1) -> None:
    print(f"export-subagents: {message}", file=sys.stderr)
    raise SystemExit(code)


def get_repo_root() -> Path:
    return Path(
        os.environ.get("AGENT_CONFIG_ROOT") or Path(__file__).resolve().parents[2]
    ).resolve()


def load_reviewers(root: Path) -> dict[str, dict[str, Any]]:
    agents_dir = root / ".codex" / "agents"
    if not agents_dir.is_dir():
        fail(f"agents directory missing: {agents_dir}", 2)
    reviewers: dict[str, dict[str, Any]] = {}
    for path in sorted(agents_dir.glob("*.toml")):
        try:
            data = tomllib.loads(path.read_text(encoding="utf-8"))
        except Exception as exc:
            fail(f"failed to parse {path.name}: {exc}", 2)
        name = data.get("name", path.stem)
        reviewers[name] = data
    return reviewers


def format_antigravity_manifest(reviewers: dict[str, dict[str, Any]]) -> dict[str, Any]:
    """Formats reviewers into Antigravity dynamic subagent manifests."""
    manifest: dict[str, Any] = {"subagents": []}
    for name, data in reviewers.items():
        role = name.replace("_", " ").title()
        manifest["subagents"].append(
            {
                "TypeName": name,
                "Role": role,
                "Prompt": data.get("developer_instructions", "").strip(),
                "Description": data.get("description", "").strip(),
                "SandboxMode": data.get("sandbox_mode", "read-only"),
            }
        )
    return manifest


def run_self_test(root: Path) -> None:
    reviewers = load_reviewers(root)
    expected = {
        "agent_config_reviewer",
        "doc_drift_checker",
        "heap_safety_reviewer",
        "multi_target_build_reviewer",
    }
    if set(reviewers.keys()) != expected:
        fail(f"self-test failed: expected reviewers {sorted(expected)}, got {sorted(reviewers.keys())}")
    for name, data in reviewers.items():
        if data.get("sandbox_mode") != "read-only":
            fail(f"self-test failed: {name} is not read-only")
        if not data.get("developer_instructions", "").strip():
            fail(f"self-test failed: {name} has empty developer instructions")
    manifest = format_antigravity_manifest(reviewers)
    if len(manifest["subagents"]) != 4:
        fail("self-test failed: manifest subagents count mismatch")
    print("export-subagents: self-test PASS (4 reviewers verified and exported)")


def main() -> None:
    parser = argparse.ArgumentParser(description="Export tesla-key-esp32 specialized reviewers.")
    parser.add_argument("--self-test", action="store_true", help="Run internal contract verification.")
    parser.add_argument("--format", choices=["json", "antigravity", "markdown"], default="json",
                        help="Export format.")
    args = parser.parse_args()
    root = get_repo_root()

    if args.self_test:
        run_self_test(root)
        return

    reviewers = load_reviewers(root)
    if args.format == "antigravity":
        manifest = format_antigravity_manifest(reviewers)
        print(json.dumps(manifest, indent=2))
    elif args.format == "json":
        print(json.dumps(reviewers, indent=2))
    elif args.format == "markdown":
        for name, data in reviewers.items():
            print(f"## {name}\n")
            print(f"**Description**: {data.get('description', '')}\n")
            print("```markdown")
            print(data.get("developer_instructions", "").strip())
            print("```\n")


if __name__ == "__main__":
    main()
