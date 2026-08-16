---
name: vehicle-command-audit
description: Read-only three-way conformance audit of tesla-key-esp32 against teslamotors/vehicle-command and the capabilities of the pinned yoziru/tesla-ble. Report upstream/local/feasibility findings; do not edit code/docs/dependencies, publish, or contact a vehicle unless the user separately authorizes implementation.
---

# Legacy compatibility adapter — $vehicle-command-audit

This [.claude](../../) entry remains active during the canary period. The canonical project
workflow is [`$vehicle-command-audit`](../../../.agents/skills/vehicle-command-audit/SKILL.md), governed by
[`AGENTS.md`](../../../AGENTS.md). Canonical skills live under [`.agents/skills/`](../../../.agents/skills/),
and both runner adapters delegate lifecycle and PR policy to
[`tools/agent-hooks/`](../../../tools/agent-hooks/).

## Authorization boundary

Strictly read-only three-way audit: upstream behavior, local implementation, and feasibility with the pinned tesla-ble. Report only; never fix code/docs/dependencies or contact, wake, or command a vehicle without separate explicit authorization.

The legacy adapter grants no broader permissions than `AGENTS.md` or the canonical skill. A
request to review, diagnose, build, approve, or run one step does not authorize adjacent mutations.
Only separate explicit user authorization may widen the scope.
Stop and report any conflict instead of falling back to historical Claude-only behavior.

## Delegation

Follow the canonical [`$vehicle-command-audit`](../../../.agents/skills/vehicle-command-audit/SKILL.md) instructions. Use
`$vehicle-command-audit` for new invocations and records. The legacy `/vehicle-command-audit` spelling may be accepted only
for existing canary-era PR records; it is not canonical and does not change authorization.
