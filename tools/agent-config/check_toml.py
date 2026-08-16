#!/usr/bin/env python3
"""Parse and validate Codex configuration and read-only reviewer contracts."""

from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import tomllib
from typing import Any


def fail(message: str, code: int = 1) -> None:
    print(f"agent-toml: {message}", file=sys.stderr)
    raise SystemExit(code)


root = Path(os.environ.get("AGENT_CONFIG_ROOT") or Path(__file__).resolve().parents[2]).resolve()


def load_toml(relative: str) -> dict[str, Any]:
    try:
        source = (root / relative).read_bytes().decode("utf-8")
        parsed = tomllib.loads(source)
    except OSError as exc:
        fail(f"cannot read {relative}: {exc}", 2)
    except (UnicodeDecodeError, tomllib.TOMLDecodeError) as exc:
        fail(f"{relative} is not valid TOML: {exc}", 2)
    if not isinstance(parsed, dict):
        fail(f"{relative} must contain a TOML table", 2)
    return parsed


def has_key(value: Any, forbidden: str) -> bool:
    if isinstance(value, dict):
        return any(key == forbidden or has_key(child, forbidden) for key, child in value.items())
    if isinstance(value, list):
        return any(has_key(child, forbidden) for child in value)
    return False


config = load_toml(".codex/config.toml")
if has_key(config, "model"):
    fail(".codex/config.toml must not pin a model")
if set(config) != {"developer_instructions", "features", "agents", "mcp_servers"}:
    fail(".codex/config.toml top-level keys drifted")
instructions = config.get("developer_instructions")
if not isinstance(instructions, str) or "AGENTS.md" not in instructions:
    fail(".codex/config.toml must delegate canonical policy to AGENTS.md")
for phrase in ("read-only", "does not", "commit", "flash", "NVS", "wake the vehicle"):
    if phrase not in instructions:
        fail(f".codex/config.toml developer instructions are missing {phrase!r}")

features = config.get("features")
if features != {"multi_agent": True, "hooks": True}:
    fail(".codex/config.toml must explicitly enable multi_agent and hooks")

agents = config.get("agents")
if not isinstance(agents, dict) or agents.get("enabled") is not True:
    fail(".codex/config.toml must enable agents")
if agents.get("max_concurrent_threads_per_session") != 3:
    fail(".codex/config.toml must cap concurrent subagent threads at 3")
expected_agents = {
    "agent_config_reviewer",
    "doc_drift_checker",
    "heap_safety_reviewer",
    "multi_target_build_reviewer",
}
registrations = set(agents) - {"enabled", "max_concurrent_threads_per_session"}
if registrations != expected_agents:
    fail(f".codex/config.toml reviewer registrations drifted: {sorted(registrations)}")
for name in expected_agents:
    registration = agents[name]
    expected_file = f"agents/{name}.toml"
    if not isinstance(registration, dict) or set(registration) != {"description", "config_file"}:
        fail(f"reviewer registration {name} must contain description and config_file")
    if not isinstance(registration["description"], str) or not registration["description"].strip():
        fail(f"reviewer registration {name} has no description")
    if registration["config_file"] != expected_file:
        fail(f"reviewer registration {name} must use {expected_file}")

mcp_servers = config.get("mcp_servers")
if not isinstance(mcp_servers, dict) or set(mcp_servers) != {"context7"}:
    fail(".codex/config.toml must configure only Context7")
context7 = mcp_servers["context7"]
if context7 != {"command": "npx", "args": ["-y", "@upstash/context7-mcp@4.0.2"]}:
    fail("Context7 must stay exactly pinned to @upstash/context7-mcp@4.0.2")
try:
    compatible = json.loads((root / ".mcp.json").read_text(encoding="utf-8"))
except OSError as exc:
    fail(f"cannot read .mcp.json: {exc}", 2)
except (UnicodeDecodeError, json.JSONDecodeError) as exc:
    fail(f".mcp.json is not valid JSON: {exc}", 2)
if compatible != {"mcpServers": {"context7": context7}}:
    fail("Context7 command and exact pin must match .mcp.json")

agent_root = root / ".codex" / "agents"
try:
    reviewer_files = sorted(agent_root.glob("*.toml"))
except OSError as exc:
    fail(f"cannot enumerate .codex/agents: {exc}", 2)
if {file.stem for file in reviewer_files} != expected_agents:
    fail(".codex/agents must contain exactly the four registered reviewers")

required_keys = {
    "name", "description", "sandbox_mode", "approval_policy", "developer_instructions"
}
for file in reviewer_files:
    relative = file.relative_to(root).as_posix()
    reviewer = load_toml(relative)
    if has_key(reviewer, "model"):
        fail(f"{relative} must not pin a model")
    if set(reviewer) != required_keys:
        fail(f"{relative} must contain exactly {', '.join(sorted(required_keys))}")
    if reviewer.get("name") != file.stem:
        fail(f"{relative} name must be {file.stem}")
    if reviewer.get("sandbox_mode") != "read-only":
        fail(f"{relative} sandbox_mode must be read-only")
    if reviewer.get("approval_policy") != "never":
        fail(f"{relative} approval_policy must be never")
    for key in ("description", "developer_instructions"):
        if not isinstance(reviewer.get(key), str) or not reviewer[key].strip():
            fail(f"{relative} needs a non-empty {key}")
    lower = reviewer["developer_instructions"].lower()
    for phrase in ("read-only", "never edit", "path and line", "cause", "impact", "evidence"):
        if phrase not in lower:
            fail(f"{relative} instructions are missing required review phrase {phrase!r}")
    for phrase in ("flash", "ota", "nvs", "wake the vehicle", "vehicle command"):
        if phrase not in lower:
            fail(f"{relative} instructions are missing Tesla safety boundary {phrase!r}")

print(f"agent-toml: parsed Codex config and {len(reviewer_files)} read-only model-independent reviewers")
