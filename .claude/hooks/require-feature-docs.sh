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
#   • Bash `gh pr merge ...`              — local terminal sessions
#   • mcp__github__merge_pull_request     — Claude Code on the web / remote (no `gh` CLI)
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
    # One shell segment per line (see gate_bash_segments): the `^` anchors below must see a
    # CHAINED `&& git push` / `&& gh pr merge`, not just one that happens to come first.
    norm="$(gate_bash_segments "$cmd")"
    if printf '%s' "$norm" | grep -Eq '^gh[[:space:]]+pr[[:space:]]+merge([[:space:]]|$)'; then
      action="gh pr merge"
      # PR selector = first PR-number or URL token after `gh pr merge` *on that line* (a `cd <dir>`
      # on a preceding line, or trailing shell noise like `2>&1 | head`, must not be mistaken for
      # it). `sed -n …/p` isolates the merge line only; absent selector -> resolve the current branch.
      selector="$(printf '%s' "$norm" \
                  | sed -nE 's/^gh[[:space:]]+pr[[:space:]]+merge[[:space:]]*//p' | head -n1 \
                  | tr ' ' '\n' | grep -m1 -E '^([0-9]+|https?://[^ ]+)$' || true)"
    fi
    ;;
esac
[ -n "$action" ] || exit 0

key="feature-docs"; skill="/feature-docs"

# RELEVANCE FILTER — the one structural difference from require-project-review.sh, which gates
# EVERY merge. This gate targets exactly "a technical feature landed or changed", so a docs-only,
# script-only or chore PR clears in seconds without a sync. Relevance = the PR touched the surface
# docs/FEATURES.md actually catalogs.
#
# Fails CLOSED: if the changed-file list cannot be read, the gate applies rather than guessing the
# PR is irrelevant. An unreadable diff is not evidence of a chore.
changed="$(gate_pr_changed_files "$selector" 2>/dev/null)"; changed_rc=$?
if [ "$changed_rc" -eq 0 ] && [ -n "$changed" ]; then
  if ! printf '%s' "$changed" | grep -Eq '^(main/|test/|sdkconfig\.defaults|partitions\.csv$|\.github/workflows/build\.yml$)'; then
    exit 0   # nothing this catalog describes was touched
  fi
fi

# Fetch the target PR (head sha + body). Fail CLOSED on anything but a clean read — a merge must
# never proceed unverified (rc 1 = no such open PR, rc 2 = GitHub unreadable; both block).
pr="$(gate_fetch_pr "$selector")"; fetch_rc=$?
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
