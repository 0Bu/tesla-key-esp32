#!/usr/bin/env bash
# Aggregate, runner-neutral Tesla PR policy.
#
# Actions:
#   * gh pr create / git push to an existing PR -> current $skill-audit record
#   * the one canonical gh pr merge command      -> current $project-review record and conditional
#                                                    $feature-docs record
# MCP merges, auto-merge/queue activation, direct REST/GraphQL merges, ambiguous payloads, and
# unverifiable network state fail closed. This lexical hook is defense in depth; branch protection
# and human review remain authoritative.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
canonical_root="$(cd "$here/../.." && pwd)"
# shellcheck source=/dev/null
if ! . "$here/pr-gate-lib.sh" 2>/dev/null; then
  echo "BLOCKED: runner-neutral PR gate library could not be loaded." >&2
  exit 2
fi
for fn in gate_bash_actions gate_pr_create_body gate_push_head_sha gate_fetch_pr \
          gate_pr_changed_files gate_checkbox_status gate_sha_matches gate_full_head_sha \
          gate_branch gate_repo_slug gate_origin_is_github agent_gate_workdir_matches \
          agent_gate_run_bounded; do
  if [ "${GATE_PR_LIB_API:-}" != 2 ] || ! declare -F "$fn" >/dev/null 2>&1; then
    echo "BLOCKED: runner-neutral PR gate library is incomplete ($fn)." >&2
    exit 2
  fi
done

body_file="${AGENT_PR_BODY_FILE:-}"
head_sha="${AGENT_PR_HEAD_SHA:-}"
files_file="${AGENT_CHANGED_FILES_FILE:-}"
selector="${AGENT_PR_SELECTOR:-}"
requested_root="${AGENT_PROJECT_DIR:-${PROJECT_DIR:-$canonical_root}}"
payload_file=""
force_check="${AGENT_POLICY_CI:-0}"
allow_discovery=1
[ "${AGENT_POLICY_CI:-0}" = 1 ] && allow_discovery=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --body-file) [ "$#" -ge 2 ] || exit 2; body_file="$2"; shift 2 ;;
    --head-sha) [ "$#" -ge 2 ] || exit 2; head_sha="$2"; shift 2 ;;
    --changed-files-file) [ "$#" -ge 2 ] || exit 2; files_file="$2"; shift 2 ;;
    --pr) [ "$#" -ge 2 ] || exit 2; selector="$2"; force_check=1; shift 2 ;;
    --project-dir) [ "$#" -ge 2 ] || exit 2; requested_root="$2"; shift 2 ;;
    --payload-file) [ "$#" -ge 2 ] || exit 2; payload_file="$2"; shift 2 ;;
    --check) force_check=1; shift ;;
    --no-discovery) allow_discovery=0; shift ;;
    *) echo "BLOCKED: unknown PR-gate argument: $1" >&2; exit 2 ;;
  esac
done

root="$(git -C "$canonical_root" rev-parse --show-toplevel 2>/dev/null)" || root="$canonical_root"
if ! agent_gate_workdir_matches "$root" "$requested_root"; then
  echo "BLOCKED: requested PR-gate project is not this tesla-key-esp32 worktree." >&2
  exit 2
fi
GATE_PROJ="$root"
cd "$root" || exit 2

payload=""
payload_expected=0
if [ -n "$payload_file" ]; then
  payload_expected=1
  [ -f "$payload_file" ] && [ -r "$payload_file" ] && [ ! -L "$payload_file" ] || {
    echo "BLOCKED: hook payload file is missing, unreadable, or a symlink." >&2; exit 2;
  }
  payload="$(cat "$payload_file")" || exit 2
elif [ "$force_check" != 1 ] && [ -z "$body_file$head_sha$files_file" ]; then
  payload_expected=1
  payload="$(cat 2>/dev/null)" || exit 2
fi
[ "$payload_expected" = 0 ] || [ -n "$payload" ] || {
  echo "BLOCKED: empty hook payload cannot prove that no protected PR action is present." >&2
  exit 2
}

tmp="$(mktemp -d)" || exit 2
trap 'rm -rf "$tmp"' EXIT

