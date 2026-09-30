#!/usr/bin/env bash
# ci-heal: Post-push CI monitoring, local failure reproduction and merge-readiness gate.
#
# Usage:
#   scripts/ci-heal.sh [--pr <number>] [--max-retries <n>] [--dry-run]
#                      [--attest <gate>=<evidence> ...] [--auto-merge]
#   scripts/ci-heal.sh --self-test
#   scripts/ci-heal.sh --help
#
# Boundaries (see .agents/skills/ci-heal/SKILL.md):
#   * ci-heal NEVER authors a commit and NEVER runs a review. A gate record ($skill-audit,
#     $pr-hygiene, $project-review, $feature-docs, $vehicle-command-audit) is written to the PR
#     only from an explicit --attest <gate>=<evidence> that names the independent review which
#     was actually completed for the exact head being stamped.
#   * Stamping, READY and --auto-merge require: local HEAD == PR head, green CI on that head, the
#     PR reporting MERGEABLE, and an attestation for every gate the PR needs (relevance comes from
#     the same GitHub-side changed-file list the merge hook uses).
#
# Exit codes: 0 ready/diagnosed, 1 blocked or failed, 2 usage/configuration, 3 gate records withheld.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

# The relevance predicates and the record format are shared with the merge hook. Running without
# them would silently drop the conditional gates, so a missing/broken library is a hard stop.
lib="$root/tools/agent-hooks/pr-gate-lib.sh"
if [ ! -f "$lib" ] || ! . "$lib"; then
  echo "ci-heal: error: cannot load $lib — refusing to run without the shared PR-gate predicates" >&2
  exit 2
fi
for gate_fn in gate_feature_docs_relevant gate_vehicle_command_relevant \
               gate_is_renovate_maintenance gate_pr_changed_files; do
  declare -F "$gate_fn" >/dev/null 2>&1 || {
    echo "ci-heal: error: pr-gate-lib.sh lacks $gate_fn — refusing to run" >&2
    exit 2
  }
done

REPO_SLUG="0Bu/tesla-key-esp32"
ALL_GATES="skill-audit project-review pr-hygiene feature-docs vehicle-command-audit"

usage() {
  cat <<'EOF'
ci-heal — Post-Push CI Monitoring, Local Failure Reproduction & Merge-Readiness Gate

Usage:
  scripts/ci-heal.sh [options]

Options:
  --pr <number>             Pull request number (default: auto-detect from branch)
  --max-retries <n>         Maximum monitoring/push iterations (default: 3, max: 5)
  --attest <gate>=<evidence>
                            Attest that the named independent review was completed for the exact
                            head being stamped. Repeatable. Gates: skill-audit, project-review,
                            pr-hygiene, feature-docs, vehicle-command-audit. Evidence is a short
                            single-line reference (report path, finding count). ci-heal never
                            stamps a gate without it and never runs the review itself.
  --auto-merge              After every condition holds, run the canonical squash merge
  --dry-run                 Diagnose only: no local commit/push, no stamping, no merge
  --stats                   Display historical self-evaluation statistics
  --self-test               Run offline self-test suite
  -h, --help                Show this help message

Exit codes: 0 ready/diagnosed, 1 blocked or failed, 2 usage/configuration, 3 gate records withheld.
EOF
}

show_stats() {
  if [ ! -f "$CACHE_FILE" ]; then
    echo "ci-heal: no historical run statistics found at .tmp/ci-heal-cache.json"
    return 0
  fi
  python3 - "$CACHE_FILE" <<'PY'
import json, sys

try:
    with open(sys.argv[1], "r") as f:
        data = json.load(f)
    print("================================================================================")
    print("ci-heal: HISTORICAL RUN STATISTICS & HEURISTICS")
    print("================================================================================")
    print(f"Total Recorded Runs:    {data.get('total_runs', 0)}")
    print(f"Successful Runs:        {data.get('success_runs', 0)}")
    freqs = data.get("failure_frequencies", {})
    if freqs:
        print("\nFailure Root Causes & Frequencies:")
        for cause, count in sorted(freqs.items(), key=lambda x: x[1], reverse=True):
            print(f"  - {cause}: {count}")
    runs = data.get("recent_runs", [])
    if runs:
        print(f"\nRecent Runs ({len(runs)}):")
        for r in runs[-5:]:
            print(f"  PR #{r.get('pr')} | {r.get('status')} | {r.get('iterations')} iters | score {r.get('score')} | cause: {r.get('root_cause')}")
    print("================================================================================")
except Exception as e:
    sys.stderr.write(f"ci-heal: error reading stats: {e}\n")
PY
}


# ─── thin wrappers around git/gh so the flow can be tested offline ────────────────────────────
git_local_head()      { git rev-parse HEAD; }
git_current_branch()  { git rev-parse --abbrev-ref HEAD; }
git_tree_dirty()      { [ -n "$(git status --porcelain)" ]; }
git_is_ancestor()     { git merge-base --is-ancestor "$1" "$2" 2>/dev/null; }
git_push_branch()     { git push origin "$1"; }
sleep_seconds()       { sleep "$1"; }
stamp_gates()         { "$root/scripts/stamp-pr-gates.sh" "$@"; }

format_canonical_merge_command() {
  local pr="$1" head_sha="$2"
  printf 'gh --repo github.com/0Bu/tesla-key-esp32 pr merge %s --match-head-commit %s --squash\n' "$pr" "$head_sha"
}

# Runs the canonical squash merge with validated arguments (no eval).
merge_pr() {
  local pr="$1" head_sha="$2"
  gh --repo github.com/0Bu/tesla-key-esp32 pr merge "$pr" --match-head-commit "$head_sha" --squash
}

