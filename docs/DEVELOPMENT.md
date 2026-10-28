# Development

Build against the checked-out source, not documentation from another revision. Before integrating,
inspect status, `origin/main`, the merge-base and intervening changes; preserve local work.
[Agent policy](../AGENTS.md) defines authorization and review requirements.

## Build

Use `scripts/idf-docker.sh` with the ESP-IDF tag and immutable digest in
[`esp-idf-toolchain.txt`](../esp-idf-toolchain.txt). The wrapper limits each container to 1.5 CPU
and 1800 MiB. A remote runner without Docker can run host checks but cannot establish IDF evidence.

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
not a hidden source change. Native Codex adapters are checked/generated with
`node tools/agent-config/sync-codex.mjs --check` / `--write`; keep reviewed primary-checkout hook
registration/trust in sync in linked worktrees. Adapter parity does not prove runtime enforcement.

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
It preserves byte offsets, whitespace, inline comments, recognized legal/tooling directive families and HTML-sensitive
sequences. JavaScript block comments remain opaque. The supported JavaScript subset refuses code
lines containing template backticks, inline block openers or a final backslash, code after a block
closer, unfinished blocks and Unicode line separators. These restrictions exclude untracked
multiline strings/templates without guessing regex syntax. Inputs are bounded UTF-8 without NUL.
Markup and code tokens remain unchanged; source comments remain readable. This is conservative
repository-specific pruning, not general minification or universal observable equivalence:
source introspection such as `Function.prototype.toString()` can expose changed comment text.
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
Fast-Build volume setup. Volume setup uses the pinned IDF image and stops on failure. Its offline
self-test executes the shipped wrapper with a passive Docker CLI, including setup failures; it
neither accesses a Docker daemon nor establishes actual container resource enforcement.

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
The gate checks that every main-component unit compiles with `-Og` and emits `-fstack-usage`;
the other components are covered by the checked `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` Kconfig
value, Mbed TLS/TF-PSA-Crypto `-Os` is checked as the effective Kconfig value, and the bootloader,
which builds with size optimization, is compared only between independent builds. Main privately
enforces C++17 and return-value/format warnings.
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
the workflow's independent-rebuild image literal, both resource baselines' toolchain identities,
the SDK-default and build-semantic version literals, and the compiled X509 bundle hash.
Review the PSA patch and canonical postimages, then rerun the protocol vectors and complete
four-target contract. CI action inputs must still use the toolchain output, and both target
matrices must still contain exactly the four supported targets. This list is a migration checklist,
not authorization to change any pin.

## Configuration-only maintenance role

The default-off [configuration recovery](CONFIG_RECOVERY.md#default-off-build-and-artifact-boundary)
owner describes its separate project identity, unsigned four-target source/reproduction driver,
resource gates and unqualified physical procedure. Its non-OTA apps never enter the normal16parts;
ordinary source/compiler/partition/signature authority is not relaxed. Do not enable its config in
an application build or treat its presence as a flashable or physically tested recovery product.

## Delivery

Unsigned local images **cannot boot**. Use provenance-bound signed artifacts and the canonical
`$flash-esp32`, `$deploy-bench`, `$ship` or `$usb-recovery` procedure, as appropriate. Every physical write
requires explicit authorization for the board, target and scope. Verify every image's signature;
production and preview artifacts must match the production public-key pin. An authorized local
development image must match the supplied development key. Size/chip/version checks alone do not
authenticate an image.

App-only USB delivery writes the signed app at `0x20000`, then activates by erasing `otadata`
last. NVS stays intact. Merged-image writes and full erases destroy configuration and keys;
[retention and signing](SECURITY.md) explain recovery limits. Signing with a development key creates
a separate trust lineage and needs an explicit key and scope.

`$deploy` handles commit, push, PR creation, current-head reviews/CI and canonical squash merge,
then observes the main build. It does not contact devices.
[`$deploy-bench`](../.agents/skills/deploy-bench/SKILL.md) identifies the physical USB bench across
Tesla/Daikin firmware, flashes a signed PR preview or development-key image through `$flash-esp32`
or an already-published image through `$usb-recovery`'s provenance and app-only steps, and tests
the implementation on that same board. Device delivery after merge remains a separately requested
`$ship` operation.

Pages acceptance requires a bounded, validated expected `changelog.json` and exact served bytes
within the same complete attempt as the manifest and 16 firmware parts. The root checker takes
`--expected-changelog-file`; staged Dev checks derive it from the candidate directory, while live
Dev checks require that argument. Relevance baselines read both files from one exact branch commit.
Changes to shipped top-level `docs/*.md` select the normal four-target build and Dev publication;
they do not reuse older firmware under the newer source SHA. Stable publication remains manual.
Before either Pages branch write, the downloaded candidate passes four key-free RSA-PSS checks
against the production public pin. The offline deploy-consumer test executes this YAML step with
disposable signed fixtures and confirms that each target failure stops before publication.

### Served PR preview acceptance

After signed preview publication, `scripts/check-pr-preview-served.py` runs trusted default-branch
code without keys or repository tokens. It validates the local signed four-target site against the
source, canonical PR version and production public-key pin, then compares the served manifest,
all 16 parts and a final manifest from the API-confirmed `PR/<N>/` authority. Each retry starts a
complete fresh snapshot. Redirects, non-200 status, mixed or incomplete bytes and expired deadlines
fail acceptance. At most 20 attempts share a 180-second network deadline and 10-second request caps.

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

Quoted jq and search expressions remain literal data during command classification. Guarded
publication arguments retain their original quotes, so expanding body-file and target paths are
still rejected. Shell execution wrappers and substitutions use conservative scanning.
The only Python exception to the partition write guard is the standalone, digest-bound
`python3 scripts/check-partition-contract.py --csv partitions.csv` in the hook's own worktree,
optionally with one literal `--binary` path. Additional options, wrappers, changed validators and
ambiguous working directories remain blocked; the secret guard runs first.

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

## Reviews and documentation

Required independent checks and exact-head records are in [AGENTS.md](../AGENTS.md#pr-and-review).
Skills specify how to perform them; a green build does not substitute for review.
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
