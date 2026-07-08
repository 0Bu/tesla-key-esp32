#!/usr/bin/env bash
# PreToolUse gate: refuse to *publish to a PR* — creating a PR, or pushing to a branch that
# already has one — until a skill-audit has run against the commit being published, so no skill
# or agent under .claude/ leaves for review out of sync with the project it maps. Plain
# `git commit` is NOT gated, and neither is the *merge* into main (that is the sibling
# require-project-review.sh).
#
# Four publish actions are gated, so the gate holds in BOTH environments:
#   • Bash `gh pr create ...`             — open a PR from a local terminal session
#   • Bash `git push ...`                 — push commits to the PR branch (local terminal)
#   • mcp__github__create_pull_request    — Claude Code on the web / remote (no `gh` CLI)
#   • mcp__github__push_files
# Matched via the `matcher` entries in .claude/settings.json that invoke this script.
#
# Mechanism (NO file marker — see pr-gate-lib.sh): after running /skill-audit and confirming every
# skill + agent matches the project (drift corrected in place), record the pass by TICKING the PR
# checklist box and STAMPING it with the audited commit:
#     - [x] `/skill-audit` clean — PR create/push gate @ <short-sha>
#   • create: the box must already be ticked+stamped in the body you submit (checked here with no
#     network — the submitted body is inspected directly).
#   • push: the box must be ticked+stamped in the existing PR's body, matching the commit you push.
#     A push to a branch that has NO PR yet is allowed — it is not "publishing to a PR"; the create
#     gate is the chokepoint.
#
# skill-audit ⊂ project-review: a full /project-review also audits the skills, so a clean review
# lets you tick BOTH boxes. /skill-audit alone ticks only this one — the merge still needs a
# current project-review.
#
# Only Claude Code sessions are gated; a human running `gh`/`git` in a plain terminal (or the
# GitHub UI) is unaffected, since hooks run only inside Claude Code.
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

# Classify the action: "create" (inspect the submitted body, no network) vs "push" (inspect the
# existing PR). See the header for what each Bash matcher does and does not catch.
action=""; kind=""; content=""
case "$tool" in
  mcp__github__create_pull_request)
    action="create_pull_request (GitHub MCP)"; kind="create"
    content="$(printf '%s' "$input" | jq -r '(.tool_input.body // "") + "\n" + (.tool_input.title // "")' 2>/dev/null)"
    ;;
  mcp__github__push_files)
    action="push_files (GitHub MCP)"; kind="push"
    ;;
  Bash)
    norm="$(printf '%s' "$cmd" | sed -E 's/^[[:space:]]+//; s/^cd[[:space:]]+[^;&|]+(&&|;)[[:space:]]*//')"
    if printf '%s' "$norm" | grep -Eq '^gh[[:space:]]+pr[[:space:]]+create([[:space:]]|$)'; then
      action="gh pr create"; kind="create"
      # The inline --body "..." is embedded in the command verbatim; also fold in a --body-file.
      content="$cmd"
      bf="$(printf '%s' "$cmd" | grep -oE -- '(--body-file|-F)[[:space:]]+[^[:space:]]+' | head -n1 \
            | sed -E 's/^(--body-file|-F)[[:space:]]+//')"
      if [ -n "$bf" ] && [ "$bf" != "-" ] && [ -f "$bf" ]; then
        content="$content"$'\n'"$(cat "$bf" 2>/dev/null)"
      fi
    elif printf '%s' "$norm" | grep -Eq '^git[[:space:]]+push([[:space:]]|$)'; then
      action="git push"; kind="push"
    fi
    ;;
esac
[ -n "$action" ] || exit 0

key="skill-audit"; skill="/skill-audit"
head_sha="$(gate_head_sha)"

# ── PUSH: verify against the existing PR; allow if the branch has no PR yet. ───────────────
if [ "$kind" = "push" ]; then
  pr="$(gate_fetch_pr "")" || exit 0   # no open PR for this branch (or unreadable) -> create gate is the chokepoint
  pr_head="$(printf '%s' "$pr" | head -n1)"
  content="$(printf '%s' "$pr" | tail -n +2)"
  anchor="${head_sha:-$pr_head}"
else
  anchor="$head_sha"
fi

status="$(gate_checkbox_status "$content" "$key")"
box_state="${status%% *}"; box_sha=""
[ "$box_state" = "checked" ] && box_sha="$(printf '%s' "$status" | awk '{print $2}')"

# Allow when the box is ticked and its stamp matches the commit being published. If no local HEAD
# is resolvable (anchor empty), accept a ticked+stamped box on trust — we can't compute freshness.
if [ "$box_state" = "checked" ] && [ -n "$box_sha" ]; then
  if [ -z "$anchor" ] || gate_sha_matches "$box_sha" "$anchor"; then
    exit 0
  fi
fi

# Blocked: tell Claude exactly what to do, then exit 2 to veto the tool call.
{
  echo "BLOCKED: \`$action\` requires a current skill-audit recorded in the PR."
  echo
  case "$box_state" in
    absent)    echo "No ticked \`$skill\` checkbox was found in the ${kind} content." ;;
    unchecked) echo "The \`$skill\` checkbox is present but unticked." ;;
    checked)
      if [ -z "$box_sha" ]; then
        echo "The \`$skill\` box is ticked but carries no \`@ <sha>\` stamp."
      else
        echo "The \`$skill\` box is stamped @ $box_sha but the commit being published is ${anchor:-unknown} — stale."
      fi ;;
  esac
  echo
  echo "Do this before creating/pushing the PR:"
  echo "  1. Run the skill-audit skill:           $skill"
  echo "     (a full /project-review also satisfies this gate — it audits the skills too.)"
  echo "  2. Once every skill + agent matches the project (drift corrected), tick + stamp the box"
  echo "     with the head commit (\`git rev-parse --short=12 HEAD\`), e.g.:"
  echo "         - [x] \`$skill\` clean — PR create/push gate @ ${head_sha:-<sha>}"
  echo "     For a NEW PR, put that line in the body you submit. For an existing PR, edit its body"
  echo "     (\`gh pr edit <pr> --body-file <file>\`, or the GitHub MCP update tool) before pushing."
  echo "  3. Re-run the $action command."
  echo
  echo "The stamp is valid only while it matches the pushed commit, so any later commit forces a"
  echo "fresh audit before the next PR create / push."
} >&2
exit 2
