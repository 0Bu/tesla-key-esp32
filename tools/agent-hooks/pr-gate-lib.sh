#!/usr/bin/env bash
# Runner-neutral shared logic for the aggregate Tesla PR gate. Sourced, never run directly.
#
# THERE IS NO FILE MARKER. The pass-state of a review/audit lives as a TICKED, SHA-STAMPED
# checkbox in the pull request's body, e.g.:
#     - [x] `$project-review` clean — merge gate @ a1b2c3d4e5f6789012345678901234567890abcd
# Historical slash-form records remain accepted for existing PR bodies.
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

# Hooks verify this marker and the functions they consume immediately after sourcing. A missing,
# truncated or syntactically broken library is a hard block, never an unclassified/allowed action.
GATE_PR_LIB_API=3

agent_gate_run_bounded() {
  local seconds="$1" root
  shift
  root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  python3 "$root/run_with_timeout.py" "$seconds" "$@"
}

agent_gate_workdir_matches() {
  local root="$1" requested="${2:-}" root_real requested_real root_git requested_git
  [ -n "$requested" ] || return 1
  root_real="$(cd "$root" 2>/dev/null && pwd -P)" || return 1
  requested_real="$(cd "$requested" 2>/dev/null && pwd -P)" || return 1
  root_git="$(git -C "$root_real" rev-parse --show-toplevel 2>/dev/null)" || return 1
  requested_git="$(git -C "$requested_real" rev-parse --show-toplevel 2>/dev/null)" || return 1
  [ "$root_git" = "$requested_git" ]
}

# gate_feature_docs_relevant
#   Reads repo-relative changed paths on stdin and succeeds when docs/FEATURES.md's technical
#   catalog can have moved. Keep the release workflow set explicit: Renovate is dependency
#   maintenance, while build/sign/publish and preview cleanup are catalogued release behavior.
gate_feature_docs_relevant() {
  grep -Eq '^(main/|test/|sdkconfig\.defaults($|\.)|partitions\.csv$|\.claude/|\.github/PULL_REQUEST_TEMPLATE\.md$|tools/agent-hooks/|tools/agent-config/|docs/(index\.html|installer-bootstrap\.mjs|serial-port-release\.mjs|web-installer\.mjs|vendor/)|\.github/workflows/(build|signed-pr-preview|pr-preview-cleanup|pr-policy|bench-acceptance)\.yml$|scripts/release-relevance\.sh$)'
}

# gate_checkbox_status <content> <key>
#   Prints exactly one of:  "checked <sha>" | "checked" | "unchecked" | "absent" | "ambiguous"
#   A match is one complete canonical Markdown task-list line. Its leading checkbox is followed by
#   exactly `$<key>` (or the historical `/<key>` form), the key-specific success word, an em dash, the
#   key-specific gate phrase and
#   one standalone lowercase full 40-hex SHA. Optional Markdown code ticks around `/<key>` are accepted; no
#   other status/trailing prose is. Callers pass an actual PR body; command text/titles are never
#   inspected.
gate_checkbox_status() {
  local content="$1" key="$2"
  printf '%s' "$key" | grep -Eq '^(skill-audit|project-review|feature-docs|pr-hygiene)$' \
    || { printf 'absent\n'; return 0; }
  printf '%s' "$content" | python3 -c '
import re, sys

key = sys.argv[1]
spec = {
    "skill-audit": ("clean", "PR create/push gate"),
    "project-review": ("clean", "merge gate"),
    "feature-docs": ("synced", "merge gate"),
    "pr-hygiene": ("clean", "content gate"),
}[key]
fence_char = None
fence_length = 0
raw_mode = None
comment = False
tasks = []
fence_re = re.compile(r"^( {0,3})(`{3,}|~{3,})(.*)$")
task_re = re.compile(r"^[-*+]\s+\[([ xX])\]\s+(.*)$")
tag_re = re.compile(r"^</?([A-Za-z][A-Za-z0-9-]*)(?=[\s/>]|$)")
raw_tags = {"address","article","aside","base","basefont","blockquote","body","caption","center","col","colgroup","dd","details","dialog","dir","div","dl","dt","fieldset","figcaption","figure","footer","form","frame","frameset","h1","h2","h3","h4","h5","h6","head","header","hr","html","iframe","legend","li","link","main","menu","menuitem","nav","noframes","ol","optgroup","option","p","param","pre","script","search","section","style","summary","table","tbody","td","textarea","tfoot","th","thead","title","tr","track","ul"}

def fence(line):
    match = fence_re.match(line)
    if not match:
        return None
    token, rest = match.group(2), match.group(3)
    if token[0] == "`" and "`" in rest:
        return None
    return token[0], len(token), rest

def starts_raw(line):
    if len(line) - len(line.lstrip(" ")) > 3:
        return None
    text = line.lstrip()
    for opener, closer in (("<?", "?>"), ("<![CDATA[", "]]>") , ("<!", ">")):
        if text.startswith(opener):
            return ("delim", closer)
    match = tag_re.match(text)
    if match and match.group(1).lower() in raw_tags:
        tag = match.group(1).lower()
        if tag in {"pre", "script", "style", "textarea"}:
            return ("tag", tag)
        return ("blank", "")
    if match and ">" in text:
        return ("blank", "")
    return None

for line in sys.stdin.read().splitlines():
    part = fence(line)
    if fence_char is not None:
        if part and part[0] == fence_char and part[1] >= fence_length and not part[2].strip():
            fence_char = None
        continue
    if comment:
        if "-->" in line:
            comment = False
        continue
    if raw_mode:
        mode, end = raw_mode
        if mode == "blank" and not line.strip():
            raw_mode = None
        elif mode == "delim" and end in line:
            raw_mode = None
        elif mode == "tag" and re.search(rf"</{re.escape(end)}\s*>", line, re.I):
            raw_mode = None
        continue
    if part:
        fence_char, fence_length = part[:2]
        continue
    if line.lstrip().startswith(">"):  # blockquotes, including nested task examples
        continue
    if line.startswith("    ") or line.startswith("\t"):
        continue
    if "<!--" in line:
        if "-->" not in line.split("<!--", 1)[1]:
            comment = True
        continue
    raw_mode = starts_raw(line)
    if raw_mode:
        mode, end = raw_mode
        if mode == "delim" and end in line[line.find("<") + 2:]:
            raw_mode = None
        continue
    match = task_re.match(line)
    if match:
        tasks.append((match.group(1), match.group(2).rstrip()))

token = re.compile(rf"(?<![A-Za-z0-9-])(?:[$/]{re.escape(key)})(?![A-Za-z0-9-])", re.I)
matching = [(mark, text) for mark, text in tasks if token.search(text)]
if not matching:
    print("absent")
    raise SystemExit
if len(matching) != 1:
    print("ambiguous")
    raise SystemExit
mark, text = matching[0]
success, phrase = spec
record = re.compile(
    rf"^`?[$/]{re.escape(key)}`?\s+{re.escape(success)}\s+—\s+{re.escape(phrase)}\s+@\s+([0-9a-f]{{40}})$",
    re.I,
)
match = record.fullmatch(text)
if not match:
    print("absent")
elif match.group(1) != match.group(1).lower():
    print("absent")
elif mark.lower() != "x":
    print("unchecked")
else:
    print("checked " + match.group(1))
' "$key"
}

# gate_sha_matches <a> <b>  -> 0 only for two identical lowercase full commit SHAs.
gate_sha_matches() {
  printf '%s' "$1" | grep -Eq '^[0-9a-f]{40}$' || return 1
  printf '%s' "$2" | grep -Eq '^[0-9a-f]{40}$' || return 1
  [ "$1" = "$2" ]
}

