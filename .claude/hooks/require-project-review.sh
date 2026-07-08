#!/usr/bin/env bash
# PreToolUse gate: refuse a PR *merge* until a project review has been run against
# the *current* working tree. Plain `git commit` and `gh pr create` are NOT gated —
# the review runs only before merging a PR into main.
#
# Two merge paths are gated, so the gate holds in BOTH environments:
#   • Bash `gh pr merge ...`              — local terminal sessions
#   • mcp__github__merge_pull_request     — Claude Code on the web / remote (no `gh`
#                                           CLI; merges go through the GitHub MCP server)
# Matched via the `matcher` entries in .claude/settings.json that both invoke this script.
#
# Mechanism: after running /project-review and confirming it passes with no blocking
# findings, record the pass by touching:
#     .claude/.project-review-passed
# This hook allows the merge only while the review still reflects the code being shipped —
# judged by GIT CONTENT, not file mtime: the tree carries no uncommitted tracked changes AND no
# commit has landed since the marker was recorded. (An mtime scan was WRONG — a git checkout /
# branch switch / worktree add rewrites the mtime of committed-but-unchanged files, e.g. the
# tracked, generated tools/display_preview.png, which then falsely read as "newer than the marker"
# and blocked a merge on an unchanged tree.) Commit new work, or leave edits uncommitted, and the
# marker goes stale — forcing a fresh review before the next merge.
#
# Only Claude Code sessions are gated; a human running `gh` in a plain terminal
# (or CI) is unaffected, since hooks run only inside Claude Code.
#
# Exit codes: 0 = allow the tool call, 2 = block it (stderr is fed back to Claude).

# Read the tool-call payload from stdin (PreToolUse JSON).
input="$(cat 2>/dev/null)"
tool="$(printf '%s' "$input" | jq -r '.tool_name // ""' 2>/dev/null)"
cmd="$(printf '%s' "$input" | jq -r '.tool_input.command // ""' 2>/dev/null)"

# Is this a gated merge? The MCP merge tool is gated unconditionally. A Bash call is gated
# only when it *invokes* `gh pr merge` at a command position — the command starts with
# `gh pr merge`, optionally behind a leading `cd <dir> &&`/`cd <dir>;` prefix. The phrase
# appearing merely as DATA (inside a commit message, a heredoc body, an echo/printf
# argument) is NOT matched — that used to cause false-positive blocks on harmless commands.
# Trade-off: a `gh pr merge` buried mid-compound-line (e.g. `foo && gh pr merge`) is not
# caught by this Bash matcher; in the web/remote environment the reliable gate is the MCP
# matcher above, and a local user can always touch the marker to bypass intentionally.
# Anything else (incl. `git commit` and `gh pr create`) falls through and is allowed.
action=""
case "$tool" in
  mcp__github__merge_pull_request)
    action="merge_pull_request (GitHub MCP)"
    ;;
  Bash)
    # Strip leading whitespace and an optional `cd <dir> &&`/`cd <dir>;` prefix, then
    # require the remainder to START with `gh pr merge` (flexible whitespace, any flags).
    norm="$(printf '%s' "$cmd" | sed -E 's/^[[:space:]]+//; s/^cd[[:space:]]+[^;&|]+(&&|;)[[:space:]]*//')"
    if printf '%s' "$norm" | grep -Eq '^gh[[:space:]]+pr[[:space:]]+merge([[:space:]]|$)'; then
      action="gh pr merge"
    fi
    ;;
esac
[ -n "$action" ] || exit 0

proj="${CLAUDE_PROJECT_DIR:-$PWD}"
marker="$proj/.claude/.project-review-passed"

# Portable file-mtime in epoch seconds. GNU coreutils first (`stat -c %Y`), then BSD/macOS
# (`stat -f %m`) — GNU-first because GNU `stat -f` means --file-system (wrong output), so we
# must try the GNU form before falling back to the BSD flag.
file_mtime() { stat -c %Y "$1" 2>/dev/null || stat -f %m "$1" 2>/dev/null; }

stale=""
reason=""
if [ -f "$marker" ]; then
  # Is the review still current? Judge by GIT CONTENT, not filesystem mtime. A git checkout /
  # branch switch / worktree add rewrites the mtime of committed-but-unchanged files (e.g. the
  # tracked, generated tools/display_preview.png), so the old `find -newer` mtime scan falsely
  # reported a clean tree as stale and blocked the merge. Instead the review is stale iff:
  #   (a) there are uncommitted tracked changes right now (git diff HEAD is non-empty), OR
  #   (b) a commit landed after the marker was recorded (HEAD committer time > marker mtime).
  # Nothing here scans sibling files, so the old mutual-marker chicken-and-egg cannot recur.
  if ! git -C "$proj" diff --quiet HEAD 2>/dev/null; then
    stale=1
    reason="uncommitted tracked changes since the review (git diff HEAD is non-empty)"
  else
    marker_ts="$(file_mtime "$marker")"
    head_ts="$(git -C "$proj" log -1 --format=%ct HEAD 2>/dev/null)"
    if [ -n "$marker_ts" ] && [ -n "$head_ts" ] && [ "$head_ts" -gt "$marker_ts" ]; then
      stale=1
      reason="a commit landed after the review was recorded (HEAD $(git -C "$proj" log -1 --format=%h HEAD 2>/dev/null))"
    fi
  fi
  [ -z "$stale" ] && exit 0   # tree clean AND no newer commit -> review is current -> allow
fi

# Blocked: tell Claude exactly what to do, then exit 2 to veto the tool call.
{
  echo "BLOCKED: \`$action\` requires a current project review first."
  echo
  if [ -n "$stale" ]; then
    echo "A review marker exists, but the tree moved on since it was recorded — the review is stale:"
    echo "    $reason"
  else
    echo "No project review has been recorded for the current working tree."
  fi
  echo
  echo "Do this before merging the PR:"
  echo "  1. Run the project-review skill:        /project-review"
  echo "  2. Once it passes with no blocking findings, record the pass:"
  echo "         touch \"$marker\""
  echo "  3. Re-run the $action command."
  echo
  echo "The marker stays valid while the tree is unchanged (no new commit, no uncommitted edits),"
  echo "so any later change forces a fresh review. To bypass intentionally, touch the marker yourself."
} >&2
exit 2
