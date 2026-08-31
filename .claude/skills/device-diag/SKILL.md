---
name: device-diag
description: Read-only, cache-only triage of tesla-key-esp32 health from existing evidence or, only with explicit user approval for the live board/target, passive GET /status, /diag, and /heap reads. Never wake or command the vehicle, change diagnostic state, pair, flash, or OTA; diagnose and hand off only.
---

# Runner adapter — `$device-diag`

The canonical workflow is [`.agents/skills/device-diag/SKILL.md`](../../../.agents/skills/device-diag/SKILL.md),
governed by [`AGENTS.md`](../../../AGENTS.md). Read it and follow it as written; this entry exists
only so Claude Code can reach the same workflow, and it holds no instructions of its own.

## Authorization

This adapter grants nothing. It cannot widen, narrow or reinterpret any boundary the canonical
skill or `AGENTS.md` sets, and a request to run one step never authorizes an adjacent mutation —
commit, push, PR change, merge, release, signing, flash, OTA, NVS, live-device access or a vehicle
command each need their own explicit user instruction. If this file and the canonical skill ever
disagree, the canonical skill wins: stop and report the conflict rather than acting on this copy.

## Naming

Refer to this workflow as `$device-diag` in reports and PR gate records. Claude Code's `/device-diag`
invocation is a runner mechanism only; it selects this same canonical workflow and changes neither
the authorization boundary nor the spelling required in a PR record.