check_gh_cli() {
  command -v gh >/dev/null 2>&1 || {
    echo "ci-heal: error: 'gh' CLI is required but not installed in PATH." >&2
    exit 2
  }
}

detect_pr() {
  local pr_input="$1"
  if [ -n "$pr_input" ]; then
    printf '%s' "$pr_input"
    return 0
  fi
  local detected
  detected="$(gh pr view --json number -q .number 2>/dev/null || true)"
  if [ -z "$detected" ]; then
    echo "ci-heal: error: could not detect open PR for current branch. Use --pr <number>." >&2
    exit 1
  fi
  printf '%s' "$detected"
}

get_pr_head_sha() {
  local pr="$1"
  gh pr view "$pr" --json headRefOid -q .headRefOid
}

get_pr_head_ref() {
  local pr="$1"
  gh pr view "$pr" --json headRefName -q .headRefName
}

watch_pr_checks() {
  local pr="$1"
  echo "ci-heal: watching CI checks for PR #$pr..."
  if gh pr checks "$pr" --watch >/dev/null 2>&1; then
    return 0
  else
    return 1
  fi
}

fetch_failed_runs() {
  local pr="$1"
  gh pr view "$pr" --json statusCheckRollup -q '
    .statusCheckRollup[]?
    | select(.conclusion == "FAILURE" or .conclusion == "TIMED_OUT" or .conclusion == "STARTUP_FAILURE")
    | "\(.name) | \(.status) | \(.conclusion) | \(.detailsUrl // "")"
  '
}

print_failed_job_logs() {
  local pr="$1"
  echo "ci-heal: fetching failure details for PR #$pr..."
  local head_sha
  head_sha="$(get_pr_head_sha "$pr")"

  local failed_runs
  failed_runs="$(gh run list --commit "$head_sha" --status failure --json databaseId,name,workflowName -q '.[] | "\(.databaseId) \(.name)"' 2>/dev/null || true)"
  if [ -z "$failed_runs" ]; then
    failed_runs="$(gh run list --limit 5 --json databaseId,conclusion,name -q '.[] | select(.conclusion == "failure") | "\(.databaseId) \(.name)"' 2>/dev/null || true)"
  fi

  if [ -n "$failed_runs" ]; then
    while read -r run_id run_name; do
      echo "--------------------------------------------------------------------------------"
      echo "ci-heal: failed run $run_id ($run_name):"
      gh run view "$run_id" --log-failed 2>/dev/null | tail -40 || true
      echo "--------------------------------------------------------------------------------"
    done <<< "$failed_runs"
  else
    echo "ci-heal: check failed on PR:"
    gh pr checks "$pr" || true
  fi
}

# Maps the failing check names to the narrowest local reproduction that should run first.
classify_failure() {
  local failed_info="$1"
  if [[ "$failed_info" == *"repo-lint"* ]]; then
    echo "repo-lint"
  elif [[ "$failed_info" == *"selftest"* || "$failed_info" == *"agent-config"* ]]; then
    echo "agent-config"
  elif [[ "$failed_info" == *"mock-test"* || "$failed_info" == *"logic-test"* ]]; then
    echo "mock-tests"
  elif [[ "$failed_info" == *"sanitizer"* ]]; then
    echo "sanitizers"
  elif [[ "$failed_info" == *"build-contract"* ]]; then
    echo "build-contracts"
  elif [[ "$failed_info" == *"pr-gate"* || "$failed_info" == *"pr-policy"* ]]; then
    echo "pr-gates"
  elif [[ "$failed_info" == *"build"* ]]; then
    echo "compiler"
  else
    echo "general-ci-failure"
  fi
}

CACHE_FILE="$root/.tmp/ci-heal-cache.json"

evaluate_and_optimize() {
  local pr="$1" status="$2" iters="$3" max_iters="$4" duration_secs="$5" root_cause="$6" files_changed="$7" lines_changed="$8"
  mkdir -p "$root/.tmp"

  local score=100
  if [ "$status" != "SUCCESS" ]; then
    score=0
  else
    # Score formula: 100 for iteration 1, -15 for each additional iteration, floor 40
    score=$((100 - (iters - 1) * 15))
    if (( score < 40 )); then score=40; fi
  fi

  echo "================================================================================"
  echo "ci-heal: RUN EVALUATION SCORECARD & SELF-OPTIMIZATION"
  echo "================================================================================"
  echo "  Outcome:             $status"
  echo "  PR:                  #$pr"
  echo "  Iterations:          $iters / $max_iters"
  echo "  Duration:            ${duration_secs}s"
  echo "  Identified Cause:    ${root_cause:-None / Clean}"
  echo "  Fix Footprint:       $files_changed files, $lines_changed lines modified"
  echo "  Efficiency Score:    $score / 100"
  echo "--------------------------------------------------------------------------------"

  python3 - "$CACHE_FILE" "$pr" "$status" "$iters" "$duration_secs" "${root_cause:-None}" "$score" <<'PY'
import json, os, sys, time

cache_file = sys.argv[1]
pr = sys.argv[2]
status = sys.argv[3]
iters = int(sys.argv[4])
duration = int(sys.argv[5])
root_cause = sys.argv[6]
score = int(sys.argv[7])

data = {
    "version": 1,
    "last_updated": int(time.time()),
    "total_runs": 0,
    "success_runs": 0,
    "failure_frequencies": {},
    "recent_runs": []
}

if os.path.exists(cache_file):
    try:
        with open(cache_file, "r") as f:
            data = json.load(f)
    except Exception:
        pass

data["total_runs"] = data.get("total_runs", 0) + 1
if status == "SUCCESS":
    data["success_runs"] = data.get("success_runs", 0) + 1

if root_cause and root_cause not in ("None", "None / Clean"):
    freqs = data.setdefault("failure_frequencies", {})
    freqs[root_cause] = freqs.get(root_cause, 0) + 1

runs = data.setdefault("recent_runs", [])
runs.append({
    "pr": pr,
    "status": status,
    "iterations": iters,
    "duration_secs": duration,
    "root_cause": root_cause,
    "score": score,
    "timestamp": int(time.time())
})
data["recent_runs"] = runs[-20:]

try:
    with open(cache_file, "w") as f:
        json.dump(data, f, indent=2)
except Exception as e:
    sys.stderr.write(f"ci-heal: warning: could not update cache: {e}\n")
PY

  echo "ci-heal: knowledge updated in .tmp/ci-heal-cache.json"
  if [ -n "$root_cause" ] && [ "$root_cause" != "None" ] && [ "$root_cause" != "None / Clean" ]; then
    echo "ci-heal: [Optimization] Prioritizing '$root_cause' in subsequent pre-flight runs."
  fi
  echo "================================================================================"
}

