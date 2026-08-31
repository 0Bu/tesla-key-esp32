#!/usr/bin/env bash
# Mutation canaries for the canonical project-agent configuration and hook core.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
passes=0

fail() { echo "agent-config selftest: $1" >&2; exit 1; }

make_fixture() {
  local dest="$1"
  rm -rf "$dest"
  mkdir -p "$dest/.codex" "$dest/.agents" "$dest/.claude" "$dest/.github" "$dest/docs" "$dest/tools"
  cp "$ROOT/.mcp.json" "$dest/.mcp.json"
  cp "$ROOT/AGENTS.md" "$dest/AGENTS.md"
  cp "$ROOT/.github/PULL_REQUEST_TEMPLATE.md" "$dest/.github/PULL_REQUEST_TEMPLATE.md"
  cp -R "$ROOT/.codex/agents" "$dest/.codex/agents"
  cp "$ROOT/.codex/config.toml" "$dest/.codex/config.toml"
  cp "$ROOT/.codex/hooks.json" "$dest/.codex/hooks.json"
  cp -R "$ROOT/.agents/skills" "$dest/.agents/skills"
  cp "$ROOT/.claude/CLAUDE.md" "$dest/.claude/CLAUDE.md"
  cp "$ROOT/.claude/settings.json" "$dest/.claude/settings.json"
  cp -R "$ROOT/.claude/agents" "$dest/.claude/agents"
  cp -R "$ROOT/.claude/skills" "$dest/.claude/skills"
  cp "$ROOT/docs/FEATURES.md" "$dest/docs/FEATURES.md"
  cp -R "$ROOT/tools/agent-config" "$dest/tools/agent-config"
  cp -R "$ROOT/tools/agent-hooks" "$dest/tools/agent-hooks"
}

run_gate() {
  local fixture="$1"
  AGENT_CONFIG_ROOT="$fixture" node "$ROOT/tools/agent-config/check.mjs" || return
  AGENT_CONFIG_ROOT="$fixture" python3 "$ROOT/tools/agent-config/check_toml.py" || return
  AGENT_CONFIG_ROOT="$fixture" python3 "$ROOT/tools/agent-config/check_hooks.py" || return
}

expect_failure() {
  local name="$1" fixture="$2" needle="$3" output rc
  set +e
  output="$(run_gate "$fixture" 2>&1)"; rc=$?
  set -e
  [ "$rc" -ne 0 ] || fail "$name: mutated fixture passed"
  printf '%s' "$output" | grep -qF "$needle" \
    || fail "$name: failure did not mention '$needle' (output: $output)"
  echo "  PASS  $name"
  passes=$((passes + 1))
}

echo "== clean contract, retired migration state and runner adapters =="
fixture="$WORK/clean"; make_fixture "$fixture"
run_gate "$fixture" >/dev/null || fail "clean fixture failed"
echo "  PASS  clean fixture"
passes=$((passes + 1))

fixture="$WORK/adapter-missing"; make_fixture "$fixture"
rm -rf "$fixture/.claude"
expect_failure "missing Claude adapter" "$fixture" "Claude memory adapter is missing"

fixture="$WORK/adapter-import"; make_fixture "$fixture"
perl -0pi -e 's/\@\.\.\/AGENTS\.md/AGENTS.md/' "$fixture/.claude/CLAUDE.md"
expect_failure "adapter drops the AGENTS.md import" "$fixture" "must import @../AGENTS.md"

fixture="$WORK/adapter-boundary"; make_fixture "$fixture"
perl -0pi -e 's/are \*\*read-only\*\*\./may edit./' "$fixture/.claude/CLAUDE.md"
expect_failure "adapter drops the read-only boundary" "$fixture" \
  "must restate the read-only boundary and defer to AGENTS.md"

fixture="$WORK/adapter-policy-bloat"; make_fixture "$fixture"
head -c 5000 /dev/zero | tr '\0' 'x' >> "$fixture/.claude/CLAUDE.md"
expect_failure "adapter restates policy" "$fixture" "over the 4096-byte adapter budget"

