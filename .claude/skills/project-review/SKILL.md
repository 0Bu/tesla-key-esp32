---
name: project-review
description: "Read-only whole-project coherence review of tesla-key-esp32 for bugs, security risks, doc/code drift, config/build mismatches, and cross-cutting inconsistencies. A review request authorizes inspection and reporting only: do not edit files, stamp or edit PRs, commit, push, merge, release, flash, OTA, contact a live vehicle/device, or fix findings unless the user separately authorizes implementation."
---

# Runner adapter — `$project-review`

The canonical workflow is [`.agents/skills/project-review/SKILL.md`](../../../.agents/skills/project-review/SKILL.md),
governed by [`AGENTS.md`](../../../AGENTS.md). Read it and follow it as written; this entry exists
only so Claude Code can reach the same workflow, and it holds no instructions of its own.

## Authorization

This adapter grants nothing. It cannot widen, narrow or reinterpret any boundary the canonical
skill or `AGENTS.md` sets, and a request to run one step never authorizes an adjacent mutation —
commit, push, PR change, merge, release, signing, flash, OTA, NVS, live-device access or a vehicle
command each need their own explicit user instruction. If this file and the canonical skill ever
disagree, the canonical skill wins: stop and report the conflict rather than acting on this copy.

## Naming

Refer to this workflow as `$project-review` in reports and PR gate records. Claude Code's `/project-review`
invocation is a runner mechanism only; it selects this same canonical workflow and changes neither
the authorization boundary nor the spelling required in a PR record.
