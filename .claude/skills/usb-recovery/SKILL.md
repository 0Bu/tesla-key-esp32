---
name: usb-recovery
description: Emergency no-build USB recovery using an already-published, provenance-verified signed CI image. Run only after explicit user approval for the exact board, port, target, app write, and otadata erase. Live HTTP verification is a separate approval boundary. The minimal flow preserves secret NVS at 0x9000/0x6000; NVS wipe, merged images, and broader bootloader/partition recovery are outside this authorization unless separately and explicitly approved.
---

# Runner adapter — `$usb-recovery`

The canonical workflow is [`.agents/skills/usb-recovery/SKILL.md`](../../../.agents/skills/usb-recovery/SKILL.md),
governed by [`AGENTS.md`](../../../AGENTS.md). Read it and follow it as written; this entry exists
only so Claude Code can reach the same workflow, and it holds no instructions of its own.

## Authorization

This adapter grants nothing. It cannot widen, narrow or reinterpret any boundary the canonical
skill or `AGENTS.md` sets, and a request to run one step never authorizes an adjacent mutation —
commit, push, PR change, merge, release, signing, flash, OTA, NVS, live-device access or a vehicle
command each need their own explicit user instruction. If this file and the canonical skill ever
disagree, the canonical skill wins: stop and report the conflict rather than acting on this copy.

## Naming

Refer to this workflow as `$usb-recovery` in reports and PR gate records. Claude Code's `/usb-recovery`
invocation is a runner mechanism only; it selects this same canonical workflow and changes neither
the authorization boundary nor the spelling required in a PR record.