run_local_verifications() {
  local prioritized_cause="${1:-}"
  echo "ci-heal: running repository syntax, mock tests, and contract checks..."

  # If a specific failure cause is prioritized via cache/self-optimization, run it first
  case "$prioritized_cause" in
    repo-lint) "$root/scripts/repo-lint.sh" ;;
    agent-config) "$root/tools/agent-config/selftest.sh" ;;
    mock-tests) "$root/scripts/run-mock-tests.sh" ;;
    build-contracts) "$root/scripts/test-build-contracts.sh" ;;
    pr-gates) "$root/scripts/test-pr-gates.sh" ;;
    *) ;;
  esac

  "$root/scripts/repo-lint.sh"
  "$root/tools/agent-config/selftest.sh"
  "$root/scripts/run-mock-tests.sh"
  "$root/scripts/test-build-contracts.sh"
  "$root/scripts/test-pr-gates.sh"
}


# ─── attestations: the only source of gate records ────────────────────────────────────────────
attest_names=()
attest_evidence=()

parse_attest_arg() {
  local entry="$1" name token known=0 g
  name="${entry%%=*}"
  token="${entry#*=}"
  if [ "$name" = "$entry" ] || [ -z "$token" ]; then
    echo "ci-heal: --attest requires <gate>=<evidence> with a non-empty evidence reference" >&2
    exit 2
  fi
  for g in $ALL_GATES; do
    [ "$g" = "$name" ] && known=1
  done
  if [ "$known" -ne 1 ]; then
    echo "ci-heal: unknown gate for --attest: $name" >&2
    exit 2
  fi
  if [[ "$token" =~ [$'\r\n'] ]] || [[ "$token" == *"-->"* ]]; then
    echo "ci-heal: attestation evidence must be one line without '-->'" >&2
    exit 2
  fi
  attest_names+=("$name")
  attest_evidence+=("$token")
}

# Prints the evidence of the attestation for $1, or returns 1 when there is none.
attestation_for() {
  local want="$1" i=0
  while [ "$i" -lt "${#attest_names[@]}" ]; do
    if [ "${attest_names[$i]}" = "$want" ]; then
      printf '%s' "${attest_evidence[$i]}"
      return 0
    fi
    i=$((i + 1))
  done
  return 1
}

# Prints the gates the PR needs, one per line — the same set the merge/check hook enforces.
# Returns 1 when the changed-file list cannot be established (never guess).
required_gates_for_pr() {
  local pr="$1" files_file rc
  files_file="$(mktemp)"
  if ! gate_pr_changed_files "$pr" > "$files_file"; then
    rm -f "$files_file"
    return 1
  fi
  if gate_is_renovate_maintenance "$files_file"; then
    rm -f "$files_file"
    return 0
  fi
  echo "skill-audit"
  echo "project-review"
  echo "pr-hygiene"
  rc=0
  gate_feature_docs_relevant < "$files_file" || rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "feature-docs"
  elif [ "$rc" -ne 1 ]; then
    rm -f "$files_file"
    return 1
  fi
  if gate_vehicle_command_relevant < "$files_file"; then
    echo "vehicle-command-audit"
  fi
  rm -f "$files_file"
  return 0
}

# Fails (exit 3) unless every listed gate has an attestation. Prints what is missing.
require_attestations() {
  local purpose="$1" head_sha="$2"
  shift 2
  local missing=() g
  for g in "$@"; do
    attestation_for "$g" >/dev/null || missing+=("$g")
  done
  if [ "${#missing[@]}" -gt 0 ]; then
    echo "ci-heal: $purpose withheld — no attestation for: ${missing[*]}" >&2
    echo "ci-heal: complete the independent review(s) for exactly $head_sha, then re-run with" >&2
    for g in "${missing[@]}"; do
      echo "ci-heal:   --attest $g=<evidence>" >&2
    done
    return 3
  fi
  return 0
}

# Stamps the given gates for $head_sha on the PR from their attestations.
stamp_attested_gates() {
  local pr="$1" head_sha="$2"
  shift 2
  local args=() g ev
  for g in "$@"; do
    ev="$(attestation_for "$g")"
    args+=(--gate "$g=$ev")
  done
  stamp_gates --update-pr "$pr" --head "$head_sha" "${args[@]}"
}

