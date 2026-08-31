---
name: feature-docs
description: Keep docs/FEATURES.md (this firmware's technical-feature catalog) in sync when a platform feature lands or changes — including runner-neutral agent policy under AGENTS.md, .agents/, .codex/, .claude/, .github/PULL_REQUEST_TEMPLATE.md, tools/agent-hooks/, and tools/agent-config/. Use after implementing or changing a technical feature, before opening the PR. Required before merging a PR whose diff reaches those agent-policy paths, main/, test/, sdkconfig.defaults, partitions.csv, the shipped Pages runtime, release-relevance.sh, or the build/signed-preview/preview-cleanup/PR-policy/bench-acceptance workflows.
---

# Runner adapter — `$feature-docs`

The canonical workflow is [`.agents/skills/feature-docs/SKILL.md`](../../../.agents/skills/feature-docs/SKILL.md),
governed by [`AGENTS.md`](../../../AGENTS.md). Read it and follow it as written; this entry exists
only so Claude Code can reach the same workflow, and it holds no instructions of its own.

## Authorization

This adapter grants nothing. It cannot widen, narrow or reinterpret any boundary the canonical
skill or `AGENTS.md` sets, and a request to run one step never authorizes an adjacent mutation —
commit, push, PR change, merge, release, signing, flash, OTA, NVS, live-device access or a vehicle
command each need their own explicit user instruction. If this file and the canonical skill ever
disagree, the canonical skill wins: stop and report the conflict rather than acting on this copy.

## Naming

Refer to this workflow as `$feature-docs` in reports and PR gate records. Claude Code's `/feature-docs`
invocation is a runner mechanism only; it selects this same canonical workflow and changes neither
the authorization boundary nor the spelling required in a PR record.
