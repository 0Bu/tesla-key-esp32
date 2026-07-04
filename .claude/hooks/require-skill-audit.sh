#!/usr/bin/env bash
# PreToolUse gate: refuse a PR *merge* until a skill-audit has been run against the
# *current* working tree — so no skill or agent under .claude/ ships out of sync with
# the project it maps. Plain `git commit` and `gh pr create` are NOT gated; the audit
# runs only before merging a PR into main. Sibling gate to require-project-review.sh.
#
# Two merge paths are gated, so the gate holds in BOTH environments:
#   • Bash `gh pr merge ...`              — local terminal sessions
#   • mcp__github__merge_pull_request     — Claude Code on the web / remote (no `gh`
#                                           CLI; merges go through the GitHub MCP server)
# Matched via the `matcher` entries in .claude/settings.json that both invoke this script.
#
# Mechanism: after running /skill-audit and confirming every skill + agent matches the
# project (drift corrected in place), record the pass by touching:
#     .claude/.skill-audit-passed
# This hook allows the merge only while that marker is newer than every source file —
# i.e. the audit still reflects the tree being shipped. Edit any file afterwards and the
# marker goes stale, forcing a fresh audit before the next merge.
#
# skill-audit ⊂ project-review: a full /project-review also audits the skills, so it
# records BOTH markers (see the project-review skill). Running /skill-audit alone records
# only this one — you still need a current project-review to clear the other gate.
#
# Only Claude Code sessions are gated; a human running `gh` in a plain terminal (or CI)
# is unaffected, since hooks run only inside Claude Code.
#
# Exit codes: 0 = allow the tool call, 2 = block it (stderr is fed back to Claude).

# Read the tool-call payload from stdin (PreToolUse JSON).
input="$(cat 2>/dev/null)"
tool="$(printf '%s' "$input" | jq -r '.tool_name // ""' 2>/dev/null)"
cmd="$(printf '%s' "$input" | jq -r '.tool_input.command // ""' 2>/dev/null)"

# Is this a gated merge? The MCP merge tool is gated unconditionally; a Bash call is
# gated only when it actually runs `gh pr merge` (flexible whitespace; tolerant of
# flags and of compound `... && gh pr merge ...` lines). Anything else (incl.
# `git commit` and `gh pr create`) falls through and is allowed.
action=""
case "$tool" in
  mcp__github__merge_pull_request)
    action="merge_pull_request (GitHub MCP)"
    ;;
  Bash)
    if printf '%s' "$cmd" | grep -Eq '(^|[^[:alnum:]_/.-])gh[[:space:]]+pr[[:space:]]+merge([[:space:]]|$)'; then
      action="gh pr merge"
    fi
    ;;
esac
[ -n "$action" ] || exit 0

proj="${CLAUDE_PROJECT_DIR:-$PWD}"
marker="$proj/.claude/.skill-audit-passed"

stale=""
newer=""
if [ -f "$marker" ]; then
  # First source file newer than the marker (if any) => audit is stale.
  # macOS/BSD-find safe: no -quit; prune generated/vendored trees.
  newer="$(find "$proj" \
      \( -path '*/.git' -o -path '*/build' -o -path '*/build_mock' \
         -o -path '*/managed_components' -o -path '*/_site' \
         -o -path '*/.claude/worktrees' \) -prune -o \
      -type f ! -name '.skill-audit-passed' -newer "$marker" -print 2>/dev/null \
      | head -n 1)"
  if [ -z "$newer" ]; then
    exit 0   # marker is newer than every source file -> audit is current -> allow
  fi
  stale=1
fi

# Blocked: tell Claude exactly what to do, then exit 2 to veto the tool call.
{
  echo "BLOCKED: \`$action\` requires a current skill-audit first."
  echo
  if [ -n "$stale" ]; then
    echo "A skill-audit marker exists, but a file changed after it — the audit is stale:"
    echo "    $newer"
  else
    echo "No skill-audit has been recorded for the current working tree."
  fi
  echo
  echo "Do this before merging the PR:"
  echo "  1. Run the skill-audit skill:           /skill-audit"
  echo "     (a full /project-review also satisfies this gate — it records both markers.)"
  echo "  2. Once every skill + agent matches the project (drift corrected), record it:"
  echo "         touch \"$marker\""
  echo "  3. Re-run the $action command."
  echo
  echo "The marker is valid only while no file changes after it, so any later edit"
  echo "forces a fresh audit. To bypass intentionally, touch the marker yourself."
} >&2
exit 2
