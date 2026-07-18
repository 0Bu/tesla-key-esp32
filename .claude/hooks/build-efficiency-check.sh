#!/usr/bin/env bash
# SessionStart hook: after a PR is merged to main, audit the resulting CI firmware
# build for EFFICIENCY regressions and surface anything actionable so the session can
# open a fix. A Claude Code hook can't observe a GitHub merge directly, so this fires
# at session start and inspects the latest *completed* `build` run on main — which is
# exactly the post-merge run. It is deduped per run id, so each merged build is audited
# once, not re-nagged every session.
#
# What "efficiency" means here (the rubric the audit applies):
#   • ccache hit rate     — a warm cache that still mostly misses ⇒ caching is broken.
#   • cache/artifact hygiene — was the ccache restored at all? did it save? key drift?
#   • build duration / regression — this run vs the previous main run.
#   • total run wall-clock — the whole workflow, against a soft budget.
#   • binary size headroom — per target, proactive warn band below the hard size-gate.
#
# Fix policy (chosen): on a problem, open a GitHub *Issue* or a *draft Fix-PR* — never an
# auto-commit to main. This hook is the trigger + rubric; the session performs the fix
# using the same GitHub access it has (gh CLI on a host, the GitHub MCP tools on the web).
#
# Two environments, like require-project-review.sh:
#   • Host with `gh` + `jq` → fully self-contained: fetch, parse, judge, emit a concrete
#     report, and advance the dedup marker.
#   • Claude Code on the web (no `gh`) → emit a directive telling the session to run the
#     same audit via the GitHub MCP `actions_*` tools, gated on the same marker so it
#     stays silent when there's nothing new.
#
# Purely informational: stdout becomes session context, it NEVER blocks (always exit 0).
# Register under "hooks.SessionStart" in .claude/settings.json.

set -uo pipefail

proj="${CLAUDE_PROJECT_DIR:-$PWD}"
cd "$proj" 2>/dev/null || exit 0

# Drain any SessionStart JSON on stdin (unused, but don't leave it on the pipe).
cat >/dev/null 2>&1 || true

marker="$proj/.claude/.build-efficiency-audited"   # holds the last-audited run id
have() { command -v "$1" >/dev/null 2>&1; }

# ── Tunable thresholds (one place to adjust the rubric) ──────────────────────────────
MIN_CCACHE_HIT_PCT=30        # warm cache below this hit rate ⇒ caching is ineffective
DUR_REGRESSION_FACTOR=130    # build-job duration > 1.30× the previous main run …
DUR_REGRESSION_MIN_S=120     # … and at least this many seconds slower ⇒ regression
TOTAL_RUN_BUDGET_S=1500      # soft budget for the whole workflow (25 min)
SIZE_GATE=$((0x1e8000))      # hard gate in ci-build-all.sh (1998848 B = slot 0x1f0000 − 32 KB)
SIZE_WARN=$((0x1d8000))      # proactive warn band ~64 KB below the gate. The largest signed image
                             # (esp32c5, ~0x1e1000) now sits inside this band — headroom is tight
                             # (~28 KB to the gate), so a growing feature must re-measure and shrink

# Portable ISO-8601 → epoch (GNU date, then BSD/macOS date). Echoes "" on failure.
epoch() {
  local ts="$1" e
  e="$(date -u -d "$ts" +%s 2>/dev/null)" && { echo "$e"; return; }
  e="$(date -u -j -f "%Y-%m-%dT%H:%M:%SZ" "$ts" +%s 2>/dev/null)" && { echo "$e"; return; }
  echo ""
}

