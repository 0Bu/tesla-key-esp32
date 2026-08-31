---
name: pr-hygiene
description: Read-only screen of this PR's commits, title/body and touched documentation for personal or private information (LAN IP addresses, MAC addresses, vehicle VINs, WiFi network names, hostnames, phone numbers, emails, private session/transcript links) and for content not written in English. Reports findings and the exact gate record only; never edits a commit, a file or a PR body, and never commits, pushes, merges, releases, flashes, OTAs, or contacts a live device/vehicle unless the user separately authorizes implementation.
---

# Runner adapter — `$pr-hygiene`

The canonical workflow is [`.agents/skills/pr-hygiene/SKILL.md`](../../../.agents/skills/pr-hygiene/SKILL.md),
governed by [`AGENTS.md`](../../../AGENTS.md). Read it and follow it as written; this entry exists
only so Claude Code can reach the same workflow, and it holds no instructions of its own.

## Authorization

This adapter grants nothing. It cannot widen, narrow or reinterpret any boundary the canonical
skill or `AGENTS.md` sets, and a request to run one step never authorizes an adjacent mutation —
commit, push, PR change, merge, release, signing, flash, OTA, NVS, live-device access or a vehicle
command each need their own explicit user instruction. If this file and the canonical skill ever
disagree, the canonical skill wins: stop and report the conflict rather than acting on this copy.

## Naming

Refer to this workflow as `$pr-hygiene` in reports and PR gate records. Claude Code's `/pr-hygiene`
invocation is a runner mechanism only; it selects this same canonical workflow and changes neither
the authorization boundary nor the spelling required in a PR record.
