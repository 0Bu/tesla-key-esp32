---
name: skill-audit
description: Read-only drift audit of every canonical skill under .agents/skills and read-only reviewer in .agents/subagents.json against tesla-key-esp32. Report contradictions and gate readiness only; never correct files, edit or stamp a PR body, commit, push, merge, release, flash, OTA, or contact a live device/vehicle unless the user separately authorizes implementation.
---

# Skill audit

Invoke `$skill-audit`. Read [AGENTS.md](../../../AGENTS.md). This is one read-only pass through all
canonical skills and `.agents/subagents.json`: discover → verify → report → stop. No fixes, stamps,
commits, publication or live device/vehicle contact. Independent later audit verifies an authorized
implementation. This workflow establishes only its own gate; whole-project review may cover both.

## Audit

0. **Step 0 — pin the baseline and review purpose.** Use a permitted fresh `git fetch origin`
   or the operator's verified fresh fetch result. A main audit requires HEAD equal current
   `origin/main`; an audit of a published PR requires HEAD equal its exact remote head.
   For a separately authorized PR-create or pre-push review, pin the clean local candidate SHA
   and intended branch instead. Inspect fresh main, merge-base and intervening changes; if a PR
   already exists, the candidate must descend from its published head without rewriting history.
   State whether the reviewed SHA is a local publication candidate or an actual remote PR head;
   neither local review nor its records establish remote CI or merge readiness. After push,
   re-read GitHub and require the remote head to equal that reviewed SHA before using its records.
   **Fail-fast on divergence unexplained by the selected review purpose, or local drift**; report
   synchronization limits rather than certifying stale content.
   **State reviewed SHA in report, together with baseline evidence.**
1. Discover every canonical `.agents/skills/*/SKILL.md` and reviewer; do not hardcode the inventory.
   Check native `.codex/` generated registrations with `sync-codex.mjs --check`, effective
   primary-checkout hook trust and parent runtime overrides. Global host/cluster skills are outside this project audit.
2. Extract claims about paths, numbers, targets, pins, hooks, commands, permissions and evidence.
   Verify against source/config/scripts and owning docs with `rg` and safe host checks. The library
   pin is v6.3.0 here; always compare to `main/idf_component.yml`, not this remembered value.
3. Name a contradicting fact for every `SKILL-DRIFT` finding. Prose preference alone is not drift;
   whole-document quality is assessed by `$project-review`. Audit its mandatory material
   `DOC-QUALITY` blocker too.
4. Give every discovered skill/reviewer a coverage verdict and proposed correction; apply none.
   Only a contradiction-free complete audit may provide the exact record for separate PR editing.

## Per-target checklist

Read each canonical SKILL.md **in full**, every reviewer prompt, and their driven scripts/validators
and owning documents. Audit **all** prerequisites, authorization, required validations, failure/stop
conditions, commands, outputs and evidence contracts. The bullets below focus coverage; they do not
replace or relax any workflow requirement.

This is the single per-sibling checklist. `$project-review` uses it by reference. Read current
workflow scripts and [deep document owners](../../../docs/README.md), not historical skill verdicts.

- **`$project-review`**: whole-tree coverage, current invariants/consumer map, documentation-quality
  criteria and clean blocker, evidence bounds and exact records; check its self-coverage too.
- **`$flash-esp32`**: pinned compile-only path, explicit development key versus signed preview,
  exact PR/head artifact and metadata version, signature/pin, chip/port/size and app→otadata ordering.
- **`$ship`**: canonical exact-head merge, exact main run/channel/artifact, metadata/signature identity;
  separate USB/OTA scopes. OTA requires candidate equality and ≥100 s monotonic+wall-clock delta
  from first post-OTA baseline, no hidden reboot and boot-local mark-valid success. USB has only
  bounded boot retries. Manual stable release is separate from main Dev publication.
- **`$deploy`**: the user's explicit invocation authorizes its commit/push/PR/merge and protected Dev
  publication phases without repeated approval; independent fix/review loop, current-head
  gates, canonical squash and exact-main CI/publication observation; no device delivery.
- **`$deploy-bench`**: physical USB identity across Tesla/Daikin firmware, no probing unrelated boards,
  user invocation authorizes its standard preflight/reset/read/flash and passive acceptance scope
  through delegated skills without repeated approval,
  explicit port/chip and compatible retained layout, delegated signed `$flash-esp32` or published-image
  `$usb-recovery` steps, exact-source targeted positive/negative tests and same-board health
  evidence; separate vehicle/cluster scope.
- **`$ci-heal`**: the user's own invocation authorizes only attested stamping and fix pushes (the
  AGENTS.md exception); script never authors commits/reviews; explicit gate attestations, green current-head
  CI (successful `build`, no other check queued, cancelled or failed; a lone failing
  `current-head-records` counts as records pending), local HEAD equality, mergeability/relevance
  and unchanged-head check before merge-readiness stamping, then a passing new
  `current-head-records` run (or, when the stamp leaves the PR body unchanged and so starts no run,
  a passing latest run) and all checks green before readiness or merge; Phase 3 pre-push records attest the
  independently reviewed local fix commit; iteration cap, actual red/green selftests and separate
  merge authority.
