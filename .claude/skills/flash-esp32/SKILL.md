---
name: flash-esp32
description: Build tesla-key-esp32 firmware or, only with explicit user approval for the identified board and target, USB-flash a verified signed image (ESP32 / S3 / C3 / C6). Local builds are compile-only until an explicit development signing key is supplied; ordinary unsigned PR artifacts are never flash sources. Every flash preserves NVS and requires unambiguous port/chip verification.
---

# Legacy compatibility adapter — $flash-esp32

This [.claude](../../) entry remains active during the canary period. The canonical project
workflow is [`$flash-esp32`](../../../.agents/skills/flash-esp32/SKILL.md), governed by
[`AGENTS.md`](../../../AGENTS.md). Canonical skills live under [`.agents/skills/`](../../../.agents/skills/),
and both runner adapters delegate lifecycle and PR policy to
[`tools/agent-hooks/`](../../../tools/agent-hooks/).

## Authorization boundary

A build request authorizes no USB write. Flash only after explicit user approval for the exact board, port, chip, target, signed image, app write, and otadata erase. Never use unsigned/merged images, access the real OTA key, or touch secret NVS at 0x9000/0x6000.

The legacy adapter grants no broader permissions than `AGENTS.md` or the canonical skill. A
request to review, diagnose, build, approve, or run one step does not authorize adjacent mutations.
Only separate explicit user authorization may widen the scope.
Stop and report any conflict instead of falling back to historical Claude-only behavior.

## Delegation

Follow the canonical [`$flash-esp32`](../../../.agents/skills/flash-esp32/SKILL.md) instructions. Use
`$flash-esp32` for new invocations and records. The legacy `/flash-esp32` spelling may be accepted only
for existing canary-era PR records; it is not canonical and does not change authorization.
