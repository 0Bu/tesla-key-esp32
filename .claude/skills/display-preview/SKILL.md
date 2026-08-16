---
name: display-preview
description: Render the on-device ST7735 display simulator to PNG state montages so a human can eyeball "does it look right?" after touching the presenter/renderer. Use when asked to "preview the display", "render the display states", "show the display", generate the display montage/simulator images, check the on-device panel layout, or after editing main/logic/display_model.hpp or main/display.cpp (landscape 160x80 / portrait 80x160, SoC gradient, RSSI bars, SSID scroll, WiFi/BLE search + Pairing animation). Read-only, python3 stdlib only — no board, no ESP-IDF, no Docker. This is the VISUAL check that complements the automated presenter↔sim parity gate.
---

# Legacy compatibility adapter — $display-preview

This [.claude](../../) entry remains active during the canary period. The canonical project
workflow is [`$display-preview`](../../../.agents/skills/display-preview/SKILL.md), governed by
[`AGENTS.md`](../../../AGENTS.md). Canonical skills live under [`.agents/skills/`](../../../.agents/skills/),
and both runner adapters delegate lifecycle and PR policy to
[`tools/agent-hooks/`](../../../tools/agent-hooks/).

## Authorization boundary

Render-only by default and no hardware access. Do not regenerate source assets with mutating simulator modes unless the user explicitly requested that source change; preview work never grants flash or publication authority.

The legacy adapter grants no broader permissions than `AGENTS.md` or the canonical skill. A
request to review, diagnose, build, approve, or run one step does not authorize adjacent mutations.
Only separate explicit user authorization may widen the scope.
Stop and report any conflict instead of falling back to historical Claude-only behavior.

## Delegation

Follow the canonical [`$display-preview`](../../../.agents/skills/display-preview/SKILL.md) instructions. Use
`$display-preview` for new invocations and records. The legacy `/display-preview` spelling may be accepted only
for existing canary-era PR records; it is not canonical and does not change authorization.
