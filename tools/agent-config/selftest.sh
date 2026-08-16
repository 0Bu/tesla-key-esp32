#!/usr/bin/env bash
# Mutation canaries for mapping, parsed configuration, hooks, adapters and safety parity.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
passes=0

fail() { echo "agent-config selftest: $1" >&2; exit 1; }

make_fixture() {
  local dest="$1" relative
  rm -rf "$dest"
  mkdir -p "$dest"
  node - "$ROOT/.codex/migration-manifest.json" > "$dest/files.txt" <<'NODE'
const fs = require("node:fs");
const manifest = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const files = new Set([
  ".mcp.json",
  ".codex/migration-manifest.json",
  "tools/agent-config/safety-invariants.json",
  "tools/agent-config/check_hooks.py",
  "tools/agent-hooks/agent_hook.py",
  "tools/agent-hooks/merge_payload.py",
  "tools/agent-hooks/pr-gate-lib.sh",
  "tools/agent-hooks/require-pr-gates.sh",
  "tools/agent-hooks/run_with_timeout.py",
  "tools/agent-hooks/selftest.sh",
  manifest.canonical_instructions,
]);
for (const entry of manifest.entries) {
  files.add(entry.source);
  for (const target of entry.targets) files.add(target);
}
process.stdout.write([...files].sort().join("\n") + "\n");
NODE
  while IFS= read -r relative; do
    [ -n "$relative" ] || continue
    mkdir -p "$dest/$(dirname "$relative")"
    cp "$ROOT/$relative" "$dest/$relative"
  done < "$dest/files.txt"
  git -C "$ROOT" ls-files -- .claude > "$dest/tracked.txt" \
    || fail "could not enumerate tracked legacy files"
}

refresh_fingerprint() {
  local fixture="$1"
  node - "$fixture" <<'NODE'
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const root = process.argv[2];
const manifestFile = path.join(root, ".codex/migration-manifest.json");
const manifest = JSON.parse(fs.readFileSync(manifestFile, "utf8"));
const files = fs.readFileSync(path.join(root, "tracked.txt"), "utf8").split(/\r?\n/).filter(Boolean)
  .sort((a, b) => Buffer.compare(Buffer.from(a), Buffer.from(b)));
const hash = crypto.createHash("sha256");
for (const file of files) {
  hash.update(Buffer.from(file)); hash.update(Buffer.from([0]));
  hash.update(fs.readFileSync(path.join(root, file))); hash.update(Buffer.from([0]));
}
manifest.legacy_tree_sha256 = hash.digest("hex");
fs.writeFileSync(manifestFile, JSON.stringify(manifest, null, 2) + "\n");
NODE
}

run_gate() {
  local fixture="$1"
  AGENT_CONFIG_ROOT="$fixture" AGENT_CONFIG_TRACKED_FILES_FILE="$fixture/tracked.txt" \
    node "$ROOT/tools/agent-config/check.mjs" || return
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

echo "== clean contract =="
fixture="$WORK/clean"
make_fixture "$fixture"
run_gate "$fixture" >/dev/null || fail "clean fixture failed"
echo "  PASS  clean fixture"
passes=$((passes + 1))

echo "== manifest and fingerprint =="
fixture="$WORK/missing-target"; make_fixture "$fixture"
target="$(node - "$fixture/.codex/migration-manifest.json" <<'NODE'
const fs = require("node:fs");
const m = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
process.stdout.write(m.entries.find((entry) => entry.status !== "deprecated").targets[0]);
NODE
)"
mv "$fixture/$target" "$fixture/$target.missing"
expect_failure "missing mapping target" "$fixture" "target for"

fixture="$WORK/unmapped"; make_fixture "$fixture"
printf '.claude/unmapped-canary\n' >> "$fixture/tracked.txt"
expect_failure "unmapped tracked legacy file" "$fixture" "absent from migration manifest"

fixture="$WORK/duplicate"; make_fixture "$fixture"
node - "$fixture/.codex/migration-manifest.json" <<'NODE'
const fs = require("node:fs"); const file = process.argv[2];
const m = JSON.parse(fs.readFileSync(file, "utf8")); m.entries.push(m.entries[0]);
fs.writeFileSync(file, JSON.stringify(m, null, 2) + "\n");
NODE
expect_failure "duplicate legacy mapping" "$fixture" "more than once"

