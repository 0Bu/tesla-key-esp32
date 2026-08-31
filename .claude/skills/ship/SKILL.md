---
name: ship
description: Coordinate an explicitly authorized PR merge, release-producing main run, and one explicitly chosen delivery path (USB flash or OTA) for tesla-key-esp32. Never infer merge, release, flash, OTA, push, or PR-edit authority from a review, approval, or another step; confirm each mutation scope separately and use only provenance-bound signed artifacts while preserving NVS.
---

# Runner adapter — `$ship`

The canonical workflow is [`.agents/skills/ship/SKILL.md`](../../../.agents/skills/ship/SKILL.md),
governed by [`AGENTS.md`](../../../AGENTS.md). Read it and follow it as written; this entry exists
only so Claude Code can reach the same workflow, and it holds no instructions of its own.

## Authorization

This adapter grants nothing. It cannot widen, narrow or reinterpret any boundary the canonical
skill or `AGENTS.md` sets, and a request to run one step never authorizes an adjacent mutation —
commit, push, PR change, merge, release, signing, flash, OTA, NVS, live-device access or a vehicle
command each need their own explicit user instruction. If this file and the canonical skill ever
disagree, the canonical skill wins: stop and report the conflict rather than acting on this copy.

## Naming

Refer to this workflow as `$ship` in reports and PR gate records. Claude Code's `/ship`
invocation is a runner mechanism only; it selects this same canonical workflow and changes neither
the authorization boundary nor the spelling required in a PR record.