fixture="$WORK/adapter-reviewer-drift"; make_fixture "$fixture"
perl -0pi -e 's/Never edit files/May edit files/' \
  "$fixture/.claude/agents/doc-drift-checker.md"
expect_failure "Claude reviewer instruction drift" "$fixture" \
  "instructions drifted from .codex/agents"

fixture="$WORK/adapter-reviewer-tools"; make_fixture "$fixture"
perl -0pi -e 's/^tools: Read, Grep, Glob, Bash$/tools: Read, Grep, Glob, Bash, Edit, Write/m' \
  "$fixture/.claude/agents/heap-safety-reviewer.md"
expect_failure "writable Claude reviewer" "$fixture" "a read-only reviewer gets no file-mutation tool"

fixture="$WORK/adapter-skill-missing"; make_fixture "$fixture"
rm -rf "$fixture/.claude/skills/ship"
expect_failure "unmirrored canonical skill" "$fixture" \
  "must cover exactly the canonical .agents/skills set"

fixture="$WORK/adapter-skill-description"; make_fixture "$fixture"
perl -0pi -e 's/^description: .+$/description: Canary./m' \
  "$fixture/.claude/skills/flash-esp32/SKILL.md"
expect_failure "Claude skill description drift" "$fixture" "description drifted from .agents/skills"

fixture="$WORK/adapter-skill-widening"; make_fixture "$fixture"
perl -0pi -e 's/This adapter grants nothing\./This adapter may widen the canonical boundary./' \
  "$fixture/.claude/skills/usb-recovery/SKILL.md"
expect_failure "Claude skill adapter widening" "$fixture" \
  "must delegate to the canonical skill without widening it"

fixture="$WORK/migration-residue"; make_fixture "$fixture"
printf '{}\n' > "$fixture/.codex/migration-manifest.json"
expect_failure "retired migration manifest" "$fixture" "migration-manifest.json must remain retired"

fixture="$WORK/budget"; make_fixture "$fixture"
set +e
output="$(AGENT_CONFIG_ROOT="$fixture" AGENT_INSTRUCTIONS_BUDGET_BYTES=1 \
  node "$ROOT/tools/agent-config/check.mjs" 2>&1)"; rc=$?
set -e
[ "$rc" -ne 0 ] && printf '%s' "$output" | grep -qF "over the 1-byte budget" \
  || fail "AGENTS budget mutation was not caught"
echo "  PASS  AGENTS budget"
passes=$((passes + 1))

echo "== parsed TOML, skills and policy =="
fixture="$WORK/config-toml"; make_fixture "$fixture"
printf '\n[broken\n' >> "$fixture/.codex/config.toml"
expect_failure "invalid config TOML" "$fixture" "not valid TOML"

fixture="$WORK/context-pin"; make_fixture "$fixture"
perl -0pi -e 's/context7-mcp\@4\.0\.2/context7-mcp\@latest/' "$fixture/.codex/config.toml"
expect_failure "Context7 pin drift" "$fixture" "Context7 must stay exactly pinned"

fixture="$WORK/hooks-disabled"; make_fixture "$fixture"
perl -0pi -e 's/hooks = true/hooks = false/' "$fixture/.codex/config.toml"
expect_failure "disabled hooks" "$fixture" "explicitly enable multi_agent and hooks"

fixture="$WORK/reviewer-model"; make_fixture "$fixture"
printf '\nmodel = "canary"\n' >> "$fixture/.codex/agents/agent_config_reviewer.toml"
expect_failure "reviewer model pin" "$fixture" "must not pin a model"

fixture="$WORK/reviewer-write"; make_fixture "$fixture"
perl -0pi -e 's/sandbox_mode = "read-only"/sandbox_mode = "workspace-write"/' \
  "$fixture/.codex/agents/doc_drift_checker.toml"
expect_failure "writable reviewer" "$fixture" "sandbox_mode must be read-only"

fixture="$WORK/skill-name"; make_fixture "$fixture"
perl -0pi -e 's/^name: add-logic-test$/name: wrong-canary/m' \
  "$fixture/.agents/skills/add-logic-test/SKILL.md"
