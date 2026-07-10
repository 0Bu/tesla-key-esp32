---
name: display-preview
description: Render the on-device ST7735 display simulator to PNG state montages so a human can eyeball "does it look right?" after touching the presenter/renderer. Use when asked to "preview the display", "render the display states", "show the display", generate the display montage/simulator images, check the on-device panel layout, or after editing main/logic/display_model.hpp or main/display.cpp (landscape 160x80 / portrait 80x160, SoC gradient, RSSI bars, SSID scroll, WiFi/BLE search + Pairing animation). Read-only, python3 stdlib only — no board, no ESP-IDF, no Docker. This is the VISUAL check that complements the automated presenter↔sim parity gate.
---

# display-preview — render the display simulator for a human eyeball pass

[`tools/display_sim.py`](../../../tools/display_sim.py) is the layout **simulator** and the 5x7
**font source of truth** for the on-device ST7735 panel (LilyGo T-Dongle-C5 + T-Dongle-S3). Its
`compose()` / `compose_portrait()` draw the screen pixel-for-pixel the way the firmware's
`draw_landscape()` / `draw_portrait()` do in [`main/display.cpp`](../../../main/display.cpp),
reading the same "what to show" decision from the pure presenter
[`main/logic/display_model.hpp`](../../../main/logic/display_model.hpp) (fed
[`main/logic/ui_state.hpp`](../../../main/logic/ui_state.hpp)). This skill runs the sim's render
modes to produce **PNGs a human can look at** — the complement to the automated correctness gate.

It needs only **python3 (stdlib — no Pillow, no board, no ESP-IDF, no Docker)**, so it works in a
cloud session as well as on a host. It writes image files; it changes no source.

## When to reach for it

Any time you touch the presenter or the renderer and want to confirm the pixels still look right:

- editing [`main/logic/display_model.hpp`](../../../main/logic/display_model.hpp) — the priority
  ladder (WiFi search > pairing > BLE search > battery), the SoC red→green gradient, RSSI→bars,
  or the SSID-scroll geometry (the `Orient` axis picks landscape vs portrait);
- editing [`main/display.cpp`](../../../main/display.cpp) — `draw_landscape` / `draw_portrait`,
  the battery shell, the charging bolt, the "ASLEEP" / search / "Pairing…" frames;
- **especially the PORTRAIT 80x160 layout**, whose MADCTL/offsets are **not yet HW-confirmed** —
  the sim is the only cheap way to sanity-check that geometry before flashing.

> **Parity green ≠ "looks right".** The automated gate proves the sim *decides* the same thing the
> C++ presenter decides; it says nothing about whether the resulting frame is legible or well
> laid-out. That judgement is exactly what this eyeball pass is for — render, then look.

## Scope boundary — what this is NOT

| Tool | Job |
|------|-----|
| **this skill** | Render the sim to PNGs for **human visual review**. Does not replace the parity gate. |
| [`scripts/check-display-sim-parity.sh`](../../../scripts/check-display-sim-parity.sh) | The **automated correctness gate**: compiles a golden dumper from the C++ presenter (`tk::display::compose` in `display_model.hpp`), has the sim re-`decide()` the same inputs, and diffs them. Runs at the end of [`scripts/run-mock-tests.sh`](../../../scripts/run-mock-tests.sh) and in CI's `logic-test` job. |
| [`../add-logic-test/SKILL.md`](../add-logic-test/SKILL.md) | Adds the pure-logic units + `CHECK` cases (the presenter *decisions* are host-tested there). |

## Render the state montages

Run from the repo root. Each mode takes an optional output path; the defaults below match the
built-in ones (all under `tools/`).

```bash
# LANDSCAPE 160x80 — montage of every state (charging, SoC red/amber/green, 100%,
# asleep-dim, BLE search, WiFi search, pairing)
python3 tools/display_sim.py states tools/display_states.png

# PORTRAIT 80x160 — the BOOT-rotated layout (vertical battery) in every state
python3 tools/display_sim.py states-portrait tools/display_states_portrait.png

# WiFi + BLE "searching" sweep, frame-by-frame (the ping-pong highlight)
python3 tools/display_sim.py search tools/display_search.png

# too-long SSID at several marquee offsets — verify horizontal scroll + clipping
python3 tools/display_sim.py scroll tools/display_scroll.png

# single charging "hero" preview frame (Jupiter / 64% / charging)
python3 tools/display_sim.py png tools/display_preview.png
```

Every mode prints `wrote <path> (<w>x<h> …)` on success.

**Then look at the output.** Read/attach the PNGs (`Read tools/display_states.png`, etc.) and
eyeball the layout: header bars + SSID, the battery fill colour across the SoC range, the bolt,
the "ASLEEP" caption, the search/pairing frames, and — for the portrait montage — the vertical
battery geometry.

## After rendering, confirm parity is still green

Rendering only shows what the *sim* draws. Confirm the sim still mirrors the C++ presenter 1:1:

```bash
scripts/run-mock-tests.sh          # runs the logic units AND check-display-sim-parity.sh at the end
# …or just the parity gate on its own:
scripts/check-display-sim-parity.sh
```

A red parity result means the sim and the firmware presenter have drifted — fix that before
trusting the PNGs, because they're then rendering a decision the device won't make.

## Two modes that are NOT preview — don't run them for a look

> ⚠️ **`cheader` rewrites a source file.** `python3 tools/display_sim.py cheader main/display_font.h`
> regenerates [`main/display_font.h`](../../../main/display_font.h) (the packed 5x7 font + the
> Bluetooth/bolt bitmaps). Run it **only when you intentionally changed a glyph** in the `G`/`BT_ROWS`/
> `BOLT_ROWS` tables and mean to regenerate the font — never as part of a preview. Review the diff.

> ⚠️ **`parity <golden.tsv>` is the gate's re-decide step, not a rendering mode.** It's invoked by
> [`check-display-sim-parity.sh`](../../../scripts/check-display-sim-parity.sh) against the golden
> TSV the C++ dumper emits. Use the wrapper script above; don't call `parity` by hand to "preview".

## Notes

- The display code only compiles in under `CONFIG_TESLA_DISPLAY_ENABLED`
  (`sdkconfig.defaults.esp32c5` + `.esp32s3`); the sim renders the same layout with no build at all.
- The presenter is **cache-only** (never wakes the car), so nothing here touches the vehicle — it's
  pure layout math over a `UiSnapshot`.
- To see the rendered frames on an actual panel instead, build + flash a C5/S3 board with the
  [`../flash-esp32/SKILL.md`](../flash-esp32/SKILL.md) skill (each BOOT tap rotates 90° through the
  four orientations).