emit_rubric() {
  cat <<EOF
Efficiency rubric to apply to that run (flag a problem when a threshold is crossed):
  • ccache hit rate: a run whose ccache was restored (warm) but reports < ${MIN_CCACHE_HIT_PCT}%
    hits means caching is broken — look at CCACHE_DIR / the cache key / set-target fullclean.
  • cache hygiene: confirm the "Cache ccache" step shows a restore AND a save; a persistent
    "Cache not found" across consecutive main runs means saves aren't landing (key churn).
  • build duration regression: build-job time > ${DUR_REGRESSION_FACTOR}% of the previous main
    run's AND ≥ ${DUR_REGRESSION_MIN_S}s slower in absolute terms.
  • total run wall-clock over the soft budget (${TOTAL_RUN_BUDGET_S}s).
  • binary size headroom: any target's app within the warn band (> $SIZE_WARN B, gate $SIZE_GATE B,
    OTA slot 2031616 B) — fix before the hard size-gate trips and breaks OTA.

On a problem: open a GitHub Issue (diagnosis + the offending run URL), or a *draft Fix-PR*
when the fix is mechanical (e.g. correct the ccache key, add a missing cache path, restore a
dropped step). Do NOT auto-commit to main. If clean, stay silent and record the run id.
After handling it, write the audited run id to: $marker
EOF
}

# ── Web / no-gh path: hand the audit to the session's GitHub MCP tools ────────────────
if ! have gh || ! have jq; then
  last="$( [ -f "$marker" ] && cat "$marker" 2>/dev/null || echo "none" )"
  cat <<EOF