expect_failure "skill name mismatch" "$fixture" "frontmatter name mismatch"

fixture="$WORK/skill-frontmatter"; make_fixture "$fixture"
perl -0pi -e 's/^description:/model: canary\ndescription:/m' \
  "$fixture/.agents/skills/add-logic-test/SKILL.md"
expect_failure "skill frontmatter" "$fixture" "frontmatter keys must be exactly"

fixture="$WORK/unreviewed-skill"; make_fixture "$fixture"
mkdir "$fixture/.agents/skills/unreviewed"
printf '%s\n' '---' 'name: unreviewed' 'description: Canary.' '---' '$unreviewed' \
  > "$fixture/.agents/skills/unreviewed/SKILL.md"
expect_failure "unreviewed project skill" "$fixture" "has no reviewed content digest"

fixture="$WORK/pr-hygiene-ranges"; make_fixture "$fixture"
perl -0pi -e 's/192\.0\.2\.0\/24/192.168.0.0\/16/' \
  "$fixture/.agents/skills/pr-hygiene/SKILL.md"
expect_failure "pr-hygiene documentation ranges" "$fixture" "privacy/language gate contract"

fixture="$WORK/ship-hygiene-gate"; make_fixture "$fixture"
perl -0pi -e 's/\$pr-hygiene clean — content gate/\$project-review clean — content gate/' \
  "$fixture/.agents/skills/ship/SKILL.md"
expect_failure "ship pr-hygiene merge gate" "$fixture" "ship is missing a merge-gate contract"

fixture="$WORK/feature-catalog-hygiene"; make_fixture "$fixture"
perl -0pi -e 's/publishing personal\/private identifiers or non-English PR\/docs content/publishing unchecked content/' \
  "$fixture/docs/FEATURES.md"
expect_failure "feature catalog pr-hygiene gate" "$fixture" "docs/FEATURES.md is missing a PR-policy contract"

fixture="$WORK/usb-live-boundary"; make_fixture "$fixture"
perl -0pi -e 's/USB-write approval does not authorize live verification/USB write also authorizes live verification/g' \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb recovery live boundary" "$fixture" "exact live-verification contract"

fixture="$WORK/usb-operational-stop"; make_fixture "$fixture"
perl -0pi -e 's/If that approval is absent, stop after/If that approval is absent, continue after/' \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb recovery operational stop" "$fixture" "contradicts the live-verification contract"

fixture="$WORK/usb-live-order"; make_fixture "$fixture"
perl -0pi -e 's/Before any HTTP request/After any HTTP request/g' \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb recovery approval order" "$fixture" "exact live-verification contract"

fixture="$WORK/usb-named-endpoints"; make_fixture "$fixture"
perl -0pi -e 's/the named GET endpoints/unnamed GET endpoints/g' \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb recovery named endpoints" "$fixture" "exact live-verification contract"

fixture="$WORK/usb-ota-state"; make_fixture "$fixture"
perl -0pi -e 's/`GET \/ota\/check` is state-changing/`GET \/ota\/check` is not state-changing/g' \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb recovery OTA state" "$fixture" "exact live-verification contract"

fixture="$WORK/usb-positive-authorization"; make_fixture "$fixture"
printf '\nThe USB-write approval also authorizes live verification of any GET endpoint.\n' >> \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb recovery positive authorization contradiction" "$fixture" \
  "contradicts the live-verification contract"

fixture="$WORK/usb-no-approval"; make_fixture "$fixture"
printf '\nLive verification needs no separate approval.\n' >> \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb recovery no-approval contradiction" "$fixture" "exact reviewed content contract drifted"

fixture="$WORK/project-review-owner"; make_fixture "$fixture"
perl -0pi -e 's/\[`docs\/README\.md`\]\(\.\.\/\.\.\/\.\.\/docs\/README\.md\) owns hardware, HTTP API, and commands\./`AGENTS.md` owns hardware, HTTP API, and commands./' \
  "$fixture/.agents/skills/project-review/SKILL.md"