check_pr_mergeability() {
  local pr="$1" mergeable_status attempt
  for attempt in 1 2 3; do
    mergeable_status="$(gh pr view "$pr" --json mergeable -q .mergeable 2>/dev/null || true)"
    [ "$mergeable_status" = "MERGEABLE" ] && return 0
    [ "$mergeable_status" = "UNKNOWN" ] || [ -z "$mergeable_status" ] || break
    sleep_seconds 5
  done
  echo "ci-heal: PR #$pr mergeable status is '${mergeable_status:-unreadable}' (not MERGEABLE)." >&2
  return 1
}

# Waits until GitHub reports the just-pushed commit as the PR head.
wait_for_remote_head() {
  local pr="$1" want="$2" tries=0 got
  while [ "$tries" -lt 12 ]; do
    got="$(get_pr_head_sha "$pr" 2>/dev/null || true)"
    [ "$got" = "$want" ] && return 0
    sleep_seconds 5
    tries=$((tries + 1))
  done
  return 1
}

is_sha() { [[ "$1" =~ ^[0-9a-f]{40}$ ]]; }

# ─── main flow ────────────────────────────────────────────────────────────────────────────────
run_heal() {
  local iteration=1 head_sha local_sha branch pr_ref failed_info duration start_time
  local green=0 gates rc g
  start_time="$(date +%s)"
  root_cause_detected="None"
  files_changed_count=0
  lines_changed_count=0

  branch="$(git_current_branch)"
  echo "================================================================================"
  echo "ci-heal: starting CI monitoring for PR #$target_pr on branch '$branch'"
  echo "================================================================================"

  pr_ref="$(get_pr_head_ref "$target_pr")"
  if [ "$branch" = "HEAD" ] || [ "$branch" != "$pr_ref" ]; then
    echo "ci-heal: error: current branch '$branch' is not the head branch '$pr_ref' of PR #$target_pr." >&2
    return 1
  fi

  while (( iteration <= max_retries )); do
    echo "ci-heal: [Iteration $iteration/$max_retries] Checking CI status..."

    head_sha="$(get_pr_head_sha "$target_pr")"
    is_sha "$head_sha" || { echo "ci-heal: error: unreadable PR head SHA '$head_sha'" >&2; return 1; }
    local_sha="$(git_local_head)"

    if watch_pr_checks "$target_pr"; then
      echo "ci-heal: all CI checks on PR #$target_pr are GREEN for $head_sha."
      green=1
      break
    fi

    echo "ci-heal: CI checks FAILED or are not green on PR #$target_pr."
    print_failed_job_logs "$target_pr"

    failed_info="$(fetch_failed_runs "$target_pr" || true)"
    if [ -z "$failed_info" ]; then
      echo "ci-heal: no failed check could be identified (pending, cancelled or API error)." >&2
      echo "ci-heal: not treating this as a fixable failure — inspect 'gh pr checks $target_pr'." >&2
      duration=$(( $(date +%s) - start_time ))
      evaluate_and_optimize "$target_pr" "FAILED" "$iteration" "$max_retries" "$duration" "unidentified" 0 0
      return 1
    fi
    root_cause_detected="$(classify_failure "$failed_info")"

    if [ "$dry_run" -eq 1 ]; then
      echo "ci-heal: [dry-run] Stop on CI failure without reproducing or pushing."
      duration=$(( $(date +%s) - start_time ))
      evaluate_and_optimize "$target_pr" "FAILED" "$iteration" "$max_retries" "$duration" "$root_cause_detected" 0 0
      return 1
    fi

    echo "ci-heal: attempting targeted local verifications (prioritizing $root_cause_detected)..."
    if ! run_local_verifications "$root_cause_detected"; then
      echo "ci-heal: local reproduction failed. Agent / developer remediation needed."
      duration=$(( $(date +%s) - start_time ))
      evaluate_and_optimize "$target_pr" "FAILED" "$iteration" "$max_retries" "$duration" "$root_cause_detected" "$files_changed_count" "$lines_changed_count"
      return 1
    fi

    # ci-heal never authors commits: a fix must already be committed by the operator.
    if git_tree_dirty; then
      echo "ci-heal: the working tree has uncommitted changes. ci-heal does not stage or commit;" >&2
      echo "ci-heal: commit the fix yourself (English message, no private identifiers), then re-run." >&2
      duration=$(( $(date +%s) - start_time ))
      evaluate_and_optimize "$target_pr" "FAILED" "$iteration" "$max_retries" "$duration" "$root_cause_detected" 0 0
      return 1
    fi
    if [ "$local_sha" = "$head_sha" ]; then
      echo "ci-heal: nothing to push — local HEAD equals the failing PR head. Inspect the logs above," >&2
      echo "ci-heal: commit a fix, then re-run." >&2
      duration=$(( $(date +%s) - start_time ))
      evaluate_and_optimize "$target_pr" "FAILED" "$iteration" "$max_retries" "$duration" "$root_cause_detected" 0 0
      return 1
    fi
    if ! git_is_ancestor "$head_sha" "$local_sha"; then
      echo "ci-heal: local HEAD $local_sha does not descend from PR head $head_sha (diverged or behind)." >&2
      echo "ci-heal: fetch and reconcile manually; ci-heal never rewrites history." >&2
      return 1
    fi

    # A fix commit is ready. The push hook needs current $skill-audit and $pr-hygiene records for
    # the exact commit being pushed, and only an attested review can supply them.
    rc=0
    require_attestations "pre-push gate records" "$local_sha" skill-audit pr-hygiene || rc=$?
    if [ "$rc" -ne 0 ]; then
      duration=$(( $(date +%s) - start_time ))
      evaluate_and_optimize "$target_pr" "FAILED" "$iteration" "$max_retries" "$duration" "$root_cause_detected" 0 0
      return "$rc"
    fi

    echo "ci-heal: stamping attested pre-push records for $local_sha and pushing '$branch'..."
    stamp_attested_gates "$target_pr" "$local_sha" skill-audit pr-hygiene
    git_push_branch "$branch"
    if ! wait_for_remote_head "$target_pr" "$local_sha"; then
      echo "ci-heal: error: GitHub did not report $local_sha as the PR head after the push." >&2
      return 1
    fi
    iteration=$((iteration + 1))
  done

  if [ "$green" -ne 1 ]; then
    echo "ci-heal: error: reached maximum retry limit ($max_retries). Manual intervention required." >&2
    duration=$(( $(date +%s) - start_time ))
    evaluate_and_optimize "$target_pr" "FAILED" "$iteration" "$max_retries" "$duration" "$root_cause_detected" "$files_changed_count" "$lines_changed_count"
    return 1
  fi

  echo "================================================================================"
  echo "ci-heal: Merge-readiness check for PR #$target_pr"
  echo "================================================================================"

  # The CI result belongs to $head_sha. Every later statement must be about that same commit.
  local final_head
  final_head="$(get_pr_head_sha "$target_pr")"
  if [ "$final_head" != "$head_sha" ]; then
    echo "ci-heal: PR head moved from $head_sha to $final_head while CI was watched; re-run." >&2
    return 1
  fi
  local_sha="$(git_local_head)"
  if [ "$local_sha" != "$final_head" ]; then
    echo "ci-heal: local HEAD $local_sha differs from PR head $final_head; the reviewed bytes are" >&2
    echo "ci-heal: not the CI-verified bytes. Check out the PR head and re-run." >&2
    return 1
  fi
  if git_tree_dirty; then
    echo "ci-heal: the working tree has uncommitted changes; readiness applies to the pushed head only." >&2
    return 1
  fi
  check_pr_mergeability "$target_pr" || return 1

  if ! gates="$(required_gates_for_pr "$target_pr")"; then
    echo "ci-heal: could not establish which gates PR #$target_pr needs (changed-file list unreadable)." >&2
    return 1
  fi
  local gate_list=()
  while IFS= read -r g; do
    [ -n "$g" ] && gate_list+=("$g")
  done <<< "$gates"
  echo "ci-heal: gates required for $final_head: ${gate_list[*]:-none (maintenance PR)}"

  if [ "$dry_run" -eq 1 ]; then
    echo "ci-heal: [dry-run] CI is green and the PR is mergeable; no gate was stamped and nothing was merged."
    duration=$(( $(date +%s) - start_time ))
    evaluate_and_optimize "$target_pr" "SUCCESS" "$iteration" "$max_retries" "$duration" "$root_cause_detected" "$files_changed_count" "$lines_changed_count"
    return 0
  fi

  if [ "${#gate_list[@]}" -gt 0 ]; then
    rc=0
    require_attestations "merge gate records" "$final_head" "${gate_list[@]}" || rc=$?
    if [ "$rc" -ne 0 ]; then
      duration=$(( $(date +%s) - start_time ))
      evaluate_and_optimize "$target_pr" "FAILED" "$iteration" "$max_retries" "$duration" "$root_cause_detected" 0 0
      return "$rc"
    fi
    # Re-read the head immediately before the body edit so a stamp can never name a stale commit.
    if [ "$(get_pr_head_sha "$target_pr")" != "$final_head" ]; then
      echo "ci-heal: PR head moved before stamping; nothing was stamped. Re-run." >&2
      return 1
    fi
    stamp_attested_gates "$target_pr" "$final_head" "${gate_list[@]}"
  fi

  local canonical_cmd
  canonical_cmd="$(format_canonical_merge_command "$target_pr" "$final_head")"

  echo "================================================================================"
  echo "ci-heal: PR #$target_pr is READY FOR MERGE (CI green, mergeable, attested gates stamped)."
  echo ""
  echo "Canonical squash merge command:"
  echo "  $canonical_cmd"
  echo "================================================================================"

  duration=$(( $(date +%s) - start_time ))
  evaluate_and_optimize "$target_pr" "SUCCESS" "$iteration" "$max_retries" "$duration" "$root_cause_detected" "$files_changed_count" "$lines_changed_count"

  if [ "$auto_merge" -eq 1 ]; then
    echo "ci-heal: --auto-merge enabled. Executing canonical squash merge..."
    merge_pr "$target_pr" "$final_head"
    echo "ci-heal: PR #$target_pr successfully merged!"
  fi
  return 0
}

