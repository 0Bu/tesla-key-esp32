---
name: skill-audit
description: Dedicated drift audit of every skill AND agent under .Codex/ against the tesla-key-esp32 project — confirms each still maps the code/config/scripts that exist and corrects any that contradicts them, in place. This is the PR-gated skills-coherence check (require-skill-audit.sh blocks opening a PR and every push to it until it has run against the current tree). Use whenever you ask to "check the skills", "audit the skills/agents", "are the skills still in sync / complete", "did a skill drift", after changing anything under .Codex/ or after a batch of code changes, or before opening/pushing a PR. Narrower and faster than project-review (which audits the whole firmware and gates the merge); skill-audit is the skills/agents subset — a full /project-review also satisfies this gate.
---

# skill-audit — keep every skill & agent in sync with the project

The `.Codex/skills/*/SKILL.md` files **and** the review agents in `.Codex/agents/*.md` are
documents that **drift**: each lags the project by exactly the changes landed since it was last
touched. A skill that names a wrong partition offset, a stale command count, a removed endpoint,
a renamed script, or a superseded target set silently mis-teaches every future session that
trusts it. This skill's one job is to **catch and fix that drift** so no skill/agent ships out of
sync with the code, config, scripts and docs it describes.

It is the **skills/agents subset of `project-review`**, pulled out as its own fast, PR-gated
check: `require-skill-audit.sh` refuses to open a PR or push to it until a skill-audit has run
against the current tree (see *The PR gate* below). `project-review` remains the deep
whole-firmware audit that gates the *merge*, and **also** covers the skills — so a full
`/project-review` ticks this gate's PR checkbox too. Run
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

1. **Enumerate — discover, don't hardcode.** `ls .Codex/skills/*/SKILL.md` (skills, incl. this
   one and `project-review`) **and** `ls .Codex/agents/*.md` (agents). Every file in that list
   must be audited; a skill/agent added since this doc was written is audited too, and appended
   to the *Per-target checklist* below. Also inventory what they describe:
   `ls .Codex/hooks/*.sh` and skim `.Codex/settings.json` (the hook wiring), `scripts/`,
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
6. **Record the pass in the PR so the gate clears** (see *The PR gate*). Only if no contradiction
   remains: tick the PR's `/skill-audit` checkbox and stamp it with the audited commit — the
   `- [x] ... @ <short-sha>` line. If drift remains unfixed, don't tick it — fix first.

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

**Skills** (`.Codex/skills/`):

- **`project-review`** — the whole-firmware coherence audit. Verify its *Project map* still lists
  every `main/*.{cpp,hpp}` + `main/logic/*.hpp`, its invariants (wake/sleep, link-state, heap,
  OTA, NVS, evcc, pairing, telemetry) still state what the code does, its cross-cutting
  "add X → also update Y" list has no removed/renamed target, and its hardcoded specifics
  (offsets `0x20000`, flash `4 MB`, slot `0x1f0000`, tesla-ble pin, command count) are current.
- **`flash-esp32`** — build + USB-flash path. Verify against `scripts/idf-docker.sh`
  (Docker-pinned, no local IDF), `partitions.csv` (`nvs@0x9000`, `otadata@0xf000`, app `@0x20000`),
  the target set + per-target bootloader offset, and that `@flash_args` never writes `nvs@0x9000`.
- **`ship`** — the merge→CI→signed-artifact→flash pipeline. Verify against
  `.github/workflows/build.yml` (artifact name `tesla-key-esp32-<version>`, `pr<N>-` prefix on
  PR builds, the firmware-change-gated release step), `scripts/ci-build-all.sh` (per-target
  suffix map + `sign_image`, the `-merged.bin` copies it must keep warning against),
  `partitions.csv` (`app@0x20000`, `otadata@0xf000/0x2000`, `nvs@0x9000` untouched), the merge
  gate it defers to (`require-project-review.sh`), and the verify endpoints (`/status`,
  `/api/proxy/1/version`, `/ota/check|update|status`).
