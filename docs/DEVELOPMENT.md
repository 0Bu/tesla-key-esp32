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

[`scripts/nanopb-source.json`](../scripts/nanopb-source.json) owns the existing nanopb 0.4.9.1
commit and seven runtime digests. Root CMake admits its complete owned Git tree before IDF
component processing, declares that exact commit, and marks it globally populated without
configuring nanopb. The unchanged component's later tag declaration cannot select another source.
Unsupported FetchContent/provider overrides fail. Reconfiguration watches the admitted inputs;
an always-run existing-only prerequisite also guards component and ELF targets when file size or
mtime is preserved. A warm configure with missing admitted state fails instead of fetching again.

The host harness selects only its owned `nanopb-pristine`, `tesla-pristine` and `tesla-patched`
caches. Tesla's source commit comes from all four validated locks. Raw Git objects establish the
complete pristine path/body/mode inventory; VERSION text, a parent repository and a managed archive
are insufficient. Only exact committed links to direct regular tracked files are allowed; all seven
nanopb runtime inputs remain regular files. The separate Tesla work copy admits the complete
pristine tree plus exactly the reviewed patch transformation, including untouched files.
Patch application sets POSIX mode at process startup with zero fuzz so allowed offset hunks do not leave backup
files; the complete inventory still refuses every extra file instead of deleting or ignoring it.

An existing foreign, modified, aliased, partial or failed cache is never adopted, repaired or silently
retried. Failure leaves its owned state for inspection; select a new empty build/cache directory or
remove that dedicated generated state deliberately after inspecting it. Only legitimate optional
cold-network absence may skip the harness; strict mode, unknown commits and every provenance
mismatch fail. Existing caches are checked before a different cold fetch can skip. Git selectors,
configuration, hooks, filters and automatic maintenance cannot supply alternate source material.
The existing harness Mbed TLS selection is unchanged.

Each Git query has bounded data pipes and a separate bounded leader-status channel. An owned
guardian watches a liveness pipe whose writer belongs only to the admission process. If an outer
timeout or SIGKILL kills admission, EOF makes the guardian terminate its own group, including Git
and pipe-holding descendants after the Git leader exits. Normal cleanup kills that entire group
atomically before closing the liveness writer, keeping ownership through parent cancellation.
The guardian holds no data-output writer.
Before side effects, admission records and checks its original process/parent identity. On Linux,
it arms and verifies parent-death SIGKILL before the first query or cache write, then rechecks the
owner. This covers CMake versions whose timeout leaves a helper alive after CMake exits; the
consumer must still fail and exit on admission timeout. Later calls never adopt a new parent or
reuse an inherited post-fork flag. Imports alone do not change kernel policy. The dedicated CLI
rejects startup already adopted by init; an imported validator may legitimately have container
init as its original parent. This cannot identify arbitrary subreaper adoption before import, and
does not fire while the original parent remains alive. Non-Linux systems retain bounded parent
identity checks and the consumer's own cancellation behavior, without a Linux kernel guarantee.
Normal success and internal failure also confirm group cleanup; malformed or missing leader status
refuses immediately. This does not authorize killing unrelated process groups.

Source/patch identity and compiler-selector salts separate owned object namespaces even when
source mtimes match. This does not authenticate compiler binaries or cached object contents.
[`test_dependency_sources.py`](../test/test_dependency_sources.py) exercises local Git/cache
admission; [`test_dependency_cmake.py`](../test/test_dependency_cmake.py) exercises real root CMake
and passive consumer targets with Make and Ninja. These fixtures are distinct from the required
pinned IDF builds and protocol vectors. This correction covers repository consumers; upstream's
standalone tag-fetch CMake path remains unchanged.

## Embedded web assets

