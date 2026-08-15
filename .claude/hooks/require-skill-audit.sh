#!/usr/bin/env bash
# PreToolUse gate: refuse to *publish to a PR* — creating a PR, or pushing to a branch that
# already has one — until a skill-audit has run against the commit being published, so no skill
# or agent under .claude/ leaves for review out of sync with the project it maps. Plain
# `git commit` is NOT gated, and neither is the *merge* into main (that is the sibling
# require-project-review.sh).
#
# Four publish actions are classified. Bash actions can satisfy the gate; structured GitHub
# actions additionally need exact repository/ref binding, and push_files is refused because its
# server-created commit does not exist when PreToolUse runs:
#   • Bash `gh pr create ...`             — open a PR from a local terminal session
#   • Bash `git push ...`                 — push commits to the PR branch (local terminal)
#   • mcp__github__create_pull_request    — exact local owner/repo + pushed current HEAD only
#   • mcp__github__push_files             — always blocked (future server commit is unbindable)
# Matched via the `matcher` entries in .claude/settings.json that invoke this script.
#
# Mechanism (NO file marker — see pr-gate-lib.sh): after running /skill-audit and confirming every
# skill + agent matches the project (drift corrected in place), record the pass by TICKING the PR
# checklist box and STAMPING it with the audited commit:
#     - [x] `/skill-audit` clean — PR create/push gate @ <short-sha>
#   • create: exactly one literal body/body-file must already contain the unique ticked+stamped
#     box (checked without network; title, other args and dynamic/multiple body sources never count).
#   • push: the box must be ticked+stamped in the existing PR's body, matching the commit you push.
#     A push to a branch that has NO PR yet is allowed — it is not "publishing to a PR"; the create
#     gate is the chokepoint. Every guarded Bash action must be standalone. Pushes are deliberately
#     current-project/current-branch/verified-origin-only: shell/env/cwd/Git context changes,
#     path-qualified executables, a foreign source/remote/destination or multiple refspecs fail
#     closed before GitHub access.
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
if ! . "$proj/.claude/hooks/pr-gate-lib.sh" 2>/dev/null; then
  echo "BLOCKED: skill-audit gate library could not be loaded." >&2
  exit 2
fi
for gate_fn in gate_bash_actions gate_fetch_pr gate_checkbox_status gate_sha_matches \
               gate_head_sha gate_push_head_sha gate_pr_create_body gate_mcp_repo_matches \
               gate_mcp_create_head; do
  if [ "${GATE_PR_LIB_API:-}" != 1 ] || ! declare -F "$gate_fn" >/dev/null 2>&1; then
    echo "BLOCKED: skill-audit gate library is incomplete ($gate_fn)." >&2
    exit 2
  fi
done

# Read the tool-call payload from stdin (PreToolUse JSON).
input="$(cat 2>/dev/null)"
tool="$(printf '%s' "$input" | jq -r '.tool_name // ""' 2>/dev/null)"
cmd="$(printf '%s'  "$input" | jq -r '.tool_input.command // ""' 2>/dev/null)"

