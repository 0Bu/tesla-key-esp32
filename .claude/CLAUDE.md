# tesla-key-esp32 — Claude Code adapter

@../AGENTS.md

[`AGENTS.md`](../AGENTS.md) above is the canonical, runner-neutral instruction set: it is imported
here because Claude Code loads `CLAUDE.md`, not `AGENTS.md`. This file adds no policy of its own and
never grants authority that `AGENTS.md` withholds. Where the two could be read differently,
`AGENTS.md` wins.

## Authorization boundary (restated so it survives a failed import)

Analysis, review, audit, diagnosis and triage are **read-only**. An implementation request permits
scoped repository edits and local verification only; it does not authorize commit, push, PR
creation or update, merge, release, Pages publication, signing with the real key, flash, OTA, NVS
mutation, live-device access, vehicle wake or any vehicle command. Each of those needs its own
explicit user instruction.

NVS is secret material (WiFi, VIN, MQTT, vehicle private key, BLE sessions) at offset `0x9000`,
size `0x6000`; partition and OTA-slot offsets do not move. Locally built unsigned images are not
flashable. The real `OTA_SIGNING_KEY` never reaches PR code, hooks or agents. A live read can wake
the vehicle.

## What this adapter maps

| Claude Code surface | Canonical source |
|---|---|
| `.claude/settings.json` hooks | [`tools/agent-hooks/`](../tools/agent-hooks/), same commands as [`.codex/hooks.json`](../.codex/hooks.json) |
| `.claude/skills/<name>/SKILL.md` | [`.agents/skills/<name>/SKILL.md`](../.agents/skills/) |
| `.claude/agents/*.md` | the read-only reviewers in [`.codex/agents/`](../.codex/agents/) |

Reference skills as `$skill-name` in PR records and reports. Claude Code's `/skill-name`
invocation is a runner mechanism only: it selects the same canonical workflow and changes no
authorization or record spelling.

## Environment note (cloud / remote sandbox)

A cloud session cannot build (no Docker daemon for `scripts/idf-docker.sh`) and cannot USB-flash
(no USB passthrough): it is for editing, review and CI-driven builds. The SessionStart capability
hook reports what the current environment actually supports — report capabilities rather than
manufacturing evidence.
