---
name: project-review
description: "Read-only whole-project coherence review of tesla-key-esp32 for bugs, security risks, doc/code drift, documentation quality, config/build mismatches, and cross-cutting inconsistencies. A review request authorizes inspection and reporting only: do not edit files, stamp or edit PRs, commit, push, merge, release, flash, OTA, contact a live vehicle/device, or fix findings unless the user separately authorizes implementation."
---

# Project review

Invoke `$project-review` for whole-project coherence, including documentation quality. Read
[AGENTS.md](../../../AGENTS.md) first. Review is read-only: inspect/test/report/stop; no edits,
PR stamps, commits, push, merge, signing, release, flash/OTA or live device/vehicle contact.
A later implementation is a separate scope and requires an independent later review.

## How to run a review

0. **Step 0 — pin the baseline.** Run `git fetch origin` only through the permitted Git boundary;
   verify HEAD equals current `origin/main` for a main audit or the exact target PR head.
   **Fail-fast on divergence** or local/untracked drift; do not manufacture a clean gate from a
   stale or changing checkout. If synchronization is unavailable, report the limitation.
   **State reviewed SHA in report.**
1. Read the owner documents and record concrete assertions to verify:

   [`AGENTS.md`](../../../AGENTS.md) owns runner policy, authorization, safety, evidence, and review contracts.
   [`docs/USER_GUIDE.md`](../../../docs/USER_GUIDE.md) owns hardware and operation.
   [`docs/API.md`](../../../docs/API.md) owns HTTP API, commands, and MQTT topics.
   [`docs/DEVELOPMENT.md`](../../../docs/DEVELOPMENT.md) owns build contracts.
   [`docs/ARCHITECTURE.md`](../../../docs/ARCHITECTURE.md) owns telemetry, MQTT, sleep/link state, pairing, and OTA.
   [`docs/MCP.md`](../../../docs/MCP.md) owns MCP tools.
   [`docs/SECURITY.md`](../../../docs/SECURITY.md) owns NVS, signing, and exposure.
   Root README owns first setup; docs/README is an index. Do not copy deep catalogs into AGENTS.
2. Discover shipped source/configuration from `main/CMakeLists.txt`, `main/`, `main/logic/`,
   `sdkconfig.defaults*`, `main/Kconfig.projbuild`, `partitions.csv`, manifest/locks, patches,
   `version.txt`, workflows, scripts, Pages runtime and tests. Follow call sites and consumers,
   not only the diff. Account for every shipped subsystem and any excluded area. Include native
   `.codex/` adapters, effective primary-checkout hook trust and parent reviewer permission overrides.
3. Compare docs→code and code→docs; verify endpoints, bounds, optional fields, persistent records,
   defaults, UI labels and build/dependency/layout facts. Re-derive runtime invariants below.
