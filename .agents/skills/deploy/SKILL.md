---
name: deploy
description: Orchestrates the full delivery lifecycle for tesla-key-esp32 from local workspace analysis and fix loops, commit, push, and PR creation, gate verification and CI monitoring, canonical squash merge and channel-bound main-run tracking, to OTA flashing on tesla-key-esp32.local, 3-tiered testing (targeted, comprehensive, e2e), and workspace cleanup. Requires explicit user authorization before execution.
---

> **Canonical runner-neutral skill.** Read [`AGENTS.md`](../../../AGENTS.md) before acting.
> Project skills are canonical under [`.agents/skills/`](../), and lifecycle/PR policy is
> enforced by the runner-neutral core under [`tools/agent-hooks/`](../../../tools/agent-hooks/).
> Invoke this workflow canonically as `$deploy`.

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

## Overview: The 6 Delivery Phases

```mermaid
flowchart TD
    P1[Phase 1: Local Analysis & Findings Fix Loop] --> P2[Phase 2: Commit, Push & PR Creation]
    P2 --> P3[Phase 3: Verification, Checklist & Gates Loop]
    P3 --> P4[Phase 4: Canonical Merge, Release & OTA Deployment]
    P4 --> P5[Phase 5: 3-Tiered Verification]
    P5 --> P6[Phase 6: Cleanup]
```

---

## Phase 1: Local Analysis & Findings Fix Loop

Before committing or pushing, verify workspace cleanliness, code style, unit tests, and contracts locally:

1. **Whitespace & Conflict Marker Check**:
   ```bash
   git diff --check
   ```
2. **Offline Repository Linter**:
   ```bash
   ./scripts/repo-lint.sh
   ```
3. **Host Mock & Unit Test Suite**:
   ```bash
   ./scripts/run-mock-tests.sh
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
6. **Firmware Compile Check (Conditional)**:
   If firmware files under `main/` were modified and Docker is available:
   ```bash
   ./scripts/idf-docker.sh idf.py -B build set-target esp32s3 build
   ```

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

## Phase 4: Canonical Merge, Release & OTA Deployment

### 1. Canonical Squash Merge
Once all PR checks pass and gates are stamped, obtain and verify the PR head SHA:
```bash
PR_HEAD=$(gh pr view "$PR" --json headRefOid -q .headRefOid)
[[ "$PR_HEAD" =~ ^[0-9a-f]{40}$ ]] || { echo "REFUSING: invalid PR head SHA" >&2; exit 1; }
```

Execute the single standalone canonical squash merge command using literal `<numeric_pr>` and `<full_40_hex_head_sha>` (never compounded, chained, or using shell variables):
```bash
gh --repo github.com/0Bu/tesla-key-esp32 pr merge <numeric_pr> --match-head-commit <full_40_hex_head_sha> --squash
```

Then retrieve the merge commit SHA:
```bash
MERGE_SHA=$(gh pr view <numeric_pr> --json mergeCommit -q .mergeCommit.oid)
[[ "$MERGE_SHA" =~ ^[0-9a-f]{40}$ ]] || { echo "REFUSING: invalid merge commit SHA" >&2; exit 1; }
```

### 2. Monitor Post-Merge Build & Release on Main
Watch the CI build run triggered by the merge commit on `main`:
```bash
set -euo pipefail
# A merge publishes the Dev channel from the main PUSH run. A stable Release exists for this commit
# only if a separately authorized manual `workflow_dispatch` run of `build` with `release: true`
# was made for it; select that run explicitly with CHANNEL_RUN=workflow_dispatch.
CHANNEL_RUN="${CHANNEL_RUN:-push}"
[[ "$CHANNEL_RUN" =~ ^(push|workflow_dispatch)$ ]] || {
  echo "REFUSING: CHANNEL_RUN must be push or workflow_dispatch" >&2; exit 1;
}
RUN_IDS=$(gh run list --workflow build --branch main --commit "$MERGE_SHA" --event "$CHANNEL_RUN" --limit 20 \
  --json databaseId,headSha,event \
  --jq ".[] | select(.headSha == \"$MERGE_SHA\" and .event == \"$CHANNEL_RUN\") | .databaseId")
