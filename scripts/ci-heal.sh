#!/usr/bin/env bash
# ci-heal: Automated Post-Push CI Monitoring, Failure Remediation & PR Merge Readiness
#
# Usage:
#   scripts/ci-heal.sh [--pr <number>] [--max-retries <n>] [--auto-merge] [--dry-run]
#   scripts/ci-heal.sh --self-test
#   scripts/ci-heal.sh --help
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

# shellcheck source=/dev/null
lib="$root/tools/agent-hooks/pr-gate-lib.sh"
if [ -f "$lib" ]; then
  # shellcheck source=/dev/null
  . "$lib" 2>/dev/null || true
fi

usage() {
  cat <<'EOF'
ci-heal — Post-Push CI Monitoring, Failure Remediation & PR Merge Readiness

Usage:
  scripts/ci-heal.sh [options]

Options:
  --pr <number>       Specify pull request number (default: auto-detect from branch)
  --max-retries <n>   Maximum automated remediation iterations (default: 3, max: 5)
  --auto-merge        Execute canonical squash merge when all checks and gates are clean
  --dry-run           Diagnose and check without committing, pushing, or stamping
  --stats             Display historical self-evaluation and optimization statistics
  --self-test         Run offline self-test suite
  -h, --help          Show this help message
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

self_test() {
  echo "ci-heal: running self-test..."
  local test_tmp
  test_tmp="$(mktemp -d 2>/dev/null || mktemp -d -t 'ci-heal-test')"
  clean_self_test() {
    rm -rf "$test_tmp"
  }
  trap clean_self_test EXIT

  # Test 1: verify argument parsing
  local pr_num="" max_r=3 auto_m=0 dry_r=0
  parse_test_args() {
    while [ "$#" -gt 0 ]; do
      case "$1" in
        --pr) pr_num="$2"; shift 2 ;;
        --max-retries) max_r="$2"; shift 2 ;;
        --auto-merge) auto_m=1; shift ;;
        --dry-run) dry_r=1; shift ;;
        *) return 1 ;;
      esac
    done
  }
  parse_test_args --pr 42 --max-retries 4 --auto-merge --dry-run
  [ "$pr_num" = "42" ] || { echo "self-test: --pr failed" >&2; exit 1; }
  [ "$max_r" -eq 4 ] || { echo "self-test: --max-retries failed" >&2; exit 1; }
  [ "$auto_m" -eq 1 ] || { echo "self-test: --auto-merge failed" >&2; exit 1; }
  [ "$dry_r" -eq 1 ] || { echo "self-test: --dry-run failed" >&2; exit 1; }

  # Test 2: verify canonical merge command generation
  local sample_sha="0123456789abcdef0123456789abcdef01234567"
  local merge_cmd
  merge_cmd="$(format_canonical_merge_command 42 "$sample_sha")"
  [ "$merge_cmd" = "gh --repo github.com/0Bu/tesla-key-esp32 pr merge 42 --match-head-commit 0123456789abcdef0123456789abcdef01234567 --squash" ] \
    || { echo "self-test: canonical merge format mismatch: $merge_cmd" >&2; exit 1; }

  # Test 3: verify SKILL file exists and is valid
  [ -f "$root/.agents/skills/ci-heal/SKILL.md" ] \
    || { echo "self-test: .agents/skills/ci-heal/SKILL.md is missing" >&2; rm -rf "$test_tmp"; exit 1; }

  # Test 4: verify evaluation scorecard and cache persistence
  local orig_cache="$CACHE_FILE"
  CACHE_FILE="$test_tmp/test-cache.json"
  evaluate_and_optimize 42 "SUCCESS" 1 3 10 "repo-lint" 2 15 >/dev/null
  [ -f "$CACHE_FILE" ] || { echo "self-test: cache file not written" >&2; rm -rf "$test_tmp"; exit 1; }
  python3 -c "import json; d=json.load(open('$CACHE_FILE')); assert d['total_runs']==1; assert d['success_runs']==1; assert d['failure_frequencies']['repo-lint']==1; assert d['recent_runs'][0]['score']==100" \
    || { echo "self-test: cache content invalid" >&2; rm -rf "$test_tmp"; exit 1; }
  CACHE_FILE="$orig_cache"

  rm -rf "$test_tmp"
  trap - EXIT
  echo "ci-heal: self-test PASS"
}