4. Assess documentation quality and audit **every** canonical skill/reviewer using the
   [skill-audit checklist](../skill-audit/SKILL.md#per-target-checklist).
5. Run proportionate local checks permitted by the scope, then produce evidence-bounded findings
   and coverage. Do not contact hardware to prove a hypothesis.
6. Only a complete clean assessment may provide the records below for a separately authorized
   PR-body update. This review never inserts them, and never establishes `$pr-hygiene` readiness.

```text
- [x] `$project-review` clean — merge gate @ <full-40-hex-sha>
- [x] `$skill-audit` clean — PR create/push gate @ <full-40-hex-sha>
```

## Documentation quality (required in every review)

Every review assesses the whole repository's documentation: `README.md`, `docs/`, `test/README.md`,
`AGENTS.md`, scoped rules, skills and reviewer instructions. Keep repository documentation in English;
the user-facing entry point must be understandable to technically experienced users without firmware
internals. Inspect changed documentation in depth and check the wider structure and repeated topics.

- **Structure:** each document has a clear purpose and audience; navigation separates setup,
  operation, integration and development, with deeper references linked where needed.
- **Clarity:** use plain, precise language, explain unfamiliar terms and give procedures their
  prerequisites, ordered steps and an observable result.
- **Concision:** retain the minimum necessary to understand and perform the task. Consolidate
  repeated explanations under one authoritative owner; keep short warnings beside risky steps.
  Remove filler, obsolete instructions and history that no longer explains a current decision.
- **Presentation:** keep paragraphs and table cells focused; separate dense explanations from
  lookup tables. Word counts identify hotspots, not arbitrary deletion quotas.
- **Completeness:** preserve safety conditions, limits, errors, compatibility and evidence boundaries;
  keep decisions traceable to their maintained references, code or tests.

Report material failures as `DOC-QUALITY` with path/line, concrete reader impact and a specific
cut, consolidation or rewrite; cite both locations for duplication. Minor optional polish is `NIT`.
The report states coverage and a documentation-quality verdict. An incomplete assessment or unresolved
material `DOC-QUALITY` finding prevents a `$project-review` clean verdict. Review remains read-only.

## Runtime review checklist

Read the named [architecture sections](../../../docs/ARCHITECTURE.md) and derive these checks from
current implementation. These are review questions, not substitute implementations.

| Area | Required checks |
|---|---|
| Sleep/wake | Cached status/MQTT/display never wake; NO_WAKE background polls pause under commands; signed health probe does not open Infotainment; fresh-contact charging and recent-command active window; debounce, retry and episode quiescence survive repeated cycles. |
| Link/presentation | One link-state owner; live Infotainment proves awake, debounced VCSEC proves sleep, reachability proves idle; unknown/unreachable and last-known labels remain honest across UI/MQTT/API; phase timers/presenter mirrors agree. |
| Commands/protocol | One registry/dispatcher; required/optional bounds and REST/MCP differences; complete fixed evcc charge_state versus optional status/MQTT fields and imperial versus metric units; Charging-Manager feasibility; request UUID/replay/authentication binding, initialized-secret admission and fail-closed encrypted VCSEC before telemetry; late completion generation/lifetime; charging-current ACK plus fresh exact readback. |
| Heap/locks | INTERNAL largest block, bounded payloads, all allocation outcomes and unwind paths; RAII ordering; no Vehicle lock while waiting; POD callbacks/zero-wait host locks; parse after unlock; sticky JSON with exactly one failure reply and no partial retained publish. |
| Startup/recovery | Essential versus optional failure, Ready admission, dual-task handshake, NimBLE sync/late callbacks; fatal pending rollback versus stable halt; durable watchdog breadcrumb/cap and suppressed restart active window. |
| NVS/identity | Exact registered namespace/key/API surface (no truncation); config durability including unreadable legacy keys; VIN/key journals, timestamp retirement and fail-closed recovery; Stable-only mutation; clock restore before session load; authoritative-clock gating for key_created/paired_at/MQTT boot_time; OTA retention and reset/identity distinction. |
| OTA | Bounded complete manifest/JSON/version intake; exact manifest/descriptor equality, channel/preview/return exceptions; wrong target/signature; shared operation ownership; no false health confirmation from setup, automatic recovery or partial runtime; network/heap/NimBLE evidence. |
| Network | Trial WiFi rollback, reconnect after proven lease, transport/lease/gateway-bound baseline and stale in-flight verdict rejection, no ICMP-induced restart; cached board detection before GPIO drive, ordered W5500 probe/all-later-board pin safety/partial unwind; route/lease ownership; quarantined asynchronous ping generations. |
| MQTT/Syslog | TLS credentials/certification policy, no command subscription; presence/units/identity; discovery→availability→state retries and invalid-cache clearing; post-unlock sinks, bounded logging, generation-safe redaction and storm throttling. |
| Build/publication | Exact four targets; v5.3.0 pin and full patch chain; pinned IDF/locks; effective-build closure and flags; both host gates plus concurrent producer/independent target legs; size/stack/projection; fresh independent bytes; protected key boundary; production RSA-PSS pin; immutable reuse, Release/Pages equality and stale-preview cleanup. |

## Cross-cutting consistency

For every changed contract, follow **all** its consumers:

| Change | Also inspect |
|---|---|
| Route/command/argument | Route table, common registry, executor, REST/MCP schemas, web UI, API/MCP docs and host fixtures |
| Telemetry/status field | Parser/presence/cache, status emitter, MQTT payload/discovery, UI/display snapshot, API docs and golden/OOM tests |
| State/timer/task | Owner/lock/atomic model, runtime admission, all consumers, repeated-cycle tests, architecture and any presenter parity |
| Persistent record/config | NVS registry/oracle, storage adapter, journals/rollback, provisioning, retention table, UI and privacy exports |
| Target/dependency/patch | All lockfiles/defaults, root CMake, patch application, vectors/harness, build/sign/manifest/reproducibility, ADR departures and skills |
| Flash/sign/channel | Layout validators, consumer provenance/signature checks, projected/actual sizes, immutable publication/preview cleanup and delivery skills |
| Agent/review policy | AGENTS/scoped rules, hooks, canonical skills/reviewers, exact content pins, relevance classifiers, PR template and negative canaries |

Use `$vehicle-command-audit` for upstream/local/pinned-library evidence when its relevance predicate
applies. [FEATURES](../../../docs/FEATURES.md) remains a pointer catalog, not another contract.

## Reviewing the skills

[Skill-audit](../skill-audit/SKILL.md#per-target-checklist) owns the per-sibling checklist; execute it
rather than maintaining a second copy here. Check this workflow's discovered source coverage,
invariants, owner references and recent source commits against its last update too.

- **`$feature-docs`**: verify `gate_feature_docs_relevant` against `main/`, `test/`,
  `sdkconfig.defaults*`, `partitions.csv`, `AGENTS.md`, `.agents/`, `.codex/`,
  `.github/PULL_REQUEST_TEMPLATE.md`, `tools/agent-hooks/`, `tools/agent-config/`, `scripts/with-host-python.sh`,
  shipped Pages (`docs/index.html`, `installer-bootstrap.mjs`, `serial-port-release.mjs`,
  `web-installer.mjs`, `docs/vendor/`),
  `.github/workflows/{build,signed-pr-preview,pr-preview-cleanup,pr-policy,bench-acceptance}.yml`
  and `scripts/release-relevance.sh`. Missing/truncated shared predicates must fail all five gates
  closed; `scripts/test-pr-gates.sh` proves the negative cases.

## Verification discipline

Distinguish confirmed paths/reproductions from suspected risks; cite the signal that separates them.
Decode crashes only against the matching ELF SHA. Distinguish PANIC, BROWNOUT and WDT; examine
largest-block trends, not total heap alone. A library change proposal belongs in a reviewed pin/patch
migration, never a hand-edited managed checkout.

Use the [development checks](../../../docs/DEVELOPMENT.md#validate-a-change) and
[test evidence limits](../../../test/README.md#whats-not-covered-by-design). Report host, IDF,
disposable signing, remote CI, browser, hardware/USB, OTA and live vehicle evidence independently;
mark missing boundaries not run. A green build is not review or physical acceptance.

## Report

State baseline SHA, scope/exclusions and overall verdict. List findings by impact with category
`BUG`, `INCONSISTENCY`, `DOC-DRIFT`, `DOC-QUALITY`, `SKILL-DRIFT`, `RISK` or `NIT`, separate severity,
path/line (both sides for drift), cause, reader/runtime impact, evidence/confidence and specific fix.
Report doc↔code, config/build, cross-cutting consumers, **documentation-quality scope/verdict/limits**,
and every skill/reviewer coverage. End with prioritized actions and exact-head readiness or reason
withheld. Unresolved material documentation findings or incomplete assessment prevent clean.