[ "$(printf '%s\n' "$RUN_IDS" | awk 'NF {n++} END {print n+0}')" -eq 1 ] || {
  echo "REFUSING: expected exactly one build run for merge SHA $MERGE_SHA" >&2; exit 1;
}
MAIN_RUN_ID=$(printf '%s\n' "$RUN_IDS" | awk 'NF {print}')
gh run watch "$MAIN_RUN_ID" --exit-status
```

A merge publishes the **Dev channel** (`gh-pages:/dev/`, a `X.Y.Z-dev.N` version) from the push run.
Bind the delivery to this exact run and its signed artifact — never to the newest GitHub Release,
which belongs to another commit. If the merge produced no signed artifact there is no firmware to
deploy; stop.

```bash
set -euo pipefail
RUN_SHA=$(gh run view "$MAIN_RUN_ID" --json headSha --jq .headSha)
[ "$RUN_SHA" = "$MERGE_SHA" ] || { echo "REFUSING: selected run is not the merged commit" >&2; exit 1; }
ARTS=$(gh api "repos/:owner/:repo/actions/runs/$MAIN_RUN_ID/artifacts" \
  --jq '.artifacts[] | select(.expired == false) | .name' \
  | grep -E "^tesla-key-esp32-(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?-${RUN_SHA}$" || true)
[ "$(printf '%s\n' "$ARTS" | awk 'NF {n++} END {print n+0}')" -eq 1 ] || {
  echo "REFUSING: expected exactly one unexpired signed main artifact (no firmware to deploy?)" >&2; exit 1;
}
ART=$(printf '%s\n' "$ARTS" | awk 'NF {print}')
VERIFY_DIR=$(mktemp -d "${TMPDIR:-/tmp}/tesla-deploy-artifact.XXXXXX")
gh run download "$MAIN_RUN_ID" -n "$ART" -D "$VERIFY_DIR"
META="$VERIFY_DIR/dist/build-metadata.txt"
[ -f "$META" ] && [ ! -L "$META" ] \
  && [ "$(grep -c '^head_sha=' "$META")" -eq 1 ] \
  && [ "$(sed -n 's/^head_sha=//p' "$META")" = "$RUN_SHA" ] \
  && [ "$(grep -c '^display_version=' "$META")" -eq 1 ] || {
  echo "REFUSING: signed artifact metadata does not match the main run SHA" >&2; exit 1;
}
RELEASE_VERSION=$(sed -n 's/^display_version=//p' "$META")
[[ "$RELEASE_VERSION" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$ ]] \
  && (( ${#RELEASE_VERSION} <= 31 )) \
  && [ "$ART" = "tesla-key-esp32-$RELEASE_VERSION-$RUN_SHA" ] || {
  echo "REFUSING: signed artifact name is not bound to metadata version and run SHA" >&2; exit 1;
}
if [[ "$RELEASE_VERSION" == *-* ]]; then CHANNEL=dev; else CHANNEL=release; fi
{ [ "$CHANNEL_RUN" = push ] && [ "$CHANNEL" = dev ]; } \
  || { [ "$CHANNEL_RUN" = workflow_dispatch ] && [ "$CHANNEL" = release ]; } || {
  echo "REFUSING: run event $CHANNEL_RUN does not match version channel $CHANNEL" >&2; exit 1;
}
if [ "$CHANNEL" = release ]; then
  git fetch --tags -q
  [ "$(git rev-parse "v$RELEASE_VERSION^{commit}")" = "$RUN_SHA" ] || {
    echo "REFUSING: release tag v$RELEASE_VERSION does not resolve to the merged commit" >&2; exit 1;
  }
fi
echo "Deploying $CHANNEL channel version $RELEASE_VERSION (run $MAIN_RUN_ID @ $RUN_SHA)"
```

### 3. OTA Update on Target Board
Target device address defaults to `tesla-key-esp32.local` (with optional `DEVICE_IP` override, e.g. `192.0.2.100`):
```bash
set -euo pipefail
TARGET_HOST="${DEVICE_IP:-tesla-key-esp32.local}"
: "${TARGET:?set TARGET to the board chip target: esp32 | esp32s3 | esp32c3 | esp32c6}"
: "${CHANNEL:?run the artifact-binding block first}"
case "$TARGET" in esp32) FAMILY=ESP32 ;; esp32s3) FAMILY=ESP32-S3 ;; esp32c3) FAMILY=ESP32-C3 ;;
  esp32c6) FAMILY=ESP32-C6 ;; *) echo "REFUSING: unsupported TARGET" >&2; exit 1 ;; esac

