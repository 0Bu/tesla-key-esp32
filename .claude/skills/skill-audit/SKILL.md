---
name: skill-audit
description: Dedicated drift audit of every skill AND agent under .claude/ against the tesla-key-esp32 project — confirms each still maps the code/config/scripts that exist and corrects any that contradicts them, in place. This is the PR-gated skills-coherence check (require-skill-audit.sh blocks opening a PR and every push to it until it has run against the current tree). Use whenever you ask to "check the skills", "audit the skills/agents", "are the skills still in sync / complete", "did a skill drift", after changing anything under .claude/ or after a batch of code changes, or before opening/pushing a PR. Narrower and faster than project-review (which audits the whole firmware and gates the merge); skill-audit is the skills/agents subset — a full /project-review also satisfies this gate.
---

# skill-audit — keep every skill & agent in sync with the project

The `.claude/skills/*/SKILL.md` files **and** the review agents in `.claude/agents/*.md` are
documents that **drift**: each lags the project by exactly the changes landed since it was last
touched. A skill that names a wrong partition offset, a stale command count, a removed endpoint,
a renamed script, or a superseded target set silently mis-teaches every future session that
trusts it. This skill's one job is to **catch and fix that drift** so no skill/agent ships out of
sync with the code, config, scripts and docs it describes.

It is the **skills/agents subset of `project-review`**, pulled out as its own fast, PR-gated
check: `require-skill-audit.sh` refuses to open a PR or push to it until a skill-audit has run
against the current tree (see *The PR gate* below). `project-review` remains the deep
whole-firmware audit that gates the *merge*, and **also** covers the skills — so a full
`/project-review` records this gate's marker too. Run
`/skill-audit` when only the skills/agents need re-checking; run `/project-review` when the code,
invariants or docs might have moved.

## What counts as drift (and what does not)

Drift is measured against the **project**, never against a skill's own wording. A finding
(`SKILL-DRIFT`) exists only where a skill or agent **contradicts** a ground-truth fact in the
code / config / script / doc:

- a **wrong number** — partition offset, flash size, slot size, command count, fragment size,
  timeout/debounce constant, a version or library pin;
- a **removed or renamed** thing — an endpoint, command, MCP tool, NVS key, Kconfig option,
  file path, script name, function;
- a **stale set** — the target list (esp32/s3/c3/c6/c5), the OTA `<suffix>` map, the enrolled role;
- a **broken pointer** — a skill that documents a script/hook whose behaviour no longer matches
  the file that actually runs, or an agent boundary that points at a skill that changed.

**Not drift** (do not touch): prose style, wording preferences, ordering, "could be clearer".
Polishing has no ground truth, so it never converges. The test for a legitimate edit: **name the
project fact it now matches.** If you can't — or the edit wouldn't survive the next audit
unchanged — it is churn; leave it.

## How to run the audit

Work in this order — it is a **single pass**: enumerate → check → fix → report → record.

1. **Enumerate — discover, don't hardcode.** `ls .claude/skills/*/SKILL.md` (skills, incl. this
   one and `project-review`) **and** `ls .claude/agents/*.md` (agents). Every file in that list
   must be audited; a skill/agent added since this doc was written is audited too, and appended
   to the *Per-target checklist* below. Also inventory what they describe:
   `ls .claude/hooks/*.sh` and skim `.claude/settings.json` (the hook wiring), `scripts/`,
   `main/`, `partitions.csv`, `main/idf_component.yml`, `version.txt`.
2. **Extract each doc's concrete claims.** For every skill/agent, list the checkable facts it
   asserts — numbers, paths, counts, flags, target set, script names, the behaviour it ascribes
   to a script or hook. These are your assertions.
3. **Verify each claim against the tree.** `grep`/`Read` the code, config or script it names and
   confirm the fact still holds. A `git`/host check beats a guess: `scripts/run-mock-tests.sh`
   actually runs the pure-logic a skill might cite; `grep` finds the other half of a cross-cutting
   link. Use parallel reads/`Explore` to move fast.
4. **Correct contradictions in place.** Fix a drifted skill/agent exactly as you'd fix any doc —
   change only the contradicting fact, minimally, without introducing a new contradiction. Do
   **not** reword or restyle. Do **not** re-open a file you already corrected in this pass.
5. **Report** in the structure below — every skill/agent gets a ✓ or a `SKILL-DRIFT` line, even
   the ones you didn't change (so the pass is auditable).
6. **Record the pass so the PR gate clears** (see *The PR gate*). Only if no contradiction
   remains: `touch .claude/.skill-audit-passed`. If drift remains unfixed, don't record it — fix
   first.

### Termination — the audit converges, it does not loop

This mirrors `project-review`'s convergence contract, and for the same reason: the tool that
prevents drift must not itself become a churn engine.

- **A skill-audit is one pass** — enumerate → check → fix → report → stop. Nothing here
  re-invokes it; no hook watches a `SKILL.md` to re-trigger. Only a user starts one.
- **Drift is measured against the project, never a skill's own prose.** The fix removes a
  contradiction without creating one, so a re-run converges: every skill == project → zero
  findings → zero edits. The stable state is "skills match the project".
- **Only correct contradictions.** Name the project fact each edit now matches, or don't make it.
- **Don't re-audit within a pass.** Corrected a skill this run? You're done with it; the next
  user-initiated audit re-checks it against the code, like any other change.

## Per-target checklist (what each skill/agent must stay true to)

