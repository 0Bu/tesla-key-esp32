---
name: skill-audit
description: Read-only drift audit of every canonical skill under .agents/skills, read-only reviewer under .codex/agents, and retained .claude compatibility adapter against tesla-key-esp32. Report contradictions and gate readiness only; never correct files, edit or stamp a PR body, commit, push, merge, release, flash, OTA, or contact a live device/vehicle unless the user separately authorizes implementation.
---

# Legacy compatibility adapter — $skill-audit

This [.claude](../../) entry remains active during the canary period. The canonical project
workflow is [`$skill-audit`](../../../.agents/skills/skill-audit/SKILL.md), governed by
[`AGENTS.md`](../../../AGENTS.md). Canonical skills live under [`.agents/skills/`](../../../.agents/skills/),
and both runner adapters delegate lifecycle and PR policy to
[`tools/agent-hooks/`](../../../tools/agent-hooks/).

## Authorization boundary

Strictly read-only and report-only. Audit canonical skills, read-only reviewers, and legacy adapters; never correct them or edit/stamp a PR body. Publication, merge, release, hardware, OTA, and vehicle actions require separate explicit authority.

The legacy adapter grants no broader permissions than `AGENTS.md` or the canonical skill. A
request to review, diagnose, build, approve, or run one step does not authorize adjacent mutations.
Only separate explicit user authorization may widen the scope.
Stop and report any conflict instead of falling back to historical Claude-only behavior.

## Delegation

Follow the canonical [`$skill-audit`](../../../.agents/skills/skill-audit/SKILL.md) instructions. Use
`$skill-audit` for new invocations and records. The legacy `/skill-audit` spelling may be accepted only
for existing canary-era PR records; it is not canonical and does not change authorization.
