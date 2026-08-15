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

push_anchor_check() { # push_anchor_check <yes|no> <args-after-push> <label>
  local want="$1" args="$2" label="$3" got=no resolved
  n=$((n + 1))
  if resolved="$(gate_push_head_sha "$args" 2>/dev/null)" \
      && [ "$resolved" = "$(git rev-parse HEAD 2>/dev/null)" ]; then
    got=yes
  fi
  if [ "$got" != "$want" ]; then
    fails=$((fails + 1))
    printf 'FAIL  %s\n      want=%s got=%s args=%s\n' "$label" "$want" "$got" "$args"
  fi
}

action_payload_check() { # action_payload_check <kind> <expected-payload> <cmd> <label>
  local kind="$1" want="$2" cmd="$3" label="$4" got
  n=$((n + 1))
  got="$(gate_bash_actions "$cmd" | awk -F '\t' -v kind="$kind" '$1 == kind {
    print substr($0, index($0, "\t") + 1); exit
  }')"
  if [ "$got" != "$want" ]; then
    fails=$((fails + 1))
    printf 'FAIL  %s\n      want payload=%s got payload=%s\n      cmd=%s\n' \
      "$label" "$want" "$got" "$cmd"
  fi
}

push_payload_check() { # push_payload_check <expected-payload> <cmd> <label>
  action_payload_check push "$1" "$2" "$3"
}

feature_relevance() { # feature_relevance <newline-separated paths> -> yes | no
  if declare -F gate_feature_docs_relevant >/dev/null 2>&1 \
      && printf '%s\n' "$1" | gate_feature_docs_relevant; then
    printf 'yes'
  else
    printf 'no'
  fi
}

merge_selector_check() { # merge_selector_check <yes|no> <expected> <args> <label>
  local want="$1" expected="$2" args="$3" label="$4" got="" ok=no
  n=$((n + 1))
  if got="$(gate_pr_merge_selector "$args" 2>/dev/null)"; then ok=yes; fi
  if [ "$ok" != "$want" ] || { [ "$want" = yes ] && [ "$got" != "$expected" ]; }; then
    fails=$((fails + 1))
    printf 'FAIL  %s\n      want=%s selector=%s got=%s selector=%s\n' \
      "$label" "$want" "$expected" "$ok" "$got"
  fi
}

merge_match_check() { # merge_match_check <yes|no> <expected-sha> <args> <label>
  local want="$1" expected="$2" args="$3" label="$4" got="" ok=no
  n=$((n + 1))
  if got="$(gate_pr_merge_match_sha "$args" 2>/dev/null)"; then ok=yes; fi
  if [ "$ok" != "$want" ] || { [ "$want" = yes ] && [ "$got" != "$expected" ]; }; then
    fails=$((fails + 1))
    printf 'FAIL  %s\n      want=%s match-sha=%s got=%s match-sha=%s\n' \
      "$label" "$want" "$expected" "$ok" "$got"
  fi
}

