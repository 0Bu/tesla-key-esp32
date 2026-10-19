---
name: deploy
description: Publish a tesla-key-esp32 change through local checks, commit, push, PR creation, independent reviews, CI and canonical squash merge. Observe the resulting main build; USB bench testing belongs to deploy-bench and device delivery is a separate workflow.
---

Invoke `$deploy`. Read [AGENTS.md](../../../AGENTS.md) first; this workflow grants no
additional authorization. Canonical skills live under `.agents/skills/`.

# deploy — commit, push, PR and merge

Take the scoped repository change through verification, commit, push, PR creation and canonical
squash merge. Follow the resulting main build so its source and publication outcome are known.
Use [$deploy-bench](../deploy-bench/SKILL.md) for USB flashing and testing the implementation.
Use [$ship](../ship/SKILL.md) when separately requested to deliver a signed main image to a device.
This workflow ends without board, vehicle, cluster, USB or OTA operations.

## Authorization and baseline

Require explicit user authorization for commit, push, PR creation/updates, merge and the protected
Dev signing/publication side effect of a firmware-relevant main push. Reuse authorization already
provided for these phases; resolve only missing scope. A stable Release requires separate manual
release authorization and is not started here.

Inspect status, branch, `origin/main`, merge-base and intervening changes. Preserve user changes and
other tasks' worktrees. Stage only this change; do not reset, stash or rewrite history. If the user
requires a bench test before merge, hand the exact candidate to `$deploy-bench` while the PR is
open and resume only after its required acceptance. A new push invalidates that candidate's evidence.

## Verify and prepare the commit

Run the narrow checks for the change and the full applicable contract from
[AGENTS.md](../../../AGENTS.md#implementation-and-evidence). Firmware integration requires the
pinned four-target build, size/stack checks and disposable-key signing/reproducibility evidence;
host tests alone are insufficient. Report unavailable checks as **not run** and stop at an unmet gate.

Run [$skill-audit](../skill-audit/SKILL.md) and [$pr-hygiene](../pr-hygiene/SKILL.md) before PR
creation or push. Synchronize [$feature-docs](../feature-docs/SKILL.md) when relevant. A validator or
digest update does not replace an actual review. Fix findings, then stage the intended files and
commit with a concise English message.

## Push, create the PR and complete CI

Push with the literal intended branch name. Create or update the scoped PR using an English
summary, validation evidence and the [PR template](../../../.github/PULL_REQUEST_TEMPLATE.md).
For multiline bodies use a file and `gh pr create --body-file <path>` or
`gh pr edit <numeric-pr> --body-file <path>`.

Records must name the full current head SHA. Only record reviews actually completed; leave
unearned records unchecked. For an existing PR, the pre-push hook requires the new local head's
skill-audit and hygiene records before push. An authorized PR-body update may publish those earned
records; do not reuse old-head results.

Monitor CI with `gh pr checks <numeric-pr> --watch` or [$ci-heal](../ci-heal/SKILL.md).
For each fix, rerun the affected checks and reviews, commit and push, then refresh all expired
records. CI-heal neither authors fixes nor supplies independent review. Continue until the exact
head is green and its current records are satisfied.

## Merge the exact reviewed head

Before every merge, complete [$project-review](../project-review/SKILL.md), including documentation
quality, and a fresh `$pr-hygiene` review. Require `$feature-docs` and
[$vehicle-command-audit](../vehicle-command-audit/SKILL.md) when the actual diff classifier applies.
Resolve material findings and update only earned records under the existing PR-update authorization.

Re-read the numeric PR, full head, base, mergeability, CI and required bench evidence. Verify the
current records with `tools/agent-hooks/require-pr-gates.sh --pr <numeric-pr> --check`.
Bind the merge authorization to that concrete PR/head, using existing scope when it already does.
Execute only the canonical command with literal values:

```text
gh --repo github.com/0Bu/tesla-key-esp32 pr merge <numeric-pr> --match-head-commit <full-40-hex-head-sha> --squash
```

No admin, auto, queue, alternate API, merge-commit or rebase variant. A changed head requires fresh
verification before integration.

## Observe main and report

Capture the resulting merge commit. Follow exactly its main `build` PUSH run with `gh run watch
<run-id> --exit-status`, using the run selection in [$ship](../ship/SKILL.md).
A red run stops completion and needs diagnosis. Report whether Dev signing/publication ran or was
skipped; no stable Release is implied. Do not continue into `$ship`'s device-delivery sections.

Report the PR, reviewed head, merge SHA, CI result and any pending bench or delivery acceptance.
Keep diagnostic evidence and other tasks' workspaces. Branch deletion or broader cleanup requires
its own applicable authorization.
