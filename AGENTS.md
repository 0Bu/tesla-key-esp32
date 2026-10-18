# tesla-key-esp32

ESP-IDF firmware exposing a Tesla BLE Charging Manager key through local HTTP/MCP, evcc,
MQTT/Home Assistant, a web UI and signed OTA. This is the canonical policy for every coding runner.
Read task-specific contracts through the [documentation index](docs/README.md).

## Authorization and ownership

- Analysis, review, audit, diagnosis and triage are read-only by default: inspect and test, then report.
  They do not authorize edits, formatting, commits, pushes, PR changes, merges, releases,
  publication, signing, flash, OTA, NVS operations or live device/vehicle contact.
- An implementation request permits scoped repository edits and proportionate local tests; it does
  not authorize commit, push, PR/stamp, merge, release, Pages, real-key signing or physical/live
  mutation. Obtain explicit authorization for each phase; one approval does not grant another.
- A live read can wake the car. Default to collected/local evidence; require explicit user authorization
  for the target and live scope before contacting a board, vehicle or evcc endpoint. Command and charge-toggle scopes
  are separate. Never pair, wake, reboot, erase evidence or change identity from a review request.
- Preserve user changes. Before integration inspect status, `origin/main`, merge-base and
  intervening changes. Do not reset, stage, stash, overwrite or rewrite history without that
  exact authorization.
- The lead owns requirements, architecture, integration and conflicts. Implementers have disjoint
  paths. Serialize shared builds, signing, hardware, vehicle and Git operations. Final certification
  comes from an independent read-only reviewer, not the implementer of that area.
- P1/P2 security, signing, NVS, pairing, partition or vehicle-safety findings stop completion until
  fixed or escalated.

## Fixed contracts

- Targets: exactly `esp32`, `esp32s3`, `esp32c3`, `esp32c6`; never add, omit or silently skip one.
  `scripts/ci-build-all.sh` is the CI entry point.
- `esp-idf-toolchain.txt` pins ESP-IDF **v6.1** and its container digest. Use
  `scripts/idf-docker.sh`; preserve its **1.5 CPU / 1800 MiB** limits. Do not substitute host IDF.
- `main/idf_component.yml` pins `yoziru/tesla-ble` **v6.2.0**. The ordered, hash-checked series in
  `patches/tesla-ble/` is fail-closed. Pins, patches, locks, trims, wire behavior and key compatibility
  require a separately authorized dependency migration and protocol-vector review.
- `teslamotors/vehicle-command` is normative for BLE protocol behavior. Record every departure in
  [ADR-0005](docs/adr/0005-tesla-ble-seam.md). PSA changes must preserve ECDH byte order,
  `SHA1(shared-secret)[:16]`, HMAC/session derivation, nonce/AAD/tag layout, response-counter tag
  verification, key-ID derivation and existing PEM/NVS keys. Prove them with V1 harness vectors;
  [ADR-0002](docs/adr/0002-idf6-mbedtls4-crypto-seam.md) owns rationale.
- Agent/configuration-only work must not alter firmware behavior, target set, dependencies,
  partitions, BLE commands, pairing or sessions.

## Implementation and evidence

Follow [main/AGENTS.md](main/AGENTS.md), [main/logic/AGENTS.md](main/logic/AGENTS.md),
[protocol rules](.agents/rules/ble-tesla-protocol.md) and
[size rules](.agents/rules/firmware-size-budget.md) for their scopes.

