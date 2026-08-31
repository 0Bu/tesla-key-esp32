---
name: skill-audit
description: Read-only drift audit of every canonical skill under .agents/skills, read-only reviewer under .codex/agents, and runner adapter under .claude against tesla-key-esp32. Report contradictions and gate readiness only; never correct files, edit or stamp a PR body, commit, push, merge, release, flash, OTA, or contact a live device/vehicle unless the user separately authorizes implementation.
---

# Runner adapter — `$skill-audit`

The canonical workflow is [`.agents/skills/skill-audit/SKILL.md`](../../../.agents/skills/skill-audit/SKILL.md),
governed by [`AGENTS.md`](../../../AGENTS.md). Read it and follow it as written; this entry exists
only so Claude Code can reach the same workflow, and it holds no instructions of its own.

## Authorization

This adapter grants nothing. It cannot widen, narrow or reinterpret any boundary the canonical
skill or `AGENTS.md` sets, and a request to run one step never authorizes an adjacent mutation —
commit, push, PR change, merge, release, signing, flash, OTA, NVS, live-device access or a vehicle
command each need their own explicit user instruction. If this file and the canonical skill ever
disagree, the canonical skill wins: stop and report the conflict rather than acting on this copy.

## Naming

Refer to this workflow as `$skill-audit` in reports and PR gate records. Claude Code's `/skill-audit`
invocation is a runner mechanism only; it selects this same canonical workflow and changes neither
the authorization boundary nor the spelling required in a PR record.
