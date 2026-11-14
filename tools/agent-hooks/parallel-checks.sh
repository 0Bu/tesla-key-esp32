#!/usr/bin/env bash
# Source from a Bash entry point and call directly so errexit stays active in each worker.
run_parallel_checks() {
  local first="$1" second="$2" first_pid="" second_pid="" status=0 log_dir
  log_dir="$(mktemp -d "${TMPDIR:-/tmp}/tesla-parallel-checks.XXXXXX")"
  cancel_checks() {
    local exit_code="$1" pid
    trap '' HUP INT TERM
    for pid in "$first_pid" "$second_pid"; do
      if [ -n "$pid" ]; then
        kill -TERM -- "-$pid" 2>/dev/null || true
      fi
    done
    wait 2>/dev/null || true
    rm -rf -- "$log_dir"
    exit "$exit_code"
  }
  trap 'cancel_checks 129' HUP
  trap 'cancel_checks 130' INT
  trap 'cancel_checks 143' TERM

  # Separate process groups let cancellation reach each shell and its foreground tool.
  set -m
  printf 'Running %s and %s in parallel\n' "$first" "$second"
  (set +m; "$first") </dev/null >"$log_dir/first" 2>&1 &
  first_pid=$!
  (set +m; "$second") </dev/null >"$log_dir/second" 2>&1 &
  second_pid=$!
  set +m

  report_check() {
    local label="$1" pid="$2" log="$3" result=0
    wait "$pid" || result=$?
    printf '\n== %s (exit %s) ==\n' "$label" "$result"
    cat "$log"
    if [ "$result" -ne 0 ]; then
      status=1
      printf 'Check failed: %s (exit %s)\n' "$label" "$result" >&2
    fi
  }
  report_check "$first" "$first_pid" "$log_dir/first"
  first_pid=""
  report_check "$second" "$second_pid" "$log_dir/second"
  second_pid=""
  trap - HUP INT TERM
  rm -rf -- "$log_dir"
  return "$status"
}