# Step A: Initiate background OTA check
CHECK_JSON=$(curl --connect-timeout 5 --max-time 10 -fsS -X POST \
  "http://$TARGET_HOST/ota/check?ms=$(date +%s000)")
printf '%s' "$CHECK_JSON" | jq -e '.started == true' >/dev/null || {
  echo "REFUSING: OTA check did not start" >&2; exit 1;
}

# Step B: Poll /ota/status until ready with matching release version
OTA_READY=0
CHECK_DEADLINE=$((SECONDS + 60))
while (( SECONDS < CHECK_DEADLINE )); do
  OTA_JSON=$(curl --connect-timeout 5 --max-time 10 -fsS \
    "http://$TARGET_HOST/ota/status") || { sleep 2; continue; }
  OTA_STATE=$(printf '%s' "$OTA_JSON" | jq -r .state)
  case "$OTA_STATE" in
    checking) sleep 2; continue ;;
    error)
      echo "REFUSING: OTA check failed: $(printf '%s' "$OTA_JSON" | jq -r .message)" >&2
      exit 1 ;;
    idle)
      printf '%s' "$OTA_JSON" | jq -e --arg v "$RELEASE_VERSION" --arg c "$CHANNEL" \
        '.update_available == true and .available == $v and .channel == $c' >/dev/null || {
        echo "REFUSING: OTA manifest is not an available $CHANNEL-channel update for $RELEASE_VERSION" >&2
        exit 1
      }
      OTA_READY=1
      break ;;
    *) echo "REFUSING: unexpected OTA state $OTA_STATE" >&2; exit 1 ;;
  esac
done
[ "$OTA_READY" -eq 1 ] || { echo "REFUSING: OTA check timed out" >&2; exit 1; }

# Step C: Trigger OTA update
UPDATE_JSON=$(curl --connect-timeout 5 --max-time 10 -fsS -X POST \
  "http://$TARGET_HOST/ota/update")
printf '%s' "$UPDATE_JSON" | jq -e '.result == true' >/dev/null || {
  echo "REFUSING: OTA update failed to start" >&2; exit 1;
}

# Steps D-F: the same fully bound monitor as $ship. A hidden reboot must not pass, so every sample
# has to show the exact version/platform, monotonic uptime and an uptime delta that tracks wall time.
live_matches_artifact() {
  LIVE_REACHABLE=0
  LIVE_STATUS_JSON=$(curl --connect-timeout 3 --max-time 5 -fsS \
    "http://$TARGET_HOST/status") || return 1
  LIVE_STATUS_WALL=$SECONDS
  LIVE_VERSION_JSON=$(curl --connect-timeout 3 --max-time 5 -fsS \
    "http://$TARGET_HOST/api/proxy/1/version") || return 1
  LIVE_REACHABLE=1
  printf '%s' "$LIVE_STATUS_JSON" | jq -e --arg v "$RELEASE_VERSION" '.version == $v' >/dev/null \
    && printf '%s' "$LIVE_VERSION_JSON" | jq -e --arg v "$RELEASE_VERSION" --arg p "$FAMILY" \
         '.version == ($v + "-esp32") and .platform == $p' >/dev/null
}