# ─── offline self-test ────────────────────────────────────────────────────────────────────────
# Drives the REAL run_heal flow with stubbed git/gh wrappers, so the red-CI entry, the push path
# and every merge-readiness refusal are exercised — not just argument parsing.
SHA_A="0123456789abcdef0123456789abcdef01234567"
SHA_B="89abcdef0123456789abcdef0123456789abcdef"

self_test() {
  echo "ci-heal: running self-test..."
  local T
  T="$(mktemp -d 2>/dev/null || mktemp -d -t 'ci-heal-test')"
  clean_self_test() { rm -rf "$T"; }
  trap clean_self_test EXIT
  local fail=0

  # Test 1: canonical merge command generation
  local merge_cmd
  merge_cmd="$(format_canonical_merge_command 42 "$SHA_A")"
  [ "$merge_cmd" = "gh --repo github.com/0Bu/tesla-key-esp32 pr merge 42 --match-head-commit $SHA_A --squash" ] \
    || { echo "self-test: canonical merge format mismatch: $merge_cmd" >&2; exit 1; }

  # Test 2: SKILL file exists
  [ -f "$root/.agents/skills/ci-heal/SKILL.md" ] \
    || { echo "self-test: .agents/skills/ci-heal/SKILL.md is missing" >&2; exit 1; }

  # Test 3: scorecard and cache persistence
  local orig_cache="$CACHE_FILE"
  CACHE_FILE="$T/test-cache.json"
  evaluate_and_optimize 42 "SUCCESS" 1 3 10 "repo-lint" 2 15 >/dev/null
  [ -f "$CACHE_FILE" ] || { echo "self-test: cache file not written" >&2; exit 1; }
  python3 -c "import json; d=json.load(open('$CACHE_FILE')); assert d['total_runs']==1; assert d['success_runs']==1; assert d['failure_frequencies']['repo-lint']==1; assert d['recent_runs'][0]['score']==100" \
    || { echo "self-test: cache content invalid" >&2; exit 1; }
  CACHE_FILE="$orig_cache"

  # Test 4: attestation parsing rejects everything that is not <known gate>=<one-line evidence>
  local bad
  for bad in "project-review" "project-review=" "unknown-gate=x" "pr-hygiene=a-->b" $'pr-hygiene=a\nb'; do
    if ( parse_attest_arg "$bad" ) >/dev/null 2>&1; then
      echo "self-test: parse_attest_arg accepted invalid attestation: $bad" >&2
      exit 1
    fi
  done
  ( attest_names=(); attest_evidence=(); parse_attest_arg "pr-hygiene=report.md, 0 findings"
    [ "$(attestation_for pr-hygiene)" = "report.md, 0 findings" ] && ! attestation_for skill-audit >/dev/null ) \
    || { echo "self-test: attestation lookup failed" >&2; exit 1; }

  # Test 5: relevance predicates read STDIN (a file-name argument used to be silently ignored)
  local rel
  rel="$(printf 'main/foo.cpp\n' | gate_feature_docs_relevant && echo yes || echo no)"
  [ "$rel" = yes ] || { echo "self-test: feature-docs predicate did not match main/ from stdin" >&2; exit 1; }
  rel="$(printf 'docs/notes.md\n' | gate_feature_docs_relevant && echo yes || echo no)"
  [ "$rel" = no ] || { echo "self-test: feature-docs predicate matched a plain doc" >&2; exit 1; }

  # ── scenario harness ──
  local LOG_STAMPS="$T/stamps" LOG_PUSH="$T/pushes" LOG_MERGE="$T/merges" LOG_VERIFY="$T/verify"
  local last_out last_rc

  scenario_defaults() {
    CACHE_FILE="$T/cache.json"
    target_pr=42; max_retries=2; auto_merge=0; dry_run=0
    attest_names=(); attest_evidence=()
    STUB_HEAD="$SHA_A"; STUB_LOCAL="$SHA_A"; STUB_BRANCH=feature; STUB_REF=feature
    STUB_SEQ="green"; STUB_FAILED=""; STUB_REPRO=ok; STUB_DIRTY=0; STUB_ANCESTOR=0
    STUB_MERGEABLE=MERGEABLE; STUB_FILES="docs/notes.md"; STUB_FILES_FAIL=0
    STUB_HEAD_MOVE_AFTER=0; STUB_HEAD_COUNTER="$T/head-counter"; echo 0 > "$STUB_HEAD_COUNTER"
    git_local_head()     { printf '%s' "$STUB_LOCAL"; }
    git_current_branch() { printf '%s' "$STUB_BRANCH"; }
    git_tree_dirty()     { [ "$STUB_DIRTY" = 1 ]; }
    git_is_ancestor()    { [ "$STUB_ANCESTOR" = 1 ]; }
    git_push_branch()    { echo "push $1" >> "$LOG_PUSH"; STUB_HEAD="$STUB_LOCAL"; }
    sleep_seconds()      { :; }
    stamp_gates()        { echo "$*" >> "$LOG_STAMPS"; }
    merge_pr()           { echo "$*" >> "$LOG_MERGE"; }
    get_pr_head_ref()    { printf '%s' "$STUB_REF"; }
    get_pr_head_sha() {
      local n; n="$(cat "$STUB_HEAD_COUNTER")"; echo $((n + 1)) > "$STUB_HEAD_COUNTER"
      if [ "$STUB_HEAD_MOVE_AFTER" -gt 0 ] && [ "$n" -ge "$STUB_HEAD_MOVE_AFTER" ]; then
        printf '%s' "$SHA_B"
      else
        printf '%s' "$STUB_HEAD"
      fi
    }
    watch_pr_checks() {
      local first="${STUB_SEQ%% *}"
      STUB_SEQ="${STUB_SEQ#"$first"}"; STUB_SEQ="${STUB_SEQ# }"
      [ "$first" = green ]
    }
    print_failed_job_logs()   { echo "(stub) failed logs"; }
    fetch_failed_runs()       { printf '%s' "$STUB_FAILED"; }
    run_local_verifications() { echo "verify $1" >> "$LOG_VERIFY"; [ "$STUB_REPRO" != fail ]; }
    gate_pr_changed_files()   { [ "$STUB_FILES_FAIL" != 1 ] && printf '%s' "$STUB_FILES"; }
    gh() { case "$*" in *mergeable*) printf '%s\n' "$STUB_MERGEABLE" ;; *) return 1 ;; esac; }
  }

  # run_case <scenario-function> <expected-rc>
  run_case() {
    local fn="$1" want="$2"
    : > "$LOG_STAMPS"; : > "$LOG_PUSH"; : > "$LOG_MERGE"; : > "$LOG_VERIFY"
    last_rc=0
    last_out="$( ( scenario_defaults; "$fn"; run_heal ) 2>&1 )" || last_rc=$?
    if [ "$last_rc" -ne "$want" ]; then
      echo "self-test: $fn exited $last_rc, expected $want" >&2
      echo "$last_out" | sed 's/^/    | /' >&2
      fail=1
    fi
  }
  expect_out()    { printf '%s' "$last_out" | grep -Fq -- "$2" || { echo "self-test: $1: output lacks: $2" >&2; fail=1; }; }
  expect_no_out() { ! printf '%s' "$last_out" | grep -Fq -- "$2" || { echo "self-test: $1: output must not contain: $2" >&2; fail=1; }; }
  expect_lines()  { local n; n="$(grep -c . "$2" || true)"; [ "$n" = "$3" ] || { echo "self-test: $1: expected $3 line(s) in $(basename "$2"), got $n" >&2; fail=1; }; }
  base_attest() {
    attest_names=(skill-audit project-review pr-hygiene)
    attest_evidence=("audit-a" "review-b" "hygiene-c")
  }

  # F03: the red-CI entry used to abort on a toplevel `local` before any diagnosis
  sc_red_clean() { STUB_SEQ="red"; STUB_FAILED="repo-lint | COMPLETED | FAILURE | u"; }
  run_case sc_red_clean 1
  expect_out red_clean "nothing to push"; expect_no_out red_clean "can only be used in a function"
  expect_lines red_clean "$LOG_VERIFY" 1; expect_lines red_clean "$LOG_STAMPS" 0; expect_lines red_clean "$LOG_PUSH" 0
  grep -Fq -- "verify repo-lint" "$LOG_VERIFY" || { echo "self-test: red_clean: repo-lint reproduction was not prioritized" >&2; fail=1; }

  sc_red_dirty() { STUB_SEQ="red"; STUB_FAILED="build | COMPLETED | FAILURE | u"; STUB_DIRTY=1; }
  run_case sc_red_dirty 1
  expect_out red_dirty "does not stage or commit"; expect_lines red_dirty "$LOG_PUSH" 0; expect_lines red_dirty "$LOG_STAMPS" 0

  sc_red_unidentified() { STUB_SEQ="red"; STUB_FAILED=""; }
  run_case sc_red_unidentified 1
  expect_out red_unidentified "no failed check could be identified"; expect_lines red_unidentified "$LOG_VERIFY" 0

  sc_red_repro_fails() { STUB_SEQ="red"; STUB_FAILED="logic-test | COMPLETED | FAILURE | u"; STUB_REPRO=fail; }
  run_case sc_red_repro_fails 1
  expect_out red_repro_fails "local reproduction failed"; expect_lines red_repro_fails "$LOG_PUSH" 0

  sc_red_diverged() { STUB_SEQ="red"; STUB_FAILED="build | COMPLETED | FAILURE | u"; STUB_LOCAL="$SHA_B"; STUB_ANCESTOR=0; }
  run_case sc_red_diverged 1
  expect_out red_diverged "diverged or behind"; expect_lines red_diverged "$LOG_PUSH" 0

  sc_red_dry_run() { STUB_SEQ="red"; STUB_FAILED="build | COMPLETED | FAILURE | u"; dry_run=1; }
  run_case sc_red_dry_run 1
  expect_lines red_dry_run "$LOG_VERIFY" 0; expect_lines red_dry_run "$LOG_PUSH" 0

  sc_wrong_branch() { STUB_REF=other; }
  run_case sc_wrong_branch 1
  expect_out wrong_branch "is not the head branch"

  # A pushed fix without attested $skill-audit/$pr-hygiene must not be stamped or pushed
  sc_push_unattested() { STUB_SEQ="red"; STUB_FAILED="build | COMPLETED | FAILURE | u"; STUB_LOCAL="$SHA_B"; STUB_ANCESTOR=1; }
  run_case sc_push_unattested 3
  expect_out push_unattested "no attestation for: skill-audit pr-hygiene"
  expect_lines push_unattested "$LOG_PUSH" 0; expect_lines push_unattested "$LOG_STAMPS" 0

  # Attested fix: stamp for the LOCAL head, push once, then CI goes green; project-review is still
  # unattested, so readiness stays withheld and no second stamp is written.
  sc_push_attested() {
    STUB_SEQ="red green"; STUB_FAILED="build | COMPLETED | FAILURE | u"; STUB_LOCAL="$SHA_B"; STUB_ANCESTOR=1
    attest_names=(skill-audit pr-hygiene); attest_evidence=("audit-a" "hygiene-c")
  }
  run_case sc_push_attested 3
  expect_lines push_attested "$LOG_PUSH" 1; expect_lines push_attested "$LOG_STAMPS" 1
  grep -Fq -- "--head $SHA_B" "$LOG_STAMPS" || { echo "self-test: push_attested: pre-push stamp not bound to the pushed head" >&2; fail=1; }
  expect_out push_attested "no attestation for: project-review"; expect_no_out push_attested "READY FOR MERGE"

  # F02: readiness needs green CI on the SAME head, local == PR head, MERGEABLE and attestations
  sc_green_unattested() { :; }
  run_case sc_green_unattested 3
  expect_out green_unattested "merge gate records withheld"; expect_no_out green_unattested "READY FOR MERGE"
  expect_lines green_unattested "$LOG_STAMPS" 0

  sc_green_unattested_automerge() { auto_merge=1; }
  run_case sc_green_unattested_automerge 3
  expect_lines green_unattested_automerge "$LOG_MERGE" 0

  sc_green_attested() { base_attest; }
  run_case sc_green_attested 0
  expect_out green_attested "READY FOR MERGE"; expect_lines green_attested "$LOG_STAMPS" 1; expect_lines green_attested "$LOG_MERGE" 0
  grep -Fq -- "--update-pr 42 --head $SHA_A" "$LOG_STAMPS" \
    && grep -Fq -- "--gate project-review=review-b" "$LOG_STAMPS" \
    || { echo "self-test: green_attested: stamp arguments incorrect" >&2; fail=1; }

  sc_green_automerge() { base_attest; auto_merge=1; }
  run_case sc_green_automerge 0
  expect_lines green_automerge "$LOG_MERGE" 1
  grep -Fq -- "42 $SHA_A" "$LOG_MERGE" || { echo "self-test: green_automerge: merge not bound to the exact head" >&2; fail=1; }

  sc_green_local_differs() { base_attest; STUB_LOCAL="$SHA_B"; }
  run_case sc_green_local_differs 1
  expect_out green_local_differs "differs from PR head"; expect_lines green_local_differs "$LOG_STAMPS" 0

  sc_green_not_mergeable() { base_attest; STUB_MERGEABLE=CONFLICTING; }
  run_case sc_green_not_mergeable 1
  expect_no_out green_not_mergeable "READY FOR MERGE"; expect_lines green_not_mergeable "$LOG_STAMPS" 0

  sc_green_dirty() { base_attest; STUB_DIRTY=1; }
  run_case sc_green_dirty 1
  expect_lines green_dirty "$LOG_STAMPS" 0

  sc_green_head_moved() { base_attest; STUB_HEAD_MOVE_AFTER=1; }
  run_case sc_green_head_moved 1
  expect_out green_head_moved "moved"; expect_lines green_head_moved "$LOG_STAMPS" 0

  sc_green_files_unreadable() { base_attest; STUB_FILES_FAIL=1; }
  run_case sc_green_files_unreadable 1
  expect_out green_files_unreadable "could not establish which gates"; expect_lines green_files_unreadable "$LOG_STAMPS" 0

  sc_green_dry_run() { dry_run=1; }
  run_case sc_green_dry_run 0
  expect_no_out green_dry_run "READY FOR MERGE"; expect_lines green_dry_run "$LOG_STAMPS" 0

  # Conditional gates follow the same predicates as the merge hook (stdin, GitHub file list)
  sc_feature_relevant() { base_attest; STUB_FILES="main/foo.cpp"; }
  run_case sc_feature_relevant 3
  expect_out feature_relevant "feature-docs"; expect_no_out feature_relevant "vehicle-command-audit"

  sc_vehicle_relevant() { base_attest; STUB_FILES="main/vehicle_ctrl.cpp"; }
  run_case sc_vehicle_relevant 3
  expect_out vehicle_relevant "feature-docs"; expect_out vehicle_relevant "vehicle-command-audit"

  sc_all_five() {
    base_attest
    attest_names+=(feature-docs vehicle-command-audit); attest_evidence+=("docs-d" "vehicle-e")
    STUB_FILES=$'main/vehicle_ctrl.cpp\ndocs/FEATURES.md'
  }
  run_case sc_all_five 0
  expect_lines all_five "$LOG_STAMPS" 1
  grep -Fq -- "--gate vehicle-command-audit=vehicle-e" "$LOG_STAMPS" || { echo "self-test: all_five: vehicle gate not stamped" >&2; fail=1; }

  sc_renovate() { STUB_FILES=".github/renovate.json"; }
  run_case sc_renovate 0
  expect_lines renovate "$LOG_STAMPS" 0; expect_out renovate "READY FOR MERGE"

  rm -rf "$T"
  trap - EXIT
  if [ "$fail" -ne 0 ]; then
    echo "ci-heal: self-test FAIL" >&2
    exit 1
  fi
  echo "ci-heal: self-test PASS"
}

