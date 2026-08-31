---
name: skill-audit
description: Read-only drift audit of every canonical skill under .agents/skills and read-only reviewer under .codex/agents against tesla-key-esp32. Report contradictions and gate readiness only; never correct files, edit or stamp a PR body, commit, push, merge, release, flash, OTA, or contact a live device/vehicle unless the user separately authorizes implementation.
---

> **Canonical runner-neutral skill.** Read [`AGENTS.md`](../../../AGENTS.md) before acting.
> Project skills are canonical under [`.agents/skills/`](../), and lifecycle/PR policy is
> enforced by the runner-neutral core under [`tools/agent-hooks/`](../../../tools/agent-hooks/).
> This skill does not grant permissions beyond the user's explicit request.
> Invoke this workflow canonically as `$skill-audit`.

# skill-audit — keep every skill & agent in sync with the project

The `.agents/skills/*/SKILL.md` files and read-only reviewers in `.codex/agents/*.toml` are
documents that **drift**. A wrong partition offset, stale command count, removed endpoint, renamed
script, or superseded target set silently mis-teaches a future session. `$skill-audit` catches and
reports that drift. It never edits a file or external state during an audit-only run.

It is the skills/agents subset of `$project-review`. A clean `$project-review` can establish
readiness for both PR records, but neither audit mutates the PR body. Use `$skill-audit` for the
narrow map audit and `$project-review` for whole-firmware coherence.

## What counts as drift (and what does not)

Drift is measured against the **project**, never against a skill's own wording. A finding
(`SKILL-DRIFT`) exists only where a skill or agent **contradicts** a ground-truth fact in the
code / config / script / doc:

- a **wrong number** — partition offset, flash size, slot size, command count, fragment size,
  timeout/debounce constant, a version or library pin;
- a **removed or renamed** thing — an endpoint, command, MCP tool, NVS key, Kconfig option,
  file path, script name, function;
- a **stale set** — the target list (esp32/s3/c3/c6), the OTA `<suffix>` map, the enrolled role;
- a **broken pointer** — a skill that documents a script/hook whose behaviour no longer matches
  the file that actually runs, or an agent boundary that points at a skill that changed.

**Not drift:** prose style, wording preferences, ordering, or "could be clearer". Every reported
finding must name the project fact it contradicts; otherwise omit it.

## How to run the audit

Work in this order—it is a **single read-only pass**: enumerate → check → report → stop.

1. **Enumerate—discover, do not hardcode.** Read every `.agents/skills/*/SKILL.md` and every
   `.codex/agents/*.toml`. Inventory `AGENTS.md`, `.codex/hooks.json`, `tools/agent-hooks/`,
   `scripts/`, `main/`, `partitions.csv`, `main/idf_component.yml`, and `version.txt`. Host/cluster
   operational skills such as global `$tesla-key-e2e-evcc` are outside this project audit.
2. **Extract concrete claims.** List numbers, paths, counts, flags, target sets, script names,
   authorization boundaries, and described hook behavior.
3. **Verify claims against the tree.** Use runner-neutral file reads/search (`rg` preferred) and
   safe host checks. Never contact a live device/vehicle or perform a mutation to prove an audit
   claim. Local tests that create ignored build output run only when the review scope requests
   them and are reported separately from CI/hardware evidence.
4. **Report, do not correct.** Every canonical skill and reviewer gets a ✓ or a
   `SKILL-DRIFT` finding with the exact proposed change. An audit request never authorizes applying
   that proposal.
5. **Report gate readiness without editing the PR.** If no contradiction remains, provide the exact
   record a separately authorized PR-body update would need; otherwise withhold readiness.

### Termination — one report-only pass

A `$skill-audit` invocation reads, checks, reports, and stops. It does not invoke itself, edit a
finding, or re-audit an edit. A separately authorized implementation may address accepted findings;
an independent later audit verifies the result.

## Per-target checklist (what each skill/agent must stay true to)

