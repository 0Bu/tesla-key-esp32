# Development

Build against the checked-out source, not documentation from another revision. Before integrating,
inspect status, `origin/main`, the merge-base and intervening changes; preserve local work.
[Agent policy](../AGENTS.md) defines authorization and review requirements.

## Build

Use `scripts/idf-docker.sh` with the ESP-IDF tag and immutable digest in
[`esp-idf-toolchain.txt`](../esp-idf-toolchain.txt). The wrapper limits each container to 1.5 CPU
and 1800 MiB and uses Docker's init reaper so killed subprocess descendants do not remain zombies.
A remote runner without Docker can run host checks but cannot establish IDF evidence.

```bash
scripts/idf-docker.sh idf.py -B build set-target esp32s3 build
scripts/idf-docker.sh ./scripts/ci-build-all.sh local
scripts/idf-docker.sh idf.py menuconfig
```

The targets are exactly esp32, esp32s3, esp32c3 and esp32c6. The component manifest and four target
lockfiles pin the graph. CMake applies the ordered hash-checked tesla-ble patch series; do not edit
managed checkouts or silently substitute a host IDF. Protocol rationale: [ADR-0005](adr/0005-tesla-ble-seam.md).
The patch-state manifest binds every touched file's complete before/after digest. Unknown or mixed
source states fail even with an existing marker; rematerialise the component deliberately. Environment
patch overrides are refused. Native harnesses may pass an explicit component path to the same verifier.
Incremental builds monitor the verifier inputs, postimages, marker and patch/lock inventories and
rerun admission when they change. This uses normal CMake timestamp and inventory tracking;
independent fresh rebuilds establish delivery provenance.

