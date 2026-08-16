---
name: usb-recovery
description: Emergency no-build USB recovery using an already-published, provenance-verified signed CI image. Run only after explicit user approval for the exact board, port, target, app write, and otadata erase. The minimal flow preserves secret NVS at 0x9000/0x6000; NVS wipe, merged images, and broader bootloader/partition recovery are outside this authorization unless separately and explicitly approved.
disable-model-invocation: true
---

# Legacy compatibility adapter — $usb-recovery

This [.claude](../../) entry remains active during the canary period. The canonical project
workflow is [`$usb-recovery`](../../../.agents/skills/usb-recovery/SKILL.md), governed by
[`AGENTS.md`](../../../AGENTS.md). Canonical skills live under [`.agents/skills/`](../../../.agents/skills/),
and both runner adapters delegate lifecycle and PR policy to
[`tools/agent-hooks/`](../../../tools/agent-hooks/).

## Authorization boundary

Destructive hardware workflow: require explicit user approval for the exact board/port/target and minimal app plus otadata writes. Never write, erase, dump, print, archive, or upload secret NVS at 0x9000/0x6000. Broader bootloader/partition recovery needs a second explicit approval; NVS wipe is outside this project skill.

The legacy adapter grants no broader permissions than `AGENTS.md` or the canonical skill. A
request to review, diagnose, build, approve, or run one step does not authorize adjacent mutations.
Only separate explicit user authorization may widen the scope.
Stop and report any conflict instead of falling back to historical Claude-only behavior.

## Delegation

Follow the canonical [`$usb-recovery`](../../../.agents/skills/usb-recovery/SKILL.md) instructions. Use
`$usb-recovery` for new invocations and records. The legacy `/usb-recovery` spelling may be accepted only
for existing canary-era PR records; it is not canonical and does not change authorization.