Discover the list at runtime (step 1); this is the authoritative map of what each current
skill/agent asserts, so you know where to look. Keep it in step with `$project-review`'s
*Reviewing the skills* section — the two describe the same siblings and must agree (this skill is
the authority for the per-sibling drift check; `$project-review` defers the mechanical part here).

**Skills** (`.agents/skills/`):

- **`$project-review`** — the whole-firmware coherence audit. Verify its *Project map* still lists
  every `main/*.{cpp,hpp}` + `main/logic/*.hpp`, its invariants (wake/sleep, link-state, heap,
  OTA, NVS, evcc, pairing, telemetry) still state what the code does, its cross-cutting
  "add X → also update Y" list has no removed/renamed target, and its hardcoded specifics
  (offsets `0x20000`, flash `4 MB`, slot `0x1f0000`, tesla-ble pin, command count) are current.
- **`$flash-esp32`** — local compile + explicitly signed USB-flash / signed-preview path. Verify
  against `scripts/idf-docker.sh` (`esp-idf-toolchain.txt` pin, no local IDF), normal PR output
  `firmware-unsigned`, the opt-in `.github/workflows/signed-pr-preview.yml` artifact, and
  `partitions.csv` (`nvs@0x9000`, `otadata@0xf000`, app `@0x20000`). Artifact selection must be
  exact and source-SHA-bound; every USB write must use a verified signed app and preserve NVS.
- **`$ship`** — the merge→CI→signed-artifact→flash pipeline. Verify against
  `.github/workflows/build.yml` (unsigned `firmware-unsigned` build artifact, protected main-only
  artifact `tesla-key-esp32-<version>-<full-source-SHA>`, the firmware-change-gated release step) and
  `.github/workflows/signed-pr-preview.yml` (separate
  `tesla-key-esp32-pr<N>-<full-head-SHA>` path).
  `scripts/ci-build-all.sh` owns the four unsigned trees + projected-signed size gate;
  `scripts/ci-sign-artifacts.sh` owns the suffix map, real signing, actual signed-size gate and
  `-merged.bin` copies. The skill must select one exact signed artifact and bind the run plus
  metadata to the merge SHA/version. Verify also `partitions.csv` (`app@0x20000`,
  `otadata@0xf000/0x2000`, `nvs@0x9000` untouched), the merge
  gate it defers to (`require-pr-gates.sh`: current `$project-review` and `$pr-hygiene` records,
  plus `$feature-docs` when relevant), and the verify endpoints (`/status`,
  `/api/proxy/1/version`, `/ota/check|update|status`). OTA must require exact-version availability
  before POST, use bounded download/reboot probing, and keep exact version/platform under
  observation for at least 100 s from the first post-OTA live baseline, with both monotonic
  uptime delta and wall-clock delta reaching that floor and staying within the documented small
  tolerance, then require boot-local evidence that the mark-valid API returned success; absolute
  device uptime is insufficient and hidden reboots or an unconfirmed rollback cancellation must
  fail closed.
  USB gets only a short bounded boot/reachability retry, never that OTA probation wait.
- **`$vehicle-command-audit`** — compares the firmware against upstream `teslamotors/vehicle-command`,
  gated by what `yoziru/tesla-ble` can do. Verify the tesla-ble **pin** in its source map
  (`v5.1.2`) still matches `main/idf_component.yml`, every repository-owned patch under
  `patches/tesla-ble/` is applied lexically/idempotently/fail-closed, and the complete patch series
  applies through root CMake to the one materialised managed-component tree; its upstream paths resolve
  (e.g. `pkg/vehicle/charge.go`), and its "worked findings" don't assert drift already fixed.
- **`$add-logic-test`** — scaffolds a `main/logic/` unit + `CHECK`s in `test/test_logic.cpp`.
  Verify against `scripts/run-mock-tests.sh`, the CI `logic-test` job
  (`.github/workflows/build.yml`), the `stop-logic-tests` handler in
  `tools/agent-hooks/agent_hook.py` wired by `.codex/hooks.json`,
  the `CHECK`/`CHECK_STR`/`CHECK_NEAR` macro set, and the `static_assert` lock pattern.
