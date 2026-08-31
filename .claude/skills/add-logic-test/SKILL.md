---
name: add-logic-test
description: Scaffold a new hardware-free pure-logic unit in main/logic/ and its CHECK cases in test/test_logic.cpp, so a decision/conversion the firmware makes is verified by the host-side mock build (the local "run it and see" loop that CI gates on). Use when extracting testable logic out of vehicle_ctrl/http_server/mqtt_ha/ota, adding a CHECK to the mock suite, or when asked to "add a logic test", "make this testable", "cover this in the mock build", or "add a pure-logic header".
---

# Runner adapter — `$add-logic-test`

The canonical workflow is [`.agents/skills/add-logic-test/SKILL.md`](../../../.agents/skills/add-logic-test/SKILL.md),
governed by [`AGENTS.md`](../../../AGENTS.md). Read it and follow it as written; this entry exists
only so Claude Code can reach the same workflow, and it holds no instructions of its own.

## Authorization

This adapter grants nothing. It cannot widen, narrow or reinterpret any boundary the canonical
skill or `AGENTS.md` sets, and a request to run one step never authorizes an adjacent mutation —
commit, push, PR change, merge, release, signing, flash, OTA, NVS, live-device access or a vehicle
command each need their own explicit user instruction. If this file and the canonical skill ever
disagree, the canonical skill wins: stop and report the conflict rather than acting on this copy.

## Naming

Refer to this workflow as `$add-logic-test` in reports and PR gate records. Claude Code's `/add-logic-test`
invocation is a runner mechanism only; it selects this same canonical workflow and changes neither
the authorization boundary nor the spelling required in a PR record.
