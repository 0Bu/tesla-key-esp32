<!--
Fill in each section below. Delete checklist lines that don't apply to this change.
This template is used by human authors and coding agents — keep it short and honest:
record the checks completed locally before the push and note inapplicable or unexercised checks.
-->

## Summary

<!-- What changed and why, in 1–3 sentences. -->

## Changes

<!-- Bullet the concrete edits. -->
-

## Verification

<!-- Name the reviewed baseline, focused/full scope and affected consumers; full audits apply to migrations, target/dependency/trust changes and unknown baselines. -->

<!-- Include the full candidate and base SHAs, the command and result, and any applicable per-target evidence. -->
- [ ] `scripts/check-local.sh --candidate` passed before push (repository, strict host/browser, protocol and Linux sanitizer checks)
- [ ] Added `--firmware` for firmware-relevant changes (pinned four-target build, size/stack, reproducibility and disposable-key signing)
- [ ] Every raised size or stack baseline entry is listed here with its measured delta and cause (copy the update tool's change lines) — N/A when no maximum was raised

## Checklist

- [ ] Docs kept in sync where behavior changed (owning document from `docs/README.md`, including `docs/API.md`, `docs/USER_GUIDE.md`, `docs/DEVELOPMENT.md`, architecture/security/MCP; `AGENTS.md` for runner policy)
- [ ] New hardware-free logic lives in `main/logic/` with a `CHECK` in `test/test_logic.cpp` (see `$add-logic-test`)
- [ ] Heap safety considered — no new large *contiguous* allocations; HTTP handlers stay under the `handle_all` try/catch (see `AGENTS.md`)
- [ ] Target-agnostic — still builds for all four chips (esp32 / esp32s3 / esp32c3 / esp32c6)

## Gates

<!--
These boxes record completed local checks, reviews and catalog sync. GitHub checks the record format
and SHA. After a clean local result, tick the box and enter the full reviewed commit SHA
(`git rev-parse HEAD`). A new commit requires fresh evidence. Review the final PR wording for
hygiene and repeat that check locally if the title or body changes.

$feature-docs only arms when the diff reaches main/, test/, sdkconfig.defaults*, partitions.csv,
the shipped Pages runtime, release-relevance logic, or the build/signed-preview/preview-cleanup/
PR-policy/bench-acceptance workflows. Because `docs/FEATURES.md` catalogs the runner-neutral policy
itself, it also arms for
`AGENTS.md`, `.agents/`, `.codex/`, `.github/PULL_REQUEST_TEMPLATE.md`, `tools/agent-hooks/`,
`tools/agent-config/`, and `scripts/with-host-python.sh` — delete its line only on a docs- or unrelated chore-only PR. A full
$project-review also clears the
skill-audit gate, but NOT the feature-docs one, NOR the pr-hygiene one.

$pr-hygiene is unconditional like $skill-audit and $project-review, and is the only gate that also
re-arms at every push AND at merge — it screens this PR's title/body, its commits, and any touched
docs for personal/private information and non-English content. Neither $project-review nor
$skill-audit being clean establishes it; run $pr-hygiene separately.

$vehicle-command-audit only arms when the diff reaches vehicle-command/BLE paths:
main/command_exec.cpp, main/vehicle_*, main/ble_client.*, main/logic/{command_registry,command_runner,ble_dispatcher,rx_framing,session_state,command_result,command_fault,vcsec_response,key_rotation,ble_chunk,ble_deferred_event,ble_deferred_queue,wake_poll,active_window,telemetry_poll}.hpp,
patches/tesla-ble/, main/idf_component.yml, test/test_tesla_ble_harness.cpp,
scripts/test-tesla-ble-harness.sh, scripts/apply-tesla-ble-patches.{sh,py},
scripts/tesla-ble-patch-state.json, scripts/check-dependency-contract.py,
docs/adr/0005-tesla-ble-seam.md, sdkconfig.defaults*, or
.agents/skills/vehicle-command-audit/ — delete its line only when the diff reaches none of these paths.
-->
- [ ] `$local-checks` passed — pre-push verification @ <full-40-hex-sha>
- [ ] `$skill-audit` clean — PR create/push gate @ <full-40-hex-sha>
- [ ] `$project-review` clean — merge gate @ <full-40-hex-sha>
- [ ] `$pr-hygiene` clean — content gate @ <full-40-hex-sha>
- [ ] `$feature-docs` synced — merge gate @ <full-40-hex-sha>
- [ ] `$vehicle-command-audit` clean — merge gate @ <full-40-hex-sha>
