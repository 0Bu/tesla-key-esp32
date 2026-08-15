#!/usr/bin/env bash
# PreToolUse gate: refuse a PR *merge* until docs/FEATURES.md has been synced against the commit
# being merged — but ONLY when the PR actually changes technical-feature surface.
#
# WHY A SEPARATE, NARROWER GATE. docs/FEATURES.md is the cross-cutting catalog of what this firmware
# implements at the platform level, and the failure it guards against is silent: a feature lands,
# the catalog does not move, and six months later the entry that would have said "this exists and
# here is the failure it prevents" simply is not there. Nothing else notices — the build is green,
# the tests pass, the narrative docs describe their own areas correctly. Unlike /project-review
# (which gates EVERY merge), this one is CONDITIONAL: a chore PR that cannot have changed a feature
# clears without ceremony. Plain `git commit` and `gh pr create` are NOT gated — the review runs only
# before merging a PR into main.
#
# Two merge paths are gated, so the gate holds in BOTH environments:
#   • Bash `gh pr merge <number-or-url> --match-head-commit <sha> ...`
#   • mcp__github__merge_pull_request     — exact github.com repo + expected_head_sha
# Matched via the `matcher` entries in .claude/settings.json that both invoke this script.
#
# Mechanism (NO file marker — see pr-gate-lib.sh): after running /feature-docs and confirming
# docs/FEATURES.md matches what the firmware now does, record the pass by TICKING the PR checklist
# box and STAMPING it with the synced commit:
#     - [x] `/feature-docs` synced — merge gate @ <short-sha>
# This is its OWN box, separate from the /project-review one require-project-review.sh reads (the
# gate matches on the "feature-docs" key, see gate_checkbox_status in pr-gate-lib.sh).
# This hook allows the merge only while that box is checked AND the stamped sha still matches the
# PR's head commit — i.e. the sync reflects exactly what is being merged. Push another commit and
# the stamp goes stale (sha mismatch), forcing a fresh sync + re-tick before the next merge.
#
# Only Claude Code sessions are gated; a human merging via the GitHub UI (or `gh` in a plain
# terminal) is unaffected, since hooks run only inside Claude Code.
#
# Exit codes: 0 = allow the tool call, 2 = block it (stderr is fed back to Claude).

proj="${CLAUDE_PROJECT_DIR:-$PWD}"
GATE_PROJ="$proj"
# shellcheck source=/dev/null
if ! . "$proj/.claude/hooks/pr-gate-lib.sh" 2>/dev/null; then
  echo "BLOCKED: feature-docs gate library could not be loaded." >&2
  exit 2
fi
for gate_fn in gate_bash_actions gate_pr_merge_selector gate_pr_merge_match_sha gate_mcp_repo_matches \
               gate_pr_changed_files gate_feature_docs_relevant gate_fetch_pr \
               gate_checkbox_status gate_sha_matches gate_full_head_sha; do
  if [ "${GATE_PR_LIB_API:-}" != 1 ] || ! declare -F "$gate_fn" >/dev/null 2>&1; then
    echo "BLOCKED: feature-docs gate library is incomplete ($gate_fn)." >&2
    exit 2
  fi
done

# Read the tool-call payload from stdin (PreToolUse JSON).
input="$(cat 2>/dev/null)"
tool="$(printf '%s' "$input" | jq -r '.tool_name // ""' 2>/dev/null)"
cmd="$(printf '%s'  "$input" | jq -r '.tool_input.command // ""' 2>/dev/null)"

