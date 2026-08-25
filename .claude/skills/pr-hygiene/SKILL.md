---
name: pr-hygiene
description: Read-only screen of this PR's commits, title/body and touched documentation for personal or private information (LAN IP addresses, MAC addresses, vehicle VINs, WiFi network names, hostnames, phone numbers, emails) and for content not written in English. Reports findings and the exact gate record only; never edits a commit, a file or a PR body, and never commits, pushes, merges, releases, flashes, OTAs, or contacts a live device/vehicle unless the user separately authorizes implementation.
---

# Legacy compatibility adapter — $pr-hygiene

This [.claude](../../) entry remains active during the canary period. The canonical project
workflow is [`$pr-hygiene`](../../../.agents/skills/pr-hygiene/SKILL.md), governed by
[`AGENTS.md`](../../../AGENTS.md). Canonical skills live under [`.agents/skills/`](../../../.agents/skills/),
and both runner adapters delegate lifecycle and PR policy to
[`tools/agent-hooks/`](../../../tools/agent-hooks/).

## Authorization boundary

Strictly read-only and report-only. Screen commits, PR title/body and touched documentation for
personal/private information and non-English content; never edit them or stamp a PR body.
Publication, merge, release, hardware, OTA, and vehicle actions require separate explicit
authority.

The legacy adapter grants no broader permissions than `AGENTS.md` or the canonical skill. A
request to review, diagnose, build, approve, or run one step does not authorize adjacent mutations.
Only separate explicit user authorization may widen the scope.
Stop and report any conflict instead of falling back to historical Claude-only behavior.

## Delegation

Follow the canonical [`$pr-hygiene`](../../../.agents/skills/pr-hygiene/SKILL.md) instructions. Use
`$pr-hygiene` for new invocations and records. The legacy `/pr-hygiene` spelling may be accepted only
for existing canary-era PR records; it is not canonical and does not change authorization.