parse_basic_payload() {
  python3 -c '
import json, os, sys
try:
    value=json.load(sys.stdin)
except Exception:
    raise SystemExit(2)
if not isinstance(value,dict):
    raise SystemExit(2)
tool=value.get("tool_name")
tool_input=value.get("tool_input")
if not isinstance(tool,str) or not tool.strip() or not isinstance(tool_input,dict):
    raise SystemExit(2)
commands=[tool_input.get(k) for k in ("command","cmd") if tool_input.get(k) not in (None,"")]
if any(not isinstance(x,str) for x in commands) or len(dict.fromkeys(commands))>1:
    error="conflicting or non-string command/cmd fields"
    command=""
else:
    error=""
    command=commands[0] if commands else ""
cwd=value.get("cwd")
if not isinstance(cwd,str) or not cwd:
    cwd=""
workdirs=[tool_input.get(k) for k in ("workdir","cwd") if tool_input.get(k) not in (None,"")]
if any(not isinstance(x,str) for x in workdirs) or len(dict.fromkeys(workdirs))>1:
    error=error or "conflicting or non-string workdir/cwd fields"
else:
    workdir=workdirs[0] if workdirs else ""
    if workdir:
        cwd=os.path.abspath(os.path.join(cwd or os.getcwd(),workdir))
for field in (tool,command,cwd,error):
    if "\x00" in field:
        raise SystemExit(2)
    sys.stdout.write(field+"\x00")
'
}

kind=""
action=""
expected_head=""
payload_cwd=""
if [ -n "$payload" ]; then
  if ! printf '%s' "$payload" | python3 "$here/merge_payload.py" >"$tmp/merge.fields"; then
    echo "BLOCKED: malformed hook JSON or payload schema." >&2
    exit 2
  fi
  fields=(); i=0
  while IFS= read -r -d '' field; do fields[$i]="$field"; i=$((i + 1)); done <"$tmp/merge.fields"
  merge_action="${fields[0]:-}"
  merge_selector="${fields[1]:-}"
  merge_cwd="${fields[2]:-}"
  merge_repo="${fields[3]:-}"
  merge_host="${fields[4]:-}"
  merge_error="${fields[5]:-}"
  merge_expected="${fields[6]:-}"

  if [ -n "$merge_action" ]; then
    [ -z "$merge_error" ] || { echo "BLOCKED: $merge_error" >&2; exit 2; }
    printf '%s' "$merge_selector" | grep -Eq '^[0-9]+$' || {
      echo "BLOCKED: merge must name one numeric PR." >&2; exit 2;
    }
    printf '%s' "$merge_expected" | grep -Eq '^[0-9a-f]{40}$' || {
      echo "BLOCKED: merge must bind one full lowercase 40-hex head SHA." >&2; exit 2;
    }
    [ -n "$merge_cwd" ] && agent_gate_workdir_matches "$root" "$merge_cwd" || {
      echo "BLOCKED: merge execution cwd is outside this worktree." >&2; exit 2;
    }
    [ "$merge_host" = github.com ] && [ "$merge_repo" = "0Bu/tesla-key-esp32" ] || {
      echo "BLOCKED: merge target must be exactly github.com/0Bu/tesla-key-esp32." >&2; exit 2;
    }
    kind=merge
    action="$merge_action"
    selector="$merge_selector"
    expected_head="$merge_expected"
  else
    if ! printf '%s' "$payload" | parse_basic_payload >"$tmp/basic.fields"; then
      echo "BLOCKED: malformed hook JSON or payload schema." >&2
      exit 2
    fi
    basic=(); i=0
    while IFS= read -r -d '' field; do basic[$i]="$field"; i=$((i + 1)); done <"$tmp/basic.fields"
    tool="${basic[0]:-}"; cmd="${basic[1]:-}"; payload_cwd="${basic[2]:-}"; basic_error="${basic[3]:-}"
    [ -z "$basic_error" ] || { echo "BLOCKED: $basic_error" >&2; exit 2; }
    tool_lower="$(printf '%s' "$tool" | tr '[:upper:]' '[:lower:]')"
    case "$tool_lower" in
      bash|exec_command|shell|shell_command)
        [ -n "$cmd" ] || { echo "BLOCKED: shell hook payload has no command." >&2; exit 2; }
        records="$(gate_bash_actions "$cmd")"
        # A merge-looking action not classified by the stricter parser is a parser disagreement.
        if printf '%s\n' "$records" | grep -Eq '^merge[[:space:]]'; then
          echo "BLOCKED: merge-like shell action is not the exact canonical merge command." >&2
          exit 2
        fi
        publish="$(printf '%s\n' "$records" | grep -E '^(create|push)[[:space:]]' || true)"
        count="$(printf '%s\n' "$publish" | awk 'NF{n++} END{print n+0}')"
        [ "$count" -le 1 ] || { echo "BLOCKED: multiple PR publish actions are not atomic." >&2; exit 2; }
        if [ "$count" = 1 ]; then
          kind="${publish%%$'\t'*}"
          spec="${publish#*$'\t'}"
          case "$spec" in
            __GATE_UNSAFE_*) echo "BLOCKED: PR create/push must be a standalone, statically bound command." >&2; exit 2 ;;
          esac
          action="$kind"
        fi
        ;;
      mcp__*)
        if printf '%s' "$tool_lower" | grep -Eq '(create_pull_request|push_files|merge|enqueue|queue|auto_merge)'; then
          echo "BLOCKED: MCP PR publication/merge cannot bind the local audited bytes; use the guarded CLI path." >&2
          exit 2
        fi
        ;;
      *)
        if printf '%s' "$cmd" | grep -Eq '(^|[;&|[:space:]])(git[[:space:]]+push|gh[[:space:]]+pr[[:space:]]+(create|merge))'; then
          echo "BLOCKED: unsupported or suffix-manipulated tool name carries a protected PR action." >&2
          exit 2
        fi
        ;;
    esac
    if [ -n "$kind" ]; then
      [ -n "$payload_cwd" ] && agent_gate_workdir_matches "$root" "$payload_cwd" || {
        echo "BLOCKED: PR publish execution cwd is missing or outside this worktree." >&2; exit 2;
      }
    fi
  fi
