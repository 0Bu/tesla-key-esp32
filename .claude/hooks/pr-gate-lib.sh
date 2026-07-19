#!/usr/bin/env bash
# Shared logic for the two PR-checkbox gates: require-project-review.sh (merge) and
# require-skill-audit.sh (PR create / push). Sourced, never run directly.
#
# THERE IS NO FILE MARKER. The pass-state of a review/audit lives as a TICKED, SHA-STAMPED
# checkbox in the pull request's body, e.g.:
#     - [x] `/project-review` clean — merge gate @ a1b2c3d4e5f6
# The gate reads that checkbox from the PR (for a merge/push) or from the body being submitted
# (for a PR-create call, no network needed) and allows the action only while the box is checked
# AND the stamped commit still matches the commit the action targets. Any new commit re-stales it
# (sha mismatch) — the git-native replacement for the old marker-vs-file-mtime staleness, which
# also ends the mutual-stale deadlock the two sibling markers used to cause (#162).
#
# Reading an EXISTING PR needs GitHub access: `gh` (local terminal) or, failing that, a REST call
# with ${GH_TOKEN:-$GITHUB_TOKEN} (web/remote). If neither is available the gate fails CLOSED with
# guidance — it never silently allows an unverified merge/push.
#
# All functions are pure/reusable; each gate script sets GATE_PROJ then calls them.

# gate_checkbox_status <content> <key>
#   Prints exactly one of:  "checked <sha>" | "checked" | "unchecked" | "absent"
#   A match is a markdown task-list item ("- [ ]" / "- [x]", any bullet) whose text contains
#   <key> (e.g. "project-review" / "skill-audit"). <sha> is the hex token (7..40) after an "@"
#   on that line, if present. <content> may be a PR body OR the raw create-call content (the
#   submitted body is embedded verbatim, so grepping it works regardless of shell quoting).
gate_checkbox_status() {
  local content="$1" key="$2" line
  # Match a markdown task-list item ("- [ ]" / "- [x]", any bullet) that is the REAL gate line:
  # it must mention <key> AND the word "gate" (the canonical lines read "… create/push gate @ <sha>"
  # / "… merge gate @ <sha>"). Requiring "gate" as well as the bare key stops an unrelated prose
  # checkbox that merely names the skill + a HEAD sha from satisfying the gate. NOT anchored to line
  # start: for a `gh pr create --body '...'` call the whole command is one line and the box sits
  # mid-line — the bullet+checkbox sequence is distinctive enough to find it there too.
  line="$(printf '%s\n' "$content" \
      | grep -iE '[-*][[:space:]]+\[[ xX]\]' \
      | grep -iF -- "$key" \
      | grep -iE 'gate' | head -n1)"
  [ -n "$line" ] || { printf 'absent\n'; return 0; }
  if printf '%s' "$line" | grep -qE '\[[xX]\]'; then
    local sha
    sha="$(printf '%s' "$line" | grep -oiE '@[[:space:]]*[0-9a-f]{7,40}' | head -n1 \
         | grep -oiE '[0-9a-f]{7,40}' | head -n1)"
    [ -n "$sha" ] && printf 'checked %s\n' "$sha" || printf 'checked\n'
  else
    printf 'unchecked\n'
  fi
}

# gate_sha_matches <a> <b>  -> 0 if one is a (>=7 char) case-insensitive prefix of the other.
gate_sha_matches() {
  local a b
  a="$(printf '%s' "$1" | tr 'A-F' 'a-f')"
  b="$(printf '%s' "$2" | tr 'A-F' 'a-f')"
  [ "${#a}" -ge 7 ] && [ "${#b}" -ge 7 ] || return 1
  case "$a" in "$b"*) return 0 ;; esac
  case "$b" in "$a"*) return 0 ;; esac
  return 1
}

# gate_head_sha  -> local HEAD (short 12), empty if not a git repo.
gate_head_sha() { git -C "${GATE_PROJ:-$PWD}" rev-parse --short=12 HEAD 2>/dev/null; }

# gate_branch  -> current branch name, empty if detached/not a repo.
gate_branch() { git -C "${GATE_PROJ:-$PWD}" rev-parse --abbrev-ref HEAD 2>/dev/null; }

