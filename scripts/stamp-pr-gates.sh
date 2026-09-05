#!/usr/bin/env bash
# Helper to audit, format, and stamp verified PR-gate checkboxes for the current HEAD.
#
# Usage:
#   scripts/stamp-pr-gates.sh [--format] [--head <40-hex-sha>]
#   scripts/stamp-pr-gates.sh --update-pr <pr-number>
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
BODY
)"

  local replaced
  replaced="$(python3 - "$sample_body" "$sha" 1 <<'PY'
import re, sys
body = sys.argv[1]
sha = sys.argv[2]
feat_relevant = sys.argv[3] == "1"

records = [
    f"- [x] `$skill-audit` clean — PR create/push gate @ {sha}",
    f"- [x] `$project-review` clean — merge gate @ {sha}",
    f"- [x] `$pr-hygiene` clean — content gate @ {sha}",
]
if feat_relevant:
    records.append(f"- [x] `$feature-docs` synced — merge gate @ {sha}")

# Replace template checkboxes if present
patterns = [
    r"[-*+]\s+\[[ xX]\]\s+`?\$skill-audit`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$project-review`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$pr-hygiene`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$feature-docs`?.*",
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

  # Verify parsed status using pr-gate-lib
  local status
  status="$(gate_checkbox_status "$replaced" "skill-audit")"
  [ "$status" = "checked $sha" ] || { echo "self-test failed on skill-audit: $status" >&2; exit 1; }
  status="$(gate_checkbox_status "$replaced" "project-review")"
  [ "$status" = "checked $sha" ] || { echo "self-test failed on project-review: $status" >&2; exit 1; }
  status="$(gate_checkbox_status "$replaced" "pr-hygiene")"
  [ "$status" = "checked $sha" ] || { echo "self-test failed on pr-hygiene: $status" >&2; exit 1; }
  status="$(gate_checkbox_status "$replaced" "feature-docs")"
  [ "$status" = "checked $sha" ] || { echo "self-test failed on feature-docs: $status" >&2; exit 1; }

  echo "stamp-pr-gates: self-test PASS"
}

target_head=""
update_pr=""
is_self_test=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --head) [ "$#" -ge 2 ] || exit 2; target_head="$2"; shift 2 ;;
    --update-pr|--pr) [ "$#" -ge 2 ] || exit 2; update_pr="$2"; shift 2 ;;
    --self-test) is_self_test=1; shift ;;
    --format) shift ;;
    *) echo "stamp-pr-gates: unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [ "$is_self_test" = 1 ]; then
  self_test
  exit 0
fi

if [ -z "$target_head" ]; then
  target_head="$(git -C "$root" rev-parse HEAD)"
fi

if ! printf '%s' "$target_head" | grep -Eq '^[0-9a-f]{40}$'; then
  echo "stamp-pr-gates: target head must be a 40-hex SHA" >&2
  exit 2
fi

# Detect feature-docs relevance
base_ref="origin/main"
if ! git -C "$root" rev-parse --verify "$base_ref" >/dev/null 2>&1; then
  base_ref="HEAD~1"
fi

changed_files="$(git -C "$root" diff --name-only "$base_ref"..."$target_head" 2>/dev/null || git -C "$root" diff --name-only HEAD~1 2>/dev/null || true)"
feat_relevant=0
if printf '%s\n' "$changed_files" | gate_feature_docs_relevant; then
  feat_relevant=1
fi

records=(
  "- [x] \`\$skill-audit\` clean — PR create/push gate @ $target_head"
  "- [x] \`\$project-review\` clean — merge gate @ $target_head"
  "- [x] \`\$pr-hygiene\` clean — content gate @ $target_head"
)
if [ "$feat_relevant" = 1 ]; then
  records+=("- [x] \`\$feature-docs\` synced — merge gate @ $target_head")
fi

if [ -n "$update_pr" ]; then
  command -v gh >/dev/null 2>&1 || { echo "stamp-pr-gates: gh CLI required for --update-pr" >&2; exit 2; }
  current_body="$(gh pr view "$update_pr" --repo 0Bu/tesla-key-esp32 --json body -q .body)"
  updated_body="$(python3 - "$current_body" "$target_head" "$feat_relevant" <<'PY'
import re, sys
body = sys.argv[1]
sha = sys.argv[2]
feat_relevant = sys.argv[3] == "1"

records = [
    f"- [x] `$skill-audit` clean — PR create/push gate @ {sha}",
    f"- [x] `$project-review` clean — merge gate @ {sha}",
    f"- [x] `$pr-hygiene` clean — content gate @ {sha}",
]
if feat_relevant:
    records.append(f"- [x] `$feature-docs` synced — merge gate @ {sha}")

patterns = [
    r"[-*+]\s+\[[ xX]\]\s+`?\$skill-audit`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$project-review`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$pr-hygiene`?.*",
    r"[-*+]\s+\[[ xX]\]\s+`?\$feature-docs`?.*",
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
  echo "stamp-pr-gates: updated PR #$update_pr with gates for $target_head"
else
  printf '%s\n' "${records[@]}"
fi