fi

if [ -n "$body_file$head_sha$files_file" ]; then
  force_check=1
  [ -n "$body_file" ] && [ -f "$body_file" ] && [ -r "$body_file" ] && [ ! -L "$body_file" ] || {
    echo "BLOCKED: AGENT_PR_BODY_FILE is missing, unreadable, or a symlink." >&2; exit 2;
  }
  [ -n "$files_file" ] && [ -f "$files_file" ] && [ -r "$files_file" ] && [ ! -L "$files_file" ] || {
    echo "BLOCKED: AGENT_CHANGED_FILES_FILE is missing, unreadable, or a symlink." >&2; exit 2;
  }
  printf '%s' "$head_sha" | grep -Eq '^[0-9a-fA-F]{40}$' || {
    echo "BLOCKED: AGENT_PR_HEAD_SHA must be one full 40-hex SHA." >&2; exit 2;
  }
  kind=check
fi

if [ "$force_check" = 1 ] && [ -z "$kind" ]; then
  kind=check
fi
[ -n "$kind" ] || exit 0

if ! gate_origin_is_github || [ "$(gate_repo_slug)" != "0Bu/tesla-key-esp32" ]; then
  echo "BLOCKED: protected PR actions require the exact github.com/0Bu/tesla-key-esp32 origin." >&2
  exit 2
fi

# gh also accepts inherited repository/host overrides.  The lexical command parser cannot see
# those bytes, so bind them here before allowing the shell to execute the protected action.
case "${GH_HOST:-}" in
  ""|github.com) ;;
  *) echo "BLOCKED: inherited GH_HOST targets a foreign GitHub host." >&2; exit 2 ;;
esac
case "${GH_REPO:-}" in
  ""|0Bu/tesla-key-esp32|github.com/0Bu/tesla-key-esp32) ;;
  *) echo "BLOCKED: inherited GH_REPO targets a foreign repository." >&2; exit 2 ;;
esac

record_ok() {
  local content="$1" key="$2" anchor="$3" status state stamp
  status="$(gate_checkbox_status "$content" "$key")" || return 1
  state="${status%% *}"
  stamp=""
  [ "$state" = checked ] && stamp="${status#checked }"
  [ "$state" = checked ] && [ -n "$stamp" ] && gate_sha_matches "$stamp" "$anchor"
}

