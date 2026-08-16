---
name: ship
description: Coordinate an explicitly authorized PR merge, release-producing main run, and one explicitly chosen delivery path (USB flash or OTA) for tesla-key-esp32. Never infer merge, release, flash, OTA, push, or PR-edit authority from a review, approval, or another step; confirm each mutation scope separately and use only provenance-bound signed artifacts while preserving NVS.
---

# Legacy compatibility adapter — $ship

This [.claude](../../) entry remains active during the canary period. The canonical project
workflow is [`$ship`](../../../.agents/skills/ship/SKILL.md), governed by
[`AGENTS.md`](../../../AGENTS.md). Canonical skills live under [`.agents/skills/`](../../../.agents/skills/),
and both runner adapters delegate lifecycle and PR policy to
[`tools/agent-hooks/`](../../../tools/agent-hooks/).

## Authorization boundary

Never infer bundled authority. Require separate explicit user approval for the exact merge, the release/signing side effect, exactly one selected delivery path (USB or OTA), and live verification. Review, approval, commit, push, or one stage grants no other stage.

The legacy adapter grants no broader permissions than `AGENTS.md` or the canonical skill. A
request to review, diagnose, build, approve, or run one step does not authorize adjacent mutations.
Only separate explicit user authorization may widen the scope.
Stop and report any conflict instead of falling back to historical Claude-only behavior.

## Delegation

Follow the canonical [`$ship`](../../../.agents/skills/ship/SKILL.md) instructions. Use
`$ship` for new invocations and records. The legacy `/ship` spelling may be accepted only
for existing canary-era PR records; it is not canonical and does not change authorization.