expect_failure "project-review owner contract" "$fixture" "documentation-owner contract"

fixture="$WORK/project-review-contradiction"; make_fixture "$fixture"
printf '\n`AGENTS.md` is the source of truth for the HTTP API.\n' >> \
  "$fixture/.agents/skills/project-review/SKILL.md"
expect_failure "compact AGENTS owner contradiction" "$fixture" \
  "assigns a deep technical catalog to compact AGENTS.md"

fixture="$WORK/flash-owner"; make_fixture "$fixture"
printf '\n`AGENTS.md` owns the HTTP API.\n' >> "$fixture/.agents/skills/flash-esp32/SKILL.md"
expect_failure "flash skill exact-content guard" "$fixture" "exact reviewed content contract drifted"

fixture="$WORK/flash-production-authority"; make_fixture "$fixture"
perl -0pi -e 's#scripts/ota-signing-public-key\.sha256#scripts/unreviewed-flash-key.sha256#g' \
  "$fixture/.agents/skills/flash-esp32/SKILL.md"
expect_failure "flash production authority" "$fixture" \
  "production-authority verification contract before flash"

fixture="$WORK/flash-preview-head-name"; make_fixture "$fixture"
perl -0pi -e 's/EXPECTED_ART="tesla-key-esp32-pr\$\{PR\}-\$\{EXPECTED_SHA\}"/EXPECTED_ART="legacy-preview-name"/' \
  "$fixture/.agents/skills/flash-esp32/SKILL.md"
expect_failure "flash preview full-head artifact" "$fixture" \
  "signed-preview consumer must bind artifact name to PR/full head SHA and version to metadata"

fixture="$WORK/flash-preview-leading-zero"; make_fixture "$fixture"
perl -0pi -e 's/\Q(0|[1-9][0-9]*)\E/[0-9]+/' \
  "$fixture/.agents/skills/flash-esp32/SKILL.md"
expect_failure "flash preview canonical core" "$fixture" \
  "signed-preview consumer must bind artifact name to PR/full head SHA and version to metadata"

fixture="$WORK/flash-preview-capture"; make_fixture "$fixture"
perl -0pi -e 's/BASH_REMATCH\[4\]/BASH_REMATCH[3]/' \
  "$fixture/.agents/skills/flash-esp32/SKILL.md"
expect_failure "flash preview PR capture" "$fixture" \
  "signed-preview consumer must bind artifact name to PR/full head SHA and version to metadata"

fixture="$WORK/ship-production-authority"; make_fixture "$fixture"
perl -0pi -e 's#scripts/ota-signing-public-key\.sha256#scripts/unreviewed-ship-key.sha256#g' \
  "$fixture/.agents/skills/ship/SKILL.md"
expect_failure "ship production authority" "$fixture" \
  "production-authority verification contract before flash"

fixture="$WORK/ship-main-artifact-head"; make_fixture "$fixture"
perl -0pi -e 's/tesla-key-esp32-\$VERSION-\$RUN_SHA/tesla-key-esp32-\$VERSION/' \
  "$fixture/.agents/skills/ship/SKILL.md"
expect_failure "ship main artifact full SHA" "$fixture" \
  "main artifact consumer must bind full run SHA and derive version from metadata"

fixture="$WORK/ship-main-artifact-leading-zero"; make_fixture "$fixture"
perl -0pi -e 's/\Q(0|[1-9][0-9]*)\E/[0-9]+/' \
  "$fixture/.agents/skills/ship/SKILL.md"
expect_failure "ship main artifact canonical core" "$fixture" \
  "main artifact consumer must bind full run SHA and derive version from metadata"

fixture="$WORK/ship-main-artifact-version-length"; make_fixture "$fixture"
perl -0pi -e 's/<= 31/<= 63/' "$fixture/.agents/skills/ship/SKILL.md"
expect_failure "ship main artifact version length" "$fixture" \
  "main artifact consumer must bind full run SHA and derive version from metadata"