# Step D: bounded download/reboot window; a reported OTA error is terminal.
VERIFIED=0
UPDATE_DEADLINE=$((SECONDS + 600))
while (( SECONDS < UPDATE_DEADLINE )); do
  if live_matches_artifact; then VERIFIED=1; break; fi
  if OTA_JSON=$(curl --connect-timeout 3 --max-time 5 -fsS "http://$TARGET_HOST/ota/status"); then
    if [ "$(printf '%s' "$OTA_JSON" | jq -r '.state // empty')" = error ]; then
      echo "DEPLOYMENT INCOMPLETE: OTA failed: $(printf '%s' "$OTA_JSON" | jq -r .message)" >&2
      exit 1
    fi
  fi
  sleep 2
done
[ "$VERIFIED" -eq 1 ] || {
  echo "DEPLOYMENT INCOMPLETE: OTA did not boot exact $RELEASE_VERSION/$FAMILY within 600 seconds" >&2
  exit 1
}

# Step E: rollback probation. Bind the baseline to the FIRST exact post-OTA observation, then
# require every later sample to keep the exact identity, never lower the uptime, and advance
# uptime and wall clock together (within 5 s) until both have covered 100 s.
PROBATION_BASELINE_UPTIME=$(printf '%s' "$LIVE_STATUS_JSON" \
  | jq -er '.sys.uptime_s | select(type == "number" and . >= 0) | floor') || {
  echo "DEPLOYMENT INCOMPLETE: live status lacks numeric sys.uptime_s" >&2; exit 1;
}
PROBATION_BASELINE_WALL=$LIVE_STATUS_WALL
LAST_UPTIME=$PROBATION_BASELINE_UPTIME
PROBATION_OK=0
PROBATION_DEADLINE=$((PROBATION_BASELINE_WALL + 180))
while (( SECONDS < PROBATION_DEADLINE )); do
  if live_matches_artifact; then
    CUR_UPTIME=$(printf '%s' "$LIVE_STATUS_JSON" \
      | jq -er '.sys.uptime_s | select(type == "number" and . >= 0) | floor') || {
      echo "DEPLOYMENT INCOMPLETE: live status lacks numeric sys.uptime_s" >&2; exit 1;
    }
    if (( CUR_UPTIME < LAST_UPTIME )); then
      echo "DEPLOYMENT INCOMPLETE: device rebooted during OTA probation" >&2
      exit 1
    fi
    LAST_UPTIME=$CUR_UPTIME
    OBSERVED_WALL=$((LIVE_STATUS_WALL - PROBATION_BASELINE_WALL))
    OBSERVED_UPTIME=$((CUR_UPTIME - PROBATION_BASELINE_UPTIME))
    CLOCK_SKEW=$((OBSERVED_UPTIME - OBSERVED_WALL))
    if (( CLOCK_SKEW < -5 || CLOCK_SKEW > 5 )); then
      echo "DEPLOYMENT INCOMPLETE: uptime/wall-clock drift suggests a hidden reboot" >&2
      exit 1
    fi
    if (( OBSERVED_WALL >= 100 && OBSERVED_UPTIME >= 100 )); then
      PROBATION_OK=1
      break
    fi
  elif [ "${LIVE_REACHABLE:-0}" -eq 1 ]; then
    echo "DEPLOYMENT INCOMPLETE: device changed version/platform during OTA probation" >&2
    exit 1
  fi
  sleep 2
done
[ "$PROBATION_OK" -eq 1 ] || {
  echo "DEPLOYMENT INCOMPLETE: exact image was not stable for 100 seconds after first live observation" >&2
  exit 1
}

# Step F: confirm rollback cancellation via /diag?redact=1 (boot-local mark-valid evidence)
OTA_DIAG=$(curl --connect-timeout 3 --max-time 5 -fsS "http://$TARGET_HOST/diag?redact=1") || {
  echo "DEPLOYMENT INCOMPLETE: cannot verify OTA mark-valid result" >&2; exit 1;
}
printf '%s' "$OTA_DIAG" | grep -F 'OTA image healthy after ' \
  | grep -F 'marked valid (rollback cancelled' >/dev/null || {
  echo "DEPLOYMENT INCOMPLETE: firmware did not confirm rollback cancellation" >&2
  exit 1
}
echo "OTA update successfully deployed and verified on $TARGET_HOST."
```

---

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
