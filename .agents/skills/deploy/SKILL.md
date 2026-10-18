---
name: deploy
description: Orchestrates the full delivery lifecycle for tesla-key-esp32 from local workspace analysis and fix loops, commit, push, and PR creation, gate verification and CI monitoring, canonical squash merge and channel-bound main-run tracking, to OTA flashing on tesla-key-esp32.local, 3-tiered testing (targeted, comprehensive, e2e), and workspace cleanup. Requires explicit user authorization before execution.
---

Invoke `$deploy`. Read [AGENTS.md](../../../AGENTS.md) first; this workflow grants no
additional authorization. Canonical skills live under `.agents/skills/`; hooks enforce local policy.

# deploy — End-to-End Release & Delivery Lifecycle

This skill orchestrates the complete, fail-closed delivery pipeline for `tesla-key-esp32`. It takes
working tree changes through local verification, PR creation, CI monitoring, canonical squash merge,
post-merge main-run and channel verification, OTA update on the bench/target board (`tesla-key-esp32.local`),
multi-tier testing (targeted, comprehensive, and cluster evcc E2E), and branch cleanup.

> **Authorization boundary.** Invoking this skill requires explicit user authorization for the entire
> delivery lifecycle: local fix loops, commit, push, PR creation, canonical squash merge, the
> signing/publication side effect of the main build (the Dev channel), OTA deployment to the identified
> device, and post-deployment live verification. A stable Release additionally needs its own explicit
> authorization for a manual `workflow_dispatch` run with `release: true`; this skill never starts it. If unrecoverable errors occur at any gate, stop and
> report immediately (fail-closed).

---

## Phase 1: Local Analysis & Findings Fix Loop

