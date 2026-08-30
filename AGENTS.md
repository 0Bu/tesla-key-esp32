# tesla-key-esp32

`tesla-key-esp32` is ESP-IDF firmware that exposes a Tesla BLE key through a local HTTP/MCP API,
an evcc-compatible API, MQTT/Home Assistant, a web UI and signed OTA updates. This file is the
compact, canonical instruction set for every coding runner. Read deeper references only when the
task touches them:

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md): firmware architecture, task/lock ownership,
  HTTP exception containment, OTA, MQTT, pairing and sleep/link-state behavior.
- [`docs/SECURITY.md`](docs/SECURITY.md): threat model, NVS/key handling, signing and deployment
  trust boundaries.
- [`docs/FEATURES.md`](docs/FEATURES.md): technical feature catalog and documentation gate.
- [`docs/MCP.md`](docs/MCP.md): MCP wire contract and command/read-only boundaries.
- [`test/README.md`](test/README.md): host-test scope, entry points and hardware exclusions.

## Authorization and ownership

- Analysis, review, audit, diagnosis and triage are **read-only by default**. They authorize
  inspection and tests, not edits, formatting, commits, pushes, PR changes, merges, releases,
  flashing, OTA, NVS operations, live vehicle calls or commands.
- An implementation request authorizes scoped repository edits and proportionate local tests. It
  does **not** automatically authorize commit, push, PR creation/update, merge, release, Pages
  publication, signing with a real key, flash, OTA, NVS mutation or vehicle/device mutation.
- Obtain explicit user authorization for each publication or physical/live mutation boundary.
  Never infer it from `fix`, `review`, `ship`, a green test, or an earlier unrelated approval.
- Preserve user changes. Before integration inspect `git status --short --branch`, the current
  `origin/main`, the merge-base and intervening changes. Never reset, stage, stash, overwrite or
  rewrite history unless the user explicitly requests that exact action.
- The root/lead agent owns requirements, architecture, integration and conflict resolution. Give
  implementers non-overlapping paths. Shared builds, signing, hardware, vehicle and Git operations
  are serialized. Final certification must come from an independent read-only reviewer, not the
  role that implemented the same area.
- A P1/P2 security, signing, NVS, pairing, partition or vehicle-safety finding is a stop condition.
  Fix or escalate it before claiming completion.

## Fixed platform and dependency contract

- Supported targets are exactly `esp32`, `esp32s3`, `esp32c3` and `esp32c6`. Do not add, drop or
  silently skip one. `scripts/ci-build-all.sh` is the four-target CI entry point.
- [`esp-idf-toolchain.txt`](esp-idf-toolchain.txt) pins ESP-IDF **v5.5.5** and its container digest.
  Use the repository wrappers; do not substitute a host IDF or move to ESP-IDF 6 as part of an
  unrelated task.
- [`main/idf_component.yml`](main/idf_component.yml) pins `yoziru/tesla-ble` **v5.1.2**.
  [`patches/tesla-ble/`](patches/tesla-ble/) is an ordered, hash-checked, fail-closed local series:
  the anti-replay response-counter fix, key-regeneration/persistence API adaptation, and bounded
  RX-framing recovery logging are current contracts, not obsolete C5 workarounds. Do not edit the
  pin, patch order, wire behavior, key compatibility or log-flood throttle without a separately
  authorized dependency migration and protocol-vector review.
- ESP-IDF 6/Mbed TLS 4/PSA work is intentionally separate. Preserve P-256 ECDH byte order,
  `SHA1(shared-secret)[:16]`, HMAC/session derivation, AES-GCM nonce/AAD/tag layout, Tesla key-ID
  derivation and PEM/NVS key reuse. See
  [`docs/adr/0002-idf6-mbedtls4-crypto-seam.md`](docs/adr/0002-idf6-mbedtls4-crypto-seam.md).
- Do not change BLE wire formats, pairing/session semantics, command meaning, dependency locks,
  target set or firmware behavior during agent/configuration-only work.

## Environment and evidence boundaries

- `scripts/idf-docker.sh` is the pinned ESP-IDF boundary. A host mock result is not an IDF build;
  an IDF build is not a signed image; a signed image is not a flash; a flash is not runtime or
  vehicle proof.
- A remote runner may have neither Docker nor USB. Report capabilities rather than manufacturing
  evidence. Do not bypass a missing boundary with an unpinned toolchain.
- Keep evidence separate in reports: host tests, four-target CI-equivalent build, disposable-key
  signing contract, remote CI, hardware/USB, OTA, vehicle/live API and browser/visual checks.
  State **not run** for every boundary not exercised.