Discover the list at runtime (step 1); this is the authoritative map of what each current
skill/agent asserts, so you know where to look. Keep it in step with `project-review`'s
*Reviewing the skills* section — the two describe the same siblings and must agree (this skill is
the authority for the per-sibling drift check; `project-review` defers the mechanical part here).

**Skills** (`.claude/skills/`):

- **`project-review`** — the whole-firmware coherence audit. Verify its *Project map* still lists
  every `main/*.{cpp,hpp}` + `main/logic/*.hpp`, its invariants (wake/sleep, link-state, heap,
  OTA, NVS, evcc, pairing, telemetry) still state what the code does, its cross-cutting
  "add X → also update Y" list has no removed/renamed target, and its hardcoded specifics
  (offsets `0x20000`, flash `4 MB`, slot `0x1f0000`, tesla-ble pin, command count) are current.
- **`flash-esp32`** — build + USB-flash path. Verify against `scripts/idf-docker.sh`
  (Docker-pinned, no local IDF), `partitions.csv` (`nvs@0x9000`, `otadata@0xf000`, app `@0x20000`),
  the target set + per-target bootloader offset, and that `@flash_args` never writes `nvs@0x9000`.
- **`e2e-evcc`** — wraps `scripts/e2e_evcc.sh`. Verify the command count (must equal the
  `handle_command` switch — currently **15**), the version-coherence claim (`/status` = `X`,
  `/api/proxy/1/version` = `X-esp32`), the `vehicle_data` fields it asserts, the out-of-scope
  endpoint list, and the env-var gates (`RUN_COMMANDS`/`ALLOW_CHARGE_TOGGLE`/`RUN_ALL_COMMANDS`).
- **`vehicle-command-audit`** — compares the firmware against upstream `teslamotors/vehicle-command`,
  gated by what `yoziru/tesla-ble` can do. Verify the tesla-ble **pin** in its source map
  (`v5.1.1`) still matches `main/idf_component.yml`, its upstream file paths still resolve
  (e.g. `pkg/vehicle/charge.go`), and its "worked findings" don't assert drift already fixed.
- **`add-logic-test`** — scaffolds a `main/logic/` unit + `CHECK`s in `test/test_logic.cpp`.
  Verify against `scripts/run-mock-tests.sh`, the CI `logic-test` job
  (`.github/workflows/build.yml`), the `run-logic-tests.sh` **Stop hook** (`.claude/settings.json`),
  the `CHECK`/`CHECK_STR`/`CHECK_NEAR` macro set, and the `static_assert` lock pattern.
- **`skill-audit`** (this skill) — verify its own numbers/paths (marker name
  `.claude/.skill-audit-passed`, hook `require-skill-audit.sh`, the sibling list, the command
  count `15`, the tesla-ble pin) still match the tree, and that the skills/agents it names still
  exist. Correct it like any other; don't re-open it after.

**Agents** (`.claude/agents/`) — audit these the same way; two duplicate content `project-review`
owns and must stay in sync with it:

- **`doc-drift-checker`** — the fast targeted-diff lens for the cross-cutting links. Its
  "add X → also update Y" enumeration must agree with `project-review`'s *Cross-cutting
  consistency* section.
- **`heap-safety-reviewer`** — the allocation/throw lens. Its heap rules/numbers must match
  `project-review`'s *Memory / heap* invariant and `main.cpp`'s heap-attribution log.
- **`claude-code-optimizer`** — audits the `.claude/` setup, not firmware logic. Confirm its
  boundary still defers firmware correctness to `project-review`, and that the hook/skill/agent
  inventory it reasons over matches what lives under `.claude/` (`ls .claude/hooks/ .claude/agents/`).

A skill or agent that drives a script is only as current as the script: when the script changes,
re-read the doc that documents it.

## The PR gate

`require-skill-audit.sh` (PreToolUse in `.claude/settings.json`) refuses to **publish to a PR** —
`gh pr create` / `git push` in a local terminal, or `mcp__github__create_pull_request` /
`mcp__github__push_files` in the web/remote environment — until `.claude/.skill-audit-passed` is
newer than every source file, i.e. the audit still reflects the tree being pushed. The *merge*
into main is **not** gated here — that is the sibling `require-project-review.sh`. After a clean
audit with no unfixed drift, record it:

```bash
touch .claude/.skill-audit-passed
```

Any later edit re-stales the marker, forcing a fresh audit before the next PR create / push. The
marker is per-tree and transient (gitignored). `skill-audit ⊂ project-review`: a full
`/project-review` audits the skills too, so it records **both** `.claude/.project-review-passed`
and this marker; `/skill-audit` alone records only this one (you still need a current
project-review for the merge gate). To bypass intentionally, `touch` the marker yourself.

## Report structure

```
# Skill audit — tesla-key-esp32 (<date>)

## Summary
<1–3 sentences: how many skills/agents checked, how many drifted, what was corrected.>

## Findings
For each drift, in priority order:
### [SKILL-DRIFT] <skill/agent> — <short title>
- **Where:** `.claude/…:line`  →  ground-truth: `path:line`
- **What:** the fact it asserts vs. what the project actually says
- **Fix:** the exact edit (applied in this pass, or why deferred)

## Coverage
<Every skill + agent, one line each: ✓ matches project / ✗ drifted (→ fixed).>

## Gate
<recorded .claude/.skill-audit-passed | withheld — drift still open>
```

Keep each finding tight and tied to a named project fact. A clean run is a valid outcome: every
skill/agent ✓, zero edits, marker recorded.