checkbox_status_check() { # checkbox_status_check <expected> <body> <key> <label>
  local expected="$1" body="$2" key="$3" label="$4" got
  n=$((n + 1))
  got="$(gate_checkbox_status "$body" "$key")"
  if [ "$got" != "$expected" ]; then
    fails=$((fails + 1))
    printf 'FAIL  %s\n      want=%s got=%s\n' "$label" "$expected" "$got"
  fi
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
check yes push 'git -C /tmp push origin br'         'unsafe git -C push is still classified'
check yes push 'git -c remote.origin.push=OTHER:refs/heads/br push origin' 'unsafe git -c push is classified'
check yes push 'git --git-dir=/tmp/repo.git push origin br' 'unsafe --git-dir push is classified'
check yes push 'git --work-tree /tmp/tree push origin br' 'unsafe --work-tree push is classified'
check yes push '(git push origin br)'               'push in a subshell'
check yes push 'if true; then git push origin br; fi' 'push in an if body'

# Prefixes that can change which repository/config/ref `git push` publishes must retain an unsafe
# marker; stripping them and validating GATE_PROJ would audit one HEAD while another is pushed.
push_payload_check '__GATE_UNSAFE_GIT_GLOBAL_CONTEXT__' \
  'GIT_DIR=/tmp/other.git GIT_WORK_TREE=/tmp/other git push origin br' \
  'direct Git repository environment is unsafe'
push_payload_check '__GATE_UNSAFE_GIT_GLOBAL_CONTEXT__' \
  'env GIT_DIR=/tmp/other.git GIT_WORK_TREE=/tmp/other git push origin br' \
  'env-wrapped Git repository environment is unsafe'
push_payload_check '__GATE_UNSAFE_GIT_GLOBAL_CONTEXT__' \
  'GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=remote.origin.push GIT_CONFIG_VALUE_0=OTHER:refs/heads/br git push origin' \
  'per-command Git config environment is unsafe'
push_payload_check '__GATE_UNSAFE_GIT_GLOBAL_CONTEXT__' \
  'env -u GIT_DIR git push origin br' \
  'unsetting Git repository environment is unsafe'
push_payload_check '__GATE_UNSAFE_GIT_GLOBAL_CONTEXT__' \
  'env --unset=GIT_WORK_TREE git push origin br' \
  'long-form Git environment unset is unsafe'
push_payload_check '__GATE_UNSAFE_GIT_GLOBAL_CONTEXT__' \
  'env --chdir=/tmp/other git push origin br' \
  'env chdir is unsafe'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'cd /tmp/other && git push origin br' \
  'persistent shell cwd change is unsafe'
push_payload_check 'origin br' 'FOO=bar git push origin br' \
  'unrelated direct environment assignment remains safe'
push_payload_check 'origin br' 'env -u UNUSED FOO=bar git push origin br' \
  'unrelated env wrapper remains safe'
push_payload_check 'origin br' 'GIT_TRACE=1 git push origin br' \
  'diagnostic Git tracing remains safe'
push_payload_check '__GATE_UNSAFE_GIT_GLOBAL_CONTEXT__' \
  'HOME=/tmp/other-home git push origin br' \
  'Git global-config home override is unsafe'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'FOO=bar; git push origin br' \
  'separate assignment command still violates standalone-only'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'git commit --allow-empty -m x && git push origin br' \
  'commit then push is a PreToolUse TOCTOU'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'git config remote.origin.push OTHER:refs/heads/br && git push origin' \
  'config mutation then push is a PreToolUse TOCTOU'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'gh pr edit 7 --body unchecked && gh pr merge 7' \
  'PR mutation then merge is a PreToolUse TOCTOU'
action_payload_check create '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'echo prepare && gh pr create --body ok' \
  'PR create must be a standalone action'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'GH_REPO=other/repo gh pr merge 7' \
  'GitHub repository environment cannot retarget merge'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'env --chdir=/tmp/other gh pr merge 7' \
  'env cwd cannot retarget merge'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'gh pr merge 7 --repo other/repo' \
  'per-command GitHub repo cannot retarget merge'
action_payload_check create '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'gh pr create --repo other/repo --body ok' \
  'per-command GitHub repo cannot retarget create'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'gh -R other/repo pr merge 7' \
  'GitHub global repo option is classified fail-closed'
action_payload_check create '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'gh --repo=other/repo pr create --body ok' \
  'GitHub global long repo option is classified fail-closed'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "sh -c 'git push origin br'" \
  'opaque shell wrapper is classified fail-closed'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  '/usr/bin/sudo git push origin br' \
  'path-qualified unknown wrapper is classified fail-closed'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  '/usr/bin/git push origin br' \
  'absolute Git executable is classified fail-closed'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  './tools/git push origin br' \
  'relative path-qualified Git executable is classified fail-closed'
action_payload_check create '__GATE_UNSAFE_SHELL_CONTEXT__' \
  '/usr/bin/gh pr create --body ok' \
  'absolute gh create executable is classified fail-closed'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  '/opt/homebrew/bin/gh pr merge 7' \
  'path-qualified gh merge executable is classified fail-closed'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "'git' push origin br" \
  'single-quoted Git executable is classified fail-closed'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  '"/usr/bin/git" push origin br' \
  'double-quoted absolute Git executable is classified fail-closed'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'g\it push origin br' \
  'escaped Git executable is classified fail-closed'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "g'i't push origin br" \
  'shell-concatenated Git executable is classified fail-closed'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "command 'git' push origin br" \
  'quoted Git behind command prefix is classified fail-closed'
action_payload_check create '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "'/usr/bin/gh' pr create --body x" \
  'single-quoted gh create executable is classified fail-closed'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  '"gh" pr merge 7' \
  'double-quoted gh merge executable is classified fail-closed'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'gh pr --repo other/repo merge 7' \
  'gh repo flag between pr and merge is classified fail-closed'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'gh pr -R other/repo merge 7' \
  'gh short repo flag between pr and merge is classified fail-closed'
action_payload_check create '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'gh pr --hostname github.com create --body x' \
  'gh hostname flag between pr and create is classified fail-closed'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "'env' GIT_TRACE=1 git push origin br" \
  'quoted env wrapper cannot hide a push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  '"/usr/bin/env" GIT_TRACE=1 git push origin br' \
  'quoted path-qualified env wrapper cannot hide a push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "'command' git push origin br" \
  'quoted command wrapper cannot hide a push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "'env' -S 'git push origin br'" \
  'env split-string payload cannot hide a push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "env --split-string='g\\it push origin br'" \
  'env escaped split-string payload cannot hide a push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "\$'git' push origin br" \
  'Bash ANSI-C quoted Git executable cannot hide a push'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  '$"gh" pr merge 7' \
  'Bash locale-quoted gh executable cannot hide a merge'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'builtin command git push origin br' \
  'builtin/command wrappers cannot hide a push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'exec g\it push origin br' \
  'exec plus escaped executable cannot hide a push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "bash -c 'g\\it push origin br'" \
  'shell -c payload cannot hide a push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  '$(command -v git) push origin br' \
  'command-substituted executable cannot hide a push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'echo "`git push origin br`"' \
  'backtick expansion cannot execute an unclassified push'
action_payload_check create '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'x=`gh pr create --body x`' \
  'backtick expansion cannot execute an unclassified create'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "env -S'git push origin br'" \
  'env attached split-string cannot hide a push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'env -C/tmp git push origin br' \
  'env attached chdir cannot hide a push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'env -P/usr/bin git push origin br' \
  'env attached search path cannot hide a push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'env -uGIT_DIR git push origin br' \
  'env attached Git unset cannot hide a push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "env 'GIT_DIR=/tmp/other.git' git push origin br" \
  'quoted env assignment cannot hide a push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'git -c alias.p=push p origin br' \
  'per-command Git push alias is classified unsafe'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  '$(git --exec-path)/git-push origin br' \
  'direct git-push executable is classified unsafe'
action_payload_check create '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'gh pr new --body x' \
  'official gh pr new alias is classified unsafe'
action_payload_check create '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "gh pr create --body 'x' --head other:branch" \
  'explicit create head cannot retarget audited local HEAD'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "gh pr merge 7 '--repo' other/repo" \
  'quoted repo option cannot retarget a merge'

# A shell variable may choose either the executable or its guarded subcommand only after
# PreToolUse. Recognise the dangerous shape without expanding it; ordinary variables on commands
# unrelated to create/push/merge remain outside the gate.
DYNAMIC_EXE='$G'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "G=git; \"$DYNAMIC_EXE\" push origin br" \
  'dynamic Git executable cannot hide a push'
action_payload_check create '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "G=gh; \"$DYNAMIC_EXE\" pr create --body x" \
  'dynamic gh executable cannot hide create'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "G=g; H=h; \"\$G\$H\" pr merge 7" \
  'concatenated dynamic executable cannot hide merge'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'P=pu; git "${P}sh" origin br' \
  'dynamic Git subcommand cannot hide push'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'X=pr; Y=merge; gh "$X" "$Y" 7' \
  'dynamic gh subcommands cannot hide merge'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'bash -c '\''git "$1" origin br'\'' _ push' \
  'dynamic shell payload cannot hide push'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'bash -c '\''"$1" pr merge 7'\'' _ gh' \
  'dynamic shell executable cannot hide merge'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'eval '\''G=gh; "$G" pr merge 7'\'' ' \
  'eval cannot hide a dynamic merge'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'CMD=(gh pr); "${CMD[@]}" merge 7' \
  'dynamic argv array cannot hide merge'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "\$'\\x67\\x69\\x74' push origin br" \
  'ANSI-C escaped executable cannot hide push'
action_payload_check create '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "\$'\\x67\\x68' pr create --body x" \
  'ANSI-C escaped executable cannot hide create'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "\$'\\x67\\x68' pr merge 7" \
  'ANSI-C escaped executable cannot hide merge'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  '/usr/bin/g?t push origin br' \
  'globbed command-position executable cannot hide push'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  '/opt/homebrew/bin/g[h] pr merge 7' \
  'bracket-globbed command-position executable cannot hide merge'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'G=git; "$G" -C /tmp push origin br' \
  'dynamic Git executable with global options cannot hide push'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'G=gh; "$G" -R other/repo pr merge 7' \
  'dynamic gh executable with global options cannot hide merge'
action_payload_check create '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'G=gh; "$G" --repo=other/repo pr create --body x' \
  'dynamic gh executable with repo option cannot hide create'
push_payload_check '__GATE_UNSAFE_GIT_GLOBAL_CONTEXT__' \
  'X=push git --config-env=alias.publish=X publish --dry-run origin br' \
  'Git config-env alias cannot hide push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=alias.publish GIT_CONFIG_VALUE_0=push git publish origin br' \
  'Git config-count alias cannot hide push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'git -c include.path=/tmp/aliases publish --dry-run origin br' \
  'indirect include.path Git alias cannot hide push'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'git --git-dir=/tmp/other.git publish --dry-run origin br' \
  'alternate git-dir alias source cannot hide push'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'GH_CONFIG_DIR=/tmp/gh gh land 7' \
  'alternate gh alias config cannot hide merge'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'G=git; P=push; "$G" "$P" origin br' \
  'fully dynamic Git executable/subcommand cannot hide push'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'G=gh; P=pr; M=merge; "$G" "$P" "$M" 7' \
  'fully dynamic gh executable/subcommands cannot hide merge'
action_payload_check create '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'G=gh; P=pr; C=create; "$G" "$P" "$C" --body x' \
  'fully dynamic gh executable/subcommands cannot hide create'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' \
  '{git,push} --dry-run origin HEAD:refs/heads/proof' \
  'brace-expanded Git command cannot hide push'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  '{gh,pr,merge} 7' \
  'brace-expanded gh command cannot hide merge'
action_payload_check create '__GATE_UNSAFE_SHELL_CONTEXT__' \
  '{gh,pr,create} --body unchecked' \
  'brace-expanded gh command cannot hide create'

# `gh api` is another official mutation surface. Read-only GETs stay outside the PR gates, while
# explicit write methods, implicit POST fields/input and GraphQL mutations must fail closed.
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'gh api --method PUT repos/example/repo/pulls/7/merge' \
  'gh api pull merge mutation is guarded'
action_payload_check create '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'gh api -X POST repos/example/repo/pulls -f head=feature -f base=main' \
  'gh api pull creation mutation is guarded'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  "gh api graphql -f 'query=mutation { closePullRequest(input: {}) { clientMutationId } }'" \
  'gh api GraphQL mutation is guarded'
action_payload_check create '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'gh api repos/example/repo/pulls -f head=feature' \
  'gh api implicit POST field is guarded'
action_payload_check merge '__GATE_UNSAFE_SHELL_CONTEXT__' \
  'gh api repos/example/repo/pulls --input payload.json' \
  'gh api input body is guarded'
check no merge 'gh api --method GET repos/example/repo/pulls/7' \
  'explicit read-only gh api GET remains outside the merge gate'

line_continued_push=$'git pu\\\nsh origin br'
push_payload_check '__GATE_UNSAFE_SHELL_CONTEXT__' "$line_continued_push" \
  'line continuation cannot split the push subcommand'

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

# ── merge selectors are exact target bindings, never flag operands/current-branch guesses ──
merge_selector_check yes 123 '123 --squash --subject 456' \
  'numeric selector is the literal first merge argument'
merge_selector_check yes 123 "'123' --body 456" \
  'single-quoted numeric selector is dequoted non-executingly'
local_repo_slug="$(gate_repo_slug 2>/dev/null || true)"
if [ -n "$local_repo_slug" ]; then
  merge_selector_check yes 123 \
    "'https://github.com/$local_repo_slug/pull/123' --body-file 456" \
    'quoted local pull URL is normalized to its numeric selector'
  merge_selector_check no '' \
    "'https://github.com/foreign/repository/pull/123' --squash" \
    'foreign pull URL cannot retarget a locally audited merge'
fi
merge_selector_check no '' '--subject 123' \
  'numeric subject operand is never reinterpreted as selector'
merge_selector_check no '' '--body 456' \
  'numeric body operand is never reinterpreted as selector'
merge_selector_check no '' '--body-file 789' \
  'numeric body-file operand is never reinterpreted as selector'
merge_selector_check no '' 'other-branch --squash' \
  'branch selector is unsupported rather than treated as current branch'
merge_selector_check no '' '123 --auto' \
  'persistent auto-merge enablement is outside the one-shot merge gate'
merge_selector_check no '' '123 --disable-auto' \
  'persistent auto-merge mutation is outside the one-shot merge gate'
merge_selector_check no '' '123 --admin' \
  'administrative merge bypass is outside the reviewed merge gate'
merge_selector_check no '' '' \
  'implicit current-branch merge is unsupported'

merge_contract_sha="$(git rev-parse HEAD 2>/dev/null)"
if [ -n "$merge_contract_sha" ]; then
  merge_match_check yes "$merge_contract_sha" \
    "123 --match-head-commit $merge_contract_sha --squash" \
    'one literal full-SHA match-head precondition is accepted'
  merge_match_check no '' '123 --squash' \
    'missing match-head precondition is blocked'
  merge_match_check no '' "123 --match-head-commit ${merge_contract_sha:0:12}" \
    'short match-head precondition is blocked'
  merge_match_check no '' \
    "123 --match-head-commit $merge_contract_sha --match-head-commit $merge_contract_sha" \
    'duplicate match-head preconditions are blocked'
  merge_match_check no '' '123 --match-head-commit "$HEAD"' \
    'dynamic match-head precondition is blocked'
fi

# ── only the leading canonical task marker determines checked state ──
checkbox_sha="$(git rev-parse --short=12 HEAD 2>/dev/null)"
if [ -n "$checkbox_sha" ]; then
  checkbox_status_check "checked $checkbox_sha" \
    "- [x] \`/project-review\` clean — merge gate @ $checkbox_sha" project-review \
    'leading checked task marker is accepted'
  checkbox_status_check unchecked \
    "- [ ] \`/project-review\` clean — merge gate @ <sha>" project-review \
    'canonical unticked template marker remains unchecked'
  checkbox_status_check absent \
    "- [ ] \`/project-review\` clean — merge gate @ $checkbox_sha (example [x])" project-review \
    'trailing text cannot turn a non-canonical unticked line into a gate'
  checkbox_status_check ambiguous \
    "- [x] \`/project-review\` clean — merge gate @ $checkbox_sha - [ ] /project-review merge gate @ $checkbox_sha" \
    project-review 'multiple gate tasks on one physical line are ambiguous'
  checkbox_status_check absent \
    "- [x] /not-project-review clean - merge gate @ $checkbox_sha" project-review \
    'project-review key must be an exact slash token'
  checkbox_status_check absent \
    "- [x] /skip-skill-audit clean - create/push gate @ $checkbox_sha" skill-audit \
    'skill-audit key must not match a longer command token'
  checkbox_status_check absent \
    "- [x] /not-feature-docs synced - merge gate @ $checkbox_sha" feature-docs \
    'feature-docs key must be an exact slash token'
  checkbox_status_check absent \
    "- [x] /project-review.old clean - merge gate @ $checkbox_sha" project-review \
    'project-review dotted suffix is not the canonical command token'
  checkbox_status_check absent \
    "- [x] /project-review/skip clean - merge gate @ $checkbox_sha" project-review \
    'project-review path suffix is not the canonical command token'
  checkbox_status_check absent \
    "- [x] /feature-docs/disabled synced - merge gate @ $checkbox_sha" feature-docs \
    'feature-docs path suffix is not the canonical command token'
  checkbox_status_check absent \
    "- [x] /project-review negated @ $checkbox_sha" project-review \
    'project-review requires the exact merge-gate phrase'
  checkbox_status_check absent \
    "- [x] /feature-docs delegated @ $checkbox_sha" feature-docs \
    'feature-docs requires the exact merge-gate phrase'
  checkbox_status_check absent \
    "- [x] \`/project-review\` FAILED — merge gate @ $checkbox_sha" project-review \
    'FAILED project-review status cannot satisfy the canonical gate'
  checkbox_status_check absent \
    "- [x] \`/feature-docs\` NOT SYNCED — merge gate @ $checkbox_sha" feature-docs \
    'NOT SYNCED feature-docs status cannot satisfy the canonical gate'
  checkbox_status_check absent \
    "- [x] \`/skill-audit\` STALE — PR create/push gate @ $checkbox_sha" skill-audit \
    'STALE skill-audit status cannot satisfy the canonical gate'
  checkbox_status_check absent \
    "- [x] \`/project-review\` clean — merge gate @ ${checkbox_sha}junk" project-review \
    'SHA followed by letters is not a canonical gate stamp'
  checkbox_status_check absent \
    "- [x] \`/skill-audit\` clean — PR create/push gate @ ${checkbox_sha}_stale" skill-audit \
    'SHA followed by an underscore is not a canonical gate stamp'
  checkbox_status_check absent \
    "- [x] \`/feature-docs\` synced — merge gate @ 0123456789012345678901234567890123456789a" \
    feature-docs '41-hex token is not a canonical gate stamp'
fi

# ── every guarded action is reported; callers reject more than one instead of checking first ──
n=$((n + 1))
if [ "$(action_count "$GHPR merge 1 || $GHPR merge 2" merge)" != "2" ]; then
  fails=$((fails + 1)); printf 'FAIL  two merge actions must both be reported\n'
fi
n=$((n + 1))
if [ "$(gate_bash_actions "$GHPR create --title x $SEP $GITP" | wc -l | tr -d ' ')" != "2" ]; then
  fails=$((fails + 1)); printf 'FAIL  create + push must both be reported\n'
fi

# ── a push audit stamp is valid only for the commit/ref actually being published ──
current_branch="$(git rev-parse --abbrev-ref HEAD 2>/dev/null)"
push_anchor_check yes "origin $current_branch" 'current branch push resolves to HEAD'
push_anchor_check yes "--set-upstream origin $current_branch" 'upstream current branch resolves to HEAD'
push_anchor_check yes "origin HEAD:$current_branch" 'explicit HEAD to current branch resolves to HEAD'
push_anchor_check no "origin OTHER_REF:$current_branch" 'non-current source ref is blocked'
push_anchor_check no "origin HEAD:another-branch" 'different destination branch is blocked'
push_anchor_check no "origin $current_branch another-ref" 'multiple refspecs are blocked'
push_anchor_check no "--all origin" 'all-branches push is blocked'
push_anchor_check no "--tags origin" 'tag push is blocked'
push_anchor_check no '__GATE_UNSAFE_GIT_GLOBAL_CONTEXT__' 'Git-global repo/config context is blocked'
push_anchor_check no "/tmp/foreign.git $current_branch" 'foreign repository path is blocked'
push_anchor_check no "other $current_branch" 'non-origin remote is blocked'
n=$((n + 1))
push_record="$(gate_bash_actions "$GITP origin OTHER_REF:$current_branch")"
if [ "${push_record#*$'\t'}" != "origin OTHER_REF:$current_branch" ]; then
  fails=$((fails + 1)); printf 'FAIL  push matcher must retain the exact refspec payload\n'
fi

# ── feature-docs relevance is a release contract, not just build.yml ──
for workflow in build.yml signed-pr-preview.yml pr-preview-cleanup.yml; do
  n=$((n + 1))
  if [ "$(feature_relevance ".github/workflows/$workflow")" != yes ]; then
    fails=$((fails + 1))
    printf 'FAIL  feature-docs must gate workflow: %s\n' "$workflow"
  fi
done
for runtime in \
  docs/index.html \
  docs/installer-bootstrap.mjs \
  docs/serial-port-release.mjs \
  docs/web-installer.mjs \
  docs/vendor/esptool-js-0.6.1.bundle.js; do
  n=$((n + 1))
  if [ "$(feature_relevance "$runtime")" != yes ]; then
    fails=$((fails + 1))
    printf 'FAIL  feature-docs must gate shipped Pages runtime: %s\n' "$runtime"
  fi
done
n=$((n + 1))
if [ "$(feature_relevance 'scripts/release-relevance.sh')" != yes ]; then
  fails=$((fails + 1))
  printf 'FAIL  feature-docs must gate the cumulative Release/Pages classifier\n'
fi
n=$((n + 1))
if [ "$(feature_relevance '.github/workflows/renovate.yaml')" != no ]; then
  fails=$((fails + 1)); printf 'FAIL  renovate workflow must not arm feature-docs gate\n'
fi
n=$((n + 1))
if [ "$(feature_relevance 'docs/README.md')" != no ]; then
  fails=$((fails + 1)); printf 'FAIL  docs-only change must not arm feature-docs gate\n'
fi

# ── the public OTA/Pages channel is Release-bound, never a generic main snapshot ──
# A manually dispatched or docs-only build may compile/test, but must not sign or replace the
# public manifest with a sourceSha that has no matching Release tag. Keep these checks beside the
# release-workflow relevance cases so workflow edits cannot silently reopen that drift.
workflow_job() { # workflow_job <job-name>
  awk -v job="$1" '
    $0 == "  " job ":" { in_job=1 }
    in_job && $0 ~ /^  [A-Za-z0-9_-]+:$/ && $0 != "  " job ":" { exit }
    in_job { print }
  ' .github/workflows/build.yml
}

build_job="$(workflow_job build)"
publish_job="$(workflow_job publish)"
deploy_job="$(workflow_job deploy)"
for spec in \
  'build|pages: read' \
  'publish|github.event_name == '\''push'\''' \
  'publish|needs.build.outputs.firmware == '\''yes'\''' \
  'deploy|github.event_name == '\''push'\''' \
  'deploy|needs.build.outputs.firmware == '\''yes'\''' \
  'deploy|needs.publish.result == '\''success'\''' \
  'version|./scripts/select-release-version.sh --select-main "$SOURCE_SHA"' \
  'version|./scripts/select-release-version.sh --latest-published-stable' \
  'changes|./scripts/release-relevance.sh --changed "$SOURCE_SHA"' \
  'publish|./scripts/select-release-version.sh --require-current-main "$SOURCE_SHA"' \
  'publish|./scripts/select-release-version.sh --require-release-candidate "$SOURCE_SHA" "$DISPLAY_VERSION"' \
  'publish|./scripts/select-release-version.sh --require-published-release "$SOURCE_SHA" "$DISPLAY_VERSION"' \
  'deploy|./scripts/select-release-version.sh --require-published-release "$SOURCE_SHA" "$DISPLAY_VERSION"' \
  'release|target_commitish: ${{ github.sha }}'; do
  n=$((n + 1))
  kind="${spec%%|*}"
  needle="${spec#*|}"
  case "$kind" in
    build) haystack="$build_job" ;;
    publish) haystack="$publish_job" ;;
    deploy) haystack="$deploy_job" ;;
    version) haystack="$(sed -n '/- name: Compute release version/,/- name: Detect firmware-relevant changes/p' .github/workflows/build.yml)" ;;
    changes) haystack="$(sed -n '/- name: Detect firmware-relevant changes/,/- name: Stamp firmware version/p' .github/workflows/build.yml)" ;;
    release) haystack="$(printf '%s\n' "$publish_job" | sed -n '/- name: Create release/,/- name: Configure Pages/p')" ;;
  esac
  if ! printf '%s\n' "$haystack" | grep -Fq -- "$needle"; then
    fails=$((fails + 1))
    printf 'FAIL  Release-bound workflow contract missing: %s\n' "$needle"
  fi
