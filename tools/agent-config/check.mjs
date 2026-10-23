#!/usr/bin/env node
// Deterministic project-agent instruction and safety contract.
import fs from "node:fs";
import path from "node:path";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";

function die(code, message) {
  console.error(`agent-config: ${message}`);
  process.exit(code);
}

function positiveInteger(value, label) {
  if (!/^[1-9][0-9]*$/.test(String(value))) die(2, `${label} must be a positive integer`);
  return Number(value);
}

function readJson(file, label) {
  let source;
  try { source = fs.readFileSync(file, "utf8"); }
  catch (error) { die(2, `cannot read ${label}: ${error.message}`); }
  try { return JSON.parse(source); }
  catch (error) { die(2, `${label} is not valid JSON: ${error.message}`); }
}

function normalizeProse(text) {
  return text.replace(/\s+/g, " ").trim();
}

function proseSentences(text) {
  const protectedDots = normalizeProse(text).replaceAll(".md", "__DOT_MD__");
  return protectedDots.split(/(?<=[.!?])\s+/).map((sentence) =>
    sentence.replaceAll("__DOT_MD__", ".md")
  );
}

function restrictedFrontmatter(file, label) {
  let text;
  try { text = fs.readFileSync(file, "utf8"); }
  catch (error) { die(1, `${label} is unreadable: ${error.message}`); }
  const lines = text.split(/\r?\n/);
  if (lines[0] !== "---") die(1, `${label} has no YAML frontmatter`);
  const end = lines.indexOf("---", 1);
  if (end < 0) die(1, `${label} has unterminated YAML frontmatter`);
  const values = new Map();
  for (const line of lines.slice(1, end)) {
    if (!line.trim()) continue;
    const match = line.match(/^([A-Za-z0-9_-]+):\s*(.+)$/);
    if (!match) die(1, `${label} has invalid restricted YAML frontmatter`);
    if (values.has(match[1])) die(1, `${label} duplicates frontmatter key ${match[1]}`);
    values.set(match[1], match[2].trim());
  }
  const body = lines.slice(end + 1).join("\n").trim();
  if (!body) die(1, `${label} has an empty body`);
  return { text, values, body };
}

const root = path.resolve(
  process.env.AGENT_CONFIG_ROOT || path.join(path.dirname(fileURLToPath(import.meta.url)), "../.."),
);
const repoPath = (relative) => path.join(root, ...relative.split("/"));
const budget = positiveInteger(process.env.AGENT_INSTRUCTIONS_BUDGET_BYTES || "24576", "AGENTS budget");

function regularFile(relative, label) {
  let stat;
  try { stat = fs.statSync(repoPath(relative)); }
  catch { die(1, `${label} is missing: ${relative}`); }
  if (!stat.isFile()) die(1, `${label} is not a regular file: ${relative}`);
}

if (fs.existsSync(repoPath(".claude"))) {
  die(1, ".claude metadata must remain retired; use AGENTS.md, .agents and tools/agent-hooks");
}
regularFile("AGENTS.md", "canonical instructions");
const agentsSize = fs.statSync(repoPath("AGENTS.md")).size;
if (agentsSize > budget) die(1, `AGENTS.md is ${agentsSize} bytes, over the ${budget}-byte budget`);

function directoryNames(relative) {
  try {
    return fs.readdirSync(repoPath(relative), { withFileTypes: true })
      .filter((entry) => entry.isDirectory()).map((entry) => entry.name).sort();
  } catch { die(1, `directory is missing: ${relative}`); }
}

const canonicalSkills = directoryNames(".agents/skills");

