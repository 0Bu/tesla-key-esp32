#!/usr/bin/env bash
# Regression test for the PR-gate command matcher (.claude/hooks/pr-gate-lib.sh).
#
# WHY THIS EXISTS. The three gates (project-review = merge, skill-audit = PR create/push,
# feature-docs = merge) all decide "is this tool call the action I guard?" by anchoring a regex
# at `^`. For a long time they ran that regex against the RAW command, so `grep` anchored per
# PHYSICAL LINE and a CHAINED action matched nothing at all:
#
#     git commit -m x AND-AND git push origin br   ->  no match
#
# An unmatched command is not a lenient classification, it is NO classification: the hook falls
# through `[ -n "$action" ] || exit 0` and allows the call having checked nothing. That let a
# `git push` through at a commit whose PR stamp was stale, and would let
# `git checkout main AND-AND gh pr merge …` past the MERGE gate — the one protecting main.
# gate_bash_actions() fixes it; this file pins the behaviour so the hole cannot reopen quietly.
# It also pins the fail-closed rule for multiple guarded actions: validating only the first merge
# in a compound command would leave every later merge unchecked.
#
# Offline and deterministic: the matcher cases are pure, and the end-to-end cases use only the
# CREATE path, which inspects the submitted body directly and never touches GitHub.
#
# A NOTE ON THE ODD STRING BUILDING BELOW. The guarded literals are assembled from variables
# rather than written out. The segmenter is deliberately conservative — it splits inside quotes
# too — so a file (or a shell line) that merely CONTAINS `<sep> gh pr create …` is enough to make
# the live hook classify the harness itself as a PR creation and block it. The matcher intentionally
# fails conservatively for such guarded-looking quoted data; the cost is exactly this awkwardness
# when writing about the thing being gated.
set -uo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=/dev/null
. .claude/hooks/pr-gate-lib.sh

fails=0
n=0

SEP='&''&'          # the chaining operator, assembled so it is not a literal here
GHPR='gh pr'        # ditto for the guarded commands
GITP='git push'

# matches <cmd> <create|merge|push> -> "yes" | "no", exactly as the hooks decide it.
matches() {
  if gate_bash_actions "$1" | awk -F '\t' -v kind="$2" '$1 == kind { found=1 } END { exit !found }'; then
    printf 'yes'
  else
    printf 'no'
  fi
}

action_count() { # action_count <cmd> <create|merge|push>
  gate_bash_actions "$1" | awk -F '\t' -v kind="$2" '$1 == kind { n++ } END { print n+0 }'
}

check() {  # check <yes|no> <create|merge|push> <cmd> <label>
  local want="$1" kind="$2" cmd="$3" label="$4" got
  n=$((n + 1))
  got="$(matches "$cmd" "$kind")"
  if [ "$got" != "$want" ]; then
    fails=$((fails + 1))
    printf 'FAIL  %s\n      want=%s got=%s\n      cmd=%s\n' "$label" "$want" "$got" "$cmd"
  fi
}

# ── the plain forms must still match (no regression on what already worked) ──
check yes push   "$GITP origin br"                    'push, alone'
check yes push   "$GITP -q origin br"                 'push, with a flag'
check yes push   "   $GITP origin br"                 'push, leading whitespace'
check yes push   "$GITP origin br >/dev/null"         'push, redirected'
check yes create "$GHPR create --base main --title x" 'create, alone'
check yes merge  "$GHPR merge 220 --squash"           'merge, alone'

# ── THE BUG: a chained action must be seen. Each of these previously matched NOTHING. ──
check yes push   "echo hi $SEP $GITP origin br"                    'push after a chain'
check yes push   "git commit -m x $SEP $GITP origin br"            'commit then push'
check yes push   "$GHPR edit 9 --body-file b.md $SEP $GITP"        'pr edit then push'
check yes push   "cd /tmp $SEP $GITP origin br"                    'cd then push'
check yes push   "true; $GITP origin br"                           'push after a semicolon'
check yes push   "GIT_TRACE=1 $GITP origin br"                     'push behind an assignment'
check yes create "echo hi $SEP $GHPR create --base main"           'create after a chain'
check yes merge  "git checkout main $SEP $GHPR merge 220 --squash" 'checkout then merge'
check yes merge  "$GHPR update-branch 220 $SEP $GHPR merge 220"    'update-branch then merge'