`main/www/inline_assets.cmake` inserts the CSS and JavaScript into the HTML marker lines,
then the build uses `gzip -9 -n`. Raw marker checks precede any transformation. The bounded,
standard-library `prune_css_comments.py` helper blanks documentation text in whole-line CSS
comments outside strings and parentheses, and ordinary whole-line JavaScript `//` comments.
CSS byte offsets and whitespace are preserved. The JavaScript pass removes leading spaces and
tabs from code lines and keeps the original line endings, including on blanked comment lines.
Inline comments, legal/tooling directives and HTML-sensitive sequences are preserved. JavaScript
block comments remain opaque. The supported JavaScript subset refuses code
lines containing template backticks, inline block openers or a final backslash, code after a block
closer, unfinished blocks and Unicode line separators. These restrictions exclude untracked
multiline strings/templates without guessing regex syntax. Inputs are bounded UTF-8 without NUL.
Markup and code tokens remain unchanged; source comments remain readable. This is conservative
repository-specific pruning, not general minification or universal observable equivalence:
source introspection such as `Function.prototype.toString()` can expose changed whitespace and comments.
The application does not use source introspection.
The helper is an explicit generator dependency. It rejects output/input aliases before cleanup,
removes the validated old output before generation and publishes complete bytes atomically.
CMake refuses helper startup, generation and timeout failures; retained files are never a success.

The effective-build gate compares the actual generated page and gzip with canonical inputs.
Host tests execute the real CMake generator and its incremental helper dependency. Linux Chromium
independently compares ordered raw CSS rules with the generated fixture's DOM stylesheet before
layout and trusted-keyboard checks; this is modern-browser evidence, not every-browser equivalence.

## Validate a change

Start with checks relevant to the diff. Host suite scope and prerequisites are in the
[test guide](../test/README.md); no host result establishes hardware or vehicle behavior.

Run the complete local handoff once with `scripts/check-local.sh`; add `--firmware` for the
four-target build, disposable-key signing and reproducibility contract. The entry point runs host,
protocol, repository/agent gates and Linux sanitizers, using the pinned Docker image for sanitizers
on macOS. It performs no publication, real-key signing, device or vehicle operation. Standalone
commands below remain available for focused iteration.

The host runner reports elapsed time for each gate. Contract parsers reuse bounded immutable
results for identical complete source strings within one process; changed fixture text is parsed
again, and returned inventories are independent copies. Every mutation and failure-propagation
case still runs. Passive dependency-flow tools use shell stubs for inert compiler/archive/system
effects. Deploy-policy fixtures copy their workflow/checker inputs without local build or cache trees.
Shared dependency, otadata and Release-acceptance self-tests have one owner in
`test-build-contracts.sh`; the full host wrapper does not repeat them. CI also leaves its shared
version, size, Pages, changelog and build-gate self-tests with that mandatory full host gate.
On main, full repository/agent preflight runs in `logic-harness` alongside the longer mock suite;
both remain required by `build`. The independent artifact join uses `prepare`'s release version
without waiting for `build`; protected publication still needs both successful joins and compares
their full exact-source inventories and bytes before key use. Main and trusted preview
acceptance still execute the complete remote contract before signing, without local receipts.

For repeated local host checks, use:

```bash
scripts/with-host-python.sh python3 scripts/run-local-checks.py --require-all
```

The Stop and strict pre-commit hooks share this coordinator. Successful runs can be reused for
identical HEAD, tracked and non-ignored new file contents/modes, resolved tools, environment and
host configuration. Deleted files invalidate the result. Concurrent callers share a worktree-local
lock, inherited by bounded sub-gates. Failed and interrupted runs leave no success receipt.
The 20-minute strict suite budget is enclosed by longer core, adapter and hook-registration
deadlines, leaving time for cleanup before an outer timeout can interrupt it.
`--force` always executes; CI never consumes or writes local receipts. These receipts provide local
feedback, not review, remote CI, signed-artifact or hardware evidence.
Pre-commit removes only the leading builtin helper path added by Git before starting tests.
Custom Git exec paths and unrelated PATH entries remain unchanged and invalidate reuse normally.