- Never claim hardware, flash, OTA, vehicle, sleep/wake, pairing, key reuse or release success from
  compilation or host tests. Never claim remote CI from a local CI-like run.

## Build and test entry points

Use the narrowest relevant checks first, then the full contract before handoff:

```bash
scripts/run-mock-tests.sh                    # capability-aware local host suite
scripts/run-mock-tests.sh --require-all      # fail-closed CI host/CMake/browser suite
scripts/repo-lint.sh                         # offline syntax/link/workflow-policy canaries
scripts/run-sanitizer-tests.sh               # Linux ASan + UBSan + LSan + fuzz/runtime suite
scripts/test-build-contracts.sh              # pins, targets, partitions and CI/release contracts
scripts/test-pr-gates.sh                     # PR command/record policy canaries
tools/agent-config/selftest.sh                # runner mapping/config/hook mutation canaries
scripts/idf-docker.sh idf.py -B build set-target esp32s3 build
scripts/idf-docker.sh ./scripts/ci-build-verify.sh "$(cat version.txt)" "$(git rev-parse HEAD)"
                                               # four targets + disposable signing +
                                               # manifest/Pages bytes + reproducibility
```

Also run syntax checks appropriate to edited JSON, TOML, YAML, Python and Bash, relative Markdown
link checks, and `git diff --check`. Use a generated throwaway RSA key for signer-contract tests.
Never read, copy, materialize or use the real offline OTA signing key.

Pure decision logic belongs in `main/logic/` with host coverage in `test/test_logic.cpp`. A change
to logic or tests, including a new untracked file, must not evade the Stop-hook test decision.
Firmware-only integration still needs the pinned IDF build because host tests do not compile the
ESP HTTP, NimBLE, NVS, OTA or FreeRTOS shells.

## Memory, exceptions and concurrency

- On ESP32 the limiting heap measure is the **largest contiguous free block**, not total free heap.
  Avoid building large temporary `std::string`, JSON or HTML responses. Stream bounded chunks and
  check every allocation/error path. Keep payload and body limits explicit and host-tested.
- Every HTTP/MCP request path must remain inside the server's exception/OOM containment net.
  C-library allocation failures (for example cJSON returning null) need explicit handling because
  they do not throw. Never let an exception escape the HTTP task or convert malformed/oversized
  input into reboot loops.
- Do not allocate, log, perform network I/O, or call throwing code while holding a shared mutex.
  Follow the lock hierarchy and snapshot-under-lock/use-after-unlock pattern in
  [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). Review the largest-block effect and all unwind
  paths, not only the happy path or total heap.
- Prefer pure, bounded transformations in `main/logic/`; keep IDF handles, tasks and mutable state
  in the integration shell. Cross-task state must follow the documented owner/atomic/lock model.

## Keys, NVS, partitions and signing

- NVS is secret material. It contains WiFi, VIN, MQTT configuration, the vehicle private key and
  BLE sessions. Do not print, upload, archive, attach, diff, inspect casually or place an NVS dump
  in the repository. The global `nvs-backup`, `nvs-restore` and `nvs-delete` skills stay outside
  this project; never copy or modify them here.
- [`partitions.csv`](partitions.csv) is immutable without an explicit partition-migration request:
  `nvs` is offset `0x9000`, size `0x6000`; `otadata` is `0xf000`, size `0x2000`; `ota_0` begins at
  `0x20000`; `ota_1` begins at `0x210000`; each OTA slot is `0x1f0000`. Moving offsets can brick
  already-installed devices or destroy key/session data.
- Locally built unsigned images are **not flashable artifacts**. Flash/OTA requires explicit user
  authorization plus the project signed-image verification path. A build command never grants
  signing or flashing authority.
- The real `OTA_SIGNING_KEY` belongs only to the protected signing/publish trust boundary. PR code,
  unprivileged builds, agent hooks and tests must never receive it. The only hook-recognized
  keyfile use is an explicitly authorized, unchained `espsecure.py sign_data`/`sign-data` command;
  no wrapper, pipe, redirect, copy, print, archive, upload or compound command is allowed.