# Classify the action: "create" (inspect the submitted body, no network) vs "push" (inspect the
# existing PR). See the header for what each Bash matcher does and does not catch.
action=""; kind=""; content=""; create_spec=""; push_spec=""; push_head=""
case "$tool" in
  mcp__github__create_pull_request)
    action="create_pull_request (GitHub MCP)"; kind="create"
    push_head="$(gate_mcp_create_head "$input")" || {
      {
        echo "BLOCKED: GitHub MCP PR creation is not bound to this repository/current remote HEAD."
        echo
        echo "owner/repo and head must exactly match this worktree's origin/current branch, and"
        echo "the remote branch must already resolve to local HEAD. Missing or unreadable state blocks."
      } >&2
      exit 2
    }
    content="$(printf '%s' "$input" \
      | jq -er '.tool_input.body | strings' 2>/dev/null)" || exit 2
    ;;
  mcp__github__push_files)
    {
      echo "BLOCKED: GitHub MCP push_files creates a new server-side commit after PreToolUse."
      echo
      echo "That future commit cannot match the local skill-audit SHA. Commit locally and use the"
      echo "standalone, current-HEAD-only \`git push origin <current-branch>\` path instead."
    } >&2
    exit 2
    ;;
  Bash)
    records="$(gate_bash_actions "$cmd")"
    if printf '%s\n' "$records" | grep -Fq $'\t__GATE_UNSAFE_SHELL_CONTEXT__'; then
      {
        echo "BLOCKED: guarded create/push must be the only shell action in its Bash tool call."
        echo
        echo "A preceding or following command can mutate HEAD, Git config, cwd or the PR body"
        echo "after PreToolUse validates it. Run the create/push as a separate Bash call."
      } >&2
      exit 2
    fi
    publish_records="$(printf '%s\n' "$records" | grep -E $'^(create|push)\t' || true)"
    publish_count=0
    [ -z "$publish_records" ] || publish_count="$(printf '%s\n' "$publish_records" | wc -l | tr -d ' ')"
    if [ "$publish_count" -gt 1 ]; then
      {
        echo "BLOCKED: multiple PR publish actions in one Bash call."
        echo
        echo "Run each create/push as a separate tool call so every target and audit stamp is verified."
      } >&2
      exit 2
    fi
    if [ "$publish_count" -eq 1 ]; then
      kind="${publish_records%%$'\t'*}"
      case "$kind" in
        create) action="gh pr create"; create_spec="${publish_records#*$'\t'}" ;;
        push) action="git push"; push_spec="${publish_records#*$'\t'}" ;;
      esac
    fi
    if [ "$kind" = "create" ]; then
      content="$(gate_pr_create_body "$create_spec")" || {
        {
          echo "BLOCKED: \`gh pr create\` needs --body/-b or --body-file/-F as its first argument."
          echo
          echo "Only the actual PR body can satisfy the skill-audit checkbox; title, other arguments,"
          echo "dynamic shell expansions, stdin, symlinks and multiple/ambiguous body sources are refused."
        } >&2
        exit 2
      }
    fi
    ;;
esac
[ -n "$action" ] || exit 0

key="skill-audit"; skill="/skill-audit"
head_sha="$(gate_head_sha)"

# ── PUSH: verify against the existing PR. Distinguish "no PR yet" (allow — the create gate is the
# chokepoint) from "PR unreadable" (fail CLOSED, mirroring the merge gate — never allow an
# unaudited push to a branch that may already have an open PR just because GitHub is unreadable). ─
if [ "$kind" = "push" ]; then
  if [ "$action" = "git push" ]; then
    push_head="$(gate_push_head_sha "$push_spec")" || {
      {
        echo "BLOCKED: \`git push\` must publish only current HEAD to the current branch."
        echo
        echo "Git/env/cwd repo/config overrides, path-qualified executables, a non-origin remote,"
        echo "non-current/multiple refspecs, another destination branch, tag/all/mirror/delete"
        echo "pushes and ambiguous custom push configuration cannot match this PR's audit stamp."
        echo "Retry explicitly as: git push origin $(gate_branch)"
      } >&2
      exit 2
    }
  fi
  pr="$(gate_fetch_pr "")"; fetch_rc=$?
  case "$fetch_rc" in
    0) : ;;             # PR exists & read OK -> verify the checkbox below
    1) exit 0 ;;        # confirmed NO open PR for this branch -> not publishing to a PR yet -> allow
    *)                  # 2 = could not read GitHub -> fail CLOSED
      {
        echo "BLOCKED: \`$action\` — could not read the pull request to verify the $skill gate."
        echo
        echo "This branch may already have an open PR, but GitHub was unreadable (needs \`gh\`, or"
        echo "\${GH_TOKEN}/\${GITHUB_TOKEN} in web/remote), so the audit state can't be confirmed. The"
        echo "gate fails closed rather than let an unaudited push through — set a token / use \`gh\`,"
        echo "or push from a session with GitHub access."
      } >&2
      exit 2 ;;
  esac
  pr_head="$(printf '%s' "$pr" | head -n1)"
  content="$(printf '%s' "$pr" | tail -n +2)"
  anchor="${push_head:-${head_sha:-$pr_head}}"
else
  anchor="${push_head:-$head_sha}"
fi

status="$(gate_checkbox_status "$content" "$key")"
box_state="${status%% *}"; box_sha=""
[ "$box_state" = "checked" ] && box_sha="$(printf '%s' "$status" | awk '{print $2}')"

# Allow only when the box is ticked and its stamp matches the exact commit being published.
if [ "$box_state" = "checked" ] && [ -n "$box_sha" ] && [ -n "$anchor" ]; then
  if gate_sha_matches "$box_sha" "$anchor"; then
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