- **`$pr-hygiene`** — screens the PR title/body, commit messages and touched documentation for
  personal/private information (`PRIVACY-LEAK`: LAN IPs, MAC addresses, VINs, WiFi network names,
  hostnames, emails) and content not written in English (`LANGUAGE`). Verify it against
  `tools/agent-hooks/require-pr-gates.sh` — it is the **fourth** PR gate, and the strictest: it
  fires at PR creation, every push, **and** merge, unlike `$skill-audit` (create/push only) or
  `$project-review`/`$feature-docs` (merge only). Unlike `$skill-audit ⊂ $project-review`, a clean
  `$project-review` or `$skill-audit` run does **not** establish `$pr-hygiene` readiness —
  confidentiality/language is a separate axis from coherence.
- **`$feature-docs`** — keeps `docs/FEATURES.md` in sync when a platform feature lands or changes.
  Verify the gate it defers to, `tools/agent-hooks/require-pr-gates.sh` — the **fourth** PR gate
  beside this one, `$project-review` and `$pr-hygiene`, and the only *conditional* one — and above
  all its **relevance filter**: the paths that arm it must still match the hook's own regex, currently
  `main/` / `test/` / `sdkconfig.defaults*` / `partitions.csv` /
  `AGENTS.md` / `.agents/` / `.codex/` / `.github/PULL_REQUEST_TEMPLATE.md` /
  `tools/agent-hooks/` / `tools/agent-config/` /
  shipped Pages runtime (`docs/index.html`, `installer-bootstrap.mjs`, `serial-port-release.mjs`,
  `web-installer.mjs`, `docs/vendor/`) /
  `.github/workflows/{build,signed-pr-preview,pr-preview-cleanup,pr-policy,bench-acceptance}.yml` /
  `scripts/release-relevance.sh` (the shared
  `gate_feature_docs_relevant` predicate). A path that drifts out of that list stops gating
  silently. All four gates must block when the shared library is missing, truncated or lacks a
  required function; `scripts/test-pr-gates.sh` pins those negative cases.
- **`$skill-audit`** (this skill) — verify its own numbers/paths, report-only boundary, neutral hook
  references, PR-record mechanism, sibling list, command count `15`, and tesla-ble pin. Report any
  self-drift; do not correct it during the audit.
- **`$device-diag`** — read-only, cache-only live-board triage from `/status` + `/diag`. Verify the
  `/status` keys it names (`paired`/`reauth`/`link`/`vcsec_sleep`/`ble{connect_fail,car_connectable}`/
  `mqtt{configured,connected,tls,error}`/`last_seen_s`) against the field contract in
  `main/logic/status_model.hpp` (the key literals live in its `emit()`; `handle_status` /
  `build_status_object()` in `main/http_status.cpp` only gather the inputs and serve them),
  the **lowercase** four `link_state_web_str` values
  (`main/logic/link_state.hpp`; uppercase are the MQTT `link_state_mqtt_str` set), the `/diag`
  params (`verbose`/`clear` in `handle_diag`), the **three** heap sources it must keep straight —
  the always-present `sys{board_mac,free_heap,min_free_heap,largest_block,…}` spot block
  (`status_model.hpp` `emit()`), the 24-hour trend served by `GET /heap`
  (`heap_trend.cpp`, `logic/heap_history.hpp`), and the log lines (`BOOT`/`HEAP` in `main.cpp`
  **and** the periodic `HEAP …internal_largest=` trend line in `vehicle_telemetry.cpp`'s
  `loop_task_fn_` — the one the heap watchdog decides on, plus the
  `HEAP CRITICAL`/`EXHAUSTED` escalation lines beside it) — that
  `last_reboot` and `last_crash` are emitted only when set, and the signature sites it cites
  (classified production `BLE connect gave up` in `vehicle_commands.cpp`, raw maximum-DEBUG
  `connect error` detail in `ble_client.cpp`, and the pairing-invalidation causes in
  `vehicle_ctrl.cpp`).