- **`$vehicle-command-audit`**: upstream/local/pinned-library feasibility, lexical full patch chain,
  resolved upstream paths, role restrictions, replay/authentication/request binding and clock domains.
- **`$add-logic-test`**: firmware delegation or shipped-code parity with negative control, real owner
  function definition/invocation, repeated-cycle boundary tests, Stop-hook/CI wiring and static asserts.
- **`$mock-test`**: current fast/mock/sanitizer entry points, strict mode and host-only evidence.
- **`$pr-hygiene`**: independent privacy/English gate on title/body/all commits/touched docs/comments;
  create/push/merge requirement, redacted findings and current-head invalidation.
- **`$feature-docs`**: verify `gate_feature_docs_relevant` against `main/`, `test/`,
  `sdkconfig.defaults*`, `partitions.csv`, `AGENTS.md`, `.agents/`, `.codex/`,
  `.github/PULL_REQUEST_TEMPLATE.md`, `tools/agent-hooks/`, `tools/agent-config/`, `scripts/with-host-python.sh`,
  shipped Pages (`docs/index.html`, `installer-bootstrap.mjs`, `serial-port-release.mjs`,
  `web-installer.mjs`, `docs/vendor/`),
  `.github/workflows/{build,signed-pr-preview,pr-preview-cleanup,pr-policy,bench-acceptance}.yml`
  and `scripts/release-relevance.sh`. A missing, truncated or incomplete `pr-gate-lib.sh` must block
  `require-pr-gates.sh` before any of the five records is read; `scripts/test-pr-gates.sh` pins
  those negative cases.
- **`$skill-audit`**: discovered inventory, current fact pointers, exact head and report-only gate;
  report self-drift without correcting it.
- **`$device-diag`**: collected evidence first; separate live-target permission; cache-only GETs with
  no verbosity/clear changes; state-changing checks use separately authorized POST aliases. Current status keys/units/presence, internal spot/log/trend heap sources,
  classified BLE failures, reset/crash/pairing causes and read-only handoff.
- **`$display-preview`**: current simulator CLI/output, PNG inspection and presenter parity;
  source-writing `cheader` is outside preview; no hardware claim.
- **`$ota-release-verify`**: immutable stable tag dereference, complete 16-part local snapshot,
  length/hash/merged ranges and target/version identity, signature/artifact limits, end-snapshot race
  recheck and separately approved live reads; no publication/ref mutation.
- **`$usb-recovery`**: exact signed Release/main sources (not Pages), same-run provenance, expiry/
  ambiguity stops, production signature pin, explicit chip/port/regions, retained bootloader-IDF
  compatibility, NVS preservation, app-first activation-last and independent live HTTP approval.

Reviewers are also read-only: `agent_config_reviewer` checks policy/config/hook authority;
`doc_drift_checker` checks concrete changed contracts and doc quality; `heap_safety_reviewer` checks
allocation/unwind/locks; `multi_target_build_reviewer` checks all-target graph, offsets/suffixes,
effective builds, independent signing/publication DAG and byte identity. Verify each prompt's facts
against its sources, scope and no-mutation sandbox.

## The PR gate

After clean complete coverage, provide without inserting:

```text
- [x] `$skill-audit` clean — PR create/push gate @ <full-40-hex-sha>
```

`tools/agent-hooks/require-pr-gates.sh` and `pr-gate-lib.sh` require unique top-level current-head
records; fenced/quoted/HTML/dynamic/ambiguous/foreign or stale forms fail. No file marker exists.
PR creation uses one exact body source; an authorized existing-body edit precedes push. Unreadable
GitHub state fails closed; new commits stale every record. Hygiene remains an independent gate.

Guarded create/push/merge must be the only shell segment. Wrappers/path-qualified actions and Git/GH
context overrides are recognized conservatively; aliases, dynamic argv/globs, changed cwd/repository,
foreign remotes and ambiguous context fail closed. Do not mutate HEAD/config/PR before the action
within the same call. `scripts/test-pr-gates.sh` pins adversarial forms.

Push supports only current HEAD→current branch on sole matching origin fetch/push URL, one refspec.
Alternate refs/remotes, tag/all/mirror pushes, followTags, custom multiple pushes and ambiguous syntax
fail closed. A checkbox for local HEAD cannot authorize other bytes. Separately authorized standalone
origin deletion of an existing unambiguous non-protected feature branch is permitted without gate
records. main/master/gh-pages, tags and other namespaces remain prohibited.

## Report

State SHA/scope, discovered skill/reviewer counts and coverage for **every** entry. Findings name
skill/agent path/line, contradictory ground-truth path/line, impact and exact proposed edit.
Conclude readiness record or withheld reason. No self-invocation, correction or publication in audit.
