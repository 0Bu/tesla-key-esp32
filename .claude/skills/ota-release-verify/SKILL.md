---
name: ota-release-verify
description: "Read-only verification of the already-published OTA channel against the latest GitHub Release: bind manifest.sourceSha to the Release tag commit, hash-check all 16 manifest parts, and verify every app's embedded version and chip family without publishing, releasing, flashing, OTA, or local ref mutation. Optional live-board GETs require separate explicit user approval."
---

# Runner adapter — `$ota-release-verify`

The canonical workflow is [`.agents/skills/ota-release-verify/SKILL.md`](../../../.agents/skills/ota-release-verify/SKILL.md),
governed by [`AGENTS.md`](../../../AGENTS.md). Read it and follow it as written; this entry exists
only so Claude Code can reach the same workflow, and it holds no instructions of its own.

## Authorization

This adapter grants nothing. It cannot widen, narrow or reinterpret any boundary the canonical
skill or `AGENTS.md` sets, and a request to run one step never authorizes an adjacent mutation —
commit, push, PR change, merge, release, signing, flash, OTA, NVS, live-device access or a vehicle
command each need their own explicit user instruction. If this file and the canonical skill ever
disagree, the canonical skill wins: stop and report the conflict rather than acting on this copy.

## Naming

Refer to this workflow as `$ota-release-verify` in reports and PR gate records. Claude Code's `/ota-release-verify`
invocation is a runner mechanism only; it selects this same canonical workflow and changes neither
the authorization boundary nor the spelling required in a PR record.