# gate_bash_segments <cmd>
#   Prints a Bash command with one conservative, shell-like segment per line, so a matcher can
#   anchor at `^` and still see an action that is CHAINED or wrapped rather than first.
#
#   THIS EXISTS BECAUSE ANCHORING ALONE WAS A COMPLETE BYPASS. The gates used to run their
#   `grep -Eq '^git[[:space:]]+push…'` against the raw command with only a leading `cd … &&`
#   stripped. `grep` anchors `^` per PHYSICAL LINE, so:
#       git commit -m x <newline> git push origin br   -> matched   (push starts a line)
#       git commit -m x && git push origin br          -> matched NOTHING
#   and an unmatched command falls through `[ -n "$action" ] || exit 0`, i.e. the hook allows
#   the call having checked nothing at all. Not a mis-classification — no classification. Seen
#   live: a `git push` at a commit whose PR stamp was stale went through behind
#   `gh pr edit … &&`, minutes after a standalone push was correctly blocked. The same hole let
#   `git checkout main && gh pr merge …` past the MERGE gate, which is the one that matters most.
#
#   Splitting on shell separators and grouping characters covers `&&`, `||`, subshells and brace
#   groups. Leading whitespace, control words, `command`, `env` and `VAR=value` prefixes are
#   stripped per segment so the real command starts the line. `cd` is deliberately retained:
#   gate_bash_actions counts it as another segment and therefore refuses a later guarded action.
#
#   Deliberately textual: it can over-split inside quotes (`git commit -m "a && b"`) and therefore
#   conservatively classify guarded-looking quoted data. Keep publish/merge actions in separate
#   tool calls when a command has unusual quoted shell syntax.
gate_bash_segments() {
  printf '%s' "$1" \
    | tr ';|&(){}' '\n\n\n\n\n\n\n' \
    | sed -E '
        s/^[[:space:]]+//
        :again
        s/^(then|do|else)[[:space:]]+//
        t again
        s/^!+[[:space:]]*//
        t again
        s/^command([[:space:]]+--)?[[:space:]]+//
        t again
        /^env[[:space:]]+([^[:space:]]+[[:space:]]+)*(-S|--split-string)(=|[[:space:]]|$)/b keep_env
        s/^env([[:space:]]+(-[i0]|--ignore-environment|--null|--|(-u|-C|-P|--unset|--chdir)[[:space:]]+[^[:space:]]+|--(unset|chdir)=[^[:space:]]+|[A-Za-z_][A-Za-z0-9_]*=[^[:space:]]*))*[[:space:]]+//
        t again
        :keep_env
        s/^([A-Za-z_][A-Za-z0-9_]*=[^[:space:]]*[[:space:]]+)+//
        t again
      '
}

# gate_action_env_name_is_unsafe <name>
#   Succeeds for an environment variable that can change the repository, object/ref/config view,
#   executable, GitHub repository/host or working directory of a guarded action. Git's tracing
#   variables are an explicit diagnostic-only exception; arbitrary unrelated variables remain
#   usable as harmless wrappers.
gate_action_env_name_is_unsafe() {
  case "$1" in
    GIT_TRACE|GIT_TRACE_[A-Za-z0-9_]*|GIT_TRACE2|GIT_TRACE2_[A-Za-z0-9_]*) return 1 ;;
    GIT_*|GH_REPO|GH_HOST|GH_CONFIG_DIR|GITHUB_REPOSITORY|HOME|XDG_CONFIG_HOME|PATH|PWD|OLDPWD|CDPATH|BASH_ENV|ENV|LD_PRELOAD|DYLD_*) return 0 ;;
    *) return 1 ;;
  esac
}

# gate_action_env_context_is_unsafe <raw-command>
#   Detect context-changing assignment/env spellings that gate_bash_segments intentionally strips
#   to expose the underlying command. PreToolUse validates GATE_PROJ/current GitHub repository,
#   while these prefixes apply only when the shell later executes the action; losing them would
#   validate one target and mutate another.
gate_action_env_context_is_unsafe() {
  local raw="$1" names name unsets

  names="$(printf '%s' "$raw" \
    | grep -Eo '(^|[[:space:];|&(){}])([A-Za-z_][A-Za-z0-9_]*)=' \
    | sed -E 's/^[[:space:];|&(){}]+//; s/=$//' || true)"
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    gate_action_env_name_is_unsafe "$name" && return 0
  done <<< "$names"

  unsets="$(printf '%s' "$raw" \
    | grep -Eo -- '(-u|--unset)(=|[[:space:]]+)[A-Za-z_][A-Za-z0-9_]*' \
    | sed -E 's/^(-u|--unset)(=|[[:space:]]+)//' || true)"
  while IFS= read -r name; do
    [ -n "$name" ] || continue
    gate_action_env_name_is_unsafe "$name" && return 0
  done <<< "$unsets"

  # Clearing the environment, changing env's cwd/search path, or selecting a different command
  # lookup cannot be proven equivalent to the context the hook just audited.
  printf '%s' "$raw" | grep -Eq \
    '(^|[[:space:]])(-i|--ignore-environment|-0|--null|-C|--chdir|-P)(=|[[:space:]]|$)' \
    && return 0
  return 1
}

# gate_gh_context_is_unsafe <segment>
#   GitHub CLI repository/host overrides would make the hook read one PR and mutate another.
#   Keep the accepted local spelling bound to the current project's configured repository.
gate_gh_context_is_unsafe() {
  command -v python3 >/dev/null 2>&1 || return 0
  python3 - "$1" <<'PY'
import shlex
import sys

try:
    words = shlex.split(sys.argv[1], posix=True)
except ValueError:
    raise SystemExit(0)
for token in words:
    if (token in {"-R", "-H", "--repo", "--hostname", "--head"}
            or token.startswith(("-R", "-H", "--repo=", "--hostname=", "--head="))):
        raise SystemExit(0)
raise SystemExit(1)
PY
}