fixture="$WORK/fingerprint"; make_fixture "$fixture"
printf '\nlegacy drift canary\n' >> "$fixture/.claude/CLAUDE.md"
expect_failure "legacy body drift" "$fixture" "fingerprint drifted"

fixture="$WORK/budget"; make_fixture "$fixture"
set +e
output="$(AGENT_CONFIG_ROOT="$fixture" AGENT_CONFIG_TRACKED_FILES_FILE="$fixture/tracked.txt" \
  AGENT_INSTRUCTIONS_BUDGET_BYTES=1 node "$ROOT/tools/agent-config/check.mjs" 2>&1)"; rc=$?
set -e
[ "$rc" -ne 0 ] && printf '%s' "$output" | grep -qF "over the 1-byte budget" \
  || fail "AGENTS budget mutation was not caught"
echo "  PASS  AGENTS budget"
passes=$((passes + 1))

echo "== parsed TOML and skills =="
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
expect_failure "canonical skill model pin" "$fixture" "frontmatter keys must be exactly"

fixture="$WORK/usb-live-boundary"; make_fixture "$fixture"
perl -0pi -e 's/USB-write approval does not authorize live verification/USB write also authorizes live verification/g' \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb recovery live boundary" "$fixture" "exact live-verification contract"

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

fixture="$WORK/usb-legacy-boundary"; make_fixture "$fixture"
perl -0pi -e 's/Before any HTTP request/After any HTTP request/g' \
  "$fixture/.claude/skills/usb-recovery/SKILL.md"
refresh_fingerprint "$fixture"
expect_failure "legacy usb recovery boundary" "$fixture" "exact live-verification contract"

fixture="$WORK/usb-operational-stop"; make_fixture "$fixture"
perl -0pi -e 's/If that approval is absent, stop after/If that approval is absent, continue after/' \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb recovery operational stop" "$fixture" "contradicts the live-verification contract"

fixture="$WORK/usb-canonical-contradiction"; make_fixture "$fixture"
printf '\nThe USB-write approval also authorizes live verification of any GET endpoint.\n' >> \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "canonical usb recovery contradiction" "$fixture" "contradicts the live-verification contract"

fixture="$WORK/usb-absent-approval-continues"; make_fixture "$fixture"
printf '\nIf that approval is absent, continue with the HTTP requests.\n' >> \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb recovery absent approval continues" "$fixture" "contradicts the live-verification contract"

fixture="$WORK/usb-legacy-contradiction"; make_fixture "$fixture"
printf '\nThe USB-write approval also authorizes live verification of any GET endpoint.\n' >> \
  "$fixture/.claude/skills/usb-recovery/SKILL.md"
refresh_fingerprint "$fixture"
expect_failure "legacy usb recovery contradiction" "$fixture" "contradicts the live-verification contract"

fixture="$WORK/usb-live-needs-no-approval"; make_fixture "$fixture"
printf '\nLive verification needs no separate approval.\n' >> \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb live needs no approval" "$fixture" "exact reviewed content contract drifted"

fixture="$WORK/usb-legacy-http-no-approval"; make_fixture "$fixture"
printf '\nNo separate approval is required for HTTP requests.\n' >> \
  "$fixture/.claude/skills/usb-recovery/SKILL.md"
refresh_fingerprint "$fixture"
expect_failure "legacy HTTP needs no approval" "$fixture" "exact reviewed content contract drifted"

fixture="$WORK/usb-ota-read-only"; make_fixture "$fixture"
printf '\n`GET /ota/check` is read-only.\n' >> \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb OTA read-only contradiction" "$fixture" "exact reviewed content contract drifted"

fixture="$WORK/usb-proceed-even-without-approval"; make_fixture "$fixture"
printf '\nProceed with live HTTP verification even if approval is absent.\n' >> \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb proceed without approval" "$fixture" "exact reviewed content contract drifted"

