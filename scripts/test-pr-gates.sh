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
# gate_bash_segments() fixes it; this file pins the behaviour so the hole cannot reopen quietly.
#
# Offline and deterministic: the matcher cases are pure, and the end-to-end cases use only the
# CREATE path, which inspects the submitted body directly and never touches GitHub.
#
# A NOTE ON THE ODD STRING BUILDING BELOW. The guarded literals are assembled from variables
# rather than written out. The segmenter is deliberately conservative — it splits inside quotes
# too — so a file (or a shell line) that merely CONTAINS `<sep> gh pr create …` is enough to make
# the live hook classify the harness itself as a PR creation and block it. Over-splitting can
# only ever cause an extra block, never a missed one, which is the right direction for a gate to
# fail; the cost is exactly this awkwardness when writing about the thing being gated.
set -uo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=/dev/null
. .claude/hooks/pr-gate-lib.sh

fails=0
n=0

SEP='&''&'          # the chaining operator, assembled so it is not a literal here
GHPR='gh pr'        # ditto for the guarded commands
GITP='git push'

# The EREs the hooks actually use, kept verbatim so this test moves when they do.
ERE_PUSH='git[[:space:]]+push'
ERE_CREATE='gh[[:space:]]+pr[[:space:]]+create'
ERE_MERGE='gh[[:space:]]+pr[[:space:]]+merge'

# matches <cmd> <ere> -> "yes" | "no", exactly as the hooks decide it.
matches() {
  if gate_bash_segments "$1" | grep -Eq "^$2([[:space:]]|$)"; then printf 'yes'; else printf 'no'; fi
}

check() {  # check <yes|no> <ere> <cmd> <label>
  local want="$1" ere="$2" cmd="$3" label="$4" got
  n=$((n + 1))
  got="$(matches "$cmd" "$ere")"
  if [ "$got" != "$want" ]; then
    fails=$((fails + 1))
    printf 'FAIL  %s\n      want=%s got=%s\n      cmd=%s\n' "$label" "$want" "$got" "$cmd"
  fi
}

# ── the plain forms must still match (no regression on what already worked) ──
check yes "$ERE_PUSH"   "$GITP origin br"                        'push, alone'
check yes "$ERE_PUSH"   "$GITP -q origin br"                     'push, with a flag'
check yes "$ERE_PUSH"   "   $GITP origin br"                     'push, leading whitespace'
check yes "$ERE_PUSH"   "$GITP origin br >/dev/null"             'push, redirected'
check yes "$ERE_CREATE" "$GHPR create --base main --title x"     'create, alone'
check yes "$ERE_MERGE"  "$GHPR merge 220 --squash"               'merge, alone'

# ── THE BUG: a chained action must be seen. Each of these previously matched NOTHING. ──
check yes "$ERE_PUSH"   "echo hi $SEP $GITP origin br"                    'push after a chain'
check yes "$ERE_PUSH"   "git commit -m x $SEP $GITP origin br"            'commit then push'
check yes "$ERE_PUSH"   "$GHPR edit 9 --body-file b.md $SEP $GITP"        'pr edit then push'
check yes "$ERE_PUSH"   "cd /tmp $SEP $GITP origin br"                    'cd then push'
check yes "$ERE_PUSH"   "true; $GITP origin br"                           'push after a semicolon'
check yes "$ERE_PUSH"   "GIT_TRACE=1 $GITP origin br"                     'push behind an env prefix'
check yes "$ERE_CREATE" "echo hi $SEP $GHPR create --base main"           'create after a chain'
check yes "$ERE_MERGE"  "git checkout main $SEP $GHPR merge 220 --squash" 'checkout then merge'
check yes "$ERE_MERGE"  "$GHPR update-branch 220 $SEP $GHPR merge 220"    'update-branch then merge'

# ── a newline-separated action already worked; keep it working ──
check yes "$ERE_PUSH"   "git commit -m x
$GITP origin br"                                                          'push on its own line'

# ── and must NOT over-match: these are not the guarded action ──
check no  "$ERE_PUSH"   'git pushd origin br'          'pushd is not push'
check no  "$ERE_PUSH"   "echo \"$GITP origin br\""     'push only as a quoted argument'
check no  "$ERE_PUSH"   'git commit -m x'              'commit alone'
check no  "$ERE_MERGE"  "$GHPR view 220 --json body"   'pr view is not merge'
check no  "$ERE_MERGE"  "$GHPR merge-queue status"     'merge-queue is not merge'
check no  "$ERE_CREATE" "$GHPR edit 220 --body-file b" 'pr edit is not create'

# ── END-TO-END: the real hook's verdict, not just the matcher ──
# CREATE path only: it inspects the submitted body directly, so no network and no repo state.
hook_verdict() {
  printf '%s' "$1" | jq -Rs '{tool_name:"Bash", tool_input:{command:.}}' \
    | CLAUDE_PROJECT_DIR="$PWD" bash .claude/hooks/require-skill-audit.sh >/dev/null 2>&1
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
  e2e 0 'git status --short' 'an unrelated command is not gated'
fi

if [ "$fails" -eq 0 ]; then
  printf 'OK  pr-gate matcher: %d cases\n' "$n"
  exit 0
fi
printf 'FAILED  %d of %d pr-gate matcher cases\n' "$fails" "$n"
exit 1