- **`$display-preview`** — renders `tools/display_sim.py` to PNGs for a human eyeball pass. Verify the
  CLI modes (`png`/`states`/`states-portrait`/`search`/`scroll`/`cheader`/`parity`) + default output
  paths still match the script's `__main__`, the `cheader`→`main/display_font.h` and `parity`→gate
  warnings, the presenter/renderer files (`main/logic/display_model.hpp`, `main/display.cpp`), and
  the parity gate it defers to (`scripts/check-display-sim-parity.sh`, run from `run-mock-tests.sh`).
  Its sibling gate `scripts/check-ble-row-parity.sh` (also run from `run-mock-tests.sh`) does the
  same for the web UI: `tk::ble::decide` (`main/logic/ble_row.hpp`) vs the `BLE_ROW` region of
  `main/www/app.js`, via `test/ble_row_golden_dump.cpp` + `tools/ble_row_parity.js`.
- **`$ota-release-verify`** — verifies the already-published OTA channel byte-for-byte against the
  latest GitHub Release whose API reports `immutable: true`: manifest `sourceSha` equals the
  dereferenced Release-tag commit, all 16
  parts match manifest length/SHA-256 and their byte ranges in the four exact Release merged
  assets, and all four apps report the exact Release version and chip family. Verify the
  manifest/firmware-base URLs (`main/Kconfig.projbuild`), the 4-chipFamily set +
  four ordered per-part offsets (bootloader per-target, partition-table `32768`, app `131072`,
  otadata `61440` last) in `scripts/build-pages.sh`, the suffix map across
  `ota_update.cpp`/`logic/target.hpp`/`ci-sign-artifacts.sh`/`build-pages.sh`, the `version.txt`
  floor vs CI-stamped version, the build/test-only `workflow_dispatch` boundary (it must never
  sign/release/republish Pages), same-SHA reuse only for the newest valid tag, current-main/tag and
  latest-Release/`immutable: true`/digest-asset selection and end-of-run race rechecks before
  signing/Release/Pages mutation and deploy, and the `/ota/*` +
  `/api/proxy/1/version` endpoints. Read-only;
  complementary to `$ship` (which cuts/flashes a release).