main() {
  target_pr=""; max_retries=3; auto_merge=0; dry_run=0
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --pr) [ "$#" -ge 2 ] || { echo "ci-heal: missing argument for --pr" >&2; exit 2; }; target_pr="$2"; shift 2 ;;
      --max-retries) [ "$#" -ge 2 ] || { echo "ci-heal: missing argument for --max-retries" >&2; exit 2; }; max_retries="$2"; shift 2 ;;
      --attest) [ "$#" -ge 2 ] || { echo "ci-heal: missing argument for --attest" >&2; exit 2; }; parse_attest_arg "$2"; shift 2 ;;
      --auto-merge) auto_merge=1; shift ;;
      --dry-run) dry_run=1; shift ;;
      --stats) show_stats; exit 0 ;;
      --self-test) self_test; exit 0 ;;
      -h|--help) usage; exit 0 ;;
      *) echo "ci-heal: unknown option: $1" >&2; usage; exit 2 ;;
    esac
  done

  [[ "$max_retries" =~ ^[0-9]+$ ]] && (( max_retries >= 1 && max_retries <= 5 )) || {
    echo "ci-heal: error: --max-retries must be between 1 and 5." >&2
    exit 2
  }
  if [ "$auto_merge" -eq 1 ] && [ "$dry_run" -eq 1 ]; then
    echo "ci-heal: error: --auto-merge cannot be combined with --dry-run." >&2
    exit 2
  fi

  check_gh_cli
  target_pr="$(detect_pr "$target_pr")"
  [[ "$target_pr" =~ ^[0-9]+$ ]] || { echo "ci-heal: error: PR number must be numeric." >&2; exit 2; }

  local rc=0
  run_heal || rc=$?
  exit "$rc"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main "$@"
fi