# gate_lex_guarded_action <segment>
#   Non-executing shell-word recognition for quoted/escaped/concatenated executable spellings,
#   quoted/path-qualified command/env wrappers and gh/git global options around the subcommand.
#   Direct canonical forms are handled without Python; this conservative fallback exists so
#   `'git'`, g\it, `"/path/gh"`, `'env' git push`, and `gh pr -R ... merge` cannot become
#   unclassified actions. Every action found here is intentionally returned as unsafe: the shell
#   spelling was not one of the canonical forms whose exact context the hook can prove.
gate_lex_guarded_action() {
  command -v python3 >/dev/null 2>&1 || return 2
  python3 - "$1" <<'PY'
import os
import re
import shlex
import sys

try:
    raw_segment = sys.argv[1]
    words = shlex.split(raw_segment, posix=True)
except ValueError:
    raise SystemExit(2)
if not words:
    raise SystemExit(0)

def unwrap_wrapper(argv):
    """Return words beginning at a conservatively recognised command/env payload."""
    while argv:
        exe = os.path.basename(argv[0])
        # eval/shell payloads may contain an assignment statement before the dynamically selected
        # executable. shlex keeps the command separator attached; discard only that exact leading
        # assignment form so the guarded command after it remains visible.
        if re.match(r"^[A-Za-z_][A-Za-z0-9_]*=.*;$", argv[0]):
            argv = argv[1:]
            continue
        if exe in {"command", "builtin", "exec"}:
            i = 1
            while i < len(argv) and argv[i].startswith("-"):
                if argv[i] == "--":
                    i += 1
                    break
                i += 1
            argv = argv[i:]
            continue
        if exe in {"bash", "sh", "zsh"}:
            i = 1
            command_string = None
            while i < len(argv) and argv[i].startswith("-"):
                option = argv[i]
                i += 1
                if "c" in option.lstrip("-"):
                    if i >= len(argv):
                        return []
                    command_string = argv[i]
                    i += 1
                    break
            if command_string is None:
                return argv
            try:
                argv = shlex.split(command_string, posix=True) + argv[i:]
            except ValueError:
                return []
            continue
        if exe == "eval":
            if len(argv) < 2:
                return []
            try:
                argv = shlex.split(" ".join(argv[1:]), posix=True)
            except ValueError:
                return []
            continue
        if exe == "env":
            i = 1
            reparsed = False
            value_options = {"-u", "-C", "-P", "-S", "--unset", "--chdir", "--split-string"}
            while i < len(argv):
                token = argv[i]
                if token == "--":
                    i += 1
                    break
                if re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", token):
                    i += 1
                    continue
                if token.startswith("-") and token != "-":
                    key = token.split("=", 1)[0]
                    i += 1
                    # Darwin/GNU env accept attached short-option operands (-uNAME, -Cdir,
                    # -Ppath and -Sstring) as well as combined flag-only forms such as -iv.
                    attached_key = None
                    attached_value = None
                    for short in ("-u", "-C", "-P", "-S"):
                        if token.startswith(short) and token != short:
                            attached_key = short
                            attached_value = token[len(short):].lstrip("=")
                            break
                    if attached_key is not None:
                        key = attached_key
                    if key in value_options:
                        if attached_value is not None:
                            option_value = attached_value
                        elif "=" in token:
                            option_value = token.split("=", 1)[1]
                        else:
                            if i >= len(argv):
                                return []
                            option_value = argv[i]
                            i += 1
                        if key in {"-S", "--split-string"}:
                            # `env -S/--split-string` reparses its operand into the executable and
                            # argv. Model that non-executingly; otherwise the complete guarded
                            # action could hide inside the value that a normal option parser skips.
                            try:
                                split_words = shlex.split(option_value, posix=True)
                            except ValueError:
                                return []
                            argv = split_words + argv[i:]
                            reparsed = True
                            break
                    continue
                break
            else:
                return []
            if not reparsed:
                argv = argv[i:]
            continue
        break
    return argv

def next_command(start, value_options):
    i = start
    while i < len(words):
        token = words[i]
        if token == "--":
            i += 1
            return (words[i], i + 1) if i < len(words) else (None, i)
        if not token.startswith("-") or token == "-":
            return token, i + 1
        key = token.split("=", 1)[0]
        i += 1
        if key in value_options and "=" not in token:
            if i >= len(words):
                return None, i
            i += 1
    return None, i

words = unwrap_wrapper(words)
if not words:
    raise SystemExit(0)

def dynamic_word(token):
    # Parameter/command expansion and command-position globbing are evaluated only after the
    # hook. They can turn a token that was not git/gh/push into one, so exact target binding is
    # impossible at PreToolUse time.
    return any(char in token for char in ("$", "`", "*", "?", "[", "]", "{", "}"))

def gh_api_is_write(argv):
    """Conservatively classify an official `gh api` mutation without executing it."""
    method = None
    has_body = False
    mutation_text = False
    i = 0
    while i < len(argv):
        token = argv[i]
        if dynamic_word(token):
            # Word/command expansion can inject a method, field or input flag after PreToolUse.
            return True
        if token in {"-X", "--method"}:
            i += 1
            if i >= len(argv):
                return True
            method = argv[i].upper()
        elif token.startswith("--method="):
            method = token.split("=", 1)[1].upper()
        elif token.startswith("-X") and token != "-X":
            method = token[2:].lstrip("=").upper()
        elif token in {"-f", "-F", "--field", "--raw-field", "--input"}:
            has_body = True
            i += 1
            if i >= len(argv):
                return True
            mutation_text = mutation_text or "mutation" in argv[i].lower()
        elif token.startswith(("--field=", "--raw-field=", "--input=")):
            has_body = True
            mutation_text = mutation_text or "mutation" in token.lower()
        elif ((token.startswith("-f") or token.startswith("-F"))
              and token not in {"-f", "-F"}):
            has_body = True
            mutation_text = mutation_text or "mutation" in token.lower()
        elif "mutation" in token.lower():
            mutation_text = True
        i += 1
    if mutation_text:
        return True
    if method is not None:
        return method != "GET"
    # gh switches from GET to POST when fields or an input body are supplied without -X/--method.
    return has_body

exe = os.path.basename(words[0])
if dynamic_word(words[0]):
    tail = words[1:]
    matched = False
    if "push" in tail:
        print("push")
        matched = True
    if "pr" in tail:
        pr_offset = tail.index("pr")
        later = tail[pr_offset + 1:]
        if "create" in later or "new" in later:
            print("create")
            matched = True
        elif "merge" in later:
            print("merge")
            matched = True
        elif any(dynamic_word(token) for token in later):
            print("github_dynamic")
            matched = True
    # If both the executable and the words that would identify its guarded subcommand are chosen
    # only at shell-expansion time, no narrower classification is defensible. Brace expansion in
    # command position (`{gh,pr,merge}`) is the same class: Bash can emit several argv words from
    # the one token shlex sees.
    if not matched and (any(dynamic_word(token) for token in tail)
                        or any(char in words[0] for char in "{}")
                        or not tail):
        print("all_dynamic")
    raise SystemExit(0)
if exe == "git":
    command, command_offset = next_command(1, {
        "-C", "-c", "--exec-path", "--git-dir", "--work-tree", "--namespace",
        "--super-prefix", "--config-env",
    })
    if command == "push":
        print("push")
    elif command is not None and dynamic_word(command):
        print("push")
    elif command_offset > 2 and command not in {
        "add", "am", "apply", "archive", "bisect", "blame", "branch", "bundle", "checkout",
        "cherry", "cherry-pick", "clean", "clone", "commit", "config", "describe", "diff",
        "diff-tree", "fetch", "for-each-ref", "format-patch", "fsck", "gc", "grep", "help",
        "init", "log", "ls-files", "ls-remote", "ls-tree", "merge", "merge-base", "mv",
        "name-rev", "notes", "pull", "range-diff", "rebase", "reflog", "remote", "reset",
        "restore", "rev-list", "rev-parse", "rm", "shortlog", "show", "show-ref", "sparse-checkout",
        "stash", "status", "submodule", "switch", "tag", "update-index", "verify-commit",
        "verify-tag", "version", "worktree",
    }:
        # Global repo/config selectors can source an alias from a different config (including an
        # indirect include.path). An unknown subcommand may therefore be push even when GATE_PROJ
        # has no such alias.
        print("push")
elif exe == "gh":
    command, offset = next_command(1, {"-R", "--repo", "--hostname"})
    if command is not None and dynamic_word(command):
        print("github_dynamic")
    elif command == "pr":
        action, _ = next_command(offset, {"-R", "--repo", "--hostname"})
        if action in {"create", "new", "merge"}:
            print("create" if action == "new" else action)
        elif action is not None and dynamic_word(action):
            print("github_dynamic")
    elif command == "api" and gh_api_is_write(words[offset:]):
        print("github_write")
PY
}

# gate_git_alias_kind <git-subcommand>
#   Resolve only the exact repository's configured alias chain. A regular alias that reaches push
#   is reported as push; a shell alias is opaque and therefore unsafe for every guarded action.
gate_git_alias_kind() {
  local command_name="$1" expansion first depth=0 seen=" "
  printf '%s' "$command_name" | grep -Eq '^[A-Za-z0-9_.-]+$' || return 1
  while [ "$depth" -lt 8 ]; do
    case "$seen" in *" $command_name "*) printf 'opaque\n'; return 0 ;; esac
    seen="$seen$command_name "
    expansion="$(git -C "${GATE_PROJ:-$PWD}" config --get "alias.$command_name" 2>/dev/null)" \
      || return 1
    [ -n "$expansion" ] || return 1
    case "$expansion" in !*) printf 'opaque\n'; return 0 ;; esac
    first="$(python3 - "$expansion" <<'PY'
import shlex
import sys
try:
    words = shlex.split(sys.argv[1], posix=True)
except ValueError:
    raise SystemExit(2)
if words:
    print(words[0])
PY
    )" || { printf 'opaque\n'; return 0; }
    case "$first" in
      push) printf 'push\n'; return 0 ;;
      '') printf 'opaque\n'; return 0 ;;
      *) command_name="$first" ;;
    esac
    depth=$((depth + 1))
  done
  printf 'opaque\n'
}

# gate_gh_alias_kind <gh-command>
#   gh aliases are local config and require no network. A regular alias is lexed as `gh <value>`;
#   shell aliases are opaque because they may invoke git/gh through arbitrary shell expansion.
gate_gh_alias_kind() {
  local command_name="$1" aliases expansion kind
  command -v gh >/dev/null 2>&1 || return 1
  printf '%s' "$command_name" | grep -Eq '^[A-Za-z0-9_.-]+$' || return 1
  aliases="$(GH_HOST=github.com gh alias list 2>/dev/null)" || return 1
  expansion="$(printf '%s\n' "$aliases" \
    | awk -F ': ' -v name="$command_name" '$1 == name { sub(/^[^:]*: /, ""); print; found++ }
      END { if (found != 1) exit 1 }')" || return 1
  case "$expansion" in !*) printf 'opaque\n'; return 0 ;; esac
  kind="$(gate_lex_guarded_action "gh $expansion" 2>/dev/null || true)"
  case "$kind" in
    create|merge|github_dynamic) printf '%s\n' "$kind"; return 0 ;;
    github_write|all_dynamic) printf 'opaque\n'; return 0 ;;
  esac
  return 1
}

