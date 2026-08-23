#!/usr/bin/env node
// Deterministic runner-neutral instruction, mapping and safety contract.
import fs from "node:fs";
import path from "node:path";
import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
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

function safeRelative(value, label) {
  if (typeof value !== "string" || !value || path.isAbsolute(value)) {
    die(2, `${label} must be a non-empty repository-relative path`);
  }
  const normalized = value.replaceAll("\\", "/");
  if (normalized.startsWith("./") || normalized.includes("//") || normalized.split("/").includes("..")) {
    die(2, `${label} is not normalized: ${value}`);
  }
  return normalized;
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
const manifestPath = repoPath(".codex/migration-manifest.json");
const budget = positiveInteger(process.env.AGENT_INSTRUCTIONS_BUDGET_BYTES || "24576", "AGENTS budget");
const fingerprintFormat =
  "sha256(path-utf8 + NUL + raw-bytes + NUL for each tracked .claude file in bytewise path order)";

function regularFile(relative, label) {
  let stat;
  try { stat = fs.statSync(repoPath(relative)); }
  catch { die(1, `${label} is missing: ${relative}`); }
  if (!stat.isFile()) die(1, `${label} is not a regular file: ${relative}`);
}

function trackedLegacyFiles() {
  if (process.env.AGENT_CONFIG_TRACKED_FILES_FILE) {
    try {
      return fs.readFileSync(process.env.AGENT_CONFIG_TRACKED_FILES_FILE, "utf8")
        .split(/\r?\n/).filter(Boolean);
    } catch (error) { die(2, `cannot read tracked-file fixture: ${error.message}`); }
  }
  const result = spawnSync("git", ["-C", root, "ls-files", "-z", "--", ".claude"], {
    encoding: "utf8",
  });
  if (result.status !== 0) die(2, "git ls-files could not enumerate tracked .claude files");
  return result.stdout.split("\0").filter(Boolean);
}

const manifest = readJson(manifestPath, "migration manifest");
if (manifest?.schema_version !== 1 || manifest?.legacy_root !== ".claude") {
  die(2, "migration manifest needs schema_version 1 and legacy_root .claude");
}
if (manifest?.canonical_instructions !== "AGENTS.md") {
  die(2, "migration manifest canonical_instructions must be AGENTS.md");
}
if (manifest?.legacy_tree_sha256_format !== fingerprintFormat) {
  die(2, `legacy_tree_sha256_format must be: ${fingerprintFormat}`);
}
if (!/^[0-9a-f]{64}$/.test(manifest?.legacy_tree_sha256 || "")) {
  die(2, "legacy_tree_sha256 must be a lowercase SHA-256");
}
if (!Array.isArray(manifest.entries) || manifest.entries.length === 0) {
  die(2, "migration manifest entries must be a non-empty array");
}

const allowedStatuses = new Set(["canonical", "adapter", "deprecated"]);
const mappings = new Map();
for (let index = 0; index < manifest.entries.length; index++) {
  const entry = manifest.entries[index];
  if (!entry || typeof entry !== "object" || Array.isArray(entry)) die(2, `entry ${index} is invalid`);
  const source = safeRelative(entry.source, `entry ${index} source`);
  if (!source.startsWith(".claude/")) die(2, `manifest source is outside .claude: ${source}`);
  if (mappings.has(source)) die(1, `migration manifest maps ${source} more than once`);
  if (!allowedStatuses.has(entry.status)) die(2, `invalid mapping status for ${source}`);
  if (!Array.isArray(entry.targets)) die(2, `targets for ${source} must be an array`);
  if (entry.status !== "deprecated" && entry.targets.length === 0) {
    die(1, `${entry.status} mapping has no target: ${source}`);
  }
  const targets = entry.targets.map((target, targetIndex) =>
    safeRelative(target, `${source} target ${targetIndex}`));
  if (new Set(targets).size !== targets.length) die(1, `${source} repeats a target`);
  regularFile(source, "manifest source");
  for (const target of targets) regularFile(target, `target for ${source}`);
  mappings.set(source, { status: entry.status, targets });
}

const tracked = trackedLegacyFiles();
if (new Set(tracked).size !== tracked.length) die(2, "tracked .claude enumeration has duplicates");
for (const source of tracked) {
  if (!mappings.has(source)) die(1, `tracked legacy file is absent from migration manifest: ${source}`);
}
for (const source of mappings.keys()) {
  if (!tracked.includes(source)) die(1, `manifest source is not a tracked legacy file: ${source}`);
}

const bytewise = [...tracked].sort((left, right) =>
  Buffer.compare(Buffer.from(left, "utf8"), Buffer.from(right, "utf8")));
const fingerprint = createHash("sha256");
for (const source of bytewise) {
  fingerprint.update(Buffer.from(source, "utf8"));
  fingerprint.update(Buffer.from([0]));
  fingerprint.update(fs.readFileSync(repoPath(source)));
  fingerprint.update(Buffer.from([0]));
}
const actualFingerprint = fingerprint.digest("hex");
if (actualFingerprint !== manifest.legacy_tree_sha256) {
  die(1, `reviewed legacy fingerprint drifted (expected ${manifest.legacy_tree_sha256}, got ${actualFingerprint})`);
}

const instructionMapping = mappings.get(".claude/CLAUDE.md");
if (instructionMapping?.status !== "adapter" || !instructionMapping.targets.includes("AGENTS.md")) {
  die(1, ".claude/CLAUDE.md must be an adapter mapped to AGENTS.md");
}
regularFile("AGENTS.md", "canonical instructions");
const agentsSize = fs.statSync(repoPath("AGENTS.md")).size;
if (agentsSize > budget) die(1, `AGENTS.md is ${agentsSize} bytes, over the ${budget}-byte budget`);

const skillMappings = new Map();
const reviewerMappings = new Map();
for (const [source, mapping] of mappings) {
  let match = source.match(/^\.claude\/skills\/([^/]+)\/SKILL\.md$/);
  if (match) {
    const expected = `.agents/skills/${match[1]}/SKILL.md`;
    if (mapping.status !== "canonical" || !mapping.targets.includes(expected)) {
      die(1, `skill ${match[1]} must map canonically to ${expected}`);
    }
    skillMappings.set(match[1], { source, target: expected });
  }
  match = source.match(/^\.claude\/agents\/([^/]+)\.md$/);
  if (match) {
    const target = mapping.targets.find((candidate) => /^\.codex\/agents\/[^/]+\.toml$/.test(candidate));
    if (mapping.status !== "canonical" || !target) {
      die(1, `legacy reviewer ${source} needs one canonical .codex/agents TOML target`);
    }
    reviewerMappings.set(match[1], { source, target });
  }
}

function directoryNames(relative) {
  try {
    return fs.readdirSync(repoPath(relative), { withFileTypes: true })
      .filter((entry) => entry.isDirectory()).map((entry) => entry.name).sort();
  } catch { die(1, `directory is missing: ${relative}`); }
}

const mappedSkills = [...skillMappings.keys()].sort();
for (const [label, names] of [
  ["legacy", directoryNames(".claude/skills")],
  ["canonical", directoryNames(".agents/skills")],
]) {
  if (names.join("\0") !== mappedSkills.join("\0")) {
    die(1, `${label} skill set differs from manifest (expected ${mappedSkills.join(", ")}; got ${names.join(", ")})`);
  }
}

const highRiskSkills = new Set(["e2e-evcc", "flash-esp32", "ship", "usb-recovery"]);
const readOnlySkills = new Set([
  "device-diag", "display-preview", "ota-release-verify", "project-review",
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
  ["add-logic-test", {canonical: "f8a37fddbbb5c47afa9eca4fb4823c203af099718b3327a656c717d0462546f7", legacy: "aaa50473b6877c8998fe6d9af4604c324c63ffa614872ce72fb0a006abdd0a9e"}],
  ["device-diag", {canonical: "0324d1c4ce9b8ef51647b26860754761fa370a9e2dd6ecd4a6adedfe0314027e", legacy: "27017438cde102b513b119171a1988aaf3e1079f2676f0d62a70e26d451edc43"}],
  ["display-preview", {canonical: "4bff95d0314d50ce29d67beac7ef4f9db1ebcbb2fa609335e560e162f5a1ed46", legacy: "486c6a3b7a7bdb86a587ed97f2368e687524e02e58f0adba4c052376eb23d0d5"}],
  ["e2e-evcc", {canonical: "04489835e6266dec020f4c5aa6663d228d47c5b880dcf6928379312b00c444cc", legacy: "8f40271c3fad7da834ed4cec4f231761af523cacf1ca14f8acb2f024a19a47dc"}],
  ["feature-docs", {canonical: "1f2851768ed02689d0a0613b644153b111e2f8f45d7e4fc2b60f39b0d01c6a6a", legacy: "a34447175d62ae1be15a9fbcf5f81af543566a1d8ead2accfa73da3e68597cb6"}],
  ["flash-esp32", {canonical: "d9945b914aa48ef7c150e207fc268011677f54ef6acde837c09111cf3229b71b", legacy: "d77312d51aa6d159b8e6a694e14f1bfaffb2709b8588a7bf51a0ac160fe2b84b"}],
  ["ota-release-verify", {canonical: "3b0c7941871b9c350bf64a046e3344ac3b5effa2228daf375428b97e0168a511", legacy: "074c330c0f1d9676922f8dbce4605d2674c021013cb5125b2c595eb6a85bec66"}],
  ["project-review", {canonical: "c64c821e44221460d66e29e7af1c23bfd0c4ebcb2cb31ca7746eb243ab3381cb", legacy: "0e754dffdbdfc6998b55895bc6d120706a02f48a76a679516752083ae856a9e9"}],
  ["ship", {canonical: "6424efe4f169b067ad407e26960b71a0765888ecd765152e11d01ba2db74bed7", legacy: "bebfbfa087093cd4e3914f306a67483e99f8beeb49c2479ee1ca8d1c9382a0c5"}],
  ["skill-audit", {canonical: "ce322222e17344f254ab89c86b445b0cf608d4ae09f94c62b3507700ee382806", legacy: "14514707fba12dfe961252632657e7319f1d8816ee6f15b66888f828dea75d76"}],
  ["usb-recovery", {canonical: "25b5d04627894c1d5bb4fea816cc81624efc540bdfc85a1229772b7faf689e32", legacy: "bc2232300747fcac0c77f4dc706bc72c640c2b42079adacf09e56478bca03af6"}],
  ["vehicle-command-audit", {canonical: "30ab4506d71e11d3993d66ca566402e024d12ae7502fd2b0bb056778cdde191a", legacy: "144feba94438891a66f96b545db0f3284c57db24a29f23e2ca9282b2d5c60072"}],
]);
const featureDocsScopeTokens = [
  "main/", "test/", "sdkconfig.defaults*", "partitions.csv", "AGENTS.md", ".agents/", ".codex/",
  "tools/agent-hooks/", "tools/agent-config/", "docs/index.html", "installer-bootstrap.mjs",
  "serial-port-release.mjs", "web-installer.mjs", "docs/vendor/",
  "build,signed-pr-preview,pr-preview-cleanup", "scripts/release-relevance.sh",
];
for (const [name, pair] of skillMappings) {
  const legacy = restrictedFrontmatter(repoPath(pair.source), `legacy skill ${name}`);
  const canonical = restrictedFrontmatter(repoPath(pair.target), `canonical skill ${name}`);
  if (legacy.values.get("name") !== name || canonical.values.get("name") !== name) {
    die(1, `skill ${name} frontmatter name mismatch`);
  }
  if (!legacy.values.get("description") || !canonical.values.get("description")) {
    die(1, `skill ${name} needs descriptions on both compatibility sides`);
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
    for (const [side, text] of [["canonical", canonical.text], ["legacy", legacy.text]]) {
      const normalized = normalizeProse(text);
      if (liveContracts.some((required) => !normalized.includes(required))) {
        die(1, `${side} usb-recovery skill is missing the exact live-verification contract`);
      }
      const contradiction = proseSentences(text).some((sentence) =>
        usbPositiveAuthorization.test(sentence) || usbNoApprovalNeeded.test(sentence) ||
        usbOtaNotStateChanging.test(sentence) || usbAbsentApprovalProceeds.test(sentence)
      );
      if (contradiction) {
        die(1, `${side} usb-recovery skill contradicts the live-verification contract`);
      }
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
  const requiredOwners = ownerContracts.get(name);
  if (requiredOwners) {
    const normalized = normalizeProse(canonical.text);
    if (requiredOwners.some((required) => !normalized.includes(required))) {
      die(1, `${name} is missing an exact documentation-owner contract`);
    }
    for (const [side, text] of [["canonical", canonical.text], ["legacy", legacy.text]]) {
      const contradictoryOwner = proseSentences(text).some((sentence) =>
        /AGENTS\.md/i.test(sentence) && deepOwnerTerms.test(sentence)
      );
      if (contradictoryOwner) {
        die(1, `${side} ${name} assigns a deep technical catalog to compact AGENTS.md`);
      }
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
  const expectedDigests = reviewedSkillSha256.get(name);
  if (!expectedDigests) die(1, `skill ${name} has no reviewed content digest`);
  for (const [side, text] of [["canonical", canonical.text], ["legacy", legacy.text]]) {
    const digest = createHash("sha256").update(text).digest("hex");
    if (digest !== expectedDigests[side]) {
      die(1, `${side} ${name} exact reviewed content contract drifted`);
    }
  }
}

const canonicalReviewerTargets = [...reviewerMappings.values()].map((pair) => pair.target).sort();
let actualReviewerTargets;
try {
  actualReviewerTargets = fs.readdirSync(repoPath(".codex/agents"), { withFileTypes: true })
    .filter((entry) => entry.isFile() && entry.name.endsWith(".toml"))
    .map((entry) => `.codex/agents/${entry.name}`).sort();
} catch { die(1, ".codex/agents is missing"); }
if (canonicalReviewerTargets.join("\0") !== actualReviewerTargets.join("\0")) {
  die(1, "canonical reviewer set differs from manifest");
}
if (reviewerMappings.size !== 4) die(1, `expected four mapped reviewers, got ${reviewerMappings.size}`);

const safety = readJson(repoPath("tools/agent-config/safety-invariants.json"), "safety invariants");
if (safety?.schema_version !== 1 || !Array.isArray(safety.invariants) || safety.invariants.length === 0) {
  die(2, "safety invariants need schema_version 1 and a non-empty invariants array");
}
const instructionTexts = new Map([
  ["AGENTS.md", fs.readFileSync(repoPath("AGENTS.md"), "utf8")],
  [".claude/CLAUDE.md", fs.readFileSync(repoPath(".claude/CLAUDE.md"), "utf8")],
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

console.log(`agent-config: ${tracked.length} legacy mappings, ${skillMappings.size} skill pairs, ${reviewerMappings.size} reviewers and ${invariantIds.size} cross-runner invariants clean`);
console.log(`agent-config: AGENTS.md budget ${agentsSize}/${budget} bytes`);
