#!/usr/bin/env bash
# PreToolUse(Bash) gate: refuse `gh pr merge` until a project review has been run
# against the *current* working tree. Plain `git commit` and `gh pr create` are NOT
# gated — the review runs only before merging a PR into main.
#
# Mechanism: after running /project-review and confirming it passes with no blocking
# findings, record the pass by touching:
#     .claude/.project-review-passed
# This hook allows the PR action only while that marker is newer than every source
# file — i.e. the review still reflects the code being shipped. Edit any file
# afterwards and the marker goes stale, forcing a fresh review before the next PR.
#
# Only Claude Code sessions are gated; a human running `gh` in a plain terminal
# (or CI) is unaffected, since hooks run only inside Claude Code.
#
# Exit codes: 0 = allow the tool call, 2 = block it (stderr is fed back to Claude).

# Read the tool-call payload from stdin (PreToolUse JSON).
input="$(cat 2>/dev/null)"
cmd="$(printf '%s' "$input" | jq -r '.tool_input.command // ""' 2>/dev/null)"

# Is this the gated command? Flexible whitespace; tolerant of flags and of compound
# `... && gh pr merge ...` lines. Anything else (incl. `git commit` and `gh pr create`)
# falls through and is allowed.
action=""
if printf '%s' "$cmd" | grep -Eq '(^|[^[:alnum:]_/.-])gh[[:space:]]+pr[[:space:]]+merge([[:space:]]|$)'; then
  action="gh pr merge"
fi
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