Before committing or pushing, run the full local contract of
[AGENTS.md](../../../AGENTS.md#implementation-and-evidence), narrowest relevant checks first:

1. **Whitespace & Conflict Marker Check**:
   ```bash
   git diff --check
   ```
2. **Offline Repository Linter**:
   ```bash
   ./scripts/repo-lint.sh
   ```
3. **Host Mock, Unit & Sanitizer Suites**:
   ```bash
   ./scripts/run-mock-tests.sh
   ./scripts/run-mock-tests.sh --require-all
   ./scripts/run-sanitizer-tests.sh
   ```
4. **Build Contracts & Partition Checks**:
   ```bash
   ./scripts/test-build-contracts.sh
   ```
5. **PR Gate Canaries & Agent Configuration Selftest**:
   ```bash
   ./scripts/test-pr-gates.sh
   ./tools/agent-config/selftest.sh
   ```
6. **Four-Target Firmware Contract**:
   Before handoff, run the full contract with the pinned container. For firmware-relevant changes
   (`gate_firmware_size_relevant` in [`pr-gate-lib.sh`](../../../tools/agent-hooks/pr-gate-lib.sh)),
   measure size and stack first ([size rule](../../rules/firmware-size-budget.md)):
   ```bash
   ./scripts/idf-docker.sh ./scripts/ci-build-verify.sh "$(cat version.txt)" "$(git rev-parse HEAD)"
   ```
   If Docker is unavailable, stop and report the firmware contract as **not run**; never skip it
   silently.

**Fix Loop**: If any check fails, analyze the root cause, apply the fix to code or tests, and re-run Phase 1 until all checks pass cleanly.

---

## Phase 2: Commit, Push & PR Creation

1. **Pre-PR Screen (`$skill-audit` & `$pr-hygiene`)**:
   - Run `$skill-audit` locally to ensure all canonical skills and subagent manifests match repository conventions.
   - Run `$pr-hygiene` screen:
     - Ensure all commit messages, PR titles, and PR descriptions are strictly written in **English**.
     - Verify that no private IP addresses (use RFC 5737/3849 documentation addresses if needed), real MAC addresses, or real vehicle VINs are committed.
2. **Commit Changes**:
   Stage only the relevant files and commit using conventional commit format:
   ```bash
   git add <modified-files>
   git commit -m "feat(<scope>): <concise description in English>"
   ```
3. **Push Feature Branch**:
   Push using the literal branch name without quotes or variable expansion (guards reject dynamic/quoted pushes):
   ```bash
   git push -u origin <branch-name>
   ```
4. **Create PR with Canonical Gate Checkboxes**:
   Determine the current HEAD SHA via `HEAD_SHA=$(git rev-parse HEAD)`.
   Generate the PR body including the 5 canonical gate checkboxes. Note that `$skill-audit` and `$pr-hygiene` must actually be executed and pass cleanly for the current commit prior to PR creation (do not pre-tick without running):
   ```markdown
   ## Summary
   <Concise description of changes in English>

   ## PR Gates
   - [x] `$skill-audit` clean — PR create/push gate @ <FULL_40_HEX_HEAD_SHA>
   - [ ] `$project-review` clean — merge gate @ <full-40-hex-sha>
   - [x] `$pr-hygiene` clean — content gate @ <FULL_40_HEX_HEAD_SHA>
   - [ ] `$feature-docs` synced — merge gate @ <full-40-hex-sha>
   - [ ] `$vehicle-command-audit` clean — merge gate @ <full-40-hex-sha>
   ```
   Submit the pull request:
   ```bash
   gh pr create --body-file <PATH_TO_BODY> --title "<Title in English>"
   PR=$(gh pr view --json number -q .number)
   ```

---

## Phase 3: Verification, Checklist & Gates Loop

1. **Monitor PR Checks on CI**:
   ```bash
   gh pr checks "$PR" --watch
   ```
   *(Alternatively, invoke `$ci-heal` via `scripts/ci-heal.sh --pr "$PR"` to monitor CI and reproduce failures locally. It never commits and stamps a gate only from an explicit `--attest <gate>=<evidence>` for a completed independent review of the exact head.)*
2. **Audit & Stamp PR Gates**:
   Run the respective audit skills (`$project-review`, `$feature-docs`, `$vehicle-command-audit`) against the current HEAD.
   Once each audit passes cleanly, stamp the verified gates on the PR. The evidence token names the
   completed review (report reference and finding count), never a mere syntax check: a passing
   `tools/agent-config/check.mjs` or a green CI run is not a `$skill-audit` or `$project-review`.
   ```bash
   HEAD=$(git rev-parse HEAD)
   ./scripts/stamp-pr-gates.sh --update-pr "$PR" --head "$HEAD" \
     --gate skill-audit="<report reference>, 0 findings @ $HEAD" \
     --gate pr-hygiene="<report reference>, clean @ $HEAD" \
     --gate project-review="<report reference>, 0 findings @ $HEAD" \
     --gate feature-docs="<report reference>, docs/FEATURES.md synced @ $HEAD" \
     --gate vehicle-command-audit="<report reference>, harness clean @ $HEAD"
   ```
   (Only include conditional gates `--gate feature-docs=...` or `--gate vehicle-command-audit=...` if relevant to the changed paths.)
3. **Fix & Re-Check Loop**:
   - If any CI job fails, inspect the failure logs:
     ```bash
     gh run view <RUN_ID> --log-failed | tail -40
     ```
   - Implement the necessary fix locally.
   - Re-run all Phase 1 checks locally.
   - Commit the fix locally:
     ```bash
     git add <modified-files>
     git commit -m "fix: resolve CI failure"
     NEW_HEAD=$(git rev-parse HEAD)
     ```
   - Re-run `$skill-audit` and `$pr-hygiene` against the new commit before updating PR gates for the push:
     Run `$skill-audit` and `$pr-hygiene` locally to verify they pass cleanly (do not stamp without running). The pre-push hook requires current `$skill-audit` and `$pr-hygiene` records on the PR before allowing a push to an open PR. Stamp them first:
     ```bash
     ./scripts/stamp-pr-gates.sh --update-pr "$PR" --head "$NEW_HEAD" \
       --gate skill-audit="<report reference>, 0 findings @ $NEW_HEAD" \
       --gate pr-hygiene="<report reference>, clean @ $NEW_HEAD"
     ```
   - Push the fix:
     ```bash
     git push -u origin <branch-name>
     ```
   - Re-audit and stamp the merge gates for the new HEAD commit:
     Re-run the relevant audit skills (`$skill-audit`, `$pr-hygiene`, `$project-review`, plus any applicable conditional audit skills `$feature-docs` or `$vehicle-command-audit`) against `$NEW_HEAD`. Only after each audit passes cleanly, re-stamp all gates on the PR:
     ```bash
     ./scripts/stamp-pr-gates.sh --update-pr "$PR" --head "$NEW_HEAD" \
       --gate skill-audit="<report reference>, 0 findings @ $NEW_HEAD" \
       --gate pr-hygiene="<report reference>, clean @ $NEW_HEAD" \
       --gate project-review="<report reference>, 0 findings @ $NEW_HEAD"
     ```
     (Include conditional `--gate feature-docs=...` or `--gate vehicle-command-audit=...` if relevant.)
   - Repeat until all PR checks are green and all gates are satisfied.

---

## Phase 4: Merge and OTA delivery

Read and execute the complete [ship procedure](../ship/SKILL.md) as a **required** part of this phase,
using the already authorized lifecycle, exact PR head, identified board and explicit OTA/live scopes.
Its merge gate, canonical squash command, exact MERGE_SHA main-run/channel/artifact selection,
metadata/signature validation and bounded OTA probation monitor all apply unchanged. Stable Release
requires separate manual-dispatch authorization; never substitute latest Release for the main run.

Stop on any gate/provenance/channel/version/download/reboot failure. Require at least 100 s of both
monotonic uptime and wall-clock progress from the first post-OTA identity baseline, no hidden reboot,
and boot-local mark-valid success. Then continue the three-tier acceptance below. Do not duplicate
or shorten `$ship`'s verification code here.

## Phase 5: 3-Tiered Verification

### 5a. Test of the Latest Implementation (Targeted Verification)
Run tests tailored specifically to the code changed in the PR:
- If pure logic in `main/logic/` changed: execute the matching mock tests in `test/test_logic.cpp`.
- If an HTTP or REST API endpoint changed: perform targeted passive GET calls to verify the response schema.
- If MQTT or display changed: verify MQTT topic publications or run `display-preview`.

### 5b. Comprehensive Health & Diagnostic Test
Check overall device stability and telemetry health:
```bash
curl -fsS "http://$TARGET_HOST/status" | jq '{version, paired, connected: .ble.connected, heap: .sys.free_heap, min_heap: .sys.min_free_heap, largest_block: .sys.largest_block, uptime: .sys.uptime_s}'
curl -fsS "http://$TARGET_HOST/api/proxy/1/version" | jq .
curl -fsS "http://$TARGET_HOST/diag?redact=1" | grep -iE 'error|warn|fail|panic' || true
```
Query VictoriaLogs (via `mcp-logs` or HTTP) for the target device over the last 15 minutes:
- Verify **zero** crash dumps, panics, or FreeRTOS task watchdog warnings.
- Verify heap stability (largest free contiguous block remains healthy).

### 5c. End-to-End evcc Verification
Execute the canonical repository E2E test script against the cluster evcc pod:
```bash
bash scripts/e2e_evcc.sh
```
This verifies:
1. Passive `/status` and `/api/proxy/1/version` reads from inside the cluster evcc pod.
2. 15x `vehicle_data` burst test with **0 timeouts**.
3. Absence of Tesla/proxy connection errors in recent evcc pod logs.

---

## Phase 6: Cleanup

Once deployment and all three tiers of verification succeed:

1. **Switch to main and sync**:
   ```bash
   git checkout main
   git pull origin main
   ```
2. **Delete Local Feature Branch**:
   ```bash
   git branch -d <branch-name>
   ```
3. **Delete Remote Feature Branch**:
   ```bash
   git push origin --delete <branch-name>
   ```
4. **Clean Workspace Artifacts**:
   Remove any temporary test files, build staging directories (`_site/`, `_signed/`, `dist/`), or diagnostic captures.
