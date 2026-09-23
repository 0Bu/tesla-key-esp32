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

apply_gate_stamps() {
  local body="$1" sha="$2"
  shift 2
  python3 - "$body" "$sha" "$@" <<'PY'
import re, sys

body = sys.argv[1]
sha = sys.argv[2]
raw_gates = sys.argv[3:]
only_confirmed = not body

# Parse existing evidence comments associated with each gate
existing_evidence = {}
gate_re = re.compile(r"[-*+]\s+\[[ xX]\]\s+`?\$([a-z-]+)`?.*")
comment_re = re.compile(r"^\s*<!--\s*(?:evidence|gate):\s*(?P<ev>.*?)\s*-->")
current_gate = None
for line in body.splitlines():
    m = gate_re.match(line.strip())
    if m:
        current_gate = m.group(1)
        continue
    cm = comment_re.match(line.strip())
    if cm and current_gate:
        existing_evidence[current_gate] = cm.group("ev")
    elif line.strip() and not line.strip().startswith("<!--"):
        current_gate = None

evidence_map = {}
for item in raw_gates:
    if not item:
        continue
    if "=" in item:
        k, v = item.split("=", 1)
        if any(c in v for c in ("\r", "\n")) or "-->" in v:
            raise ValueError(f"evidence token contains forbidden characters: {v!r}")
        evidence_map[k] = v
    else:
        evidence_map[item] = existing_evidence.get(item, "passed")

gate_specs = [
    ("skill-audit", "clean", "PR create/push gate"),
    ("project-review", "clean", "merge gate"),
    ("pr-hygiene", "clean", "content gate"),
    ("feature-docs", "synced", "merge gate"),
    ("vehicle-command-audit", "clean", "merge gate"),
]

records = []
for name, status_word, role in gate_specs:
    if name in evidence_map:
        rec = f"- [x] `${name}` {status_word} — {role} @ {sha}"
        ev = evidence_map[name]
        if ev:
            rec += f"\n  <!-- evidence: {ev} -->"
        records.append(rec)
    elif not only_confirmed:
        records.append(f"- [ ] `${name}` {status_word} — {role} @ <full-40-hex-sha>")

if only_confirmed:
    print("\n".join(records))
    sys.exit(0)

patterns = [
    r"[-*+]\s+\[[ xX]\]\s+`?\$skill-audit`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$project-review`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$pr-hygiene`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$feature-docs`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$vehicle-command-audit`?.*",
]
evidence_re = re.compile(r"^\s*<!--\s*(?:evidence|gate):\s*.*-->")

lines = body.splitlines()
new_lines = []
stamped = False
for line in lines:
    if any(re.match(p, line.strip()) for p in patterns):
        if not stamped:
            new_lines.extend(records)
            stamped = True
    elif evidence_re.match(line.strip()):
        continue
    else:
        new_lines.append(line)

if not stamped:
    new_lines.append("\n<!-- Stamped PR Gates -->")
    new_lines.extend(records)

print("\n".join(new_lines))
PY
}

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

  # 2. Canary: selective gate confirmation with comma in token
  local partial
  partial="$(apply_gate_stamps "$sample_body" "$sha" "skill-audit=report.md, 0 findings @ $sha" "pr-hygiene=clean")"

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

  # Verify evidence comments exist with preserved commas and re-stamping does not duplicate them
  if ! echo "$partial" | grep -q "<!-- evidence: report.md, 0 findings @ $sha -->"; then
    echo "self-test failed: evidence comment with comma missing or corrupted in partial stamp" >&2
    exit 1
  fi
  local restamped
  restamped="$(apply_gate_stamps "$partial" "$sha" "skill-audit=report.md, 0 findings @ $sha" "pr-hygiene=clean")"
  local count
  count="$(echo "$restamped" | grep -c "<!-- evidence: " || true)"
  [ "$count" -eq 2 ] || { echo "self-test failed: re-stamping duplicated evidence comments (count=$count)" >&2; exit 1; }

  # 3. All gates confirmed
  local all_stamped
  all_stamped="$(apply_gate_stamps "$sample_body" "$sha" \
    "skill-audit=passed" \
    "project-review=passed" \
    "pr-hygiene=passed" \
    "feature-docs=synced" \
    "vehicle-command-audit=passed")"

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

  # 4. Canary: token injection attempts fail closed
  if "$root/scripts/stamp-pr-gates.sh" --head "$sha" --gate "pr-hygiene=bad"$'\n'"token" >/dev/null 2>&1; then
    echo "self-test failed: stamp-pr-gates accepted token with newline" >&2
    exit 1
  fi
  if "$root/scripts/stamp-pr-gates.sh" --head "$sha" --gate "pr-hygiene=bad-->token" >/dev/null 2>&1; then
    echo "self-test failed: stamp-pr-gates accepted token with -->" >&2
    exit 1
  fi

  echo "stamp-pr-gates: self-test PASS"
}

target_head=""
update_pr=""
is_self_test=0
confirmed_gates=()
gate_entries=()

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
  if [[ "$token" =~ [$'\r\n'] ]] || [[ "$token" == *"-->"* ]]; then
    echo "stamp-pr-gates: evidence token must not contain newlines or HTML comment delimiters (-->)" >&2
    exit 2
  fi
  confirmed_gates+=("$name")
  gate_entries+=("$name=$token")
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

if [ -n "$update_pr" ]; then
  command -v gh >/dev/null 2>&1 || { echo "stamp-pr-gates: gh CLI required for --update-pr" >&2; exit 2; }
  current_body="$(gh pr view "$update_pr" --repo 0Bu/tesla-key-esp32 --json body -q .body)"
  updated_body="$(apply_gate_stamps "$current_body" "$target_head" "${gate_entries[@]}")"
  gh pr edit "$update_pr" --repo 0Bu/tesla-key-esp32 --body "$updated_body"
  echo "stamp-pr-gates: updated PR #$update_pr with confirmed gates for $target_head"
else
  apply_gate_stamps "" "$target_head" "${gate_entries[@]}"
fi
