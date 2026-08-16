# Runner-neutral agent migration and canary runbook

This repository is migrating from a Claude-centred layout to a runner-neutral policy layer. The
migration changes development tooling only; it does not change firmware behavior, ESP-IDF,
`yoziru/tesla-ble`, the patch series, supported targets, partitions, pairing/session compatibility,
vehicle commands, signing, OTA, Release, or Pages contracts.

## Canonical layout

- [`AGENTS.md`](../AGENTS.md) is the concise policy and authorization source of truth.
- [`.agents/skills/`](../.agents/skills/) contains the canonical runner-neutral skills. Invoke them
  as `$skill-name`.
- [`.codex/config.toml`](../.codex/config.toml), [`.codex/hooks.json`](../.codex/hooks.json), and
  [`.codex/agents/`](../.codex/agents/) configure Codex and read-only specialist reviewers.
- [`tools/agent-hooks/`](../tools/agent-hooks/) is the single hook and PR-gate policy core.
- [`tools/agent-config/`](../tools/agent-config/) contains parser-based configuration checks and
  mutation self-tests used by the existing `logic-test` CI job.
- [`.codex/migration-manifest.json`](../.codex/migration-manifest.json) maps every tracked legacy
  `.claude` file exactly once and fingerprints the sorted path and raw-byte stream.

`.claude/` remains an active compatibility layer throughout the canary. Its settings and scripts
must be thin adapters or explicitly mapped legacy inputs, not an independent policy
implementation. Do not delete, bulk-rewrite, or bypass it. Any change to a tracked `.claude` body
changes the fingerprint and must trigger a fresh compatibility review.

## Authorization and evidence boundaries

Analysis, review, diagnosis, and triage are read-only unless the user explicitly requests an
implementation. Implementation does not itself authorize commit, push, PR creation, merge,
release, flash, OTA, NVS access, hardware operations, live device calls, or vehicle commands.
Nothing may wake the vehicle without explicit authorization. NVS dumps, vehicle private keys,
BLE sessions, credentials, captures, and OTA signing keys are secret material.

Keep evidence lanes separate:

| Lane | What counts |
|---|---|
| Host | Parsers, syntax checks, self-tests, mock logic tests, and local diffs that actually ran |
| CI | The exact PR head and named jobs/checks that completed remotely |
| Signing | Disposable-key contract tests, or separately authorized proof from the protected signer |
| Hardware | A named board/target and the exact authorized flash/device observation |
| Vehicle | A separately authorized live interaction that records sleep/wake and command impact |
| UI | A rendered browser/device surface, not source inspection alone |

One lane is not proof of another. In particular, green host/CI checks do not prove hardware,
vehicle, flash, OTA, genuine signing, Release, Pages, or visual behavior.

## Local and CI verification

Run the agent configuration entry point before treating a migration change as locally complete:

```bash
tools/agent-config/selftest.sh
```

The suite validates the AGENTS size budget, migration manifest and fingerprint, skill frontmatter,
TOML/JSON configuration, read-only/model-independent reviewers, hook events and adapter wiring,
the Context7 pin, and canonical safety invariants. The `logic-test` CI job calls the same entry
point before host logic and build-contract tests. Hooks are lexical defense in depth and do not
replace the sandbox, authorization rules, GitHub protection, CI trust boundaries, or review.

## Phase 7 canary (manual; not authorized by the migration itself)

Publication and merge remain blocked until the user gives a new explicit authorization. After
that authorization, use this sequence without skipping steps:

1. Start a fresh Codex session in the repository.
2. Open `/hooks` and inspect every loaded command, matcher, timeout, blocking/async flag, and
   source path.
3. Trust only the exact current hook-definition hash. Record it with the canary evidence.
4. After any hook or adapter change, discard that result and repeat in another fresh session.
5. Start a fresh Claude session and exercise the same allow/block decisions through the retained
   adapters, including malformed payload and mutation canaries.
6. Run the exact candidate PR head through remote CI. Confirm the unprivileged PR boundary and
   the unchanged `logic-test -> build -> publish -> deploy` dependency chain.
7. Run `$skill-audit`, `$project-review`, and conditional `$feature-docs` against that exact head.
   Stamp only that reviewed head; any new commit invalidates the records.
8. Only after the explicit publication instruction may an operator commit, push, create a PR, or
   use the sole merge form:

   ```bash
   gh --repo github.com/0Bu/tesla-key-esp32 pr merge <numeric-pr> \
     --match-head-commit <full-40-hex-head-sha> --squash
   ```

   Administrative, automatic, merge-commit, rebase, queue, REST, GraphQL, MCP, foreign-host, and
   foreign-repository merge forms remain blocked.

Do not use the canary to access the real OTA key, sign or publish a PR artifact outside the
existing protected workflow, flash/OTA a board, access NVS, or contact the vehicle unless each
operation is separately and explicitly authorized.

## Rollback

Rollback is configuration-only and preserves history:

1. Disable the new project `.codex` configuration for the affected runner/session.
2. Fall back to the retained `.claude` compatibility entry points.
3. Do not delete legacy files, rewrite history, reset user changes, or weaken signing/PR gates.
4. Capture the failing definition hash, payload, decision, and self-test output for correction.
5. Re-enter the canary from a fresh session after the fix.

Removing `.claude/` is not part of this migration. Retirement requires a separate request, a new
inventory/fingerprint review, and independent approval after the canary has completed.
