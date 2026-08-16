---
name: project-review
description: "Read-only whole-project coherence review of tesla-key-esp32 for bugs, security risks, doc/code drift, config/build mismatches, and cross-cutting inconsistencies. A review request authorizes inspection and reporting only: do not edit files, stamp or edit PRs, commit, push, merge, release, flash, OTA, contact a live vehicle/device, or fix findings unless the user separately authorizes implementation."
---

# Legacy compatibility adapter — $project-review

This [.claude](../../) entry remains active during the canary period. The canonical project
workflow is [`$project-review`](../../../.agents/skills/project-review/SKILL.md), governed by
[`AGENTS.md`](../../../AGENTS.md). Canonical skills live under [`.agents/skills/`](../../../.agents/skills/),
and both runner adapters delegate lifecycle and PR policy to
[`tools/agent-hooks/`](../../../tools/agent-hooks/).

## Authorization boundary

Review/audit is strictly read-only and report-only. Never fix findings, edit/stamp a PR body, commit, push, merge, release, sign, flash, OTA, or contact a live device/vehicle unless the user separately authorizes implementation.

The legacy adapter grants no broader permissions than `AGENTS.md` or the canonical skill. A
request to review, diagnose, build, approve, or run one step does not authorize adjacent mutations.
Only separate explicit user authorization may widen the scope.
Stop and report any conflict instead of falling back to historical Claude-only behavior.

## Delegation

Follow the canonical [`$project-review`](../../../.agents/skills/project-review/SKILL.md) instructions. Use
`$project-review` for new invocations and records. The legacy `/project-review` spelling may be accepted only
for existing canary-era PR records; it is not canonical and does not change authorization.
