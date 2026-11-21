#!/usr/bin/env python3
"""Parse and validate runner-neutral agent hook configuration."""

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


def check_handler_list(handlers: Any, commands: list[tuple[str, int]], label: str) -> None:
    if not isinstance(handlers, list) or len(handlers) != len(commands):
        fail(f"{label} command count drifted")
    # Counts have already been checked; keep this validator usable with macOS Python 3.9.
    for hook, (command, timeout) in zip(handlers, commands):
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
    check_handler_list(hooks, commands, label)


git_root = "$(git rev-parse --show-toplevel)"
py = f'python3 "{git_root}/tools/agent-hooks/agent_hook.py"'

agents = load_json(".agents/hooks.json")
if set(agents) != {"pre-tool-guards"} or not isinstance(agents["pre-tool-guards"], dict):
    fail(".agents/hooks.json must contain only the pre-tool-guards object")

pre_tools = agents["pre-tool-guards"].get("PreToolUse")
if not isinstance(pre_tools, list) or len(pre_tools) != 2:
    fail("Antigravity PreToolUse group count drifted")
command_hook(
    pre_tools[0],
    matcher="^(?:run_command|view_file|replace_file_content|write_to_file|apply_patch|Read|Edit|MultiEdit|Write|Bash|exec_command|shell|shell_command)$",
    commands=[(f"{py} pre-tool-guards", 15)],
    label="Antigravity guard",
)
dispatch = f'bash "{git_root}/scripts/with-host-python.sh" python3 "{git_root}/tools/agent-config/codex_hook.py"'
fallback = """printf '%s\\n' '{"decision":"deny","reason":"Project hook bootstrap failed"}'"""
command_hook(
    pre_tools[1],
    matcher="^(?:run_command|Bash|exec_command|shell|shell_command|mcp__.*(?:github|GitHub).*)$",
    commands=[(f"{dispatch} AntigravityPR || {fallback}", 200)],
    label="Antigravity PR policy",
)

core_files = {
    "agent_hook.py", "merge_payload.py", "parallel-checks.sh", "pr-gate-lib.sh", "require-pr-gates.sh",
    "run_with_timeout.py", "selftest.sh", "shell_read.py",
}
core_dir = root / "tools" / "agent-hooks"
try:
    actual_core = {path.name for path in core_dir.iterdir() if path.is_file()}
except OSError as exc:
    fail(f"cannot enumerate neutral hook core: {exc}", 2)
if actual_core != core_files:
    fail("runner-neutral hook-core inventory drifted")
foreign = re.compile(
    r"daikin|x10a|heat.?pump|hp_modbus|victorialogs|schematic|absence|ui-use-case", re.I
)
for relative in [
    *(f"tools/agent-hooks/{name}" for name in sorted(core_files)),
    ".agents/hooks.json",
]:
    if foreign.search((root / relative).read_text(encoding="utf-8")):
        fail(f"foreign-project policy residue found in {relative}")

print("agent-hook-config: parsed lifecycle events and exact synchronous agent dispatch")