The critical memory measure is INTERNAL **largest contiguous free block**, not total free heap. Use bounded responses,
check allocation failures, and contain all exceptions at HTTP/MCP, C callbacks and task boundaries.
Follow the [lock/task contract](docs/ARCHITECTURE.md#concurrency-normative-contract): snapshot leaf
state under lock, use after unlock; no logging, network or storage work under leaf cache/result locks.
Background status/display/telemetry consume caches and preserve sleep debounce, retry/backoff and
foreground-command arbitration. Reboot loops are safety failures.

Pure decisions belong in `main/logic/`, with host tests and registered ownership. New untracked logic
or tests must not evade the Stop-hook test decision. Firmware integration also requires pinned IDF
compilation; host tests do not compile the ESP/NimBLE/NVS/FreeRTOS shells.

Use the narrowest relevant checks, then the full contract before handoff:

```bash
scripts/run-mock-tests.sh
scripts/run-mock-tests.sh --require-all
scripts/repo-lint.sh
scripts/run-sanitizer-tests.sh
scripts/test-build-contracts.sh
scripts/test-pr-gates.sh
tools/agent-config/selftest.sh
scripts/idf-docker.sh ./scripts/ci-build-verify.sh "$(cat version.txt)" "$(git rev-parse HEAD)"
```

Run edited-format syntax, relative Markdown links and `git diff --check`. Use only a generated
throwaway RSA key for signer tests. [Development](docs/DEVELOPMENT.md) explains build contracts.
Report host, four-target build, disposable signing, remote CI, USB/hardware, OTA, live vehicle/API
and browser/visual evidence separately. Mark unexercised boundaries **not run**; compilation never
proves signed delivery, hardware, pairing, key reuse, sleep/wake or release acceptance.

## Secrets and delivery

NVS is secret material: WiFi, VIN, MQTT settings, private keys and sessions. Never print, upload, attach, archive,
diff or casually inspect a dump, or put one in the repository. Global `nvs-backup`, `nvs-restore`
and `nvs-delete` skills stay outside this project.

`partitions.csv` is immutable without an explicit migration: NVS `0x9000/0x6000`, otadata
`0xf000/0x2000`, OTA starts `0x20000` and `0x210000`, each `0x1f0000` bytes.
Ordinary OTA must preserve keys/sessions. NVS erase, forced generation, restore or identity change
needs explicit approval and a backup/rollback plan. Firmware rollback does not recover NVS.

Unsigned images are not flashable. Flash/OTA requires explicit scope plus signed-image verification.
The real `OTA_SIGNING_KEY` belongs only to protected publication; PR code, hooks and tests never
receive it. Never read, copy, materialize or use the offline key. The hook permits only an explicitly
authorized, unchained `espsecure.py sign_data`/`sign-data` keyfile command; no wrapper, pipe,
redirect, copy, archive or compound use.

Preserve the [signing/publication contract](docs/SECURITY.md#release-pipeline-and-trust-boundaries):
unprivileged unsigned build, independent exact-source rebuild, protected signing, key removal,
production RSA-PSS pin, Release/Pages byte identity, immutable same-SHA key-free reuse and signed
preview revalidation/cleanup. Key/pin changes need a reviewed USB fleet migration; software TOFU
cannot rotate authority over OTA. Live evcc checks use `scripts/e2e_evcc.sh` with explicit scopes.

## Documentation

Use English, plain precise prose and the minimum sufficient explanation. Each document has a clear
purpose; each topic has one owner. Keep warnings beside risky steps, with deeper contracts linked.
Update the owner with the changed code/configuration. Do not duplicate technical catalogs here.

Skills use `$skill-name` and `.agents/skills/`; legacy `/skill-name` is historical only.
Hook policy lives in `tools/agent-hooks/`, configuration in `.agents/` and `tools/agent-config/`.
Native `.codex/` registrations are generated adapters, not a second policy. Check/regenerate with
`node tools/agent-config/sync-codex.mjs --check` / `--write`. Codex hooks require project and current
hook trust through `/hooks`; linked worktrees load declarations from the primary checkout. Keep its
reviewed setup in sync or run gates explicitly. Reviewer read-only/no-escalation defaults do not
replace inspection of parent runtime permission overrides. `scripts/with-host-python.sh` selects a
capable host Python for gates. Hooks are lexical defense in depth, not authorization or sandboxing.

## PR and review

- `$skill-audit` is required before PR creation or push. `$project-review` is required before every
  merge and always assesses documentation quality; unresolved material `DOC-QUALITY` findings
  prevent clean. `$pr-hygiene` is required at PR creation, every push, and every merge. It checks
  privacy and English independently of the other reviews.
- `$feature-docs` is required when its cataloged surface changes, including native `.codex/` adapters
  and `scripts/with-host-python.sh`. `$vehicle-command-audit` is
  required when the BLE/command relevance classifier in `tools/agent-hooks/pr-gate-lib.sh` applies.
  Consult those skills for exact scope; no silent relevance exemptions.
- Strict Renovate maintenance (`gate_is_renovate_maintenance`, only `.github/workflows/renovate.yaml`
  or `.github/renovate.json`) is exempt from manual records. Dependency updates are not exempt.
- Records name the exact current PR head and expire after every push. Local hooks/scripts enforce
  advisory records; server-side enforcement needs the required `pr-policy / current-head-records`
  check. Report findings with severity, path/line, cause, impact and evidence; resolve P1/P2 and
  rerun the affected independent review.
- The only accepted, separately authorized merge shape is:

  ```text
  gh --repo github.com/0Bu/tesla-key-esp32 pr merge <numeric-pr> --match-head-commit <full-40-hex-head-sha> --squash
  ```

  No admin/auto/queue, merge/rebase shape, alternate API/repository or compound mutation prefix.
  Local implementation completion grants no publication approval.