fixture="$WORK/usb-write-permits-http"; make_fixture "$fixture"
printf '\nThe USB write approval permits HTTP live verification.\n' >> \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb write permits HTTP" "$fixture" "exact reviewed content contract drifted"

fixture="$WORK/usb-split-authorization"; make_fixture "$fixture"
printf '\nThe USB-write approval is sufficient. For live verification, use it.\n' >> \
  "$fixture/.agents/skills/usb-recovery/SKILL.md"
expect_failure "usb split authorization contradiction" "$fixture" "exact reviewed content contract drifted"

fixture="$WORK/project-review-owner"; make_fixture "$fixture"
perl -0pi -e 's/\[`docs\/README\.md`\]\(\.\.\/\.\.\/\.\.\/docs\/README\.md\) owns hardware, HTTP API, and commands\./`AGENTS.md` owns hardware, HTTP API, and commands./' \
  "$fixture/.agents/skills/project-review/SKILL.md"
expect_failure "project-review owner contract" "$fixture" "exact documentation-owner contract"

fixture="$WORK/project-review-contradiction"; make_fixture "$fixture"
printf '\n`AGENTS.md` contains the HTTP API catalog.\n' >> \
  "$fixture/.agents/skills/project-review/SKILL.md"
expect_failure "compact AGENTS contradiction" "$fixture" "deep technical catalog to compact AGENTS.md"

fixture="$WORK/project-review-documented-in"; make_fixture "$fixture"
printf '\nThe HTTP API is documented in `AGENTS.md`.\n' >> \
  "$fixture/.agents/skills/project-review/SKILL.md"
expect_failure "compact AGENTS documented-in contradiction" "$fixture" "deep technical catalog to compact AGENTS.md"

fixture="$WORK/project-review-source-of-truth"; make_fixture "$fixture"
printf '\n`AGENTS.md` is the source of truth for the HTTP API.\n' >> \
  "$fixture/.agents/skills/project-review/SKILL.md"
expect_failure "compact AGENTS source-of-truth contradiction" "$fixture" "deep technical catalog to compact AGENTS.md"

fixture="$WORK/legacy-project-review-owner"; make_fixture "$fixture"
printf '\n`AGENTS.md` is the source of truth for the HTTP API.\n' >> \
  "$fixture/.claude/skills/project-review/SKILL.md"
refresh_fingerprint "$fixture"
expect_failure "legacy compact AGENTS owner contradiction" "$fixture" "deep technical catalog to compact AGENTS.md"

fixture="$WORK/project-review-belongs-in"; make_fixture "$fixture"
printf '\nThe HTTP API belongs in `AGENTS.md`.\n' >> \
  "$fixture/.agents/skills/project-review/SKILL.md"
expect_failure "compact AGENTS belongs-in contradiction" "$fixture" "deep technical catalog to compact AGENTS.md"

fixture="$WORK/legacy-project-review-authoritative"; make_fixture "$fixture"
printf '\n`AGENTS.md` is authoritative for the HTTP API.\n' >> \
  "$fixture/.claude/skills/project-review/SKILL.md"
refresh_fingerprint "$fixture"
expect_failure "legacy compact AGENTS authoritative contradiction" "$fixture" "deep technical catalog to compact AGENTS.md"

fixture="$WORK/project-review-canonical-home"; make_fixture "$fixture"
printf '\nThe canonical home for the HTTP API is `AGENTS.md`.\n' >> \
  "$fixture/.agents/skills/project-review/SKILL.md"
expect_failure "compact AGENTS canonical-home contradiction" "$fixture" "deep technical catalog to compact AGENTS.md"

fixture="$WORK/device-diag-owner"; make_fixture "$fixture"
perl -0pi -e 's/\[`docs\/ARCHITECTURE\.md`\]\(\.\.\/\.\.\/\.\.\/docs\/ARCHITECTURE\.md\) owns pairing lifecycle and invalidation\./`AGENTS.md` owns pairing lifecycle and invalidation./' \
  "$fixture/.agents/skills/device-diag/SKILL.md"
expect_failure "device-diag owner contract" "$fixture" "exact documentation-owner contract"