fixture="$WORK/ota-release-stable-tag"; make_fixture "$fixture"
perl -0pi -e 's/(\[\[ "\$RELEASE_TAG" =~ [^\n]+)\$ \]\]/$1(-[0-9A-Za-z.-]+)?\$ ]]/g' \
  "$fixture/.agents/skills/ota-release-verify/SKILL.md"
expect_failure "OTA verifier stable Release" "$fixture" \
  "canonical <=31-byte stable Release tag"

fixture="$WORK/usb-release-stable-tag"; make_fixture "$fixture"
perl -0pi -e 's/(\[\[ "\$RELEASE_TAG" =~ [^\n]+)\$ \]\]/$1(-[0-9A-Za-z.-]+)?\$ ]]/' \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "USB recovery stable Release" "$fixture" \
  "Release selection must be canonical, stable and <=31 bytes"

fixture="$WORK/usb-production-authority"; make_fixture "$fixture"
perl -0pi -e 's#scripts/ota-signing-public-key\.sha256#scripts/unreviewed-recovery-key.sha256#g' \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb recovery production authority" "$fixture" \
  "production-authority verification contract before flash"

fixture="$WORK/usb-main-artifact-head"; make_fixture "$fixture"
perl -0pi -e 's/SIGNED_ART="tesla-key-esp32-\$VERSION-\$SOURCE_SHA"/SIGNED_ART="tesla-key-esp32-\$VERSION"/' \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb recovery main artifact full SHA" "$fixture" \
  "main artifact consumer must bind full run SHA and derive version from metadata"

fixture="$WORK/usb-main-artifact-version"; make_fixture "$fixture"
perl -0pi -e 's/VERSION=\$\(sed -n '\''s\/\^display_version=\/\/p'\'' "\$META"\)/VERSION="\${ART#tesla-key-esp32-}"/' \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb recovery main artifact metadata version" "$fixture" \
  "main artifact consumer must bind full run SHA and derive version from metadata"

fixture="$WORK/device-diag-owner"; make_fixture "$fixture"
perl -0pi -e 's/\[`docs\/ARCHITECTURE\.md`\]\(\.\.\/\.\.\/\.\.\/docs\/ARCHITECTURE\.md\) owns pairing lifecycle and invalidation\./`AGENTS.md` owns pairing lifecycle and invalidation./' \
  "$fixture/.agents/skills/device-diag/SKILL.md"
expect_failure "device-diag owner contract" "$fixture" "documentation-owner contract"

fixture="$WORK/vehicle-audit-owner"; make_fixture "$fixture"
perl -0pi -e 's/\[`docs\/README\.md`\]\(\.\.\/\.\.\/\.\.\/docs\/README\.md\) owns the HTTP command catalog\./`AGENTS.md` owns the HTTP command catalog./' \
  "$fixture/.agents/skills/vehicle-command-audit/SKILL.md"
expect_failure "vehicle audit owner contract" "$fixture" "documentation-owner contract"

fixture="$WORK/project-review-feature-path"; make_fixture "$fixture"
perl -0pi -e 's/tools\/agent-config\//tools\/agent-config-missing\//' \
  "$fixture/.agents/skills/project-review/SKILL.md"
expect_failure "project-review feature path" "$fixture" "omits a relevance-scope path"

fixture="$WORK/feature-template-path"; make_fixture "$fixture"
perl -0pi -e 's/\.github\/PULL_REQUEST_TEMPLATE\.md/.github\/MISSING_TEMPLATE.md/g' \
  "$fixture/.agents/skills/feature-docs/SKILL.md"
expect_failure "feature-docs PR template path" "$fixture" "omits a relevance-scope path"

fixture="$WORK/skill-audit-release-path"; make_fixture "$fixture"
perl -0pi -e 's/scripts\/release-relevance\.sh/scripts\/release-relevance-missing.sh/' \
  "$fixture/.agents/skills/skill-audit/SKILL.md"
expect_failure "skill-audit release relevance path" "$fixture" "omits a relevance-scope path"

