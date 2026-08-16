#!/usr/bin/env python3
"""Parse exact runner hook dispatch and retained thin Claude adapters."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import stat
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


codex = load_json(".codex/hooks.json")
if set(codex) != {"description", "hooks"} or not isinstance(codex.get("description"), str):
    fail(".codex/hooks.json must contain only description and hooks")
codex_hooks = codex.get("hooks")
events = {"SessionStart", "SubagentStart", "Stop", "PreToolUse", "PostToolUse"}
if not isinstance(codex_hooks, dict) or set(codex_hooks) != events:
    fail(".codex/hooks.json event set drifted")
if any(not isinstance(codex_hooks[event], list) for event in events):
    fail(".codex/hooks.json event groups must be arrays")
if len(codex_hooks["SessionStart"]) != 1 or len(codex_hooks["SubagentStart"]) != 1:
    fail("Codex start dispatch count drifted")
if len(codex_hooks["Stop"]) != 1 or len(codex_hooks["PreToolUse"]) != 2:
    fail("Codex stop/pre-tool dispatch count drifted")
if len(codex_hooks["PostToolUse"]) != 1:
    fail("Codex post-tool dispatch count drifted")

git_root = "$(git rev-parse --show-toplevel)"
py = f'python3 "{git_root}/tools/agent-hooks/agent_hook.py"'
command_hook(
    codex_hooks["SessionStart"][0],
    matcher="^(?:startup|resume|clear|compact)$",
    commands=[(f"{py} capabilities", 15), (f"{py} build-efficiency", 15)],
    label="Codex SessionStart",
)
command_hook(
    codex_hooks["SubagentStart"][0], matcher=None,
    commands=[(f"{py} subagent-context", 10)], label="Codex SubagentStart",
)
command_hook(
    codex_hooks["Stop"][0], matcher=None,
    commands=[(f"{py} stop-logic-tests", 600)], label="Codex Stop",
)
command_hook(
    codex_hooks["PreToolUse"][0],
    matcher="^(?:Read|Edit|MultiEdit|Write|Bash|apply_patch|exec_command|shell|shell_command)$",
    commands=[(f"{py} pre-tool-guards --runner codex", 15)], label="Codex guard",
)
command_hook(
    codex_hooks["PreToolUse"][1],
    matcher="^(?:Bash|exec_command|shell|shell_command|mcp__.*(?:github|GitHub).*)$",
    commands=[(f'bash "{git_root}/tools/agent-hooks/require-pr-gates.sh"', 180)],
    label="Codex PR policy",
)
command_hook(
    codex_hooks["PostToolUse"][0], matcher="^(?:Edit|MultiEdit|Write|apply_patch)$",
    commands=[(f"{py} format", 30)], label="Codex formatter",
)

claude = load_json(".claude/settings.json")
if set(claude) != {"permissions", "hooks"}:
    fail(".claude/settings.json top-level keys drifted")
if claude.get("permissions") != {"allow": []}:
    fail(".claude/settings.json permissions.allow must be empty")
claude_hooks = claude.get("hooks")
if not isinstance(claude_hooks, dict) or set(claude_hooks) != events:
    fail(".claude/settings.json event set drifted")
claude_root = "${CLAUDE_PROJECT_DIR:-.}"
command_hook(
    claude_hooks["SessionStart"][0], matcher=None,
    commands=[
        (f'bash "{claude_root}/.claude/hooks/report-capabilities.sh"', 15),
        (f'bash "{claude_root}/.claude/hooks/build-efficiency-check.sh"', 15),
    ], label="Claude SessionStart",
)
command_hook(
    claude_hooks["SubagentStart"][0], matcher=None,
    commands=[(f'python3 "{claude_root}/tools/agent-hooks/agent_hook.py" subagent-context', 10)],
    label="Claude SubagentStart",
)
command_hook(
    claude_hooks["Stop"][0], matcher=None,
    commands=[(f'bash "{claude_root}/.claude/hooks/run-logic-tests.sh"', 600)],
    label="Claude Stop",
)
command_hook(
    claude_hooks["PreToolUse"][0], matcher="^(?:Read|Edit|MultiEdit|Write|Bash)$",
    commands=[(f'bash "{claude_root}/.claude/hooks/guard-secrets.sh"', 15)],
    label="Claude guard",
)
command_hook(
    claude_hooks["PreToolUse"][1], matcher="^(?:Bash|mcp__.*(?:github|GitHub).*)$",
    commands=[(f'bash "{claude_root}/.claude/hooks/require-project-review.sh"', 180)],
    label="Claude PR policy",
)
command_hook(
    claude_hooks["PostToolUse"][0], matcher="^(?:Edit|MultiEdit|Write)$",
    commands=[(f'bash "{claude_root}/.claude/hooks/clang-format-edit.sh"', 30)],
    label="Claude formatter",
)

adapter_root = 'root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"'
adapters: dict[str, str] = {
    "build-efficiency-check.sh": "\n".join((
        "#!/usr/bin/env bash",
        "# Claude compatibility adapter; canonical implementation is runner-neutral and report-only.",
        "set -u", adapter_root,
        'exec python3 "$root/tools/agent-hooks/agent_hook.py" build-efficiency "$@"', "",
    )),
    "clang-format-edit.sh": "\n".join((
        "#!/usr/bin/env bash", "# Claude compatibility adapter for the runner-neutral formatter.",
        "set -u", adapter_root, 'exec python3 "$root/tools/agent-hooks/agent_hook.py" format', "",
    )),
    "guard-partitions.sh": "\n".join((
        "#!/usr/bin/env bash",
        "# Claude compatibility alias for the consolidated runner-neutral pre-tool guards.",
        "set -u", adapter_root,
        'exec python3 "$root/tools/agent-hooks/agent_hook.py" pre-tool-guards --runner claude', "",
    )),
    "guard-secrets.sh": "\n".join((
        "#!/usr/bin/env bash",
        "# Claude compatibility adapter for consolidated secret and partition guards.",
        "set -u", adapter_root,
        'exec python3 "$root/tools/agent-hooks/agent_hook.py" pre-tool-guards --runner claude', "",
    )),
    "pr-gate-lib.sh": "\n".join((
        "#!/usr/bin/env bash",
        "# Claude compatibility adapter; source the runner-neutral PR-gate library.",
        adapter_root, "# shellcheck source=/dev/null", '. "$root/tools/agent-hooks/pr-gate-lib.sh"', "",
    )),
    "report-capabilities.sh": "\n".join((
        "#!/usr/bin/env bash", "# Claude compatibility adapter for runner-neutral capability discovery.",
        "set -u", adapter_root, 'exec python3 "$root/tools/agent-hooks/agent_hook.py" capabilities', "",
    )),
    "require-feature-docs.sh": "\n".join((
        "#!/usr/bin/env bash",
        "# Claude compatibility adapter; the aggregate core enforces conditional feature-docs evidence.",
        "set -u", adapter_root,
        'exec "$root/tools/agent-hooks/require-pr-gates.sh" --project-dir "$root"', "",
    )),
    "require-project-review.sh": "\n".join((
        "#!/usr/bin/env bash",
        "# Claude compatibility adapter for the aggregate runner-neutral PR policy.",
        "set -u", adapter_root,
        'exec "$root/tools/agent-hooks/require-pr-gates.sh" --project-dir "$root"', "",
    )),
    "require-skill-audit.sh": "\n".join((
        "#!/usr/bin/env bash",
        "# Claude compatibility adapter; the aggregate core enforces create/push skill-audit evidence.",
        "set -u", adapter_root,
        'exec "$root/tools/agent-hooks/require-pr-gates.sh" --project-dir "$root"', "",
    )),
    "run-logic-tests.sh": "\n".join((
        "#!/usr/bin/env bash", "# Claude compatibility adapter for changed host logic tests.",
        "set -u", adapter_root, 'exec python3 "$root/tools/agent-hooks/agent_hook.py" stop-logic-tests', "",
    )),
}
adapter_dir = root / ".claude" / "hooks"
try:
    actual_adapters = {path.name for path in adapter_dir.iterdir() if path.is_file()}
except OSError as exc:
    fail(f"cannot enumerate Claude adapters: {exc}", 2)
if actual_adapters != set(adapters):
    fail("Claude adapter inventory drifted")
for name, expected_text in adapters.items():
    path = adapter_dir / name
    try:
        text = path.read_text(encoding="utf-8")
        mode = path.stat().st_mode
    except OSError as exc:
        fail(f"cannot read adapter {name}: {exc}", 2)
    if not mode & stat.S_IXUSR:
        fail(f"Claude adapter drifted: {name} must be executable Bash")
    if text != expected_text:
        fail(f"Claude adapter drifted: {name} must match its exact thin delegation")

core_files = {
    "agent_hook.py", "merge_payload.py", "pr-gate-lib.sh", "require-pr-gates.sh",
    "run_with_timeout.py", "selftest.sh",
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
    ".codex/hooks.json", ".claude/settings.json",
    *(f".claude/hooks/{name}" for name in sorted(adapters)),
]:
    if foreign.search((root / relative).read_text(encoding="utf-8")):
        fail(f"foreign-project policy residue found in {relative}")

print("agent-hook-config: parsed 5 lifecycle events, exact synchronous dispatch and 10 thin adapters")
