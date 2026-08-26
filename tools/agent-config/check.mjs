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
  ["feature-docs", "43c1db745640619aea155b2e34b8fb0127b1d681b936294bffdfd8422566bb46"],
  ["flash-esp32", "6ead7b8f1732c66285f16c8427ee952043ad8d730001fd76c90c8d0ed01fc99e"],
  ["ota-release-verify", "3b0c7941871b9c350bf64a046e3344ac3b5effa2228daf375428b97e0168a511"],
  ["pr-hygiene", "addfebbad2a6e7717f34acccaafd765fee02f1b05aa3735e84e6dfff61c4af5f"],
  ["project-review", "3bee267fe77b7ef047e73acb47991643d2a522e980ae98e2980b713215cfc0b3"],
  ["ship", "604da450727cb85e56c7cbc7ac6924b6378e7bb15a2ef4a5fd93983588af6ed1"],
  ["skill-audit", "bf3b11bc0bfaa9d29c7e261a549b9bbb94a429f19468648afe42d50c3a42f6ce"],
  ["usb-recovery", "ed31dfbbf41a7a155eb73ec80e09c23338049a3ca7ac4754bdfdff960e733c24"],
  ["vehicle-command-audit", "30ab4506d71e11d3993d66ca566402e024d12ae7502fd2b0bb056778cdde191a"],
]);
const featureDocsScopeTokens = [
  "main/", "test/", "sdkconfig.defaults*", "partitions.csv", "AGENTS.md", ".agents/", ".codex/",
  ".github/PULL_REQUEST_TEMPLATE.md",
  "tools/agent-hooks/", "tools/agent-config/", "docs/index.html", "installer-bootstrap.mjs",
  "serial-port-release.mjs", "web-installer.mjs", "docs/vendor/",
  "build,signed-pr-preview,pr-preview-cleanup", "scripts/release-relevance.sh",
];
const prHygieneContracts = [
  "### `PRIVACY-LEAK`", "### `LANGUAGE`", "at PR creation, every push, and merge",
  "192.0.2.0/24", "198.51.100.0/24", "203.0.113.0/24", "2001:db8::/32",
  "- [x] `$pr-hygiene` clean — content gate @ <short-sha>",
];
const shipMergeGateContracts = [
  "- [x] $project-review clean — merge gate @ <sha>",
  "- [x] $pr-hygiene clean — content gate @ <sha>",
  "- [x] $feature-docs synced — merge gate @ <sha>",
  "`$project-review` does not establish `$pr-hygiene` readiness",
];
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
    "`$pr-hygiene` clean — content gate @ <sha>",
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
