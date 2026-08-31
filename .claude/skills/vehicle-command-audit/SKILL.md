---
name: vehicle-command-audit
description: Read-only three-way conformance audit of tesla-key-esp32 against teslamotors/vehicle-command and the capabilities of the pinned yoziru/tesla-ble. Report upstream/local/feasibility findings; do not edit code/docs/dependencies, publish, or contact a vehicle unless the user separately authorizes implementation.
---

# Runner adapter — `$vehicle-command-audit`

The canonical workflow is [`.agents/skills/vehicle-command-audit/SKILL.md`](../../../.agents/skills/vehicle-command-audit/SKILL.md),
governed by [`AGENTS.md`](../../../AGENTS.md). Read it and follow it as written; this entry exists
only so Claude Code can reach the same workflow, and it holds no instructions of its own.

## Authorization

This adapter grants nothing. It cannot widen, narrow or reinterpret any boundary the canonical
skill or `AGENTS.md` sets, and a request to run one step never authorizes an adjacent mutation —
commit, push, PR change, merge, release, signing, flash, OTA, NVS, live-device access or a vehicle
command each need their own explicit user instruction. If this file and the canonical skill ever
disagree, the canonical skill wins: stop and report the conflict rather than acting on this copy.

## Naming

Refer to this workflow as `$vehicle-command-audit` in reports and PR gate records. Claude Code's `/vehicle-command-audit`
invocation is a runner mechanism only; it selects this same canonical workflow and changes neither
the authorization boundary nor the spelling required in a PR record.