# Is this a gated merge, and which PR does it target? The MCP merge tool is gated
# unconditionally (selector = its pullNumber). A Bash call is gated only when it *invokes*
# `gh pr merge` at a command position. gate_bash_actions recognises chained/grouped/wrapped
# actions and returns every guarded action. A merge must be the only shell segment; a preceding
# PR edit/checkout could mutate what was validated. Multiple/ambiguous actions fail closed. The
# matcher is deliberately textual and may conservatively block guarded-looking quoted data.
action=""; selector=""; expected_head=""; mcp_merge=0; pr=""
case "$tool" in
  mcp__github__merge_pull_request)
    action="merge_pull_request (GitHub MCP)"
    if ! gate_mcp_repo_matches "$input"; then
      echo "BLOCKED: GitHub MCP merge target does not exactly match this worktree's origin." >&2
      exit 2
    fi
    selector="$(printf '%s' "$input" \
      | jq -er '(.tool_input.pullNumber // .tool_input.pull_number) | tostring | select(test("^[0-9]+$"))' \
          2>/dev/null)" || {
      echo "BLOCKED: GitHub MCP merge requires one explicit numeric pullNumber." >&2
      exit 2
    }
    expected_head="$(printf '%s' "$input" \
      | jq -er '.tool_input.expected_head_sha | strings | select(test("^[0-9a-f]{40}$"))' \
          2>/dev/null)" || {
      echo "BLOCKED: GitHub MCP merge requires expected_head_sha as one full commit SHA." >&2
      exit 2
    }
    mcp_merge=1
    ;;
  Bash)
    records="$(gate_bash_actions "$cmd")"
    if printf '%s\n' "$records" | grep -Fq $'\t__GATE_UNSAFE_SHELL_CONTEXT__'; then
      {
        echo "BLOCKED: \`gh pr merge\` must be the only shell action in its Bash tool call."
        echo
        echo "A preceding/following command can mutate the synced PR after PreToolUse validates it."
        echo "Run the merge as a separate Bash call."
      } >&2
      exit 2
    fi
    merge_records="$(printf '%s\n' "$records" | grep $'^merge\t' || true)"
    merge_count=0
    [ -z "$merge_records" ] || merge_count="$(printf '%s\n' "$merge_records" | wc -l | tr -d ' ')"
    if [ "$merge_count" -gt 1 ]; then
      {
        echo "BLOCKED: multiple \`gh pr merge\` actions in one Bash call."
        echo
        echo "Run each merge as a separate tool call so every PR selector and docs stamp is verified."
      } >&2
      exit 2
    fi
    if [ "$merge_count" -eq 1 ]; then
      action="gh pr merge"
      merge_args="${merge_records#*$'\t'}"
      selector="$(gate_pr_merge_selector "$merge_args")" || {
        echo "BLOCKED: \`gh pr merge\` requires a literal PR number/URL as its first argument." >&2
        exit 2
      }
      expected_head="$(gate_pr_merge_match_sha "$merge_args")" || {
        echo "BLOCKED: merge requires --match-head-commit <full-40hex-PR-head> immediately after the selector." >&2
        exit 2
      }
    fi
    ;;
esac
[ -n "$action" ] || exit 0

key="feature-docs"; skill="/feature-docs"

# The conditional relevance shortcut must not run before the merge target is bound to the same PR
# head that was audited locally. Otherwise an irrelevant foreign/stale PR could clear early.
pr="$(gate_fetch_pr "$selector")"; fetch_rc=$?
[ "$fetch_rc" -eq 0 ] || {
  echo "BLOCKED: pull request could not be read for merge target/head verification." >&2
  exit 2
}
pr_head="$(printf '%s' "$pr" | head -n1)"
local_head="$(gate_full_head_sha)"
if [ -z "$local_head" ] || [ "$local_head" != "$pr_head" ] \
    || [ -z "$expected_head" ] || [ "$expected_head" != "$pr_head" ]; then
  echo "BLOCKED: merge PR head does not exactly match the locally audited HEAD." >&2
  exit 2
fi