const highRiskSkills = new Set(["deploy", "deploy-bench", "flash-esp32", "ship", "usb-recovery"]);
const readOnlySkills = new Set([
  "device-diag", "display-preview", "mock-test", "ota-release-verify", "pr-hygiene", "project-review",
  "skill-audit", "vehicle-command-audit",
]);
const ownerContracts = new Map([
  ["project-review", [
    "[`AGENTS.md`](../../../AGENTS.md) owns runner policy, authorization, safety, evidence, and review contracts.",
    "[`docs/USER_GUIDE.md`](../../../docs/USER_GUIDE.md) owns hardware and operation.",
    "[`docs/API.md`](../../../docs/API.md) owns HTTP API, commands, and MQTT topics.",
    "[`docs/DEVELOPMENT.md`](../../../docs/DEVELOPMENT.md) owns build contracts.",
    "[`docs/ARCHITECTURE.md`](../../../docs/ARCHITECTURE.md) owns telemetry, MQTT, sleep/link state, pairing, and OTA.",
    "[`docs/MCP.md`](../../../docs/MCP.md) owns MCP tools.",
    "[`docs/SECURITY.md`](../../../docs/SECURITY.md) owns NVS, signing, and exposure.",
  ]],
  ["device-diag", [
    "[`docs/ARCHITECTURE.md`](../../../docs/ARCHITECTURE.md) owns pairing lifecycle and invalidation.",
  ]],
  ["vehicle-command-audit", [
    "[`docs/API.md`](../../../docs/API.md) owns the HTTP command catalog.",
    "[`docs/ARCHITECTURE.md`](../../../docs/ARCHITECTURE.md) owns link-state and pairing semantics.",
    "[`docs/MCP.md`](../../../docs/MCP.md) owns MCP tools.",
    "`AGENTS.md` owns only runner policy and safety boundaries.",
  ]],
]);
const deepOwnerTerms = /(?:HTTP|\bAPI\b|(?<![-/])\bcommands?\b|\bMCP\b|\bNVS\b|\bMQTT\b|link-state|pairing)/i;
const usbPositiveAuthorization = /USB-write approval[^.!?]*(?:also\s+)?(?:authorizes|allows|permits|covers|includes|is sufficient for)[^.!?]*(?:live verification|HTTP|GET)/i;
const usbNoApprovalNeeded = /(?:live verification|HTTP requests?|(?:GET|POST|HTTP) endpoints?)[^.!?]*(?:without (?:separate )?(?:approval|authorization)|requires? no (?:approval|authorization)|need not (?:be )?(?:approved|authorized))/i;
const usbOtaNotStateChanging = /(?:GET|POST) \/ota\/check[^.!?]*(?:is not|isn't|not) state-changing/i;
const usbAbsentApprovalProceeds = /(?:(?:approval|authorization)[^.!?]*(?:absent|missing|not obtained)|without (?:separate )?(?:approval|authorization))[^.!?]*(?:continue|proceed|run|contact|send|request)/i;
const reviewedSkillSha256 = new Map([
  ["add-logic-test", "b37264357f243110c29275fb9c4e19dcbaba692920fa248a2ba951632ee6f2ac"],
  ["ci-heal", "ea3440e3333ff118413f28c88ac0ac71d4d51ab1d62cdadbea5b18d187e72074"],
  ["deploy", "169d30f409d6a447752b5fc7f3b7579ee6e714df9ce33ecd51cb344dd0d877f0"],
  ["deploy-bench", "0471fee2f77a43b0e3e8292956bb74bc2d08020bd25fe29f416ea5ba609b7e02"],
  ["device-diag", "76d7a48051049ee0c105531baedbf66841758442b12c463e4ecd9591db95030b"],
  ["display-preview", "32fae92f904a8385ce86f60f7fbd6cbacbd93c6b2a21343c393dece4174ec313"],
  ["feature-docs", "17a486258723b88a5d1257284a33e295529bd52331d3b066335e832b666f1884"],
  ["flash-esp32", "47085d29aaf104489e6a154b2f59c7a26b3498014ad1cbe14e4ba8ab4e5dc498"],
  ["mock-test", "394d21dc5b76e529089362f2346c434b0060dd05c0948bf520b1f411cd90f9d3"],
  ["ota-release-verify", "82fb2844d28f95ba2ea49504202dc8e0dd116a1062d17b59686a6cf46ed3448e"],
  ["pr-hygiene", "8491da669686acc5e00741e5e1fcfec6d2caf29abb8ebff0479ca973f20e87f1"],
  ["project-review", "2a9490b88cf001e5e4a72bb309426c90b841c54a94eb7db389d08fef03cbed6b"],
  ["ship", "f48fe6041073eb930574cacd87aae64c4d954b9683a8c8de9d2b8d1bb958a8fa"],
  ["skill-audit", "f55998cd31158aed7d8f889d25a5ec3845964693f7dd1b53f1ada944335a51ec"],
  ["usb-recovery", "02c77d65474e6f426b51eb434e7633d2bb9f189f58ecab2c3110d3fd09325421"],
  ["vehicle-command-audit", "f2992b4a9b3639e7bd3465d5153fa09b47ceca367a69cc9244307f8a1644558b"],
]);
const featureDocsScopeTokens = [
  "main/", "test/", "sdkconfig.defaults*", "partitions.csv", "AGENTS.md", ".agents/", ".codex/",
  ".github/PULL_REQUEST_TEMPLATE.md",
  "tools/agent-hooks/", "tools/agent-config/", "docs/index.html", "installer-bootstrap.mjs",
  "serial-port-release.mjs", "web-installer.mjs", "docs/vendor/",
  "build,signed-pr-preview,pr-preview-cleanup,pr-policy,bench-acceptance", "scripts/release-relevance.sh",
];
const prHygieneContracts = [
  "### `PRIVACY-LEAK`", "### `LANGUAGE`", "at PR creation, every push, and merge",
  "192.0.2.0/24", "198.51.100.0/24", "203.0.113.0/24", "2001:db8::/32",
  "- [x] `$pr-hygiene` clean — content gate @ <full-40-hex-sha>",
];
const shipMergeGateContracts = [
  "- [x] $project-review clean — merge gate @ <sha>",
  "- [x] $pr-hygiene clean — content gate @ <sha>",
  "- [x] $feature-docs synced — merge gate @ <sha>",
  "- [x] $vehicle-command-audit clean — merge gate @ <sha>",
  "`$project-review` does not establish `$pr-hygiene` readiness",
];
const productionFlashAuthorityCommand =
  'python3 scripts/check-firmware-artifacts.py --app-only --target "$TARGET" --version "$VERSION" --app "$APP" --signed-app --expected-public-key-digest scripts/ota-signing-public-key.sha256';
const productionFlashAuthorityCounts = new Map([
  ["flash-esp32", 1],
  ["ship", 1],
  ["usb-recovery", 2],
]);
const canonicalMainArtifactNameRegex =
  'grep -E "^tesla-key-esp32-(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?-${RUN_SHA}$"';
const canonicalMainVersionRegex =
  '[[ "$VERSION" =~ ^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$ ]]';
const canonicalVersionLength = '&& (( ${#VERSION} <= 31 ))';
const canonicalStableReleaseTagRegex =
  '[[ "$RELEASE_TAG" =~ ^v(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$ ]]';
const mainArtifactConsumerContracts = new Map([
  ["ship", [
    canonicalMainArtifactNameRegex,
    "VERSION=$(sed -n 's/^display_version=//p' \"$META\")",
    canonicalMainVersionRegex,
    canonicalVersionLength,
    '[ "$ART" = "tesla-key-esp32-$VERSION-$RUN_SHA" ]',
  ]],
  ["usb-recovery", [
    'SIGNED_ART="tesla-key-esp32-$VERSION-$SOURCE_SHA"',
    canonicalMainArtifactNameRegex,
    "VERSION=$(sed -n 's/^display_version=//p' \"$META\")",
    canonicalMainVersionRegex,
    canonicalVersionLength,
    '[ "$ART" = "tesla-key-esp32-$VERSION-$RUN_SHA" ]',
  ]],
]);
let idfComponentYml;
try { idfComponentYml = fs.readFileSync(repoPath("main/idf_component.yml"), "utf8"); }
catch { idfComponentYml = null; }
const teslaBlePinMatch = idfComponentYml ? idfComponentYml.match(/git:\s*"https:\/\/github\.com\/yoziru\/tesla-ble\.git"\s+version:\s*"([^"]+)"/) : null;