- **`e2e-evcc`** — wraps `scripts/e2e_evcc.sh`. Verify the command count (must equal the
  REST rows — `api_name != nullptr` — in `logic/command_registry.hpp`'s `kCommands` —
  currently **15**), the version-coherence claim (`/status` = `X`,
  `/api/proxy/1/version` = `X-esp32`), the `vehicle_data` fields it asserts, the out-of-scope
  endpoint list, and the env-var gates (`RUN_COMMANDS`/`ALLOW_CHARGE_TOGGLE`/`RUN_ALL_COMMANDS`).
- **`vehicle-command-audit`** — compares the firmware against upstream `teslamotors/vehicle-command`,
  gated by what `yoziru/tesla-ble` can do. Verify the tesla-ble **pin** in its source map
  (`v5.1.1`) still matches `main/idf_component.yml`, its upstream file paths still resolve
  (e.g. `pkg/vehicle/charge.go`), and its "worked findings" don't assert drift already fixed.
- **`add-logic-test`** — scaffolds a `main/logic/` unit + `CHECK`s in `test/test_logic.cpp`.
  Verify against `scripts/run-mock-tests.sh`, the CI `logic-test` job
  (`.github/workflows/build.yml`), the `run-logic-tests.sh` **Stop hook** (`.Codex/settings.json`),
  the `CHECK`/`CHECK_STR`/`CHECK_NEAR` macro set, and the `static_assert` lock pattern.
- **`skill-audit`** (this skill) — verify its own numbers/paths (hook `require-skill-audit.sh`,
  the PR-checkbox gate mechanism — no file marker, the sibling list, the command count `15`, the
  tesla-ble pin) still match the tree, and that the skills/agents it names still exist. Correct it
  like any other; don't re-open it after.
- **`device-diag`** — read-only, cache-only live-board triage from `/status` + `/diag`. Verify the
  `/status` keys it names (`paired`/`reauth`/`link`/`vcsec_sleep`/`ble{connect_fail,car_connectable}`/
  `mqtt{configured,connected,tls,error}`/`last_seen_s`) against `handle_status`
  (`main/http_status.cpp`), the **lowercase** four `link_state_web_str` values
  (`main/logic/link_state.hpp`; uppercase are the MQTT `link_state_mqtt_str` set), the `/diag`
  params (`verbose`/`clear` in `handle_diag`), that there is still **no** heap field in `/status`
  (heap comes from the `BOOT`/`HEAP` serial lines in `main.cpp`), and the signature sites it cites
  (`connect error` in `ble_client.cpp`, the pairing-invalidation causes in `vehicle_ctrl.cpp`).
- **`display-preview`** — renders `tools/display_sim.py` to PNGs for a human eyeball pass. Verify the
  CLI modes (`png`/`states`/`states-portrait`/`search`/`scroll`/`cheader`/`parity`) + default output
  paths still match the script's `__main__`, the `cheader`→`main/display_font.h` and `parity`→gate
  warnings, the presenter/renderer files (`main/logic/display_model.hpp`, `main/display.cpp`), and
  the parity gate it defers to (`scripts/check-display-sim-parity.sh`, run from `run-mock-tests.sh`).
- **`ota-release-verify`** — verifies the already-published OTA channel (Pages manifest + per-target
  images + version coherence). Verify the manifest/firmware-base URLs (`main/Kconfig.projbuild`), the
  5-chipFamily set + per-part offsets (bootloader per-target, partition-table `32768`, app `131072`)
  in `scripts/build-pages.sh`, the suffix map across `ota_update.cpp`/`ci-build-all.sh`/
  `build-pages.sh`, the `version.txt` floor vs CI-stamped version, and the `/ota/*` +
  `/api/proxy/1/version` endpoints. Read-only; complementary to `ship` (which cuts/flashes a release).
