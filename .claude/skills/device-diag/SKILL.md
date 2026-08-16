---
name: device-diag
description: Read-only, cache-only triage of tesla-key-esp32 health from existing evidence or, only with explicit user approval for the live board/target, passive GET /status, /diag, and /heap reads. Never wake or command the vehicle, change diagnostic state, pair, flash, or OTA; diagnose and hand off only.
---

# Legacy compatibility adapter — $device-diag

This [.claude](../../) entry remains active during the canary period. The canonical project
workflow is [`$device-diag`](../../../.agents/skills/device-diag/SKILL.md), governed by
[`AGENTS.md`](../../../AGENTS.md). Canonical skills live under [`.agents/skills/`](../../../.agents/skills/),
and both runner adapters delegate lifecycle and PR policy to
[`tools/agent-hooks/`](../../../tools/agent-hooks/).

## Authorization boundary

Strictly read-only and cache-only by default. Prefer existing evidence; even passive GET state requires explicit user approval for the identified live board/target. Never wake or command the vehicle. Diagnostic state changes such as verbose/clear, serial access, pairing, flash, or OTA require separate explicit user approval and are handed off.

The legacy adapter grants no broader permissions than `AGENTS.md` or the canonical skill. A
request to review, diagnose, build, approve, or run one step does not authorize adjacent mutations.
Only separate explicit user authorization may widen the scope.
Stop and report any conflict instead of falling back to historical Claude-only behavior.

## Delegation

Follow the canonical [`$device-diag`](../../../.agents/skills/device-diag/SKILL.md) instructions. Use
`$device-diag` for new invocations and records. The legacy `/device-diag` spelling may be accepted only
for existing canary-era PR records; it is not canonical and does not change authorization.