for (const name of canonicalSkills) {
  const canonical = restrictedFrontmatter(
    repoPath(`.agents/skills/${name}/SKILL.md`), `canonical skill ${name}`,
  );
  if (canonical.values.get("name") !== name) {
    die(1, `skill ${name} frontmatter name mismatch`);
  }
  if (!canonical.values.get("description")) {
    die(1, `skill ${name} needs a description`);
  }
  const canonicalKeys = [...canonical.values.keys()].sort();
  if (canonicalKeys.join("\0") !== "description\0name") {
    die(1, `canonical skill ${name} frontmatter keys must be exactly name and description`);
  }
  const invocation = new RegExp(`(^|[\\s\\x60(])/${name}(?=$|[\\s\\x60.,;)])`, "m");
  if (invocation.test(canonical.text)) die(1, `canonical skill ${name} uses legacy /${name} invocation`);
  if (!canonical.text.includes(`$${name}`)) die(1, `canonical skill ${name} must identify itself as $${name}`);
  if (highRiskSkills.has(name) && !/explicit user (?:authorization|approval)|user explicitly authoriz/is.test(canonical.text)) {
    die(1, `high-risk skill ${name} must require explicit user authorization`);
  }
  if (name === "usb-recovery") {
    const liveContracts = [
      "The USB-write approval does not authorize live verification.",
      "Before any HTTP request, obtain separate explicit user approval for the exact recovered device/IP and the named HTTP methods and endpoints.",
      "`POST /ota/check` is state-changing and must be named explicitly in that live approval.",
    ];
    const normalized = normalizeProse(canonical.text);
    if (liveContracts.some((required) => !normalized.includes(required))) {
      die(1, "canonical usb-recovery skill is missing the exact live-verification contract");
    }
    const contradiction = proseSentences(canonical.text).some((sentence) =>
      usbPositiveAuthorization.test(sentence) || usbNoApprovalNeeded.test(sentence) ||
      usbOtaNotStateChanging.test(sentence) || usbAbsentApprovalProceeds.test(sentence)
    );
    if (contradiction) {
      die(1, "canonical usb-recovery skill contradicts the live-verification contract");
    }
    const operationalContracts = [
      "Do not run this section merely because the USB recovery was approved.",
      "If that approval is absent, stop after the verified USB write and report that live recovery acceptance remains pending.",
    ];
    const normalizedCanonical = normalizeProse(canonical.text);
    if (operationalContracts.some((required) => !normalizedCanonical.includes(required))) {
      die(1, "canonical usb-recovery skill is missing the exact operational live-verification stop");
    }
  }
  if (name === "pr-hygiene") {
    const normalized = normalizeProse(canonical.text);
    if (prHygieneContracts.some((required) => !normalized.includes(required))) {
      die(1, "pr-hygiene is missing a privacy/language gate contract");
    }
  }
  if (name === "ship") {
    const normalized = normalizeProse(canonical.text);
    if (shipMergeGateContracts.some((required) => !normalized.includes(required))) {
      die(1, "ship is missing a merge-gate contract");
    }
  }
  const expectedFlashAuthorityCount = productionFlashAuthorityCounts.get(name);
  if (expectedFlashAuthorityCount !== undefined) {
    const actualFlashAuthorityCount = canonical.text.split(productionFlashAuthorityCommand).length - 1;
    if (actualFlashAuthorityCount !== expectedFlashAuthorityCount) {
      die(1, `${name} is missing its production-authority verification contract before flash`);
    }
  }
  if (name === "ship") {
    const commonTargetBlock = canonical.text.match(
      /## 4\. Select target, then USB-flash or OTA[\s\S]*?```bash\n([\s\S]*?)```\s*\n### Explicitly selected USB delivery/,
    )?.[1] || "";
    const commonVerification = [
      'APP="$SHIP_DIR/tesla-key-esp32$SFX.bin"',
      '[ -f "$APP" ] && [ ! -L "$APP" ]',
      '[ "$(wc -c < "$APP" | tr -d \' \')" -le $((0x1e8000)) ]',
      productionFlashAuthorityCommand,
    ];
    let previousIndex = commonTargetBlock.indexOf('unsupported target');
    if (previousIndex < 0 || commonVerification.some((required) => {
      const index = commonTargetBlock.indexOf(required);
      const outOfOrder = index <= previousIndex;
      previousIndex = index;
      return outOfOrder;
    })) {
      die(1, "ship must verify the selected app in the common block before USB or OTA delivery");
    }
  }
  const mainArtifactContracts = mainArtifactConsumerContracts.get(name);
  if (mainArtifactContracts?.some((required) => !canonical.text.includes(required))) {
    die(1, `${name} main artifact consumer must bind full run SHA and derive version from metadata`);
  }
  if (name === "flash-esp32") {
    const previewConsumerContracts = [
      'EXPECTED_ART="tesla-key-esp32-pr${PR}-${EXPECTED_SHA}"',
      'VERSION=$(sed -n \'s/^display_version=//p\' "$META")',
      '[[ "$VERSION" =~ ^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)-PR-([1-9][0-9]*)$ ]]',
      canonicalVersionLength,
      '[ "${BASH_REMATCH[4]}" = "$PR" ]',
    ];
    if (previewConsumerContracts.some((required) => !canonical.text.includes(required))) {
      die(1, "flash-esp32 signed-preview consumer must bind artifact name to PR/full head SHA and version to metadata");
    }
  }
  if (name === "ota-release-verify") {
    const stableTagCount = canonical.text.split(canonicalStableReleaseTagRegex).length - 1;
    const stableLengthCount = canonical.text.split('(( ${#REL} <= 31 ))').length - 1;
    if (stableTagCount !== 2 || stableLengthCount !== 2) {
      die(1, "ota-release-verify must select the canonical <=31-byte stable Release tag in both snapshots");
    }
  }
  if (name === "usb-recovery") {
    const releaseVersionContract =
      'VERSION=${RELEASE_TAG#v}\n(( ${#VERSION} <= 31 )) || {';
    if (!canonical.text.includes(canonicalStableReleaseTagRegex) ||
        !canonical.text.includes(releaseVersionContract)) {
      die(1, "usb-recovery Release selection must be canonical, stable and <=31 bytes");
    }
  }
  const requiredOwners = ownerContracts.get(name);
  if (requiredOwners) {
    const normalized = normalizeProse(canonical.text);
    if (requiredOwners.some((required) => !normalized.includes(required))) {
      die(1, `${name} is missing an exact documentation-owner contract`);
    }
    const contradictoryOwner = proseSentences(canonical.text).some((sentence) =>
      /AGENTS\.md/i.test(sentence) && deepOwnerTerms.test(sentence)
    );
    if (contradictoryOwner) {
      die(1, `canonical ${name} assigns a deep technical catalog to compact AGENTS.md`);
    }
  }
  if (["feature-docs", "project-review", "skill-audit"].includes(name)) {
    const scopeText = name === "feature-docs" ? canonical.text :
      canonical.text.match(/\*\*`\$feature-docs`\*\*[\s\S]*?(?=\n- \*\*`\$|\n### |\n## |$)/)?.[0] || "";
    if (featureDocsScopeTokens.some((required) => !scopeText.includes(required))) {
      die(1, `${name} feature-docs checklist omits a relevance-scope path`);
    }
  }
  if (["vehicle-command-audit", "skill-audit", "project-review"].includes(name)) {
    if (teslaBlePinMatch && !canonical.text.includes(teslaBlePinMatch[1])) {
      die(1, `canonical ${name} pin assertion does not match idf_component.yml pin (${teslaBlePinMatch[1]})`);
    }
  }
  if (["project-review", "skill-audit"].includes(name)) {
    const textLower = canonical.text.toLowerCase();
    for (const token of [
      "step 0 — pin the baseline",
      "git fetch origin",
      "fail-fast on divergence",
      "state reviewed sha in report",
    ]) {
      if (!textLower.includes(token)) {
        die(1, `canonical ${name} is missing baseline pinning contract: ${token}`);
      }
    }
  }
  if (readOnlySkills.has(name) && !/read-only|does not (?:edit|modify)|must not (?:edit|modify)/is.test(canonical.text)) {
    die(1, `review/diagnostic skill ${name} must state its read-only boundary`);
  }
  const expectedDigest = reviewedSkillSha256.get(name);
  if (!expectedDigest) die(1, `skill ${name} has no reviewed content digest`);
  const digest = createHash("sha256").update(canonical.text).digest("hex");
  if (digest !== expectedDigest) {
    die(1, `canonical ${name} exact reviewed content contract drifted`);
  }
}

for (const [relative, contracts] of new Map([
  ["AGENTS.md", ["`$pr-hygiene` is required at PR creation, every push, and every merge"]],
  [".github/PULL_REQUEST_TEMPLATE.md", [
    "These boxes ARE the publish/merge gates",
    "`$pr-hygiene` clean — content gate @ <full-40-hex-sha>",
  ]],
  ["docs/FEATURES.md", [
    "Runner-neutral agent policy and five SHA-bound PR gates",
    "publishing personal/private identifiers or non-English PR/docs content",
  ]],
])) {
  let text;
  try { text = normalizeProse(fs.readFileSync(repoPath(relative), "utf8")); }
  catch (error) { die(1, `${relative} is unreadable: ${error.message}`); }
  if (contracts.some((required) => !text.includes(required))) {
    die(1, `${relative} is missing a PR-policy contract`);
  }
}

regularFile(".agents/subagents.json", "canonical subagents manifest");
const subagentsDoc = readJson(repoPath(".agents/subagents.json"), "subagents manifest");
if (!subagentsDoc || !Array.isArray(subagentsDoc.subagents)) {
  die(1, ".agents/subagents.json must contain a subagents array");
}
const canonicalReviewerTypes = [
  "agent_config_reviewer",
  "doc_drift_checker",
  "heap_safety_reviewer",
  "multi_target_build_reviewer",
];
const actualReviewerTypes = subagentsDoc.subagents
  .map((s) => s.TypeName)
  .sort();
if (canonicalReviewerTypes.join("\0") !== actualReviewerTypes.join("\0")) {
  die(1, "canonical reviewer set differs from manifest");
}
if (actualReviewerTypes.length !== 4) {
  die(1, `expected four canonical reviewers, got ${actualReviewerTypes.length}`);
}
for (const subagent of subagentsDoc.subagents) {
  if (subagent.SandboxMode !== "read-only") {
    die(1, `${subagent.TypeName} sandbox_mode must be read-only`);
  }
  if (!subagent.Prompt || typeof subagent.Prompt !== "string" || !subagent.Prompt.trim()) {
    die(1, `${subagent.TypeName} has empty prompt`);
  }
}
const multiTargetReviewerObj = subagentsDoc.subagents.find((s) => s.TypeName === "multi_target_build_reviewer");
const multiTargetReviewer = normalizeProse(multiTargetReviewerObj?.Prompt || "");
const multiTargetPublicationContracts = [
  "prepare -> logic-test + logic-harness + build-target + independent-rebuild-target (concurrent) -> build -> independent-rebuild -> publish -> deploy",
  "build needs prepare, logic-test, logic-harness and build-target",
  "independent-rebuild needs build and every independent-rebuild-target leg",
  "SHA/version-bound Actions artifact",
  "deploy consumes only that named artifact",
  "without a signing Environment, OTA key or OIDC",
];
if (multiTargetPublicationContracts.some((required) => !multiTargetReviewer.includes(required))) {
  die(1, "multi-target reviewer is missing the independent-rebuild/publication DAG contract");
}
if (teslaBlePinMatch) {
  const currentPin = teslaBlePinMatch[1];
  if (!multiTargetReviewer.includes(`yoziru/tesla-ble remains ${currentPin}`)) {
    die(1, `multi-target reviewer pin assertion does not match idf_component.yml pin (${currentPin})`);
  }
  const docDriftReviewerObj = subagentsDoc.subagents.find((s) => s.TypeName === "doc_drift_checker");
  const docDriftReviewer = normalizeProse(docDriftReviewerObj?.Prompt || "");
  if (!docDriftReviewer.includes(`yoziru/tesla-ble ${currentPin}`)) {
    die(1, `doc drift checker pin assertion does not match idf_component.yml pin (${currentPin})`);
  }
}

const safety = readJson(repoPath("tools/agent-config/safety-invariants.json"), "safety invariants");
if (safety?.schema_version !== 1 || !Array.isArray(safety.invariants) || safety.invariants.length === 0) {
  die(2, "safety invariants need schema_version 1 and a non-empty invariants array");
}
const instructionTexts = new Map([
  ["AGENTS.md", fs.readFileSync(repoPath("AGENTS.md"), "utf8")],
]);
const invariantIds = new Set();
for (const invariant of safety.invariants) {
  if (!invariant || typeof invariant.id !== "string" || invariantIds.has(invariant.id)) {
    die(2, "safety invariant ids must be non-empty and unique");
  }
  invariantIds.add(invariant.id);
  let pattern;
  try { pattern = new RegExp(invariant.pattern, "imu"); }
  catch (error) { die(2, `invalid safety invariant ${invariant.id}: ${error.message}`); }
  for (const [file, text] of instructionTexts) {
    if (!pattern.test(text)) die(1, `safety invariant ${invariant.id} is missing from ${file}`);
  }
}

try {
  execFileSync(process.execPath, [repoPath("tools/agent-config/sync-codex.mjs"), "--check"], {
    env: { ...process.env, AGENT_CONFIG_ROOT: root }, stdio: "inherit",
  });
} catch { die(1, "native Codex registration check failed"); }
console.log(`agent-config: ${canonicalSkills.length} canonical skills, ${actualReviewerTypes.length} reviewers and ${invariantIds.size} instruction invariants clean`);
console.log(`agent-config: AGENTS.md budget ${agentsSize}/${budget} bytes`);
