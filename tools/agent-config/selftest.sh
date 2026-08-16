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