done

for count_spec in \
  'publish|--require-current-main "$SOURCE_SHA"|1' \
  'publish|--require-release-candidate "$SOURCE_SHA" "$DISPLAY_VERSION"|2' \
  'publish|--require-published-release "$SOURCE_SHA" "$DISPLAY_VERSION"|2' \
  'deploy|--require-published-release "$SOURCE_SHA" "$DISPLAY_VERSION"|2'; do
  n=$((n + 1))
  job="${count_spec%%|*}"
  rest="${count_spec#*|}"
  needle="${rest%%|*}"
  want="${rest##*|}"
  case "$job" in publish) haystack="$publish_job" ;; deploy) haystack="$deploy_job" ;; esac
  got="$(printf '%s\n' "$haystack" | grep -Fc -- "$needle")"
  if [ "$got" != "$want" ]; then
    fails=$((fails + 1))
    printf 'FAIL  %s stale-run checks: want=%s got=%s (%s)\n' "$job" "$want" "$got" "$needle"
  fi
done

for job in publish deploy; do
  n=$((n + 1))
  case "$job" in publish) haystack="$publish_job" ;; deploy) haystack="$deploy_job" ;; esac
  if printf '%s\n' "$haystack" | grep -Fq workflow_dispatch; then
    fails=$((fails + 1))
    printf 'FAIL  workflow_dispatch must not reach %s job\n' "$job"
  fi