[build-efficiency audit — post-merge]
No \`gh\` CLI here, so run this via the GitHub MCP tools (0Bu/tesla-key-esp32):
  1. actions_list (list_workflow_runs, workflow build.yml, branch main, status completed,
     per_page 2) → the newest run is the post-merge build.
  2. If its run id equals the last-audited id ($last), nothing was merged since — stay
     silent and stop. Otherwise continue.
  3. get_job_logs for the build job (and the previous run for a duration baseline). The
     log already carries the data: a "ccache stats" group, "size-gate <target> OK: <N> B"
     lines, the "Cache ccache" restore/save lines, and run timestamps for duration.
$(emit_rubric)
EOF
  exit 0
fi

# ── Host path: fetch + parse + judge with gh, then advance the marker ─────────────────
runs="$(gh run list --workflow build.yml --branch main --status completed --limit 2 \
        --json databaseId,conclusion,createdAt,updatedAt,headSha,displayTitle,url 2>/dev/null)"
[ -n "$runs" ] && [ "$(printf '%s' "$runs" | jq 'length')" -gt 0 ] 2>/dev/null || exit 0

id="$(printf '%s' "$runs"      | jq -r '.[0].databaseId')"
concl="$(printf '%s' "$runs"   | jq -r '.[0].conclusion')"
url="$(printf '%s' "$runs"     | jq -r '.[0].url')"
title="$(printf '%s' "$runs"   | jq -r '.[0].displayTitle')"
c0="$(printf '%s' "$runs"      | jq -r '.[0].createdAt')"
u0="$(printf '%s' "$runs"      | jq -r '.[0].updatedAt')"

# Dedup: already audited this run ⇒ nothing merged since ⇒ emit nothing.
[ -f "$marker" ] && [ "$(cat "$marker" 2>/dev/null)" = "$id" ] && exit 0

# Total run wall-clock.
ce="$(epoch "$c0")"; ue="$(epoch "$u0")"; total=""
[ -n "$ce" ] && [ -n "$ue" ] && total=$(( ue - ce ))

# Previous main run's duration as a regression baseline.
prev=""
if [ "$(printf '%s' "$runs" | jq 'length')" -ge 2 ]; then
  pce="$(epoch "$(printf '%s' "$runs" | jq -r '.[1].createdAt')")"
  pue="$(epoch "$(printf '%s' "$runs" | jq -r '.[1].updatedAt')")"
  [ -n "$pce" ] && [ -n "$pue" ] && prev=$(( pue - pce ))
fi

log="$(gh run view "$id" --log 2>/dev/null || true)"

# ccache hit rate — handle ccache 4.x ("Hits: H / C (P%)") and 3.x ("cache hit rate P %").
cc_pct="$(printf '%s' "$log" | grep -iE 'cache hit rate|^[[:space:]]*Hits:' | grep -oE '[0-9]+(\.[0-9]+)?[[:space:]]*%' | head -n1 | grep -oE '[0-9]+' | head -n1)"
# Was the cache restored / did it save?
cache_restored="no"; printf '%s' "$log" | grep -qiE 'Cache restored (from|with) key' && cache_restored="yes"
cache_saved="no";    printf '%s' "$log" | grep -qiE 'Cache saved (successfully|with key)' && cache_saved="yes"
cache_miss="no";     printf '%s' "$log" | grep -qiE 'Cache not found for input keys'      && cache_miss="yes"

# Per-target binary sizes from the size-gate echo.
sizes="$(printf '%s' "$log" | grep -oE 'size-gate [a-z0-9]+ OK: [0-9]+ B' || true)"

# ── Apply the rubric. ─────────────────────────────────────────────────────────────────
problems=()
[ "$concl" != "success" ] && problems+=("build run did not succeed (conclusion: $concl)")

if [ "$cache_restored" = "yes" ] && [ -n "$cc_pct" ] && [ "$cc_pct" -lt "$MIN_CCACHE_HIT_PCT" ]; then
  problems+=("ccache warm but hit rate ${cc_pct}% (< ${MIN_CCACHE_HIT_PCT}%) — caching ineffective")
fi
[ "$cache_saved" = "no" ] && [ "$cache_restored" = "yes" ] && \
  problems+=("ccache restored but no save recorded — next run starts cold (key churn?)")

if [ -n "$total" ] && [ "$total" -gt "$TOTAL_RUN_BUDGET_S" ]; then
  problems+=("total run ${total}s over the ${TOTAL_RUN_BUDGET_S}s budget")
fi
if [ -n "$total" ] && [ -n "$prev" ] && [ "$prev" -gt 0 ]; then
  if [ $(( total * 100 )) -gt $(( prev * DUR_REGRESSION_FACTOR )) ] && [ $(( total - prev )) -ge "$DUR_REGRESSION_MIN_S" ]; then
    problems+=("run ${total}s vs previous ${prev}s — duration regression")
  fi
fi
while IFS= read -r line; do
  [ -z "$line" ] && continue
  b="$(printf '%s' "$line" | grep -oE '[0-9]+ B' | grep -oE '[0-9]+')"
  t="$(printf '%s' "$line" | awk '{print $2}')"
  [ -n "$b" ] && [ "$b" -gt "$SIZE_WARN" ] && \
    problems+=("$t app ${b} B inside the warn band (> $SIZE_WARN B, gate $SIZE_GATE B) — shrink before the gate trips")
done <<< "$sizes"

# ── Report. ───────────────────────────────────────────────────────────────────────────
{
  echo "[build-efficiency audit — post-merge build #$id]"
  echo "  title:      $title"
  echo "  conclusion: $concl    url: $url"
  echo "  total run:  ${total:-n/a}s${prev:+  (previous main run: ${prev}s)}"
  echo "  ccache:     hit ${cc_pct:-n/a}%  restored=$cache_restored saved=$cache_saved cold=$cache_miss"
  [ -n "$sizes" ] && printf '  %s\n' "$sizes"
  echo
  if [ "${#problems[@]}" -eq 0 ]; then
    echo "Verdict: OK — no efficiency regression. (run id recorded; will not re-audit this build)"
  else
    echo "Verdict: PROBLEMS (${#problems[@]}):"
    printf '  - %s\n' "${problems[@]}"
    echo
    emit_rubric
  fi
}

# Record this run as audited so we don't re-report it next session (report-once-per-merge).
printf '%s\n' "$id" > "$marker" 2>/dev/null || true
exit 0