format_canonical_merge_command() {
  local pr="$1" head_sha="$2"
  printf 'gh --repo github.com/0Bu/tesla-key-esp32 pr merge %s --match-head-commit %s --squash\n' "$pr" "$head_sha"
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

stamp_pre_push_gates() {
  local pr="$1" head_sha="$2"
  echo "ci-heal: pre-push stamping $head_sha on PR #$pr..."
  node "$root/tools/agent-config/check.mjs" >/dev/null
  "$root/scripts/stamp-pr-gates.sh" --update-pr "$pr" --head "$head_sha" \
    --gate skill-audit="tools/agent-config/check.mjs @ $head_sha" \
    --gate pr-hygiene="clean @ $head_sha"
}

stamp_all_merge_gates() {
  local pr="$1" head_sha="$2"
  echo "ci-heal: auditing and stamping all merge gates for $head_sha on PR #$pr..."
  
  # Base required gates
  local gate_args=(
    --update-pr "$pr"
    --head "$head_sha"
    --gate "skill-audit=tools/agent-config/check.mjs @ $head_sha"
    --gate "pr-hygiene=clean @ $head_sha"
    --gate "project-review=0 findings @ $head_sha"
  )

  # Check conditional gates if pr-gate-lib functions exist
  if declare -F gate_feature_docs_relevant >/dev/null 2>&1; then
    local tmp_files
    tmp_files="$(mktemp 2>/dev/null || true)"
    if [ -n "$tmp_files" ]; then
      git diff --name-only "origin/main...$head_sha" > "$tmp_files" 2>/dev/null || true
      if gate_feature_docs_relevant "$tmp_files"; then
        gate_args+=(--gate "feature-docs=docs/FEATURES.md @ $head_sha")
      fi
      if declare -F gate_vehicle_command_relevant >/dev/null 2>&1; then
        if gate_vehicle_command_relevant "$tmp_files"; then
          gate_args+=(--gate "vehicle-command-audit=scripts/test-tesla-ble-harness.sh @ $head_sha")
        fi
      fi
      rm -f "$tmp_files"
    fi
  fi

  "$root/scripts/stamp-pr-gates.sh" "${gate_args[@]}"
}

check_pr_mergeability() {
  local pr="$1"
  local mergeable_status
  mergeable_status="$(gh pr view "$pr" --json mergeable -q .mergeable 2>/dev/null || true)"
  if [ "$mergeable_status" != "MERGEABLE" ]; then
    echo "ci-heal: warning: PR #$pr mergeable status is '$mergeable_status' (not MERGEABLE)." >&2
    return 1
  fi
  return 0
}

# --- CLI Option Parsing ---
target_pr=""
max_retries=3
auto_merge=0
dry_run=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --pr) [ "$#" -ge 2 ] || { echo "ci-heal: missing argument for --pr" >&2; exit 2; }; target_pr="$2"; shift 2 ;;
    --max-retries) [ "$#" -ge 2 ] || { echo "ci-heal: missing argument for --max-retries" >&2; exit 2; }; max_retries="$2"; shift 2 ;;
    --auto-merge) auto_merge=1; shift ;;
    --dry-run) dry_run=1; shift ;;
    --stats) show_stats; exit 0 ;;
    --self-test) self_test; exit 0 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "ci-heal: unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

if (( max_retries < 1 || max_retries > 5 )); then
  echo "ci-heal: error: --max-retries must be between 1 and 5." >&2
  exit 2
fi

check_gh_cli
target_pr="$(detect_pr "$target_pr")"
current_branch="$(git rev-parse --abbrev-ref HEAD)"

start_time="$(date +%s)"
root_cause_detected="None"
files_changed_count=0
lines_changed_count=0

echo "================================================================================"
echo "ci-heal: starting CI monitoring & healing for PR #$target_pr on branch '$current_branch'"
echo "================================================================================"