done

n=$((n + 1))
if ! grep -Fq 'docs/(index\.html|installer-bootstrap\.mjs|serial-port-release\.mjs|web-installer\.mjs|vendor/)' \
    scripts/release-relevance.sh; then
  fails=$((fails + 1))
  printf 'FAIL  shipped Pages runtime changes must cut a Release\n'
fi
n=$((n + 1))
if ! grep -Fq 'ci-sign-artifacts|next-version|select-release-version' scripts/release-relevance.sh; then
  fails=$((fails + 1))
  printf 'FAIL  release version/signing scripts must trigger the Release pipeline\n'
fi
n=$((n + 1))
if grep -Fq 'github.event.before' .github/workflows/build.yml; then
  fails=$((fails + 1))
  printf 'FAIL  Release relevance must not use only the immediately preceding push\n'
fi
n=$((n + 1))
if grep -Fq "git tag -l 'v*' --sort=-v:refname" .github/workflows/build.yml; then
  fails=$((fails + 1))
  printf 'FAIL  PR preview base must not use the raw newest prerelease-capable v* tag\n'
fi

# ── ship OTA success is bound to observation time, never absolute device uptime ──
# The device may spend substantial time reaching WiFi/services before its health gate begins.
# Pin both elapsed clocks to the first exact post-OTA observation so an already-high uptime cannot
# make the delivery runbook declare a still-PENDING_VERIFY image durable.
for needle in \
  'PROBATION_BASELINE_UPTIME=' \
  'LIVE_STATUS_WALL=$SECONDS' \
  'PROBATION_BASELINE_WALL=$LIVE_STATUS_WALL' \
  'PROBATION_DEADLINE=$((PROBATION_BASELINE_WALL + 180))' \
  'LIVE_UPTIME < LAST_UPTIME' \
  'OBSERVED_WALL=$((LIVE_STATUS_WALL - PROBATION_BASELINE_WALL))' \
  'OBSERVED_UPTIME=$((LIVE_UPTIME - PROBATION_BASELINE_UPTIME))' \
  'CLOCK_SKEW < -5 || CLOCK_SKEW > 5' \
  'OBSERVED_WALL >= 100 && OBSERVED_UPTIME >= 100' \
  'http://$DEVICE_IP/diag?redact=1' \
  'marked valid (rollback cancelled'; do
  n=$((n + 1))
  if ! grep -Fq -- "$needle" .claude/skills/ship/SKILL.md; then
    fails=$((fails + 1))
    printf 'FAIL  ship OTA probation contract missing: %s\n' "$needle"
  fi
done
n=$((n + 1))
if grep -Fq 'LIVE_UPTIME >= 100' .claude/skills/ship/SKILL.md; then
  fails=$((fails + 1))
  printf 'FAIL  ship OTA probation must not trust absolute device uptime\n'
fi

# ── published OTA verification must bind Pages bytes to one stable Release asset snapshot ──
for needle in \
  'scripts/check-release-pages-bytes.py "$SITE" "$RELEASE" --version "$REL"' \
  'START_ASSET_SET=' \
  '{id, name, size, digest}' \
  'END_ASSET_SET=' \
  '"$END_ASSET_SET" == "$START_ASSET_SET"'; do
  n=$((n + 1))
  if ! grep -Fq -- "$needle" .claude/skills/ota-release-verify/SKILL.md; then
    fails=$((fails + 1))
    printf 'FAIL  OTA Release byte/TOCTOU contract missing: %s\n' "$needle"
  fi