Startup files, module search paths and nonliteral compiler overrides disable reuse. On macOS,
changes to the browser runner's local image also invalidate it; unrelated Docker images do not.
Initial browser/CMake setup can change runtime
inputs during a successful run; that result is reported but not cached. Pre-commit uses `--staged`:
when working-tree inputs differ from the index, it executes fresh checks without reuse. Those
checks still exercise the working tree; partial staging needs separate staged-snapshot verification
before claiming commit-byte coverage. Host runners remove signing-key environment variables.
Per-call runner metadata (`CODEX_TOOL_CALL_ID`) is removed from Git and test subprocesses and
omitted from the cache identity. Other unknown environment variables still invalidate reuse.

```bash
scripts/run-mock-tests.sh
scripts/repo-lint.sh
scripts/test-build-contracts.sh
scripts/test-pr-gates.sh
tools/agent-config/selftest.sh
```

Automatic browser tests are Linux-only. On macOS, use `scripts/run-browser-tests-linux.sh`
(Docker and package access for the initial image build), or the Linux CI host job. The container
runs the actual desktop/mobile DOM and accessibility checks without network or writable source
access. Native macOS browser launches are refused; `--require-all` runs this Linux container
on macOS and fails if Docker, the image build or the real browser check fails. Prepare a cold
image with `scripts/run-browser-tests-linux.sh` first; the strict sub-gate keeps its 120-second
outer deadline, including Docker startup. Controlled launch-policy tests are separate
from real-browser evidence. For optional local visual review, use the provided in-app Browser
with an offline fixture; do not launch native Chrome from shell tests.

Before firmware handoff, use the complete four-target contract:

```bash
scripts/idf-docker.sh ./scripts/ci-build-verify.sh "$(cat version.txt)" "$(git rev-parse HEAD)"
```

This adds disposable-key signing, manifest/Pages byte checks and reproducibility to compilation.
Never use the real offline OTA key in tests. Before a firmware push, `scripts/precheck-firmware.sh`
checks all four size and stack baselines incrementally; `--target` narrows investigation and
`--clean` removes its cache. Narrow checks do not replace the four-target handoff.

The IDF wrapper applies the same CPU/memory ceiling to ordinary builds, daemon creation and
Fast-Build volume setup. Volume setup uses the pinned IDF image and stops on failure.
Daemon reuse additionally requires the init reaper. The offline self-test executes the shipped
wrapper with a passive Docker CLI, including setup failures; it
neither accesses a Docker daemon nor establishes actual container resource enforcement.

CI computes cumulative firmware relevance once in `prepare`, conservatively building when the
published baseline cannot be verified. Target jobs consume that scheduling decision; signing and
publication revalidate current source and authority at their own boundaries. The primary matrix verifies all four supported targets. Independent main builds use the same target defaults.

PR feedback is selected by `scripts/check-change-scope.py`: ordinary Markdown-only changes use repository
lint without firmware compilation; browser changes use strict browser checks; firmware source uses
runtime, protocol and sanitizer checks. The PR template, feature catalog and third-party notices
are executable checker inputs and select full coverage. Protocol vectors and shared native/browser
parity run in both profiles. The strict runtime gate includes the retained MQTT JSON/registry owner;
Linux sanitizers execute that same owner with instrumentation. Unknown,
combined runtime/browser code, test, policy, build and dependency changes use full coverage. `repo-lint.sh --quick` keeps
actual validators while omitting their unrelated mutation self-tests. Local handoff and main/manual
publication remain full. Main still reconciles shipped documentation through the existing signed
site pipeline; PR selection does not change that publication/source contract.

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
`check-firmware-size.sh` builds all four targets by default. The pre-push hook runs the
incremental `precheck-firmware.sh` on all four (`PRE_PUSH_TARGETS` narrows it deliberately); it
checks only the size and stack gates, not CI's dependency, partition, otadata, semantics,
artifact-identity, reproducibility or signing gates. Stack reports inventory frames, reject unbounded dynamic frames
and frames over 4096 B; they do not prove call depth or runtime stack margin.

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

