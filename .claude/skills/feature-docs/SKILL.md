---
name: feature-docs
description: Keep docs/FEATURES.md (this firmware's technical-feature catalog) in sync when a platform feature lands or changes — including runner-neutral agent policy under AGENTS.md, .agents/, .codex/, .github/PULL_REQUEST_TEMPLATE.md, tools/agent-hooks/, and tools/agent-config/. Use after implementing or changing a technical feature, before opening the PR. Required before merging a PR whose diff reaches those agent-policy paths, main/, test/, sdkconfig.defaults, partitions.csv, the shipped Pages runtime, release-relevance.sh, or the build/signed-preview/preview-cleanup release workflows.
---

# Legacy compatibility adapter — $feature-docs

This [.claude](../../) entry remains active during the canary period. The canonical project
workflow is [`$feature-docs`](../../../.agents/skills/feature-docs/SKILL.md), governed by
[`AGENTS.md`](../../../AGENTS.md). Canonical skills live under [`.agents/skills/`](../../../.agents/skills/),
and both runner adapters delegate lifecycle and PR policy to
[`tools/agent-hooks/`](../../../tools/agent-hooks/).

## Authorization boundary

Edit docs only when the explicit implementation/documentation request includes them. In review/audit scope, report drift only. Never infer PR-body edits, stamps, commit, push, merge, release, flash, OTA, device, or vehicle authority.

The legacy adapter grants no broader permissions than `AGENTS.md` or the canonical skill. A
request to review, diagnose, build, approve, or run one step does not authorize adjacent mutations.
Only separate explicit user authorization may widen the scope.
Stop and report any conflict instead of falling back to historical Claude-only behavior.

## Delegation

Follow the canonical [`$feature-docs`](../../../.agents/skills/feature-docs/SKILL.md) instructions. Use
`$feature-docs` for new invocations and records. The legacy `/feature-docs` spelling may be accepted only
for existing canary-era PR records; it is not canonical and does not change authorization.

Changes under `AGENTS.md`, `.agents/`, `.codex/`, `.github/PULL_REQUEST_TEMPLATE.md`,
`tools/agent-hooks/`, or `tools/agent-config/` are feature-relevant because `docs/FEATURES.md`
catalogs the runner-neutral policy and gate contract.
