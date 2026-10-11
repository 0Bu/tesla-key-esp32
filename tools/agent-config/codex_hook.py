#!/usr/bin/env python3
"""Adapt native Codex lifecycle payloads to the single runner-neutral hook core."""
from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "tools/agent-hooks/agent_hook.py"


def invoke(command: list[str], payload: dict, timeout: int) -> subprocess.CompletedProcess:
    return subprocess.run(command, input=json.dumps(payload), text=True, capture_output=True,
                          cwd=ROOT, timeout=timeout, check=False)


def core(action: str, payload: dict, timeout: int = 15) -> str:
    result = invoke([sys.executable, str(CORE), action], payload, timeout)
    if result.returncode:
        raise RuntimeError("project hook could not complete")
    return result.stdout


def deny(reason: str) -> dict:
    return {"hookSpecificOutput": {"hookEventName": "PreToolUse",
            "permissionDecision": "deny", "permissionDecisionReason": reason}}


def dispatch(event: str, payload: dict) -> dict:
    if payload.get("hook_event_name") != event:
        raise ValueError("hook event does not match registration")
    if event == "PreToolUse":
        tool = payload.get("tool_name")
        if not isinstance(tool, str) or not tool or "tool_input" not in payload:
            return deny("Malformed Codex tool payload; project safety cannot be verified.")
        neutral = json.loads((ROOT / ".agents/hooks.json").read_text())
        groups = neutral["pre-tool-guards"]["PreToolUse"]
        if not any(re.fullmatch(group["matcher"], tool) for group in groups):
            return deny("Unsupported Codex tool payload for the registered project safety hook.")
        if re.fullmatch(groups[0]["matcher"], tool):
            output = core("pre-tool-guards", payload)
            verdict = json.loads(output) if output.strip() else {}
            specific = verdict.get("hookSpecificOutput", {})
            if specific.get("permissionDecision") == "deny":
                return {"hookSpecificOutput": specific}
            if verdict:
                raise ValueError("unexpected guard verdict")
        if re.fullmatch(groups[1]["matcher"], tool):
            result = invoke(["bash", str(ROOT / "tools/agent-hooks/require-pr-gates.sh")], payload, 180)
            if result.returncode:
                # The core emits redacted policy diagnostics, never the tool arguments.
                return deny("Current project PR policy could not be verified; inspect the PR gate with its explicit check entry point.")
        return {}
    if event == "PostToolUse":
        core("format", payload, 25)
        return {}
    if event in ("SessionStart", "SubagentStart"):
        actions = ("capabilities", "build-efficiency") if event == "SessionStart" else ("subagent-context",)
        context = "\n".join(core(action, payload, 10) for action in actions)
        return {"hookSpecificOutput": {"hookEventName": event, "additionalContext": context[:8000]}}
    if event == "Stop":
        output = core("stop-logic-tests", payload, 550)
        verdict = json.loads(output) if output.strip() else {}
        if verdict.get("decision") == "block":
            return {"decision": "block", "reason": verdict.get("reason", "Host checks did not pass.")}
        if verdict:
            raise ValueError("unexpected Stop verdict")
        return {}
    raise ValueError("unknown Codex hook event")


def main() -> int:
    event = sys.argv[1] if len(sys.argv) == 2 else ""
    try:
        raw = sys.stdin.read(1048577)
        if len(raw) > 1048576:
            raise ValueError("oversized hook payload")
        payload = json.loads(raw)
        if not isinstance(payload, dict):
            raise ValueError("hook payload must be an object")
        output = dispatch(event, payload)
    except Exception:
        # Codex continues on an ordinary hook crash. Convert every adapter failure to the
        # event's explicit blocking shape, without reflecting sensitive arguments or logs.
        if event == "PreToolUse":
            output = deny("Project safety hook failed; tool execution is blocked.")
        elif event == "Stop":
            output = {"decision": "block", "reason": "Project Stop hook failed; host checks remain unverified."}
        else:
            print("Codex project lifecycle hook could not complete.", file=sys.stderr)
            return 1
    print(json.dumps(output, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
