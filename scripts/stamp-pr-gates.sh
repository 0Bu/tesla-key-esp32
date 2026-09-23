#!/usr/bin/env bash
# Helper to audit, format, and stamp verified PR-gate checkboxes for the current HEAD.
#
# Usage:
#   scripts/stamp-pr-gates.sh --gate <name>=<evidence> [--gate <name>=<evidence> ...] [--head <40-hex-sha>]
#   scripts/stamp-pr-gates.sh --update-pr <pr-number> --gate <name>=<evidence> ...
#   scripts/stamp-pr-gates.sh --self-test
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
lib="$root/tools/agent-hooks/pr-gate-lib.sh"
# shellcheck source=/dev/null
if [ ! -f "$lib" ] || ! . "$lib" 2>/dev/null; then
  echo "stamp-pr-gates: cannot load pr-gate-lib.sh" >&2
  exit 2
fi
cd "$root" || exit 2

self_test() {
  local sha="0123456789abcdef0123456789abcdef01234567"
  local sample_body
  sample_body="$(cat <<BODY
## Summary
Fix some logic.

<!-- Gates -->
- [ ] \`\$skill-audit\` clean — PR create/push gate @ <full-40-hex-sha>
- [ ] \`\$project-review\` clean — merge gate @ <full-40-hex-sha>
- [ ] \`\$pr-hygiene\` clean — content gate @ <full-40-hex-sha>
- [ ] \`\$feature-docs\` synced — merge gate @ <full-40-hex-sha>
- [ ] \`\$vehicle-command-audit\` clean — merge gate @ <full-40-hex-sha>
BODY
)"

  # 1. Canary: refusing to stamp without gate confirmation
  if "$root/scripts/stamp-pr-gates.sh" --head "$sha" >/dev/null 2>&1; then
    echo "self-test failed: stamp-pr-gates succeeded without any --gate confirmation" >&2
    exit 1
  fi

  # 2. Canary: selective gate confirmation
  local partial
  partial="$(python3 - "$sample_body" "$sha" "skill-audit,pr-hygiene" <<'PY'
import re, sys
body, sha, gates = sys.argv[1], sys.argv[2], set(sys.argv[3].split(","))

gate_specs = [
    ("skill-audit", f"- [x] `$skill-audit` clean — PR create/push gate @ {sha}", "- [ ] `$skill-audit` clean — PR create/push gate @ <full-40-hex-sha>"),
    ("project-review", f"- [x] `$project-review` clean — merge gate @ {sha}", "- [ ] `$project-review` clean — merge gate @ <full-40-hex-sha>"),
    ("pr-hygiene", f"- [x] `$pr-hygiene` clean — content gate @ {sha}", "- [ ] `$pr-hygiene` clean — content gate @ <full-40-hex-sha>"),
    ("feature-docs", f"- [x] `$feature-docs` synced — merge gate @ {sha}", "- [ ] `$feature-docs` synced — merge gate @ <full-40-hex-sha>"),
    ("vehicle-command-audit", f"- [x] `$vehicle-command-audit` clean — merge gate @ {sha}", "- [ ] `$vehicle-command-audit` clean — merge gate @ <full-40-hex-sha>"),
]
records = [chk if name in gates else unchk for name, chk, unchk in gate_specs]

patterns = [
    r"[-*+]\s+\[[ xX]\]\s+`?\$skill-audit`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$project-review`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$pr-hygiene`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$feature-docs`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$vehicle-command-audit`?.*",
]
lines = body.splitlines()
new_lines = []
stamped = False
for line in lines:
    if any(re.match(p, line.strip()) for p in patterns):
        if not stamped:
            new_lines.extend(records)
            stamped = True
    else:
        new_lines.append(line)
if not stamped:
    new_lines.extend(records)
print("\n".join(new_lines))
PY
)"

  local status
  status="$(gate_checkbox_status "$partial" "skill-audit")"
  [ "$status" = "checked $sha" ] || { echo "self-test failed on partial skill-audit: $status" >&2; exit 1; }
  status="$(gate_checkbox_status "$partial" "project-review")"
  [ "$status" = "absent" ] || { echo "self-test failed on unconfirmed project-review: $status" >&2; exit 1; }
  status="$(gate_checkbox_status "$partial" "pr-hygiene")"
  [ "$status" = "checked $sha" ] || { echo "self-test failed on partial pr-hygiene: $status" >&2; exit 1; }
  status="$(gate_checkbox_status "$partial" "feature-docs")"
  [ "$status" = "absent" ] || { echo "self-test failed on unconfirmed feature-docs: $status" >&2; exit 1; }
  status="$(gate_checkbox_status "$partial" "vehicle-command-audit")"
  [ "$status" = "absent" ] || { echo "self-test failed on unconfirmed vehicle-command-audit: $status" >&2; exit 1; }

  # 3. All gates confirmed
  local full_gates="skill-audit,project-review,pr-hygiene,feature-docs,vehicle-command-audit"
  local all_stamped
  all_stamped="$(python3 - "$sample_body" "$sha" "$full_gates" <<'PY'
import re, sys
body, sha, gates = sys.argv[1], sys.argv[2], set(sys.argv[3].split(","))

gate_specs = [
    ("skill-audit", f"- [x] `$skill-audit` clean — PR create/push gate @ {sha}", "- [ ] `$skill-audit` clean — PR create/push gate @ <full-40-hex-sha>"),
    ("project-review", f"- [x] `$project-review` clean — merge gate @ {sha}", "- [ ] `$project-review` clean — merge gate @ <full-40-hex-sha>"),
    ("pr-hygiene", f"- [x] `$pr-hygiene` clean — content gate @ {sha}", "- [ ] `$pr-hygiene` clean — content gate @ <full-40-hex-sha>"),
    ("feature-docs", f"- [x] `$feature-docs` synced — merge gate @ {sha}", "- [ ] `$feature-docs` synced — merge gate @ <full-40-hex-sha>"),
    ("vehicle-command-audit", f"- [x] `$vehicle-command-audit` clean — merge gate @ {sha}", "- [ ] `$vehicle-command-audit` clean — merge gate @ <full-40-hex-sha>"),
]
records = [chk if name in gates else unchk for name, chk, unchk in gate_specs]

patterns = [
    r"[-*+]\s+\[[ xX]\]\s+`?\$skill-audit`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$project-review`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$pr-hygiene`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$feature-docs`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$vehicle-command-audit`?.*",
]
lines = body.splitlines()
new_lines = []
stamped = False
for line in lines:
    if any(re.match(p, line.strip()) for p in patterns):
        if not stamped:
            new_lines.extend(records)
            stamped = True
    else:
        new_lines.append(line)
if not stamped:
    new_lines.extend(records)
print("\n".join(new_lines))
PY
)"

  status="$(gate_checkbox_status "$all_stamped" "skill-audit")"
  [ "$status" = "checked $sha" ] || { echo "self-test failed on skill-audit: $status" >&2; exit 1; }
  status="$(gate_checkbox_status "$all_stamped" "project-review")"
  [ "$status" = "checked $sha" ] || { echo "self-test failed on project-review: $status" >&2; exit 1; }
  status="$(gate_checkbox_status "$all_stamped" "pr-hygiene")"
  [ "$status" = "checked $sha" ] || { echo "self-test failed on pr-hygiene: $status" >&2; exit 1; }
  status="$(gate_checkbox_status "$all_stamped" "feature-docs")"
  [ "$status" = "checked $sha" ] || { echo "self-test failed on feature-docs: $status" >&2; exit 1; }
  status="$(gate_checkbox_status "$all_stamped" "vehicle-command-audit")"
  [ "$status" = "checked $sha" ] || { echo "self-test failed on vehicle-command-audit: $status" >&2; exit 1; }

  echo "stamp-pr-gates: self-test PASS"
}

target_head=""
update_pr=""
is_self_test=0
confirmed_gates=()

parse_gate_arg() {
  local entry="$1"
  local name="${entry%%=*}"
  local token="${entry#*=}"
  if [ "$name" = "$token" ]; then
    echo "stamp-pr-gates: --gate <name>=<evidence> requires non-empty evidence token or report path" >&2
    exit 2
  fi
  case "$name" in
    skill-audit|project-review|pr-hygiene|feature-docs|vehicle-command-audit) ;;
    *) echo "stamp-pr-gates: unknown gate name: $name" >&2; exit 2 ;;
  esac
  if [ -z "$token" ]; then
    echo "stamp-pr-gates: empty evidence token for gate: $name" >&2
    exit 2
  fi
  confirmed_gates+=("$name")
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --head) [ "$#" -ge 2 ] || exit 2; target_head="$2"; shift 2 ;;
    --update-pr|--pr) [ "$#" -ge 2 ] || exit 2; update_pr="$2"; shift 2 ;;
    --gate) [ "$#" -ge 2 ] || exit 2; parse_gate_arg "$2"; shift 2 ;;
    --self-test) is_self_test=1; shift ;;
    --format) shift ;;
    *) echo "stamp-pr-gates: unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [ "$is_self_test" = 1 ]; then
  self_test
  exit 0
fi

if [ "${#confirmed_gates[@]}" -eq 0 ]; then
  echo "stamp-pr-gates: refusing to stamp without verified gate confirmation (--gate <name>=<evidence>)" >&2
  exit 2
fi

if [ -z "$target_head" ]; then
  target_head="$(git -C "$root" rev-parse HEAD)"
fi

if ! printf '%s' "$target_head" | grep -Eq '^[0-9a-f]{40}$'; then
  echo "stamp-pr-gates: target head must be a 40-hex SHA" >&2
  exit 2
fi

# Build joined list of confirmed gates
joined_gates="$(IFS=,; echo "${confirmed_gates[*]}")"

records=()
for gate in "${confirmed_gates[@]}"; do
  case "$gate" in
    skill-audit) records+=("- [x] \`\$skill-audit\` clean — PR create/push gate @ $target_head") ;;
    project-review) records+=("- [x] \`\$project-review\` clean — merge gate @ $target_head") ;;
    pr-hygiene) records+=("- [x] \`\$pr-hygiene\` clean — content gate @ $target_head") ;;
    feature-docs) records+=("- [x] \`\$feature-docs\` synced — merge gate @ $target_head") ;;
    vehicle-command-audit) records+=("- [x] \`\$vehicle-command-audit\` clean — merge gate @ $target_head") ;;
  esac
done

if [ -n "$update_pr" ]; then
  command -v gh >/dev/null 2>&1 || { echo "stamp-pr-gates: gh CLI required for --update-pr" >&2; exit 2; }
  current_body="$(gh pr view "$update_pr" --repo 0Bu/tesla-key-esp32 --json body -q .body)"
  updated_body="$(python3 - "$current_body" "$target_head" "$joined_gates" <<'PY'
import re, sys
body = sys.argv[1]
sha = sys.argv[2]
confirmed = set(sys.argv[3].split(",")) if sys.argv[3] else set()

gate_specs = [
    ("skill-audit", f"- [x] `$skill-audit` clean — PR create/push gate @ {sha}", "- [ ] `$skill-audit` clean — PR create/push gate @ <full-40-hex-sha>"),
    ("project-review", f"- [x] `$project-review` clean — merge gate @ {sha}", "- [ ] `$project-review` clean — merge gate @ <full-40-hex-sha>"),
    ("pr-hygiene", f"- [x] `$pr-hygiene` clean — content gate @ {sha}", "- [ ] `$pr-hygiene` clean — content gate @ <full-40-hex-sha>"),
    ("feature-docs", f"- [x] `$feature-docs` synced — merge gate @ {sha}", "- [ ] `$feature-docs` synced — merge gate @ <full-40-hex-sha>"),
    ("vehicle-command-audit", f"- [x] `$vehicle-command-audit` clean — merge gate @ {sha}", "- [ ] `$vehicle-command-audit` clean — merge gate @ <full-40-hex-sha>"),
]

records = [chk if name in confirmed else unchk for name, chk, unchk in gate_specs]

patterns = [
    r"[-*+]\s+\[[ xX]\]\s+`?\$skill-audit`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$project-review`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$pr-hygiene`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$feature-docs`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$vehicle-command-audit`?.*",
]
lines = body.splitlines()
new_lines = []
stamped = False
for line in lines:
    if any(re.match(p, line.strip()) for p in patterns):
        if not stamped:
            new_lines.extend(records)
            stamped = True
    else:
        new_lines.append(line)

if not stamped:
    new_lines.append("\n<!-- Stamped PR Gates -->")
    new_lines.extend(records)

print("\n".join(new_lines))
PY
)"
  gh pr edit "$update_pr" --repo 0Bu/tesla-key-esp32 --body "$updated_body"
  echo "stamp-pr-gates: updated PR #$update_pr with confirmed gates for $target_head"
else
  printf '%s\n' "${records[@]}"
fi
