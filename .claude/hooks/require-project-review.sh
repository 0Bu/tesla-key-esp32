#!/usr/bin/env bash
# PreToolUse gate: refuse a PR *merge* until a project review has been run against the commit
# being merged. Plain `git commit` and `gh pr create` are NOT gated — the review runs only
# before merging a PR into main.
#
# Two merge paths are gated, so the gate holds in BOTH environments:
#   • Bash `gh pr merge ...`              — local terminal sessions
#   • mcp__github__merge_pull_request     — Claude Code on the web / remote (no `gh` CLI)
# Matched via the `matcher` entries in .claude/settings.json that both invoke this script.
#
# Mechanism (NO file marker — see pr-gate-lib.sh): after running /project-review and confirming
# it passes with no blocking findings, record the pass by TICKING the PR checklist box and
# STAMPING it with the reviewed commit:
#     - [x] `/project-review` clean — merge gate @ <short-sha>
# This hook allows the merge only while that box is checked AND the stamped sha still matches the
# PR's head commit — i.e. the review reflects exactly what is being merged. Push another commit and
# the stamp goes stale (sha mismatch), forcing a fresh review + re-tick before the next merge.
#
# Only Claude Code sessions are gated; a human merging via the GitHub UI (or `gh` in a plain
# terminal) is unaffected, since hooks run only inside Claude Code.
#
# Exit codes: 0 = allow the tool call, 2 = block it (stderr is fed back to Claude).

proj="${CLAUDE_PROJECT_DIR:-$PWD}"
GATE_PROJ="$proj"
# shellcheck source=/dev/null
. "$proj/.claude/hooks/pr-gate-lib.sh" 2>/dev/null || exit 0   # lib missing -> don't block

# Read the tool-call payload from stdin (PreToolUse JSON).
input="$(cat 2>/dev/null)"
tool="$(printf '%s' "$input" | jq -r '.tool_name // ""' 2>/dev/null)"
cmd="$(printf '%s'  "$input" | jq -r '.tool_input.command // ""' 2>/dev/null)"

# Is this a gated merge, and which PR does it target? The MCP merge tool is gated
# unconditionally (selector = its pullNumber). A Bash call is gated only when it *invokes*
# `gh pr merge` at a command position — the command starts with `gh pr merge`, optionally behind a
# leading `cd <dir> &&`/`cd <dir>;` prefix. The phrase appearing merely as DATA (a commit message,
# a heredoc body, an echo/printf argument) is NOT matched. Anything else falls through and is
# allowed. Trade-off: a `gh pr merge` buried mid-compound-line is not caught by this Bash matcher;
# in the web/remote environment the reliable gate is the MCP matcher above.
action=""; selector=""
case "$tool" in
  mcp__github__merge_pull_request)
    action="merge_pull_request (GitHub MCP)"
    selector="$(printf '%s' "$input" | jq -r '.tool_input.pullNumber // .tool_input.pull_number // ""' 2>/dev/null)"
    ;;
  Bash)
    norm="$(printf '%s' "$cmd" | sed -E 's/^[[:space:]]+//; s/^cd[[:space:]]+[^;&|]+(&&|;)[[:space:]]*//')"
    if printf '%s' "$norm" | grep -Eq '^gh[[:space:]]+pr[[:space:]]+merge([[:space:]]|$)'; then
      action="gh pr merge"
      # First non-flag token after `gh pr merge` = the PR number/branch/url (may be absent -> current branch).
      selector="$(printf '%s' "$norm" | sed -E 's/^gh[[:space:]]+pr[[:space:]]+merge[[:space:]]*//' \
                  | tr ' ' '\n' | grep -vE '^-' | head -n1)"
    fi
    ;;
esac
[ -n "$action" ] || exit 0

key="project-review"; skill="/project-review"

# Fetch the target PR (head sha + body). Fail CLOSED if it can't be read.
if ! pr="$(gate_fetch_pr "$selector")"; then
  {
    echo "BLOCKED: \`$action\` — could not read the pull request to verify the $skill gate."
    echo
    echo "This gate reads a ticked, SHA-stamped checkbox from the PR body. Reading it needs GitHub"
    echo "access — \`gh\` (local) or \${GH_TOKEN}/\${GITHUB_TOKEN} (web/remote). Neither worked here."
    echo
    echo "Either run this from a session with \`gh\` authenticated, export a token, or merge via the"
    echo "GitHub UI after confirming the $skill box is ticked (hooks don't gate the web UI)."
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
  echo "BLOCKED: \`$action\` requires a current project review recorded in the PR."
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
