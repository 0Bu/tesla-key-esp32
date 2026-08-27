#!/usr/bin/env node
// Deterministic project-agent instruction and safety contract.
import fs from "node:fs";
import path from "node:path";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";

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
  die(1, ".claude metadata must remain retired; use AGENTS.md, .agents, .codex and tools/agent-hooks");
}
if (fs.existsSync(repoPath(".codex/migration-manifest.json"))) {
  die(1, ".codex/migration-manifest.json must remain retired with the compatibility layer");
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

const highRiskSkills = new Set(["flash-esp32", "ship", "usb-recovery"]);
const readOnlySkills = new Set([
  "device-diag", "display-preview", "ota-release-verify", "pr-hygiene", "project-review",
  "skill-audit", "vehicle-command-audit",
]);
const ownerContracts = new Map([
  ["project-review", [
    "[`AGENTS.md`](../../../AGENTS.md) owns runner policy, authorization, safety, evidence, build, and review contracts.",
    "[`docs/README.md`](../../../docs/README.md) owns hardware, HTTP API, and commands.",
    "[`docs/ARCHITECTURE.md`](../../../docs/ARCHITECTURE.md) owns telemetry, MQTT, sleep/link state, pairing, and OTA.",
    "[`docs/MCP.md`](../../../docs/MCP.md) owns MCP tools.",
    "[`docs/SECURITY.md`](../../../docs/SECURITY.md) owns NVS, signing, and exposure.",
  ]],
  ["device-diag", [
    "[`docs/ARCHITECTURE.md`](../../../docs/ARCHITECTURE.md) owns pairing lifecycle and invalidation.",
  ]],
  ["vehicle-command-audit", [
    "[`docs/README.md`](../../../docs/README.md) owns the HTTP command catalog.",
    "[`docs/ARCHITECTURE.md`](../../../docs/ARCHITECTURE.md) owns link-state and pairing semantics.",
    "[`docs/MCP.md`](../../../docs/MCP.md) owns MCP tools.",
    "`AGENTS.md` owns only runner policy and safety boundaries.",
  ]],
]);
const deepOwnerTerms = /(?:HTTP|\bAPI\b|(?<![-/])\bcommands?\b|\bMCP\b|\bNVS\b|\bMQTT\b|link-state|pairing)/i;
const usbPositiveAuthorization = /USB-write approval[^.!?]*(?:also\s+)?(?:authorizes|allows|permits|covers|includes|is sufficient for)[^.!?]*(?:live verification|HTTP|GET)/i;
const usbNoApprovalNeeded = /(?:live verification|HTTP requests?|GET endpoints?)[^.!?]*(?:without (?:separate )?(?:approval|authorization)|requires? no (?:approval|authorization)|need not (?:be )?(?:approved|authorized))/i;
const usbOtaNotStateChanging = /GET \/ota\/check[^.!?]*(?:is not|isn't|not) state-changing/i;
const usbAbsentApprovalProceeds = /(?:(?:approval|authorization)[^.!?]*(?:absent|missing|not obtained)|without (?:separate )?(?:approval|authorization))[^.!?]*(?:continue|proceed|run|contact|send|request)/i;
const reviewedSkillSha256 = new Map([
  ["add-logic-test", "f8a37fddbbb5c47afa9eca4fb4823c203af099718b3327a656c717d0462546f7"],
  ["device-diag", "7babf410873975ec05bb029c3c9522e70f9aadd96d0823829c48236f24ca3d44"],
  ["display-preview", "4bff95d0314d50ce29d67beac7ef4f9db1ebcbb2fa609335e560e162f5a1ed46"],
  ["feature-docs", "c2d26e873399d4970145ed53ab64e5e3abfd198391343ad107ec1dbdf10012ff"],
  ["flash-esp32", "cd67535f6206b72eb824548fce9338f97c5e813aff14633c6149b636b2146aeb"],
  ["ota-release-verify", "347c7f3cdffa25a9563f104c099e08d1f91101e2d54b39442b97e9fcbc77400b"],
  ["pr-hygiene", "7ef6544f83a50dbe696e360081c33091ce8d7f0826ec839efd7c4805cdf2344a"],
  ["project-review", "1cb0f6c4f5df556743c9a32670662ae51e76bc4b13d823a8f682ab8693d18d2d"],
  ["ship", "41ae3355c7b2d24624d92a5c23666b18361083f8ef305faf43b57303a9f20275"],
  ["skill-audit", "231af57cfccedb5e16d2a5196b2d9857d34a3098b5e74885f37f51a7590d684a"],
  ["usb-recovery", "6f3cbd9533e75d14b5a14cd19987fa07db046b6c5412f4d52d2aa54944481cf9"],
  ["vehicle-command-audit", "3921ad8810c7804caf145dbee2c2571373dca712c0488a9c77dd7746b19c691a"],
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
      "Before any HTTP request, obtain separate explicit user approval for the exact recovered device/IP and the named GET endpoints.",
      "`GET /ota/check` is state-changing and must be named explicitly in that live approval.",
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
    "These four boxes ARE the publish/merge gates",
    "`$pr-hygiene` clean — content gate @ <full-40-hex-sha>",
  ]],
  ["docs/FEATURES.md", [
    "Runner-neutral agent policy and four SHA-bound PR gates",
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

const canonicalReviewerTargets = [
  ".codex/agents/agent_config_reviewer.toml",
  ".codex/agents/doc_drift_checker.toml",
  ".codex/agents/heap_safety_reviewer.toml",
  ".codex/agents/multi_target_build_reviewer.toml",
];
let actualReviewerTargets;
try {
  actualReviewerTargets = fs.readdirSync(repoPath(".codex/agents"), { withFileTypes: true })
    .filter((entry) => entry.isFile() && entry.name.endsWith(".toml"))
    .map((entry) => `.codex/agents/${entry.name}`).sort();
} catch { die(1, ".codex/agents is missing"); }
if (canonicalReviewerTargets.join("\0") !== actualReviewerTargets.join("\0")) {
  die(1, "canonical reviewer set differs from manifest");
}
if (actualReviewerTargets.length !== 4) {
  die(1, `expected four canonical reviewers, got ${actualReviewerTargets.length}`);
}
const multiTargetReviewer = normalizeProse(
  fs.readFileSync(repoPath(".codex/agents/multi_target_build_reviewer.toml"), "utf8"),
);
const multiTargetPublicationContracts = [
  "logic-test -> build -> independent-rebuild -> publish -> deploy",
  "SHA/version-bound Actions artifact",
  "deploy consumes only that named artifact",
  "without a signing Environment, OTA key or OIDC",
];
if (multiTargetPublicationContracts.some((required) => !multiTargetReviewer.includes(required))) {
  die(1, "multi-target reviewer is missing the independent-rebuild/publication DAG contract");
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

console.log(`agent-config: ${canonicalSkills.length} canonical skills, ${actualReviewerTargets.length} reviewers and ${invariantIds.size} instruction invariants clean`);
console.log(`agent-config: AGENTS.md budget ${agentsSize}/${budget} bytes`);