Unsigned local images **cannot boot**. Use provenance-bound signed artifacts and the canonical
`$flash-esp32`, `$deploy-bench`, `$ship` or `$usb-recovery` procedure, as appropriate. Every physical write
requires explicit authorization for the board, target and scope. The user's `$deploy` invocation
for firmware deployment and a direct `$deploy-bench` invocation both supply the standard USB
preflight, flash and passive acceptance scope for the intended bench. Resolve its physical identity
and verify the exact artifact before acting. Honor any narrower scope the user specified.
Verify every image's signature;
production and preview artifacts must match the production public-key pin. An authorized local
development image must match the supplied development key. Size/chip/version checks alone do not
authenticate an image.

App-only USB delivery writes the signed app at `0x20000`, then activates by erasing `otadata`
last. NVS stays intact. Merged-image writes and full erases destroy configuration and keys;
[retention and signing](SECURITY.md) explain recovery limits. Signing with a development key creates
a separate trust lineage and needs an explicit key and scope.

`$deploy` handles commit, push, PR creation, current-head reviews/CI and canonical squash merge,
then follows the protected Dev build. For firmware deployment it passes the resulting signed image
and the existing approval to [`$deploy-bench`](../.agents/skills/deploy-bench/SKILL.md). A required
pre-merge bench check uses the exact candidate while the PR is open. `$deploy-bench` binds the
physical board across Tesla/Daikin firmware, verifies the artifact through `$flash-esp32` or the
app-only steps of `$usb-recovery`, and accepts the implementation on that same board. Establish
that the board lacks a configured vehicle VIN or is isolated from the vehicle before a USB reset.
Ask only for missing bench details. Production delivery through `$ship`, OTA and live evcc or vehicle
tests require their own authorization.

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

The assembler emits an empty `.nojekyll`, and every shared branch-publisher mode maintains it at
the branch root. Pages therefore serves the already-built static files without a Jekyll rebuild.
The explicit build request is retained because a `GITHUB_TOKEN` branch push does not trigger Pages.
This removes redundant site processing; GitHub's deployment queue and CDN propagation remain
external waits and still require the same acceptance checks.

Stable and Dev acceptance first observe the latest Pages build with up to 32 status reads at fixed
10-second intervals (the same 310-second waiting span). This avoids up to 60 seconds of readiness
detection lag from exponential polling. Queued/building, failed
builds and unavailable status have distinct diagnostics before byte acceptance starts. This latest
build observation is a readiness hint, not provenance for a particular firmware snapshot.
Post-publication byte acceptance then retries the complete manifest, all 16 Pages parts and changelog with
exponential delays capped at 60 seconds (190 seconds of default waiting). Each attempt uses a fresh
cache key and must independently pass all
identity, size, hash and byte-binding checks. Results from incomplete attempts are never combined.
Invalid staged Dev bytes, expected changelog or immutable Release metadata/assets fail before Pages polling. This
handles delayed CDN propagation; it does not establish an atomic CDN snapshot.
Pages acceptance requires a bounded, validated expected `changelog.json` and exact served bytes
within the same complete attempt as the manifest and 16 firmware parts. The root checker takes
`--expected-changelog-file`; staged Dev checks derive it from the candidate directory, while live
Dev checks require that argument. Relevance baselines read both files from one exact branch commit.
Changes to shipped top-level `docs/*.md` select the normal four-target build and Dev publication;
they do not reuse older firmware under the newer source SHA. Stable publication remains manual.
Before either Pages branch write, the downloaded candidate passes four key-free RSA-PSS checks
against the production public pin. The offline deploy-consumer test executes this YAML step with
disposable signed fixtures and confirms that each target failure stops before publication.

Dev publication prefetches the unchanged digest-pinned IDF image on the signing runner while
key-free artifact and provenance checks run. The pull must finish successfully and the exact image
must exist before key provisioning; source/version authority is refreshed after the wait.
`scripts/prefetch-idf-image.sh` exclusively creates private per-job state and rejects failed pulls,
changed pins, stale state and timeouts. The signer still uses `scripts/idf-docker.sh` with its
existing resource limits and all independent-byte, signature, size and layout checks. Stable
creation and immutable reuse retain their existing paths. Prefetching is not signing authority.

