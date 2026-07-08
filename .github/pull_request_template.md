<!--
Fill in each section below. Delete checklist lines that don't apply to this change.
This template is used by human authors and by Claude Code — keep it short and honest:
say what was actually verified, and note anything that couldn't be (a cloud session
cannot build or USB-flash — see .claude/CLAUDE.md).
-->

## Summary

<!-- What changed and why, in 1–3 sentences. -->

## Changes

<!-- Bullet the concrete edits. -->
-

## Verification

<!-- How this was checked. State what ran and what couldn't (e.g. no build in a cloud session). -->
- [ ] `scripts/run-mock-tests.sh` passes (host-side logic tests — CI's `logic-test` gate)
- [ ] Firmware built (`scripts/idf-docker.sh idf.py build`, or relied on CI) — N/A in a cloud session (no Docker daemon / no USB)

## Checklist

- [ ] Docs kept in sync where behavior changed (`.claude/CLAUDE.md`, `docs/ARCHITECTURE.md`, `README.md`, `docs/SECURITY.md`, `docs/MCP.md`)
- [ ] New hardware-free logic lives in `main/logic/` with a `CHECK` in `test/test_logic.cpp` (see the `add-logic-test` skill)
- [ ] Heap safety considered — no new large *contiguous* allocations; HTTP handlers stay under the `handle_all` try/catch (`.claude/CLAUDE.md` → "Memory is tight")
- [ ] Target-agnostic — still builds for all five chips (esp32 / esp32s3 / esp32c3 / esp32c6 / esp32c5)

## Gates

<!--
These two boxes ARE the publish/merge gates — they replace the old on-disk markers. The
require-skill-audit.sh (create/push) and require-project-review.sh (merge) hooks read them
straight from this PR body. After a CLEAN run, tick the box and replace <sha> with the reviewed
commit (`git rev-parse --short=12 HEAD`). A later commit changes the sha and re-stales the gate,
forcing a fresh run. Do NOT tick a box without actually running the check.
-->
- [ ] `/skill-audit` clean — PR create/push gate @ <sha>
- [ ] `/project-review` clean — merge gate @ <sha>
