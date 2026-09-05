#!/usr/bin/env node
// Deterministic skill-digest verification and updater for tools/agent-config/check.mjs.
import fs from "node:fs";
import path from "node:path";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";

function die(code, message) {
  console.error(`update-skill-digests: ${message}`);
  process.exit(code);
}

const defaultRoot = path.resolve(
  process.env.AGENT_CONFIG_ROOT || path.join(path.dirname(fileURLToPath(import.meta.url)), "../.."),
);

export function computeSkillDigests(rootDir) {
  const skillsDir = path.join(rootDir, ".agents", "skills");
  if (!fs.existsSync(skillsDir)) die(1, `skills directory missing: ${skillsDir}`);
  const entries = fs.readdirSync(skillsDir, { withFileTypes: true })
    .filter((e) => e.isDirectory())
    .map((e) => e.name)
    .sort();
  const digests = new Map();
  for (const name of entries) {
    const skillFile = path.join(skillsDir, name, "SKILL.md");
    if (!fs.existsSync(skillFile)) die(1, `SKILL.md missing for ${name}`);
    const content = fs.readFileSync(skillFile, "utf8");
    const digest = createHash("sha256").update(content).digest("hex");
    digests.set(name, digest);
  }
  return digests;
}

export function parseCheckMjsDigests(checkMjsPath) {
  const content = fs.readFileSync(checkMjsPath, "utf8");
  const match = content.match(/const reviewedSkillSha256 = new Map\(\[\n([\s\S]*?)\n\]\);/);
  if (!match) die(1, `cannot find reviewedSkillSha256 in ${checkMjsPath}`);
  const lines = match[1].split("\n");
  const digests = new Map();
  for (const line of lines) {
    const entryMatch = line.match(/^\s*\["([^"]+)",\s*"([0-9a-f]{64})"\]/);
    if (entryMatch) {
      digests.set(entryMatch[1], entryMatch[2]);
    }
  }
  return { content, digests, rawBlock: match[0] };
}

export function generateDigestsBlock(digestsMap) {
  const sortedNames = [...digestsMap.keys()].sort();
  const lines = ["const reviewedSkillSha256 = new Map(["];
  for (const name of sortedNames) {
    lines.push(`  ["${name}", "${digestsMap.get(name)}"],`);
  }
  lines.push("]);");
  return lines.join("\n");
}

export function updateCheckMjs(rootDir, dryRun = false) {
  const checkMjsPath = path.join(rootDir, "tools", "agent-config", "check.mjs");
  if (!fs.existsSync(checkMjsPath)) die(1, `check.mjs missing: ${checkMjsPath}`);
  const actualDigests = computeSkillDigests(rootDir);
  const { content, digests: parsedDigests, rawBlock } = parseCheckMjsDigests(checkMjsPath);
  let changed = false;
  for (const [name, hash] of actualDigests) {
    if (parsedDigests.get(name) !== hash) {
      changed = true;
      break;
    }
  }
  if (!changed && actualDigests.size === parsedDigests.size) {
    return { changed: false, count: actualDigests.size };
  }
  const newBlock = generateDigestsBlock(actualDigests);
  if (!dryRun) {
    const updatedContent = content.replace(rawBlock, newBlock);
    fs.writeFileSync(checkMjsPath, updatedContent, "utf8");
  }
  return { changed: true, count: actualDigests.size, newBlock };
}

function runSelfTest() {
  const tmp = fs.mkdtempSync(path.join("/tmp", "skill-digests-selftest-"));
  try {
    const skillsDir = path.join(tmp, ".agents", "skills", "demo");
    fs.mkdirSync(skillsDir, { recursive: true });
    fs.writeFileSync(path.join(skillsDir, "SKILL.md"), "demo content\n", "utf8");
    const checkDir = path.join(tmp, "tools", "agent-config");
    fs.mkdirSync(checkDir, { recursive: true });
    const dummyCheckMjs = [
      '// header',
      'const reviewedSkillSha256 = new Map([',
      '  ["demo", "0000000000000000000000000000000000000000000000000000000000000000"],',
      ']);',
      '// footer',
    ].join("\n");
    fs.writeFileSync(path.join(checkDir, "check.mjs"), dummyCheckMjs, "utf8");

    const check1 = updateCheckMjs(tmp, true);
    if (!check1.changed) die(1, "self-test failed: expected drift not detected");

    const update = updateCheckMjs(tmp, false);
    if (!update.changed) die(1, "self-test failed: write did not update");

    const check2 = updateCheckMjs(tmp, true);
    if (check2.changed) die(1, "self-test failed: check after write detected drift");

    console.log("update-skill-digests: self-test PASS");
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
}

const args = process.argv.slice(2);
if (args.includes("--self-test")) {
  runSelfTest();
  process.exit(0);
}

const isWrite = args.includes("--write");
const isCheck = args.includes("--check") || !isWrite;

const result = updateCheckMjs(defaultRoot, !isWrite);
if (isCheck && !isWrite) {
  if (result.changed) {
    console.error("update-skill-digests: skill digests drifted. Run with --write to update.");
    process.exit(1);
  } else {
    console.log(`update-skill-digests: all ${result.count} canonical skill digests clean`);
    process.exit(0);
  }
}

if (isWrite) {
  if (result.changed) {
    console.log(`update-skill-digests: successfully updated ${result.count} skill digests in check.mjs`);
  } else {
    console.log(`update-skill-digests: all ${result.count} skill digests already up to date`);
  }
}
