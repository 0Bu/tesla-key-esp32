#!/usr/bin/env python3
"""Parse the exact synchronous Claude hook dispatch and the shared hook core."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import sys
from typing import Any


def fail(message: str, code: int = 1) -> None:
    print(f"agent-hook-config: {message}", file=sys.stderr)
    raise SystemExit(code)


root = Path(os.environ.get("AGENT_CONFIG_ROOT") or Path(__file__).resolve().parents[2]).resolve()


def load_json(relative: str) -> dict[str, Any]:
    try:
        value = json.loads((root / relative).read_text(encoding="utf-8"))
    except OSError as exc:
        fail(f"cannot read {relative}: {exc}", 2)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        fail(f"{relative} is not valid JSON: {exc}", 2)
    if not isinstance(value, dict):
        fail(f"{relative} must contain an object", 2)
    return value


def command_hook(
    group: Any,
    *,
    matcher: str | None,
    commands: list[tuple[str, int]],
    label: str,
) -> None:
    if not isinstance(group, dict):
        fail(f"{label} hook group must be an object")
    expected_group_keys = {"hooks"} if matcher is None else {"matcher", "hooks"}
    if set(group) != expected_group_keys:
        fail(f"{label} hook group keys drifted")
    if group.get("matcher") != matcher:
        fail(f"{label} matcher drifted")
    hooks = group.get("hooks")
    if not isinstance(hooks, list) or len(hooks) != len(commands):
        fail(f"{label} command count drifted")
    for hook, (command, timeout) in zip(hooks, commands, strict=True):
        if not isinstance(hook, dict):
            fail(f"{label} command hook must be an object")
        if hook.get("async") is True:
            fail(f"{label} blocking hook must not be async")
        if set(hook) != {"type", "command", "statusMessage", "timeout"}:
            fail(f"{label} command keys drifted; blocking hooks must not be async")
        if hook.get("type") != "command" or hook.get("command") != command:
            fail(f"{label} command drifted")
        if not isinstance(hook.get("statusMessage"), str) or not hook["statusMessage"]:
            fail(f"{label} needs a statusMessage")
        if hook.get("timeout") != timeout:
            fail(f"{label} timeout drifted")


settings = load_json(".claude/settings.json")
if set(settings) != {"hooks"}:
    fail(".claude/settings.json must contain only hooks")
claude_hooks = settings.get("hooks")
events = {"SessionStart", "SubagentStart", "Stop", "PreToolUse", "PostToolUse"}
if not isinstance(claude_hooks, dict) or set(claude_hooks) != events:
    fail(".claude/settings.json event set drifted")
if any(not isinstance(claude_hooks[event], list) for event in events):
    fail(".claude/settings.json event groups must be arrays")
if len(claude_hooks["SessionStart"]) != 1 or len(claude_hooks["SubagentStart"]) != 1:
    fail("Claude start dispatch count drifted")
if len(claude_hooks["Stop"]) != 1 or len(claude_hooks["PreToolUse"]) != 2:
    fail("Claude stop/pre-tool dispatch count drifted")
if len(claude_hooks["PostToolUse"]) != 1:
    fail("Claude post-tool dispatch count drifted")

project_dir = "${CLAUDE_PROJECT_DIR}"
py = f'python3 "{project_dir}/tools/agent-hooks/agent_hook.py"'
command_hook(
    claude_hooks["SessionStart"][0],
    matcher="^(?:startup|resume|clear|compact|fork)$",
    commands=[(f"{py} capabilities", 15), (f"{py} build-efficiency", 15)],
    label="Claude SessionStart",
)
command_hook(
    claude_hooks["SubagentStart"][0], matcher=None,
    commands=[(f"{py} subagent-context", 10)], label="Claude SubagentStart",
)
command_hook(
    claude_hooks["Stop"][0], matcher=None,
    commands=[(f"{py} stop-logic-tests", 600)], label="Claude Stop",
)
command_hook(
    claude_hooks["PreToolUse"][0],
    matcher="^(?:Read|Edit|MultiEdit|Write|Bash)$",
    commands=[(f"{py} pre-tool-guards", 15)], label="Claude guard",
)
command_hook(
    claude_hooks["PreToolUse"][1],
    matcher="^(?:Bash|mcp__.*(?:github|GitHub).*)$",
    commands=[(f'bash "{project_dir}/tools/agent-hooks/require-pr-gates.sh"', 180)],
    label="Claude PR policy",
)
command_hook(
    claude_hooks["PostToolUse"][0], matcher="^(?:Edit|MultiEdit|Write)$",
    commands=[(f"{py} format", 30)], label="Claude formatter",
)

core_files = {
    "agent_hook.py", "merge_payload.py", "pr-gate-lib.sh", "require-pr-gates.sh",
    "run_with_timeout.py", "selftest.sh",
}
core_dir = root / "tools" / "agent-hooks"
try:
    actual_core = {path.name for path in core_dir.iterdir() if path.is_file()}
except OSError as exc:
    fail(f"cannot enumerate shared hook core: {exc}", 2)
if actual_core != core_files:
    fail("shared hook-core inventory drifted")
foreign = re.compile(
    r"daikin|x10a|heat.?pump|hp_modbus|victorialogs|schematic|absence|ui-use-case", re.I
)
for relative in [
    *(f"tools/agent-hooks/{name}" for name in sorted(core_files)),
    ".claude/settings.json",
]:
    if foreign.search((root / relative).read_text(encoding="utf-8")):
        fail(f"foreign-project policy residue found in {relative}")

print("agent-hook-config: parsed 5 lifecycle events and exact synchronous Claude dispatch")
