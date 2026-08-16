---
name: e2e-evcc
description: Run the live end-to-end test of the evcc → tesla-key-esp32 → vehicle path from inside the evcc pod. Use only after the user explicitly authorizes contacting the live cluster/device; command modes require a separate, scope-specific opt-in because they send real signed BLE commands and can physically actuate the vehicle.
---

# Legacy compatibility adapter — $e2e-evcc

This [.claude](../../) entry remains active during the canary period. The canonical project
workflow is [`$e2e-evcc`](../../../.agents/skills/e2e-evcc/SKILL.md), governed by
[`AGENTS.md`](../../../AGENTS.md). Canonical skills live under [`.agents/skills/`](../../../.agents/skills/),
and both runner adapters delegate lifecycle and PR policy to
[`tools/agent-hooks/`](../../../tools/agent-hooks/).

## Authorization boundary

Do not contact the live cluster, board, or vehicle without explicit user approval. Read-path approval does not authorize RUN_COMMANDS, charge toggles, or the full command sweep; each broader command scope needs a separate explicit opt-in.

The legacy adapter grants no broader permissions than `AGENTS.md` or the canonical skill. A
request to review, diagnose, build, approve, or run one step does not authorize adjacent mutations.
Only separate explicit user authorization may widen the scope.
Stop and report any conflict instead of falling back to historical Claude-only behavior.

## Delegation

Follow the canonical [`$e2e-evcc`](../../../.agents/skills/e2e-evcc/SKILL.md) instructions. Use
`$e2e-evcc` for new invocations and records. The legacy `/e2e-evcc` spelling may be accepted only
for existing canary-era PR records; it is not canonical and does not change authorization.