case "$kind" in
  create)
    body="$(gate_pr_create_body "$spec" "$payload_cwd")" || {
      echo "BLOCKED: gh pr create needs one literal first --body/--body-file source." >&2; exit 2;
    }
    anchor="$(gate_full_head_sha)"
    printf '%s' "$anchor" | grep -Eq '^[0-9a-f]{40}$' || exit 2
    record_ok "$body" skill-audit "$anchor" || {
      echo "BLOCKED: gh pr create requires one current top-level \$skill-audit record for $anchor." >&2
      exit 2
    }
    ;;
  push)
    anchor="$(gate_push_head_sha "$spec")" || {
      echo "BLOCKED: git push must publish only current HEAD to the current branch on origin." >&2
      exit 2
    }
    pr="$(gate_fetch_pr "")"; rc=$?
    case "$rc" in
      0) ;;
      1) exit 0 ;;
      *) echo "BLOCKED: existing-PR state is unreadable; skill-audit gate fails closed." >&2; exit 2 ;;
    esac
    pr_head="$(printf '%s' "$pr" | head -n 1)"
    body="$(printf '%s' "$pr" | tail -n +2)"
    printf '%s' "$pr_head" | grep -Eq '^[0-9a-fA-F]{40}$' || exit 2
    record_ok "$body" skill-audit "$anchor" || {
      echo "BLOCKED: git push to an open PR requires one current top-level \$skill-audit record for $anchor." >&2
      exit 2
    }
    ;;
  merge|check)
    if [ -z "$body_file$head_sha$files_file" ]; then
      [ "$allow_discovery" = 1 ] || {
        echo "BLOCKED: PR inputs are absent and network discovery is disabled." >&2; exit 2;
      }
      pr="$(gate_fetch_pr "$selector")"; rc=$?
      [ "$rc" = 0 ] || { echo "BLOCKED: PR body/head discovery failed closed." >&2; exit 2; }
      head_sha="$(printf '%s' "$pr" | head -n 1)"
      body="$(printf '%s' "$pr" | tail -n +2)"
      changed="$(gate_pr_changed_files "$selector")" || {
        echo "BLOCKED: paginated changed-file discovery failed or was truncated." >&2; exit 2;
      }
      body_file="$tmp/body.md"; files_file="$tmp/files.txt"
      printf '%s' "$body" >"$body_file"
      printf '%s' "$changed" >"$files_file"
    fi
    printf '%s' "$head_sha" | grep -Eq '^[0-9a-fA-F]{40}$' || {
      echo "BLOCKED: discovered PR head is not a full commit SHA." >&2; exit 2;
    }
    if [ "$kind" = merge ]; then
      local_head="$(gate_full_head_sha)"
      [ "$local_head" = "$head_sha" ] && [ "$expected_head" = "$(printf '%s' "$head_sha" | tr 'A-F' 'a-f')" ] || {
        echo "BLOCKED: canonical merge expected-head, PR head, and local HEAD differ." >&2; exit 2;
      }
    fi
    if ! python3 - "$files_file" <<'PY'
import pathlib, sys
lines=pathlib.Path(sys.argv[1]).read_text().splitlines()
if len(lines)>6000:
    raise SystemExit(1)
for value in lines:
    path=pathlib.PurePosixPath(value)
    if not value or "\x00" in value or path.is_absolute() or ".." in path.parts:
        raise SystemExit(1)
PY
    then
      echo "BLOCKED: changed-files input is malformed or exceeds the rename-expanded limit." >&2
      exit 2
    fi
    body="$(cat "$body_file")" || exit 2
    record_ok "$body" project-review "$head_sha" || {
      echo "BLOCKED: merge/check requires one current top-level \$project-review record for $head_sha." >&2
      exit 2
    }
    if [ "$kind" = check ]; then
      record_ok "$body" skill-audit "$head_sha" || {
        echo "BLOCKED: aggregate check requires one current top-level \$skill-audit record for $head_sha." >&2
        exit 2
      }
    fi
    if printf '%s\n' "$(cat "$files_file")" | gate_feature_docs_relevant; then
      record_ok "$body" feature-docs "$head_sha" || {
        echo "BLOCKED: feature-relevant merge/check requires one current top-level \$feature-docs record for $head_sha." >&2
        exit 2
      }
    fi
    ;;
  *) echo "BLOCKED: internal PR-gate action classification failed." >&2; exit 2 ;;
esac

echo "agent PR gates: $kind policy satisfied"
exit 0
