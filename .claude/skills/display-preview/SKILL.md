---
name: display-preview
description: Render the on-device ST7735 display simulator to PNG state montages so a human can eyeball "does it look right?" after touching the presenter/renderer. Use when asked to "preview the display", "render the display states", "show the display", generate the display montage/simulator images, check the on-device panel layout, or after editing main/logic/display_model.hpp or main/display.cpp (landscape 160x80 / portrait 80x160, SoC gradient, RSSI bars, SSID scroll, WiFi/BLE search + Pairing animation). Read-only, python3 stdlib only — no board, no ESP-IDF, no Docker. This is the VISUAL check that complements the automated presenter↔sim parity gate.
---

# Runner adapter — `$display-preview`

The canonical workflow is [`.agents/skills/display-preview/SKILL.md`](../../../.agents/skills/display-preview/SKILL.md),
governed by [`AGENTS.md`](../../../AGENTS.md). Read it and follow it as written; this entry exists
only so Claude Code can reach the same workflow, and it holds no instructions of its own.

## Authorization

This adapter grants nothing. It cannot widen, narrow or reinterpret any boundary the canonical
skill or `AGENTS.md` sets, and a request to run one step never authorizes an adjacent mutation —
commit, push, PR change, merge, release, signing, flash, OTA, NVS, live-device access or a vehicle
command each need their own explicit user instruction. If this file and the canonical skill ever
disagree, the canonical skill wins: stop and report the conflict rather than acting on this copy.

## Naming

Refer to this workflow as `$display-preview` in reports and PR gate records. Claude Code's `/display-preview`
invocation is a runner mechanism only; it selects this same canonical workflow and changes neither
the authorization boundary nor the spelling required in a PR record.
