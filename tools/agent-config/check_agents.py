#!/usr/bin/env python3
"""Parse and validate the Claude subagent contracts and the pinned MCP server.

Codex expressed each reviewer's read-only boundary as `sandbox_mode = "read-only"` plus
`approval_policy = "never"`. Claude subagents have no sandbox field, so the same boundary is
carried by two mechanisms that this checker enforces together: the frontmatter `tools` allow-list
(no file-mutating or delegating tool is reachable at all) and the instruction text itself.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import sys


def fail(message: str, code: int = 1) -> None:
    print(f"agent-subagents: {message}", file=sys.stderr)
    raise SystemExit(code)


root = Path(os.environ.get("AGENT_CONFIG_ROOT") or Path(__file__).resolve().parents[2]).resolve()

EXPECTED_AGENTS = {
    "agent-config-reviewer",
    "doc-drift-checker",
    "heap-safety-reviewer",
    "multi-target-build-reviewer",
}
REQUIRED_KEYS = {"name", "description", "tools"}
# Read-only inspection surface. Everything absent from this set — Edit, MultiEdit, Write,
# NotebookEdit, Task, WebFetch and every MCP tool — stays unreachable for a reviewer.
ALLOWED_TOOLS = {"Read", "Grep", "Glob", "Bash"}
REVIEW_PHRASES = ("read-only", "never edit", "path and line", "cause", "impact", "evidence")
SAFETY_PHRASES = ("flash", "ota", "nvs", "wake the vehicle", "vehicle command")


def frontmatter(relative: str) -> tuple[dict[str, str], str]:
    try:
        text = (root / relative).read_text(encoding="utf-8")
    except OSError as exc:
        fail(f"cannot read {relative}: {exc}", 2)
    except UnicodeDecodeError as exc:
        fail(f"{relative} is not valid UTF-8: {exc}", 2)
    lines = text.split("\n")
    if not lines or lines[0] != "---":
        fail(f"{relative} has no YAML frontmatter")
    try:
        end = lines.index("---", 1)
    except ValueError:
        fail(f"{relative} has unterminated YAML frontmatter")
    values: dict[str, str] = {}
    for line in lines[1:end]:
        if not line.strip():
            continue
        key, separator, value = line.partition(":")
        if not separator or not key.strip() or key != key.strip():
            fail(f"{relative} has invalid restricted YAML frontmatter")
        if key in values:
            fail(f"{relative} duplicates frontmatter key {key}")
        values[key] = value.strip()
    body = "\n".join(lines[end + 1:]).strip()
    if not body:
        fail(f"{relative} has an empty body")
    return values, body


agent_root = root / ".claude" / "agents"
try:
    reviewer_files = sorted(path for path in agent_root.glob("*.md") if path.is_file())
except OSError as exc:
    fail(f"cannot enumerate .claude/agents: {exc}", 2)
if {file.stem for file in reviewer_files} != EXPECTED_AGENTS:
    fail(".claude/agents must contain exactly the four registered reviewers")

for file in reviewer_files:
    relative = file.relative_to(root).as_posix()
    values, body = frontmatter(relative)
    if "model" in values:
        fail(f"{relative} must not pin a model")
    if set(values) != REQUIRED_KEYS:
        fail(f"{relative} frontmatter must contain exactly {', '.join(sorted(REQUIRED_KEYS))}")
    if values["name"] != file.stem:
        fail(f"{relative} name must be {file.stem}")
    if not values["description"].strip():
        fail(f"{relative} needs a non-empty description")
    tools = [tool.strip() for tool in values["tools"].split(",") if tool.strip()]
    if not tools:
        fail(f"{relative} must declare an explicit read-only tool allow-list")
    if len(set(tools)) != len(tools):
        fail(f"{relative} repeats a tool in its allow-list")
    forbidden = sorted(set(tools) - ALLOWED_TOOLS)
    if forbidden:
        fail(f"{relative} grants non-read-only tools: {', '.join(forbidden)}")
    lower = body.lower()
    for phrase in REVIEW_PHRASES:
        if phrase not in lower:
            fail(f"{relative} instructions are missing required review phrase {phrase!r}")
    for phrase in SAFETY_PHRASES:
        if phrase not in lower:
            fail(f"{relative} instructions are missing Tesla safety boundary {phrase!r}")

try:
    mcp = json.loads((root / ".mcp.json").read_text(encoding="utf-8"))
except OSError as exc:
    fail(f"cannot read .mcp.json: {exc}", 2)
except (UnicodeDecodeError, json.JSONDecodeError) as exc:
    fail(f".mcp.json is not valid JSON: {exc}", 2)
expected_mcp = {
    "mcpServers": {"context7": {"command": "npx", "args": ["-y", "@upstash/context7-mcp@4.0.2"]}}
}
if mcp != expected_mcp:
    fail("Context7 must stay exactly pinned to @upstash/context7-mcp@4.0.2 in .mcp.json")

print(
    f"agent-subagents: parsed {len(reviewer_files)} read-only model-independent reviewers "
    "and the pinned Context7 server"
)