# RELEVANCE FILTER — the one structural difference from require-project-review.sh, which gates
# EVERY merge. This gate targets exactly "a technical feature landed or changed", so a docs-only,
# unrelated script/workflow or chore PR clears in seconds without a sync. Relevance = the PR
# touched the surface docs/FEATURES.md actually catalogs, including the shipped Pages/installer
# runtime and the build, signed-preview and preview-cleanup release workflows.
#
# Fails CLOSED: if the changed-file list cannot be read, the gate applies rather than guessing the
# PR is irrelevant. An unreadable diff is not evidence of a chore.
changed="$(gate_pr_changed_files "$selector" 2>/dev/null)"; changed_rc=$?
if [ "$changed_rc" -eq 0 ] && [ -n "$changed" ]; then
  if ! printf '%s' "$changed" | gate_feature_docs_relevant; then
    exit 0   # nothing this catalog describes was touched
  fi
fi

# Fetch the target PR (head sha + body). Fail CLOSED on anything but a clean read — a merge must
# never proceed unverified (rc 1 = no such open PR, rc 2 = GitHub unreadable; both block).
if [ -z "$pr" ]; then
  pr="$(gate_fetch_pr "$selector")"; fetch_rc=$?
else
  fetch_rc=0
fi
if [ "$fetch_rc" -ne 0 ]; then
  {
    if [ "$fetch_rc" -eq 1 ]; then
      echo "BLOCKED: \`$action\` — no open pull request found for ${selector:-the current branch}."
      echo
      echo "The $skill gate reads a ticked, SHA-stamped checkbox from the PR body; there is no PR to read."
    else
      echo "BLOCKED: \`$action\` — could not read the pull request to verify the $skill gate."
      echo
      echo "This gate reads a ticked, SHA-stamped checkbox from the PR body. Reading it needs GitHub"
      echo "access — \`gh\` (local) or \${GH_TOKEN}/\${GITHUB_TOKEN} (web/remote). Neither worked here."
      echo
      echo "Either run this from a session with \`gh\` authenticated, export a token, or merge via the"
      echo "GitHub UI after confirming the $skill box is ticked (hooks don't gate the web UI)."
    fi
  } >&2
  exit 2
fi
head_sha="$(printf '%s' "$pr" | head -n1)"
body="$(printf '%s' "$pr" | tail -n +2)"

status="$(gate_checkbox_status "$body" "$key")"
box_state="${status%% *}"; box_sha=""
[ "$box_state" = "checked" ] && box_sha="$(printf '%s' "$status" | awk '{print $2}')"

if [ "$box_state" = "checked" ] && gate_sha_matches "$box_sha" "$head_sha"; then
  exit 0   # ticked and stamped with the PR's head commit -> review is current -> allow
fi

# Blocked: tell Claude exactly what to do, then exit 2 to veto the tool call.
{
  echo "BLOCKED: \`$action\` requires a current docs/FEATURES.md sync recorded in the PR."
  echo
  case "$box_state" in
    absent)    echo "The PR body has no \`$skill\` checkbox — the review has not been recorded." ;;
    unchecked) echo "The \`$skill\` checkbox in the PR body is present but unticked." ;;
    checked)
      if [ -z "$box_sha" ]; then
        echo "The \`$skill\` box is ticked but carries no \`@ <sha>\` stamp — cannot prove it"
        echo "reviewed the commit being merged."
      else
        echo "The \`$skill\` box is stamped @ $box_sha but the PR head is ${head_sha:-unknown} —"
        echo "the review is stale (commits landed since it ran)."
      fi ;;
  esac
  echo
  echo "Do this before merging the PR:"
  echo "  1. Run the project-review skill:        $skill"
  echo "  2. Once it passes with no blocking findings, tick + stamp the PR checkbox with the head"
  echo "     commit (\`git rev-parse --short=12 HEAD\`), e.g.:"
  echo "         - [x] \`$skill\` clean — merge gate @ ${head_sha:-<sha>}"
  echo "     (edit the PR body: \`gh pr edit <pr> --body-file <file>\`, or the GitHub MCP update tool.)"
  echo "  3. Re-run the $action command."
  echo
  echo "The stamp is valid only while it matches the PR head, so any later commit forces a fresh"
  echo "review. skill-audit ⊂ project-review — a clean review also lets you tick the \`/skill-audit\` box."
} >&2
exit 2