fixture="$WORK/safety"; make_fixture "$fixture"
python3 - "$fixture/tools/agent-config/safety-invariants.json" <<'PY'
import json, pathlib, sys
path = pathlib.Path(sys.argv[1])
value = json.loads(path.read_text())
value["invariants"].append({"id": "missing-canary", "pattern": "NEVER_PRESENT_CANARY"})
path.write_text(json.dumps(value, indent=2) + "\n")
PY
expect_failure "missing safety invariant" "$fixture" "missing-canary"

fixture="$WORK/multi-target-publication-dag"; make_fixture "$fixture"
perl -0pi -e 's/logic-test -> build -> independent-rebuild -> publish ->/logic-test -> build -> publish ->/' \
  "$fixture/.codex/agents/multi_target_build_reviewer.toml"
expect_failure "multi-target publication DAG" "$fixture" \
  "multi-target reviewer is missing the independent-rebuild/publication DAG contract"

echo "== hook configuration =="
fixture="$WORK/hook-async"; make_fixture "$fixture"
python3 - "$fixture/.codex/hooks.json" <<'PY'
import json, pathlib, sys
path = pathlib.Path(sys.argv[1])
value = json.loads(path.read_text())
value["hooks"]["PreToolUse"][0]["hooks"][0]["async"] = True
path.write_text(json.dumps(value, indent=2) + "\n")
PY
expect_failure "async blocking hook" "$fixture" "must not be async"

fixture="$WORK/claude-hook-command"; make_fixture "$fixture"
python3 - "$fixture/.claude/settings.json" <<'PYCANARY'
import json, pathlib, sys
path = pathlib.Path(sys.argv[1])
value = json.loads(path.read_text())
value["hooks"]["PreToolUse"][0]["hooks"][0]["command"] = "true"
path.write_text(json.dumps(value, indent=2) + "\n")
PYCANARY
expect_failure "Claude hook command drift" "$fixture" "Claude PreToolUse command drifted"

fixture="$WORK/claude-hook-async"; make_fixture "$fixture"
python3 - "$fixture/.claude/settings.json" <<'PYCANARY'
import json, pathlib, sys
path = pathlib.Path(sys.argv[1])
value = json.loads(path.read_text())
value["hooks"]["PreToolUse"][0]["hooks"][0]["async"] = True
path.write_text(json.dumps(value, indent=2) + "\n")
PYCANARY
expect_failure "async Claude blocking hook" "$fixture" "must not be async"

fixture="$WORK/claude-hook-preapproval"; make_fixture "$fixture"
python3 - "$fixture/.claude/settings.json" <<'PYCANARY'
import json, pathlib, sys
path = pathlib.Path(sys.argv[1])
value = json.loads(path.read_text())
value["permissions"]["allow"] = ["Bash"]
path.write_text(json.dumps(value, indent=2) + "\n")
PYCANARY
expect_failure "Claude tool pre-approval" "$fixture" "must pre-approve no tool"

fixture="$WORK/claude-hook-matcher"; make_fixture "$fixture"
python3 - "$fixture/.claude/settings.json" <<'PYCANARY'
import json, pathlib, sys
path = pathlib.Path(sys.argv[1])
value = json.loads(path.read_text())
group = value["hooks"]["PreToolUse"][0]
group["matcher"] = group["matcher"].removeprefix("^")
path.write_text(json.dumps(value, indent=2) + "\n")
PYCANARY
expect_failure "unanchored Claude matcher" "$fixture" "Claude PreToolUse matcher drifted"

fixture="$WORK/hook-matcher"; make_fixture "$fixture"
python3 - "$fixture/.codex/hooks.json" <<'PY'
import json, pathlib, sys
path = pathlib.Path(sys.argv[1])
value = json.loads(path.read_text())
value["hooks"]["PreToolUse"][0]["matcher"] = value["hooks"]["PreToolUse"][0]["matcher"].removeprefix("^")
path.write_text(json.dumps(value, indent=2) + "\n")
PY
expect_failure "unanchored blocking matcher" "$fixture" "matcher drifted"

echo
echo "agent-config selftest: all $passes mutation canaries caught"