- Preserve CI's split: unprivileged build emits unsigned bytes and provenance; protected publish
  revalidates the exact main/release candidate before provisioning the key, signs already-built
  bytes, removes the key, and binds signed Release/Pages artifacts. Every production app must pass
  RSA-PSS verification against `scripts/ota-signing-public-key.sha256`; changing the key/pin is a
  separately reviewed USB fleet migration because software-only TOFU cannot rotate keys over OTA.
  A same-SHA retry with an exact immutable Release must use the key-free byte-verified reuse path;
  it must never sign, re-upload or mutate that Release again, but it does publish one new
  SHA-bound Actions recovery artifact from the verified bytes. Do not weaken action/privileged-job
  pins, symlink checks, signed size gates, preview revalidation or release/Pages byte identity.
- Pairing keys and sessions must survive ordinary OTA. Any NVS erase, forced key generation,
  restore or identity change is destructive and needs a backup/rollback plan plus explicit user
  approval. Never confuse firmware rollback with NVS/key recovery.

## Vehicle and live-device safety

- A live read is not automatically harmless: connecting or requesting stale data can wake the
  vehicle. Default diagnosis uses already-collected/local evidence. Do not contact a vehicle,
  evcc endpoint or device unless the user explicitly authorizes the live boundary and target.
- Live evcc end-to-end checks are host/cluster operations provided by the global
  `$tesla-key-e2e-evcc` skill. They do not belong to this versioned project skill set and retain
  their own explicit read, command, and charge-toggle authorization boundaries.
- Never send a vehicle command, pair, regenerate keys, change VIN, modify charge current, wake the
  car, reboot the board, flash, OTA or clear a crash/NVS artifact from a review or diagnosis task.
- Background telemetry, display and status code must consume cached state and must not introduce a
  hidden wake path. Preserve sleep debounce, backoff and foreground-command arbitration.
- Treat reboot loops as safety failures: repeated boot pairing/connect/command attempts can wake or
  drain the vehicle and can make recovery impossible. Validate failure throttling, safe mode,
  rollback and largest-block behavior when changing startup or networking code.
- A vehicle-command proposal needs three-way evidence: upstream Tesla/VCSEC behavior, this local
  implementation, and feasibility with the pinned patched `tesla-ble`. Do not infer support from a
  protocol name alone.

## Documentation sources of truth

- Architecture, concurrency, exception, HTTP, OTA, MQTT, sleep/link and pairing details:
  [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).
- Secrets, NVS, signing, CI trust boundaries and hardening: [`docs/SECURITY.md`](docs/SECURITY.md).
- Shipped technical features and their evidence: [`docs/FEATURES.md`](docs/FEATURES.md).
- MCP methods, read-only tools and command semantics: [`docs/MCP.md`](docs/MCP.md).
- Host-test scope and how to add logic coverage: [`test/README.md`](test/README.md).

Update the owning document with the code/configuration that changes its contract. Do not move long
API, architecture, incident or field references into this compact file. References to project
skills use `$skill-name` and canonical paths under `.agents/skills/`; `/skill-name` is accepted only
in historical PR records. Hook policy lives in `tools/agent-hooks/`.

## PR, review and merge discipline

- `$skill-audit` is required before PR creation or push. `$project-review` is required before every
  merge. `$pr-hygiene` is required at PR creation, every push, and every merge — it screens the PR
  title/body, commit messages and touched documentation for personal/private information (LAN IPs,
  MAC addresses, VINs, WiFi network names, hostnames, emails) and for content not written in
  English; it is not a subset of `$project-review` or `$skill-audit`. `$feature-docs` is
  conditionally required when the cataloged feature surface changes, including the PR-policy and
  bench-acceptance workflows. Records are bound to the exact current PR head and become stale after
  any push.
- Reviewers report actionable findings with path/line, cause, impact and evidence. A green build is
  not review proof. Resolve P1/P2 findings and rerun the affected independent review after edits.
- The only accepted merge shape is:

  ```text
  gh --repo github.com/0Bu/tesla-key-esp32 pr merge <numeric-pr> --match-head-commit <full-40-hex-head-sha> --squash
  ```

  No `--admin`, `--auto`, `--merge`, `--rebase`, queue/auto-merge, REST/GraphQL/MCP merge, foreign
  host/repository, missing numeric PR, missing exact head SHA or mutation-prefixed compound command.
- Do not create a commit, push, PR, review stamp, merge, release or Pages update unless the user has
  authorized that phase explicitly. Local migration completion is not Phase 7 publication approval.

Runner-neutral policy is implemented in [`tools/agent-hooks/`](tools/agent-hooks/) and configured
for this project by [`.codex/hooks.json`](.codex/hooks.json). Hooks are lexical defense in depth:
they do not replace sandboxing, explicit authorization, branch protection, protected environments
or human review.
