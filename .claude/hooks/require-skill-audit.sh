#!/usr/bin/env bash
# PreToolUse gate: refuse to *publish to a PR* — creating a PR or pushing a commit to it —
# until a skill-audit has been run against the *current* working tree, so no skill or agent
# under .claude/ leaves for review out of sync with the project it maps. Plain `git commit`
# is NOT gated, and neither is the *merge* into main (that is the sibling project-review gate,
# require-project-review.sh). The audit runs at PR-publish time — when code first goes up for
# review (`gh pr create`) and on every subsequent push to the branch.
#
# Four publish actions are gated, so the gate holds in BOTH environments:
#   • Bash `gh pr create ...`             — open a PR from a local terminal session
#   • Bash `git push ...`                 — push commits to the PR branch (local terminal)
#   • mcp__github__create_pull_request    — Claude Code on the web / remote (no `gh` CLI;
#   • mcp__github__push_files               PR/push go through the GitHub MCP server)
# Matched via the `matcher` entries in .claude/settings.json that invoke this script.
#
# Mechanism: after running /skill-audit and confirming every skill + agent matches the
# project (drift corrected in place), record the pass by touching:
#     .claude/.skill-audit-passed
# This hook allows the publish only while that marker is newer than every source file —
# i.e. the audit still reflects the tree being pushed. Edit any file afterwards and the
# marker goes stale, forcing a fresh audit before the next PR create / push.
#
# skill-audit ⊂ project-review: a full /project-review also audits the skills, so it
# records BOTH markers (see the project-review skill). Running /skill-audit alone records
# only this one — the merge gate still needs a current project-review.
#
# Only Claude Code sessions are gated; a human running `gh`/`git` in a plain terminal (or CI)
# is unaffected, since hooks run only inside Claude Code.
#
# Exit codes: 0 = allow the tool call, 2 = block it (stderr is fed back to Claude).

# Read the tool-call payload from stdin (PreToolUse JSON).
input="$(cat 2>/dev/null)"
tool="$(printf '%s' "$input" | jq -r '.tool_name // ""' 2>/dev/null)"
cmd="$(printf '%s' "$input" | jq -r '.tool_input.command // ""' 2>/dev/null)"

# Is this a gated publish? The MCP PR/push tools are gated unconditionally. A Bash call is
# gated only when it *invokes* `gh pr create` or `git push` at a command position — the
# command starts with that verb, optionally behind a leading `cd <dir> &&`/`cd <dir>;` prefix.
# The phrase appearing merely as DATA (inside a commit message, a heredoc body, an echo/printf
# argument) is NOT matched — that would cause false-positive blocks on harmless commands.
# Trade-off: a `git push` buried mid-compound-line (e.g. `git commit -m .. && git push`) is not
# caught by this Bash matcher; in the web/remote environment the reliable gate is the MCP
# matcher above, and a local user can always touch the marker to bypass intentionally.
# Anything else (incl. `git commit` and `gh pr merge`) falls through and is allowed.
action=""
case "$tool" in
  mcp__github__create_pull_request)
    action="create_pull_request (GitHub MCP)"
    ;;
  mcp__github__push_files)
    action="push_files (GitHub MCP)"
    ;;
  Bash)
    # Strip leading whitespace and an optional `cd <dir> &&`/`cd <dir>;` prefix, then require
    # the remainder to START with `gh pr create` or `git push` (flexible whitespace, any flags).
    norm="$(printf '%s' "$cmd" | sed -E 's/^[[:space:]]+//; s/^cd[[:space:]]+[^;&|]+(&&|;)[[:space:]]*//')"
    if printf '%s' "$norm" | grep -Eq '^gh[[:space:]]+pr[[:space:]]+create([[:space:]]|$)'; then
      action="gh pr create"
    elif printf '%s' "$norm" | grep -Eq '^git[[:space:]]+push([[:space:]]|$)'; then
      action="git push"
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
  # EXCLUDE BOTH gate markers — this one AND the sibling .project-review-passed. They are gate
  # state, not source, and a clean /project-review touches both (in some order). Counting the
  # sibling as a "source file" would leave whichever was touched a hair earlier permanently
  # newer-than → this gate forever stale: a mutual chicken-and-egg that blocks every publish.
  newer="$(find "$proj" \
      \( -path '*/.git' -o -path '*/build' -o -path '*/build_mock' \
         -o -path '*/managed_components' -o -path '*/_site' \
         -o -path '*/.claude/worktrees' \) -prune -o \
      -type f ! -name '.skill-audit-passed' ! -name '.project-review-passed' \
      -newer "$marker" -print 2>/dev/null \
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
  echo "Do this before creating/pushing the PR:"
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