fixture="$WORK/vehicle-audit-owner"; make_fixture "$fixture"
perl -0pi -e 's/\[`docs\/README\.md`\]\(\.\.\/\.\.\/\.\.\/docs\/README\.md\) owns the HTTP command catalog\./`AGENTS.md` owns the HTTP command catalog./' \
  "$fixture/.agents/skills/vehicle-command-audit/SKILL.md"
expect_failure "vehicle audit owner contract" "$fixture" "exact documentation-owner contract"

fixture="$WORK/project-review-feature-path"; make_fixture "$fixture"
perl -0pi -e 's/, and\n  `tools\/agent-config\/`/, and\n  `tools\/agent-config-missing\/`/' \
  "$fixture/.agents/skills/project-review/SKILL.md"
expect_failure "project-review feature path" "$fixture" "omits a relevance-scope path"

fixture="$WORK/skill-audit-release-path"; make_fixture "$fixture"
perl -0pi -e 's/scripts\/release-relevance\.sh/scripts\/release-relevance-missing.sh/' \
  "$fixture/.agents/skills/skill-audit/SKILL.md"
expect_failure "skill-audit release relevance path" "$fixture" "omits a relevance-scope path"

fixture="$WORK/safety"; make_fixture "$fixture"
node - "$fixture/tools/agent-config/safety-invariants.json" <<'NODE'
const fs = require("node:fs"); const file = process.argv[2];
const value = JSON.parse(fs.readFileSync(file, "utf8"));
value.invariants.push({id: "missing-canary", pattern: "NEVER_PRESENT_CANARY"});
fs.writeFileSync(file, JSON.stringify(value, null, 2) + "\n");
NODE
expect_failure "missing safety invariant" "$fixture" "missing-canary"

echo "== hook configuration and adapters =="
fixture="$WORK/hook-async"; make_fixture "$fixture"
node - "$fixture/.codex/hooks.json" <<'NODE'
const fs = require("node:fs"); const file = process.argv[2];
const value = JSON.parse(fs.readFileSync(file, "utf8"));
value.hooks.PreToolUse[0].hooks[0].async = true;
fs.writeFileSync(file, JSON.stringify(value, null, 2) + "\n");
NODE
expect_failure "async blocking hook" "$fixture" "must not be async"

fixture="$WORK/hook-matcher"; make_fixture "$fixture"
node - "$fixture/.codex/hooks.json" <<'NODE'
const fs = require("node:fs"); const file = process.argv[2];
const value = JSON.parse(fs.readFileSync(file, "utf8"));
value.hooks.PreToolUse[0].matcher = value.hooks.PreToolUse[0].matcher.replace(/^\^/, "");
fs.writeFileSync(file, JSON.stringify(value, null, 2) + "\n");
NODE
expect_failure "unanchored blocking matcher" "$fixture" "matcher drifted"

fixture="$WORK/permissions"; make_fixture "$fixture"
node - "$fixture/.claude/settings.json" <<'NODE'
const fs = require("node:fs"); const file = process.argv[2];
const value = JSON.parse(fs.readFileSync(file, "utf8")); value.permissions.allow = ["Bash(*)"];
fs.writeFileSync(file, JSON.stringify(value, null, 2) + "\n");
NODE
refresh_fingerprint "$fixture"
expect_failure "legacy permission grant" "$fixture" "permissions.allow must be empty"

fixture="$WORK/adapter"; make_fixture "$fixture"
printf '%s\n' '#!/usr/bin/env bash' 'exit 0' > "$fixture/.claude/hooks/guard-secrets.sh"
chmod +x "$fixture/.claude/hooks/guard-secrets.sh"
refresh_fingerprint "$fixture"
expect_failure "corrupt Claude adapter" "$fixture" "adapter drifted"

fixture="$WORK/adapter-extra"; make_fixture "$fixture"
printf '%s\n' 'true # unauthorized extra adapter behavior' >> "$fixture/.claude/hooks/guard-secrets.sh"
refresh_fingerprint "$fixture"
expect_failure "extra Claude adapter behavior" "$fixture" "exact thin delegation"

echo
echo "agent-config selftest: all $passes mutation canaries caught"
