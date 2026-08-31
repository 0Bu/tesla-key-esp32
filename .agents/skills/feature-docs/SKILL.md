---
name: feature-docs
description: Keep docs/FEATURES.md (this firmware's technical-feature catalog) in sync when a platform feature lands or changes — including runner-neutral agent policy under AGENTS.md, .agents/, .codex/, .claude/, .github/PULL_REQUEST_TEMPLATE.md, tools/agent-hooks/, and tools/agent-config/. Use after implementing or changing a technical feature, before opening the PR. Required before merging a PR whose diff reaches those agent-policy paths, main/, test/, sdkconfig.defaults, partitions.csv, the shipped Pages runtime, release-relevance.sh, or the build/signed-preview/preview-cleanup/PR-policy/bench-acceptance workflows.
---

> **Canonical runner-neutral skill.** Read [`AGENTS.md`](../../../AGENTS.md) before acting.
> Project skills are canonical under [`.agents/skills/`](../), and lifecycle/PR policy is
> enforced by the runner-neutral core under [`tools/agent-hooks/`](../../../tools/agent-hooks/).
> This skill does not grant permissions beyond the user's explicit request.
> Invoke this workflow canonically as `$feature-docs`.

# feature-docs — sync docs/FEATURES.md with what the firmware actually does

`docs/FEATURES.md` is the cross-cutting catalog of what **tesla-key-esp32** implements at the
*platform* level: the ESP-IDF capabilities, security mechanisms, network behaviour and diagnostic
surfaces, including Tesla/BLE mechanisms when they enforce a cross-cutting safety contract. It
answers "does this device do X, and where does X live?" for someone who has not read the whole tree.

It rots in a way nothing else catches. A feature lands, every test passes, every narrative doc
stays correct about its own area — and the catalog simply does not mention the new thing. Six months
later the entry that would have said *"this exists, and here is the failure it prevents"* is not
there, so the mechanism looks optional and gets deleted, or gets built a second time.

[`tools/agent-hooks/require-pr-gates.sh`](../../../tools/agent-hooks/require-pr-gates.sh) gates a merge on this having been run — but only when the
PR's diff reaches `main/`, `test/`, `sdkconfig.defaults*`, `partitions.csv`, runner-neutral
agent policy (`AGENTS.md`, `.agents/`, `.codex/`, `.claude/`,
`.github/PULL_REQUEST_TEMPLATE.md`, `tools/agent-hooks/`, `tools/agent-config/`) or
the shipped Pages runtime (`docs/index.html`, `installer-bootstrap.mjs`,
`serial-port-release.mjs`, `web-installer.mjs`, `docs/vendor/`) or
`.github/workflows/{build,signed-pr-preview,pr-preview-cleanup,pr-policy,bench-acceptance}.yml`, or the cumulative
Release/Pages-baseline classifier `scripts/release-relevance.sh`. Other docs-only or unrelated
script/workflow PRs clear without ceremony. The shared gate library and every function this hook
uses are load-checked first; missing or truncated gate code blocks the merge.

This skill may edit `docs/FEATURES.md` only when the user's implementation/documentation request
includes that change. In an audit or review, report drift without editing. It never commits, pushes,
opens/edits a PR, stamps a PR body, merges, releases, flashes, or starts OTA unless the user grants
that separate authority through the appropriate workflow.

## What to do

1. **Read the diff.** `git diff origin/main...HEAD --stat`, then the files that matter.

2. **Decide whether a technical feature moved.** It did if the change:
   - adds/removes an ESP-IDF component (`main/idf_component.yml`, `REQUIRES` in `main/CMakeLists.txt`)
   - adds/changes an `sdkconfig.defaults*` capability (watchdogs, coredump, security, TLS, PSRAM…)
   - adds/changes an HTTP route, an OTA rule, a security mechanism, a network behaviour or a
     diagnostic surface
   - changes the shipped Web-Installer runtime, its local dependency bundle or its serial/manifest
     safety contract
   - changes how CI decides whether current main still contains Release/Pages-unpublished firmware,
     or changes the single branch-backed Pages authority/source validator
   - changes runner-neutral instructions, skills, reviewer/config registration, lifecycle gates, or
     their validators under `AGENTS.md`, `.agents/`, `.codex/`, `.claude/`,
     `.github/PULL_REQUEST_TEMPLATE.md`, `tools/agent-hooks/`, or `tools/agent-config/`
   - adds a new `main/logic/` header, or a new `main/*.cpp` subsystem
   - changes `partitions.csv`
   - turns a stub into something real (or the reverse)

   It did **not** if the change is a bug fix inside an existing mechanism, a doc edit, a test-only
   change, or a refactor with no behavioural surface. Say so and stop — an entry per commit is how a
   catalog becomes unreadable.

3. **Write or update the entry.** Each row of the tables in `docs/FEATURES.md` is
   `Feature | Where | Prevents`. All three columns are load-bearing:
   - **Feature** — what it IS, in the reader's vocabulary, not the identifier's.
   - **Where** — the file(s) someone would open. Name the pure `logic/` header AND its glue when
     both exist, since that split is this project's convention and a reader needs both halves.
   - **Prevents** — *the failure this exists to stop*. This is the column that decides whether the
     feature can be safely removed later, and the one that is hardest to reconstruct afterwards.
     Write the concrete failure ("a reboot nobody can attribute", "advertising a download the
     decoder will reject"), never a restatement of the feature ("provides crash reporting").

   Do **not** invent history. If a mechanism was built because of a measured incident that is
   recorded in the tree, cite it; if it was built pre-emptively, describe the failure mode without
   claiming it happened.

4. **Check the section still fits.** The file is organised by concern (boot/crash, configuration,
   security, network, diagnostics, build/CI). If a new feature does not fit any section, that is
   worth a moment's thought before adding a section — usually it belongs in one of them.

5. **Check the cross-references.** If the feature is also described in
   `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/SECURITY.md` or `README.md`, make sure they
   agree. FEATURES.md is a catalog, not a second narrative: it should point at the deep reference
   rather than duplicate it.

6. **Special case — the static-analysis section.** `docs/FEATURES.md` ends with a *measured*
   rejection of adding a static analyser. If a change alters that measurement (a new lint class, a
   new warning flag pinned in `main/CMakeLists.txt`), re-run the measurement rather than editing the
   numbers:
   ```bash
   clang-tidy --quiet --checks='-*,bugprone-*,clang-analyzer-*,cert-*,performance-*' \
     test/test_logic.cpp -- -std=c++17 -Imain 2>/dev/null | grep -c "warning:"
   g++ -std=c++17 -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Imain \
     -fsyntax-only test/test_logic.cpp
   ```
   A number in that section that nobody re-measured is worse than no number.

7. **Report gate readiness; do not mutate the PR by default.** After a successful sync, provide the
   exact top-level CommonMark record for a separately authorized PR-body update:
   ```
   - [x] `$feature-docs` synced — merge gate @ <full-40-hex-sha>
   ```
   The stamp must be the exact lowercase 40-hex PR head; prefixes are rejected. A later commit
   forces a fresh sync. An
   implementation or documentation request does not authorize editing the PR body or merging it.
   The only canonical merge command is
   `gh --repo github.com/0Bu/tesla-key-esp32 pr merge <numeric-pr> --match-head-commit <full-40-hex-head-sha> --squash`.
   Foreign URLs/repos/hosts, implicit selectors, structured/MCP merge actions, compound commands,
   `--admin`, `--auto`, `--merge`, `--rebase`, and missing/wrong/duplicate head guards fail closed.

## What this skill is not

It does not review the feature. Correctness, memory safety and cross-cutting coherence are
`$project-review`'s job; per-target build divergence is the `multi-target-build-reviewer` agent's.
This skill answers exactly one question: **can the next reader find out that this exists, and why?**
