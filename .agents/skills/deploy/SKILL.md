---
name: deploy
description: Verify and independently review a committed tesla-key-esp32 change locally before pushing, record the results in the PR, and squash merge the verified head. Follow the Dev build and delegate firmware acceptance on the intended USB bench to deploy-bench.
---

Invoke `$deploy`. Read [AGENTS.md](../../../AGENTS.md) first. The user's explicit invocation
authorizes the workflow below; automatic selection by an agent does not. Canonical skills live
under `.agents/skills/`.

# deploy — commit, push, PR and merge

Take the scoped change through verification, commit, push, PR creation and canonical squash merge.
Follow the resulting main build. Use [$deploy-bench](../deploy-bench/SKILL.md) for USB flash and
acceptance on the intended test board. Production delivery belongs to [$ship](../ship/SKILL.md).

## Authorization and baseline

The user's own `$deploy` invocation is explicit user authorization for the current task's local
checks, commit, push, PR creation/updates and earned gate records, independent reviews, canonical
squash merge of the verified head, protected Dev signing/publication and the standard
`$deploy-bench` scope on the intended USB bench. Carry that approval into the delegated USB preflight,
signed app-only flash, final `otadata` activation and same-board passive acceptance. Honor any
narrower scope, such as repository-only deployment. Ask only for missing target information or
additional scope. Stable Releases, production delivery, OTA, NVS/identity changes, cluster and
vehicle actions require separate user authorization.

Inspect status, branch, `origin/main`, merge-base and intervening changes. Preserve user changes and
other tasks' worktrees. Stage only this change; do not reset, stash or rewrite history. If the user
requires a bench test before merge, hand the exact candidate and the existing authorization to
`$deploy-bench` while the PR is open. Resume only after its required acceptance. A new push
invalidates that candidate's evidence.

## Prepare and verify the candidate

Finish the scoped implementation and relevant checks. Synchronize
[$feature-docs](../feature-docs/SKILL.md) when relevant, then stage only the intended files and
commit with a concise English message. Ensure the clean committed candidate includes the current
`main` before verification so the local checks cover the integration result.

Run `scripts/check-local.sh --candidate`. Add `--firmware` for firmware, installer runtime and
build/signing pipeline changes that need the four-target release contract. This verifies
compilation, size/stack, reproducibility and
disposable-key signing on all four targets in the pinned container. An unavailable required check
stops the push. Record the command, result, candidate SHA, base SHA and applicable target evidence
alongside the `$local-checks` checkbox.

Complete independent [$skill-audit](../skill-audit/SKILL.md) and
[$project-review](../project-review/SKILL.md) reviews and the conditional
[$vehicle-command-audit](../vehicle-command-audit/SKILL.md) on that candidate. Confirm any required
feature catalog sync against the same commit. Run [$pr-hygiene](../pr-hygiene/SKILL.md) against the
candidate and the intended PR title/body, including the test and review evidence. Keep the wording
reviewed when adding its final hygiene record. Resolve findings and repeat verification for a new
candidate before pushing.

## Record local results and push

Prepare the English PR title/body from the [PR template](../../../.github/PULL_REQUEST_TEMPLATE.md).
Set each earned checkbox to the same full candidate SHA. For an existing PR, preserve unrelated
edits and publish the earned records for the proposed candidate before pushing. Check the proposed
body locally with `tools/agent-hooks/require-pr-gates.sh --check --body-file <path> --head-sha
<full-40-hex-sha> --changed-files-file <path>` using the complete rename-expanded diff.

Push the literal intended branch. Open a PR with that title and body if one is needed. Use
`gh pr create --body-file <path>` or `gh pr edit <numeric-pr> --body-file <path>` for multiline
bodies. Confirm the remote head and PR text match the locally verified candidate and wording.
Wait for GitHub's required checks. On ordinary PRs, `current-head-records` validates the recorded
results and the `build` check is skipped. An explicitly requested signed PR preview runs its
artifact production and signing checks in GitHub Actions.

## Merge the exact reviewed head

Re-read the numeric PR, full head, base, mergeability, required checks and any required bench
evidence. If the checked source or base changed, verify the new integration candidate locally
before another push. If the PR title or body changed, recheck hygiene locally. Verify the
current records with `tools/agent-hooks/require-pr-gates.sh --pr <numeric-pr> --check`.
Bind the user's workflow authorization to that concrete PR/head after the required verification;
do not ask for another merge approval when the user already instructed `$deploy` for this task.
Execute only the canonical command with literal values:

```text
gh --repo github.com/0Bu/tesla-key-esp32 pr merge <numeric-pr> --match-head-commit <full-40-hex-head-sha> --squash
```

No admin, auto, queue, alternate API, merge-commit or rebase variant. A changed head requires fresh
verification before integration.

## Observe main and complete bench acceptance

Capture the resulting merge commit. Follow exactly its main `build` PUSH run with `gh run watch
<run-id> --exit-status`, using the run selection in [$ship](../ship/SKILL.md).
A red run stops completion and needs diagnosis. For a firmware deployment, pass the exact signed
artifact from that main run to `$deploy-bench`, unless the user requested repository-only scope
or the accepted pre-merge bench run already satisfies the requested test. If the bench is unresolved,
finish the authorized repository phases and ask only for the missing board or isolation details. A
missing eligible artifact or failed board, layout, or bootloader check stops the USB path. Report
which artifact was accepted and whether it came from the PR head or the resulting main build.

Report the PR, reviewed head, merge SHA, CI result, Dev publication and bench acceptance separately.
Keep diagnostic evidence and other tasks' workspaces. Branch deletion or broader cleanup requires
its own applicable authorization.
