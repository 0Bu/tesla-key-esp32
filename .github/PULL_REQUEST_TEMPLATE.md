<!--
Fill in each section below. Delete checklist lines that don't apply to this change.
This template is used by human authors and coding agents — keep it short and honest:
say what was actually verified, and note anything that couldn't be (a cloud session
cannot build or USB-flash — see AGENTS.md).
-->

## Summary

<!-- What changed and why, in 1–3 sentences. -->

## Changes

<!-- Bullet the concrete edits. -->
-

## Verification

<!-- How this was checked. State what ran and what couldn't (e.g. no build in a cloud session). -->
- [ ] `scripts/run-mock-tests.sh` passes (host-side logic tests — CI's `logic-test` gate)
- [ ] `tools/agent-config/selftest.sh` passes when agent config, skills, hooks, or compatibility files changed
- [ ] Firmware built (`scripts/idf-docker.sh idf.py build`, or relied on CI) — N/A in a cloud session (no Docker daemon / no USB)

## Checklist

- [ ] Docs kept in sync where behavior changed (`AGENTS.md`, `docs/ARCHITECTURE.md`, `README.md`, `docs/SECURITY.md`, `docs/MCP.md`)
- [ ] New hardware-free logic lives in `main/logic/` with a `CHECK` in `test/test_logic.cpp` (see `$add-logic-test`)
- [ ] Heap safety considered — no new large *contiguous* allocations; HTTP handlers stay under the `handle_all` try/catch (see `AGENTS.md`)
- [ ] Target-agnostic — still builds for all four chips (esp32 / esp32s3 / esp32c3 / esp32c6)

## Gates

<!--
These four boxes ARE the publish/merge gates — they replace the old on-disk markers. The
runner-neutral gate under tools/agent-hooks/ reads them straight from this PR body, each matching
its OWN box. After a CLEAN run, tick the box and replace <sha> with the reviewed commit
(`git rev-parse --short=12 HEAD`). A later commit changes the sha and re-stales the gate, forcing
a fresh run. Do NOT tick a box without actually running the check.

$feature-docs only arms when the diff reaches main/, test/, sdkconfig.defaults*, partitions.csv,
the shipped Pages runtime, release-relevance logic, or the build/signed-preview/preview-cleanup
workflows. Because `docs/FEATURES.md` catalogs the runner-neutral policy itself, it also arms for
`AGENTS.md`, `.agents/`, `.codex/`, `tools/agent-hooks/`, and `tools/agent-config/` — delete its
line only on a docs- or unrelated chore-only PR. A full $project-review also clears the
skill-audit gate, but NOT the feature-docs one, NOR the pr-hygiene one.

$pr-hygiene is unconditional like $skill-audit and $project-review, and is the only gate that also
re-arms at every push AND at merge — it screens this PR's title/body, its commits, and any touched
docs for personal/private information and non-English content. Neither $project-review nor
$skill-audit being clean establishes it; run $pr-hygiene separately.
-->
- [ ] `$skill-audit` clean — PR create/push gate @ <sha>
- [ ] `$project-review` clean — merge gate @ <sha>
- [ ] `$pr-hygiene` clean — content gate @ <sha>
- [ ] `$feature-docs` synced — merge gate @ <sha>
