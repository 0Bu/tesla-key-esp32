---
name: add-logic-test
description: Scaffold a new hardware-free pure-logic unit in main/logic/ and its CHECK cases in test/test_logic.cpp, so a decision/conversion the firmware makes is verified by the host-side mock build (the local "run it and see" loop that CI gates on). Use when extracting testable logic out of vehicle_ctrl/http_server/mqtt_ha/ota, adding a CHECK to the mock suite, or when asked to "add a logic test", "make this testable", "cover this in the mock build", or "add a pure-logic header".
---

# Legacy compatibility adapter — $add-logic-test

This [.claude](../../) entry remains active during the canary period. The canonical project
workflow is [`$add-logic-test`](../../../.agents/skills/add-logic-test/SKILL.md), governed by
[`AGENTS.md`](../../../AGENTS.md). Canonical skills live under [`.agents/skills/`](../../../.agents/skills/),
and both runner adapters delegate lifecycle and PR policy to
[`tools/agent-hooks/`](../../../tools/agent-hooks/).

## Authorization boundary

Use only for an explicitly requested implementation scope. It may edit the requested logic/tests, but it never infers commit, push, PR, merge, release, signing, flash, OTA, device, or vehicle authority.

The legacy adapter grants no broader permissions than `AGENTS.md` or the canonical skill. A
request to review, diagnose, build, approve, or run one step does not authorize adjacent mutations.
Only separate explicit user authorization may widen the scope.
Stop and report any conflict instead of falling back to historical Claude-only behavior.

## Delegation

Follow the canonical [`$add-logic-test`](../../../.agents/skills/add-logic-test/SKILL.md) instructions. Use
`$add-logic-test` for new invocations and records. The legacy `/add-logic-test` spelling may be accepted only
for existing canary-era PR records; it is not canonical and does not change authorization.