- **`usb-recovery`** — no-build emergency reflash, **user-only** (`disable-model-invocation: true`).
  Verify the partition map against `partitions.csv` (app `@0x20000`, `otadata@0xf000/0x2000` erased,
  `nvs@0x9000/0x6000` never touched, `ota_1@0x210000`), per-target bootloader offset, the
  signed-image requirement (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`), the `-merged.bin`
  NVS-wipe warning, and the C5 no-auto-reset / `--before no-reset` / ROM-node gotchas.

**Agents** (`.Codex/agents/`) — audit these the same way; two duplicate content `project-review`
owns and must stay in sync with it:

- **`doc-drift-checker`** — the fast targeted-diff lens for the cross-cutting links. Its
  "add X → also update Y" enumeration must agree with `project-review`'s *Cross-cutting
  consistency* section.
- **`heap-safety-reviewer`** — the allocation/throw lens. Its heap rules/numbers must match
  `project-review`'s *Memory / heap* invariant and `main.cpp`'s heap-attribution log.
- **`Codex-optimizer`** — audits the `.Codex/` setup, not firmware logic. Confirm its
  boundary still defers firmware correctness to `project-review`, and that the hook/skill/agent
  inventory it reasons over matches what lives under `.Codex/` (`ls .Codex/hooks/ .Codex/agents/`).
- **`multi-target-build-reviewer`** — the per-target build/config divergence lens. Verify its
  facts against the build wiring: the target set (esp32/s3/c3/c6/c5), per-target bootloader
  offsets (`0x1000`/`0x2000`/`0x0`), the image-suffix map across `scripts/ci-build-all.sh` +
  `scripts/build-pages.sh` + `main/ota_update.cpp` (`TESLA_OTA_IMG_SUFFIX`), the app-size gate
  (`slot − 32 KB` = `0x1e8000`), the esp32c5 patch routing (`scripts/prepare-tesla-ble-c5.sh` +
  `main/idf_component.yml`), and the display/LED opt-in Kconfig. Complementary to
  `project-review`, not a firmware-logic reviewer.

A skill or agent that drives a script is only as current as the script: when the script changes,
re-read the doc that documents it.

## The PR gate

`require-skill-audit.sh` (PreToolUse in `.Codex/settings.json`) refuses to **publish to a PR** —
`gh pr create` / `git push` in a local terminal, or `mcp__github__create_pull_request` /
`mcp__github__push_files` in the web/remote environment — until the PR's `/skill-audit` checkbox
is ticked **and** stamped with the commit being published. There is **no file marker**: the
pass-state lives in the PR body itself (see `.Codex/hooks/pr-gate-lib.sh`). The *merge* into main
is **not** gated here — that is the sibling `require-project-review.sh`. After a clean audit with
no unfixed drift, tick + stamp the box with the head commit:

```
- [x] `/skill-audit` clean — PR create/push gate @ <short-sha>    # <short-sha> = git rev-parse --short=12 HEAD
```

For a **new** PR, put that line in the body you submit (`gh pr create --body-file …` or the MCP
`create_pull_request` body — the gate reads it directly, no network). For an **existing** PR, edit
its body (`gh pr edit <pr> --body-file …`, or the GitHub MCP update tool) before pushing. A push to
a branch that has **no PR yet** is allowed — that is not publishing to a PR; the create gate is the
chokepoint. But if GitHub is **unreadable** (no `gh` and no `GH_TOKEN`/`GITHUB_TOKEN` — e.g.
web/remote without a token), the push gate fails **closed** rather than let an unaudited push slip
through, mirroring the merge gate: set a token or push from a `gh`-authenticated session. Any later
commit changes the sha and re-stales the box, forcing a fresh audit before
the next PR create / push. `skill-audit ⊂ project-review`: a full `/project-review` audits the
skills too, so it lets you tick **both** the `/project-review` and `/skill-audit` boxes;
`/skill-audit` alone ticks only this one (you still need a current project-review for the merge
gate). To bypass intentionally, tick the box yourself.

## Report structure

```
# Skill audit — tesla-key-esp32 (<date>)

## Summary
<1–3 sentences: how many skills/agents checked, how many drifted, what was corrected.>

## Findings
For each drift, in priority order:
### [SKILL-DRIFT] <skill/agent> — <short title>
- **Where:** `.Codex/…:line`  →  ground-truth: `path:line`
- **What:** the fact it asserts vs. what the project actually says
- **Fix:** the exact edit (applied in this pass, or why deferred)

## Coverage
<Every skill + agent, one line each: ✓ matches project / ✗ drifted (→ fixed).>

## Gate
<ticked the PR `/skill-audit` box @ <sha> | withheld — drift still open>
```

Keep each finding tight and tied to a named project fact. A clean run is a valid outcome: every
skill/agent ✓, zero edits, PR box ticked.