done

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

mcp_create_verdict() { # mcp_create_verdict <title> <body> <owner> <repo> <head>
  jq -n --arg title "$1" --arg body "$2" --arg owner "${3:-}" --arg repo "${4:-}" \
      --arg head "${5:-}" \
    '{tool_name:"mcp__github__create_pull_request",
      tool_input:{title:$title, body:$body, owner:$owner, repo:$repo, head:$head}}' \
    | CLAUDE_PROJECT_DIR="$PWD" bash .claude/hooks/require-skill-audit.sh >/dev/null 2>&1
  printf '%s' "$?"
}

mcp_hook_verdict_at() { # mcp_hook_verdict_at <project> <hook> <tool> <owner> <repo> <number> <head> <body>
  local project="$1" hook="$2" tool="$3" owner="$4" repo="$5" number="$6" head="$7" body="$8"
  jq -n --arg tool "$tool" --arg owner "$owner" --arg repo "$repo" --arg number "$number" \
      --arg head "$head" --arg body "$body" \
    '{tool_name:$tool, tool_input:{owner:$owner, repo:$repo, pullNumber:$number,
      head:$head, expected_head_sha:$head, body:$body}}' \
    | CLAUDE_PROJECT_DIR="$project" bash "$project/.claude/hooks/$hook" >/dev/null 2>&1
  printf '%s' "$?"
}