# gate_bash_actions <cmd>
#   Prints every guarded Bash action as `<kind><TAB><payload>`, one record per segment. Kinds are
#   `create`, `merge`, and `push`. Consumers MUST count records and fail closed on more than one:
#   validating only the first action in `gh pr merge 1 || gh pr merge 2` would gate PR 1 while PR 2
#   could still execute. Every guarded action must also be the sole non-empty shell segment in its
#   Bash tool call: a preceding commit/config/PR-edit/cd could mutate the audited state after
#   PreToolUse, while a following segment makes the call non-atomic to reason about. Such actions
#   carry `__GATE_UNSAFE_SHELL_CONTEXT__`. A Git global option or repository/config-affecting env
#   prefix before `push` similarly carries a fail-closed context marker. The audit anchor is
#   resolved only in GATE_PROJ, so every alternate spelling must fail closed instead of validating
#   the wrong HEAD. The parser stays deliberately textual; conservative false positives are
#   acceptable.
gate_bash_actions() {
  local raw="$1" normalized_raw segments segment payload git_rest segment_count shell_unsafe=0
  local action_env_unsafe=0 emitted_create emitted_merge emitted_push lex_kind raw_lex_kind scan_raw
  local dynamic_scan git_command alias_kind gh_rest gh_command line_continuation
  # Remove shell line continuations before classifying the command. Bash 3.2 and Bash 5.x assign
  # opposite meanings to the one- vs two-backslash inline patterns here. Holding the exact bytes
  # in a quoted variable avoids that pattern-parser drift without broadening newline handling.
  line_continuation=$'\\\n'
  normalized_raw="${raw//"$line_continuation"/}"
  [ "$normalized_raw" = "$raw" ] || shell_unsafe=1
  segments="$(gate_bash_segments "$normalized_raw")"
  # Count raw segments before wrapper/assignment normalisation. A standalone `FOO=x; git push`
  # is still two commands even though the first segment normalises to an empty string.
  segment_count="$(printf '%s' "$normalized_raw" | tr ';|&(){}' '\n\n\n\n\n\n\n' \
    | awk 'NF { n++ } END { print n+0 }')"
  [ "$segment_count" -eq 1 ] || shell_unsafe=1
  gate_action_env_context_is_unsafe "$normalized_raw" && action_env_unsafe=1
  case "$normalized_raw" in *'>'*|*'<'*) shell_unsafe=1 ;; esac

  # Shell expansion can execute a guarded action before the apparent outer command. Remove simple
  # escaping only for this conservative hint scan; any dynamic construct containing an action is
  # unsafe even when it appears inside echo/assignment/quoted text.
  scan_raw="${normalized_raw//\\/}"
  # Single-quoted strings are the one shell context where backticks/$() are literal. Removing those
  # spans keeps canonical literal PR bodies usable while double-quoted/unquoted expansions block.
  dynamic_scan="$(printf '%s' "$scan_raw" | sed -E "s/'[^']*'//g")"
  # Bash array expansion can supply several command words after PreToolUse (for example an array
  # containing `gh pr`). It is not representable by the scalar shlex fallback; when the remaining
  # literal action is guarded, reject the whole dynamic argv form.
  if printf '%s' "$normalized_raw" | grep -Eq '\$\{[^}]+\[(\@|\*)\]\}'; then
    if printf '%s' "$normalized_raw" | grep -Eq '(^|[[:space:]])push([[:space:]]|$)'; then
      printf 'push\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'; return 0
    fi
    if printf '%s' "$normalized_raw" | grep -Eq '(^|[[:space:]])(create|new)([[:space:]]|$)'; then
      printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'; return 0
    fi
    if printf '%s' "$normalized_raw" | grep -Eq '(^|[[:space:]])merge([[:space:]]|$)'; then
      printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'; return 0
    fi
  fi
  if printf '%s' "$dynamic_scan" | grep -Eq '(`|\$\()'; then
    if printf '%s' "$dynamic_scan" | grep -Eq 'git([^[:alnum:]_]|.)*push([[:space:]]|$)'; then
      printf 'push\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
    fi
    if printf '%s' "$dynamic_scan" | grep -Eq 'gh([^[:alnum:]_]|.)*pr([^[:alnum:]_]|.)*(create|new)([[:space:]]|$)'; then
      printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
    fi
    if printf '%s' "$dynamic_scan" | grep -Eq 'gh([^[:alnum:]_]|.)*pr([^[:alnum:]_]|.)*merge([[:space:]]|$)'; then
      printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
    fi
  fi

  # Before textual prefix normalisation, lex the complete standalone command. This catches env's
  # attached options, quoted wrappers, shell -c/eval, ANSI-C executable quoting and other forms
  # that sed must not reinterpret as canonical. Canonical direct/harmless-wrapper forms continue
  # through the exact payload parser below; every non-canonical spelling is blocked here.
  raw_lex_kind="$(gate_lex_guarded_action "$normalized_raw" 2>/dev/null || true)"
  if [ -n "$raw_lex_kind" ]; then
    if [ "$segment_count" -ne 1 ]; then
      case "$raw_lex_kind" in
        all_dynamic)
          printf 'push\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          return 0
          ;;
        github_write)
          printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          return 0
          ;;
      esac
      # Ordinary compound calls are parsed segment-by-segment below so every guarded action is
      # reported. Only an opaque eval/shell-command string can be split *inside its quoted
      # payload* by the conservative segmenter; retain the full-command lexer result for those.
      if printf '%s' "$normalized_raw" | grep -Eq \
          '(^[[:space:]]*([^[:space:]]*/)?(eval|bash|sh|zsh)([[:space:]]|$)|\$\{)'; then
        case "$raw_lex_kind" in
          push) printf 'push\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__' ;;
          create) printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__' ;;
          merge) printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__' ;;
          github_dynamic)
            printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
            printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
            ;;
          github_write)
            printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
            printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
            ;;
          all_dynamic)
            printf 'push\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
            printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
            printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
            ;;
        esac
        return 0
      fi
    else
      case "$raw_lex_kind" in
        push)   printf '%s' "$segments" | grep -Eq '^git[[:space:]]+push([[:space:]]|$)' || {
                  printf 'push\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'; return 0; } ;;
        create) printf '%s' "$segments" | grep -Eq '^gh[[:space:]]+pr[[:space:]]+create([[:space:]]|$)' || {
                  printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'; return 0; } ;;
        merge)  printf '%s' "$segments" | grep -Eq '^gh[[:space:]]+pr[[:space:]]+merge([[:space:]]|$)' || {
                  printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'; return 0; } ;;
        github_dynamic)
          printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          return 0
          ;;
        github_write)
          printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          return 0
          ;;
        all_dynamic)
          printf 'push\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          return 0
          ;;
      esac
    fi
  fi

  # Direct git-push executables and per-command alias definitions can invoke push without the
  # normal `git push` word pair. Their audited repository/ref context is not provable.
  if printf '%s' "$scan_raw" | grep -Eq '(^|[[:space:]/)])git-push([[:space:]]|$)' \
      || printf '%s' "$scan_raw" | grep -Eq '(^|[[:space:]])git[[:space:]].*-c[[:space:]]+alias\.[^=[:space:]]+='; then
    printf 'push\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
    return 0
  fi

  while IFS= read -r segment || [ -n "$segment" ]; do
    emitted_create=0; emitted_merge=0; emitted_push=0
    # A path-qualified executable may be a different binary/wrapper from the `git`/`gh` whose
    # repository and credentials the hook validates. Recognise the guarded intent, but never treat
    # an arbitrary absolute/relative path as a transparent spelling.
    if printf '%s' "$segment" | grep -Eq \
        '^([^[:space:]]*/)+gh[[:space:]]+([^[:space:]]+[[:space:]]+)*pr[[:space:]]+create([[:space:]]|$)'; then
      printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
      emitted_create=1
    fi
    if printf '%s' "$segment" | grep -Eq \
        '^([^[:space:]]*/)+gh[[:space:]]+([^[:space:]]+[[:space:]]+)*pr[[:space:]]+merge([[:space:]]|$)'; then
      printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
      emitted_merge=1
    fi
    if printf '%s' "$segment" | grep -Eq \
        '^([^[:space:]]*/)+git[[:space:]]+([^[:space:]]+[[:space:]]+)*push([[:space:]]|$)'; then
      printf 'push\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
      emitted_push=1
    fi
    if printf '%s' "$segment" | grep -Eq \
        '^gh[[:space:]]+-[^[:space:]]*([[:space:]]+[^[:space:]]+)*[[:space:]]+pr[[:space:]]+create([[:space:]]|$)'; then
      printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
      emitted_create=1
    fi
    if printf '%s' "$segment" | grep -Eq \
        '^gh[[:space:]]+-[^[:space:]]*([[:space:]]+[^[:space:]]+)*[[:space:]]+pr[[:space:]]+merge([[:space:]]|$)'; then
      printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
      emitted_merge=1
    fi
    if printf '%s' "$segment" \
        | grep -Eq '^gh[[:space:]]+pr[[:space:]]+create([[:space:]]|$)'; then
      payload="$(printf '%s' "$segment" \
        | sed -E 's/^gh[[:space:]]+pr[[:space:]]+create[[:space:]]*//')"
      if [ "$shell_unsafe" -ne 0 ] || [ "$action_env_unsafe" -ne 0 ] \
          || gate_gh_context_is_unsafe "$segment"; then
        payload=__GATE_UNSAFE_SHELL_CONTEXT__
      fi
      printf 'create\t%s\n' "$payload"
      emitted_create=1
    fi
    if printf '%s' "$segment" \
        | grep -Eq '^gh[[:space:]]+pr[[:space:]]+merge([[:space:]]|$)'; then
      payload="$(printf '%s' "$segment" \
        | sed -E 's/^gh[[:space:]]+pr[[:space:]]+merge[[:space:]]*//')"
      if [ "$shell_unsafe" -ne 0 ] || [ "$action_env_unsafe" -ne 0 ] \
          || gate_gh_context_is_unsafe "$segment"; then
        payload=__GATE_UNSAFE_SHELL_CONTEXT__
      fi
      printf 'merge\t%s\n' "$payload"
      emitted_merge=1
    fi
    git_rest="$(printf '%s' "$segment" | sed -nE 's/^git[[:space:]]+//p')"
    case "$git_rest" in
      push|push[[:space:]]*)
        payload="$(printf '%s' "$git_rest" | sed -E 's/^push([[:space:]]+|$)//')"
        if [ "$shell_unsafe" -ne 0 ]; then
          payload=__GATE_UNSAFE_SHELL_CONTEXT__
        elif [ "$action_env_unsafe" -ne 0 ]; then
          payload=__GATE_UNSAFE_GIT_GLOBAL_CONTEXT__
        fi
        printf 'push\t%s\n' "$payload"
        emitted_push=1
        ;;
      -*|\"-*|\'-*)
        # Any option before the subcommand changes or ambiguously influences Git's execution
        # context. Classify a later push so the hook blocks it; do not strip the very evidence
        # needed to make that decision. Non-push Git commands remain unclassified.
        if printf '%s' "$git_rest" \
            | grep -Eq '(^|[[:space:]])push([[:space:]]|$)' \
            || printf '%s' "$git_rest" | grep -Eq \
              '^(--config-env(=|[[:space:]])|-c[[:space:]]+alias\.)'; then
          printf 'push\t%s\n' '__GATE_UNSAFE_GIT_GLOBAL_CONTEXT__'
          emitted_push=1
        fi
        ;;
    esac

    if [ "$emitted_push" -eq 0 ] && [ -n "$git_rest" ]; then
      git_command="${git_rest%%[[:space:]]*}"
      alias_kind="$(gate_git_alias_kind "$git_command" 2>/dev/null || true)"
      case "$alias_kind" in
        push)
          printf 'push\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          emitted_push=1
          ;;
        opaque)
          printf 'push\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          emitted_push=1; emitted_create=1; emitted_merge=1
          ;;
      esac
      if [ "$emitted_push" -eq 0 ] && [ "$action_env_unsafe" -ne 0 ]; then
        printf 'push\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
        emitted_push=1
      fi
    fi

    if printf '%s' "$segment" | grep -Eq '^gh[[:space:]]+'; then
      gh_rest="$(printf '%s' "$segment" | sed -E 's/^gh[[:space:]]+//')"
      gh_command="${gh_rest%%[[:space:]]*}"
      alias_kind="$(gate_gh_alias_kind "$gh_command" 2>/dev/null || true)"
      case "$alias_kind" in
        create)
          [ "$emitted_create" -ne 0 ] || printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          emitted_create=1
          ;;
        merge)
          [ "$emitted_merge" -ne 0 ] || printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          emitted_merge=1
          ;;
        github_dynamic|opaque)
          [ "$emitted_create" -ne 0 ] || printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          [ "$emitted_merge" -ne 0 ] || printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          emitted_create=1; emitted_merge=1
          ;;
      esac
      if [ "$emitted_create" -eq 0 ] && [ "$emitted_merge" -eq 0 ] \
          && [ "$action_env_unsafe" -ne 0 ]; then
        printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
        printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
        emitted_create=1; emitted_merge=1
      fi
    fi

    lex_kind="$(gate_lex_guarded_action "$segment" 2>/dev/null || true)"
    case "$lex_kind" in
      create)
        if [ "$emitted_create" -eq 0 ]; then
          printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          emitted_create=1
        fi
        ;;
      merge)
        if [ "$emitted_merge" -eq 0 ]; then
          printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          emitted_merge=1
        fi
        ;;
      push)
        if [ "$emitted_push" -eq 0 ]; then
          printf 'push\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          emitted_push=1
        fi
        ;;
      github_dynamic)
        if [ "$emitted_create" -eq 0 ]; then
          printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          emitted_create=1
        fi
        if [ "$emitted_merge" -eq 0 ]; then
          printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          emitted_merge=1
        fi
        ;;
      github_write)
        if [ "$emitted_create" -eq 0 ]; then
          printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          emitted_create=1
        fi
        if [ "$emitted_merge" -eq 0 ]; then
          printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          emitted_merge=1
        fi
        ;;
      all_dynamic)
        if [ "$emitted_push" -eq 0 ]; then
          printf 'push\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          emitted_push=1
        fi
        if [ "$emitted_create" -eq 0 ]; then
          printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          emitted_create=1
        fi
        if [ "$emitted_merge" -eq 0 ]; then
          printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
          emitted_merge=1
        fi
        ;;
    esac

    # Unknown execution wrappers (shell -c/eval/sudo/etc.) are not provably transparent. If their
    # textual payload contains a guarded action, classify it with the unsafe marker instead of
    # allowing the spelling to bypass the hook. Ordinary echo/commit arguments are not commands.
    if ! printf '%s' "$segment" | grep -Eq \
        '^([^[:space:]]*/)?(bash|sh|zsh|env|eval|exec|sudo|nohup|nice|time|timeout|xargs)([[:space:]]|$)'; then
      continue
    fi
    if [ "$emitted_create" -eq 0 ] && printf '%s' "$segment" \
        | grep -Eq 'gh[[:space:]]+pr[[:space:]]+create([[:space:]]|$)'; then
      printf 'create\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
    fi
    if [ "$emitted_merge" -eq 0 ] && printf '%s' "$segment" \
        | grep -Eq 'gh[[:space:]]+pr[[:space:]]+merge([[:space:]]|$)'; then
      printf 'merge\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
    fi
    if [ "$emitted_push" -eq 0 ] && printf '%s' "$segment" \
        | grep -Eq 'git[[:space:]]+([^[:space:]]+[[:space:]]+)*push([[:space:]]|$)'; then
      printf 'push\t%s\n' '__GATE_UNSAFE_SHELL_CONTEXT__'
    fi
  done <<< "$segments"
}