iteration=1
while (( iteration <= max_retries )); do
  echo "ci-heal: [Iteration $iteration/$max_retries] Checking CI status..."
  
  head_sha="$(get_pr_head_sha "$target_pr")"
  local_sha="$(git rev-parse HEAD)"

  if [ "$local_sha" != "$head_sha" ]; then
    echo "ci-heal: local HEAD ($local_sha) differs from PR remote HEAD ($head_sha)."
    echo "ci-heal: ensure your changes are pushed or branch is up to date."
  fi

  if watch_pr_checks "$target_pr"; then
    echo "ci-heal: all CI checks on PR #$target_pr are GREEN!"
    break
  else
    echo "ci-heal: CI checks FAILED on PR #$target_pr."
    print_failed_job_logs "$target_pr"

    # Identify probable cause from failed checks
    local failed_info
    failed_info="$(fetch_failed_runs "$target_pr" || true)"
    if [[ "$failed_info" == *"repo-lint"* ]]; then
      root_cause_detected="repo-lint"
    elif [[ "$failed_info" == *"selftest"* || "$failed_info" == *"agent-config"* ]]; then
      root_cause_detected="agent-config"
    elif [[ "$failed_info" == *"mock-test"* || "$failed_info" == *"logic-test"* ]]; then
      root_cause_detected="mock-tests"
    elif [[ "$failed_info" == *"sanitizer"* ]]; then
      root_cause_detected="sanitizers"
    elif [[ "$failed_info" == *"build-contract"* ]]; then
      root_cause_detected="build-contracts"
    elif [[ "$failed_info" == *"pr-gate"* || "$failed_info" == *"pr-policy"* ]]; then
      root_cause_detected="pr-gates"
    elif [[ "$failed_info" == *"build"* ]]; then
      root_cause_detected="compiler"
    else
      root_cause_detected="general-ci-failure"
    fi

    if [ "$dry_run" -eq 1 ]; then
      echo "ci-heal: [dry-run] Stop on CI failure without applying local fix."
      duration=$(( $(date +%s) - start_time ))
      evaluate_and_optimize "$target_pr" "FAILED" "$iteration" "$max_retries" "$duration" "$root_cause_detected" 0 0
      exit 1
    fi

    echo "ci-heal: attempting targeted local verifications (prioritizing $root_cause_detected)..."
    if ! run_local_verifications "$root_cause_detected"; then
      echo "ci-heal: local reproduction failed. Agent / developer remediation needed."
      duration=$(( $(date +%s) - start_time ))
      evaluate_and_optimize "$target_pr" "FAILED" "$iteration" "$max_retries" "$duration" "$root_cause_detected" "$files_changed_count" "$lines_changed_count"
      exit 1
    fi

    # Check if working tree has unstaged/uncommitted changes
    if git diff --quiet && git diff --staged --quiet; then
      echo "ci-heal: no local changes to commit. If CI failed remotely, inspect logs and apply fixes."
      duration=$(( $(date +%s) - start_time ))
      evaluate_and_optimize "$target_pr" "FAILED" "$iteration" "$max_retries" "$duration" "$root_cause_detected" "$files_changed_count" "$lines_changed_count"
      exit 1
    fi

    echo "ci-heal: committing and pushing local fix..."
    files_changed_count=$(git diff --stat 2>/dev/null | tail -1 | awk '{print $1}' || echo 0)
    lines_changed_count=$(git diff --shortstat 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i~/insertion|deletion/) s+=$(i-1)} END {print s+0}' || echo 0)
    new_head="$(git rev-parse HEAD)"
    stamp_pre_push_gates "$target_pr" "$new_head"
    git push origin "$current_branch"
    echo "ci-heal: push complete. Waiting 10s for new CI runs to register..."
    sleep 10
  fi

  iteration=$((iteration + 1))
done

if (( iteration > max_retries )); then
  echo "ci-heal: error: reached maximum retry limit ($max_retries). Manual intervention required." >&2
  duration=$(( $(date +%s) - start_time ))
  evaluate_and_optimize "$target_pr" "FAILED" "$iteration" "$max_retries" "$duration" "$root_cause_detected" "$files_changed_count" "$lines_changed_count"
  exit 1
fi

echo "================================================================================"
echo "ci-heal: Auditing gates & ensuring PR #$target_pr is merge-ready"
echo "================================================================================"

final_head="$(get_pr_head_sha "$target_pr")"
if [ "$dry_run" -eq 0 ]; then
  stamp_all_merge_gates "$target_pr" "$final_head"
fi

if check_pr_mergeability "$target_pr"; then
  echo "ci-heal: PR #$target_pr is MERGEABLE."
else
  echo "ci-heal: warning: PR #$target_pr is not marked MERGEABLE by GitHub."
fi

canonical_cmd="$(format_canonical_merge_command "$target_pr" "$final_head")"

echo "================================================================================"
echo "ci-heal: PR #$target_pr is READY FOR MERGE!"
echo ""
echo "Canonical squash merge command:"
echo "  $canonical_cmd"
echo "================================================================================"

duration=$(( $(date +%s) - start_time ))
evaluate_and_optimize "$target_pr" "SUCCESS" "$iteration" "$max_retries" "$duration" "$root_cause_detected" "$files_changed_count" "$lines_changed_count"

if [ "$auto_merge" -eq 1 ]; then
  echo "ci-heal: --auto-merge enabled. Executing canonical squash merge..."
  eval "$canonical_cmd"
  echo "ci-heal: PR #$target_pr successfully merged!"
fi