# ── common wrappers/grouping must not turn into alternate spellings of the bypass ──
check yes push 'env GIT_TRACE=1 git push origin br' 'push behind env'
check yes push 'env -u UNUSED git push origin br'   'push behind env -u'
check yes push 'env --unset=UNUSED git push origin br' 'push behind env --unset'
check yes push 'command git push origin br'         'push behind command'
check yes push 'git -C /tmp push origin br'         'push with git -C'
check yes push '(git push origin br)'               'push in a subshell'
check yes push 'if true; then git push origin br; fi' 'push in an if body'

# ── a newline-separated action already worked; keep it working ──
check yes push "git commit -m x
$GITP origin br"                                                          'push on its own line'

# ── and must NOT over-match: these are not the guarded action ──
check no push   'git pushd origin br'          'pushd is not push'
check no push   "echo \"$GITP origin br\""     'push only as a quoted argument'
check no push   'git commit -m push'           'push only as a commit argument'
check no push   'git commit -m x'              'commit alone'
check no merge  "$GHPR view 220 --json body"   'pr view is not merge'
check no merge  "$GHPR merge-queue status"     'merge-queue is not merge'
check no create "$GHPR edit 220 --body-file b" 'pr edit is not create'

# ── every guarded action is reported; callers reject more than one instead of checking first ──
n=$((n + 1))
if [ "$(action_count "$GHPR merge 1 || $GHPR merge 2" merge)" != "2" ]; then
  fails=$((fails + 1)); printf 'FAIL  two merge actions must both be reported\n'
fi
n=$((n + 1))
if [ "$(gate_bash_actions "$GHPR create --title x $SEP $GITP" | wc -l | tr -d ' ')" != "2" ]; then
  fails=$((fails + 1)); printf 'FAIL  create + push must both be reported\n'
fi

# ── END-TO-END: the real hook's verdict, not just the matcher ──
# CREATE path only: it inspects the submitted body directly, so no network and no repo state.
hook_verdict() {
  printf '%s' "$1" | jq -Rs '{tool_name:"Bash", tool_input:{command:.}}' \
    | CLAUDE_PROJECT_DIR="$PWD" bash .claude/hooks/require-skill-audit.sh >/dev/null 2>&1
  printf '%s' "$?"
}

merge_hook_verdict() {
  printf '%s' "$1" | jq -Rs '{tool_name:"Bash", tool_input:{command:.}}' \
    | CLAUDE_PROJECT_DIR="$PWD" bash .claude/hooks/require-project-review.sh >/dev/null 2>&1
  printf '%s' "$?"
}

e2e() {  # e2e <expected-exit> <cmd> <label>
  local want="$1" cmd="$2" label="$3" got
  n=$((n + 1))
  got="$(hook_verdict "$cmd")"
  if [ "$got" != "$want" ]; then
    fails=$((fails + 1))
    printf 'FAIL  %s\n      want exit=%s got exit=%s\n      cmd=%s\n' "$label" "$want" "$got" "$cmd"
  fi
}

head_sha="$(git rev-parse --short=12 HEAD 2>/dev/null)"
if [ -n "$head_sha" ] && command -v jq >/dev/null 2>&1; then
  box="- [x] \`/skill-audit\` clean - PR create/push gate @ ${head_sha}"
  e2e 2 "echo hi $SEP $GHPR create --base main --title x --body 'nothing here'" \
      'chained create, no checkbox -> BLOCKED'
  e2e 2 "echo hi $SEP $GHPR create --base main --title x --body '- [ ] /skill-audit gate'" \
      'chained create, unticked box -> BLOCKED'
  e2e 0 "echo hi $SEP $GHPR create --base main --title x --body '${box}'" \
      'chained create, ticked+stamped at HEAD -> allowed'
  e2e 2 "$GHPR create --base main --body '${box}' $SEP $GITP" \
      'multiple publish actions -> BLOCKED'
  n=$((n + 1))
  if [ "$(merge_hook_verdict "$GHPR merge 1 || $GHPR merge 2")" != "2" ]; then
    fails=$((fails + 1)); printf 'FAIL  multiple merge actions must be blocked before network\n'
  fi
  e2e 0 'git status --short' 'an unrelated command is not gated'
fi

if [ "$fails" -eq 0 ]; then
  printf 'OK  pr-gate matcher: %d cases\n' "$n"
  exit 0
fi
printf 'FAILED  %d of %d pr-gate matcher cases\n' "$fails" "$n"
exit 1