Host gates use `scripts/with-host-python.sh` to resolve Python ≥3.11 for tomllib. It re-execs once,
not a hidden source change. [AGENTS.md](../AGENTS.md#documentation) owns native adapter
generation, linked-worktree hook setup and trust; adapter parity does not prove runtime enforcement.

Optional build acceleration: `IDF_FAST_BUILD=1` uses a container-native volume;
`scripts/idf-docker.sh daemon start` keeps a warm container, and `daemon stop` releases it.
The same limits and toolchain apply. USB flashing runs on the host, not Docker Desktop.
Git metadata is mounted read-only in both layouts: a primary checkout has a `.git` overlay;
a linked worktree exposes only its external Git common directory, without the primary checkout's
source files. Daemon reuse checks the actual mount inventory as well
as its image, resource limits and mount-policy label. An older, broader daemon is recreated.

## Exact dependency source admission

Root CMake admits the exact Nanopb 0.4.9.1 commit and seven runtime digests from
[`scripts/nanopb-source.json`](../scripts/nanopb-source.json) before IDF component processing.
It declares the exact commit and marks that tree populated. Alternate provider overrides fail.
An existing-only build prerequisite rechecks the inputs even when mtimes or sizes are preserved.
A warm configure requires the admitted state.

The host harness uses its owned `nanopb-pristine`, `tesla-pristine` and `tesla-patched` trees.
All four Tesla locks must agree. Raw Git objects bind paths, modes and bodies, including untouched
files. Committed links may target only direct regular tracked files. The seven Nanopb inputs
must be regular. The separate Tesla tree accepts only the reviewed patch transformation:
POSIX patch mode, zero fuzz, and no extra files. Object namespaces include source/patch identity
and compiler selectors. They do not authenticate compiler or object bytes.

Foreign, modified or partial state fails closed. Preserve it for inspection and select an empty
dedicated cache for a fresh attempt. Only optional cold-network absence can skip the harness.
Strict mode, unknown commits and provenance mismatches fail. Existing caches are checked first.
Git configuration, hooks, filters and maintenance cannot supply alternate inputs.

[`dependency_source.py`](../scripts/dependency_source.py) bounds Git data and status channels.
Its liveness guardian owns group cleanup, including descendants retaining pipes after leader exit.
Linux also verifies parent-death protection before side effects. Other hosts retain bounded parent
checks and consumer cancellation. A missing leader status, changed owner or unconfirmed cleanup
fails. The guardian only terminates its owned group. See [dependency tests](../test/README.md#gate-index).

## Embedded web assets

`main/www/inline_assets.cmake` validates raw markers, inlines bounded UTF-8 CSS/JS, then runs
`gzip -9 -n`. `prune_css_comments.py` blanks eligible whole-line CSS/JS comments and trims leading
CSS/JS indentation while keeping line endings intact. Inline, legal/tooling and HTML-sensitive
content is preserved. Unsupported multiline JS forms and NUL are rejected. Source introspection
can observe the changed whitespace and comments.

The helper is an explicit dependency. It rejects input/output aliases and atomically publishes
complete output after removing the old output. Startup, generation and timeout failures fail the
build. Effective-build checks compare emitted HTML and gzip. Tests cover incremental generation and
[Linux browser CSS/layout](../test/README.md#browser-prerequisites).

## Validate a change

```bash
scripts/check-local.sh                # host, repository policy, BLE protocol and Linux sanitizers
scripts/check-local.sh --firmware     # also build, sign with a disposable key and compare all four targets
scripts/check-local.sh --candidate    # select checks for the clean committed push candidate
scripts/check-local.sh --verify-receipt # confirm the push receipt without retesting
```

Start with focused [host/runtime/browser checks](../test/README.md#run-it). The handoff runs strict
host checks beside repository policy and the BLE harness, then serializes sanitizers and firmware.
On macOS the sanitizer path uses pinned Docker. Run the applicable complete handoff before pushing
and record the tested source and base SHAs. The candidate command requires current `origin/main`
to be an ancestor. Refresh it before running checks. It selects link checks for prose-only
changes and four-target verification for publication inputs. The push receipt binds commit, base,
profile, and the pinned toolchain file. See [PR review policy](../AGENTS.md#pr-and-review) and
[protected publication](SECURITY.md#release-pipeline-and-trust-boundaries).

On `main`, GitHub requires the `current-head-records` check and an up-to-date PR branch.
An ordinary PR skips the `build` job. That job and any CodeQL results are optional for merge.
On release-relevant merges, the protected `main` workflow still builds and signs the Dev
firmware before publishing it to Pages.

Dev runs the strict host suite in two parallel jobs. One runs runtime and browser checks.
The other runs policy and build-contract checks. Both must pass before publication. Local
checks and manual builds keep the full suite together.

On pushes to the canonical `main`, the unprivileged preparation, host checks, and ESP-IDF
build commands compare their source SHA with the public Git ref. Long commands recheck after
20 seconds of work and stop when GitHub reports a different head. If the ref cannot be read,
the work continues and the existing publication checks still verify the current head.
The workflow remains serialized through signing and Pages publication. Manual releases and
PR builds use their usual checks.

To measure overlapping Dev runs, compare the newer merge time with the older run's completion.
The `Superseded Dev work` notice in a job log identifies the head change that stopped its work.
Check the newer run's `publish` and `deploy` results, then verify the served Dev manifest and
artifact bytes against that run's source SHA.

PRs changing only `.github/workflows/renovate.yaml` and/or `.github/renovate.json` need no manual
check or review records at merge/check. This lets Renovate maintain its own action pin without a
firmware handoff. The complete changed-file list determines the exception, including both sides
of renames; any other path restores the normal gates. Human PR creation/push gates and explicit
merge authorization still apply.

For repeated local host runs:

```bash
scripts/with-host-python.sh python3 scripts/run-local-checks.py --require-all
```

The worktree-local coordinator reuses only successful identical source and runtime inputs:
HEAD, tracked and nonignored files (including mode/deletion), tools, environment and host setup.
On macOS the Linux browser image is part of runtime identity. Indirect tool selectors disable
reuse. Setup that changes runtime identity leaves no receipt. Concurrent callers share a lock.
The strict suite has a 20-minute budget. Surviving owned workers retain the inherited lock while
cleanup is pending. Failures/interruption clear reuse. `--force` reruns. CI ignores local receipts. Under `--staged`,
different working-tree inputs trigger a fresh working-tree test, so partial staging still needs
separate staged-snapshot evidence.

For firmware size/stack iteration, use `scripts/precheck-firmware.sh` (all four targets by default,
`--target` narrows diagnosis, `--clean` clears its cache). The complete local build additionally
runs signed-size, manifest/Pages byte and reproducibility gates. Use disposable test keys only.
Keep host, build, signing, CI and physical evidence separate.

## Build and evidence contracts

| Contract | Authority |
|---|---|
| Toolchain, locked components and patches | `esp-idf-toolchain.txt`, `main/idf_component.yml`, `dependencies.lock.*`, `patches/tesla-ble/`, `scripts/tesla-ble-patch-state.json` |
| Exact four targets and unsigned outputs | `scripts/ci-build-all.sh`, `check-build-artifact-inventory.py` |
| Physical flash layout | `partitions.csv`, `check-partition-contract.py`, `check-otadata-contract.py` |
| Effective compiler/generated/link inputs | `check-build-semantics.py` |
| Per-target memory and frame budgets | `firmware-size-baseline.json`, `firmware-stack-baseline.json`, `check-firmware-size.sh`, `check-stack-usage.py` under `scripts/` |
| Source fingerprint and independent byte reproducibility | `check-reproducible-build.sh`, `check-reproducible-artifacts.py` |
| Protected signing and immutable publication | [Security pipeline](SECURITY.md#release-pipeline-and-trust-boundaries) |

The effective-build validator closes every C/C++ compilation unit to reviewed source roots and
pinned compilers/includes. It verifies generated producers, Ninja commands/dependencies, archive
objects, linker response files and the exact ELF-to-image chain. One literal main-component `SRCS`
registration is permitted; hidden attachment, escaped sources or forced includes fail.
The gate checks effective optimization tokens for every application C/C++ unit. Ordinary project
and component units require exactly `-Og`; main also requires one `-fstack-usage`. The six OTA
units `http_ota.cpp`, `ota_update.cpp`, `ota_transport.cpp`, `ota_http.cpp`,
`ota_coordinator.cpp` and `ota_writer.cpp` append exactly one `-Os` after `-Og`.
Their literal CMake property block and exact main-component source/object owners are closed by
`check-build-semantics.py`; missing, additional or overriding options fail. This keeps the
signed C6 app within the unchanged OTA slot and retains stack reports and independent four-target
byte rebuilds. Closed source/owner rules
preserve the pinned SDK's private Mbed TLS/TF-PSA-Crypto `-Og -Os`, its two PQCP units at `-Og`,
and the six C3/C6 libc primitives at `-Og -O2` only when the SDK's optimized-misaligned-access
selector is enabled. The generated project anchor retains its exact SDK recipe without an
optimization flag. The validator owns the admitted paths and object owners; missing, duplicate,
reordered or overriding optimization tokens fail. The SDK exceptions preserve their pinned
compiler behavior; the OTA profile is the explicit project exception. The separate
size-optimized bootloader is compared between independent builds. Main
privately enforces C++17 and return-value/format warnings.
The Tesla BLE component privately selects `TESLA_BLE_LOG_LEVEL=2`; effective commands must
contain that single definition. Its host harness uses the same level and keys object reuse to
the patch-state digest. DEBUG/VERBOSE arguments and formatting are excluded; INFO remains.
Compiler/include/search flags, `SDKCONFIG_DEFAULTS`, `EXTRA_COMPONENT_DIRS`, and all
`IDF_COMPONENT_*`, `TESLA_BLE_*` and `CCACHE_*` environment overrides are rejected even when empty.
Both primary and reproduction entry points apply these guards; ccache is disabled.

The size policy limits the projected signed app to slot minus 32 KiB (`0x1e8000`): the app is
padded to 64 KiB and signing adds one 4 KiB sector, bounded by the `0x1f0000` slot. Because size
moves in 64 KiB steps, each size report also shows the content headroom to the next step. Size
reports are current evidence; historical prose values are not. `--update-baseline` raises maxima only; `--tighten-baseline`
requires the old baseline to pass, then lowers each checked target to measured values. Memory-model
and static/IRAM capacities are identity fields, immutable under either ratchet. The stack baseline
uses the same modes (`check-stack-usage.py --observed-dir dist --write-baseline <file> --mode
raise|tighten`) and lists every changed entry; a PR lists each raised entry with its cause.
Stack tighten forbids growth and newly reviewed frames or translation units; it permits removed
units and removed or below-threshold frames, then lowers the remaining maxima to measurements.
`check-firmware-size.sh` builds all four targets by default. The
[Git pre-push hook](../.githooks/pre-push) admits only the current feature branch and verifies the
local receipt. The pinned local candidate build owns the full size, stack and protocol checks.
The full build additionally checks dependencies, partitions, otadata, compiler semantics,
artifact identity, reproducibility and signing. Stack reports reject unbounded dynamic frames and
frames over 4096 B. Runtime margin also depends on call depth.

From a feature branch, run `scripts/check-local.sh --verify-receipt` to verify the local evidence
without pushing. Run `scripts/install-git-hooks.sh` to configure Git to use the
repository hooks from the active worktree on push.

### ESP-IDF migration inputs

An approved toolchain migration must update the image tag and digest in `esp-idf-toolchain.txt`,
the dependency verifier's IDF/Mbed TLS pins, all four lockfiles, the harness `MBEDTLS_REF`,
the workflow's `logic-harness` version assertion, both resource baselines' toolchain identities,
the SDK-default and build-semantic version literals, the compiled X509 bundle hash, and the
version, links and rows of the [ESP-IDF matrix](ESP_IDF_MATRIX.md).
Review the PSA patch and canonical postimages, then rerun the protocol vectors and complete
four-target contract. CI action inputs must still use the toolchain output, and both target
matrices must still contain exactly the four supported targets. This list is a migration checklist,
not authorization to change any pin.

## Configuration-only maintenance role

The default-off [configuration recovery](CONFIG_RECOVERY.md#default-off-build-and-artifact-boundary)
owner describes its separate project identity, unsigned four-target source/reproduction driver,
resource gates and unqualified physical procedure. Its non-OTA apps never enter the normal 16 parts;
ordinary source/compiler/partition/signature authority is not relaxed. Do not enable its config in
an application build or treat its presence as a flashable or physically tested recovery product.

## Delivery

**Unsigned local images cannot boot.** Verify the approved signature authority and exact artifact
before writing. Production and previews use the production RSA pin. An authorized development key
establishes a different trust lineage. [Signing and rotation](SECURITY.md#signed-ota-images)

App-only USB writes the signed app at `0x20000` and erases otadata last. Preserve NVS and verify the
[installed layout](#installed-layout-proof) and [retained bootloader](#retained-bootloader-references).
Merged writes and full erases destroy NVS. [Retention](SECURITY.md#nvs-retention)

Use the [authorized workflows](../AGENTS.md#authorization-and-ownership). For `$deploy-bench`,
identify the intended physical board and confirm no configured VIN or isolation from the vehicle
before a USB reset. Keep acceptance on that board.
[Bench workflow](../.agents/skills/deploy-bench/SKILL.md)

### Installed-layout proof

Every app-only USB path, including standalone `$usb-recovery`, requires evidence of the selected
board's installed partition table before writing. A matching chip or signed app does not establish
its layout. Reuse collected evidence only while its physical-board binding and installed layout
remain established. Otherwise obtain the missing bounded read scope and collect only the table
metadata at the established table location (`0x8000`, at most `0x1000` bytes for this project),
excluding NVS. `$deploy-bench` already authorizes its scoped preflight reads; do not request that
approval again. Keep per-board evidence outside Git.

Decode the table with the pinned SDK tooling and verify:

- Valid entries and termination, and the format's checksum. A checksum-less historical table needs
  independently established byte identity with a known compatible table; unknown integrity stops.
- All regions fit the established physical flash capacity without overlap. The protected NVS range
  remains `0x9000/0x6000`, and OTA data remains `0xf000/0x2000`.
- The intended `ota_0` app starts at `0x20000`, has the correct app subtype and can hold the complete
  verified signed image. Erasing OTA data must select that app, without a factory app taking priority.

Compatible historical tables may omit newer diagnostic partitions or have different remaining
regions; they need not equal today's table byte-for-byte. Record the actual table checks, selected
image length/hash and same-board binding rather than a manually asserted pass flag. If evidence is
missing, unreadable or incompatible, stop the app-only procedure. A broader conversion or
bootloader/table recovery needs its separately authorized plan; it is not an automatic fallback.
The retained-bootloader and signature checks remain separate prerequisites.

### Local OTA test before merge

The global maintainer skill `$ota` tests the intended local source on an explicitly selected
production board before merge or feed publication. It routes Tesla and Daikin separately;
Tesla uses [`scripts/dev-ota-upload.py`](../scripts/dev-ota-upload.py). Read its `--help` and
the current project instructions. Creating or editing the skill grants no live/signing/upload
authority. Remote CI, a commit and PR-body gate stamps are not local-test prerequisites; the
ordinary PR gates remain required at their existing publication/merge boundaries.

`prepare --evidence <absolute-private-directory> --inventory <absolute-private-json>
--board <approved-id> --version <intended-version>` snapshots the complete intended tree,
including non-ignored new files. The inventory is mode `0600`, outside Git, with one selected
production entry's project, supported target, literal IP and hardware MAC. No discovery,
default board or automatic channel change exists. Keep real inventory/signing details global,
not in this repository. The chip comes from that explicit hardware binding, never an S3 default.

Run `tests` and `build` with the same `--evidence` directory. They execute the existing local
host/contract/format checks and the pinned Docker build in a fresh case-specific directory.
The selected version must fit the running version-core floor. Through a separately authorized
standalone signer, sign those exact bytes with the existing approved authority; the helper
never reads a private key. `bind-image --image <absolute-signed-app.bin> --sha256 <signed-file-sha256>`
checks the signature pin, app project/chip/version/size and correspondence to that unsigned build.

`review` launches an independent Codex process with enforced read-only sandbox and no approvals,
using the existing reviewer instructions for OTA/heap/security, persistence/NVS/pairing,
BLE/evcc meaning, documentation and UI. Its actual thread identity, structured findings and
report/log hashes join the source/image-bound local journal. A manually supplied pass cannot
replace this process. Missing checks, a failed run, blocking findings or source/artifact edits
stop readiness; regenerate the affected evidence rather than reusing old proof.

`check` is offline. `preflight` performs live passive reads and requires the selected target/read
scope. Only an explicitly authorized board test permits `execute --execute --case-id <case-id>`.
The board must already be on saved `dev`, with stable verification and adequate memory.
Before its sole POST the helper rechecks all evidence and consumes its exclusive attempt marker.
No timeout/refusal/lost response grants a retry, channel change or different delivery path.

Acceptance needs a distinct boot nonce, full expected ELF/version/project/chip, unchanged
hardware identity and observable retention, plus healthy stable runtime. It records changed
behavior as **not run** until the separately authorized concrete functional test is performed.
No vehicle command or live evcc/cluster call is inferred. Report these boundaries separately;
HTTP success, local host fixtures and a compiled image cannot prove the real hardware/function.
See [API](API.md#direct-signed-firmware-upload) and [security](SECURITY.md#ota-self-update).

### Served Pages acceptance

Check the latest Pages build up to 32 times at fixed 10 s intervals (310 s of waiting). Treat
queued/building, failed and inaccessible status separately. Readiness does not establish byte
provenance. The explicit build request is required for a branch push using GitHub's workflow token.

Each byte attempt uses a fresh cache key and verifies one complete manifest, its 16 parts and the
expected changelog. Attempts retry with exponential delays capped at 60 s (190 s default waiting).
Never combine partial attempts. A bad staged Dev tree, changelog or immutable Release fails before
polling. Cache busting does not establish an atomic CDN snapshot.

Root and live Dev checks take `--expected-changelog-file`. Staged Dev derives it from the candidate.
Before a branch write, the key-free consumer verifies all four production signatures.
[Publication authority and static marker](SECURITY.md#release-pipeline-and-trust-boundaries)

### Recover a published stable site

A Release that became immutable before a failed deploy can be recovered after main advances.
Use a clean checkout of current main, Python 3.11+, `gh`, `rsync`, authenticated GitHub reads and
an explicitly authorized stable Pages recovery. Publication additionally requires `GITHUB_TOKEN`
with the repository/Pages write permissions. Do not rerun historical workflow code or use `--exact`
to recreate the existing tag.

First verify the original stable build's numeric run ID without writing Pages:

```bash
python3 scripts/recover-release-pages.py --run-id '<original-run-id>'
```

After the recovery is authorized, add `--publish` to the same invocation. The helper downloads
the exact unexpired signed artifact and checks its API-authenticated ZIP digest before extraction.
It preserves the original HTML/JavaScript, firmware and changelog; it does not rebuild current
documentation under an old source SHA. The candidate must equal the entire original site plus
exactly one empty root `.nojekyll` control file, including when the original predates that marker.
An unsafe original marker or any changed, missing or extra payload is rejected.
It requires the latest immutable Release, exact tag/source,
source ancestry and successful original host/rebuild/signature gates. A later publish-job failure
is recoverable only when all critical pre-publication steps succeeded; cancelled runs are refused.
Explicitly attested key-free immutable reuse is also accepted. Every branch retry repeats these
checks, then readiness and full public-byte acceptance follow. Expired artifacts, foreign authority,
changed latest/main/tag or any byte mismatch stop the operation. Stable creation and Dev never use
this exception.

Unprivileged `prepare` accepts an empty latest-stable base only for no eligible immutable latest
Release; existing version fallbacks then apply. API reads retry at most three times; malformed or
denied responses remain errors. Signed preview version selection still requires an eligible base.

### Served PR preview acceptance

`scripts/check-pr-preview-served.py` runs key-free trusted default-branch code. It validates the
signed four-target site, source, PR version and production pin, then compares the manifest,
16 parts and a final manifest against the API-confirmed `PR/<N>/` URL. Each retry checks a new
complete snapshot. Redirects, mixed/incomplete bytes and expired deadlines fail. Limits are
20 attempts, 180 s total network time and 10 s per request.

Cleanup reports `kept-eligible`, `already-absent` or `removed`. The last two require 404/410
for the manifest, installer entrypoint, 16 parts and validated paths collected before deletion.
Check both normal and cache-busted URLs. DNS/TLS errors and 5xx do not prove absence.
Only the queried URLs are covered. Failed acceptance does not repeat signing.
[Preview authority and cleanup](SECURITY.md#release-pipeline-and-trust-boundaries)

### Retained-bootloader references

`scripts/check-bootloader-compat.py` compares stable ESP-IDF descriptors before an app-only USB
write. For some bootloaders esptool reports no ESP-IDF version (esptool 5.6.0 on the esp32c6 v6.1
bootloader, although the image carries a descriptor). These require a byte match to
[`scripts/bootloader-references.json`](../scripts/bootloader-references.json), never a guessed
version or the assumption that the current app's bootloader is installed. The selected target's
reference binds image length/SHA-256, source commit, CI run/artifact identity and ZIP digest,
toolchain tag/digest and the independently accepted local rebuild's source/receipt digests.
The catalog is reviewed policy, not a self-authenticating producer manifest or an app-signing key.

Before adding or replacing a reference, independently verify the CI job's exact source and pinned
toolchain, the downloaded ZIP against GitHub's artifact digest, its exact bootloader path and bytes,
and a separately rebuilt byte-equal bootloader. Retain the evidence and obtain independent review
of the entry. Schema validation alone cannot establish those facts. The initial four references
come from the successful primary target jobs in [run 39685199994](https://github.com/0Bu/tesla-key-esp32/actions/runs/39685199994),
source `e1c3197596575728f336bec9eee0eafc4a0793f6`, and match the separately accepted local builds.
That workflow's host sanitizer failure remains a publication blocker; these references certify
only the recorded bootloader bytes and toolchain, with no app-delivery or whole-workflow approval.

For an image whose ESP-IDF version esptool does not report, accept only the exact recorded image or the full confirmed project
bootloader region with an all-`0xff` suffix: `0x1000/0x7000` on ESP32, `0x0/0x8000` on S3/C3/C6.
Unknown bytes, partial padding, missing provenance or a wrong target fail closed. Collection
belongs to the explicitly identified board and approved read plan; the checker contacts no device.
This admission does not prove bootability, signature authority, physical identity or runtime stack.

## Running workflow shell blocks

The project hooks judge each shell tool call by its text before the shell runs it. Sent inline,
most multi-line blocks in `.agents/skills/` are rejected: the PR gate cannot rule out that a
command substitution or an arithmetic assignment such as `X=$((…))` yields a publish or merge
action, and both hooks treat a glob such as the `case` arm `*)` as unbounded. A single-line
command shown with `<placeholders>` runs inline once each holds its literal value. Run multi-line
blocks this way:

1. Write the blocks approved for this call, unchanged and in document order, to one script in the
   runner's scratch directory (or `$TMPDIR`) with the file-writing tool, not a shell redirect.
2. Run it with one static command, `bash <script-path>`. Put inputs the blocks require (`PR`,
   `TARGET`, `PORT`, `RUNID`, `DEVICE_IP`, …) in front as literal `NAME=value` assignments
   so the hooks still see them.
3. A later call that needs an earlier step's variables repeats that step's read-only blocks. Pass a
   value that only a completed write, erase or other mutation set (for example `DELIVERY` in
   `$ship`) as a literal assignment; never repeat the mutation.

Never put `git push`, `gh pr create`, `gh pr merge`, or a key, NVS or credential path into such a
script: those stay standalone, statically bound commands that the hooks inspect, or the operator's
step. The hooks see only `bash <script-path>`, not the script's contents, so they cannot enforce
this rule; the agent writing the script does. A script grants no authorization; every approval
boundary of the workflow still applies.

### Standalone read checks

The publication classifier recognizes bounded literal reads so quoted search/query data can name
publication commands without being treated as commands themselves. Use `rg --no-config` with
literal arguments and the supported ordinary search options; preprocessors, hostname commands,
unquoted globs, expansions, wrappers and pipelines retain conservative classification. GitHub CLI
exceptions cover a supported `pr`/`run` read command with active `--json` and `--jq`/`-q` options,
or a standalone `gh api --method GET repos/OWNER/REPO/RESOURCE` with an optional literal formatter.
For example, run `rg --no-config -n 'git push|gh pr merge' AGENTS.md` as a separate shell call.
The credential guard uses the same literal-read classification only for its stdin-shell check;
all secret-path, credential-output and partition checks still apply.

The partition guard permits only the reviewed validator's isolated, input-only invocation from the
repository root: `python3 -I -S scripts/check-partition-contract.py --csv partitions.csv`.
An optional final `--binary <file>` may name an existing regular file within that worktree; a
symlink or an external path is rejected. The validator's exact SHA-256 is pinned in the hook.
Changing it needs review and a deliberate pin update. Python isolation prevents repository or
environment imports from replacing its standard-library dependencies. Bare Python, alternate
scripts, extra flags, redirections and chained commands receive no exception. These classifications
grant no device-read, publication or vehicle authority.

## Reviews and documentation

Follow the independent review, evidence and exact-head [PR policy](../AGENTS.md#pr-and-review).
Update the document that owns the changed contract. Use [FEATURES](FEATURES.md) as an index.
`$project-review` checks structure, clarity, concision and completeness. Material documentation
findings in scope prevent a clean result.

### Static-analysis decision

The recorded clang-tidy 18 survey found 3355 blanket findings but only three curated bug-finding
findings, all in test helpers; conversion warnings were bounded gradient conversions, and shadow/
sign-conversion checks found none. This is historical measurement, not a current clean claim.
There is no inert `.clang-tidy` configuration. Before introducing an analyzer, remeasure useful
yield on the actual revision; do not replace host/runtime/IDF evidence with that survey.
