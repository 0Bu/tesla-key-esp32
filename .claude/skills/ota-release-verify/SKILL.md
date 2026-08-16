---
name: ota-release-verify
description: "Read-only verification of the already-published OTA channel against the latest GitHub Release: bind manifest.sourceSha to the Release tag commit, hash-check all 16 manifest parts, and verify every app's embedded version and chip family without publishing, releasing, flashing, OTA, or local ref mutation. Optional live-board GETs require separate explicit user approval."
---

# Legacy compatibility adapter — $ota-release-verify

This [.claude](../../) entry remains active during the canary period. The canonical project
workflow is [`$ota-release-verify`](../../../.agents/skills/ota-release-verify/SKILL.md), governed by
[`AGENTS.md`](../../../AGENTS.md). Canonical skills live under [`.agents/skills/`](../../../.agents/skills/),
and both runner adapters delegate lifecycle and PR policy to
[`tools/agent-hooks/`](../../../tools/agent-hooks/).

## Authorization boundary

Public-channel verification is read-only and does not publish, release, update local refs, flash, or start OTA. Any optional live-board GET requires separate explicit user approval; never send POST /ota/update from this skill.

The legacy adapter grants no broader permissions than `AGENTS.md` or the canonical skill. A
request to review, diagnose, build, approve, or run one step does not authorize adjacent mutations.
Only separate explicit user authorization may widen the scope.
Stop and report any conflict instead of falling back to historical Claude-only behavior.

## Delegation

Follow the canonical [`$ota-release-verify`](../../../.agents/skills/ota-release-verify/SKILL.md) instructions. Use
`$ota-release-verify` for new invocations and records. The legacy `/ota-release-verify` spelling may be accepted only
for existing canary-era PR records; it is not canonical and does not change authorization.
