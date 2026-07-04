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
# This hook allows the merge only while that marker is newer than every source file —
# i.e. the review still reflects the code being shipped. Edit any file afterwards and
# the marker goes stale, forcing a fresh review before the next merge.
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

stale=""
newer=""
if [ -f "$marker" ]; then
  # First source file newer than the marker (if any) => review is stale.
  # macOS/BSD-find safe: no -quit; prune generated/vendored trees.
  newer="$(find "$proj" \
      \( -path '*/.git' -o -path '*/build' -o -path '*/build_mock' \
         -o -path '*/managed_components' -o -path '*/_site' \
         -o -path '*/.claude/worktrees' \) -prune -o \
      -type f ! -name '.project-review-passed' -newer "$marker" -print 2>/dev/null \
      | head -n 1)"
  if [ -z "$newer" ]; then
    exit 0   # marker is newer than every source file -> review is current -> allow
  fi
  stale=1
fi

# Blocked: tell Claude exactly what to do, then exit 2 to veto the tool call.
{
  echo "BLOCKED: \`$action\` requires a current project review first."
  echo
  if [ -n "$stale" ]; then
    echo "A review marker exists, but a file changed after it — the review is stale:"
    echo "    $newer"
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
  echo "The marker is valid only while no file changes after it, so any later edit"
  echo "forces a fresh review. To bypass intentionally, touch the marker yourself."
} >&2
exit 2