- **`$usb-recovery`** — no-build emergency reflash requiring explicit, scope-specific user approval.
  Verify the partition map against `partitions.csv` (app `@0x20000`, `otadata@0xf000/0x2000` erased,
  `nvs@0x9000/0x6000` never touched, `ota_1@0x210000`), per-target bootloader offset, the
  signed-image requirement (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`), exact Release-byte ↔
  source-SHA-bound main-artifact match or exact signed main artifact (never Pages), the `-merged.bin`
  NVS-wipe warning, explicit unambiguous port / no-auto-reset / ROM-node handling, and bounded
  post-reset verification of exact version/platform plus `paired:true`.

**Agents** (`.codex/agents/`) — audit these the same way; two duplicate content `$project-review`
owns and must stay in sync with it:

- **`doc_drift_checker`** — the fast targeted-diff lens for the cross-cutting links. Its
  "add X → also update Y" enumeration must agree with `$project-review`'s *Cross-cutting
  consistency* section.
- **`heap_safety_reviewer`** — the allocation/throw lens. Its heap rules/numbers must match
  `$project-review`'s *Memory / heap* invariant and `main.cpp`'s heap-attribution log.
- **`agent_config_reviewer`** — audits runner-neutral configuration, not firmware logic. Confirm
  its read-only boundary and inventory of `AGENTS.md`, `.agents/`, `.codex/`, and
  `tools/agent-hooks/`.
- **`multi_target_build_reviewer`** — the per-target build/config divergence lens. Verify its
  facts against the build wiring: the target set (esp32/s3/c3/c6), per-target bootloader
  offsets (`0x1000` classic esp32 / `0x0` s3·c3·c6 — `boot_offset()` in
  `ci-sign-artifacts.sh`/`build-pages.sh`), the image-suffix map across `scripts/ci-sign-artifacts.sh` +
  `scripts/build-pages.sh` + `main/ota_update.cpp` (`TESLA_OTA_IMG_SUFFIX`), the app-size gate
  (`slot − 32 KB` = `0x1e8000`), the tesla-ble target list
  (`main/idf_component.yml`, Component-Manager-enforced), the complete ordered all-target patch-series
  wiring (`patches/tesla-ble/` + `scripts/apply-tesla-ble-patches.sh` + root CMake), and
  the display/LED opt-in Kconfig. Complementary to
  `$project-review`, not a firmware-logic reviewer.

A skill or agent that drives a script is only as current as the script: when the script changes,
re-read the doc that documents it.

## The PR gate

[`tools/agent-hooks/require-pr-gates.sh`](../../../tools/agent-hooks/require-pr-gates.sh) refuses
PR create/push until the PR's `$skill-audit` record is uniquely
present and stamped with the exact commit being published. Server-side push-files actions fail
closed because no local audit SHA can bind a commit created after the pre-tool check. There is no
file marker; pass state lives only in the PR body and is parsed by the neutral core. `$skill-audit`
reports the following record after a clean audit but never inserts or stamps it:

```
- [x] `$skill-audit` clean — PR create/push gate @ <full-40-hex-sha>
```

**What the gate recognises.** `gate_bash_actions` in `pr-gate-lib.sh` splits Bash input into
conservative shell-like segments and recognises direct plus wrapped/path-qualified publish
actions. A create/push/merge must be the **only shell segment** in its tool call: a preceding
commit/config/cd/PR edit would mutate the state after PreToolUse checked it. Git-global options,
repository/config-affecting `GIT_*`/`env` prefixes, env cwd changes, GH repo/host overrides,
unknown/path-qualified executables and foreign repositories are recognised but fail closed;
diagnostic `GIT_TRACE*` and unrelated single-segment assignments remain usable. An unmatched
action used to mean NO verdict, which is why `scripts/test-pr-gates.sh` adversarially pins every
spelling. Dynamic executable/subcommand/argv expansion, command-position globs, per-command alias
config and configured Git/GH aliases are unsafe; parsing is deliberately conservative, so keep
each guarded action in a separate Bash call.

For Bash `git push`, the gate also resolves the refspec before trusting the stamp. Only one push
from the current project of the current `HEAD` to the current branch is supported
(`git push origin <current-branch>`; safe current-branch `HEAD:<branch>` is equivalent). The sole
remote must be `origin`, whose one fetch and push URL must agree. Git global/env repo/config
context, another remote/source/destination, multiple refspecs, tag/all/mirror/delete pushes,
`push.followTags`, quoted/ambiguous syntax or custom multi-ref push configuration fail closed:
otherwise a checkbox
stamped for local HEAD could publish unaudited bytes from another repository or ref.

For a new PR, a separately authorized publisher supplies that record in the one exact body source;
for an existing PR, a separately authorized PR-body edit precedes push. Duplicate, dynamic, hidden,
fenced, quoted-example, HTML, blockquote, stdin, redirection, symlink, foreign-repo, stale-head, or
ambiguous records fail closed. If GitHub state is unreadable, publishing fails closed. Any later
commit stales the record. `skill-audit ⊂ project-review`: clean `$project-review` may establish
readiness for both records, while `$skill-audit` establishes only its own.

## Report structure

```
# Skill audit — tesla-key-esp32 (<date>)

## Summary
<1–3 sentences: how many canonical skills and reviewers were checked; how many drifted.>

## Findings
For each drift, in priority order:
### [SKILL-DRIFT] <skill/agent> — <short title>
- **Where:** `.agents/…` or `.codex/…:line` → ground truth: `path:line`
- **What:** the fact it asserts vs. what the project actually says
- **Proposed fix:** the exact edit; not applied during this audit

## Coverage
<Every canonical skill and reviewer: ✓ matches project / ✗ drifted.>

## Gate
<ready record for a separately authorized PR edit | withheld — drift still open>
```

Keep each finding tight and tied to a named project fact. A clean run is a valid outcome: every
canonical skills/reviewers ✓, zero edits, gate record ready but not inserted.