### Recover a published stable site

A Release that became immutable before a failed deploy can be recovered after main advances.
Use a clean checkout of current main, Python 3.11+, `gh`, `rsync`, authenticated GitHub reads and
an explicitly authorized stable Pages recovery. Publication additionally requires `GITHUB_TOKEN`
with the repository/Pages write permissions. Do not rerun historical workflow code or use `--exact`
to recreate the existing tag.

First verify the original stable build's numeric run ID without writing Pages:

```bash
python3 scripts/recover-release-pages.py --run-id <original-run-id>
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

The preview version is selected once from trusted default-branch code. Four separate, cache-free,
read-only target jobs rebuild the exact PR head concurrently, without producer artifact access.
A secret-free join requires every target and assembles the complete source/version-bound inventory.
The protected signer compares producer and independent payloads, revalidates the live PR and stable
version, and never checks out PR code.
Eligible previews additionally require both complete host/protocol acceptance suites, parallel with
the target legs. Selected PR feedback never suffices for signing. A source run without the unsigned
firmware artifact is ineligible, so a documentation-only PR does not start preview rebuilds.

After signed preview publication, `scripts/check-pr-preview-served.py` runs trusted default-branch
code without keys or repository tokens. It validates the local signed four-target site against the
source, canonical PR version and production public-key pin, then compares the served manifest,
all 16 parts and a final manifest from the API-confirmed `PR/<N>/` authority. Each retry starts a
complete fresh snapshot. Redirects, non-200 status, mixed or incomplete bytes and expired deadlines
fail acceptance. At most 20 attempts share a 180-second network deadline and 10-second request caps.

Event and scheduled cleanup select the repository default branch at checkout, keeping the decision
producer and served checker together. A historical PR base can predate both their shared contract
and the checker; it is not a cleanup-code source.

Cleanup emits one JSON decision: `kept-eligible`, `already-absent` or `removed`. Eligible previews
are retained without an absence claim. The other decisions require 404/410 for the manifest,
installer entrypoint, all 16 canonical parts and every validated path collected before deletion.
Both the ordinary URL and a fresh cache key must be absent. DNS/TLS errors and 5xx do not prove
absence. This proves only the reported URL observations, not
eviction of arbitrary historical CDN cache keys. Publication/removal and served acceptance remain
separate results; a failed acceptance does not repeat signing or grant a recovery operation.

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
exceptions require a supported `pr`/`run` read command with active `--json` and `--jq`/`-q` options.
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

Required independent checks and exact-head records are in [AGENTS.md](../AGENTS.md#pr-and-review).
Skills specify how to perform them; a green build does not substitute for review.
Bounded changes use focused reviews of changed contracts and affected consumers against an accepted
baseline. Explicit whole-project reviews, cross-cutting migrations, target/dependency or trust-boundary
changes, and unknown baselines use full audits. Reports state scope and exclusions; the existing
head-bound records and independent privacy/English check remain required.
Before PR creation or a new push, publication reviews can bind a clean local candidate with
verified branch, base and ancestry. Recheck the exact GitHub head after push; these records do
not establish remote CI or merge acceptance.
Update the owning document with each changed contract. Keep [FEATURES](FEATURES.md) as an index,
not a second narrative. `$project-review` always assesses documentation structure, clarity,
concision and completeness; unresolved material documentation findings prevent a clean result.

### Static-analysis decision

The recorded clang-tidy 18 survey found 3355 blanket findings but only three curated bug-finding
findings, all in test helpers; conversion warnings were bounded gradient conversions, and shadow/
sign-conversion checks found none. This is historical measurement, not a current clean claim.
There is no inert `.clang-tidy` configuration. Before introducing an analyzer, remeasure useful
yield on the actual revision; do not replace host/runtime/IDF evidence with that survey.