# gate_pr_create_body <arguments-after-gh-pr-create> <execution-cwd>
#   Print only the literal inline body or the contents of one exact body file. The body option must
#   be the FIRST create argument (and the only body source), so a preceding option cannot consume a
#   body-looking token as its own operand. Titles and unrelated arguments are never searched for the
#   checkbox. Shell expansion is intentionally not performed; dynamic/ambiguous body sources fail
#   closed and should be replaced by a regular body file. Python's shlex is a non-executing lexer,
#   never eval.
gate_pr_create_body() {
  local args="$1" execution_cwd="${2:-}" parsed kind value resolved
  command -v python3 >/dev/null 2>&1 && command -v jq >/dev/null 2>&1 || return 2
  parsed="$(python3 - "$args" <<'PY'
import json
import re
import shlex
import sys

try:
    tokens = shlex.split(sys.argv[1], posix=True)
except ValueError:
    raise SystemExit(2)

if not tokens:
    raise SystemExit(2)

first = tokens[0]
consumed = 1
if first in ("-b", "--body", "-F", "--body-file"):
    if len(tokens) < 2:
        raise SystemExit(2)
    kind = "file" if first in ("-F", "--body-file") else "inline"
    value = tokens[1]
    consumed = 2
elif first.startswith("--body-file="):
    kind, value = "file", first.split("=", 1)[1]
elif first.startswith("--body="):
    kind, value = "inline", first.split("=", 1)[1]
else:
    # gh/pflag options consume operands. Requiring the body first prevents `--title --body=...`
    # from being mistaken for a body when gh actually treats that token as the title value.
    raise SystemExit(2)

# A later second body source is ambiguous even when the first one is valid.
for token in tokens[consumed:]:
    if (token in ("-b", "--body", "-F", "--body-file")
            or token.startswith(("--body=", "--body-file="))
            or token.startswith("-b") and not token.startswith("--")
            or token.startswith("-F") and not token.startswith("--")):
        raise SystemExit(2)

if not value or "\x00" in value or "\n" in value and kind == "file":
    raise SystemExit(2)
# Inline body text must be one literal single-quoted shell word. Double/unquoted forms can expand
# variables/backticks after PreToolUse, changing the body that gh actually submits. Complex text
# belongs in a regular --body-file, which the hook reads directly.
raw = sys.argv[1]
if kind == "inline":
    literal = None
    for pattern in (
        r"^\s*(?:-b|--body)\s+'([^']*)'(?:\s|$)",
        r"^\s*--body='([^']*)'(?:\s|$)",
    ):
        match = re.match(pattern, raw, flags=re.DOTALL)
        if match:
            if literal is not None:
                raise SystemExit(2)
            literal = match.group(1)
    if literal is None or literal != value:
        raise SystemExit(2)
else:
    # Body-file paths must also be static shell text. A `$var`, command substitution or backslash
    # could point gh at different bytes from the file inspected by PreToolUse.
    file_literal = None
    for pattern in (
        r"^\s*(?:-F|--body-file)\s+'([^']*)'(?:\s|$)",
        r"^\s*--body-file='([^']*)'(?:\s|$)",
        r"^\s*(?:-F|--body-file)\s+([A-Za-z0-9_./+:-]+)(?:\s|$)",
        r"^\s*--body-file=([A-Za-z0-9_./+:-]+)(?:\s|$)",
    ):
        match = re.match(pattern, raw, flags=re.DOTALL)
        if match:
            if file_literal is not None:
                raise SystemExit(2)
            file_literal = match.group(1)
    if file_literal is None or file_literal != value:
        raise SystemExit(2)
print(json.dumps({"kind": kind, "value": value}))
PY
  )" || return 2
  kind="$(printf '%s' "$parsed" | jq -er '.kind | select(. == "inline" or . == "file")')" \
    || return 2
  value="$(printf '%s' "$parsed" | jq -er '.value | strings')" || return 2
  if [ "$kind" = inline ]; then
    printf '%s' "$value"
    return 0
  fi
  [ -n "$execution_cwd" ] && [ -d "$execution_cwd" ] || return 2
  case "$value" in
    /*) resolved="$value" ;;
    *) resolved="$execution_cwd/$value" ;;
  esac
  [ -n "$value" ] && [ "$value" != - ] && [ -f "$resolved" ] && [ ! -L "$resolved" ] || return 2
  cat -- "$resolved" 2>/dev/null || return 2
}

# gate_head_sha  -> exact local HEAD commit, empty if not a git repo.
gate_head_sha() { git -C "${GATE_PROJ:-$PWD}" rev-parse --verify 'HEAD^{commit}' 2>/dev/null; }

# gate_full_head_sha  -> exact local HEAD commit, empty if not a git repo.
gate_full_head_sha() {
  git -C "${GATE_PROJ:-$PWD}" rev-parse --verify 'HEAD^{commit}' 2>/dev/null
}

# gate_branch  -> current branch name, empty if detached/not a repo.
gate_branch() { git -C "${GATE_PROJ:-$PWD}" rev-parse --abbrev-ref HEAD 2>/dev/null; }

# gate_push_head_sha <arguments-after-git-push>
#   Print the full commit SHA only when this is a single-current-branch push whose source is HEAD.
#   The skill-audit checkbox is branch/commit scoped; accepting another source ref or destination
#   would verify local HEAD while publishing different bytes. Complex/quoted spellings fail closed
#   and can be retried as `git push origin <current-branch>`.
gate_push_head_sha() {
  local payload="$1" branch head token push_default remote ref source dest idx=0 after_opts=0
  local origin_fetch origin_push
  local -a words=() positional=() refs=()
  case "$payload" in
    __GATE_UNSAFE_GIT_GLOBAL_CONTEXT__|__GATE_UNSAFE_SHELL_CONTEXT__) return 2 ;;
  esac
  case "$payload" in *[\"\'\\]*) return 2 ;; esac
  read -r -a words <<< "$payload"
  branch="$(gate_branch)"
  [ -n "$branch" ] && [ "$branch" != HEAD ] || return 2
  head="$(git -C "${GATE_PROJ:-$PWD}" rev-parse --verify 'HEAD^{commit}' 2>/dev/null)"
  [ -n "$head" ] || return 2

  while [ "$idx" -lt "${#words[@]}" ]; do
    token="${words[$idx]}"
    idx=$((idx + 1))
    if [ "$after_opts" -eq 0 ]; then
      case "$token" in
        --) after_opts=1; continue ;;
        --all|--mirror|--tags|--delete|-d) return 2 ;;
        -u|--set-upstream|-f|--force|--force-if-includes|--atomic|--no-atomic|--no-verify|--porcelain|-q|--quiet|-v|--verbose|-n|--dry-run|--progress|--ipv4|--ipv6) continue ;;
        --force-with-lease|--force-with-lease=*|--signed|--signed=*|--no-signed) continue ;;
        -o|--push-option)
          [ "$idx" -lt "${#words[@]}" ] || return 2
          idx=$((idx + 1)); continue ;;
        --push-option=*) continue ;;
        -*) return 2 ;;
      esac
    fi
    positional+=("$token")
  done

  # First positional is the repository. Any remaining positional is a refspec.
  if ((${#positional[@]} > 0)); then
    remote="${positional[0]}"
  else
    remote="$(git -C "${GATE_PROJ:-$PWD}" config --get "branch.$branch.pushRemote" 2>/dev/null || true)"
    [ -n "$remote" ] || remote="$(git -C "${GATE_PROJ:-$PWD}" config --get remote.pushDefault 2>/dev/null || true)"
    [ -n "$remote" ] || remote="$(git -C "${GATE_PROJ:-$PWD}" config --get "branch.$branch.remote" 2>/dev/null || true)"
    [ -n "$remote" ] || remote=origin
  fi
  [ "$remote" = origin ] || return 2
  origin_fetch="$(git -C "${GATE_PROJ:-$PWD}" remote get-url --all origin 2>/dev/null)" || return 2
  origin_push="$(git -C "${GATE_PROJ:-$PWD}" remote get-url --push --all origin 2>/dev/null)" || return 2
  [ "$(printf '%s\n' "$origin_fetch" | awk 'NF { n++ } END { print n+0 }')" -eq 1 ] \
    && [ "$(printf '%s\n' "$origin_push" | awk 'NF { n++ } END { print n+0 }')" -eq 1 ] \
    && [ "$origin_fetch" = "$origin_push" ] || return 2
  [ "$(git -C "${GATE_PROJ:-$PWD}" config --bool --get push.followTags 2>/dev/null || true)" != true ] || return 2
  [ "$(git -C "${GATE_PROJ:-$PWD}" config --bool --get "remote.$remote.mirror" 2>/dev/null || true)" != true ] || return 2
  [ "$(git -C "${GATE_PROJ:-$PWD}" config --bool --get "remote.$remote.pushMirror" 2>/dev/null || true)" != true ] || return 2
  if ((${#positional[@]} > 1)); then
    refs=("${positional[@]:1}")
  fi
  ((${#refs[@]} <= 1)) || return 2

  if ((${#refs[@]} == 0)); then
    push_default="$(git -C "${GATE_PROJ:-$PWD}" config --get push.default 2>/dev/null || true)"
    case "${push_default:-simple}" in simple|current) ;; *) return 2 ;; esac
    [ -z "$(git -C "${GATE_PROJ:-$PWD}" config --get-all "remote.$remote.push" 2>/dev/null || true)" ] || return 2
  else
    ref="${refs[0]}"; ref="${ref#+}"
    [ -n "$ref" ] || return 2
    if [[ "$ref" == *:* ]]; then
      source="${ref%%:*}"; dest="${ref#*:}"
      [ -n "$source" ] && [ -n "$dest" ] || return 2
    else
      source="$ref"; dest="$branch"
    fi
    case "$source" in HEAD|"$branch"|"refs/heads/$branch") ;; *) return 2 ;; esac
    case "$dest" in "$branch"|"refs/heads/$branch") ;; *) return 2 ;; esac
  fi

  printf '%s\n' "$head"
}

# gate_pr_merge_selector <arguments-after-gh-pr-merge>
#   Print the exact PR number only when a number or local-repository GitHub pull URL is the FIRST
#   merge argument. A validated local URL is normalized to its number. Foreign URLs, implicit
#   current-branch targets, branch selectors, options before the selector and dynamic syntax are
#   deliberately unsupported: the hook must never reinterpret `--subject 123` as PR 123 or audit
#   local PR 7 while the command merges foreign PR 7.
gate_pr_merge_selector() {
  local parsed kind rest url_slug number local_slug
  command -v python3 >/dev/null 2>&1 || return 2
  parsed="$(python3 - "$1" <<'PY'
import re
import shlex
import sys

try:
    words = shlex.split(sys.argv[1], posix=True)
except ValueError:
    raise SystemExit(2)
if not words:
    raise SystemExit(2)
selector = words[0]
if any(token in {"--auto", "--disable-auto", "--admin"}
       or token.startswith(("--auto=", "--disable-auto=", "--admin="))
       for token in words[1:]):
    raise SystemExit(2)
if re.fullmatch(r"[0-9]+", selector):
    print("number\t" + selector)
    raise SystemExit(0)
match = re.fullmatch(r"https://github\.com/([^/]+)/([^/]+)/pull/([0-9]+)/?", selector)
if match:
    print("url\t" + match.group(1) + "/" + match.group(2) + "\t" + match.group(3))
    raise SystemExit(0)
raise SystemExit(2)
PY
  )" || return 2
  kind="${parsed%%$'\t'*}"
  rest="${parsed#*$'\t'}"
  case "$kind" in
    number)
      printf '%s\n' "$rest"
      ;;
    url)
      url_slug="${rest%%$'\t'*}"
      number="${rest#*$'\t'}"
      local_slug="$(gate_repo_slug)" || return 2
      [ "$url_slug" = "$local_slug" ] || return 2
      printf '%s\n' "$number"
      ;;
    *) return 2 ;;
  esac
}

# gate_pr_merge_match_sha <arguments-after-gh-pr-merge>
#   Require the race-closing head precondition as the first option pair after the selector. The
#   SHA is literal/full-length and occurs exactly once; option reordering, expansion and duplicate
#   flags are intentionally refused so gh cannot consume a body/subject operand differently.
gate_pr_merge_match_sha() {
  command -v python3 >/dev/null 2>&1 || return 2
  python3 - "$1" <<'PY'
import re
import shlex
import sys

try:
    words = shlex.split(sys.argv[1], posix=True)
except ValueError:
    raise SystemExit(2)
if len(words) < 3 or words[1] != "--match-head-commit":
    raise SystemExit(2)
sha = words[2]
if not re.fullmatch(r"[0-9a-f]{40}", sha):
    raise SystemExit(2)
if sum(1 for word in words[1:]
       if word == "--match-head-commit" or word.startswith("--match-head-commit=")) != 1:
    raise SystemExit(2)
if any(word in {"--auto", "--disable-auto", "--admin"}
       or word.startswith(("--auto=", "--disable-auto=", "--admin=")) for word in words[3:]):
    raise SystemExit(2)
print(sha)
PY
}

# gate_repo_slug  -> exact owner/repo from this project's origin. Empty if undeterminable.
gate_repo_slug() {
  local s count
  # Extract owner/repo from the origin URL. Handles SSH (git@host:owner/repo), HTTPS
  # (https://host/owner/repo) and web-runner proxy forms
  # (http://user@127.0.0.1:PORT/git/owner/repo) by strapping to the final two path segments.
  s="$(git -C "${GATE_PROJ:-$PWD}" remote get-url --all origin 2>/dev/null)" || return 2
  count="$(printf '%s\n' "$s" | awk 'NF { n++ } END { print n+0 }')"
  [ "$count" -eq 1 ] || return 2
  s="$(printf '%s' "$s" | sed -E 's#\.git$##; s#.*[/:]([^/:]+/[^/:]+)$#\1#')"
  printf '%s' "$s" | grep -Eq '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' || return 2
  printf '%s' "$s"
}

# gate_origin_is_github
#   MCP mutates github.com, not an arbitrary local/bare repository whose final path components
#   merely resemble owner/repo. Structured actions therefore require an explicit github.com
#   HTTPS or SSH origin; local paths and opaque web-proxy origins fail closed.
gate_origin_is_github() {
  local origin count
  origin="$(git -C "${GATE_PROJ:-$PWD}" remote get-url --all origin 2>/dev/null)" || return 2
  count="$(printf '%s\n' "$origin" | awk 'NF { n++ } END { print n+0 }')"
  [ "$count" -eq 1 ] || return 2
  printf '%s' "$origin" | grep -Eq \
    '^(https://github\.com/|git@github\.com:|ssh://git@github\.com/)[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(\.git)?$'
}

# gate_mcp_repo_matches <PreToolUse-json>
#   Structured GitHub tools must target the exact owner/repo configured by this worktree.
gate_mcp_repo_matches() {
  local input="$1" owner repo slug
  command -v jq >/dev/null 2>&1 || return 2
  gate_origin_is_github || return 2
  owner="$(printf '%s' "$input" | jq -er '.tool_input.owner | strings | select(length > 0)' 2>/dev/null)" \
    || return 2
  repo="$(printf '%s' "$input" | jq -er '.tool_input.repo | strings | select(length > 0)' 2>/dev/null)" \
    || return 2
  printf '%s' "$owner" | grep -Eq '^[A-Za-z0-9_.-]+$' || return 2
  printf '%s' "$repo" | grep -Eq '^[A-Za-z0-9_.-]+$' || return 2
  slug="$(gate_repo_slug)" || return 2
  [ "$owner/$repo" = "$slug" ]
}

# gate_mcp_create_head <PreToolUse-json>
#   Print local HEAD only when the structured create targets this exact repository/current branch
#   and that remote branch already points byte-for-byte at local HEAD. Missing network/ref state is
#   a hard block; otherwise a local checkbox could authorise a PR for unrelated remote bytes.
gate_mcp_create_head() {
  local input="$1" branch requested head ref rows remote
  gate_mcp_repo_matches "$input" || return 2
  branch="$(gate_branch)"; [ -n "$branch" ] && [ "$branch" != HEAD ] || return 2
  requested="$(printf '%s' "$input" \
    | jq -er '.tool_input.head | strings | select(length > 0)' 2>/dev/null)" || return 2
  [ "$requested" = "$branch" ] || return 2
  head="$(git -C "${GATE_PROJ:-$PWD}" rev-parse --verify 'HEAD^{commit}' 2>/dev/null)"
  printf '%s' "$head" | grep -Eq '^[0-9a-f]{40}$' || return 2
  ref="refs/heads/$branch"
  rows="$(git -C "${GATE_PROJ:-$PWD}" ls-remote --refs --exit-code origin "$ref" 2>/dev/null)" \
    || return 2
  remote="$(printf '%s\n' "$rows" \
    | awk -v ref="$ref" '$2 == ref && $1 ~ /^[0-9a-f]{40}$/ { print $1 }')"
  [ "$(printf '%s\n' "$remote" | awk 'NF { n++ } END { print n+0 }')" -eq 1 ] || return 2
  [ "$remote" = "$head" ] || return 2
  printf '%s\n' "$head"
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
  local sel="$1" json tok slug branch list count
  slug="$(gate_repo_slug)" || return 2
  [ -n "$slug" ] || return 2
  if command -v gh >/dev/null 2>&1; then
    if [ -n "$sel" ]; then
      printf '%s' "$sel" | grep -Eq '^[0-9]+$' || return 2
      json="$(agent_gate_run_bounded 30 env GH_HOST=github.com GH_REPO="github.com/$slug" gh pr view "$sel" --json body,headRefOid 2>/dev/null)" || return 2
    else
      branch="$(gate_branch)"; [ -n "$branch" ] && [ "$branch" != HEAD ] || return 2
      list="$(agent_gate_run_bounded 30 env GH_HOST=github.com GH_REPO="github.com/$slug" gh pr list --head "$branch" --state open --json number,body,headRefOid 2>/dev/null)" || return 2
      count="$(printf '%s' "$list" | python3 -c '
import json,sys
v=json.load(sys.stdin)
if not isinstance(v,list): raise SystemExit(1)
print(len(v))
' 2>/dev/null)" || return 2
      [ "$count" = 0 ] && return 1
      [ "$count" = 1 ] || return 2
      json="$(printf '%s' "$list" | python3 -c 'import json,sys; print(json.dumps(json.load(sys.stdin)[0]))')" || return 2
    fi
  else
    tok="${GH_TOKEN:-${GITHUB_TOKEN:-}}"
    [ -n "$tok" ] && command -v curl >/dev/null 2>&1 || return 2
    printf '%s' "$tok" | grep -Eq '^[A-Za-z0-9_.=-]+$' || return 2
    if [ -n "$sel" ]; then
      printf '%s' "$sel" | grep -Eq '^[0-9]+$' || return 2
      json="$(printf 'Authorization: Bearer %s\n' "$tok" | curl -fsSL --connect-timeout 5 --max-time 12 -H @- -H 'Accept: application/vnd.github+json' "https://api.github.com/repos/$slug/pulls/$sel" 2>/dev/null)" || return 2
    else
      branch="$(gate_branch)"; [ -n "$branch" ] && [ "$branch" != HEAD ] || return 2
      list="$(printf 'Authorization: Bearer %s\n' "$tok" | curl -fsSL --connect-timeout 5 --max-time 12 -H @- -H 'Accept: application/vnd.github+json' "https://api.github.com/repos/$slug/pulls?head=${slug%%/*}:$branch&state=open" 2>/dev/null)" || return 2
      count="$(printf '%s' "$list" | python3 -c '
import json,sys
v=json.load(sys.stdin)
if not isinstance(v,list): raise SystemExit(1)
print(len(v))
' 2>/dev/null)" || return 2
      [ "$count" = 0 ] && return 1
      [ "$count" = 1 ] || return 2
      json="$(printf '%s' "$list" | python3 -c 'import json,sys; print(json.dumps(json.load(sys.stdin)[0]))')" || return 2
    fi
  fi
  printf '%s' "$json" | python3 -c '
import json,re,sys
v=json.load(sys.stdin)
head=v.get("headRefOid") or (v.get("head") or {}).get("sha") or ""
body=v.get("body")
if not isinstance(head,str) or not re.fullmatch(r"[0-9a-fA-F]{40}",head): raise SystemExit(1)
if body is None: body=""
if not isinstance(body,str) or "\x00" in body: raise SystemExit(1)
print(head)
sys.stdout.write(body)
' || return 2
}

gate_extract_changed_pages() {
  local expected="$1"
  python3 -c '
import json,pathlib,sys
expected=int(sys.argv[1])
if expected < 0 or expected > 3000: raise SystemExit(1)
pages=json.load(sys.stdin)
if not isinstance(pages,list): raise SystemExit(1)
if pages and all(isinstance(x,dict) for x in pages): pages=[pages]
records=[]
for page in pages:
    if not isinstance(page,list) or len(page)>100: raise SystemExit(1)
    records.extend(page)
if len(records)!=expected: raise SystemExit(1)
paths=[]
for record in records:
    if not isinstance(record,dict): raise SystemExit(1)
    current=record.get("filename"); previous=record.get("previous_filename")
    if not isinstance(current,str) or not current or "\n" in current or "\x00" in current: raise SystemExit(1)
    values=[current]
    if previous is not None:
        if not isinstance(previous,str) or not previous or "\n" in previous or "\x00" in previous: raise SystemExit(1)
        values.append(previous)
    for value in values:
        path=pathlib.PurePosixPath(value)
        if path.is_absolute() or ".." in path.parts: raise SystemExit(1)
        if value not in paths: paths.append(value)
sys.stdout.write("\n".join(paths))
' "$expected"
}

# Print current and previous rename names. Count mismatch, malformed/truncated pages, and the
# 3000-record API ceiling fail closed. Authentication reaches curl only through stdin.
gate_pr_changed_files() {
  local sel="$1" slug meta number expected pages tok page page_json page_count retrieved separator deadline
  slug="$(gate_repo_slug)" || return 2
  [ -n "$slug" ] || return 2
  if command -v gh >/dev/null 2>&1; then
    if [ -n "$sel" ]; then
      printf '%s' "$sel" | grep -Eq '^[0-9]+$' || return 2
      meta="$(agent_gate_run_bounded 30 env GH_HOST=github.com GH_REPO="github.com/$slug" gh pr view "$sel" --json number,changedFiles 2>/dev/null)" || return 2
    else
      meta="$(agent_gate_run_bounded 30 env GH_HOST=github.com GH_REPO="github.com/$slug" gh pr view --json number,changedFiles 2>/dev/null)" || return 2
    fi
    read -r number expected <<EOF
$(printf '%s' "$meta" | python3 -c '
import json,sys
v=json.load(sys.stdin); n=v.get("number"); c=v.get("changedFiles")
if not isinstance(n,int) or isinstance(n,bool) or n<=0: raise SystemExit(1)
if not isinstance(c,int) or isinstance(c,bool) or c<0 or c>3000: raise SystemExit(1)
print(n,c)
' 2>/dev/null)
EOF
    [ -n "$number" ] && [ -n "$expected" ] || return 2
    pages="$(agent_gate_run_bounded 60 env GH_HOST=github.com GH_REPO="github.com/$slug" gh api --hostname github.com --paginate --slurp "repos/$slug/pulls/$number/files?per_page=100" 2>/dev/null)" || return 2
    printf '%s' "$pages" | gate_extract_changed_pages "$expected" || return 2
    return 0
  fi
  tok="${GH_TOKEN:-${GITHUB_TOKEN:-}}"
  [ -n "$tok" ] && command -v curl >/dev/null 2>&1 || return 2
  printf '%s' "$tok" | grep -Eq '^[A-Za-z0-9_.=-]+$' || return 2
  [ -n "$sel" ] && printf '%s' "$sel" | grep -Eq '^[0-9]+$' || return 2
  number="$sel"
  meta="$(printf 'Authorization: Bearer %s\n' "$tok" | curl -fsSL --connect-timeout 5 --max-time 12 -H @- -H 'Accept: application/vnd.github+json' "https://api.github.com/repos/$slug/pulls/$number" 2>/dev/null)" || return 2
  expected="$(printf '%s' "$meta" | python3 -c '
import json,sys
v=json.load(sys.stdin).get("changed_files")
if not isinstance(v,int) or isinstance(v,bool) or v<0 or v>3000: raise SystemExit(1)
print(v)
' 2>/dev/null)" || return 2
  deadline=$((SECONDS + 60))
  pages='['; separator=''; retrieved=0; page=1
  while :; do
    [ "$SECONDS" -lt "$deadline" ] || return 2
    page_json="$(printf 'Authorization: Bearer %s\n' "$tok" | curl -fsSL --connect-timeout 5 --max-time 12 -H @- -H 'Accept: application/vnd.github+json' "https://api.github.com/repos/$slug/pulls/$number/files?per_page=100&page=$page" 2>/dev/null)" || return 2
    page_count="$(printf '%s' "$page_json" | python3 -c '
import json,sys
v=json.load(sys.stdin)
if not isinstance(v,list) or len(v)>100: raise SystemExit(1)
print(len(v))
' 2>/dev/null)" || return 2
    pages="${pages}${separator}${page_json}"; separator=,
    retrieved=$((retrieved + page_count))
    [ "$retrieved" -le "$expected" ] || return 2
    [ "$retrieved" -eq "$expected" ] && break
    [ "$page_count" -eq 100 ] || return 2
    page=$((page + 1)); [ "$page" -le 30 ] || return 2
  done
  pages="${pages}]"
  printf '%s' "$pages" | gate_extract_changed_pages "$expected" || return 2
  return 0
}