# gate_repo_slug  -> owner/repo, from gh else the origin remote URL. Empty if undeterminable.
gate_repo_slug() {
  local s
  s="${GATE_REPO_SLUG:-}"
  [ -z "$s" ] && command -v gh >/dev/null 2>&1 && s="$(gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null)"
  # Extract owner/repo from the origin URL. Handles SSH (git@host:owner/repo), HTTPS
  # (https://host/owner/repo) AND the Claude-Code-on-the-web proxy form
  # (http://user@127.0.0.1:PORT/git/owner/repo) by strapping to the final two path segments.
  [ -z "$s" ] && s="$(git -C "${GATE_PROJ:-$PWD}" remote get-url origin 2>/dev/null \
        | sed -E 's#\.git$##; s#.*[/:]([^/:]+/[^/:]+)$#\1#')"
  printf '%s' "$s"
}

# gate_fetch_pr <selector>
#   Reads an EXISTING PR (selector: a PR number/URL, or empty for the current branch). On success
#   prints the head sha on line 1, then the PR body.
#   THREE-STATE exit — callers MUST tell these apart:
#     0  read OK (head+body printed)
#     1  confirmed NO open PR for this branch  (we HAD working access and the query came back empty)
#     2  could NOT read GitHub  (no gh AND no usable token, or the gh/API call errored/transient)
#   Collapsing 1 and 2 into "allow" is a FAIL-OPEN: a push to a branch that already has a PR would
#   slip through unaudited whenever GitHub is momentarily unreadable. The merge gate blocks on both;
#   the push gate allows only on 1 (positively no PR) and fails CLOSED on 2.
gate_fetch_pr() {
  local sel="$1" json rc tok slug num owner branch list cnt
  if command -v gh >/dev/null 2>&1; then
    if [ -n "$sel" ]; then
      json="$(gh pr view "$sel" --json body,headRefOid 2>/dev/null)"; rc=$?
      if [ "$rc" -eq 0 ] && [ -n "$json" ]; then
        printf '%s\n' "$(printf '%s' "$json" | jq -r '.headRefOid // ""')"
        printf '%s'   "$(printf '%s' "$json" | jq -r '.body // ""')"
        return 0
      fi
      return 2   # explicit selector but view failed -> treat as unreadable (merge gate blocks on 1|2)
    fi
    branch="$(gate_branch)"; [ -n "$branch" ] || return 2
    # `gh pr list` distinguishes "none" (exit 0, []) from "error" (non-zero) — unlike `gh pr view`.
    list="$(gh pr list --head "$branch" --state open --json number,body,headRefOid 2>/dev/null)"; rc=$?
    { [ "$rc" -eq 0 ] && [ -n "$list" ]; } || return 2
    cnt="$(printf '%s' "$list" | jq 'length' 2>/dev/null)"; [ -n "$cnt" ] || return 2
    [ "$cnt" = "0" ] && return 1
    printf '%s\n' "$(printf '%s' "$list" | jq -r '.[0].headRefOid // ""')"
    printf '%s'   "$(printf '%s' "$list" | jq -r '.[0].body // ""')"
    return 0
  fi
  # Token/REST fallback (web/remote, no gh). Needs a token + curl + jq + the repo slug.
  tok="${GH_TOKEN:-${GITHUB_TOKEN:-}}"
  { [ -n "$tok" ] && command -v curl >/dev/null 2>&1 && command -v jq >/dev/null 2>&1; } || return 2
  slug="$(gate_repo_slug)"; [ -n "$slug" ] || return 2
  if printf '%s' "$sel" | grep -qE '^[0-9]+$'; then
    json="$(curl -fsSL -H "Authorization: Bearer $tok" -H "Accept: application/vnd.github+json" \
        "https://api.github.com/repos/$slug/pulls/$sel" 2>/dev/null)" || return 2
    [ -n "$json" ] || return 2
  else
    branch="$(gate_branch)"; owner="${slug%%/*}"
    list="$(curl -fsSL -H "Authorization: Bearer $tok" -H "Accept: application/vnd.github+json" \
        "https://api.github.com/repos/$slug/pulls?head=$owner:$branch&state=open" 2>/dev/null)" || return 2
    cnt="$(printf '%s' "$list" | jq 'length' 2>/dev/null)"; [ -n "$cnt" ] || return 2
    [ "$cnt" = "0" ] && return 1
    json="$(printf '%s' "$list" | jq -c '.[0]' 2>/dev/null)"
  fi
  [ -n "$json" ] || return 2
  printf '%s\n' "$(printf '%s' "$json" | jq -r '.head.sha // .headRefOid // ""')"
  printf '%s'   "$(printf '%s' "$json" | jq -r '.body // ""')"
  return 0
}