hook_verdict_at() { # hook_verdict_at <project-dir> <hook-name> <cmd>
  local project_dir="$1" hook="$2" cmd="$3"
  printf '%s' "$cmd" | jq -Rs '{tool_name:"Bash", tool_input:{command:.}}' \
    | CLAUDE_PROJECT_DIR="$project_dir" bash "$project_dir/.claude/hooks/$hook" \
        >/dev/null 2>&1
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
  gate_tmp="$(mktemp -d "${TMPDIR:-/tmp}/tesla-pr-gates.XXXXXX")"
  trap 'rm -rf -- "$gate_tmp"' EXIT
  other_repo="$gate_tmp/other-repo"
  mkdir -p "$other_repo"
  git -C "$other_repo" init -q
  git -C "$other_repo" config user.name test
  git -C "$other_repo" config user.email test@example.invalid
  git -C "$other_repo" config commit.gpgsign false
  git -C "$other_repo" commit --allow-empty -qm other-head
  git -C "$other_repo" branch -M "$current_branch"
  other_head="$(git -C "$other_repo" rev-parse HEAD)"
  [ "$other_head" != "$(git rev-parse HEAD)" ] || {
    printf 'FAIL  alternate-repository fixture unexpectedly shares current HEAD\n'
    exit 1
  }

  box="- [x] \`/skill-audit\` clean — PR create/push gate @ ${head_sha}"
  body_file="$gate_tmp/pr-body.md"
  printf '%s\n' "$box" > "$body_file"
  body_link="$gate_tmp/pr-body-link.md"
  ln -s "$body_file" "$body_link"
  duplicate_body_file="$gate_tmp/pr-body-duplicate.md"
  printf '%s\n- [ ] /skill-audit create/push gate @ %s\n' "$box" "$head_sha" \
    > "$duplicate_body_file"
  e2e 2 "echo hi $SEP $GHPR create --base main --title x --body 'nothing here'" \
      'chained create, no checkbox -> BLOCKED'
  e2e 2 "echo hi $SEP $GHPR create --base main --title x --body '- [ ] /skill-audit gate'" \
      'chained create, unticked box -> BLOCKED'
  e2e 2 "echo hi $SEP $GHPR create --base main --title x --body '${box}'" \
      'chained create, even ticked+stamped -> BLOCKED as non-standalone'
  e2e 0 "$GHPR create --body '${box}' --base main --title x" \
      'standalone create reads a first long-form inline body'
  e2e 0 "$GHPR create -b '${box}' --base main --title x" \
      'standalone create reads the exact short-form inline body before later options'
  e2e 0 "$GHPR create --body='${box}' --base main --title x" \
      'standalone create reads the equals-form inline body'
  e2e 2 "$GHPR create --body \"${box}\" --base main --title x" \
      'double-quoted inline body is expansion-capable and refused'
  e2e 0 "$GHPR create --body-file '$body_file' --base main --title x" \
      'standalone create reads one exact regular body file'
  e2e 0 "$GHPR create -F '$body_file' --base main --title x" \
      'standalone create reads the short-form body file'
  e2e 0 "$GHPR create --body-file='$body_file' --base main --title x" \
      'standalone create reads the equals-form body file'
  e2e 2 "$GHPR create --base main --body '${box}' --title x" \
      'body option after another create option is refused'
  e2e 2 "$GHPR create --title --body='${box}'" \
      'body-looking title operand cannot satisfy a missing PR body'
  e2e 2 "$GHPR create --base main --title '${box}' --body 'unchecked body'" \
      'checked title cannot satisfy an unchecked body'
  e2e 2 "$GHPR create --base main --title '${box}'" \
      'checked title cannot replace a missing body'
  e2e 2 "$GHPR create --base main --body '${box}' --body 'unchecked body'" \
      'multiple inline body sources are ambiguous and blocked'
  e2e 2 "$GHPR create --body '${box}' -bunchecked" \
      'attached short duplicate body source is ambiguous and blocked'
  e2e 2 "$GHPR create --base main --body '${box}' --body-file '$body_file'" \
      'mixed inline/file body sources are ambiguous and blocked'
  e2e 2 "$GHPR create --base main --body-file '$body_link'" \
      'symlink body file is refused'
  e2e 2 "$GHPR create --base main --body-file '$duplicate_body_file'" \
      'duplicate gate checkbox lines are ambiguous and refused'
  e2e 2 "$GHPR create --body-file '$body_file' > '$body_file'" \
      'body-file redirection TOCTOU is refused before shell truncation'
  e2e 2 "$GHPR create --body '${box}' --head other:branch" \
      'explicit foreign create head is refused'
  e2e 2 "$GHPR create --base main -- --body '${box}'" \
      'body-looking positional data after option terminator is refused'
  n=$((n + 1))
  if [ "$(mcp_create_verdict "$box" 'unchecked body' foreign owner "$current_branch")" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  MCP create title must never satisfy the body gate\n'
  fi
  n=$((n + 1))
  if [ "$(mcp_create_verdict x "$box" foreign owner "$current_branch")" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  MCP create foreign repository must fail closed\n'
  fi
  n=$((n + 1))
  if [ "$(mcp_create_verdict x "$box" '' '' '')" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  MCP create missing repository/head must fail closed\n'
  fi
  e2e 2 "$GHPR create --base main --body '${box}' $SEP $GITP" \
      'multiple publish actions -> BLOCKED'
  e2e 2 "git commit --allow-empty -m x $SEP $GITP origin $current_branch" \
      'commit then push -> BLOCKED before either segment executes'
  e2e 2 "git config remote.origin.push OTHER:refs/heads/$current_branch $SEP $GITP origin" \
      'config mutation then push -> BLOCKED before either segment executes'
  e2e 2 "$GITP origin HEAD~1:$current_branch" \
      'non-current refspec push -> BLOCKED before GitHub lookup'
  e2e 2 "git -C $other_repo push origin $current_branch" \
      'same branch in another repository -> BLOCKED before GitHub lookup'
  e2e 2 "git -c remote.origin.push=OTHER_REF:refs/heads/$current_branch push origin" \
      'per-command remote push override -> BLOCKED before GitHub lookup'
  e2e 2 "git --git-dir=$other_repo/.git --work-tree=$other_repo push origin $current_branch" \
      'explicit git-dir/work-tree context -> BLOCKED before GitHub lookup'
  e2e 2 "GIT_DIR=$other_repo/.git GIT_WORK_TREE=$other_repo $GITP origin $current_branch" \
      'direct Git environment context -> BLOCKED before GitHub lookup'
  e2e 2 "env GIT_DIR=$other_repo/.git GIT_WORK_TREE=$other_repo $GITP origin $current_branch" \
      'env-wrapped Git environment context -> BLOCKED before GitHub lookup'
  e2e 2 "env -u GIT_DIR $GITP origin $current_branch" \
      'env short unset of Git context -> BLOCKED before GitHub lookup'
  e2e 2 "env --unset=GIT_WORK_TREE $GITP origin $current_branch" \
      'env long unset of Git context -> BLOCKED before GitHub lookup'
  e2e 2 "GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=remote.origin.push GIT_CONFIG_VALUE_0=OTHER:refs/heads/$current_branch $GITP origin" \
      'Git config-count ref override -> BLOCKED before GitHub lookup'
  e2e 2 "env --chdir=$other_repo $GITP origin $current_branch" \
      'env chdir to another repository -> BLOCKED before GitHub lookup'
  e2e 2 "cd $other_repo $SEP $GITP origin $current_branch" \
      'prior cwd change to another repository -> BLOCKED before GitHub lookup'
  e2e 2 "$GITP $other_repo $current_branch" \
      'foreign repository path -> BLOCKED before GitHub lookup'
  e2e 2 "$GITP other $current_branch" \
      'non-origin remote -> BLOCKED before GitHub lookup'
  e2e 2 "/usr/bin/git push origin $current_branch" \
      'absolute Git executable -> BLOCKED before GitHub lookup'
  e2e 2 "'git' push origin $current_branch" \
      'quoted Git executable -> BLOCKED before GitHub lookup'
  e2e 2 "g\\it push origin $current_branch" \
      'escaped Git executable -> BLOCKED before GitHub lookup'
  e2e 2 "G=git; \"$DYNAMIC_EXE\" push origin $current_branch" \
      'dynamic quoted Git executable -> BLOCKED before GitHub lookup'
  e2e 2 "G=git; $DYNAMIC_EXE push origin $current_branch" \
      'dynamic unquoted Git executable -> BLOCKED before GitHub lookup'
  e2e 2 "G=git; \"$DYNAMIC_EXE\" -C $other_repo push origin $current_branch" \
      'dynamic Git executable with global context -> BLOCKED before GitHub lookup'
  e2e 2 'G=git; P=push; "$G" "$P" origin branch' \
      'fully dynamic Git action -> BLOCKED before GitHub lookup'
  e2e 2 '{git,push} --dry-run origin HEAD:refs/heads/proof' \
      'brace-expanded Git action -> BLOCKED before GitHub lookup'
  e2e 2 "\$'\\x67\\x69\\x74' push origin $current_branch" \
      'ANSI-C escaped Git executable -> BLOCKED before GitHub lookup'
  e2e 2 "/usr/bin/g?t push origin $current_branch" \
      'globbed Git executable -> BLOCKED before GitHub lookup'
  e2e 2 "env -S '$GITP origin $current_branch'" \
      'env split-string push -> BLOCKED before GitHub lookup'
  e2e 2 "/usr/bin/gh pr create --body '${box}'" \
      'absolute gh create executable -> BLOCKED before body acceptance'
  e2e 2 "'/usr/bin/gh' pr create --body '${box}'" \
      'quoted absolute gh create -> BLOCKED before body acceptance'
  e2e 2 "$GHPR create --repo other/repo --body '${box}'" \
      'gh create repo override -> BLOCKED before body acceptance'
  e2e 2 "gh pr --repo other/repo create --body '${box}'" \
      'gh create repo override between pr/action -> BLOCKED before body acceptance'
  e2e 2 "G=gh; \"$DYNAMIC_EXE\" pr create --body '${box}'" \
      'dynamic quoted gh create -> BLOCKED before body acceptance'
  e2e 2 "G=gh; $DYNAMIC_EXE pr create --body '${box}'" \
      'dynamic unquoted gh create -> BLOCKED before body acceptance'
  e2e 2 "G=gh; \"$DYNAMIC_EXE\" --repo=other/repo pr create --body '${box}'" \
      'dynamic gh create with repo context -> BLOCKED before body acceptance'
  e2e 2 'G=gh; P=pr; C=create; "$G" "$P" "$C" --body unchecked' \
      'fully dynamic gh create -> BLOCKED before body acceptance'
  e2e 2 '{gh,pr,create} --body unchecked' \
      'brace-expanded gh create -> BLOCKED before body acceptance'
  e2e 2 'gh api -X POST repos/example/repo/pulls -f head=feature' \
      'gh api pull creation -> BLOCKED before mutation'
  e2e 2 "\$'\\x67\\x68' pr create --body '${box}'" \
      'ANSI-C escaped gh create -> BLOCKED before body acceptance'
  n=$((n + 1))
  if [ "$(merge_hook_verdict "$GHPR edit 7 --body unchecked $SEP $GHPR merge 7")" != "2" ]; then
    fails=$((fails + 1)); printf 'FAIL  PR edit then merge must be blocked before network\n'
  fi
  n=$((n + 1))
  if [ "$(merge_hook_verdict "/opt/homebrew/bin/gh pr merge 7")" != "2" ]; then
    fails=$((fails + 1)); printf 'FAIL  path-qualified gh merge must be blocked before network\n'
  fi
  n=$((n + 1))
  if [ "$(merge_hook_verdict "gh -R other/repo pr merge 7")" != "2" ]; then
    fails=$((fails + 1)); printf 'FAIL  gh global repo override merge must be blocked before network\n'
  fi
  n=$((n + 1))
  if [ "$(merge_hook_verdict "\"/opt/homebrew/bin/gh\" pr merge 7")" != "2" ]; then
    fails=$((fails + 1)); printf 'FAIL  quoted path-qualified gh merge must be blocked before network\n'
  fi
  n=$((n + 1))
  if [ "$(merge_hook_verdict "gh pr --hostname github.com merge 7")" != "2" ]; then
    fails=$((fails + 1)); printf 'FAIL  gh hostname override between pr/action must block before network\n'
  fi
  n=$((n + 1))
  if [ "$(merge_hook_verdict 'G=gh; P=pr; M=merge; "$G" "$P" "$M" 7')" != "2" ]; then
    fails=$((fails + 1)); printf 'FAIL  fully dynamic gh merge must block before network\n'
  fi
  n=$((n + 1))
  if [ "$(merge_hook_verdict '{gh,pr,merge} 7')" != "2" ]; then
    fails=$((fails + 1)); printf 'FAIL  brace-expanded gh merge must block before network\n'
  fi
  n=$((n + 1))
  if [ "$(merge_hook_verdict 'gh api --method PUT repos/example/repo/pulls/7/merge')" != "2" ]; then
    fails=$((fails + 1)); printf 'FAIL  gh api merge mutation must block before network\n'
  fi
  n=$((n + 1))
  if [ "$(merge_hook_verdict "gh api graphql -f 'query=mutation { x }'")" != "2" ]; then
    fails=$((fails + 1)); printf 'FAIL  gh api GraphQL mutation must block before network\n'
  fi
  n=$((n + 1))
  if [ "$(merge_hook_verdict "G=gh; \"$DYNAMIC_EXE\" pr merge 7")" != "2" ]; then
    fails=$((fails + 1)); printf 'FAIL  project-review dynamic gh merge must block before network\n'
  fi
  n=$((n + 1))
  if [ "$(hook_verdict_at "$PWD" require-feature-docs.sh \
      "G=gh; \"$DYNAMIC_EXE\" pr merge 7")" != "2" ]; then
    fails=$((fails + 1)); printf 'FAIL  feature-docs dynamic gh merge must block before network\n'
  fi
  n=$((n + 1))
  if [ "$(merge_hook_verdict "/opt/homebrew/bin/g[h] pr merge 7")" != "2" ]; then
    fails=$((fails + 1)); printf 'FAIL  globbed gh merge must block before network\n'
  fi
  n=$((n + 1))
  if [ "$(merge_hook_verdict "$GHPR merge 1 || $GHPR merge 2")" != "2" ]; then
    fails=$((fails + 1)); printf 'FAIL  multiple merge actions must be blocked before network\n'
  fi
  for merge_args in '--subject 123' '--body 456' '--body-file 789' 'other-branch' \
      '7 --auto' '7 --disable-auto' '7 --admin' '7 --squash' \
      '7 --match-head-commit 0000000000000000000000000000000000000000' \
      "7 --match-head-commit $head_sha --match-head-commit $head_sha"; do
    n=$((n + 1))
    if [ "$(merge_hook_verdict "$GHPR merge $merge_args")" != 2 ]; then
      fails=$((fails + 1)); printf 'FAIL  ambiguous merge selector must block: %s\n' "$merge_args"
    fi
    n=$((n + 1))
    if [ "$(hook_verdict_at "$PWD" require-feature-docs.sh "$GHPR merge $merge_args")" != 2 ]; then
      fails=$((fails + 1)); printf 'FAIL  feature-docs ambiguous merge selector must block: %s\n' "$merge_args"
    fi
  done
  e2e 0 'git status --short' 'an unrelated command is not gated'

  # Structured GitHub actions are accepted only for this exact repo/ref and already-pushed HEAD.
  # A local bare remote makes both the mismatch and match deterministic without network access.
  mcp_remote="$gate_tmp/remotes/fixture-owner/fixture-repo.git"
  mcp_project="$gate_tmp/mcp-project"
  mkdir -p "$(dirname "$mcp_remote")" "$mcp_project/.claude"
  git init --bare -q "$mcp_remote"
  git -C "$mcp_project" init -q
  git -C "$mcp_project" config user.name test
  git -C "$mcp_project" config user.email test@example.invalid
  git -C "$mcp_project" config commit.gpgsign false
  git -C "$mcp_project" commit --allow-empty -qm remote-head
  git -C "$mcp_project" branch -M gate-branch
  git -C "$mcp_project" remote add origin "$mcp_remote"
  git -C "$mcp_project" push -q origin gate-branch
  git -C "$mcp_project" commit --allow-empty -qm local-audited-head
  cp -R .claude/hooks "$mcp_project/.claude/hooks"
  mcp_head="$(git -C "$mcp_project" rev-parse --short=12 HEAD)"
  mcp_box="- [x] /skill-audit clean — PR create/push gate @ $mcp_head"
  n=$((n + 1))
  if [ "$(mcp_hook_verdict_at "$mcp_project" require-skill-audit.sh \
      mcp__github__create_pull_request fixture-owner fixture-repo '' gate-branch "$mcp_box")" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  MCP create remote head mismatch must block\n'
  fi
  git -C "$mcp_project" push -q origin gate-branch
  n=$((n + 1))
  if [ "$(mcp_hook_verdict_at "$mcp_project" require-skill-audit.sh \
      mcp__github__create_pull_request fixture-owner fixture-repo '' gate-branch "$mcp_box")" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  MCP create must reject a slug-shaped local origin path\n'
  fi
  n=$((n + 1))
  if [ "$(mcp_hook_verdict_at "$mcp_project" require-skill-audit.sh \
      mcp__github__create_pull_request foreign fixture-repo '' gate-branch "$mcp_box")" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  MCP create foreign owner must block\n'
  fi
  n=$((n + 1))
  if [ "$(mcp_hook_verdict_at "$mcp_project" require-skill-audit.sh \
      mcp__github__create_pull_request fixture-owner fixture-repo '' another-branch "$mcp_box")" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  MCP create different head branch must block\n'
  fi
  n=$((n + 1))
  if [ "$(mcp_hook_verdict_at "$mcp_project" require-skill-audit.sh \
      mcp__github__push_files fixture-owner fixture-repo '' gate-branch "$mcp_box")" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  MCP push_files must always block before server-side commit\n'
  fi

  # User-defined aliases are execution indirection too. Resolve a regular Git alias from the
  # audited repository and a gh alias from an isolated config; neither may bypass classification.
  git -C "$mcp_project" config alias.publish push
  git -C "$mcp_project" publish --dry-run origin gate-branch >/dev/null 2>&1
  n=$((n + 1))
  alias_push="$(GATE_PROJ="$mcp_project" gate_bash_actions 'git publish origin gate-branch' \
    | awk -F '\t' '$1 == "push" { print $2; exit }')"
  if [ "$alias_push" != __GATE_UNSAFE_SHELL_CONTEXT__ ]; then
    fails=$((fails + 1)); printf 'FAIL  persistent Git push alias must classify unsafe\n'
  fi
  n=$((n + 1))
  if [ "$(hook_verdict_at "$mcp_project" require-skill-audit.sh 'git publish origin gate-branch')" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  persistent Git push alias must block in the real hook\n'
  fi
  git -C "$mcp_project" config --unset alias.publish

  alias_include="$gate_tmp/injected-aliases.config"
  printf '%s\n' '[alias]' '  publish = push' > "$alias_include"
  n=$((n + 1))
  if [ "$(hook_verdict_at "$mcp_project" require-skill-audit.sh \
      "git -c include.path=$alias_include publish --dry-run origin gate-branch")" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  include.path Git alias must block in the real hook\n'
  fi
  n=$((n + 1))
  if [ "$(hook_verdict_at "$mcp_project" require-skill-audit.sh \
      "git --git-dir=$mcp_project/.git publish --dry-run origin gate-branch")" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  alternate git-dir alias lookup must block in the real hook\n'
  fi

  git -C "$mcp_project" config push.followTags true
  n=$((n + 1))
  if GATE_PROJ="$mcp_project" gate_push_head_sha 'origin gate-branch' >/dev/null 2>&1; then
    fails=$((fails + 1)); printf 'FAIL  push.followTags must block even with an explicit branch ref\n'
  fi
  n=$((n + 1))
  if [ "$(hook_verdict_at "$mcp_project" require-skill-audit.sh \
      "$GITP origin gate-branch")" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  followTags push must block in the real hook\n'
  fi
  git -C "$mcp_project" config --unset push.followTags

  if command -v gh >/dev/null 2>&1; then
    gh_config="$gate_tmp/gh-config"
    mkdir -p "$gh_config"
    GH_CONFIG_DIR="$gh_config" gh alias set land 'pr merge' >/dev/null
    GH_CONFIG_DIR="$gh_config" gh land --help >/dev/null
    n=$((n + 1))
    if [ "$(GH_CONFIG_DIR="$gh_config" \
        hook_verdict_at "$mcp_project" require-project-review.sh 'gh land 7')" != 2 ]; then
      fails=$((fails + 1)); printf 'FAIL  persistent gh merge alias must block project-review\n'
    fi
    n=$((n + 1))
    if [ "$(GH_CONFIG_DIR="$gh_config" \
        hook_verdict_at "$mcp_project" require-feature-docs.sh 'gh land 7')" != 2 ]; then
      fails=$((fails + 1)); printf 'FAIL  persistent gh merge alias must block feature-docs\n'
    fi
  fi

  # `remote.origin.mirror=true` makes an argument-less push publish every local branch and tag.
  # Prove the fixture really has that breadth, then require the anchor parser and real hook to
  # reject it before any PR/network lookup.
  mirror_remote="$gate_tmp/remotes/mirror-owner/mirror-repo.git"
  mirror_project="$gate_tmp/mirror-project"
  mkdir -p "$(dirname "$mirror_remote")" "$mirror_project/.claude"
  git init --bare -q "$mirror_remote"
  git -C "$mirror_project" init -q
  git -C "$mirror_project" config user.name test
  git -C "$mirror_project" config user.email test@example.invalid
  git -C "$mirror_project" config commit.gpgsign false
  git -C "$mirror_project" commit --allow-empty -qm main
  git -C "$mirror_project" branch -M main
  git -C "$mirror_project" branch other
  git -C "$mirror_project" tag mirror-tag
  git -C "$mirror_project" remote add origin "$mirror_remote"
  git -C "$mirror_project" config remote.origin.mirror true
  cp -R .claude/hooks "$mirror_project/.claude/hooks"
  mirror_dry_run="$(git -C "$mirror_project" push --dry-run origin 2>&1)"
  for ref in 'main -> main' 'other -> other' 'mirror-tag -> mirror-tag'; do
    n=$((n + 1))
    if ! printf '%s\n' "$mirror_dry_run" | grep -Fq -- "$ref"; then
      fails=$((fails + 1)); printf 'FAIL  mirror fixture did not include ref: %s\n' "$ref"
    fi
  done
  n=$((n + 1))
  if GATE_PROJ="$mirror_project" gate_push_head_sha origin >/dev/null 2>&1; then
    fails=$((fails + 1)); printf 'FAIL  remote.origin.mirror must fail the push anchor\n'
  fi
  n=$((n + 1))
  if [ "$(hook_verdict_at "$mirror_project" require-skill-audit.sh "$GITP origin")" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  mirror-configured push must block in the real hook\n'
  fi

  # Stub only the read-only gh queries so MCP merge target/head checks are deterministic.
  fake_bin="$gate_tmp/fake-bin"
  mkdir -p "$fake_bin"
  printf '%s\n' \
    '#!/usr/bin/env bash' \
    'case " $* " in' \
    '  *" --json files "*) printf "main/main.cpp\\n" ;;' \
    '  *" --json number,body,headRefOid "*) printf "%s" "$FAKE_PR_LIST" ;;' \
    '  *" --json body,headRefOid "*) jq -n --arg head "$FAKE_PR_HEAD" --arg body "$FAKE_PR_BODY" '\''{headRefOid:$head,body:$body}'\'' ;;' \
    '  *) exit 1 ;;' \
    'esac' > "$fake_bin/gh"
  chmod +x "$fake_bin/gh"
  mcp_old_head="$(git -C "$mcp_project" rev-parse HEAD~1)"
  mcp_full_head="$(git -C "$mcp_project" rev-parse HEAD)"
  git -C "$mcp_project" remote set-url origin https://github.com/fixture-owner/fixture-repo.git
  multi_pr_list="$(jq -n --arg head "$mcp_full_head" --arg checked "$mcp_box" \
    '[{number:1,headRefOid:$head,body:$checked},
      {number:2,headRefOid:$head,body:"- [ ] /skill-audit clean - PR create/push gate"}]')"
  n=$((n + 1))
  if [ "$(PATH="$fake_bin:$PATH" FAKE_PR_LIST="$multi_pr_list" \
      hook_verdict_at "$mcp_project" require-skill-audit.sh "$GITP origin gate-branch")" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  multiple open same-head PRs must block the gh-list push gate\n'
  fi

  rest_bin="$gate_tmp/rest-bin"
  mkdir -p "$rest_bin"
  ln -s "$(command -v git)" "$rest_bin/git"
  ln -s "$(command -v jq)" "$rest_bin/jq"
  printf '%s\n' '#!/usr/bin/env bash' 'printf "%s" "$FAKE_PR_LIST"' > "$rest_bin/curl"
  chmod +x "$rest_bin/curl"
  n=$((n + 1))
  if [ "$(PATH="$rest_bin:/usr/bin:/bin" GH_TOKEN=test FAKE_PR_LIST="$multi_pr_list" \
      hook_verdict_at "$mcp_project" require-skill-audit.sh "$GITP origin gate-branch")" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  multiple open same-head PRs must block the REST push gate\n'
  fi
  project_old_box="- [x] /project-review clean — merge gate @ ${mcp_old_head:0:12}"
  feature_old_box="- [x] /feature-docs synced — merge gate @ ${mcp_old_head:0:12}"
  n=$((n + 1))
  if [ "$(PATH="$fake_bin:$PATH" FAKE_PR_HEAD="$mcp_old_head" FAKE_PR_BODY="$project_old_box" \
      mcp_hook_verdict_at "$mcp_project" require-project-review.sh \
      mcp__github__merge_pull_request fixture-owner fixture-repo 7 '' '')" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  MCP merge PR head differing from local HEAD must block\n'
  fi
  n=$((n + 1))
  if [ "$(PATH="$fake_bin:$PATH" FAKE_PR_HEAD="$mcp_old_head" FAKE_PR_BODY="$feature_old_box" \
      mcp_hook_verdict_at "$mcp_project" require-feature-docs.sh \
      mcp__github__merge_pull_request fixture-owner fixture-repo 7 '' '')" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  feature-docs MCP merge differing PR head must block\n'
  fi
  n=$((n + 1))
  if [ "$(PATH="$fake_bin:$PATH" FAKE_PR_HEAD="$mcp_full_head" \
      FAKE_PR_BODY="- [x] /project-review clean — merge gate @ ${mcp_full_head:0:12}" \
      mcp_hook_verdict_at "$mcp_project" require-project-review.sh \
      mcp__github__merge_pull_request fixture-owner fixture-repo 7 "$mcp_full_head" '')" != 0 ]; then
    fails=$((fails + 1)); printf 'FAIL  MCP merge exact repository and PR/local HEAD should pass\n'
  fi
  n=$((n + 1))
  if [ "$(PATH="$fake_bin:$PATH" FAKE_PR_HEAD="$mcp_full_head" \
      FAKE_PR_BODY="- [x] /project-review clean — merge gate @ ${mcp_full_head:0:12}" \
      mcp_hook_verdict_at "$mcp_project" require-project-review.sh \
      mcp__github__merge_pull_request fixture-owner fixture-repo 7 "$mcp_old_head" '')" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  MCP merge wrong expected_head_sha must block\n'
  fi
  n=$((n + 1))
  if [ "$(PATH="$fake_bin:$PATH" FAKE_PR_HEAD="$mcp_full_head" FAKE_PR_BODY="$project_old_box" \
      mcp_hook_verdict_at "$mcp_project" require-project-review.sh \
      mcp__github__merge_pull_request foreign fixture-repo 7 '' '')" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  MCP merge foreign owner must block before PR lookup\n'
  fi
  n=$((n + 1))
  if [ "$(PATH="$fake_bin:$PATH" FAKE_PR_HEAD="$mcp_old_head" FAKE_PR_BODY="$project_old_box" \
      hook_verdict_at "$mcp_project" require-project-review.sh "$GHPR merge 7")" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  Bash numeric merge PR head differing from local HEAD must block\n'
  fi
  n=$((n + 1))
  if [ "$(PATH="$fake_bin:$PATH" FAKE_PR_HEAD="$mcp_old_head" FAKE_PR_BODY="$feature_old_box" \
      hook_verdict_at "$mcp_project" require-feature-docs.sh "$GHPR merge 7")" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  feature-docs Bash merge differing PR head must block\n'
  fi
  n=$((n + 1))
  if [ "$(PATH="$fake_bin:$PATH" FAKE_PR_HEAD="$mcp_full_head" \
      FAKE_PR_BODY="- [x] /project-review clean — merge gate @ ${mcp_full_head:0:12}" \
      hook_verdict_at "$mcp_project" require-project-review.sh \
      "$GHPR merge 7 --match-head-commit $mcp_full_head --squash")" != 0 ]; then
    fails=$((fails + 1)); printf 'FAIL  Bash numeric merge exact local PR/local HEAD should pass\n'
  fi
  n=$((n + 1))
  if [ "$(PATH="$fake_bin:$PATH" FAKE_PR_HEAD="$mcp_full_head" \
      FAKE_PR_BODY="- [x] /project-review clean — merge gate @ ${mcp_full_head:0:12}" \
      hook_verdict_at "$mcp_project" require-project-review.sh \
      "$GHPR merge https://github.com/foreign/repository/pull/7 --squash")" != 2 ]; then
    fails=$((fails + 1)); printf 'FAIL  Bash foreign pull URL must block before local PR audit\n'
  fi

  # A missing or successfully-sourced-but-incomplete shared library must block a guarded action.
  # These copies isolate the negative cases without modifying the repository under test.
  mkdir -p "$gate_tmp/missing/.claude/hooks" "$gate_tmp/defective/.claude/hooks"
  for hook in require-project-review.sh require-feature-docs.sh require-skill-audit.sh; do
    cp ".claude/hooks/$hook" "$gate_tmp/missing/.claude/hooks/$hook"
    cp ".claude/hooks/$hook" "$gate_tmp/defective/.claude/hooks/$hook"
  done
  : > "$gate_tmp/defective/.claude/hooks/pr-gate-lib.sh"

  for variant in missing defective; do
    n=$((n + 1))
    if [ "$(hook_verdict_at "$gate_tmp/$variant" require-project-review.sh "$GHPR merge 236")" != 2 ]; then
      fails=$((fails + 1)); printf 'FAIL  project-review must block with %s gate library\n' "$variant"
    fi
    n=$((n + 1))
    if [ "$(hook_verdict_at "$gate_tmp/$variant" require-feature-docs.sh "$GHPR merge 236")" != 2 ]; then
      fails=$((fails + 1)); printf 'FAIL  feature-docs must block with %s gate library\n' "$variant"
    fi
    n=$((n + 1))
    if [ "$(hook_verdict_at "$gate_tmp/$variant" require-skill-audit.sh "$GHPR create --body x")" != 2 ]; then
      fails=$((fails + 1)); printf 'FAIL  skill-audit must block with %s gate library\n' "$variant"
    fi
  done
fi

if [ "$fails" -eq 0 ]; then
  printf 'OK  pr-gate matcher: %d cases\n' "$n"
  exit 0
fi
printf 'FAILED  %d of %d pr-gate matcher cases\n' "$fails" "$n"
exit 1
