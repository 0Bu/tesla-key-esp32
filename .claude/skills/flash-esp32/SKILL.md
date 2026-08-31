---
name: flash-esp32
description: Build tesla-key-esp32 firmware or, only with explicit user approval for the identified board and target, USB-flash a verified signed image (ESP32 / S3 / C3 / C6). Local builds are compile-only until an explicit development signing key is supplied; ordinary unsigned PR artifacts are never flash sources. Every flash preserves NVS and requires unambiguous port/chip verification.
---

# Runner adapter — `$flash-esp32`

The canonical workflow is [`.agents/skills/flash-esp32/SKILL.md`](../../../.agents/skills/flash-esp32/SKILL.md),
governed by [`AGENTS.md`](../../../AGENTS.md). Read it and follow it as written; this entry exists
only so Claude Code can reach the same workflow, and it holds no instructions of its own.

## Authorization

This adapter grants nothing. It cannot widen, narrow or reinterpret any boundary the canonical
skill or `AGENTS.md` sets, and a request to run one step never authorizes an adjacent mutation —
commit, push, PR change, merge, release, signing, flash, OTA, NVS, live-device access or a vehicle
command each need their own explicit user instruction. If this file and the canonical skill ever
disagree, the canonical skill wins: stop and report the conflict rather than acting on this copy.

## Naming

Refer to this workflow as `$flash-esp32` in reports and PR gate records. Claude Code's `/flash-esp32`
invocation is a runner mechanism only; it selects this same canonical workflow and changes neither
the authorization boundary nor the spelling required in a PR record.
